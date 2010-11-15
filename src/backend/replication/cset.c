/*-------------------------------------------------------------------------
 *
 * cset.c
 *
 *	  Changeset processing and tuple serialization functions
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
#include "access/genam.h"
#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/transam.h"
#include "access/printtup.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"
#include "parser/parse_oper.h"
#include "replication/replication.h"
#include "replication/utils.h"
#include "storage/imsg.h"
#include "storage/buffer.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "storage/proc.h"
#include "parser/parser.h"


/* ----------------------------------------------------------------
 *						changeset handling
 * ----------------------------------------------------------------
 */

void
cset_init(void)
{
	Assert(!CurrentCset);
	Assert(MemoryContextIsValid(TopTransactionContext));

	CurrentCset = MemoryContextAlloc(TopTransactionContext, sizeof(ChangeSet));
	CurrentCset->memCtx = TopTransactionContext;

	CurrentCset->writer = NULL;
	CurrentCset->reader = NULL;

	CurrentCset->max_stmt_id = 1;

	DLInitList(&CurrentCset->stmtlist);
	CurrentCset->numStatements = 0;
}

void
cset_reset(void)
{
	CurrentCset = NULL;
	csets_sent_counter = 0;
	csets_recvd_counter = 0;
}


/* ----------------------------------------------------------------
 *						changeset handling
 * ----------------------------------------------------------------
 */

void
serialize_tuple_attr(IStreamWriter writer, TupleDesc tdesc, Datum attr,
					 int attnum)
{
	Oid		typoid, typoutput;
	bool	typisvarlena;
	char   *str;
	int		size;

	/*
	 * Special treatment for the oid attribute, which is used for some
	 * of the system catalog tables as primary key during recovery.
	 */
	if (attnum == -2)
		typoid = OIDOID;
	else
		typoid = (Oid) tdesc->attrs[attnum - 1]->atttypid;

	getTypeOutputInfo(typoid, &typoutput, &typisvarlena);
	str = OidOutputFunctionCall(typoutput, attr);

	size = strlen(str);

	istream_write_int32(writer, size);
	istream_write_data(writer, str, size);
}

void
deserialize_tuple_attr(IStreamReader reader, TupleDesc tdesc, Datum *attr, int attnum)
{
	Oid		typid,
            typinput,
	        typioparam;
	char   *str;
	int		size;

	size = istream_read_int32(reader);
	str = (char*) palloc(size + 1);
	istream_read_data(reader, str, size);
	str[size] = 0;
	Assert(strlen(str) == size);

	if (attnum > 0)
		typid = tdesc->attrs[attnum - 1]->atttypid;
	else
		typid = OIDOID;

   	getTypeInputInfo(typid, &typinput, &typioparam);
	*attr = OidInputFunctionCall(typinput, str, typioparam, -1);
}

void
serialize_tuple_pkey(IStreamWriter writer, const IndexInfo* indexInfo,
					 TupleDesc tdesc, HeapTuple tuple)
{
	Datum	attr;
	bool	isNull;
	int		i, attnum;

	for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
	{
		attnum = indexInfo->ii_KeyAttrNumbers[i];
		attr = heap_getattr(tuple, attnum, tdesc, &isNull);
		Assert(!isNull);
		serialize_tuple_attr(writer, tdesc, attr, attnum);
	}
}

/*
 * Deserializes primary only key attributes into values
 */
void
deserialize_tuple_pkey(IStreamReader reader, Relation index, TupleDesc tdesc,
					   Datum *values, Datum *tupleoid)
{
	int				i, attnum;

    for (i = 0; i < index->rd_index->indnatts; i++)
    {
	    attnum = index->rd_index->indkey.values[i];
		if (attnum >= 0)
		    deserialize_tuple_attr(reader, tdesc, &values[attnum - 1], attnum);
		else
		{
			Assert(tupleoid != NULL);
			deserialize_tuple_attr(reader, tdesc, tupleoid, attnum);
		}
    }
}

void
serialize_tuple_changes(IStreamWriter writer, TupleDesc tdesc, HeapTuple old,
						HeapTuple new, bool with_oid)
{
	int		i, attnum;
	bool	old_isNull, new_isNull;
	Datum	old_attr, new_attr;

	if (with_oid)
	{
		Assert(!old);		/* only used during recovery */
		attnum = -2;

		new_attr = heap_getattr(new, attnum, tdesc, &new_isNull);
		Assert(!new_isNull);

		istream_write_int8(writer, 's');
		serialize_tuple_attr(writer, tdesc, new_attr, attnum);
	}

	for (i = 0; i < tdesc->natts; i++)
	{
		attnum = i + 1;
		Assert(tdesc->attrs[i] != NULL);

		new_attr = heap_getattr(new, attnum, tdesc, &new_isNull);

		old_isNull = TRUE;
		old_attr = 0;

		if (old)
			old_attr = heap_getattr(new, attnum, tdesc, &old_isNull);

		if (new_isNull)
		{
			if (old && old_isNull)
				istream_write_int8(writer, 'u');	/* 'u'nchanged */
			else
				istream_write_int8(writer, 'r');	/* 'r'eset to NULL */
		}
		else
		{
			/* FIXME: should check, if the value has changed! */

			istream_write_int8(writer, 's');		/* 's'et to new value */

			serialize_tuple_attr(writer, tdesc, new_attr, attnum);
		}
	}
}

void
deserialize_tuple_changes(IStreamReader reader, TupleDesc tdesc,
						  HeapTuple old, Datum *values, bool *nulls)
{
	int		i;
	char	attr_state;

	for (i = 0; i < tdesc->natts; i++)
	{
		int attnum = i + 1;

		Assert(tdesc->attrs[i] != NULL);

		attr_state = istream_read_int8(reader);

		switch (attr_state)
		{
			case 'u':		/* unchanged, use the value from the old tuple */
			    if (old)
					values[i] = heap_getattr(old, attnum, tdesc, &nulls[i]);
				else
					nulls[i] = TRUE;

				break;

			case 'r':		/* reset */
				nulls[i] = TRUE;
				values[i] = 0;
				break;

			case 's':		/* set */
				nulls[i] = FALSE;
				deserialize_tuple_attr(reader, tdesc, &values[i], attnum);
				break;

			default:
				Assert(FALSE);
		}
	}
}
