/*-------------------------------------------------------------------------
 *
 * cset.h
 *
 *    Data structures and prototypes relating to changesets and
 *    changeset processing.
 *
 * Portions Copyright (c) 2010, Translattice, Inc
 * Portions Copyright (c) 2006-2010, PostgreSQL Global Development Group
 * 
 * Based on the work of Win Bausch and Bettina Kemme (ETH Zurich)
 * and some earlier efforts to integrate Postgres-R into
 * Postgres 7.2
 *
 *-------------------------------------------------------------------------
 */

#ifndef CSET_H
#define CSET_H

#include "lib/dllist.h"
#include "catalog/index.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "replication/replication.h"
#include "storage/imsg.h"
#include "storage/istream.h"
#include "storage/buffer.h"
#include "storage/itemptr.h"
#include "utils/tqual.h"
#include "utils/rel.h"

//#define CSET_SIZE_LIMIT 16320
#define CSET_SIZE_LIMIT 65472

typedef enum CsetCmdType
{
	CSCMD_UPDATE = 'u',				/* register update stmt */
	CSCMD_INSERT = 'i',				/* register insert stmt */
	CSCMD_DELETE = 'd',				/* register delete stmt */

	CSCMD_CLOSE_STMT = 'x',			/* unregister a statement */

	CSCMD_TUPLE = 't',				/* send tuple change for a statement */

	CSCMD_SUBTXN_START = 's',		/* start a sub transaction */
	CSCMD_SUBTXN_COMMIT = 'c',		/* commit a sub transaction */
	CSCMD_SUBTXN_ABORT = 'a',		/* abort (rollback) a subtransaction */

	CSCMD_EOS = 'e'					/* end of (change set) stream */
} CsetCmdType;


/*
 *    CsetStmtInfo
 *
 * Change sets contain informations for statements, which are then
 * referenced by their statement id within the change set. On the remote
 * side, this structure hold all the per-statement information required for
 * application of tuple changes.
 */
typedef struct CsetStmtInfo
{
	Dlelem			dle;

	int				stmt_id;		/* lookup id, as referenced by serialized
									 * the change set */

	CsetCmdType		type;			/* command type */

	MemoryContext	memCtx;			/* memory context for this statement */
	Oid				localRelOid;	/* local OID of the relation */
	Relation		base_rel;		/* the base relation, .. */
	ResultRelInfo *rr_info;			/* ..its result info.. */
	TupleDesc		tdesc;			/* ..and its tuple descriptor */

	IndexScanDesc	iscan;			/* scan desc on the primary key, reused
									 * during application of tuples. */
	
#if 0
/*
 * FIXME: defunct since the istream addition, should probably be revived.
 */
	char	   *changeArray;	/* contains 1 char for each attribute,
								 * indicates wheter the attr has been
								 * changed by the query or not.
								 * 'r' means changed, ' ' means unchanged.
								 */
#endif
} CsetStmtInfo;

/*
 * changeset structure
 */
typedef  struct
{
	MemoryContext	memCtx;
	IStreamWriter	writer;
	IStreamReader	reader;

	int				max_stmt_id;

	/* for remote application exclusively */
	Dllist			stmtlist;
	int				numStatements;
} ChangeSet;
typedef ChangeSet *ChangeSetPtr;

/* a global defined in globals.c */
extern ChangeSetPtr CurrentCset;
extern int csets_sent_counter;
extern int csets_recvd_counter;

extern void cset_init(void);
extern void cset_reset(void);
extern void cset_prepare_writer(void);
extern void cset_prepare_reader(IMessage *msg);

/* tuple serialization and deserialization */
extern void serialize_tuple_attr(IStreamWriter writer, TupleDesc tdesc,
								 Datum attr, int attnum);

extern void deserialize_tuple_attr(IStreamReader reader, TupleDesc tdesc,
								   Datum *attr, int attnum);

extern void serialize_tuple_pkey(IStreamWriter writer,
								 const IndexInfo *indeInfo,
								 TupleDesc tdesc, HeapTuple tuple);

extern void deserialize_tuple_pkey(IStreamReader reader, Relation index,
								   TupleDesc tdesc, Datum *values,
								   Datum *tupleoid);

extern void serialize_tuple_changes(IStreamWriter writer, TupleDesc tdesc,
									HeapTuple old, HeapTuple new,
									bool with_oid);

extern void deserialize_tuple_changes(IStreamReader reader, TupleDesc tdesc,
									  HeapTuple old, Datum *values,
									  bool *nulls);


/* in local.c */
extern void cset_open_statement(ResultRelInfo *rr_info, CmdType cmd);
extern void cset_close_statement(ResultRelInfo *rr_info);
extern void cset_add_subtxn_start(void);
extern void cset_add_subtxn_commit(void);
extern void cset_add_subtxn_abort(void);
extern void cset_collect_tuple(EState *estate, CmdType type,
							   ItemPointer origPtr, HeapTuple tuple);

/* in remote.c */
extern void cset_process(EState *estate);

/* FIXME: we sure don't want to import that one: */
extern void exec_simple_query(const char *query_string);

#endif   /* CSET_H */

