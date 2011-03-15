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

#define PeerTxnEntries 1024

typedef struct PeerTxnEntry
{
	NodeId			origin_node_id;
	TransactionId	origin_xid;
	TransactionId	local_xid;
	CommitOrderId	local_coid;
	bool            valid;
	bool            aborted;
	bool            tic;
} PeerTxnEntry;

typedef struct PteOriginHashEntry
{
	int8 origin_node_id_xid; /* first 4bytes=node_id, second 4bytes=xid */
	PeerTxnEntry  *pte;
} PteOriginHashEntry;

#define FORM_ORIGIN_NODE_ID_XID(nid, xid) ((int8)((nid))<<sizeof(int4)) | (int8)((xid))

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

/* globals keeping pointers to the shmem areas */
ReplLutCtlData *ReplLutCtl;
HTAB           *PteOriginHash;
HTAB           *PteLocalXidHash;
HTAB           *PteLocalCoidHash;
RegProcedure    pg_replication_eq_func = InvalidOid;

void          InitPte(PeerTxnEntry *pte);
int           ReplLutCtlShmemSize();
void          InitSharedPeerTxnQueue();
void          InitPteHash();
PeerTxnEntry* alloc_new_entry();
void          storePteToPgReplication(PeerTxnEntry *pte);
void          insertPteHash(PeerTxnEntry *pte);
void          deletePteHash(PeerTxnEntry *pte);
CommitOrderId getLowestKnownCommitOrderId();
RegProcedure  getPgReplicationEqFunc();

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
initPte(PeerTxnEntry *pte)
{
	pte->origin_node_id = InvalidNodeId;
	pte->valid = false;
	pte->tic = false;
	pte->aborted = false;
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
			initPte(&pte[i]);
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
		info.keysize = sizeof(int8);
		info.entrysize = sizeof(PteOriginHashEntry);
		info.hash = hashint8;
		PteOriginHash = ShmemInitHash("PteOriginHash",
		                              PeerTxnEntries, PeerTxnEntries, &info,
		                              HASH_ELEM | HASH_FUNCTION);
		if (!PteOriginHash)
			elog(FATAL, "Could not initialize PteOriginHash hash table");
		SpinLockRelease(&rctl->origin_lock);

		SpinLockAcquire(&rctl->localxid_lock);
		info.keysize = sizeof(TransactionId);
		info.entrysize = sizeof(PteLocalXidHashEntry);
		info.hash = hashint4;
		PteLocalXidHash = ShmemInitHash("PteLocalxidHash",
		                                PeerTxnEntries, PeerTxnEntries, &info,
		                                HASH_ELEM | HASH_FUNCTION);
		if (!PteLocalXidHash)
			elog(FATAL, "Could not initialize PteLocalXidHash hash table");
		SpinLockRelease(&rctl->localxid_lock);

		SpinLockAcquire(&rctl->localcoid_lock);
		info.keysize = sizeof(TransactionId);
		info.entrysize = sizeof(PteLocalCoidHashEntry);
		info.hash = hashint4;
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

RegProcedure
getPgReplicationEqFunc()
{
	Relation idx_rel;
	Oid      eq_func_oid;

	if (pg_replication_eq_func == InvalidOid) {
		/* init pg_replication_eq_func.  liyu: this is possible because
		   all 4 attrs in pg_replication are of type int32
		*/
		idx_rel = index_open(ReplicationOriginIndexId, AccessShareLock);
		get_sort_group_operators(idx_rel->rd_att->attrs[0]->atttypid,
		                         false, true, false,
		                         NULL, &eq_func_oid, NULL);
		pg_replication_eq_func = get_opcode(eq_func_oid);
		index_close(idx_rel, NoLock);
	}
	return pg_replication_eq_func;
}

static PeerTxnEntry *
find_entry_for(NodeId origin_node_id, TransactionId origin_xid)
{
	volatile ReplLutCtlData *rctl    = ReplLutCtl;
	PeerTxnEntry			*pte     = (PeerTxnEntry*) &rctl[1];
	int						 i;
	Relation                 rep_rel = NULL;
	Relation                 idx_rel = NULL;
	Oid                      eq_func;
	IndexScanDesc            scan;
	ScanKey                  skeys;
	HeapTuple                tuple;
	int8                     search_key;
	bool                     found;
	void                    *ret;
	bool                     isnull;

	/*
	  liyu: now this is a hash search.
	 */

	search_key = FORM_ORIGIN_NODE_ID_XID(origin_node_id, origin_xid);
	found = false;
	ret = hash_search(PteOriginHash, &search_key, HASH_FIND, &found);
	if (ret != NULL) {
		pte = ((PteOriginHashEntry*)ret)->pte;
		return pte;
	}

	/*
	  liyu: Now we have a system table pg_replication store the last
	  committed origin-local combination. So if we do not find
	  anything in the shmem list, we have to search the system table.
	 */

	pte = NULL;
	skeys = (ScanKey)palloc0(2 * sizeof(ScanKeyData));
	ScanKeyInit(&skeys[0], 1, BTEqualStrategyNumber, get_opcode(eq_func), origin_node_id);
	ScanKeyInit(&skeys[1], 2, BTEqualStrategyNumber, get_opcode(eq_func), origin_xid);
	rep_rel = heap_open(ReplicationRelationId, AccessShareLock);
	idx_rel = index_open(ReplicationOriginIndexId, AccessShareLock);
	scan = index_beginscan(rep_rel, idx_rel, SnapshotNow, 2, skeys);
	tuple = index_getnext(scan, ForwardScanDirection);
	if (tuple) {
		pte = alloc_new_entry();
		pte->origin_node_id = origin_node_id;
		pte->origin_xid = origin_xid;
		pte->valid = true;
		pte->local_xid = fastgetattr(tuple, Anum_pg_replication_replocalxid,
		                             rep_rel->rd_att, &isnull);
		pte->local_coid = fastgetattr(tuple, Anum_pg_replication_replocalcoid,
		                              rep_rel->rd_att, &isnull);
	}
	index_endscan(scan);
	pfree(skeys);
	index_close(idx_rel, NoLock);
	heap_close(rep_rel, NoLock);

	return pte;
}

PeerTxnEntry *
alloc_new_entry()
{
	PeerTxnEntry            *pte;
	volatile ReplLutCtlData *rctl = ReplLutCtl;
	int                      i, old_laststop;
	bool                     pass;

	if (rctl->ptxn_head < PeerTxnEntries) {
		/* still have not used slot (only happens shortly after init) */
		pte = (PeerTxnEntry*) &rctl[1];
		pte = &pte[rctl->ptxn_head++];
		return pte;
	}
	else {
		/* usual case, here we apply a clock algorithm to swap in-valid slot */
		old_laststop = rctl->ptxn_laststop;
		pass = false;
		while(true) {
			pte = (PeerTxnEntry*)&rctl[rctl->ptxn_laststop];
			if (!pte->valid) {
				if (!pte->tic) {
					pte->tic = true;
					rctl->ptxn_laststop += 1;
					if (rctl->ptxn_laststop >= rctl->ptxn_head)
						rctl->ptxn_laststop = 0;
				}
				else {
					if (!pte->aborted)
						storePteToPgReplication(pte);
					initPte(pte);
					rctl->ptxn_laststop += 1;
					if (rctl->ptxn_laststop >= rctl->ptxn_head)
						rctl->ptxn_laststop = 0;
					return pte;
				}
			}
			if (rctl->ptxn_laststop == old_laststop) {
				if (!pass)
					pass = true;
				else {
					/* we have done two round search, no in-valid slot now, must return */
					break;
				}
			}
		}
	}

	return NULL;
}

void
storePteToPgReplication(PeerTxnEntry *pte)
{
	int			  i;
	Relation      rep_rel = NULL;
	Relation      idx_rel = NULL;
	IndexScanDesc scan;
	ScanKey       skeys;
	HeapTuple     tuple;
	HeapTuple     newtuple;
	int           opflag;       /* 0 - insert, 1 - update */
	Datum         values[Natts_pg_replication];
	bool          nulls[Natts_pg_replication];
	bool          doReplace[Natts_pg_replication];

	/* search for the old tuple first */
	skeys = (ScanKey)palloc0(2 * sizeof(ScanKeyData));
	ScanKeyInit(&skeys[0], 1, BTEqualStrategyNumber, getPgReplicationEqFunc(), pte->origin_node_id);
	ScanKeyInit(&skeys[1], 2, BTEqualStrategyNumber, getPgReplicationEqFunc(), pte->origin_xid);
	rep_rel = heap_open(ReplicationRelationId, AccessShareLock);
	idx_rel = index_open(ReplicationOriginIndexId, AccessShareLock);
	scan = index_beginscan(rep_rel, idx_rel, SnapshotNow, 2, skeys);
	tuple = index_getnext(scan, ForwardScanDirection);
	if (tuple) {
		/* already have a record, but the old one is useless now */
		newtuple = heap_copytuple(tuple);
		opflag = 1;
	}
	else {
		/* record not exist, so we insert new one */
		opflag = 0;
	}
	index_endscan(scan);
	pfree(skeys);
	index_close(idx_rel, NoLock);
	heap_close(rep_rel, NoLock);

	/* now either insert or update */
	rep_rel = heap_open(ReplicationRelationId, RowExclusiveLock);
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(doReplace, true, sizeof(doReplace));
	values[Anum_pg_replication_reporiginnodeid - 1] = Int32GetDatum(pte->origin_node_id);
	values[Anum_pg_replication_reporiginxid - 1] = TransactionIdGetDatum(pte->origin_xid);
	values[Anum_pg_replication_replocalxid - 1] = TransactionIdGetDatum(pte->local_xid);
	values[Anum_pg_replication_replocalcoid -1] = TransactionIdGetDatum(pte->local_coid);
	if (opflag == 0) {
		newtuple = heap_form_tuple(RelationGetDescr(rep_rel), values, nulls);
		simple_heap_insert(rep_rel, newtuple);
		CatalogUpdateIndexes(rep_rel, newtuple);
	}
	else if (opflag == 1) {
		newtuple = heap_modify_tuple(newtuple, RelationGetDescr(rep_rel), values, nulls, doReplace);
		simple_heap_update(rep_rel, &newtuple->t_self, newtuple);
		CatalogUpdateIndexes(rep_rel, newtuple);
	}
	heap_close(rep_rel, NoLock);
}

void
insertPteHash(PeerTxnEntry *pte)
{
	PteOriginHashEntry    *rorigin    = NULL;
	PteLocalXidHashEntry  *rlocalxid  = NULL;
	PteLocalCoidHashEntry *rlocalcoid = NULL;
	int8                   skorigin;
	int4                   sklocalxid;
	int4                   sklocalcoid;
	bool                   found;

	skorigin = FORM_ORIGIN_NODE_ID_XID(pte->origin_node_id, pte->origin_xid);
	hash_search(PteOriginHash, &skorigin, HASH_REMOVE, &found);
	rorigin = (PteOriginHashEntry*)hash_search(PteOriginHash, &skorigin,
	                                           HASH_ENTER, &found);
	rorigin->pte = pte;

	sklocalxid = pte->local_xid;
	hash_search(PteLocalXidHash, &sklocalxid, HASH_REMOVE, &found);
	rlocalxid = (PteLocalXidHashEntry*)hash_search(PteLocalXidHash, &sklocalxid,
	                                               HASH_ENTER, &found);
	rlocalxid->pte = pte;

	sklocalcoid = pte->local_coid;
	hash_search(PteLocalCoidHash, &sklocalcoid, HASH_REMOVE, &found);
	rlocalcoid = (PteLocalCoidHashEntry*)hash_search(PteLocalCoidHash, &sklocalcoid,
	                                                 HASH_ENTER, &found);
	rlocalcoid->pte = pte;
}

void
deletePteHash(PeerTxnEntry *pte)
{
	int8 skorigin;
	int4 sklocalxid;
	int4 sklocalcoid;
	bool found;

	skorigin = FORM_ORIGIN_NODE_ID_XID(pte->origin_node_id, pte->origin_xid);
	hash_search(PteOriginHash, &skorigin, HASH_REMOVE, &found);

	sklocalxid = pte->local_xid;
	hash_search(PteLocalXidHash, &sklocalxid, HASH_REMOVE, &found);

	sklocalcoid = pte->local_coid;
	hash_search(PteLocalCoidHash, &sklocalcoid, HASH_REMOVE, &found);
}

void
store_transaction_coid(NodeId origin_node_id, TransactionId origin_xid,
					   CommitOrderId local_coid)
{
	PeerTxnEntry   *pte;

	Assert(CommitOrderIdIsValid(local_coid));

#ifdef COORDINATOR_DEBUG
	elog(DEBUG5, "Coordinator: storing origin node %d xid %d -> local coid: %d",
		 origin_node_id, origin_xid, local_coid);
#endif

	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile ReplLutCtlData *rctl = ReplLutCtl;

		SpinLockAcquire(&rctl->ptxn_lock);

		pte = find_entry_for(origin_node_id, origin_xid);
		if (!pte)
		{
			pte = alloc_new_entry();
			pte->origin_node_id = origin_node_id;
			pte->origin_xid = origin_xid;
			pte->local_xid = InvalidTransactionId;
		}

		pte->local_coid = local_coid;
		insertPteHash(pte);

		SpinLockRelease(&rctl->ptxn_lock);
	}
	END_CRIT_SECTION();
}

void
store_transaction_local_xid(NodeId origin_node_id, TransactionId origin_xid,
							TransactionId local_xid)
{
	PeerTxnEntry   *pte;

	Assert(TransactionIdIsValid(local_xid));

#ifdef COORDINATOR_DEBUG
	elog(DEBUG5, "bg worker [%d/%d]: storing origin node %d xid %d -> local xid: %d",
		 MyProcPid, MyBackendId, origin_node_id, origin_xid, local_xid);
#endif

	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile ReplLutCtlData *rctl = ReplLutCtl;

		SpinLockAcquire(&rctl->ptxn_lock);

		pte = find_entry_for(origin_node_id, origin_xid);
		if (!pte)
		{
			pte = alloc_new_entry();
			pte->origin_node_id = origin_node_id;
			pte->origin_xid = origin_xid;
			pte->local_coid = InvalidCommitOrderId;
		}

		pte->local_xid = local_xid;
		insertPteHash(pte);

		SpinLockRelease(&rctl->ptxn_lock);
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
			deletePteHash(pte);
			pte->origin_node_id = InvalidNodeId;
			pte->valid = false;
		}

		SpinLockRelease(&rctl->ptxn_lock);
	}
	END_CRIT_SECTION();
}

void
get_origin_by_local_xid(TransactionId local_xid,
						NodeId *origin_node_id, TransactionId *origin_xid)
{
	int			   i;
	PeerTxnEntry  *pte;
	Relation       rep_rel = NULL;
	Relation       idx_rel = NULL;
	IndexScanDesc  scan;
	ScanKey        skeys;
	HeapTuple      tuple;
	bool           found   = false;
	bool           isnull;

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
		for (i = rctl->ptxn_tail; i < rctl->ptxn_head; i++)
		{
#if 0
			elog(DEBUG5, "    node %d xid %d -> local coid %d xid %d",
				 pte[i].origin_node_id, pte[i].origin_xid,
				 pte[i].local_coid, pte[i].local_xid);
#endif

			if ((pte[i].origin_node_id != InvalidNodeId) &&
				(pte[i].local_xid == local_xid))
			{
				*origin_node_id = pte[i].origin_node_id;
				*origin_xid = pte[i].origin_xid;
				found = true;
				break;
			}
		}

		SpinLockRelease(&rctl->ptxn_lock);

		if (!found) {
			/* Not found in shmem, now try pg_replication */
			skeys = (ScanKey)palloc0(1 * sizeof(ScanKeyData));
			ScanKeyInit(&skeys[0], 1, BTEqualStrategyNumber,
			            getPgReplicationEqFunc(), local_xid);
			rep_rel = heap_open(ReplicationRelationId, AccessShareLock);
			idx_rel = index_open(ReplicationXidIndexId, AccessShareLock);
			scan = index_beginscan(rep_rel, idx_rel, SnapshotNow, 1, skeys);
			tuple = index_getnext(scan, ForwardScanDirection);
			if (tuple) {
				*origin_node_id = fastgetattr(tuple, Anum_pg_replication_reporiginnodeid,
				                              rep_rel->rd_att, &isnull);
				*origin_xid = fastgetattr(tuple, Anum_pg_replication_reporiginxid,
				                          rep_rel->rd_att, &isnull);
			}
			index_endscan(scan);
			pfree(skeys);
			index_close(idx_rel, NoLock);
			heap_close(rep_rel, NoLock);
		}
	}
	END_CRIT_SECTION();
}


void
get_local_xid_by_coid(CommitOrderId coid, TransactionId *local_xid)
{
	int			   i;
	PeerTxnEntry  *pte;
	Relation       rep_rel = NULL;
	Relation       idx_rel = NULL;
	IndexScanDesc  scan;
	ScanKey        skeys;
	HeapTuple      tuple;
	bool           found   = false;
	bool           isnull;

	*local_xid = InvalidTransactionId;

	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile ReplLutCtlData *rctl = ReplLutCtl;

		SpinLockAcquire(&rctl->ptxn_lock);

		pte = (PeerTxnEntry*) &rctl[1];

		/* we don't support wrapping, yet */
		Assert(rctl->ptxn_head >= rctl->ptxn_tail);
		for (i = rctl->ptxn_tail; i < rctl->ptxn_head; i++)
		{
#if 0
			elog(DEBUG5, "    node %d xid %d -> local coid %d xid %d",
				 pte[i].origin_node_id, pte[i].origin_xid,
				 pte[i].local_coid, pte[i].local_xid);
#endif

			if (pte[i].local_coid == coid)
			{
				*local_xid = pte[i].local_xid;
				found = true;
				break;
			}
		}

		SpinLockRelease(&rctl->ptxn_lock);

		if (!found) {
			/* Not found in shmem, now try pg_replication */
			skeys = (ScanKey)palloc0(1 * sizeof(ScanKeyData));
			ScanKeyInit(&skeys[0], 1, BTEqualStrategyNumber,
			            getPgReplicationEqFunc(), coid);
			rep_rel = heap_open(ReplicationRelationId, AccessShareLock);
			idx_rel = index_open(ReplicationCoidIndexId, AccessShareLock);
			scan = index_beginscan(rep_rel, idx_rel, SnapshotNow, 1, skeys);
			tuple = index_getnext(scan, ForwardScanDirection);
			if (tuple) {
				*local_xid = fastgetattr(tuple, Anum_pg_replication_replocalxid,
				                         rep_rel->rd_att, &isnull);
			}
			index_endscan(scan);
			pfree(skeys);
			index_close(idx_rel, NoLock);
			heap_close(rep_rel, NoLock);
		}
	}
	END_CRIT_SECTION();
}

CommitOrderId
get_local_coid_by_origin(NodeId origin_node_id, TransactionId origin_xid)
{
	int			   i;
	PeerTxnEntry  *pte;
	CommitOrderId  coid    = InvalidCommitOrderId;
	Relation       rep_rel = NULL;
	Relation       idx_rel = NULL;
	IndexScanDesc  scan;
	ScanKey        skeys;
	HeapTuple      tuple;
	bool           found   = false;
	bool           isnull;

	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile ReplLutCtlData *rctl = ReplLutCtl;

		SpinLockAcquire(&rctl->ptxn_lock);

		pte = (PeerTxnEntry*) &rctl[1];

		/* we don't support wrapping, yet */
		Assert(rctl->ptxn_head >= rctl->ptxn_tail);
		for (i = rctl->ptxn_tail; i < rctl->ptxn_head; i++)
		{
#if 0
			elog(DEBUG5, "    node %d xid %d -> local coid %d xid %d",
				 pte[i].origin_node_id, pte[i].origin_xid,
				 pte[i].local_coid, pte[i].local_xid);
#endif

			if ((pte[i].origin_node_id == origin_node_id) &&
				(pte[i].origin_xid == origin_xid))
			{
				coid = pte[i].local_coid;
				found = true;
				break;
			}
		}

		SpinLockRelease(&rctl->ptxn_lock);

		if (!found) {
			/* Not found in shmem, now try pg_replication */
			skeys = (ScanKey)palloc0(2 * sizeof(ScanKeyData));
			ScanKeyInit(&skeys[0], 1, BTEqualStrategyNumber,
			            getPgReplicationEqFunc(), origin_node_id);
			ScanKeyInit(&skeys[1], 2, BTEqualStrategyNumber,
			            getPgReplicationEqFunc(), origin_xid);
			rep_rel = heap_open(ReplicationRelationId, AccessShareLock);
			idx_rel = index_open(ReplicationOriginIndexId, AccessShareLock);
			scan = index_beginscan(rep_rel, idx_rel, SnapshotNow, 2, skeys);
			tuple = index_getnext(scan, ForwardScanDirection);
			if (tuple) {
				coid = fastgetattr(tuple, Anum_pg_replication_replocalcoid,
				                   rep_rel->rd_att, &isnull);
			}
			index_endscan(scan);
			pfree(skeys);
			index_close(idx_rel, NoLock);
			heap_close(rep_rel, NoLock);
		}
	}
	END_CRIT_SECTION();

	return coid;
}

CommitOrderId
get_local_coid_by_local_xid(TransactionId local_xid)
{
	int			   i;
	PeerTxnEntry  *pte;
	CommitOrderId  coid    = InvalidCommitOrderId;
	Relation       rep_rel = NULL;
	Relation       idx_rel = NULL;
	IndexScanDesc  scan;
	ScanKey        skeys;
	HeapTuple      tuple;
	bool           found   = false;
	bool           isnull;

	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile ReplLutCtlData *rctl = ReplLutCtl;

		SpinLockAcquire(&rctl->ptxn_lock);

		pte = (PeerTxnEntry*) &rctl[1];

		/* we don't support wrapping, yet */
		Assert(rctl->ptxn_head >= rctl->ptxn_tail);
		for (i = rctl->ptxn_tail; i < rctl->ptxn_head; i++)
		{
#if 0
			elog(DEBUG5, "    node %d xid %d -> local coid %d xid %d",
				 pte[i].origin_node_id, pte[i].origin_xid,
				 pte[i].local_coid, pte[i].local_xid);
#endif

			if (pte[i].local_xid == local_xid)
			{
				coid = pte[i].local_coid;
				found = true;
				break;
			}
		}

		SpinLockRelease(&rctl->ptxn_lock);

		if (!found) {
			/* Not found in shmem, now try pg_replication */
			skeys = (ScanKey)palloc0(1 * sizeof(ScanKeyData));
			ScanKeyInit(&skeys[0], 1, BTEqualStrategyNumber,
			            getPgReplicationEqFunc(), local_xid);
			rep_rel = heap_open(ReplicationRelationId, AccessShareLock);
			idx_rel = index_open(ReplicationXidIndexId, AccessShareLock);
			scan = index_beginscan(rep_rel, idx_rel, SnapshotNow, 1, skeys);
			tuple = index_getnext(scan, ForwardScanDirection);
			if (tuple) {
				coid = fastgetattr(tuple, Anum_pg_replication_replocalcoid,
				                   rep_rel->rd_att, &isnull);
			}
			index_endscan(scan);
			pfree(skeys);
			index_close(idx_rel, NoLock);
			heap_close(rep_rel, NoLock);
		}
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
	int            i;
	PeerTxnEntry  *pte;
	CommitOrderId  ret = InvalidCommitOrderId;

	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile ReplLutCtlData *rctl = ReplLutCtl;
		SpinLockAcquire(&rctl->ptxn_lock);

		pte = (PeerTxnEntry*) &rctl[1];
		for (i=rctl->ptxn_tail; i<rctl->ptxn_head; ++i) {
			if (pte[i].valid) {
				if (ret == InvalidCommitOrderId) {
					ret = pte[i].local_coid;
				}
				else if (pte[i].local_coid < ret) {
					ret = pte[i].local_coid;
				}
			}
		}

		SpinLockRelease(&rctl->ptxn_lock);
	}
	END_CRIT_SECTION();

	return ret;
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
		while (!TransactionIdIsValid(xid))
		{
			if (MyProc->abortFlag)
				goto abort_waiting;

			elog(DEBUG3, "bg worker [%d/%d]: WaitUntilCommittable(): coid %d: waiting for coid %d",
				 MyProcPid, MyBackendId, my_coid, dep_coid);
			pg_usleep(30000L);

			get_local_xid_by_coid(dep_coid, &xid);
		}

		/*
		 * Wait until the transaction has committed or aborted.
		 */
		while (TransactionIdIsInProgress(xid))
		{
			if (MyProc->abortFlag)
				goto abort_waiting;

			elog(DEBUG3, "bg worker [%d/%d]: WaitUntilCommittable(): coid %d: waiting for txn %d",
				 MyProcPid, MyBackendId, my_coid, xid);
			pg_usleep(30000L);
		}

		/* increment to process the next higher coid */
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

