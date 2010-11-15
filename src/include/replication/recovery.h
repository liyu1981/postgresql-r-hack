/*-------------------------------------------------------------------------
 *
 * recovery.h
 *
 *	  Prototypes and structures for recovery of replication.
 *
 * Portions Copyright (c) 2010, Translattice, Inc
 * Portions Copyright (c) 2005-2010, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef RECOVERY_H
#define RECOVERY_H

#include "lib/dllist.h"
#include "storage/imsg.h"
#include "storage/buffer.h"
#include "replication/replication.h"

#define MAX_CONCURRENT_RECOVERY_REQUESTS_PER_DB 64

typedef enum
{
	RLS_REQUEST_QUEUED,
	RLS_REQUEST_FIRED,
	RLS_STREAMING
} recovery_lane_state;

typedef struct
{
	int			round_no, curr_lane, num_lanes, chunk_no;
	const char *schemaname,
			   *relname;
	int			key_data_len;
	char	   *key_data;

	/* the node this request has been sent to */
	group_node *provider_node;

	/* for use on the provider side */
	NodeId	    subscriber_node_id;

	recovery_lane_state state;

	/* the worker backend which applies the stream for this recovery lane */
	BackendId   stream_backend_id;

	IMessage   *msg;

} recovery_request_info;

typedef struct
{
	char	   *schemaname,
			   *relname;
} recovery_relation_info;

struct s_repl_recovery_state
{
	/* a round number, incremented after every schema recovery restart */
	int			round_no;

	/* total number of open streams for recovery */
	int			open_streams;

	/* number of queued requests, including pending ones */
	int			total_requests;

	/* a list of requests, cached or pending ones */
	Dllist		request_list;

	/* the worker backend which currently does schema recovery */
	BackendId   recovery_backend_id;
};

/*
 * Mainly used to exchange info about the current recovery request with
 * istream and the callback methods.
 */
extern recovery_request_info *CurrentRecoveryRequest;

/*
 * helper functions for the coordinator.
 */
extern int relation_recovery_completed(repl_recovery_state *rs,
										const char *schemaname,
										const char *relname);

extern void coordinator_fire_recovery_requests(gcs_group *group);
extern void coordinator_restart_schema_recovery(gcs_group *group);
extern void coordinator_restart_data_recovery(gcs_group *group);
extern void coordinator_stop_data_recovery(gcs_group *group);

/*
 * recovery provider functions
 */
extern void ProcessRecoveryRequest(IMessage *msg);

/*
 * recovery subscriber functions
 */
extern void ProcessSchemaRecoveryRestart(void);
extern void ProcessSchemaAdaption(IMessage *msg);
extern void ProcessRecoveryData(IMessage *msg);

#endif   /* RECOVERY_H */

