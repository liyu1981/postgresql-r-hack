/*-------------------------------------------------------------------------
 *
 * utils.h
 *
 * Copyright (c) 2001-2010, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#ifndef REPLICATION_UTILS_H
#define REPLICATION_UTILS_H

#include "replication/coordinator.h"
#include "replication/cset.h"
#include "replication/replication.h"
#include "nodes/nodes.h"
#include "storage/proc.h"

#define REPLICATION_PRINT_MEMORY(pointer, size, withchar)	  \
	{ \
	    printf("%%%%% Memory start from %x: \n", (pointer)); \
		int i; \
		for(i=0;i<(size); ++i) { \
		    if(i%8 == 0 && i != 0) \
			    printf("\n"); \
		    if((withchar)) \
			    printf("%2x('%1c') ", *((pointer)+i), *((pointer)+i)); \
		    else \
			    printf("%2x ", *((pointer)+i)); \
		} \
		printf("\n"); \
	    printf("%%%%% \n"); \
    }

/* helper function in coordinator.c */
extern void *std_memory_context_alloc(size_t size);

/* mainly for printing reabable debug output */

extern char *decode_database_state(rdb_state state);

extern char *decode_command_type(CsetCmdType type);

/* shared memory structs */
typedef struct ReplLutCtlData
{
	/*
	 * Used to transfer the name of the database, for which the postmaster
	 * should start a helper backend.
	 */
	NameData	dbname;

	/* lock for editing the message queue */
	slock_t		ptxn_lock;
	slock_t		origin_lock;
	slock_t		localxid_lock;
	slock_t     localcoid_lock;

	/*
	 * The head of the round robin memory, i.e. where new elements are
	 * inserted. The tail is where our list ends. Normally, we process
	 * the list from tail to head when searching a transaction's
	 * ids.
	 *
	 * Head and tail may well be equal (in which case the list is empty).
	 * In case ptxn_head < ptxn_tail, our list has wrapped.
	 */
	int			ptxn_head;
	int			ptxn_tail;
	int         ptxn_laststop;
} ReplLutCtlData;

extern ReplLutCtlData *ReplLutCtl;

/* shared memory helper functions */
extern int ReplLutShmemSize(void);
extern void ReplLutShmemInit(void);

extern void store_transaction_coid(NodeId origin_node_id,
								   TransactionId origin_xid,
								   CommitOrderId local_coid);

extern void store_transaction_local_xid(NodeId origin_node_id,
										TransactionId origin_xid,
										TransactionId local_xid);

extern void erase_transaction(NodeId origin_node_id,
                              TransactionId origin_xid,
                              bool is_commit);

/* query functions */
extern void get_multi_coids(CommitOrderId *eff_coid,
							CommitOrderId *req_coid,
							TransactionId local_xid,
							NodeId origin_node_id,
							TransactionId origin_xid);

extern void get_local_xid_by_coid(CommitOrderId coid,
								  TransactionId *local_xid);

extern void get_origin_by_local_xid(TransactionId local_xid,
									NodeId *origin_node_id,
									TransactionId *origin_xid);

extern CommitOrderId get_local_coid_by_local_xid(TransactionId local_xid);
extern CommitOrderId get_local_coid_by_origin(NodeId origin_node_id,
											  TransactionId origin_xid);

extern void resolve_conflict(TransactionId xid_other, CommitOrderId *my_coid);

/* called by xact.c, just before committing */
extern void WaitUntilCommittable(void);

/* called by backends and workers on startup */
extern rdb_state get_db_replication_state(Oid dboid);

#endif   /* REPLICATION_UTILS_H */

