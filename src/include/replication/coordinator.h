/*-------------------------------------------------------------------------
 *
 * coordinator.h
 *
 *	  The coordinator for helper backends.
 *
 * Portions Copyright (c) 2010, Translattice, Inc
 * Portions Copyright (c) 2001-2010, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#ifndef REPL_COORDINATOR_H
#define REPL_COORDINATOR_H

#include "postmaster/coordinator.h"
#include "replication/gc.h"
#include "replication/replication.h"
#include "storage/buffer.h"
#include "storage/proc.h"

typedef struct
{
	int		nodeid;
	Oid		oid;
	char   *name;
} cached_name_oid;


extern gcs_group *replication_group;

/* liyu: add one global flag for spread */
extern bool coordinator_now_terminate;

extern void coordinator_replication_init(void);
extern void coordinator_replication_reg_gcs(fd_set *socks, int *num_socks);
extern void coordinator_replication_check_sockets(fd_set *socks);
extern void coordinator_notify_backend_ready(BackendId backend_id);

/* imessage handler functions */
extern void handle_imessage_cset(gcs_group *group,
								 IMessage *msg, TransactionId local_xid);
extern void handle_imessage_ordering_request(gcs_group *group,
											 IMessage *msg,
											 TransactionId local_xid);
extern void handle_imessage_txn_aborted(gcs_group *group,
										IMessage *msg);
extern void handle_imessage_seq_increment(gcs_group *group,
										  IMessage *msg);
extern void handle_imessage_seq_setval(gcs_group *group,
									   IMessage *msg);

extern void handle_imessage_recovery_restart(gcs_group *group,
											 IMessage *msg);
extern void handle_imessage_recovery_data(gcs_group *group,
										  IMessage *msg);
extern void handle_imessage_recovery_request(gcs_group *group,
											 IMessage *msg);
extern void handle_imessage_recovery_lane_completed(gcs_group *group,
													IMessage *msg);
extern void handle_imessage_schema_adaption(gcs_group *group,
											IMessage *msg);


/* recovery uses this function */
extern void coordinator_db_state_change(co_database *rdbi,
								 rdb_state new_state);
extern void coordinator_restart_data_recovery(gcs_group *group);

/* gc layer callbacks */
extern void gcsi_gcs_ready(const gcs_info *gcsi);
extern void gcsi_gcs_failed(const gcs_info *gcsi);

extern void gcsi_viewchange_start(gcs_group *group);
extern void gcsi_viewchange_stop(gcs_group *group);
extern void gcsi_node_changed(gcs_group *group, group_node *node,
							  const gcs_viewchange_type type);
extern void coordinator_handle_gc_message(gcs_group *group,
								   group_node *sender_node,
								   const char type, buffer *b);

/* called from postmaster/coordinator.c */
extern void coordinator_handle_gcs_connections(void);
extern void coordinator_check_seed_state(void);

extern void coordinator_prepare_job_delivery(gcs_group *group, IMessage *msg,
											 BackendId recipient);


extern group_node *gcsi_add_node(gcs_group *group, uint32 new_node_id);
extern group_node *gcsi_get_node(const gcs_group *group, uint32 node_id);
extern void gcsi_remove_node(gcs_group *group, const group_node* node);

#endif   /* REPL_COORDINATOR_H */
