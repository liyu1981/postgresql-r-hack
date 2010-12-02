/*-------------------------------------------------------------------------
 *
 * coordinator.c
 *
 * Replication specific additions to the coordinator.
 *
 * Portions Copyright (c) 2010, Translattice, Inc
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * Based on the work of Win Bausch and Bettina Kemme (ETH Zurich)
 * and some earlier efforts trying to integrate Postgres-R into
 * Postgres 7.2
 *
 *-------------------------------------------------------------------------
 */

#include <unistd.h>
#include <errno.h>

#include "postgres.h"
#include "miscadmin.h"

#include "access/heapam.h"
#include "access/transam.h"
#include "catalog/pg_database.h"
#include "postmaster/coordinator.h"
#include "postmaster/postmaster.h"
#include "lib/dllist.h"
#include "libpq/hba.h"
#include "libpq/pqsignal.h"
#include "replication/replication.h"
#include "replication/coordinator.h"
#include "replication/recovery.h"
#include "replication/utils.h"
#include "replication/gc.h"
#include "storage/fd.h"
#include "storage/buffer.h"
#include "storage/imsg.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/ps_status.h"

/* global variables */
static gcs_info *gcsi = NULL;			/* the active gcs connection */
static bool seed_mode = false;
static HTAB *co_txn_info = NULL;

typedef struct CoTransactionInfo
{
	NodeId			origin_node_id;
	TransactionId	origin_xid;
	BackendId       local_backend_id;
	TransactionId	local_xid;
	CommitOrderId	local_coid;
	int             deliverable_cset_no;
	int             total_csets;
	bool            aborted;
} CoTransactionInfo;


/* the main replication group used as a parent group for all databases */
gcs_group *replication_group = NULL;

static void coordinator_cleanup(int code, Datum arg);

/*
 * coordinator_process_backend_requests
 *
 * If there's no request in progress, we check the list of databases and
 * request backends as required.
 */
static void gc_broadcast_imsg(const gcs_group *group, const IMessage *msg,
							  bool atomic);
static void gc_unicast_imsg(const gcs_group *group, const IMessage *msg);

static void coordinator_broadcast_node_state(gcs_group *group);
static void coordinator_send_db_state_to_backend(pid_t pid, BackendId id,
												 rdb_state state);

static void coordinator_join_main_group(void);
static void coordinator_join_group_for(co_database* codb);
static void coordinator_join_database_groups(void);

static void coordinator_check_group_liveness(gcs_group *group);
static void coordinator_check_group_state(gcs_group *group);
static void coordinator_check_recovery_streams(gcs_group *group);
static void coordinator_state_change(gcs_group *group, rdb_state new_state);

static CoTransactionInfo *get_co_transaction_info(NodeId origin_node_id,
												  TransactionId origin_xid);
static recovery_request_info *get_recovery_stream_info(gcs_group *group,
   int round_no, int lane_no, int num_lanes,
   const char *schemaname, const char *relname);


/*
 *    coordinator_cleanup()
 *
 * cleans up the coordinator global data structures, installed as an
 * on_proc_exit callback.
 */
static void
coordinator_cleanup(int code, Datum arg)
{
	/*
	 * shutdown the connection to the group communication system
	 */
    if (gcsi && gcsi->conn_state == GCSCS_ESTABLISHED)
		gcsi->funcs.disconnect(gcsi);
}

static void
coordinator_join_main_group()
{
	co_database *codb;

	LWLockAcquire(CoordinatorDatabasesLock, LW_SHARED);
    codb = get_co_database(TemplateDbOid);
	coordinator_join_group_for(codb);
	LWLockRelease(CoordinatorDatabasesLock);
}

/*
 * coordinator_join_group_for
 *
 * ATTN: the caller needs to hold the CoordinatorDatabasesLock.
 */
static void
coordinator_join_group_for(co_database* codb)
{
	/* liyu: codb of templateDb has group name inited to be NULL
	   so this is badly wrong :(
	 */
	/* Assert(!codb->group); */

	if (codb->codb_dboid == TemplateDbOid)
	{
#ifdef COORDINATOR_DEBUG
		elog(DEBUG1, "Coordinator: joining main group%s",
			 (seed_mode ? " (seeding)" : ""));
#endif

		replication_group = gcsi->funcs.join(gcsi, replication_group_name,
											 NULL);
		codb->group = replication_group;
	}
	else
	{
#ifdef COORDINATOR_DEBUG
		elog(DEBUG1, "Coordinator: joining group for database %d%s",
			 codb->codb_dboid, (seed_mode ? " (seeding)" : ""));
#endif

		Assert(replication_group);
		Assert(codb->group_name);
		codb->group = gcsi->funcs.join(gcsi, codb->group_name,
									   replication_group);
	}

	codb->group->dboid = codb->codb_dboid;
	coordinator_state_change(codb->group, RDBS_JOIN_REQUESTED);
}

void
handle_imessage_recovery_restart(gcs_group *group, IMessage *msg)
{
	IMessageRemove(msg);

	group->rstate->recovery_backend_id = InvalidBackendId;
	coordinator_check_recovery_streams(group);
}

void
handle_imessage_recovery_data(gcs_group *group, IMessage *msg)
{
	/* forward the imessage to the GCS */
	gc_unicast_imsg(group, msg);
	IMessageRemove(msg);
}

static void
parse_recovery_imessage(IMessage *msg, recovery_request_info *rp)
{
	buffer	b;
	int		num_key_attrs;

	IMessageGetReadBuffer(&b, msg);

	rp->round_no = get_int8(&b);
	rp->curr_lane = get_int8(&b);
	rp->num_lanes = get_int8(&b);
	rp->schemaname = get_pstring(&b);
	rp->relname = get_pstring(&b);

	if (msg->type != IMSGT_RELATION_RECOVERED)
	{
		num_key_attrs = get_int8(&b);
		if (num_key_attrs > 0)
		{
			rp->key_data_len = b.fill_size - b.ptr;
			rp->key_data = palloc(rp->key_data_len);
			memcpy(rp->key_data, (char*) b.data + b.ptr, rp->key_data_len);
		}
		else
		{
			rp->key_data_len = 0;
			rp->key_data = NULL;
		}
	}

	rp->state = RLS_REQUEST_QUEUED;
	rp->provider_node = InvalidNodeId;
}

static void
free_recovery_request_info(recovery_request_info *ri)
{
	Assert(ri->schemaname);
	if (ri->schemaname)
		pfree(ri->schemaname);

	Assert(ri->relname);
	if (ri->relname)
		pfree(ri->relname);

	/* key_data is optional, so no assertion here */
	if (ri->key_data)
		pfree(ri->key_data);

	pfree(ri);
}

static void
database_recovery_completed(gcs_group *group)
{
	coordinator_stop_data_recovery(group);

	elog(INFO, "Coordinator: database %d recovered.", group->dboid);

	/*
	 * FIXME: should only switch to OPERATING, if there are enough nodes
	 * online.
	 */
	coordinator_state_change(group, RDBS_OPERATING);

	/*
	 * If the template database has just been recovered, join all groups
	 * for each separate database and start recovery (or seeding) for them.
	 */
	if (group->dboid == TemplateDbOid)
		coordinator_join_database_groups();
}

/*
 * Handle a recovery request from a worker backend
 */
void
handle_imessage_recovery_request(gcs_group *group, IMessage *msg)
{
	recovery_request_info *ri = NULL, *rp;

	Assert(group);
	Assert(group->rstate);  /* FIXME: how safe is that assumption? */
	Assert((group->db_state == RDBS_RECOVERING_SCHEMA) ||
		   (group->db_state == RDBS_RECOVERING_DATA));

	rp = (recovery_request_info *) palloc(sizeof(recovery_request_info));
	parse_recovery_imessage(msg, rp);

	/*
	 * Scan the list of pending recovery requests. If one exists, we only need
	 * to updates its key data or mark it as done.
	 */
	ri = get_recovery_stream_info(group, rp->round_no, rp->curr_lane,
								  rp->num_lanes, rp->schemaname, rp->relname);

	/*
	 * FIXME: with the new istream stuff, the request should only be sent
	 * exactly once. Refreshing an existing request should only be required
	 * upon provider errors.
	 */
	Assert(!ri);

	/* FIXME: indentation */
				if (!ri)
				{
#ifdef COORDINATOR_DEBUG
					elog(DEBUG3, "Coordinator: new recovery request for relation %s.%s "
						 "(round %d, lane %d of %d, key_data_len: %d).",
						 rp->schemaname, rp->relname, rp->round_no,
						 rp->curr_lane, rp->num_lanes, rp->key_data_len);
#endif

					DLAddTail(&group->rstate->request_list, DLNewElem(rp));
					rp->state = RLS_REQUEST_QUEUED;
					group->rstate->total_requests++;
					rp->provider_node = NULL;
					rp->subscriber_node_id = InvalidBackendId;
					rp->chunk_no = 0;
					rp->msg = msg;
				}
				else
				{
					IMessageRemove(msg);

#ifdef COORDINATOR_DEBUG
					elog(DEBUG3, "Coordinator: update recovery request for relation %s.%s "
						 "(round %d, lane %d of %d, key_data_len: %d).",
						 rp->schemaname, rp->relname, rp->round_no,
						 rp->curr_lane, rp->num_lanes, rp->key_data_len);
#endif

					ri->round_no = rp->round_no;
					ri->key_data_len = rp->key_data_len;
					ri->state = RLS_REQUEST_QUEUED;

					/*
					 * transfer the key data, removing it from the original
					 * recovery_request_info struct, so it doesn't get
					 * pfree'd.
					 */
					ri->key_data = rp->key_data;
					rp->key_data = NULL;

					free_recovery_request_info(rp);
				}

#ifdef COORDINATOR_DEBUG
				elog(DEBUG3, "Coordinator: db %d: open recovery streams: %d/%d",
					 group->dboid, group->rstate->open_streams,
					 group->rstate->total_requests);
#endif

				/* fire requests, if possible */
				coordinator_fire_recovery_requests(group);
}

static void
coordinator_check_recovery_streams(gcs_group *group)
{
	IMessage *msg;
	co_database *codb;

#if COORDINATOR_DEBUG
	elog(DEBUG3, "Coordinator: db %d: open recovery streams: %d/%d",
		 group->dboid, group->rstate->open_streams,
		 group->rstate->total_requests);
#endif

	if (group->rstate->total_requests == 0 &&
		(group->rstate->recovery_backend_id == InvalidBackendId))
	{
		if (group->db_state == RDBS_RECOVERING_SCHEMA)
		{
			/*
			 * After the schema data has been transferred, we start the
			 * adaption job. We expect the background worker to respond with
			 * an IMSGT_SCHEMA_ADAPTION upon completion.
			 */
#if COORDINATOR_DEBUG
			elog(DEBUG3, "Coordinator: starting to recover schema of database %d",
				 group->dboid);
#endif

			msg = IMessageCreate(IMSGT_SCHEMA_ADAPTION, 0);

			LWLockAcquire(CoordinatorDatabasesLock, LW_SHARED);
			codb = hash_search(co_databases, &group->dboid,
							   HASH_FIND, NULL);
			Assert(codb);
			dispatch_job(msg, codb);
			LWLockRelease(CoordinatorDatabasesLock);
		}
		else
			database_recovery_completed(group);
	}
}

/*
 * Handle a confirmation message that a relation (or one of its
 * lanes) has been recovered.
 */
void
handle_imessage_recovery_lane_completed(gcs_group *group, IMessage *msg)
{
	Dlelem *dle;
	recovery_request_info *ri = NULL, rp;
	NodeId msg_sender;

	Assert(group);
	Assert((group->db_state == RDBS_RECOVERING_SCHEMA) ||
		   (group->db_state == RDBS_RECOVERING_DATA));

	parse_recovery_imessage(msg, &rp);
	msg_sender = msg->sender;
	IMessageRemove(msg);

	/*
	 * Scan the list of pending recovery requests. If one exists, we
	 * only need to update its key data or mark it as done.
	 */
	dle = DLGetHead(&group->rstate->request_list);

	/* FIXME: indentation */
			while (dle)
			{
				ri = DLE_VAL(dle);

				if ((ri->round_no == rp.round_no)
					&& (ri->curr_lane == rp.curr_lane)
					&& (ri->num_lanes == rp.num_lanes)
					&& (strncmp(ri->schemaname, rp.schemaname, NAMEDATALEN) == 0)
					&& (strncmp(ri->relname, rp.relname, NAMEDATALEN) == 0))
				{
					break;
				}

				dle = DLGetSucc(dle);
			}

			/* FIXME: indentation */

				if (!dle)
				{
					/*
					 * FIXME: better error handling
					 *
					 * We want to continue with recovery here, even though
					 * this really shouldn't happen and there's probably not
					 * much chance of getting the relation recovered completely
					 * and correctly.
					 *
					 * However, aborting with FATAL would kill the coordinator
					 * and make all other relations useless as well. Maybe
					 * some limited retrying?
					 */
					elog(WARNING, "Coordinator: recovery lane completed for "
						 "relation %s.%s (round %d, lane %d of %d), but "
						 "there's no such request pending.",
						 ri->schemaname, ri->relname, ri->round_no,
						 ri->curr_lane, ri->num_lanes);
				}
				else
				{
#ifdef COORDINATOR_DEBUG
					elog(DEBUG3, "Coordinator: recovery completed for relation "
						 "%s.%s (round %d, lane %d of %d).",
						 rp.schemaname, rp.relname, rp.round_no,
						 rp.curr_lane, rp.num_lanes);
#endif

					Assert(ri->state == RLS_STREAMING);

					free_recovery_request_info(ri);
					DLRemove(dle);

					group->rstate->open_streams--;
					group->rstate->total_requests--;

					/* check if recovery is completed */
					coordinator_check_recovery_streams(group);
				}
}

void
handle_imessage_schema_adaption(gcs_group *group, IMessage *msg)
{
	Assert(group);

#ifdef COORDINATOR_DEBUG
	elog(DEBUG2, "Coordinator: schema recovery for %d completed.",
		 group->dboid);
#endif

	IMessageRemove(msg);

	/* release the schema recovery backend */
	group->rstate->recovery_backend_id = InvalidBackendId;

	if (group->rstate->total_requests > 0)
	{
		Assert(group->dboid != TemplateDbOid);

		/*
		 * Start data recovery, per initiated requests from the
		 * schema adaption step.
		 */
		coordinator_state_change(group, RDBS_RECOVERING_DATA);

#ifdef COORDINATOR_DEBUG
		elog(DEBUG2, "Coordinator: starting data recovery for database %d",
			 group->dboid);
#endif

		coordinator_fire_recovery_requests(group);
	}
	else
	{
		database_recovery_completed(group);
	}
}

CoTransactionInfo *
get_co_transaction_info(NodeId origin_node_id, TransactionId origin_xid)
{
	CoTransactionInfo key;
	CoTransactionInfo *xi;
	bool found;

	key.origin_node_id = origin_node_id;
	key.origin_xid = origin_xid;
	xi = hash_search(co_txn_info, &key, HASH_ENTER, &found);

	if (!found)
	{
		xi->local_backend_id = InvalidBackendId;
		xi->local_xid = InvalidTransactionId;
		xi->local_coid = InvalidCommitOrderId;
		xi->deliverable_cset_no = 0;
		xi->total_csets = -1;
		xi->aborted = false;
	}

	return xi;
}

recovery_request_info *
get_recovery_stream_info(gcs_group *group, int round_no, int lane_no,	
						 int num_lanes, const char *schemaname,
						 const char *relname)
{
	Dlelem *curr_req;
	recovery_request_info *ri = NULL;

	curr_req = NULL;
	if (group->rstate)
		curr_req = DLGetHead(&group->rstate->request_list);

	while (curr_req)
	{
		ri = DLE_VAL(curr_req);
		
		if ((ri->round_no == round_no)
			&& (ri->curr_lane == lane_no)
			&& (ri->num_lanes == num_lanes)
			&& (strncmp(ri->schemaname, schemaname, NAMEDATALEN) == 0)
			&& (strncmp(ri->relname, relname, NAMEDATALEN) == 0))
			break;
		
		curr_req = DLGetSucc(curr_req);
	}
	
	return (curr_req ? DLE_VAL(curr_req) : NULL);
}

/*
 * Turns a message received from the GCS into an IMessage to be sent
 * to a background worker.
 */ 
static IMessage*
convert_to_imessage(IMessageType msg_type, group_node *sender,
					char *data, int size)
{
	IMessage *msg;
	buffer    b;

	msg = IMessageCreate(msg_type, size + 4);
	IMessageGetWriteBuffer(&b, msg);
	put_int32(&b, sender->id);
	put_data(&b, data, size);
	return msg;
}

void
coordinator_handle_gc_message(gcs_group *group, group_node *sender_node,
							  const char type, buffer *b)
{
	NodeId              origin_node_id;
	TransactionId		origin_xid;
	IMessage           *msg;
	IMessageType		msg_type;
	char               *start_of_msg;
	int                 msg_size;
	co_database        *codb = NULL;
	CoTransactionInfo  *xi;
#ifdef COORDINATOR_DEBUG
	char                node_desc[255];
#endif

	/* read the message type and.. */
	msg_type = get_int8(b);

	/* ..skip it for construction of an IMessage */
	msg_size = b->fill_size - b->ptr;
	start_of_msg = (char*) b->data + b->ptr;

#ifdef COORDINATOR_DEBUG
	group->gcsi->funcs.get_node_desc(group, sender_node, node_desc);
    elog(DEBUG1,
		 "Coordinator: received message %s\n"
		 "             for database %d\n"
		 "             from node %s of size %d",
	 	 decode_imessage_type(msg_type), group->dboid,
		 node_desc, msg_size + 1);
#endif

	if (msg_type == IMSGT_DB_STATE)
	{
		/*
		 * FIXME: somehow make sure these DB_STATES are ordered, i.e.
		 *        mark the DB_STATE with a view seq number or some such
		 *        and discard old DB_STATEs.
		 */
		rdb_state new_state = get_int8(b);

#ifdef COORDINATOR_DEBUG
		group->gcsi->funcs.get_node_desc(group, sender_node, node_desc);
		elog(DEBUG1,
			 "Coordinator: node %s changed from %s to %s",
			 node_desc, decode_database_state(sender_node->state),
			 decode_database_state(new_state));
#endif

		if (new_state != sender_node->state &&
			!group->gcsi->funcs.is_local(group, sender_node))
		{
			sender_node->state = new_state;
			coordinator_check_group_state(group);
		}
	}
	else if ((msg_type == IMSGT_RECOVERY_REQUEST) ||
			 (msg_type == IMSGT_RECOVERY_DATA))
	{
		int		round_no, curr_lane, num_lanes,
				chunk_no = 0,
				num_key_attrs = 0;
		char   *schemaname,
			   *relname;
		recovery_request_info *rsi;

		round_no = get_int8(b);
		curr_lane = get_int8(b);
		num_lanes = get_int8(b);
		schemaname = get_pstring(b);
		relname = get_pstring(b);

		if (msg_type == IMSGT_RECOVERY_REQUEST)
			num_key_attrs = get_int8(b);
		else
			chunk_no = get_int32(b);

#ifdef COORDINATOR_DEBUG
		if (msg_type == IMSGT_RECOVERY_REQUEST)
			elog(DEBUG5, "Coordinator: request for recovery of relation %s.%s "
				 "(round %d, lane %d of %d, key attrs %d)",
				 schemaname, relname, round_no, curr_lane, num_lanes,
				 num_key_attrs);
		else
			elog(DEBUG5, "Coordinator: data for recovery of relation %s.%s "
				 "(round %d, lane %d of %d, chunk no %d)",
				 schemaname, relname, round_no, curr_lane, num_lanes,
				 chunk_no);
#endif

		if (msg_type == IMSGT_RECOVERY_DATA)
		{
			rsi = get_recovery_stream_info(group, round_no, curr_lane,
										   num_lanes, schemaname, relname);
			if (!rsi)
			{
				/* FIXME: same error handling issue as above */
				elog(WARNING, "Coordinator: recovery data for unknown request "
					 "(db %d, relation: %s.%s.", group->dboid, schemaname,
					 relname);

				pfree(relname);
				pfree(schemaname);
				return;
			}
			else
			{
				Assert(rsi->state == RLS_REQUEST_FIRED ||
					   rsi->state == RLS_STREAMING);
				rsi->state = RLS_STREAMING;
			}
		}

		pfree(relname);
		pfree(schemaname);

		msg = convert_to_imessage(msg_type, sender_node,
								  start_of_msg, msg_size);

#if 0
        /*
         * FIXME: the GCS API should definitely be rethought.
         * let b point to the end of the message, just so the GCS doesn't
         * complain
         */
        b->ptr = start_of_msg + msg_size - (char*) b->data;
#endif

		LWLockAcquire(CoordinatorDatabasesLock, LW_SHARED);
		codb = hash_search(co_databases, &group->dboid, HASH_FIND, NULL);
		Assert(codb);
		if (chunk_no == 0 || msg_type == IMSGT_RECOVERY_REQUEST)
			dispatch_job(msg, codb);
		else
			dispatch_ooo_msg(msg, codb);
		LWLockRelease(CoordinatorDatabasesLock);
	}
	else if (msg_type == IMSGT_SEQ_INCREMENT || msg_type == IMSGT_SEQ_SETVAL)
	{
		char *seqname;

		/* get the origin xid */
		origin_xid = get_int32(b);

		seqname = get_pstring(b);

		/* turn the gc message into an imessage */
		msg = convert_to_imessage(msg_type, sender_node,
								  start_of_msg, msg_size);

		if (group->db_state == RDBS_OPERATING)
		{
#ifdef COORDINATOR_DEBUG
			if (msg_type == IMSGT_SEQ_INCREMENT)
				elog(DEBUG1, "Coordinator: requested increment of sequence %s",
					 seqname);
			else
				elog(DEBUG1, "Coordiantor: requested setval on sequence %s",
					 seqname);
#endif

			if (group->gcsi->funcs.is_local(group, sender_node))
			{
				/* A local transaction must have a backend associated. */
				BackendId backend_id = BackendXidGetBackendId(origin_xid);

				/*
				 * A local transaction must have a backend associated.
				 * Otherwise we must assume the backend has crashed. We
				 * should forward the changeset to a helper backend in that
				 * case, because all other nodes are already applying it...
				 */
				if (backend_id == InvalidBackendId)
				{
					elog(ERROR, "Coordinator: Unable to handle crashed backend.");

					/* cleanup the imessage again */
					IMessageRemove(msg);

					Assert(false);
				}

				IMessageActivate(msg, backend_id);
			}
			else
			{
				LWLockAcquire(CoordinatorDatabasesLock, LW_SHARED);
				codb = hash_search(co_databases, &group->dboid,
								   HASH_FIND, NULL);
				Assert(codb);
				dispatch_job(msg, codb);
				LWLockRelease(CoordinatorDatabasesLock);
			}
		}
		else if ((group->db_state == RDBS_RECOVERING_DATA) ||
				 (group->db_state == RDBS_RECOVERING_SCHEMA))
		{
			/*
			 * We have received a seq increment from a remote peer. Since
			 * we are still in recovery status, we have to cache the
			 * seq increment request.
			 */
			LWLockAcquire(CoordinatorDatabasesLock, LW_SHARED);
			codb = hash_search(co_databases, &group->dboid, HASH_FIND, NULL);
			Assert(codb);
			dispatch_job(msg, codb);
			LWLockRelease(CoordinatorDatabasesLock);
		}
		else
			Assert(false);

		pfree(seqname);
	}
	else if (msg_type == IMSGT_ORDERING)
	{
		CommitOrderId coid;
		int count_csets;

		origin_node_id = sender_node->id;

		/* get the local xid */
		origin_xid = get_int32(b);

		/* assign a local commit order id */
		coid = group->coid;
		CommitOrderIdAdvance(group->coid);
		store_transaction_coid(sender_node->id, origin_xid, coid);

		count_csets = get_int32(b);

		/* turn the gc message into an imessage */
		msg = convert_to_imessage(msg_type, sender_node,
								  start_of_msg, msg_size);

		if (group->db_state == RDBS_OPERATING)
		{
		    elog(DEBUG1, "Coordinator:     commit for database %d, count csets: %d, coid: %d",
				 group->dboid, count_csets, coid);

			/*
			 * Store the assigned coid in the transaction's info.
			 */
			xi = get_co_transaction_info(origin_node_id, origin_xid);
			xi->local_coid = coid;
			xi->total_csets = count_csets;

			/*
			 * If this transaction is stil running, dispatch the change set
			 * to a background worker process.
			 */
			if (!xi->aborted)
			{
#ifdef COORDINATOR_DEBUG
				elog(DEBUG1, "Coordinator:     dispatching as a job");
#endif

				LWLockAcquire(CoordinatorDatabasesLock, LW_SHARED);
				codb = hash_search(co_databases, &group->dboid,
								   HASH_FIND, NULL);
				Assert(codb);
				dispatch_ooo_msg(msg, codb);
				LWLockRelease(CoordinatorDatabasesLock);
			}
			else
			{
#ifdef COORDINATOR_DEBUG
				elog(DEBUG1, "Coordinator:     node %d / xid %d: commit req for database %d ignored, transaction already aborted",
					 sender_node->id, origin_xid, group->dboid);
#endif
			}
		}
		else if ((group->db_state == RDBS_RECOVERING_SCHEMA) ||
				 // FIXME: later, we should process changesets as soon
				 //        as we are in data recovery. See recovery.c
				 (group->db_state == RDBS_RECOVERING_DATA))
		{
			/*
			 * We have received a changeset from a remote peer. Since
			 * we are still in recovery status, we have to cache the
			 * changeset.
			 */
			LWLockAcquire(CoordinatorDatabasesLock, LW_SHARED);
			codb = hash_search(co_databases, &group->dboid, HASH_FIND, NULL);
			Assert(codb);
			dispatch_ooo_msg(msg, codb);
			LWLockRelease(CoordinatorDatabasesLock);
		}
		else
			Assert(false);
	}
	else if (msg_type == IMSGT_TXN_ABORTED)
	{
		Assert(group);
		Assert(group->gcsi);

		get_int32(b);	/* skip the additional node id */
		origin_xid = get_int32(b);
		get_int32(b); /* skip the errcode */

#ifdef COORDINATOR_DEBUG
		elog(DEBUG5, "Coordinator: transaction %d/%d needs to be aborted",
			 sender_node->id, origin_xid);
#endif

		xi = get_co_transaction_info(sender_node->id, origin_xid);
		xi->aborted = true;

		/*
		 * Drop all pending local ooo messages for this transaction.
		 */
		LWLockAcquire(CoordinatorDatabasesLock, LW_SHARED);
		codb = hash_search(co_databases, &group->dboid, HASH_FIND, NULL);
		Assert(codb);
		if (xi->local_backend_id != InvalidBackendId)
			drop_ooo_msgs_for(codb, xi->local_backend_id);
		LWLockRelease(CoordinatorDatabasesLock);

		/* turn the gc message into an imessage */
		msg = convert_to_imessage(msg_type, sender_node,
								  start_of_msg, msg_size);

		dispatch_ooo_msg(msg, codb);
	}
	else if (msg_type == IMSGT_CSET)
	{
		NodeId snap_node;
		TransactionId snap_xid;
		int cset_no;

		origin_node_id = sender_node->id;

		/* get the origin xid */
		origin_xid = get_int32(b);

		/* get the incremental change set number */
		cset_no = get_int32(b);

		/*
		 * Simply ignore the change sets we sent out ourselves. Ideally, the
		 * GCS wouldn't even send it back to the same node - as these
		 * change sets don't need to be ordered atomically. However, not all
		 * GCS support that.
		 */
		if (group->gcsi->funcs.is_local(group, sender_node))
		{
#ifdef COORDINATOR_DEBUG
		    elog(DEBUG1, "Coordinator: ignoring IMSGT_CSET from local node.");
#endif
			return;
		}

		snap_node = get_int32(b);
		snap_xid = get_int32(b);

		/* turn the gc message into an imessage */
		msg = convert_to_imessage(msg_type, sender_node,
								  start_of_msg, msg_size);

		xi = get_co_transaction_info(origin_node_id, origin_xid);

		if (xi->aborted)
		{
		    elog(DEBUG1, "Coordinator:     node %d / xid %d: changeset %d for database %d ignored, transaction already aborted",
				 sender_node->id, origin_xid, cset_no, group->dboid);
		}
		else if (group->db_state == RDBS_OPERATING)
		{
		    elog(DEBUG1, "Coordinator:     changeset %d for database %d",
				 cset_no, group->dboid);
			elog(DEBUG1, "Coordinator:     based on snapshot: %d/%d",
				 snap_node, snap_xid);

			LWLockAcquire(CoordinatorDatabasesLock, LW_SHARED);
			codb = hash_search(co_databases, &group->dboid,
							   HASH_FIND, NULL);
			Assert(codb);
			if (cset_no == 0)
				dispatch_job(msg, codb);
			else
				dispatch_ooo_msg(msg, codb);

			LWLockRelease(CoordinatorDatabasesLock);
		}
		else if ((group->db_state == RDBS_RECOVERING_SCHEMA) ||
				 // FIXME: later, we should process changesets as soon
				 //        as we are in data recovery. See recovery.c
				 (group->db_state == RDBS_RECOVERING_DATA))
		{
			/*
			 * We have received a changeset from a remote peer. Since
			 * we are still in recovery status, we have to cache the
			 * changeset.
			 */
			LWLockAcquire(CoordinatorDatabasesLock, LW_SHARED);
			codb = hash_search(co_databases, &group->dboid, HASH_FIND, NULL);
			Assert(codb);
			dispatch_ooo_msg(msg, codb);
			LWLockRelease(CoordinatorDatabasesLock);
		}
		else
			Assert(false);

	}
#ifdef COORDINATOR_DEBUG
	else
	{
		elog(WARNING, "Invalid message from the group communication "
			 "system. (%c)", msg_type);
	}
#endif
}

/*
 *	coordinator_replication_init
 */
void
coordinator_replication_init(void)
{
	HASHCTL hash_ctl;

	/*
	 * Install exit handler that disconnects nicely from the GCS.
	 */
	on_proc_exit(coordinator_cleanup, 0);

	/*
	 * Initialize GCS connection information.
	 */
	gcsi = palloc0(sizeof(gcs_info));
	gcsi->connection_tries = 0;
	gcsi->conn_state = GCSCS_DOWN;
	/* init the data to be NULL, since we need it in spread_init */
	gcsi->data = NULL;


	/* initialize the coordinator's private transaction info hash */
	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(NodeId) + sizeof(TransactionId);
	hash_ctl.entrysize = sizeof(CoTransactionInfo);
	hash_ctl.hash = tag_hash;
	co_txn_info = hash_create("Coordinator: txn info",
							  100, &hash_ctl, HASH_ELEM | HASH_FUNCTION);

	set_ps_display("disconnected", false);

	if (replication_enabled)
	{
//#if 0
		if ((pg_strncasecmp(replication_gcs, "ensemble", 8) == 0) &&
				(replication_gcs[8] == ':' || replication_gcs[8] == '\0'))
			ens_init(gcsi, gc_parse_params(&replication_gcs[8]));
		else if (pg_strncasecmp(replication_gcs, "spread", 6) == 0 &&
		         (replication_gcs[6] == ':' || replication_gcs[6] == '\0'))
			spread_init(gcsi, gc_parse_params(&replication_gcs[6]));
//#endif
		else if (pg_strncasecmp(replication_gcs, "egcs", 4) == 0 &&
				(replication_gcs[4] == ':' || replication_gcs[4] == '\0'))
			egcs_init(gcsi, gc_parse_params(&replication_gcs[4]));
		else
		{
			/*
			 * FIXME: postmaster itself should throw that error, no? What's the
			 *        GUC configured to?
			 */ 
			elog(ERROR, "Unknonw GCS specified.");
		}

#ifdef COORDINATOR_DEBUG
		elog(DEBUG1, "Connecting to GCS: '%s'", replication_gcs);
#endif

		coordinator_handle_gcs_connections();
	}
}

void
coordinator_replication_reg_gcs(fd_set *socks, int *max_sock_id)
{
	if (gcsi && gcsi->funcs.set_socks)
		gcsi->funcs.set_socks(gcsi, socks, max_sock_id);
}

void
coordinator_replication_check_sockets(fd_set *socks)
{
	if (gcsi && gcsi->funcs.handle_message)
		gcsi->funcs.handle_message(gcsi, socks);
}

/*
 *    gcsi_gcs_ready
 *
 * called by the gc layer as soon as the group communication connection
 * is ready.
 */
void
gcsi_gcs_ready(const gcs_info *gcsi)
{
#ifdef COORDINATOR_DEBUG
	elog(DEBUG1, "Coordinator: connected to the communication system.");
#endif

	Assert(replication_enabled);
	Assert(gcsi->conn_state == GCSCS_ESTABLISHED);

	/*
	 * Join the main group to find out if there are enough nodes back online
	 * to be able to reach a consensus on an initialization state to start
	 * from.
	 */
	coordinator_join_main_group();
}

static void
coordinator_join_database_groups()
{
	HASH_SEQ_STATUS		hash_status;
	co_database        *codb;

	Assert(gcsi->conn_state == GCSCS_ESTABLISHED);
	Assert(replication_group->db_state == RDBS_OPERATING);

	LWLockAcquire(CoordinatorDatabasesLock, LW_SHARED);

	hash_seq_init(&hash_status, co_databases);
	while ((codb = (co_database*) hash_seq_search(&hash_status)))
	{
		/* skip template0 and template1 databases */
		if (codb->codb_is_template0 || codb->codb_dboid == TemplateDbOid)
			continue;
		
		coordinator_join_group_for(codb);
	}

	LWLockRelease(CoordinatorDatabasesLock);
}

void
coordinator_check_seed_state()
{
	Assert(replication_group);

	if (replication_may_seed && !seed_mode)
	{
		/*
		 * Check if we can enter seed mode
		 */
		if (replication_group->db_state == RDBS_OPERATING)
		{
			/* FIXME: more prominent error reporting would be good. */
			elog(WARNING, "Coordinator: already operating, seeding not "
				 "possible. Please revert replication_may_seed to false!");
		}
		else if (replication_group->num_nodes_operating > 0)
		{
			/* FIXME: more prominent error reporting would be good. */
			elog(WARNING, "Coordinator: other nodes are already operating, "
				 "seeding not possible.");
		}
		else if ((replication_group->last_vc_join_pending &&
				  replication_group->db_state == RDBS_JOIN_REQUESTED) ||
				 replication_group->db_state == RDBS_RECOVERING_SCHEMA)
		{
			/* Entering seed mode */
#ifdef COORDINATOR_DEBUG
			elog(DEBUG1, "Coordinator: starting to seed other nodes.");
#endif

			seed_mode = true;

			/* we handled the join, so reset that flag */
			replication_group->last_vc_join_pending = false;

			coordinator_state_change(replication_group, RDBS_OPERATING);
			coordinator_join_database_groups();
		}
		else
			Assert(false);
	}
}

void
gcsi_gcs_failed(const gcs_info *gcsi)
{
	/*
	 * FIXME: should retry with increasing intervals
	 */
	if (gcsi->connection_tries >= 5)
	{
		elog(WARNING, "Unable to connect to the GCS.");
		
		/* FIXME: maybe notify concurrent seeding procedures? */
		elog(ERROR, "Failing GCS connections are not handled "
			 "properly, yet.");
	}
}

void
gcsi_viewchange_start(gcs_group *group)
{
	/*
	 * TODO: maybe block worker as well as helper backends from sending
     *       changesets to this group during the viewchange?
	 */

#ifdef COORDINATOR_DEBUG
	elog(DEBUG3, "Coordinator: start viewchange");
#endif

	/*
	 * FIMXE: stop all concurrent recovery action.
	 */

	/* reset viewchange counters */
	group->last_vc_join_pending = false;
	group->last_vc_leave_pending = false;
	group->last_vc_remotes_joined = 0;
	group->last_vc_remotes_left = 0;
}

void
gcsi_viewchange_stop(gcs_group *group)
{
	group_node			   *node;
	bool                    send_db_state;

#ifdef COORDINATOR_DEBUG
	elog(DEBUG3, "Coordinator: stop viewchange (now %ld nodes in the group, "
		 "former state: %s)", hash_get_num_entries(group->nodes),
		 decode_database_state(group->db_state));
#endif

	/*
	 * Check if the local node is still part of the group.
	 */
	node = gcsi_get_node(group, group->node_id_self_ref);
	if (!node)
	{
		if (group->last_vc_leave_pending)
			elog(WARNING, "Coordinator: lost connection to the group.");
		else
			elog(DEBUG3, "Coordinator: got a view which does not "
				 "include the local node.");

		/*
		 * FIXME: maybe try to re-join?
		 */
		if (group->db_state != RDBS_UNKNOWN &&
			group->db_state != RDBS_JOIN_REQUESTED)
			coordinator_state_change(group, RDBS_UNKNOWN);

		return;
	}

	/*
	 * Send the state of this node the group, if at least one node
	 * joined compared to the former view of the group.
	 */
	send_db_state = (group->last_vc_remotes_joined > 0 ||
					 group->last_vc_join_pending);

	/*
	 * Check the group's state, possibly deferring acting until enough
	 * other nodes have sent their state.
	 */
	coordinator_check_group_state(group);

	if (send_db_state)
		coordinator_broadcast_node_state(group);
}

static void
coordinator_check_group_liveness(gcs_group *group)
{
	group_node		   *node,
	                   *local_node = NULL;
	HASH_SEQ_STATUS		hash_status;
#ifdef COORDINATOR_DEBUG
	char                node_desc[255];
#endif

	/*
	 * Check the group state, i.e. count the nodes which are in the group
	 * as seen from this node.
	 */
#ifdef COORDINATOR_DEBUG
	elog(DEBUG1, "Coordinator: checking group state (database %d):",
		group->dboid);
#endif

	group->num_nodes_operating = 0;
	hash_seq_init(&hash_status, group->nodes);
	while ((node = (group_node*) hash_seq_search(&hash_status)))
	{
#ifdef COORDINATOR_DEBUG
		bool is_local = group->gcsi->funcs.is_local(group, node);
		if (is_local)
			local_node = node;

		group->gcsi->funcs.get_node_desc(group, node, node_desc);
		elog(DEBUG1, "Coordinator:     %s node %s: %s",
			 (is_local ? "*" : " "), node_desc,
			 decode_database_state(node->state));
#endif

		if (node->state == RDBS_OPERATING)
			group->num_nodes_operating++;
	}
}

/*
 * coordinator_check_group_state()
 *
 * Called after every viewchange as well as after having received an
 * IMSGT_DB_STATE message from another node, iff the local node is part of
 * the view.
 */
void
coordinator_check_group_state(gcs_group *group)
{
	group_node         *node;
	rdb_state           new_db_state;

	bool                recovery_provider_available;

	node = gcsi_get_node(group, group->node_id_self_ref);
	if (!node)
	{
		elog(DEBUG5, "Coordinator: local node not part of the group.");
		return;
	}

	/* update the local node's state, before evaluating liveness */
	node->state = group->db_state;

	/* Check for group liveness */
	coordinator_check_group_liveness(group);

	if (group->dboid == TemplateDbOid)
		coordinator_check_seed_state();

	if (group->last_vc_join_pending)
	{
		if(group->dboid != TemplateDbOid)
			Assert(group->db_state == RDBS_JOIN_REQUESTED);

		if (group->dboid != TemplateDbOid)
			Assert(replication_group->db_state == RDBS_OPERATING);

		new_db_state = (seed_mode ? RDBS_OPERATING : RDBS_RECOVERING_SCHEMA);
		coordinator_state_change(group, new_db_state);

		/* mark the join event as handled */
		group->last_vc_join_pending = false;
	}

	/*
	 * If there's at least one node which can provide recovery data, we
	 * might want to (re)start recovery. However, we take care to only do
	 * that if we need to.
	 */
	recovery_provider_available = (group->num_nodes_operating > 0);

	switch (group->db_state)
	{
		case RDBS_UNKNOWN:
		case RDBS_JOIN_REQUESTED:
		case RDBS_UNABLE_TO_CONNECT:
			break;

		case RDBS_AWAITING_MAJORITY:
			break;

		case RDBS_RECOVERING_SCHEMA:
			if (recovery_provider_available && group->rstate == NULL)
				coordinator_restart_schema_recovery(group);

			break;

		case RDBS_RECOVERING_DATA:
			if (recovery_provider_available && group->rstate == NULL)
				coordinator_restart_data_recovery(group);
			break;

		case RDBS_OPERATING:
			break;
	}
}

static char *
gc_prepare_imsg(const gcs_group *group, const IMessage *msg, Offset offset,
				int *size)
{
	char *data;

	*size = 1 + msg->size - offset;
	data = palloc(*size);

	data[0] = msg->type;
	/* FIXME: this copying step shouldn't be necessary... */
	memcpy(&data[1], IMSG_DATA(msg) + offset, msg->size - offset);

	return data;
}

void
gc_broadcast_imsg(const gcs_group *group, const IMessage *msg, bool atomic)
{
	int size;
	char *data;

	data = gc_prepare_imsg(group, msg, 0, &size);
	group->gcsi->funcs.broadcast(group, data, size, atomic);
	pfree(data);
}

void
gc_unicast_imsg(const gcs_group *group, const IMessage *msg)
{
	char *data;
    group_node *node;
    int node_id, size;
    buffer b;
#ifdef COORDINATOR_DEBUG
	char node_desc[255];
#endif

	Assert(group);
	IMessageGetReadBuffer(&b, msg);
    node_id = get_int32(&b);
	node = hash_search(group->nodes, &node_id, HASH_FIND, NULL);
	Assert(node);

#ifdef COORDINATOR_DEBUG
	group->gcsi->funcs.get_node_desc(group, node, node_desc);
	elog(DEBUG5, "Coordinator: sending message of type %s to node %s",
		 decode_imessage_type(msg->type), node_desc);
#endif

	data = gc_prepare_imsg(group, msg, 4, &size);
	group->gcsi->funcs.unicast(group, node, data, size);
	pfree(data);
}

/*
 *    handle_imessage_cset
 */
void
handle_imessage_cset(gcs_group *group, IMessage *msg, TransactionId local_xid)
{
	CoTransactionInfo *xi;
	group_node *local_node;

	Assert(group);
	Assert(group->gcsi);

	local_node = group->gcsi->funcs.get_local_node(group);
	xi = get_co_transaction_info(local_node->id, local_xid);
	xi->local_backend_id = msg->sender;
	xi->local_xid = local_xid;

	gc_broadcast_imsg(group, msg, false);
	IMessageRemove(msg);
}

/*
 *    handle_imessage_ordering_request
 */
void
handle_imessage_ordering_request(gcs_group *group, IMessage *msg,
								 TransactionId local_xid)
{
	CoTransactionInfo *xi;
	group_node *local_node;

	Assert(group);
	Assert(group->gcsi);

#ifdef COORDINATOR_DEBUG
	elog(DEBUG5, "Coordinator: ordering request for local transaction %d",
		 local_xid);
#endif

	local_node = group->gcsi->funcs.get_local_node(group);
	xi = get_co_transaction_info(local_node->id, local_xid);
	Assert(xi->local_backend_id == msg->sender);
	Assert(xi->local_xid == local_xid);

	gc_broadcast_imsg(group, msg, true);
	IMessageRemove(msg);
}

/*
 *    handle_imessage_txn_aborted
 */
void
handle_imessage_txn_aborted(gcs_group *group, IMessage *msg)
{
	CoTransactionInfo *xi;
	buffer b;
	co_database *codb;
	group_node *local_node;
	NodeId				origin_node_id;
	TransactionId		origin_xid;
	int errcode;

	Assert(group);
	Assert(group->gcsi);

	IMessageGetReadBuffer(&b, msg);
	origin_node_id = get_int32(&b);
	origin_xid = get_int32(&b);
	errcode = get_int32(&b);

#ifdef COORDINATOR_DEBUG
	elog(DEBUG5, "Coordinator: backend %d informs: txn (%d/%d) failed with errcode %d",
		 msg->sender, origin_node_id, origin_xid, errcode);

	if (errcode == ERRCODE_T_R_SERIALIZATION_FAILURE)
	{
		elog(DEBUG5, "Coordinator: (%d/%d) serialization failure.",
			 origin_node_id, origin_xid);
	}
#endif

	xi = get_co_transaction_info(origin_node_id, origin_xid);
	Assert(xi->local_backend_id == msg->sender);
	xi->aborted = true;

	/*
	 * For local transactions we need to multicast this abort message,
	 * so other nodes know they can release resources and locks
	 * already granted.
	 *
	 * Background workers applying remote transactions also send such
	 * a message in case their transaction got aborted due to a local
	 * conflict, before receiving the TXN_ABORTED message from the
	 * origin node.
	 */
	local_node = group->gcsi->funcs.get_local_node(group);
	if (xi->origin_node_id == local_node->id)
		gc_broadcast_imsg(group, msg, false);

	/*
	 * Drop all pending local ooo messages for this transaction.
	 */
	LWLockAcquire(CoordinatorDatabasesLock, LW_SHARED);
	codb = hash_search(co_databases, &group->dboid, HASH_FIND, NULL);
	Assert(codb);
	drop_ooo_msgs_for(codb, msg->sender);
	LWLockRelease(CoordinatorDatabasesLock);

	/*
	 * Return the TXN_ABORTED message to the backend, so it knows the
	 * coordinator has handled the abort and it can safely continue.
	 */
	IMessageActivate(msg, msg->sender);
}

void
handle_imessage_seq_increment(gcs_group *group, IMessage *msg)
{
	buffer				b;
	TransactionId		xid;

	Assert(group);
	Assert(group->gcsi);

	/*
	 * FIXME: Only one worker backend should be able to send an increment to
	 *        the group. If multiple backends concurrently request an
	 *        increment, we let the first one request an increment and
	 *        put the others on wait for the same request. Then, those can
	 *        most probably be served from the rcache.
	 *
	 *        Although, if seq.rcache_value == 1, there's not much point in
	 *        letting others wait.
	 */

	IMessageGetReadBuffer(&b, msg);

	/* get the local transaction id */
	xid = get_int32(&b);

#ifdef COORDINATOR_DEBUG
	elog(DEBUG5, "Coordinator:     sequence increment for local transaction %d",
		 xid);
    elog(DEBUG5, "Coordinator:     sending it out to the group");
#endif

	gc_broadcast_imsg(group, msg, true);
	IMessageRemove(msg);
}

void
handle_imessage_seq_setval(gcs_group *group, IMessage *msg)
{
	buffer				b;
	TransactionId		xid;

	Assert(group);
	Assert(group->gcsi);

	/*
	 * FIXME: Only one worker backend should be able to send an increment to
	 *        the group. If multiple backends concurrently request an
	 *        increment, we let the first one request an increment and
	 *        put the others on wait for the same request. Then, those can
	 *        most probably be served from the rcache.
	 *
	 *        Although, if seq.rcache_value == 1, there's not much point in
	 *        letting others wait.
	 */

	IMessageGetReadBuffer(&b, msg);

	/* get the local transaction id */
	xid = get_int32(&b);

#ifdef COORDINATOR_DEBUG
	elog(DEBUG5, "Coordinator:     sequence setval for local transaction %d",
		 xid);
	elog(DEBUG5, "Coordinator:     sending it out to the group");
#endif

	gc_broadcast_imsg(group, msg, true);
	IMessageRemove(msg);
}


/*
 * Takes care of opening the connection to the group communication system.
 *
 * FIXME: rename this function!
 */
void
coordinator_handle_gcs_connections(void)
{
	Assert(gcsi);
	if (gcsi->conn_state == GCSCS_DOWN)
	{
		/* FIXME: wait a certain interval, before a retry */

		/* connect to the gcs */
		gcsi->connection_tries++;
		gcsi->funcs.connect(gcsi);
	}
}


/*
 * called before a job is delivered to a worker backend
 */
void
coordinator_prepare_job_delivery(gcs_group *group, IMessage *msg,
								 BackendId recipient)
{
	NodeId              origin_node_id;
	TransactionId       origin_xid;
	CoTransactionInfo  *xi;
	buffer              b;
	int                 sender_id;
	int                 cset_no;
#ifdef COORDINATOR_DEBUG
    group_node         *sender_node = NULL;
	char                node_desc[255];
#endif

	if (msg->type != IMSGT_RECOVERY_RESTART &&
		msg->type != IMSGT_SCHEMA_ADAPTION)
	{
		IMessageGetReadBuffer(&b, msg);
		sender_id = get_int32(&b);
#ifdef COORDINATOR_DEBUG
        sender_node = gcsi_get_node(group, sender_id);
#endif
	}
	else
		sender_id = InvalidNodeId;

#ifdef COORDINATOR_DEBUG
    if (sender_node)
	    group->gcsi->funcs.get_node_desc(group, sender_node, node_desc);
    else
        strcpy(node_desc, "<invalid node>");

	elog(DEBUG1, "Coordinator: delivering msg %s of size %d from node %s "
		 "for database %d to backend %d",
		 decode_imessage_type(msg->type), msg->size, node_desc,
		 group->dboid, recipient);
#endif

	if (msg->type == IMSGT_ORDERING || msg->type == IMSGT_CSET)
	{
		/* read the origin node and xid */
		origin_node_id = sender_id;
		origin_xid = get_int32(&b);

		/*
		 * Store the assigned backend id, so future change sets or the commit
		 * request can locate the target backend.
		 */
		xi = get_co_transaction_info(origin_node_id, origin_xid);
		Assert(!xi->aborted);
		xi->local_backend_id = recipient;

		cset_no = get_int32(&b);
		if (msg->type == IMSGT_CSET)
		{
			Assert(cset_no == xi->deliverable_cset_no);
			xi->deliverable_cset_no++;
		}

#ifdef COORDINATOR_DEBUG
		elog(DEBUG1, "Coordinator: prepare_job_delivery: origin node id: %d, origin xid: %d, cset_no: %d",
			 origin_node_id, origin_xid, cset_no);
#endif
	}
	else if (msg->type == IMSGT_RECOVERY_RESTART ||
			 msg->type == IMSGT_SCHEMA_ADAPTION)
	{
		/*
		 * This is a node-internal message from the coordinator
		 * to the schema recovery backend. But it's important for the
		 * coordinator to remember the backend as our schema recovery
		 * helper.
		 */
		Assert(group->rstate);
		group->rstate->recovery_backend_id = recipient;
	}
	else if (msg->type == IMSGT_RECOVERY_REQUEST)
	{
	}
	else if (msg->type == IMSGT_RECOVERY_DATA)
	{
		int		round_no, curr_lane, num_lanes, chunk_no;
		char   *schemaname,
			   *relname;
		recovery_request_info *rsi;

		round_no = get_int8(&b);
		curr_lane = get_int8(&b);
		num_lanes = get_int8(&b);
		schemaname = get_pstring(&b);
		relname = get_pstring(&b);
		chunk_no = get_int32(&b);

		rsi = get_recovery_stream_info(group, round_no, curr_lane,
									   num_lanes, schemaname, relname);
		Assert(rsi);

		if (chunk_no == 0)
			rsi->stream_backend_id = recipient;

		rsi->chunk_no++;
	}
	else if (msg->type == IMSGT_SEQ_INCREMENT || msg->type == IMSGT_SEQ_SETVAL)
	{
	}
	else if (msg->type == IMSGT_TXN_ABORTED)
	{
	}
#ifdef COORDINATOR_DEBUG
	else
	{
		elog(WARNING,
			 "Coordinator: prepare_job_delivery: unknown message type: %s",
			 decode_imessage_type(msg->type));
	}
#endif
}

bool
coordinator_replication_can_deliver(IMessage *msg, co_database *codb,
									BackendId *target)
{
	rdb_state db_state;
	CoTransactionInfo *xi;
	NodeId origin_node_id;
	TransactionId origin_xid;
	group_node *sender_node;
	bool is_from_local_node;
	buffer b;
	int cset_no, chunk_no;

	int		round_no, curr_lane, num_lanes;
	char   *schemaname,
		   *relname;
	recovery_request_info *rsi;

	Assert(codb->group);
	db_state = codb->group->db_state;
	switch (msg->type)
	{
		case IMSGT_CSET:
		case IMSGT_ORDERING:
			IMessageGetReadBuffer(&b, msg);

			/* read the transaction origin info */
			origin_node_id = get_int32(&b);
			origin_xid = get_int32(&b);

			/* get the change set number */
			cset_no = get_int32(&b);

			xi = get_co_transaction_info(origin_node_id, origin_xid);
			if (xi->aborted)
			{
				elog(WARNING, "Coordinator: can_deliver: txn aborted, should have droped the message!");
				return false;
			}

#ifdef COORDINATOR_DEBUG
			elog(DEBUG3, "Coordinator: can_deliver: backend id: %d, origin node id: %d, origin xid: %d",
				 xi->local_backend_id, origin_node_id, origin_xid);
#endif

			if (xi->local_backend_id != InvalidBackendId)
				*target = xi->local_backend_id;
			else
				*target = InvalidBackendId;

			sender_node = codb->group->gcsi->funcs.get_local_node(codb->group);
			is_from_local_node = (sender_node->id == origin_node_id);

			if (msg->type == IMSGT_CSET && !is_from_local_node)
			{
				/*
				 * Prevent out-of-order delivery of change sets.
				 */
				if (cset_no != xi->deliverable_cset_no)
				{
#ifdef COORDINATOR_DEBUG
					elog(DEBUG3, "Coordinator: can_deliver returns FALSE: cset_no %d is currently not deliverable (%d)",
						 cset_no, xi->deliverable_cset_no);
#endif

					return false;
				}
			}

			if (msg->type == IMSGT_ORDERING && !is_from_local_node)
			{
				/*
				 * Prevent delivery of the commit request before having
				 * delivered all change sets for the transaction.
				 */
				if (xi->deliverable_cset_no < xi->total_csets)
				{
#ifdef COORDINATOR_DEBUG
					elog(DEBUG3, "Coordinator: can_deliver returns FALSE: total csets: %d, csets delivered: %d",
						 xi->total_csets, xi->deliverable_cset_no);
#endif

					return false;
				}
			}

			return db_state == RDBS_RECOVERING_DATA ||
				db_state == RDBS_OPERATING;

		case IMSGT_TXN_ABORTED:
			IMessageGetReadBuffer(&b, msg);

			/* read the transaction header info */
			origin_node_id = get_int32(&b);
			get_int32(&b);	/* skip the additional node id */
			origin_xid = get_int32(&b);

			xi = get_co_transaction_info(origin_node_id, origin_xid);

			if (xi->aborted)
			{
				elog(DEBUG5, "Coordinator: can_deliver: already aborted due to conflict");
				return false;
			}

			return true;

		case IMSGT_RECOVERY_DATA:
			if (db_state != RDBS_RECOVERING_SCHEMA &&
				db_state != RDBS_RECOVERING_DATA &&
				db_state != RDBS_OPERATING)
				return false;

			IMessageGetReadBuffer(&b, msg);

			get_int32(&b);		/* skip the origin_node_id */

			round_no = get_int8(&b);
			curr_lane = get_int8(&b);
			num_lanes = get_int8(&b);
			schemaname = get_pstring(&b);
			relname = get_pstring(&b);
			chunk_no = get_int32(&b);

			rsi = get_recovery_stream_info(codb->group, round_no, curr_lane,
										   num_lanes, schemaname, relname);

#ifdef COORDINATOR_DEBUG
			elog(DEBUG3, "Coordinator: checking delivery of recovery data for relation %s.%s (round %d, lane %d of %d, chunk %d (vs %d)).",
				 schemaname, relname, round_no, curr_lane, num_lanes,	
				 chunk_no, (rsi ? rsi->chunk_no : -1));
#endif

			/*
			 * FIXME: erronously sent recovery data gets queued
			 * infinitely that way.
			 */
			if (!rsi)
				return false;

			if (chunk_no > 0)
				*target = rsi->stream_backend_id;

			return rsi->chunk_no == chunk_no;

		case IMSGT_RECOVERY_RESTART:
			return db_state == RDBS_RECOVERING_SCHEMA ||
				db_state == RDBS_RECOVERING_DATA ||
				db_state == RDBS_OPERATING;

		case IMSGT_RECOVERY_REQUEST:
			return db_state == RDBS_OPERATING;

		case IMSGT_SCHEMA_ADAPTION:
		case IMSGT_SEQ_INCREMENT:
		case IMSGT_SEQ_SETVAL:
			return true;

		default:
			Assert(false);
			return true;
	}
}

static void
coordinator_send_db_state_to_backend(pid_t pid, BackendId id, rdb_state state)
{
	IMessage		   *msg;
	buffer				b;

	msg = IMessageCreate(IMSGT_DB_STATE, 1);
	IMessageGetWriteBuffer(&b, msg);
	put_int8(&b, state);
	IMessageActivate(msg, id);
}

static bool
coordinator_db_state_change_cb(int pid, BackendId id, void *data)
{
	gcs_group *group = (gcs_group*) data;
	coordinator_send_db_state_to_backend(pid, id, group->db_state);
	return false;
}

static void
coordinator_broadcast_node_state(gcs_group *group)
{
	buffer b;
	const int msg_size = 2;
	char msg[msg_size];

#ifdef COORDINATOR_DEBUG
	elog(DEBUG1, "broadcasting state %s for database %d on this node",
		 decode_database_state(group->db_state), group->dboid);
#endif

	init_buffer(&b, msg, msg_size);

	put_int8(&b, IMSGT_DB_STATE);
	put_int8(&b, group->db_state);

	group->gcsi->funcs.broadcast(group, msg, b.ptr, false);
}

/*
 *     cooldinator_state_change
 *
 * Changes the local node's state of a given database.
 */
void
coordinator_state_change(gcs_group *group, rdb_state new_state)
{
	co_database *codb;

#ifdef COORDINATOR_DEBUG
	elog(DEBUG5, "Coordinator: database %d: changing state to %s (from %s)",
		 group->dboid, decode_database_state(new_state),
		 decode_database_state(group->db_state));
#endif

	LWLockAcquire(CoordinatorDatabasesLock, LW_SHARED);
	codb = hash_search(co_databases, &group->dboid, HASH_FIND, NULL);
	Assert(codb);
	codb->state = new_state;
	group->db_state = new_state;
	LWLockRelease(CoordinatorDatabasesLock);

	if (group->dboid != TemplateDbOid)
	{
		/* send the new db state to all active helper backends */
		Assert(OidIsValid(group->dboid));
		ForeachDBBackend(group->dboid, &coordinator_db_state_change_cb, group);
	}

	/*
	 * If the cloud has just been seeded, or if recovery has just been
	 * finished, this node informs all others that it is ready to
	 * provide initialization data for other nodes.
	 */
	if (new_state == RDBS_OPERATING ||
		new_state == RDBS_RECOVERING_SCHEMA ||
		new_state == RDBS_RECOVERING_DATA)
		coordinator_broadcast_node_state(group);
}

group_node *
gcsi_add_node(gcs_group *group, uint32 new_node_id)
{
	group_node	   *node;
	bool			found;

	node = hash_search(group->nodes, &new_node_id,
					   HASH_ENTER, &found);
	Assert(!found);

	node->state = RDBS_UNKNOWN;
	node->gcs_node = NULL;
	return node;
}

group_node *
gcsi_get_node(const gcs_group *group, const uint32 node_id)
{
	group_node	   *node;

	node = hash_search(group->nodes, &node_id,
					   HASH_FIND, NULL);
	return node;
}

void
gcsi_remove_node(gcs_group *group, const group_node* node)
{
	bool	found;
	hash_search(group->nodes, &node->id, HASH_REMOVE, &found);
	Assert(found);
}

void
gcsi_node_changed(gcs_group *group, group_node *node,
				  const gcs_viewchange_type type)
{
	bool					is_local;

	is_local = group->gcsi->funcs.is_local(group, node);

#ifdef COORDINATOR_DEBUG
	Assert((type == GCVC_JOINED) || (type == GCVC_LEFT));
	elog(DEBUG1, "Coordinator: %s node has %s group %s.",
		 (is_local ? "local" : "remote"),
		 (type == GCVC_JOINED ? "joined" : "left"),
		 group->name);
#endif

	/* adjust counters as necessary */
	switch (type)
	{
		case GCVC_JOINED:
			if (is_local)
				group->last_vc_join_pending = true;
			else
				group->last_vc_remotes_joined++;
			break;

		case GCVC_LEFT:
			if (is_local)
				group->last_vc_leave_pending = true;
			else
				group->last_vc_remotes_left++;
			break;

		default:
			elog(ERROR, "Unknown node change type from GCS.");
	}
}
