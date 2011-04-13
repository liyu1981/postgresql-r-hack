/*-------------------------------------------------------------------------
 *
 * replication.h
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

#ifndef REPLICATION_H
#define REPLICATION_H

#include "access/htup.h"        /* only for cset_form_tuple */
#include "executor/tuptable.h"  /* only for cset_form_tuple */
#include "utils/hsearch.h"
#include "utils/relcache.h"     /* only for cset_form_tuple */
#include "storage/buffer.h"
#include "storage/imsg.h"
#include "storage/spin.h"

/*
 * database state of a node
 */
typedef enum {
	RDBS_UNKNOWN = 0,
	RDBS_JOIN_REQUESTED = 1,
	RDBS_AWAITING_MAJORITY = 5,    /* not used, yet */
	RDBS_RECOVERING_SCHEMA = 10,
	RDBS_RECOVERING_DATA = 11,
	RDBS_OPERATING = 31,
	RDBS_UNABLE_TO_CONNECT = 90,
} rdb_state;

typedef uint32 NodeId;

#define InvalidNodeId		((NodeId) 0)
#define FirstNormalNodeId	((NodeId) 1)

/*
 * CommitOrderIds, inspired by TransactionId from transam.h.
 */
typedef uint32 CommitOrderId;

#define InvalidCommitOrderId			((CommitOrderId) 0)
#define KnownGoodCommitOrderId			((CommitOrderId) 1)
#define FirstNormalCommitOrderId		((CommitOrderId) 2)
#define MaxCommitOrderId				((CommitOrderId) 0xFFFFFFFF)
#define MaxConcurrentCommitOrderIds		((CommitOrderId) 0x00FFFFFF)

#define CommitOrderIdIsValid(coid)		((coid) != InvalidCommitOrderId)
#define CommitOrderIdIsNormal(coid)		((coid) >= FirstNormalCommitOrderId)

/* requires normal commit order ids */
#define CommitOrderIdsInSameRange(a, b) \
	(Abs((int32) (b) - (int32) (a)) < \
	 MaxConcurrentCommitOrderIds)

#define CmpCommitOrderIds(a, b) \
	((int32) (a) - (int32) (b))

/* advance a commit order id variable, handling wraparound correctly */
#define CommitOrderIdAdvance(dest)	\
	do { \
		(dest)++; \
		if ((dest) < FirstNormalCommitOrderId) \
			(dest) = FirstNormalCommitOrderId; \
	} while (0)


#define IsReplicatedBackend() \
	(replication_enabled && !IsBackgroundWorkerProcess())


/* some helpful forward declarations with their typedefs */
struct s_repl_recovery_state;
typedef struct s_repl_recovery_state repl_recovery_state;
struct s_coordinator_backend_info;
typedef struct s_coordinator_backend_info coordinator_backend_info;

typedef struct s_gcs_group
{
	char							name[NAMEDATALEN];
	struct s_gcs_info			   *gcsi;
	struct s_gcs_group			   *parent;

	/*
	 * Fields tracking the state of the replicated database.
	 */
	Oid                             dboid;
	rdb_state		                db_state;

	/* the current global transaction id */
	CommitOrderId	                coid;

	/* state tracking for recovery, if the database is being recovered */
	repl_recovery_state            *rstate;

	/*
	 * The nodes of the group, hashed by the GCS node info. This
	 * HTAB is maintained by the GCS Layer. Each node also has a
	 * node_id, which points to the coordinator node id
	 * in the hashtable below.
	 */
	HTAB						   *gcs_nodes;

	/*
	 * The nodes of the group, hashed by coordinator node_id with a
	 * pointer to the above gcs_node entry.
	 */
	HTAB						   *nodes;

	/*
	 * The coordinator's node_id of the local node, may only be updated
	 * by the GC Layer during a viewchange. Should not be used by
	 * the coordinator, as this is currently only used by
	 * ensemble.
	 */
	NodeId							node_id_self_ref;

	/*
	 * State counters
	 */
	unsigned int                    num_nodes_operating;
	unsigned int					last_vc_remotes_joined;
	unsigned int					last_vc_remotes_left;

	/*
	 * Event flags
	 */
	bool							last_vc_join_pending;
	bool							last_vc_leave_pending;
} gcs_group;

/* global for the database state, per backend */
extern rdb_state db_replication_state;

/* configuration options in guc.c */
extern bool		replication_enabled;
extern bool		replication_may_seed;
extern char	   *replication_gcs;
extern char	   *replication_group_name;
extern int      replication_peer_txn_entries;
extern int      replication_co_txn_info_max;

/* local.c - worker backend functions */
extern void send_txn_aborted_msg(TransactionId xid, int errcode);
extern void send_commit_ordering_request(void);
extern void replication_global_commit(void);
extern void StartupReplication(void);
extern void RegisterDatabases(void);
extern IMessage *await_imessage(void);

extern void replication_request_sequence_increment(const char *seqname);
extern void replication_request_sequence_reset(const char *seqname,
											   int64 new_value, bool iscalled);

/* remote.c - helper backend functions */
extern void cset_get_datum(Datum *values, char *data, int numTargets,
						   AttrNumber *TargetNums, TupleDesc tdesc);
extern HeapTuple cset_form_tuple(Relation rel, HeapTuple oldTuple,
								 char *newtup_data, TupleTableSlot *slot);

/* postgres.c - over there for history reasons */
extern void bgworker_db_state_change(IMessage *msg);
extern bool bgworker_apply_cset(IMessage *msg);
extern bool bgworker_commit_request(IMessage *msg);
extern bool bgworker_abort_request(IMessage *msg);
extern void bgworker_seq_increment(IMessage *msg);
extern void bgworker_seq_setval(IMessage *msg);
extern void bgworker_recovery_restart(IMessage *msg);
extern void bgworker_schema_adaption(IMessage *msg);
extern void bgworker_recovery_data(IMessage *msg);
extern void bgworker_recovery_request(IMessage *msg);

/* coordinator.c */
extern HTAB *co_txn_info;
void init_co_txn_info_table();
void erase_co_txn_info(NodeId original_node_id, TransactionId origin_xid);

#endif   /* REPLICATION_H */
