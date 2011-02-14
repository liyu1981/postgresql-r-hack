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
#include "storage/imsg.h"
#include "storage/lwlock.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "postmaster/coordinator.h"
#include "replication/replication.h"
#include "replication/cset.h"
#include "replication/utils.h"

static int ReplLutCtlShmemSize(void);
void InitSharedPeerTxnQueue(int size);
void InitSharedGOLHash(int size);
void InitSharedLOLHash(int size);

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


#define PeerTxnEntries 1000
#define GOLEntries 1000
#define LOLEntries 1000

typedef struct PeerTxnEntry
{
	NodeId			origin_node_id;
	TransactionId	origin_xid;
	TransactionId	local_xid;
	CommitOrderId	local_coid;
} PeerTxnEntry;

typedef struct GlobalOidLookupEntry
{
	Oid		loid, goid;
} GlobalOidLookupEntry;

typedef struct LocalOidLookupEntry
{
	Oid		goid, loid;
} LocalOidLookupEntry;

/* globals keeping pointers to the shmem areas */
ReplLutCtlData *ReplLutCtl;
HTAB *SharedPeerTxnHash;
HTAB *SharedGOLHash;
HTAB *SharedLOLHash;


static int
ReplLutCtlShmemSize(void)
{
	Size		size;

	/* size of the control structure itself */
	size = MAXALIGN(sizeof(ReplLutCtlData));

	/* size of peer transaction queue */
	size = add_size(size, mul_size(PeerTxnEntries,
		MAXALIGN(sizeof(PeerTxnEntry))));

	return size;
}


int
ReplLutShmemSize(void)
{
	Size		size = ReplLutCtlShmemSize();

	/* size of local oid -> global oid LUT */
	size = add_size(size, mul_size(GOLEntries, 
		MAXALIGN(sizeof(HASHELEMENT)) +
			MAXALIGN(sizeof(GlobalOidLookupEntry))));

	/* size of control structure */
	size = add_size(size, mul_size(LOLEntries,
		MAXALIGN(sizeof(HASHELEMENT)) +
			MAXALIGN(sizeof(LocalOidLookupEntry))));

	return size;
}


void
InitSharedPeerTxnQueue(int size)
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

		pte = (PeerTxnEntry*) &rctl[1];

		for (i = 0; i < PeerTxnEntries; i++)
			pte[i].origin_node_id = InvalidNodeId;

		SpinLockRelease(&rctl->ptxn_lock);
	}
	END_CRIT_SECTION();
}


void
InitSharedGOLHash(int size)
{
	HASHCTL		info;

	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile ReplLutCtlData *rctl = ReplLutCtl;

		SpinLockAcquire(&rctl->gol_lock);

		/* BufferTag maps to Buffer */
		info.keysize = sizeof(Oid);
		info.entrysize = sizeof(GlobalOidLookupEntry);
		info.hash = oid_hash;

		SharedGOLHash = ShmemInitHash("Shared Global Oid Lookup Table",
			size, size, &info,
			HASH_ELEM | HASH_FUNCTION | HASH_PARTITION);

		if (!SharedGOLHash)
			elog(FATAL, "could not initialize shared buffer hash table");

		SpinLockRelease(&rctl->ptxn_lock);
	}
	END_CRIT_SECTION();
}


void
InitSharedLOLHash(int size)
{
	HASHCTL		info;

	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile ReplLutCtlData *rctl = ReplLutCtl;

		SpinLockAcquire(&rctl->lol_lock);

		/* BufferTag maps to Buffer */
		info.keysize = sizeof(Oid);
		info.entrysize = sizeof(LocalOidLookupEntry);
		info.hash = oid_hash;

		SharedLOLHash = ShmemInitHash("Shared Local Oid Lookup Table",
			size, size, &info,
			HASH_ELEM | HASH_FUNCTION | HASH_PARTITION);

		if (!SharedGOLHash)
			elog(FATAL, "could not initialize shared buffer hash table");

		SpinLockRelease(&rctl->ptxn_lock);
	}
	END_CRIT_SECTION();
}


void
ReplLutShmemInit(void)
{
	bool		found;

#ifdef IMSG_DEBUG
	elog(DEBUG3, "ReplLutShmemInit(): initializing shared memory");
	elog(DEBUG5, "                    of size %d", ReplLutCtlShmemSize());
#endif

	ReplLutCtl = (ReplLutCtlData *)
		ShmemInitStruct("Replication Lookup Ctl",
						ReplLutCtlShmemSize(), &found);

	if (found)
		return;

	memset(ReplLutCtl, 0, sizeof(ReplLutCtlData));

	/* initialize all the spin locks */
	SpinLockInit(&ReplLutCtl->ptxn_lock);
	SpinLockInit(&ReplLutCtl->gol_lock);
	SpinLockInit(&ReplLutCtl->lol_lock);

	/* initialize all the hash tables */
	InitSharedPeerTxnQueue(PeerTxnEntries);
#if 0
	InitSharedGOLHash(GOLEntries);
	InitSharedLOLHash(LOLEntries);
#endif
}

static PeerTxnEntry *
find_entry_for(NodeId origin_node_id, TransactionId origin_xid)
{
	volatile ReplLutCtlData    *rctl = ReplLutCtl;
	PeerTxnEntry			   *pte = (PeerTxnEntry*) &rctl[1];
	int							i;

	/* we don't support wrapping, yet */
	Assert(rctl->ptxn_head >= rctl->ptxn_tail);
	for (i = rctl->ptxn_tail; i < rctl->ptxn_head; i++)
	{
		if (pte[i].origin_node_id == origin_node_id &&
			pte[i].origin_xid == origin_xid)
		{
#ifdef CSET_APPL_DEBUG
			elog(DEBUG3, "bg worker [%d/%d]: found entry for (%d/%d)",
				 MyProcPid, MyBackendId, origin_node_id, origin_xid);
#endif

			return &pte[i];
		}
	}

	return NULL;
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
			pte = (PeerTxnEntry*) &rctl[1];
			pte = &pte[rctl->ptxn_head++];
			pte->origin_node_id = origin_node_id;
			pte->origin_xid = origin_xid;
			pte->local_xid = InvalidTransactionId;
		}

		pte->local_coid = local_coid;

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
			pte = (PeerTxnEntry*) &rctl[1];
			pte = &pte[rctl->ptxn_head++];
			pte->origin_node_id = origin_node_id;
			pte->origin_xid = origin_xid;
			pte->local_coid = InvalidCommitOrderId;
		}

		pte->local_xid = local_xid;

		SpinLockRelease(&rctl->ptxn_lock);
	}
	END_CRIT_SECTION();
}

void
get_origin_by_local_xid(TransactionId local_xid,
						NodeId *origin_node_id, TransactionId *origin_xid)
{
	int				i;
	PeerTxnEntry   *pte;

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
				break;
			}
		}

		SpinLockRelease(&rctl->ptxn_lock);
	}
	END_CRIT_SECTION();
}


void
get_local_xid_by_coid(CommitOrderId coid, TransactionId *local_xid)
{
	int				i;
	PeerTxnEntry   *pte;

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
				break;
			}
		}

		SpinLockRelease(&rctl->ptxn_lock);
	}
	END_CRIT_SECTION();
}

CommitOrderId
get_local_coid_by_origin(NodeId origin_node_id, TransactionId origin_xid)
{
	int				i;
	PeerTxnEntry   *pte;
	CommitOrderId   coid = InvalidCommitOrderId;

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
				break;
			}
		}

		SpinLockRelease(&rctl->ptxn_lock);
	}
	END_CRIT_SECTION();

	return coid;
}

CommitOrderId
get_local_coid_by_local_xid(TransactionId local_xid)
{
	int				i;
	PeerTxnEntry   *pte;
	CommitOrderId   coid = InvalidCommitOrderId;

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
				break;
			}
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
	int				i;
	PeerTxnEntry   *pte;

	bool			found_eff_coid = false,
					found_req_coid = false;

	*eff_coid = KnownGoodCommitOrderId;
	*req_coid = KnownGoodCommitOrderId;

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
				elog(LOG, "found eff_coid: origin_node_id=%d, origin_xid=%d, local_xid=%d, local_coid=%d", pte[i].origin_node_id, pte[i].origin_xid, pte[i].local_xid, pte[i].local_coid);
				*eff_coid = pte[i].local_coid;
				if (found_req_coid)
					break;
				found_eff_coid = true;
			}

			if ((pte[i].origin_node_id == origin_node_id) &&
				(pte[i].origin_xid == origin_xid))
			{
				elog(LOG, "found req_coid: origin_node_id=%d, origin_xid=%d, local_xid=%d, local_coid=%d", pte[i].origin_node_id, pte[i].origin_xid, pte[i].local_xid, pte[i].local_coid);
				*req_coid = pte[i].local_coid;
				if (found_eff_coid)
					break;
				found_req_coid = true;
			}
		}

		SpinLockRelease(&rctl->ptxn_lock);
	}
	END_CRIT_SECTION();

	if (found_eff_coid && !found_req_coid) {
		*req_coid = *eff_coid;
	}
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
	TransactionId   xid;
	CommitOrderId   my_coid = InvalidCommitOrderId,
		            dep_coid;
	PeerTxnEntry   *pte;

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
	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile ReplLutCtlData *rctl = ReplLutCtl;
		SpinLockAcquire(&rctl->ptxn_lock);

		pte = (PeerTxnEntry*) &rctl[1];
		dep_coid = pte[rctl->ptxn_tail].local_coid;

		SpinLockRelease(&rctl->ptxn_lock);
	}
	END_CRIT_SECTION();

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

