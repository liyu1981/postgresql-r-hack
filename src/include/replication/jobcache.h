/*-------------------------------------------------------------------------
 *
 * jobcache.h
 *
 * Copyright (c) 2001-2010, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#ifndef JOBCACHE_H
#define JOBCACHE_H

#include "replication/replication.h"
#include "replication/coordinator.h"

extern void coordinator_forward_job(PGPROC *proc, IMessageType msg_type,
									buffer *b, group_node *sender_node,
									CommitOrderId coid);
extern void coordinator_process_cached_job(PGPROC *proc, co_database *rdbi);
extern void coordinator_cache_job(const co_database *rdbi,
						   PGPROC *proc, IMessageType msg_type,
						   buffer *b, group_node *sender_node,
						   CommitOrderId coid);
extern void coordinator_forward_or_cache_job(const gcs_group *group,
									  PGPROC *proc, IMessageType msg_type,
									  buffer *b,
									  group_node *sender_node,
									  CommitOrderId coid);

#endif   /* JOBCACHE_H */
