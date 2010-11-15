/*-------------------------------------------------------------------------
 *
 * replicacmds.c
 *
 *	  Helper functions for starting and stopping replication.
 *
 * Copyright (c) 2006-2010, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#ifndef REPLICACMDS_H
#define REPLICACMDS_H

#include "nodes/parsenodes.h"

extern void startReplicationForDatabase(Oid db_oid, char *dbname,
										Oid gcs_oid, char *group_name);
extern void stopReplicationForDatabase(Oid db_oid);
extern Oid lookupReplicationGcs(char *gcs_name);

#endif   /* REPLICACMDS_H */
