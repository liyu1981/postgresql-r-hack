/*-------------------------------------------------------------------------
 *
 * coordinator.h
 *	  header file for the coordinator
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#ifndef COORDINATOR_H
#define COORDINATOR_H

#include "pgstat.h"
#include "lib/dllist.h"
#include "utils/palloc.h"
#ifdef REPLICATION
#include "replication/gc.h"
#include "replication/replication.h"
#endif
#include "storage/imsg.h"
#include "storage/lock.h"

/*
 * Valid backend states for background workers.
 */
typedef enum
{
	WS_IDLE = 'I',

	WS_AUTOVACUUM = 'V',

#ifdef REPLICATION
	WS_CSET_APPLICATOR = 'C',
	WS_RECOVERY_PROVIDER = 'P',
	WS_RECOVERY_SUBSCRIBER = 'S'
#endif

} worker_state;

#define IsIdleWorker(wi)			(IsBackgroundWorkerProcess() && (wi->wi_state == WS_IDLE))
#define IsAutoVacuumWorker(wi)      (IsBackgroundWorkerProcess() && (wi->wi_state == WS_AUTOVACUUM))

#ifdef REPLICATION
#define IsCsetApplicator(wi)		(IsBackgroundWorkerProcess() && (wi->wi_state == WS_CSET_APPLICATOR))
#define IsRecoveryProvider(wi)		(IsBackgroundWorkerProcess() && (wi->wi_state == WS_RECOVERY_PROVIDER))
#define IsRecoverySubscriber(wi)	(IsBackgroundWorkerProcess() && (wi->wi_state == WS_RECOVERY_SUBSCRIBER))
#endif


/*-------------
 * This struct holds information about a single worker's whereabouts.  We keep
 * an array of these in shared memory, sized according to
 * max_background_workers.
 *
 * wi_links		entry into free list or running list
 * wi_dboid		OID of the database this worker is supposed to work on
 * wi_proc		pointer to PGPROC of the running worker, NULL if not started
 * wi_launchtime Time at which this worker was launched
 *
 * wi_tableoid	OID of the table currently being vacuumed
 * wi_cost_*	Vacuum cost-based delay parameters current in this worker
 *
 * All fields are protected by WorkerInfoLock, except for wi_tableoid which is
 * protected by WorkerScheduleLock (which is read-only for everyone except
 * that worker itself).
 *-------------
 */
typedef struct WorkerInfoData
{
	SHM_QUEUE	wi_links;
	Oid			wi_dboid;
	BackendId   wi_backend_id;
	TimestampTz wi_launchtime;
	worker_state wi_state;

	/* autovacuum specific fields */
	Oid			wi_tableoid;
	int			wi_cost_delay;
	int			wi_cost_limit;
	int			wi_cost_limit_base;
} WorkerInfoData;

typedef struct WorkerInfoData *WorkerInfo;

/*-------------
 * The main background worker shmem struct.  On shared memory we store this
 * main struct and the array of WorkerInfo structs.	This struct keeps:
 *
 * co_coordinatorid     the PID of the coordinator
 * co_freeWorkers       the WorkerInfo freelist
 * co_runningWorkers    the WorkerInfo non-free queue
 * co_startingWorker    pointer to WorkerInfo currently being started
 *                      (cleared by the worker itself as soon as it's up and
 *                      running)
 *
 * This struct is protected by WorkerInfoLock, except for parts of the worker
 * list (see above).
 *-------------
 */
typedef struct
{
	pid_t		co_coordinatorid;
	WorkerInfo	co_freeWorkers;
	SHM_QUEUE	co_runningWorkers;
	WorkerInfo	co_startingWorker;
	bool        co_av_started;
} CoordinatorShmemStruct;

extern CoordinatorShmemStruct *CoordinatorShmem;

/* struct to keep track of databases in launcher */
typedef struct avl_dbase
{
	Oid			adl_datid;		/* hash key -- must be first */
	TimestampTz adl_next_worker;
	int			adl_score;
} avl_dbase;

/* struct to keep track of databases in worker */
typedef struct avw_dbase
{
	Oid			adw_datid;		/* hash key -- must be first */
	char	   *adw_name;
	TransactionId adw_frozenxid;
	PgStat_StatDBEntry *adw_entry;
#ifdef REPLICATION
	char       *adw_group_name;
#endif
} avw_dbase;

/* struct to keep track of databases in the coordinator */
typedef struct co_database
{
	Oid					codb_dboid;

	/*
	 * The following fields are for internal use by the coordinator only. Used
	 * to keep track of messages that cannot currently be delivered. We
	 * differentiate two types of caches:
	 *
	 * Cached jobs, which can be delivered to any background worker (but
	 * might get cached because all workers are busy). This is a FIFO queue,
	 * so an idle worker always gets the first job from the queue.
	 *
	 * Out of order messages, which are part of another job. The coordinator
	 * already knows the background worker these messages need to be sent to.
	 * However, to maintain the required ordering of messages, it may decide
	 * to cache a message until it is deliverable.
	 */
	int                 codb_num_cached_jobs;
	Dllist              codb_cached_jobs;
	int                 codb_num_ooo_msgs;
	Dllist              codb_ooo_msgs;

	/* tracking of idle workers, shared */
	int				    codb_num_idle_workers;
	SHM_QUEUE           codb_idle_workers;

	/* tracking of connected workers, shared */
	int                 codb_num_connected_workers;

	/*
	 * fields used for replication
	 */

#ifdef REPLICATION
	/*
	 * The database "template0" is never replicated, so we mark it in
	 * populate_co_databases. Note that template1 isn't replicated either,
	 * but that one can easily be identified by its OID.
	 */
	bool				codb_is_template0;

	/* global database state, shared */
	rdb_state		state;

	/* all of the following fields are used by the coordinator only */
	Oid				gcs_oid;
	char		   *group_name;

	/* points to the coordinator-only state tracking struct */
	gcs_group	   *group;
#endif
} co_database;


/* GUC variables */
extern int	max_background_workers;
extern int min_spare_background_workers;
extern int max_spare_background_workers;

extern HTAB *co_databases;

/*
 * The database list in the coordinator, and the context that contains it,
 * for use by autovacuum
 */
extern Dllist *DatabaseList;
extern MemoryContext DatabaseListCxt;

extern MemoryContext CoordinatorMemCxt;
extern MemoryContext BgWorkerMemCxt;

extern WorkerInfo MyWorkerInfo;

extern char *decode_worker_state(worker_state state);
extern co_database *get_co_database(Oid dboid);

/* Status inquiry functions */
extern bool IsCoordinatorProcess(void);
extern bool IsBackgroundWorkerProcess(void);
extern BackendId GetCoordinatorId(void);
extern bool CoordinatorCanLaunchWorker(TimestampTz current_time);

extern void dispatch_job(IMessage *msg, co_database *codb);
extern void dispatch_ooo_msg(IMessage *msg, co_database *codb);
extern void drop_ooo_msgs_for(co_database *codb, BackendId backend_id);

extern bool coordinator_replication_can_deliver(IMessage *msg,
												co_database *codb,
												BackendId *target);

/* Process startup functions */
extern int	StartCoordinator(void);
extern int	StartBackgroundWorker(void);

#ifdef EXEC_BACKEND
extern void CoordinatorMain(int argc, char *argv[]);
extern void BackgroundWorkerMain(int argc, char *argv[]);
extern void BackgroundWorkerIAm(void);
extern void CoordinatorIAm(void);
#endif

/* shared memory stuff */
extern Size CoordinatorShmemSize(void);
extern void CoordinatorShmemInit(void);

/* various query functions */
extern List *get_database_list(void);

/* background worker control */
extern void bgworker_job_initialize(worker_state new_state);
extern void bgworker_job_completed(void);
extern void bgworker_job_failed(int errcode);
extern void bgworker_reset(void);

#endif   /* COORDINATOR_H */
