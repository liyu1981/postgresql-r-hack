/*-------------------------------------------------------------------------
 *
 * local.c
 *
 *	  Helper functions for worker backends - mostly for communication
 *    with the coordinator for replication.
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
#include "access/xact.h"
#include "access/xlogutils.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_tablespace.h"
#include "lib/dllist.h"
#include "postmaster/autovacuum.h"
#include "postmaster/coordinator.h"
#include "replication/cset.h"
#include "replication/replication.h"
#include "replication/utils.h"
#include "storage/buffer.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"
#include "storage/imsg.h"
#include "storage/istream.h"
#include "storage/ipc.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

static CsetCmdType get_cset_cmd_type(CmdType cmd);

/* interface method for the istream API */
static void cset_write_header(buffer *b);


/*
 *		cset_write_header
 *
 * Prepends a header to the change set, just before writing the first
 * chunks of data to a change set message. Called from the istream
 * API.
 */
static void
cset_write_header(buffer *b)
{
	put_int32(b, GetCurrentTransactionId());
	put_int32(b, csets_sent_counter++);
}

void
cset_prepare_writer(void)
{
	Assert(CurrentCset);
	Assert(!CurrentCset->reader);

	CurrentCset->writer = MemoryContextAlloc(CurrentCset->memCtx,
											 sizeof(IStreamWriterData));
	istream_init_writer(CurrentCset->writer, IMSGT_CSET, GetCoordinatorId(),
						CSET_SIZE_LIMIT, &cset_write_header);
}

/*
 *    send_commit_ordering_request
 */
void
send_commit_ordering_request(void)
{
	int					size;
	IMessage		   *msg;
	buffer				b;

	size = 4 +  /* transaction id */
		   4;   /* number of change sets sent */

	msg = IMessageCreate(IMSGT_ORDERING, size);
	init_buffer(&b, IMSG_DATA(msg), size);

	/*
	 * Add our local transaction id and the total amount of change sets
	 * sent for this transaction.
	 */
	put_int32(&b, GetCurrentTransactionId());
	put_int32(&b, csets_sent_counter);

#ifdef DEBUG_CSET_COLL
	elog(DEBUG3, "send_commit_message: sending commit msg to coordinator.");
#endif

	IMessageActivate(msg, GetCoordinatorId());
}

/*
 * send_txn_aborted_msg
 */
void
send_txn_aborted_msg(TransactionId xid, int errcode)
{
	NodeId              origin_node_id;
	TransactionId       origin_xid;
	BackendId           coordinator_id;
	IMessage		   *msg;
	buffer              b;
	int                 size = 12;

	coordinator_id = GetCoordinatorId();
	if (coordinator_id != InvalidBackendId)
	{
		get_origin_by_local_xid(xid, &origin_node_id, &origin_xid);

		msg = IMessageCreate(IMSGT_TXN_ABORTED, size);
		init_buffer(&b, IMSG_DATA(msg), size);

		put_int32(&b, origin_node_id);
		put_int32(&b, origin_xid);
		put_int32(&b, errcode);

		IMessageActivate(msg, coordinator_id);


		/*
		 * Must wait for the coordinator to confirm aborted transaction.
		 */
		elog(DEBUG1, "bg worker [%d/%d]: waiting for abort confirmation",
			 MyProcPid, MyBackendId);
		msg = IMessageCheck();
		while (msg == NULL || msg->type != IMSGT_TXN_ABORTED)
		{
			if (msg)
				IMessageRemove(msg);

			elog(DEBUG1, "bg worker [%d/%d]: waiting for abort confirmation",
				 MyProcPid, MyBackendId);
			pg_usleep(50000L);

			msg = IMessageCheck();
		}

		elog(DEBUG1, "bg worker [%d/%d]: got abort confirmation",
			 MyProcPid, MyBackendId);

		/* eat the IMSGT_TXN_ABORTED */
		IMessageRemove(msg);
	}
	else
	{
		elog(WARNING, "send_txn_aborted_msg: Coordinator has gone?");
	}
}

IMessage*
await_imessage()
{
	IMessage   *msg;

	while (!(msg = IMessageCheck()))
	{
		ImmediateInterruptOK = true;
		CHECK_FOR_INTERRUPTS();

		pg_usleep(1000000L);

		ImmediateInterruptOK = false;

		if (MyProc->abortFlag)
			break;
	}

	return msg;
}

void
replication_request_sequence_increment(const char *seqname)
{
	IMessage	   *msg;
	buffer			b;
	TransactionId	xid;

    /*
	 * FIXME: do we really need to check this every time we try to send a
	 *        msg to the coordinator?
	 */
	if (GetCoordinatorId() == InvalidBackendId)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("Communication errror: no coordinator started")));

	msg = IMessageCreate(IMSGT_SEQ_INCREMENT, 5 + strlen(seqname));
	IMessageGetWriteBuffer(&b, msg);

	/*
	 * Add our local transaction id to identify this request.
	 */
	put_int32(&b, GetCurrentTransactionId());

	put_pstring(&b, seqname);

	IMessageActivate(msg, GetCoordinatorId());

	msg = await_imessage();
	IMessageGetReadBuffer(&b, msg);

	Assert(msg->type == IMSGT_SEQ_INCREMENT);
	b.ptr += sizeof(NodeId);
	xid = get_int32(&b);

	Assert(xid == GetCurrentTransactionId());

	IMessageRemove(msg);
}


void
replication_request_sequence_reset(const char *seqname, int64 new_value,
								   bool iscalled)
{
	IMessage	   *msg;
	buffer			b;
	TransactionId	xid;

    /*
	 * FIXME: do we really need to check this every time we try to send a
	 *        msg to the coordinator?
	 */
	if (GetCoordinatorId() == InvalidBackendId)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("Communication errror: no coordinator started")));

	msg = IMessageCreate(IMSGT_SEQ_SETVAL,
						 5 + strlen(seqname) + 9);
	IMessageGetWriteBuffer(&b, msg);

	/*
	 * Add our local transaction id to identify this request.
	 */
	put_int32(&b, GetCurrentTransactionId());

	put_pstring(&b, seqname);
	put_int64(&b, new_value);
	put_int8(&b, iscalled);

	IMessageActivate(msg, GetCoordinatorId());

	msg = await_imessage();
	IMessageGetReadBuffer(&b, msg);

	Assert(msg->type == IMSGT_SEQ_SETVAL);
	b.ptr += sizeof(NodeId);
	xid = get_int32(&b);

	Assert(xid == GetCurrentTransactionId());

	IMessageRemove(msg);
}

static CsetCmdType
get_cset_cmd_type(CmdType cmd)
{
	Assert((cmd == CMD_INSERT) ||
		   (cmd == CMD_DELETE) ||
		   (cmd == CMD_UPDATE));

	switch (cmd)
	{
		case CMD_INSERT: return CSCMD_INSERT;
		case CMD_UPDATE: return CSCMD_UPDATE;
		case CMD_DELETE: return CSCMD_DELETE;

		/* ..and to keep the compiler happy */
		default:
			return CSCMD_UPDATE;
	}
}

/*
 *    cset_open_statement
 *
 * Opens a new statement for which tuples will be serialized. Adds required
 * information like schema- and relation name as well as helpful additional
 * info like which attributes might have changed and which not.
 *
 * Assigs a statement id that's stored back to the ResultRelInfo for
 * referency by the cset_collect_tuple() method.
 */
void
cset_open_statement(ResultRelInfo *rr_info, CmdType cmd)
{
	TupleDesc			tdesc;
	Oid					nspoid;
	char			   *relname,
					   *nspname;
	IStreamWriter		writer;

#if 0
	AttrNumber			attnum;
	ListCell		   *tl;
#endif

	Assert(rr_info->ri_cset_stmt_id == 0);

	if (CurrentCset == NULL)
	{
		cset_init();
		cset_prepare_writer();
	}

	writer = CurrentCset->writer;

	tdesc = RelationGetDescr(rr_info->ri_RelationDesc);

	relname = RelationGetRelationName(rr_info->ri_RelationDesc);
	nspoid = RelationGetNamespace(rr_info->ri_RelationDesc);
	nspname = get_namespace_name(nspoid);
	if (!nspname)
		elog(ERROR, "cache lookup failed for namespace %u", nspoid);

	istream_write_int8(writer, get_cset_cmd_type(cmd));
	istream_write_pstring(writer, nspname);
	istream_write_pstring(writer, relname);
	istream_write_int32(writer, tdesc->natts);

	/*
	 * FIXME: the list of changed attrs has been defunct with the change to
	 * istream.
	 */
#if 0
	if (targetlist == NULL)
		return;

	foreach (tl, targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tl);

		if (!tle->resjunk)
		{
			attnum = tle->resno - 1;
			Assert(attnum >= 0);
			Assert(attnum < rr_info->tcoll->numAttrs);
			rr_info->tcoll->changeArray[attnum] = 'r';
		}
	}

	tcoll->numAttrs = tdesc->natts;
	tcoll->changeArray = (char *)
		MemoryContextAlloc(CurrentCset->memCtx,
						   tcoll->numAttrs * sizeof(char));
	MemSet(tcoll->changeArray, ' ', tcoll->numAttrs * sizeof(char));
#endif

	/*
	 * assign a statement id to be referenced from tuple changes. The
	 * remote change set applicator later on looks up the statements
	 * by this id.
	 */
	rr_info->ri_cset_stmt_id = CurrentCset->max_stmt_id++;
	istream_write_int32(writer, rr_info->ri_cset_stmt_id);
}

/*
 *    cset_close_statement
 *
 * Closes an open statement, identified by its id. The remote backend uses
 * this to close open indices and drop the id from the list of open
 * statements. No more tuples may be serialized for this statement after
 * closing it.
 */
void
cset_close_statement(ResultRelInfo *rr_info)
{
	Assert(CurrentCset);
	Assert(CurrentCset->writer);
	Assert(IsReplicatedBackend());

	Assert(rr_info->ri_cset_stmt_id > 0);

	istream_write_int8(CurrentCset->writer, CSCMD_CLOSE_STMT);
	istream_write_int32(CurrentCset->writer, rr_info->ri_cset_stmt_id);

	rr_info->ri_cset_stmt_id = 0;
}

void
cset_add_subtxn_start()
{
	istream_write_int8(CurrentCset->writer, CSCMD_SUBTXN_START);
}

void
cset_add_subtxn_abort()
{
	istream_write_int8(CurrentCset->writer, CSCMD_SUBTXN_ABORT);
}

void
cset_add_subtxn_commit()
{
	istream_write_int8(CurrentCset->writer, CSCMD_SUBTXN_COMMIT);
}


/*
 * replication_global_commit()
 *
 * Sends a commit request to the coordinator (and thus to the group
 * communication layer) and awaits an ordering agreement. Gets called from
 * the final CommitTransaction() call.
 */
void
replication_global_commit(void)
{
	IMessage	   *msg;
	buffer			b;
	NodeId			origin_node_id;
	TransactionId	origin_xid;

	Assert(replication_enabled);
	Assert(!IsBackgroundWorkerProcess());
	Assert(XactReplLevel != XACT_REPL_LAZY);

	/* FIXME: indentation */
	{
#ifdef DEBUG_CSET_COLL
		elog(DEBUG3, "cset_replicate:     waiting for ordering decision");
#endif

		send_commit_ordering_request();

		/*
		 * wait for our changeset to be delivered via the totally
		 * ordered socket.
		 */
		msg = await_imessage();

		if (!MyProc->abortFlag)
		{
			IMessageGetReadBuffer(&b, msg);
			if (msg->type == IMSGT_ORDERING)
			{
				origin_node_id = get_int32(&b);
				origin_xid = get_int32(&b);

				IMessageRemove(msg);

				/* store the local transaction id in shared memory */
				Assert(origin_xid == MyProc->xid);
				store_transaction_local_xid(origin_node_id, origin_xid, origin_xid);

				/* read the commit order id from shared memory */
				Assert(CommitOrderIdIsValid(
						   get_local_coid_by_origin(origin_node_id, origin_xid)));

				WaitUntilCommittable();

				if (MyProc->abortFlag)
					ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
									errmsg("could not serialize access due to "
										   "concurrent update")));
			}
			else
			{
				elog(ERROR, "Replication protocol error in %s:%d\n"
					 "got a message of type %s instead of a changeset.",
					 __FILE__, __LINE__, decode_imessage_type(msg->type));

				IMessageRemove(msg);
			}
		}
		else
		{
			if (msg)
				IMessageRemove(msg);

			ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				errmsg("could not serialize access due to "
					   "concurrent update")));

			/* reset the abort flag */
			MyProc->abortFlag = false;
		}
	}
}


/*
 * cset_collect_tuple
 *
 * adds a single tuple change to the serialized change set, is called from
 * execInsert(), execUpdate() and execDelete().
 *
 * params:
 * tid        tuple id of the old (unmodified) tuple, used to get primary
 *            key values.
 * tuple      the new, modified tuple
 * currQuery  pointer to the query info struct in the changeset to
 *            which the tuple has to be added to.
 */
void
cset_collect_tuple(EState *estate, CmdType type, ItemPointer tid,
				   HeapTuple tuple)
{
	ResultRelInfo	   *rr_info;
	Relation			rel;
	HeapTupleData		oldTuple;
	TupleDesc			tdesc;
	IndexInfo		   *idx_info;
	int 				natts;
	Buffer				oldTupleBuffer;
	NodeId				origin_node_id;
	TransactionId		xid, origin_xid;

	IStreamWriter		writer;

	Assert(!IsBackgroundWorkerProcess());
	Assert(replication_enabled);
	Assert(CurrentCset);

	writer = CurrentCset->writer;
	rr_info = estate->es_result_relation_info;
	Assert(rr_info->ri_cset_stmt_id > 0);

	/*
	 * Make sure there is a primary key for this relation, otherwise we
	 * cannot currently replicate it.
	 *
	 * FIXME: initialization and recovery should check for this already,
	 *         so we theoretically don't need to check here. However, there
	 *         is no such check, yet.
	 */
	if (!rr_info->ri_IndexRelationInfo || rr_info->ri_PrimaryKey < 0)
		elog(ERROR,
			 "cannot replicate tuple from relation without primary key");

	idx_info = rr_info->ri_IndexRelationInfo[rr_info->ri_PrimaryKey];
	rel = rr_info->ri_RelationDesc;

	tdesc = RelationGetDescr(rel);
	natts = RelationGetNumberOfAttributes(rel);

	istream_write_int8(writer, CSCMD_TUPLE);
	istream_write_int32(writer, rr_info->ri_cset_stmt_id);

	if (type == CMD_UPDATE || type == CMD_DELETE)
	{
		/*
		 * Fetch the old tuple, as we need to fetch the primary key from
		 * that one for updates and deletes.
		 */
		oldTuple.t_self = *tid;
		oldTuple.t_tableOid = RelationGetRelid(rel);

		if (!heap_fetch(rel, SnapshotAny,
						&oldTuple, &oldTupleBuffer, false, NULL))
			elog(ERROR, "failed to fetch deleted tuple for replication");

		/*
		 * Serialize the tuple's origin info, i.e. the global
		 * transaction id of the transaction that last inserted or
		 * updated the tuple (lookup by xmin).
		 */
		xid = HeapTupleHeaderGetXmin(oldTuple.t_data);
		get_origin_by_local_xid(xid, &origin_node_id, &origin_xid);

#ifdef DEBUG_CSET_COLL
		elog(DEBUG5, "cset_collect_tuple: xmin is: %d", xid);
		elog(DEBUG5, "cset_collect_tuple: origin node: %d origin xid: %d",
			 origin_node_id, origin_xid);
#endif

		istream_write_int32(writer, origin_node_id);
		istream_write_int32(writer, origin_xid);

		/*
		 * for UPDATE and DELETE we serialize the (former) tuple's primary key
		 * attributes.
		 */
		serialize_tuple_pkey(writer, idx_info, tdesc, &oldTuple);

		ReleaseBuffer(oldTupleBuffer);
	}

	/*
	 * For inserts and updates, we also need to store all changed or
	 * inserted attributes besides the primary key attributes.
	 */
	if (type == CMD_INSERT)
	{
#ifdef DEBUG_CSET_COLL
		elog(DEBUG3, "serializing tuple data attributes");
#endif

		serialize_tuple_changes(writer, tdesc, NULL, tuple, false);
	}
	else if (type == CMD_UPDATE)
	{
#ifdef DEBUG_CSET_COLL
		elog(DEBUG3, "serializing tuple data changes");
#endif

		serialize_tuple_changes(writer, tdesc, &oldTuple, tuple, false);
	}
}
