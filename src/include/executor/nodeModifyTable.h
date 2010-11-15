/*-------------------------------------------------------------------------
 *
 * nodeModifyTable.h
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEMODIFYTABLE_H
#define NODEMODIFYTABLE_H

#include "nodes/execnodes.h"

extern ModifyTableState *ExecInitModifyTable(ModifyTable *node, EState *estate, int eflags);
extern TupleTableSlot *ExecModifyTable(ModifyTableState *node);
extern void ExecEndModifyTable(ModifyTableState *node);
extern void ExecReScanModifyTable(ModifyTableState *node);

#ifdef REPLICATION
/* export for use in replication/recovery.c */
extern TupleTableSlot *ExecInsert(TupleTableSlot *slot,
								  TupleTableSlot *planSlot, EState *estate);
#endif

#endif   /* NODEMODIFYTABLE_H */
