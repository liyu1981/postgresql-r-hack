/*-------------------------------------------------------------------------
 *
 * utils.c
 *
 *	  Utility functions for replication
 *
 * Portions Copyright (c) 2010, Translattice, Inc
 * Portions Copyright (c) 2001-2010, PostgreSQL Global Development Group
 *
 * Based on the work of Win Bausch and Bettina Kemme (ETH Zurich)
 * and some earlier efforts trying to integrate Postgres-R into
 * Postgres 7.2
 *
 *-------------------------------------------------------------------------
 */

#include <unistd.h>

#include "postgres.h"

#include "miscadmin.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/hash.h"
#include "catalog/pg_replication.h"
#include "catalog/indexing.h"
#include "storage/imsg.h"
#include "storage/lwlock.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "postmaster/coordinator.h"
#include "replication/replication.h"
#include "replication/cset.h"
#include "replication/utils.h"
#include "utils/fmgroids.h"

/* #define PeerTxnEntries 1024 */
/* #define PeerTxnEntries 2 */
#define PeerTxnEntries (replication_peer_txn_entries)

CommitOrderId lowestKnownCommitId = FirstNormalCommitOrderId;

typedef struct PeerTxnEntry
{
	NodeId			origin_node_id;
	TransactionId	origin_xid;
	TransactionId	local_xid;
	CommitOrderId	local_coid;

	bool            valid;
	bool            commited;
	bool            aborted;

	TransactionId   dep_coid;
} PeerTxnEntry;

typedef struct PteOriginHashEntry
{
	TransactionId origin_node_id;
	TransactionId origin_xid;
	PeerTxnEntry *pte;
} PteOriginHashEntry;

typedef struct PteLocalXidHashEntry
{
	TransactionId  local_xid;
	PeerTxnEntry  *pte;
} PteLocalXidHashEntry;

typedef struct PteLocalCoidHashEntry
{
	TransactionId  local_coid;
	PeerTxnEntry  *pte;
} PteLocalCoidHashEntry;

#define PTEHASH_INSERT_ORIGIN 0x01
#define PTEHASH_INSERT_XID    0x02
#define PTEHASH_INSERT_COID   0x04

/* globals keeping pointers to the shmem areas */
ReplLutCtlData *ReplLutCtl;
HTAB           *PteOriginHash;
HTAB           *PteLocalXidHash;
HTAB           *PteLocalCoidHash;

void          InitPte(PeerTxnEntry *pte);
int           ReplLutCtlShmemSize();
void          InitSharedPeerTxnQueue();
void          InitPteHash();
PeerTxnEntry* alloc_new_entry();
void          cleanPeerTxnEntries();
void          insertPteHash(PeerTxnEntry *pte, int optflag);
void          deletePteHash(PeerTxnEntry *pte);
CommitOrderId getLowestKnownCommitOrderId();

char *
decode_database_state(rdb_state state)
{
	switch (state)
	{
		case RDBS_UNKNOWN: return "RDBS_UNKNOWN";
		case RDBS_JOIN_REQUESTED: return "RDBS_JOIN_REQUESTED";
		case RDBS_AWAITING_MAJORITY: return "RDBS_AWAITING_MAJORITY";
		case RDBS_RECOVERING_SCHEMA: return "RDBS_RECOVERING_SCHEMA";
		case RDBS_RECOVERING_DATA: return "RDBS_RECOVERING_DATA";
		case RDBS_OPERATING: return "RDBS_OPERATING";
		case RDBS_UNABLE_TO_CONNECT: return "RDBS_UNABLE_TO_CONNECT";

		default: return "UNKNOWN RDB STATE";
	}
}

char *
decode_command_type(CsetCmdType cmd)
{
	switch (cmd)
	{
		case CSCMD_EOS: return "EOS";

		case CSCMD_INSERT: return "INSERT";
		case CSCMD_UPDATE: return "UPDATE";
		case CSCMD_DELETE: return "DELETE";

		case CSCMD_TUPLE: return "TUPLE";

		case CSCMD_SUBTXN_START: return "SUBTXN_START";
		case CSCMD_SUBTXN_COMMIT: return "SUBTXN_COMMIT";
		case CSCMD_SUBTXN_ABORT: return "SUBTXN_ABORT";

		default: return "UNKNOWN COMMAND TYPE";
	}
}

void
InitPte(PeerTxnEntry *pte)
{
	pte->origin_node_id = InvalidNodeId;
	pte->origin_xid = 0;
	pte->local_coid = InvalidCommitOrderId;
	pte->local_xid = 0;

	pte->valid = false;
	pte->commited = false;
	pte->aborted = false;
	pte->dep_coid = InvalidCommitOrderId;
}

int
ReplLutCtlShmemSize()
{
	Size		size;

	/* size of the control structure itself */
	size = MAXALIGN(sizeof(ReplLutCtlData));

	/* size of peer transaction queue */
	size = add_size(size, mul_size(PeerTxnEntries,
	                               MAXALIGN(sizeof(PeerTxnEntry))));

	/* size of PteOriginHash */
	size = add_size(size, mul_size(PeerTxnEntries,
	                               MAXALIGN(sizeof(HASHELEMENT)+sizeof(PteOriginHashEntry))));

	/* size of PteLocalXidHash */
	size = add_size(size, mul_size(PeerTxnEntries,
	                               MAXALIGN(sizeof(HASHELEMENT)+sizeof(PteLocalXidHashEntry))));

	/* size of PteLocalCoidHash */
	size = add_size(size, mul_size(PeerTxnEntries,
	                               MAXALIGN(sizeof(HASHELEMENT)+sizeof(PteLocalCoidHashEntry))));

	return size;
}

void
InitSharedPeerTxnQueue()
{
	int				i;
	PeerTxnEntry   *pte;

	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile ReplLutCtlData *rctl = ReplLutCtl;

		SpinLockAcquire(&rctl->ptxn_lock);

		rctl->ptxn_head = 0;
		rctl->ptxn_tail = 0;
		rctl->ptxn_laststop = 0;

		pte = (PeerTxnEntry*) &rctl[1];

		for (i = 0; i < PeerTxnEntries; i++) {
			InitPte(&pte[i]);
		}

		SpinLockRelease(&rctl->ptxn_lock);
	}
	END_CRIT_SECTION();
}

void
InitPteHash()
{
	HASHCTL		info;

	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile ReplLutCtlData *rctl = ReplLutCtl;

		SpinLockAcquire(&rctl->origin_lock);
		info.keysize = sizeof(TransactionId) + sizeof(TransactionId);
		info.entrysize = sizeof(PteOriginHashEntry);
		info.hash = tag_hash;
		PteOriginHash = ShmemInitHash("PteOriginHash",
		                              PeerTxnEntries, PeerTxnEntries, &info,
		                              HASH_ELEM | HASH_FUNCTION);
		if (!PteOriginHash)
			elog(FATAL, "Could not initialize PteOriginHash hash table");
		SpinLockRelease(&rctl->origin_lock);

		SpinLockAcquire(&rctl->localxid_lock);
		info.keysize = sizeof(TransactionId);
		info.entrysize = sizeof(PteLocalXidHashEntry);
		info.hash = oid_hash;
		PteLocalXidHash = ShmemInitHash("PteLocalxidHash",
		                                PeerTxnEntries, PeerTxnEntries, &info,
		                                HASH_ELEM | HASH_FUNCTION);
		if (!PteLocalXidHash)
			elog(FATAL, "Could not initialize PteLocalXidHash hash table");
		SpinLockRelease(&rctl->localxid_lock);

		SpinLockAcquire(&rctl->localcoid_lock);
		info.keysize = sizeof(TransactionId);
		info.entrysize = sizeof(PteLocalCoidHashEntry);
		info.hash = oid_hash;
		PteLocalCoidHash = ShmemInitHash("PteLocalcoidHash",
		                                 PeerTxnEntries, PeerTxnEntries, &info,
		                                 HASH_ELEM | HASH_FUNCTION);
		if (!PteLocalCoidHash)
			elog(FATAL, "Could not initialize PteLocalCoidHash hash table");
		SpinLockRelease(&rctl->localcoid_lock);
	}
	END_CRIT_SECTION();
}

void
ReplLutShmemInit(void)
{
	bool	 found;

#ifdef IMSG_DEBUG
	elog(DEBUG3, "ReplLutShmemInit(): initializing shared memory");
	elog(DEBUG5, "                    of size %d", ReplLutCtlShmemSize());
#endif

	elog(LOG, "PeerTxnEntries = %d", PeerTxnEntries);

	ReplLutCtl = (ReplLutCtlData *)ShmemInitStruct("Replication Lookup Ctl",
	                                               ReplLutCtlShmemSize(), &found);

	if (found)
		return;

	memset(ReplLutCtl, 0, sizeof(ReplLutCtlData));

	/* initialize all the spin locks */
	SpinLockInit(&ReplLutCtl->ptxn_lock);
	SpinLockInit(&ReplLutCtl->origin_lock);
	SpinLockInit(&ReplLutCtl->localxid_lock);
	SpinLockInit(&ReplLutCtl->localcoid_lock);

	/* initialize all the hash tables */
	InitSharedPeerTxnQueue();
	InitPteHash();
}

static PeerTxnEntry *
find_entry_for(NodeId origin_node_id, TransactionId origin_xid)
{
	PeerTxnEntry       *pte = NULL;
	PteOriginHashEntry  search_key;
	bool                found;
	void               *ret;

	search_key.origin_node_id = origin_node_id;
	search_key.origin_xid = origin_xid;
	found = false;
	ret = hash_search(PteOriginHash, &search_key, HASH_FIND, &found);
	if (ret != NULL) {
		pte = ((PteOriginHashEntry*)ret)->pte;
	}

	return pte;
}

PeerTxnEntry *
alloc_new_entry()
{
	PeerTxnEntry            *pte;
	volatile ReplLutCtlData *rctl = ReplLutCtl;
	int                      i;
	PeerTxnEntry            *ret = NULL;
	bool                     found;

	if (rctl->ptxn_head < PeerTxnEntries) {
		/* still have not used slot (only happens shortly after init) */
		pte = (PeerTxnEntry*) &rctl[1];
		ret = &pte[rctl->ptxn_head++];
		elog(DEBUG1, "alloc_new_entry, now size=%d", rctl->ptxn_head - rctl->ptxn_tail);
	}
	else {
		pte = (PeerTxnEntry*) &rctl[1];
		i = rctl->ptxn_tail;
		found = false;
		while (i<rctl->ptxn_head) {
			if (!pte[i].valid) {
				ret = &pte[i];
				if (ret->local_coid == lowestKnownCommitId) {
					lowestKnownCommitId = FirstNormalCommitOrderId;
				}
				InitPte(ret);
				elog(DEBUG1, "alloc_new_entry, reuse no. %d", i);
				found = true;
				break;
			}
			i++;
		}

		/* try to clear some slots and do it again */
		if (!found) {
			cleanPeerTxnEntries();
			pte = (PeerTxnEntry*) &rctl[1];
			i = rctl->ptxn_tail;
			while (i<rctl->ptxn_head) {
				if (!pte[i].valid) {
					ret = &pte[i];
					if (ret->local_coid == lowestKnownCommitId) {
						lowestKnownCommitId = FirstNormalCommitOrderId;
					}
					InitPte(ret);
					elog(DEBUG1, "alloc_new_entry, after clean reuse no. %d", i);
					break;
				}
				i++;
			}
		}
	}

	if (ret != NULL)
		ret->valid = true;

	return ret;
}

void
cleanPeerTxnEntries()
{
	PeerTxnEntry            *pte;
	volatile ReplLutCtlData *rctl = ReplLutCtl;
	int                      i, j;
	bool                     found;
	int                      count = 0;

	pte = (PeerTxnEntry*) &rctl[1];
	for (i = rctl->ptxn_tail; i<rctl->ptxn_head; ++i) {
		if (!(pte[i].commited || pte[i].aborted))
			continue;

#ifdef DEBUG
		elog(LOG, "start to valid pte %d [%d, %d, %d, %d] depends one %d < %d", i,
		     pte[i].origin_node_id, pte[i].origin_xid,
		     pte[i].local_xid, pte[i].local_coid);
#endif

		found = false;
		for (j=rctl->ptxn_tail; j<rctl->ptxn_head; ++j) {
			if (pte[j].dep_coid <= pte[i].local_coid) {
				/*elog(LOG, "found small pte %d [%d, %d, %d, %d] depends one %d < %d", i,
				     pte[j].origin_node_id, pte[j].origin_xid,
				     pte[j].local_xid, pte[j].local_coid, pte[j].dep_coid, pte[i].local_coid);*/
				found = true;
				if (pte[j].commited || pte[j].aborted)
					found =false;
				if (found)
					break;
			}				
		}
		if (pte[i].local_coid == getLowestKnownCommitOrderId())
		{
			// we can not clean this one, since some concurrent txn may start to rely on it
			found = true;
		}
		if (found)
			continue;
		else {
#ifdef DEBUG
			elog(LOG, "pte reclaimed %d [%d, %d, %d, %d]", i,
			     pte[i].origin_node_id, pte[i].origin_xid,
			     pte[i].local_xid, pte[i].local_coid);
#endif
			count += 1;
			pte[i].valid = false;
			/* delete the entry in co_txn_info also */
			erase_co_txn_info(pte[i].origin_node_id, pte[i].origin_xid);
		}
	}
	elog(LOG, "CleanPeerTxnEntries :   Reclaimed %d ptes this time.", count);
}

void
insertPteHash(PeerTxnEntry *pte, int optflag)
{
	PteOriginHashEntry    *rorigin    = NULL;
	PteLocalXidHashEntry  *rlocalxid  = NULL;
	PteLocalCoidHashEntry *rlocalcoid = NULL;
	PteOriginHashEntry     skorigin;
	PteLocalXidHashEntry   sklocalxid;
	PteLocalCoidHashEntry  sklocalcoid;
	bool                   found;

	if (optflag & PTEHASH_INSERT_ORIGIN) {
		skorigin.origin_node_id = pte->origin_node_id;
		skorigin.origin_xid = pte->origin_xid;
		hash_search(PteOriginHash, &skorigin, HASH_REMOVE, &found);
		rorigin = (PteOriginHashEntry*)hash_search(PteOriginHash, &skorigin,
		                                           HASH_ENTER, &found);
		rorigin->pte = pte;
	}

	if (optflag & PTEHASH_INSERT_XID) {
		sklocalxid.local_xid = pte->local_xid;
		hash_search(PteLocalXidHash, &sklocalxid, HASH_REMOVE, &found);
		rlocalxid = (PteLocalXidHashEntry*)hash_search(PteLocalXidHash, &sklocalxid,
		                                               HASH_ENTER, &found);
		rlocalxid->pte = pte;
	}

	if (optflag & PTEHASH_INSERT_COID) {
		sklocalcoid.local_coid = pte->local_coid;
		hash_search(PteLocalCoidHash, &sklocalcoid, HASH_REMOVE, &found);
		rlocalcoid = (PteLocalCoidHashEntry*)hash_search(PteLocalCoidHash, &sklocalcoid,
		                                                 HASH_ENTER, &found);
		rlocalcoid->pte = pte;
	}
}

void
deletePteHash(PeerTxnEntry *pte)
{
	PteOriginHashEntry    skorigin;
	PteLocalXidHashEntry  sklocalxid;
	PteLocalCoidHashEntry sklocalcoid;
	bool                  found;

	skorigin.origin_node_id = pte->origin_node_id;
	skorigin.origin_xid = pte->origin_xid;
	hash_search(PteOriginHash, &skorigin, HASH_REMOVE, &found);

	sklocalxid.local_xid = pte->local_xid;
	hash_search(PteLocalXidHash, &sklocalxid, HASH_REMOVE, &found);

	sklocalcoid.local_coid = pte->local_coid;
	hash_search(PteLocalCoidHash, &sklocalcoid, HASH_REMOVE, &found);
}

void
store_transaction_coid(NodeId origin_node_id, TransactionId origin_xid,
					   CommitOrderId local_coid)
{
	PeerTxnEntry   *pte;
	bool            newflag = false;

	Assert(CommitOrderIdIsValid(local_coid));

#ifdef COORDINATOR_DEBUG
	elog(DEBUG5, "Coordinator: storing origin node %d xid %d -> local coid: %d",
		 origin_node_id, origin_xid, local_coid);
#endif

	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile ReplLutCtlData *rctl = ReplLutCtl;

		while(true) {
			SpinLockAcquire(&rctl->ptxn_lock);

			pte = find_entry_for(origin_node_id, origin_xid);

			if (pte && pte->local_coid != InvalidCommitOrderId) {
				elog(LOG, "Pte (%d/%d) with txn %d already have a coid %d associated.",
				     pte->origin_node_id, pte->origin_xid, pte->local_xid, pte->local_coid);
				elog(PANIC, "Must die.");
			}

			if (!pte)
			{
				pte = alloc_new_entry();
				if (pte == NULL) {
					SpinLockRelease(&rctl->ptxn_lock);
					pg_usleep(1000);
					elog(LOG, "Out of space for pte coid, next try.");
					continue;
				}
				pte->origin_node_id = origin_node_id;
				pte->origin_xid = origin_xid;
				pte->local_xid = InvalidTransactionId;
				pte->dep_coid = getLowestKnownCommitOrderId();
				newflag = true;
			}

			pte->local_coid = local_coid;
			if (newflag) {
				insertPteHash(pte, PTEHASH_INSERT_ORIGIN | PTEHASH_INSERT_COID);
			}
			else {
				insertPteHash(pte, PTEHASH_INSERT_COID);
			}
			
			SpinLockRelease(&rctl->ptxn_lock);
			break;
		}
	}
	END_CRIT_SECTION();
}

void
store_transaction_local_xid(NodeId origin_node_id, TransactionId origin_xid,
							TransactionId local_xid)
{
	PeerTxnEntry   *pte;
	bool            newflag = false;

	Assert(TransactionIdIsValid(local_xid));

#ifdef COORDINATOR_DEBUG
	elog(DEBUG5, "bg worker [%d/%d]: storing origin node %d xid %d -> local xid: %d",
		 MyProcPid, MyBackendId, origin_node_id, origin_xid, local_xid);
#endif

	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile ReplLutCtlData *rctl = ReplLutCtl;

		while(1) {
			SpinLockAcquire(&rctl->ptxn_lock);

			pte = find_entry_for(origin_node_id, origin_xid);

			if (pte && pte->local_xid > 0) {
				elog(LOG, "Pte (%d/%d) with coid %d already have a txn %d associated.",
				     pte->origin_node_id, pte->origin_xid, pte->local_coid, pte->local_xid);
				elog(PANIC, "Must die.");
			}
			
			if (!pte)
			{
				pte = alloc_new_entry();
				if (pte == NULL) {
					SpinLockRelease(&rctl->ptxn_lock);
					pg_usleep(1000);
					elog(LOG, "Out of space for pte xid, next try.");
					continue;
				}
				pte->origin_node_id = origin_node_id;
				pte->origin_xid = origin_xid;
				pte->local_coid = InvalidCommitOrderId;
				pte->dep_coid = getLowestKnownCommitOrderId();
				newflag = true;
			}

			pte->local_xid = local_xid;
			if (newflag) {
				insertPteHash(pte, PTEHASH_INSERT_ORIGIN | PTEHASH_INSERT_XID);
			}
			else {
				insertPteHash(pte, PTEHASH_INSERT_XID);
			}
			
			SpinLockRelease(&rctl->ptxn_lock);
			break;
		}
	}
	END_CRIT_SECTION();
}

void
erase_transaction(NodeId origin_node_id, TransactionId origin_xid, bool is_commit)
{
	PeerTxnEntry   *pte;

#ifdef COORDINATOR_DEBUG
	elog(DEBUG5, "Coordinator: erasing origin node %d xid %d",
		 origin_node_id, origin_xid);
#endif

	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile ReplLutCtlData *rctl = ReplLutCtl;

		SpinLockAcquire(&rctl->ptxn_lock);

		pte = find_entry_for(origin_node_id, origin_xid);
		if (pte)
		{
			pte->commited = is_commit;
			pte->aborted = !is_commit;
			if (pte->commited && lowestKnownCommitId < pte->local_coid) {
				lowestKnownCommitId = pte->local_coid;
			}
		}

		SpinLockRelease(&rctl->ptxn_lock);
#ifdef DEBUG
		elog(LOG, "erase_transaction, is_commit=%d (%d)", is_commit, lowestKnownCommitId);
#endif
	}
	END_CRIT_SECTION();
}

void
get_origin_by_local_xid(TransactionId local_xid,
						NodeId *origin_node_id, TransactionId *origin_xid)
{
	int                   i;
	PeerTxnEntry         *pte;
	bool                  found   = false;
	PteLocalXidHashEntry *ret;

	Assert(TransactionIdIsValid(local_xid));

	/* handle special, but valid transaction ids */
	if (local_xid < FirstNormalTransactionId)
	{
		*origin_node_id = InvalidNodeId;
		*origin_xid = FrozenTransactionId;
		return;
	}

	/*
	 * If we don't have the origin information in our list anymore,
	 * we can safely assume it's Known Good (tm), i.e. older than the
	 * minimum commit order id.
	 */
	*origin_node_id = InvalidNodeId;
	*origin_xid = FrozenTransactionId;

	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile ReplLutCtlData *rctl = ReplLutCtl;

		SpinLockAcquire(&rctl->ptxn_lock);

		pte = (PeerTxnEntry*) &rctl[1];

		/* we don't support wrapping, yet */
		Assert(rctl->ptxn_head >= rctl->ptxn_tail);
		ret = (PteLocalXidHashEntry*)hash_search(PteLocalXidHash, &local_xid,
		                                         HASH_FIND, &found);
		if (ret != NULL && ret->pte->valid) {
			pte = ret->pte;
			*origin_node_id = pte->origin_node_id;
			*origin_xid = pte->origin_xid;
		}

		SpinLockRelease(&rctl->ptxn_lock);
	}
	END_CRIT_SECTION();
}


void
get_local_xid_by_coid(CommitOrderId coid, TransactionId *local_xid)
{
	int                    i;
	PeerTxnEntry          *pte;
	bool                   found = false;
	PteLocalCoidHashEntry *ret;

	*local_xid = InvalidTransactionId;

	if (coid < getLowestKnownCommitOrderId())
	{
		*local_xid = -1; /* means no need to wait */
		return;
	}

	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile ReplLutCtlData *rctl = ReplLutCtl;

		SpinLockAcquire(&rctl->ptxn_lock);

		pte = (PeerTxnEntry*) &rctl[1];

		/* we don't support wrapping, yet */
		Assert(rctl->ptxn_head >= rctl->ptxn_tail);
		ret = (PteLocalCoidHashEntry*)hash_search(PteLocalCoidHash, &coid,
		                                          HASH_FIND, &found);
		if (ret != NULL & ret->pte->valid) {
			pte = ret->pte;
			*local_xid = pte->local_xid;
		}

		SpinLockRelease(&rctl->ptxn_lock);
	}
	END_CRIT_SECTION();
}

CommitOrderId
get_local_coid_by_origin(NodeId origin_node_id, TransactionId origin_xid)
{
	int                 i;
	PeerTxnEntry       *pte;
	CommitOrderId       coid  = InvalidCommitOrderId;
	bool                found = false;
	PteOriginHashEntry *ret;
	PteOriginHashEntry  skey;

	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile ReplLutCtlData *rctl = ReplLutCtl;

		SpinLockAcquire(&rctl->ptxn_lock);

		pte = (PeerTxnEntry*) &rctl[1];

		/* we don't support wrapping, yet */
		Assert(rctl->ptxn_head >= rctl->ptxn_tail);
		skey.origin_node_id = origin_node_id;
		skey.origin_xid = origin_xid;
		ret = (PteOriginHashEntry*)hash_search(PteOriginHash, &skey,
		                                       HASH_FIND, &found);
		if (ret != NULL && ret->pte->valid) {
			pte = ret->pte;
			coid = pte->local_coid;
		}

		SpinLockRelease(&rctl->ptxn_lock);
	}
	END_CRIT_SECTION();

	return coid;
}

CommitOrderId
get_local_coid_by_local_xid(TransactionId local_xid)
{
	int                   i;
	PeerTxnEntry         *pte;
	CommitOrderId         coid  = InvalidCommitOrderId;
	bool                  found = false;
	PteLocalXidHashEntry *ret;

	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile ReplLutCtlData *rctl = ReplLutCtl;

		SpinLockAcquire(&rctl->ptxn_lock);

		pte = (PeerTxnEntry*) &rctl[1];

		/* we don't support wrapping, yet */
		Assert(rctl->ptxn_head >= rctl->ptxn_tail);
		ret = (PteLocalXidHashEntry*)hash_search(PteLocalXidHash, &local_xid,
		                                         HASH_FIND, &found);
		if (ret != NULL && ret->pte->valid) {
			pte = ret->pte;
			coid = pte->local_coid;
		}

		SpinLockRelease(&rctl->ptxn_lock);
	}
	END_CRIT_SECTION();

	return coid;
}

void
get_multi_coids(CommitOrderId *eff_coid, CommitOrderId *req_coid,
				TransactionId local_xid,
				NodeId origin_node_id, TransactionId origin_xid)
{
	bool		  found_eff_coid = false,
		          found_req_coid = false;
	CommitOrderId r1, r2;

	*eff_coid = KnownGoodCommitOrderId;
	*req_coid = KnownGoodCommitOrderId;

	r1 = get_local_coid_by_local_xid(local_xid);
	if (r1 == InvalidCommitOrderId) {
		found_eff_coid = false;
	}
	else {
		found_eff_coid = true;
		*eff_coid = r1;
	}

	r2 = get_local_coid_by_origin(origin_node_id, origin_xid);
	if (r2 == InvalidCommitOrderId) {
		found_req_coid = false;
	}
	else {
		found_req_coid = true;
		*req_coid = r2;
	}

	if (found_eff_coid && !found_req_coid) {
		*req_coid = *eff_coid;
	}
}

CommitOrderId
getLowestKnownCommitOrderId()
{
	return lowestKnownCommitId;
}

/*
 * Make sure we respect the commit order id given by the
 * coordinator and the underlying GCS.
 *
 * We only care about OPERATING mode, because during recovery
 * conflicts are non-existent. (FIXME: what about replaying
 * transactions during recovery?)
 */
void
WaitUntilCommittable(void)
{
	TransactionId  xid;
	CommitOrderId  my_coid = InvalidCommitOrderId,
		           dep_coid;
	PeerTxnEntry  *pte;
	PteLocalXidHashEntry *ret;
	bool found;

	/*
	 * Short circuit for aborted transactions.
	 */
	if (MyProc->abortFlag)
		goto abort_waiting;

	my_coid = get_local_coid_by_local_xid(MyProc->xid);
	elog(DEBUG1, "bg worker [%d/%d]: WaitUntilCommittable(): we have commit order id %d",
		 MyProcPid, MyBackendId, my_coid);

	/*
	 * Lookup the lowest known commit order id
	 */
	dep_coid = getLowestKnownCommitOrderId();
	ret = (PteLocalXidHashEntry*)hash_search(PteLocalXidHash, &(MyProc->xid), HASH_FIND, &found);
	if (ret != NULL && ret->pte->valid) {
		pte = ret->pte;
		if (dep_coid < pte->dep_coid)
			dep_coid = pte->dep_coid;
	}

	/*
	 * Loop over all transactions with commit order ids in between the
	 * lowest known and our own one.
	 */
	while (dep_coid < my_coid)
	{
		Assert(CommitOrderIdIsNormal(dep_coid));

		/*
		 * Wait until the transaction id is locally known.
		 */
		get_local_xid_by_coid(dep_coid, &xid);
#ifdef DEBUG
		elog(LOG, "start to check valid for %d/%d", dep_coid, xid);
#endif
		while (xid >0 && !TransactionIdIsValid(xid))
		{
			if (MyProc->abortFlag)
				goto abort_waiting;

			elog(DEBUG3, "bg worker [%d/%d]: WaitUntilCommittable(): coid %d: waiting for coid %d",
				 MyProcPid, MyBackendId, my_coid, dep_coid);
			pg_usleep(30000L);

			get_local_xid_by_coid(dep_coid, &xid);
		}

		if (xid <= 0) {
			goto next_higher_dep;
		}

#ifdef DEBUG
		if (xid == MyProc->xid) {
			PteLocalCoidHashEntry *tmpentry = NULL;
			PeerTxnEntry *pte1, *pte2;
			bool found;
			tmpentry = (PteLocalCoidHashEntry*) hash_search(PteLocalCoidHash, &my_coid, HASH_FIND, &found);
			if (tmpentry != NULL) {
				pte1 = tmpentry->pte;
			}
			tmpentry = NULL;
			tmpentry = (PteLocalCoidHashEntry*) hash_search(PteLocalCoidHash, &dep_coid, HASH_FIND, &found);
			if (tmpentry != NULL) {
				pte2 = tmpentry->pte;
			}
			elog(PANIC,
			     "bg worker [%d/%d]: WaitUntilCommittable(): txn %d/%d: waiting for itself.\n detail: (%d/%d, %d/%d) waiting for (%d/%d, %d/%d), my_coid=%d, dep_coid=%d",
			     MyProcPid, MyBackendId, my_coid, MyProc->xid,
			     pte1->origin_node_id, pte1->origin_xid, pte1->local_xid, pte1->local_coid,
			     pte2->origin_node_id, pte2->origin_xid, pte2->local_xid, pte2->local_coid,
			     my_coid, dep_coid
				);
		}
#endif

		/*
		 * Wait until the transaction has committed or aborted.
		 */
		while (TransactionIdIsInProgress(xid))
		{
			if (MyProc->abortFlag)
				goto abort_waiting;

			elog(DEBUG3, "bg worker [%d/%d]: WaitUntilCommittable(): txn %d/%d: waiting for txn %d/%d.",
			     MyProcPid, MyBackendId, my_coid, MyProc->xid, dep_coid, xid);
			pg_usleep(30000L);
		}

		/* increment to process the next higher coid */
	next_higher_dep:
		dep_coid++;
	}

abort_waiting:
	elog(DEBUG1, "bg worker [%d/%d]: WaitUntilCommittable(): coid %d result: %s",
		 MyProcPid, MyBackendId, my_coid,
		 (MyProc->abortFlag ? "MUST ABORT!" : "commit now!"));
}

/*
 * get_db_replication_state
 *
 * Short-cut to just retrieve the database replication state
 */
rdb_state
get_db_replication_state(Oid dboid)
{
	co_database *codb;
    rdb_state result;

	LWLockAcquire(CoordinatorDatabasesLock, LW_SHARED);
    codb = get_co_database(dboid);
    result = codb->state;
	LWLockRelease(CoordinatorDatabasesLock);

	return result;
}

/*
 *    resolve_conflict
 *
 * Called upon having detected a conflict with another transaction, which is
 * known by its xid. Looks up and compares the CommitOrderIds of the current
 * and the conflicting transaction and resolves the conflict by aborting one
 * of the two.
 *
 * others_xid:	the transaction id that the current transaction is in
 *				conflict with, for whatever reason.
 * my_coid:		a pointer to a CommitOrderId variable storing the COID of
 *				the current transaction, if any. Does the lookup only if
 *				*my_coid is invalid, reuses the previously fetched and store
 *				COID otherwise.
 */
void
resolve_conflict(TransactionId others_xid, CommitOrderId *my_coid)
{
	NodeId              origin_node_id;
	TransactionId		origin_xid;
	CommitOrderId       others_coid = InvalidCommitOrderId;
	pid_t				others_pid;
	bool				other_must_abort;

retry:
	/*
	 * Re-check if we got a valid commit order id before every retry of
	 * solving a conflict. Once we got a coid, we don't ever need to check
	 * again, so we store it back to the variable provided by the caller
	 * for reuse.
	 */
	if (!CommitOrderIdIsValid(*my_coid))
		*my_coid = get_local_coid_by_local_xid(MyProc->xid);

	/*
	 * Likewise, check if we've already received the ordering decision for
	 * the other transaction.
	 */
	get_origin_by_local_xid(others_xid, &origin_node_id,
							&origin_xid);

	if (origin_node_id != InvalidNodeId &&
		TransactionIdIsValid(origin_xid))
		others_coid = get_local_coid_by_local_xid(others_xid);
	else
		others_coid = InvalidCommitOrderId;

	if (CommitOrderIdIsValid(others_coid) && CommitOrderIdIsValid(*my_coid))
	{
#ifdef DEBUG_CSET_APPL
		elog(DEBUG5, "bg worker [%d/%d]: resolve_conflict:  my coid: %d, others coid %d",
			 MyProcPid, MyBackendId, *my_coid, others_coid);
#endif
		Assert(CommitOrderIdsInSameRange(others_coid, *my_coid));
		if (CmpCommitOrderIds(others_coid, *my_coid) < 0)
		{
			/*
			 * We must abort, since the other transaction comes before
			 * according to the commit order decided by the GCS.
			 */
#ifdef DEBUG_CSET_APPL
			elog(DEBUG5, "bg worker [%d/%d]: resolve_conflict: must abort.",
				 MyProcPid, MyBackendId);
#endif

			other_must_abort = false;
		}
		else
		{
			/*
			 * Abort the other transaction, as we take precedence according
			 * to commit order decided by the GCS.
			 */
#ifdef DEBUG_CSET_APPL
			elog(DEBUG5, "bg worker [%d/%d]: resolve_conflict: aborting txn %d with higher coid %d.",
				 MyProcPid, MyBackendId, others_xid, others_coid);
#endif

			other_must_abort = true;
		}
	}
	else if (origin_node_id != InvalidNodeId &&
			 TransactionIdIsValid(origin_xid) &&
			 CommitOrderIdIsValid(*my_coid))
	{
		/*
		 * Abort the other transaction, as it isn't even ordered by the GCS.
		 * As we expect the GCS to send ordering messages in the decided
		 * order,
		 */
#ifdef DEBUG_CSET_APPL
		elog(DEBUG5, "bg worker [%d/%d]: resolve_conflict: aborting transaction %d, no coid.",
			 MyProcPid, MyBackendId, others_xid);
#endif

		other_must_abort = true;
	}
	else if (CommitOrderIdIsValid(others_coid) && !CommitOrderIdIsValid(my_coid))
	{
		/*
		 * Similarly, abort the current transaction, if it didn't get its
		 * ordering decision, but the other has.
		 */
#ifdef DEBUG_CSET_APPL
		elog(DEBUG5, "bg worker [%d/%d]: resolve_conflict: must abort, no coid.",
			 MyProcPid, MyBackendId);
#endif

		other_must_abort = false;
	}
	else
	{
		/*
		 * None of the two conflicting transactions got an ordering decision,
		 * so we must wait and retry.
		 */
#ifdef DEBUG_CSET_APPL
		elog(DEBUG5, "bg worker [%d/%d]: resolve_conflict: txn %d is waiting for ordering decision due to conflict",
			 MyProc->pid, MyBackendId, MyProc->xid);
#endif

		/*
		 * FIXME: should only wait until this backend gets a IMSGT_ORDERING
		 *        decision or the other transaction transaction terminated.
		 */
		pg_usleep(50000);

		if (MyProc->abortFlag)
			other_must_abort = false;
		else
			goto retry;
	}

	if (other_must_abort)
	{
		others_pid = CancelConcurrentTransaction(others_xid);

		if (others_pid != 0)
		{
#ifdef DEBUG_CSET_APPL
			elog(DEBUG5, "bg worker [%d/%d]: waiting for txn %d, pid %d",
				 MyProcPid, MyBackendId, others_xid, others_pid);
#endif

			/*
			 * A process for the conflicting transaction with id others_xid
			 * has been found and sent a signal. We wait until it aborts,
			 * before we allow retrying the operations that raised the
			 * conflict.
			 */
			XactLockTableWait(others_xid);

#ifdef DEBUG_CSET_APPL
			elog(DEBUG5, "bg worker [%d/%d]: txn %d, pid %d aborted",
				 MyProcPid, MyBackendId, others_xid, others_pid);
#endif
		}

		/*
		 * Whether we found the pid and killed it or not, the given
		 * transaction shouldn't be in progress anymore.
		 */
		Assert(!TransactionIdIsInProgress(others_xid));
	}
	else
		MyProc->abortFlag = true;
}

