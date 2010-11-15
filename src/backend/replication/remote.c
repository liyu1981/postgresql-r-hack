/*-------------------------------------------------------------------------
 *
 * remote.c
 *
 *	  Helper functions for replication helper backends - mostly for
 *    communication with the coordinator.
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
#include "catalog/namespace.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "postmaster/coordinator.h"
#include "replication/replication.h"
#include "replication/utils.h"
#include "storage/imsg.h"
#include "storage/buffer.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"


static void
cset_parse_header(buffer *b)
{
	int				cset_no;
	NodeId          origin_node_id;
	TransactionId   local_xid, origin_xid;

	Assert(get_bytes_read(b) > 12);

	/* read the transaction origin info */
	origin_node_id = get_int32(b);
	origin_xid = get_int32(b);

	cset_no = get_int32(b);
	Assert(cset_no == csets_recvd_counter);
	csets_recvd_counter++;

#ifdef DEBUG_CSET_APPL
	elog(DEBUG3, "bg worker [%d/%d]: applying change set no %d for (%d/%d)",
		 MyProcPid, MyBackendId, cset_no, origin_node_id, origin_xid);
#endif

	/*
	 * If this is the first change set received, get a new transaction id
	 * and store the mapping of the assigned local xid to the node_id/xid
	 * pair in shared memory.
	 */
	if (cset_no == 0)
	{
		local_xid = GetCurrentTransactionId();
		store_transaction_local_xid(origin_node_id, origin_xid, local_xid);
	}
}

void
cset_prepare_reader(IMessage *msg)
{
	Assert(CurrentCset);
	Assert(!CurrentCset->writer);

	CurrentCset->reader = MemoryContextAlloc(CurrentCset->memCtx,
											 sizeof(IStreamReaderData));
	istream_init_reader(CurrentCset->reader, &cset_parse_header, msg);
}

static CmdType
get_cmd_type_by_cscmd(CsetCmdType cscmd)
{
	Assert((cscmd == CSCMD_INSERT) ||
		   (cscmd == CSCMD_DELETE) ||
		   (cscmd == CSCMD_UPDATE));

	switch (cscmd)
	{
		case CSCMD_INSERT: return CMD_INSERT;
		case CSCMD_UPDATE: return CMD_UPDATE;
		case CSCMD_DELETE: return CMD_DELETE;

		/* ..and to keep the compiler happy */
		default:
			return CMD_UPDATE;
	}
}

static CsetStmtInfo *
find_statement(int stmt_id)
{
	CsetStmtInfo *csi;
	Dlelem *dle;

	dle = DLGetHead(&CurrentCset->stmtlist);
	while (dle)
	{
		csi = (CsetStmtInfo*) dle;

		if (csi->stmt_id == stmt_id)
			return csi;

		dle = DLGetSucc(dle);
	}

	return NULL;
}

static void
cset_parse_open_statement(CsetCmdType type)
{
	Oid				nspoid;
	CsetStmtInfo   *csi;
	char		   *nspname, *relname;
	MemoryContext	ctx, oldcontext;
	int				numAttrs;

	Assert(CurrentCset);
	Assert(CurrentCset->reader);
	Assert(IsBackgroundWorkerProcess());

	ctx = AllocSetContextCreate(CurrentCset->memCtx,
								"remote statement context",
								ALLOCSET_DEFAULT_MINSIZE,
								ALLOCSET_DEFAULT_INITSIZE,
								ALLOCSET_DEFAULT_MAXSIZE);

	oldcontext = MemoryContextSwitchTo(ctx);

	csi = (CsetStmtInfo*) palloc(sizeof(CsetStmtInfo));
	csi->memCtx = ctx;
	csi->type = type;
	nspname = istream_read_pstring(CurrentCset->reader);
	relname = istream_read_pstring(CurrentCset->reader);
	numAttrs = istream_read_int32(CurrentCset->reader);
	csi->stmt_id = istream_read_int32(CurrentCset->reader);
	csi->iscan = NULL;

	/* add the element to the list */
	DLInitElem(&csi->dle, csi);
	DLAddTail(&CurrentCset->stmtlist, &csi->dle);
	CurrentCset->numStatements++;

	/*
	 * Prepare for later tuple application
	 */
	nspoid = LookupNamespaceNoError(nspname);
	if (!OidIsValid(nspoid))
		elog(ERROR, "bg worker [%d/%d]: namespace '%s' does not exist",
			 MyProcPid, MyBackendId, nspname);

	csi->localRelOid = get_relname_relid(relname, nspoid);
	if(!OidIsValid(csi->localRelOid))
		elog(ERROR, "bg worker [%d/%d]: relation '%s' doesn't exist",
			 MyProcPid, MyBackendId, relname);


	/*
	 * Open the target relation
	 */
	csi->base_rel = heap_open(csi->localRelOid, RowExclusiveLock);
	Assert(csi->base_rel);
	Assert(csi->base_rel->rd_rel->relkind == RELKIND_RELATION);
	Assert(csi->base_rel->rd_rel->relhasindex);

	/*
	 * Init the result relation, which opens the required indices.
	 */
	csi->rr_info = (ResultRelInfo*) palloc(sizeof(ResultRelInfo));
	InitResultRelInfo(csi->rr_info,
					  csi->base_rel,
					  0,		/* dummy rangetable index */
					  get_cmd_type_by_cscmd(csi->type),
					  0);

	csi->tdesc = RelationGetDescr(csi->base_rel);
	Assert(csi->tdesc->natts == numAttrs);

	MemoryContextSwitchTo(oldcontext);
}

static void
cset_parse_close_statement(void)
{
	CsetStmtInfo   *csi;
	MemoryContext	ctx;
	int				stmt_id;

	stmt_id = istream_read_int32(CurrentCset->reader);
	csi = find_statement(stmt_id);
	Assert(csi);

	/* close the scan, all index relations and the base relation */
	if (csi->iscan)
		index_endscan(csi->iscan);

	/*
	 * Close the relation and the indices opened by InitResultRelInfo.
	 */
	ExecCloseIndices(csi->rr_info);
	heap_close(csi->base_rel, RowExclusiveLock);

	/*
	 * The CsetStmtInfo itself is allocated within the statement's
	 * MemoryContext, so we better remove the element before deleting that.
	 */
	ctx = csi->memCtx;
	DLRemove(&csi->dle);
	MemoryContextDelete(ctx);
}


/*
 *    cset_process
 *
 * Processes a change set from a remote transaction.
 */
void
cset_process(EState *estate)
{
	CsetCmdType		cmd_type;
	CsetStmtInfo   *csi;
	int				stmt_id;
	bool			terminated = false;
	IStreamReader	reader = CurrentCset->reader;

	Assert(IsBackgroundWorkerProcess());

	while (!MyProc->abortFlag && !terminated)
	{
		cmd_type = istream_read_int8(reader);
		elog(DEBUG5, "bg worker [%d/%d]: cset_process: cmd_type: %s",
			 MyProcPid, MyBackendId, decode_command_type(cmd_type));
		switch (cmd_type)
		{
			case CSCMD_TUPLE:
				stmt_id = istream_read_int32(reader);
				csi = find_statement(stmt_id);
				Assert(csi);
				ExecProcessTuple(reader, estate, csi);
				break;

			case CSCMD_INSERT:
			case CSCMD_UPDATE:
			case CSCMD_DELETE:
				cset_parse_open_statement(cmd_type);
				break;

			case CSCMD_CLOSE_STMT:
				cset_parse_close_statement();
				break;

			case CSCMD_SUBTXN_START:
				BeginInternalSubTransaction(NULL);
				break;
			
			case CSCMD_SUBTXN_COMMIT:
				ReleaseCurrentSubTransaction();
				break;

			case CSCMD_SUBTXN_ABORT:
				RollbackAndReleaseCurrentSubTransaction();
				break;

			case CSCMD_EOS:
				terminated = true;
				break;

			default:
				Assert(false);
		}
	}

	/*
	 * Free the change set after it has been applied. This ensures further
	 * change sets (if any) will start from a clean state.
	 */
	istream_close_reader(reader);
}
