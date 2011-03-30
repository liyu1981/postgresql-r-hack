/*-------------------------------------------------------------------------
 *
 * pg_replication.h
 *	  definition of the system "replication" relation (pg_replication)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL$
 *
 * NOTES
 *		the genbki.pl script reads this file and generates .bki
 *		information from the DATA() statements.
 *
 *		XXX do NOT break up DATA() statements into multiple lines!
 *			the scripts are not as smart as you might think...
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_REPLICATION_H
#define PG_REPLICATION_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_replication definition.  cpp turns this into
 *		typedef struct FormData_pg_replication
 * ----------------
 */
#define ReplicationRelationId	8888

CATALOG(pg_replication,8888)
{
	int4 reporiginnodeid;     /* origin_node_id */
	int4 reporiginxid;        /* origin_xid */
	int4 replocalxid;         /* local_xid */
	int4 replocalcoid;        /* local_coid */
} FormData_pg_replication;

/* ----------------
 *		Form_pg_replication corresponds to a pointer to a tuple with
 *		the format of pg_replication relation.
 * ----------------
 */
typedef FormData_pg_replication *Form_pg_replication;

/* ----------------
 *		compiler constants for pg_replication
 * ----------------
 */
#define Natts_pg_replication				4
#define Anum_pg_replication_reporiginnodeid	1
#define Anum_pg_replication_reporiginxid	2
#define Anum_pg_replication_replocalxid		3
#define Anum_pg_replication_replocalcoid	4

/* ----------------
 *		initial contents of pg_replication
 * ----------------
 */

/*
 * function prototypes
 */

#endif   /* PG_AM_H */
