/*-------------------------------------------------------------------------
 *
 * coordinator.c
 *
 * PostgreSQL Coordinator of Background Worker
 *
 * The background worker system is structured in two different kinds of
 * processes: the coordinator and the background workers.  The coordinator
 * is an always-running process, started by the postmaster.  It schedules
 * background workers to be started when appropriate.  The workers are the
 * processes which execute the actual job, for example vacuuming; they
 * connect to a database as requested by the coordinator, and once connected
 * register with the coordinator to receive background jobs to process for
 * that database.
 *
 * The coordinator cannot start the worker processes by itself, because doing
 * so would cause robustness issues (namely, failure to shut them down on
 * exceptional conditions, and also, since the coordinator is connected to
 * shared memory and is thus subject to corruption there, it is not as robust
 * as the postmaster).  So it leaves that task to the postmaster.
 *
 * There is a shared memory area for the coordinator, where it stores state
 * information about the background workers.  When it wants a new worker to
 * start, it sets a flag in shared memory and sends a signal to the
 * postmaster.	Then postmaster knows nothing more than it must start a worker;
 * so it forks a new child, which turns into a background worker.	This new
 * process connects to shared memory, and there it can inspect the information
 * that the launcher has set up, including what database to connect to.
 *
 * If the fork() call fails in the postmaster, it sets a flag in the shared
 * memory area, and sends a signal to the coordinator.  The coordinator, upon
 * noticing the flag, can try again to start a worker for the same database
 * by resending the signal.  Note that the failure can only be transient (fork
 * failure due to high load, memory pressure, too many processes, etc); more
 * permanent problems, like failure to connect to a database, are detected
 * later in the worker and dealt with just by having the background worker
 * exit normally.  The coordinator will launch a new worker again later, per
 * schedule. (FIXME: that might be fine for VACUUM, but not for replication).
 *
 * When a background worker is done with a job it sends an IMSGT_READY to the
 * coordinator, signaling that it is ready to process another job for the
 * database it is connected to.
 *
 * Note that there can be more than one background worker in a database
 * concurrently.  The coordinator takes care of enforcing a lower and an
 * upper limit of spare workers (i.e. workers that are still connected to a
 * database, but are still waiting for a next job to process).
 *
 * Portions Copyright (c) 2010, Translattice, Inc
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "access/heapam.h"
#include "access/reloptions.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "commands/dbcommands.h"
#include "commands/vacuum.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/coordinator.h"
#include "postmaster/fork_process.h"
#include "postmaster/postmaster.h"
#ifdef REPLICATION
#include "replication/coordinator.h"
#include "replication/cset.h"
#include "replication/utils.h"
#endif
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/sinvaladt.h"
#include "tcop/tcopprot.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tqual.h"


/*
 * GUC parameters
 */
int			max_background_workers;
int			min_spare_background_workers;
int			max_spare_background_workers;

/* Flags to tell if we are in a coordinator or background worker process */
static bool am_coordinator = false;
static bool am_background_worker = false;

/* Flags set by signal handlers */
static volatile sig_atomic_t got_SIGHUP = false;
static volatile sig_atomic_t got_SIGUSR2 = false;
static volatile sig_atomic_t got_SIGTERM = false;

/* Memory contexts for long-lived data */
MemoryContext CoordinatorMemCxt;
MemoryContext BgWorkerMemCxt;

typedef struct cached_job {
	Dlelem cj_links;
	IMessage *cj_msg;
} cached_job;

typedef struct cached_msg {
	Dlelem cm_links;
	IMessage *cm_msg;
	BackendId cm_backend_id;
} cached_msg;

CoordinatorShmemStruct *CoordinatorShmem;

/*
 * Table of databases with at least one connected worker, resides in shared
 * memory, protected by CoordinatorDatabasesLock
 */
HTAB *co_databases = NULL;

/* the database list in the launcher, and the context that contains it */
Dllist *DatabaseList = NULL;
MemoryContext DatabaseListCxt = NULL;

/* Pointer to my own WorkerInfo, valid on each worker */
WorkerInfo MyWorkerInfo = NULL;
WorkerInfo terminatable_worker = NULL;

/* liyu: add one global flag for spread */
bool coordinator_now_terminate = false;

#ifdef EXEC_BACKEND
static pid_t coordinator_forkexec(void);
static pid_t bgworker_forkexec(void);
#endif
NON_EXEC_STATIC void BackgroundWorkerMain(int argc, char *argv[]);
static void handle_imessage(IMessage *msg);
NON_EXEC_STATIC void CoordinatorMain(int argc, char *argv[]);

static void init_co_database(co_database *codb);
static void populate_co_databases(void);

static bool can_deliver_cached_job(co_database *codb, IMessage *msg,
								   BackendId *target);
static WorkerInfo get_idle_worker(co_database *codb);
static void cache_job(IMessage *msg, co_database *codb);
static void forward_job(IMessage *msg, co_database *codb,
						 BackendId backend_id);
static void process_cached_jobs(co_database *codb);

static void process_ooo_msgs_for(co_database *codb, BackendId backend_id);
static void add_ooo_msg(IMessage *msg, co_database *codb,
						BackendId backend_id);


static void manage_workers(bool can_launch);

static void do_start_worker(Oid dboid);

static void add_as_idle_worker(Oid dbid, bool inc_connected_count);
static void FreeWorkerInfo(int code, Datum arg);

static void avl_sighup_handler(SIGNAL_ARGS);
static void avl_sigusr2_handler(SIGNAL_ARGS);
static void avl_sigterm_handler(SIGNAL_ARGS);


char *
decode_worker_state(worker_state state)
{
	switch (state)
	{
		case WS_IDLE: return "WS_IDLE";
		case WS_AUTOVACUUM: return "WS_AUTOVACUUM";

#ifdef REPLICATION
		case WS_CSET_APPLICATOR: return "WS_CSET_APPLICATOR";
		case WS_RECOVERY_PROVIDER: return "WS_RECOVERY_PROVIDER";
		case WS_RECOVERY_SUBSCRIBER: return "WS_RECOVERY_SUBSCRIBER";
#endif

		default: return "UNKNOWN STATE";
	}
}


/********************************************************************
 *					      COORDINATOR CODE
 ********************************************************************/

#ifdef EXEC_BACKEND
/*
 * forkexec routine for the coordinator process.
 *
 * Format up the arglist, then fork and exec.
 */
static pid_t
coordinator_forkexec(void)
{
	char	   *av[10];
	int			ac = 0;

	av[ac++] = "postgres";
	av[ac++] = "--forkcoordinator";
	av[ac++] = NULL;			/* filled in by postmaster_forkexec */
	av[ac] = NULL;

	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}

/*
 * We need this set from the outside, before InitProcess is called
 */
void
CoordinatorIAm(void)
{
	am_coordinator = true;
}
#endif

/*
 * Main entry point for coordinator process, to be called from the postmaster.
 */
int
StartCoordinator(void)
{
	pid_t		CoordinatorPID;

#ifdef EXEC_BACKEND
	switch ((CoordinatorPID = coordinator_forkexec()))
#else
	switch ((CoordinatorPID = fork_process()))
#endif
	{
		case -1:
			ereport(LOG,
				 (errmsg("could not fork the coordinator process: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* in postmaster child ... */
			/* Close the postmaster's sockets */
			ClosePostmasterPorts(false);

			/* Lose the postmaster's on-exit routines */
			on_exit_reset();

			CoordinatorMain(0, NULL);
			break;
#endif
		default:
			return (int) CoordinatorPID;
	}

	/* shouldn't get here */
	return 0;
}

static void
init_co_database(co_database *codb)
{
	Assert(ShmemAddrIsValid(codb));
	SHMQueueInit(&codb->codb_idle_workers);
	codb->codb_num_idle_workers = 0;

	/*
	 * While only the coordinator may fiddle with this list, as its entries
	 * reside in that process' memory, it's safe to set the counters to 0
	 * and initialize the list headers with NULL values using DLInitList().
	 */
	codb->codb_num_cached_jobs = 0;
	DLInitList(&codb->codb_cached_jobs);

	codb->codb_num_ooo_msgs = 0;
	DLInitList(&codb->codb_ooo_msgs);

	codb->codb_num_connected_workers = 0;

#ifdef REPLICATION
	codb->state = RDBS_UNKNOWN;
	codb->group_name = NULL;
#endif
}

static void
cache_job(IMessage *msg, co_database *codb)
{
	cached_job *job;

#ifdef COORDINATOR_DEBUG
	elog(DEBUG5, "Coordinator: caching job of type %s for database %d",
		 decode_imessage_type(msg->type), codb->codb_dboid);
#endif

	job = palloc(sizeof(cached_job));
	DLInitElem(&job->cj_links, job);
	job->cj_msg = msg;
	DLAddTail(&codb->codb_cached_jobs, &job->cj_links);
	codb->codb_num_cached_jobs++;
}

void
add_ooo_msg(IMessage *msg, co_database *codb, BackendId backend_id)
{
	cached_msg *cm;

#ifdef COORDINATOR_DEBUG
	elog(DEBUG5, "Coordinator: storing out-of-order message of type %s for database %d",
		 decode_imessage_type(msg->type), codb->codb_dboid);
#endif

	cm = palloc(sizeof(cached_msg));
	DLInitElem(&cm->cm_links, cm);
	cm->cm_msg = msg;
	cm->cm_backend_id = backend_id;
	DLAddTail(&codb->codb_ooo_msgs, &cm->cm_links);
	codb->codb_num_ooo_msgs++;
}

/*
 * get_idle_worker
 *
 * Returns the first idle worker for a given database, removing it from its
 * list of idle workers. The caller is expected to make sure that there is
 * at least one idle worker and it must hold the CoordinatorDatabasesLock.
 */
static WorkerInfo
get_idle_worker(co_database *codb)
{
	WorkerInfo worker;

	/* remove a worker from the list of idle workers */
	worker = (WorkerInfo) SHMQueueNext(&codb->codb_idle_workers,
									   &codb->codb_idle_workers,
									   offsetof(WorkerInfoData, wi_links));
	Assert(worker);
	SHMQueueDelete(&worker->wi_links);
	Assert(worker->wi_backend_id != InvalidBackendId);

	/* maintain per-database counter */
	codb->codb_num_idle_workers--;

	return worker;
}

/*
 * forward_job
 *
 * Takes an imessage and forwards it to the first idle backend for the given
 * database as its next job to process. The caller must hold the
 * CoordinatorDatabasesLock.
 */
static void
forward_job(IMessage *msg, co_database *codb, BackendId backend_id)
{
	/* various actions before job delivery depending on the message type */
	switch (msg->type)
	{
		case IMSGT_TERM_WORKER:
			break;

		case IMSGT_PERFORM_VACUUM:
#ifdef COORDINATOR_DEBUG
			elog(DEBUG1, "Coordinator: delivering msg %s of size %d for "
				 "database %d to backend %d",
				 decode_imessage_type(msg->type), msg->size, codb->codb_dboid,
				 backend_id);
#endif
			autovacuum_update_timing(codb->codb_dboid, GetCurrentTimestamp());
			break;

#ifdef REPLICATION
		case IMSGT_RECOVERY_RESTART:
		case IMSGT_SEQ_INCREMENT:
		case IMSGT_SEQ_SETVAL:
		case IMSGT_CSET:
		case IMSGT_ORDERING:
		case IMSGT_TXN_ABORTED:
		case IMSGT_RECOVERY_REQUEST:
		case IMSGT_RECOVERY_DATA:
		case IMSGT_SCHEMA_ADAPTION:
			coordinator_prepare_job_delivery(codb->group, msg, backend_id);
			break;
#endif

		default:
			Assert(false);
	}

	IMessageActivate(msg, backend_id);
}

/*
 * dispatch_job
 *
 * Depending on the status of idle workers, this either forwards a job to an
 * idle worker directly or caches it for later processing. The caller must
 * hold the CoordinatorDatabasesLock.
 */
void
dispatch_job(IMessage *msg, co_database *codb)
{
	bool can_deliver;
	BackendId target = InvalidBackendId;

	can_deliver = can_deliver_cached_job(codb, msg, &target);

	if (can_deliver && target == InvalidBackendId)
		can_deliver = (codb->codb_num_idle_workers > 0);

	if (can_deliver)
	{
		if (target == InvalidBackendId)
			target = get_idle_worker(codb)->wi_backend_id;
		forward_job(msg, codb, target);
	}
	else
		cache_job(msg, codb);
}

void
dispatch_ooo_msg(IMessage *msg, co_database *codb)
{
	bool can_deliver;
	BackendId target = InvalidBackendId;

	can_deliver = can_deliver_cached_job(codb, msg, &target);

	if (can_deliver && target == InvalidBackendId)
		can_deliver = (codb->codb_num_idle_workers > 0);

	if (can_deliver)
    {
		if (target == InvalidBackendId)
			target = get_idle_worker(codb)->wi_backend_id;
		forward_job(msg, codb, target);
		process_ooo_msgs_for(codb, target);
		elog(DEBUG1, "Coordinator:       forwarded job to bgworker:%d.", target);
    }
	else {
		add_ooo_msg(msg, codb, InvalidBackendId);
		elog(DEBUG1, "Coordinator:       job delayed.");
	}
}

/*
 * process_cached_jobs
 *
 * Dispatches cached jobs to idle background workers, as long as there are
 * of both. Before delivering a job, an additional check is performed with
 * can_deliver_cached_job(), which also chooses the background worker to run
 * the job on.
 */
static void
process_cached_jobs(co_database *codb)
{
	BackendId	target;
	cached_job *job;

#ifdef COORDINATOR_DEBUG
	elog(DEBUG5, "Coordinator: cached jobs: %d, idle workers: %d",
		 codb->codb_num_cached_jobs, codb->codb_num_idle_workers);
#endif

	job = (cached_job*) DLGetHead(&codb->codb_cached_jobs);
	while ((codb->codb_num_cached_jobs > 0) &&
		   (codb->codb_num_idle_workers > 0) &&
		   (job != NULL))
	{
		target = InvalidBackendId;
		if (can_deliver_cached_job(codb, job->cj_msg, &target))
		{
			/* remove the job from the cache */
			DLRemove(&job->cj_links);
			codb->codb_num_cached_jobs--;
			
			/* forward the job to some idle worker and cleanup */
			if (target == InvalidBackendId)
				target = get_idle_worker(codb)->wi_backend_id;

			forward_job(job->cj_msg, codb, target);
			pfree(job);

			/*
			 * Trigger subsequent ooo messages, as the delivery of a job
			 * might change the delivery status of further out-of-order
			 * messages 
			 */
			process_ooo_msgs_for(codb, target);
			
			job = (cached_job*) DLGetHead(&codb->codb_cached_jobs);
		}
		else
			job = (cached_job*) DLGetSucc(&job->cj_links);
	}
}

void
process_ooo_msgs_for(co_database *codb, BackendId backend_id)
{
	BackendId target;
	cached_msg *cm;
	bool can_deliver;

#ifdef COORDINATOR_DEBUG
	elog(DEBUG5, "Coordinator: out-of-order messages: %d",
		 codb->codb_num_ooo_msgs);
#endif

	cm = (cached_msg*) DLGetHead(&codb->codb_ooo_msgs);
	while (cm != NULL)
	{
		target = InvalidBackendId;
		can_deliver = false;

		if (cm->cm_backend_id == InvalidBackendId ||
			cm->cm_backend_id == backend_id)
		{
			can_deliver = can_deliver_cached_job(codb, cm->cm_msg, &target);
		}

		if (can_deliver)
		{
			if (target == InvalidBackendId)
			{
				elog(FATAL, "process_ooo_msg_for: no PGPROC, database %d, backend %d, msg type: %s",
					 codb->codb_dboid, backend_id, decode_imessage_type(cm->cm_msg->type));
			}

			Assert(target != InvalidBackendId);

			/* remove the message from the cache */
			DLRemove(&cm->cm_links);
			codb->codb_num_ooo_msgs--;

			forward_job(cm->cm_msg, codb, target);
			pfree(cm);

			/* re-scan the list of ooo messages */
			cm = (cached_msg*) DLGetHead(&codb->codb_ooo_msgs);
		}
		else
			cm = (cached_msg*) DLGetSucc(&cm->cm_links);
	}
}

void
drop_ooo_msgs_for(co_database *codb, BackendId backend_id)
{
	cached_msg *cm, *next;

	Assert(backend_id != InvalidBackendId);

#ifdef COORDINATOR_DEBUG
	elog(DEBUG5, "Coordinator: dropping ooo msgs for backend %d",
		 backend_id);
#endif

	cm = (cached_msg*) DLGetHead(&codb->codb_ooo_msgs);
	while (cm != NULL)
	{
		next = (cached_msg*) DLGetSucc(&cm->cm_links);

		if (cm->cm_backend_id == backend_id)
		{
			/* drop the message */
			DLRemove(&cm->cm_links);
			codb->codb_num_ooo_msgs--;
			pfree(cm);
		}

		/* continue scanning the list of ooo messages */
		cm = next;
	}
}

/*
 * populate_co_databases
 *
 * Called at startup of the coordinator to scan pg_database. Schedules an
 * initial VACUUM job on the template database to populate pg_stat.
 */
static void
populate_co_databases()
{
	List     *dblist;
	ListCell *cell;
	IMessage *msg;

	dblist = get_database_list();
	LWLockAcquire(CoordinatorDatabasesLock, LW_EXCLUSIVE);
	foreach(cell, dblist)
	{
		avw_dbase  *avdb = lfirst(cell);
		co_database *codb = get_co_database(avdb->adw_datid);
#ifdef REPLICATION
		codb->codb_is_template0 = !strncmp("template0", avdb->adw_name,
										   NAMEDATALEN);
		codb->group_name = pstrdup(avdb->adw_group_name);
#endif

		if (codb->codb_dboid == TemplateDbOid)
		{
			/*
			 * Create a cached job as an imessage to ourselves, but without
			 * activating it. It can get forwarded to a backend later on.
			 */
			msg = IMessageCreate(IMSGT_PERFORM_VACUUM, 0);
			cache_job(msg, codb);
		}


		pfree(avdb->adw_name);

#ifdef REPLICATION
		pfree(avdb->adw_group_name);
#endif										  
	}
	LWLockRelease(CoordinatorDatabasesLock);

	list_free(dblist);
}

/*
 * Main loop for the coordinator process.
 */
NON_EXEC_STATIC void
CoordinatorMain(int argc, char *argv[])
{
	sigjmp_buf	local_sigjmp_buf;
	IMessage   *msg = NULL;
	bool        can_launch;

	/* we are a postmaster subprocess now */
	IsUnderPostmaster = true;
	am_coordinator = true;

	/* reset MyProcPid */
	MyProcPid = getpid();

	/* record Start Time for logging */
	MyStartTime = time(NULL);
	
    /* Identify myself via ps */
	init_ps_display("coordinator process", "", "", "");

	elog(LOG, "Coordinator started with pid %d", MyProcPid);

    /* liyu: add some code for debuging child process */
	while(1)
	{
		sleep(1);
		FILE* fp = fopen("/var/pg_child_debug.txt", "r");
		if(fp != NULL)
		{
			fclose(fp);
			break;
		}
	}
    /* liyu: */

	if (PostAuthDelay)
		pg_usleep(PostAuthDelay * 1000000L);

	SetProcessingMode(InitProcessing);

	/*
	 * If possible, make this process a group leader, so that the postmaster
	 * can signal any child processes too.	(coordinator probably never has
	 * any child processes, but for consistency we make all postmaster child
	 * processes do this.)
	 */
#ifdef HAVE_SETSID
	if (setsid() < 0)
		elog(FATAL, "setsid() failed: %m");
#endif

	/*
	 * Set up signal handlers.	We operate on databases much like a regular
	 * backend, so we use the same signal handling.  See equivalent code in
	 * tcop/postgres.c.
	 */
	pqsignal(SIGHUP, avl_sighup_handler);
	pqsignal(SIGINT, StatementCancelHandler);
	pqsignal(SIGTERM, avl_sigterm_handler);

	pqsignal(SIGQUIT, quickdie);
	pqsignal(SIGALRM, handle_sig_alarm);

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, avl_sigusr2_handler);
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGCHLD, SIG_DFL);

	/* Early initialization */
	BaseInit();

	/*
	 * Create a per-backend PGPROC struct in shared memory, except in the
	 * EXEC_BACKEND case where this was done in SubPostmasterMain. We must do
	 * this before we can use LWLocks (and in the EXEC_BACKEND case we already
	 * had to do some stuff with LWLocks).
	 */
#ifndef EXEC_BACKEND
	InitProcess();
#endif

	InitPostgres(NULL, InvalidOid, NULL, NULL);

	SetProcessingMode(NormalProcessing);

	/*
	 * Create a memory context that we will do all our work in.  We do this so
	 * that we can reset the context during error recovery and thereby avoid
	 * possible memory leaks.
	 */
	CoordinatorMemCxt = AllocSetContextCreate(TopMemoryContext,
											  "Coordinator",
											  ALLOCSET_DEFAULT_MINSIZE,
											  ALLOCSET_DEFAULT_INITSIZE,
											  ALLOCSET_DEFAULT_MAXSIZE);
	MemoryContextSwitchTo(CoordinatorMemCxt);

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * This code is a stripped down version of PostgresMain error recovery.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Forget any pending QueryCancel request */
		QueryCancelPending = false;
		disable_sig_alarm(true);
		QueryCancelPending = false;		/* again in case timeout occurred */

		/* Report the error to the server log */
		EmitErrorReport();

		/* Abort the current transaction in order to recover */
		AbortOutOfAnyTransaction();

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 */
		MemoryContextSwitchTo(CoordinatorMemCxt);
		FlushErrorState();

		/* Flush any leaked data in the top-level context */
		MemoryContextResetAndDeleteChildren(CoordinatorMemCxt);

		/* don't leave dangling pointers to freed memory */
		DatabaseListCxt = NULL;
		DatabaseList = NULL;

		/*
		 * Make sure pgstat also considers our stat data as gone.  Note: we
		 * mustn't use autovac_refresh_stats here.
		 */
		pgstat_clear_snapshot();

		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();

		/*
		 * Sleep at least 1 second after any error.  We don't want to be
		 * filling the error logs as fast as we can.
		 */
		pg_usleep(1000000L);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/* must unblock signals before calling rebuild_database_list */
	PG_SETMASK(&UnBlockSig);

	CoordinatorShmem->co_coordinatorid = MyBackendId;

	/*
	 * Initial population of the database list from pg_database
	 */
	populate_co_databases();

#ifdef REPLICATION
	coordinator_replication_init();
#endif

	/*
	 * Create the initial database list.  The invariant we want this list to
	 * keep is that it's ordered by decreasing next_time.  As soon as an entry
	 * is updated to a higher time, it will be moved to the front (which is
	 * correct because the only operation is to add autovacuum_naptime to the
	 * entry, and time always increases).
	 */
	rebuild_database_list(InvalidOid);

	for (;;)
	{
		TimestampTz		current_time;
		struct timespec	nap;
		sigset_t		sigmask, oldmask;
		fd_set			socks;
		int				max_sock_id;
		bool			socket_ready;

		/*
		 * Emergency bailout if postmaster has died.  This is to avoid the
		 * necessity for manual cleanup of all postmaster children.
		 */
		if (!PostmasterIsAlive(true))
			proc_exit(1);

		can_launch = (CoordinatorShmem->co_freeWorkers != NULL);
		coordinator_determine_sleep(can_launch, false, &nap);

		/* Initialize variables for listening on sockets */ 
		FD_ZERO(&socks);
		max_sock_id = 0;
		socket_ready = false;

#ifdef REPLICATION
		coordinator_replication_reg_gcs(&socks, &max_sock_id);
#endif

#ifdef COORDINATOR_DEBUG
		elog(DEBUG3, "Coordinator: listening...");
#endif

		/* Allow sinval catchup interrupts while sleeping */
		EnableCatchupInterrupt();

		/*
		 * Sleep for a while according to schedule - and possibly interrupted
		 * by messages from one of the sockets or by internal messages from
		 * background workers or normal backends.
		 *
		 * Using pselect here prevents the possible loss of a singnal in
		 * between the last check for imessages and following select call.
		 * However, it requires a newish platform that supports pselect.
		 *
		 * On some platforms, signals won't interrupt select. Postgres used
		 * to split the nap time into one second intervals to ensure to react
		 * reasonably promptly for autovacuum purposes. However, for
		 * Postgres-R this is not tolerable, so that mechanism has been
		 * removed.
		 *
		 * FIXME: to support these platforms or others that don't implement
		 *        pselect properly, another work-around like for example the
		 *        self-pipe trick needs to be implemented. On Windows, we
		 *        could implement pselect based on the current port's select
		 *        method.
		 */

		sigemptyset(&sigmask);
		sigaddset(&sigmask, SIGINT);
		sigaddset(&sigmask, SIGHUP);
		sigaddset(&sigmask, SIGUSR2);
		sigprocmask(SIG_BLOCK, &sigmask, &oldmask);

		/*
		 * Final check for the (now blocked) signals. Prevent waiting on
		 * events on the file descriptors with pselect(), if we've already
		 * gotten a signal.
		 */
		if (!got_SIGTERM && !got_SIGUSR2 && !got_SIGHUP)
		{
			sigemptyset(&sigmask);
			if (pselect(max_sock_id + 1, &socks, NULL, NULL, &nap,
						&sigmask) < 0)
			{
				if (errno != EINTR)
				{
					elog(WARNING, "Coordinator: pselect failed: %m");
					socket_ready = true;
				}
			}
			else
				socket_ready = true;
		}

		sigprocmask(SIG_SETMASK, &oldmask, NULL);

		/*
		 * Emergency bailout if postmaster has died.  This is to avoid the
		 * necessity for manual cleanup of all postmaster children.
		 */
		if (!PostmasterIsAlive(true))
			proc_exit(1);

		DisableCatchupInterrupt();

		/* the normal shutdown case */
		if (got_SIGTERM)
		{
			coordinator_now_terminate = true;
			break;
		}

		if (got_SIGHUP)
		{
#ifdef COORDINATOR_DEBUG
			elog(DEBUG5, "Coordinator: got SIGHUP");
#endif

			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);

			/* rebalance in case the default cost parameters changed */
			LWLockAcquire(WorkerInfoLock, LW_EXCLUSIVE);
			autovac_balance_cost();
			LWLockRelease(WorkerInfoLock);

			/* rebuild the list in case the naptime changed */
			rebuild_database_list(InvalidOid);

#ifdef REPLICATION
			coordinator_check_seed_state();
#endif
		}

		/*
		 * postmaster signalled failure to start a worker
		 */
		if (got_SIGUSR2)
		{
#ifdef COORDINATOR_DEBUG
			elog(DEBUG5, "Coordinator: got SIGUSR2");
#endif

			got_SIGUSR2 = false;

			/*
			 * If the postmaster failed to start a new worker, we sleep
			 * for a little while and resend the signal.  The new worker's
			 * state is still in memory, so this is sufficient.  After
			 * that, we restart the main loop.
			 *
			 * XXX should we put a limit to the number of times we retry?
			 * I don't think it makes much sense, because a future start
			 * of a worker will continue to fail in the same way.
			 *
			 * FIXME: for the autovac launcher, it might have been okay
			 *        to just sleep. But the coordinator needs to remain
			 *        as responsive as possible, even if the postmaster
			 *        is currently unable to fork new workers.
			 */
			pg_usleep(1000000L);	/* 1s */
			SendPostmasterSignal(PMSIGNAL_START_BGWORKER);
			continue;
		}

		/* handle sockets with pending reads */
		if (socket_ready)
		{
#ifdef REPLICATION
		if (replication_enabled)
			coordinator_replication_check_sockets(&socks);
#endif
		}

		/* handle pending imessages */
		while ((msg = IMessageCheck()) != NULL)
			handle_imessage(msg);

		current_time  = GetCurrentTimestamp();
		can_launch = CoordinatorCanLaunchWorker(current_time);

		/*
		 * Periodically check and trigger autovacuum workers, if autovacuum
		 * is enabled.
		 */
		if (autovacuum_enabled)
			autovacuum_maybe_trigger_job(current_time, can_launch);

		manage_workers(can_launch);

#ifdef REPLICATION
		if (replication_enabled)
			coordinator_handle_gcs_connections();
#endif
	}

	/* Normal exit from the coordinator is here */
	ereport(LOG,
			(errmsg("coordinator shutting down")));
	CoordinatorShmem->co_coordinatorid = InvalidBackendId;

	proc_exit(0);				/* done */
}

void
handle_imessage(IMessage *msg)
{
	BackendId		msg_sender;
	PGPROC         *proc;
	TransactionId   local_xid = InvalidTransactionId;
	co_database    *codb = NULL;
#ifdef REPLICATION
	gcs_group      *group = NULL;
#endif
	Oid             dboid = InvalidOid;

	/*
	 * Get the PGPROC entry of the sender and the related database info, if
	 * any.
	 */
	msg_sender = msg->sender;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	proc = BackendIdGetProc(msg_sender);
	if (proc)
	{
		local_xid = proc->xid;
		dboid = proc->databaseId;
	}

	LWLockRelease(ProcArrayLock);

#ifdef COORDINATOR_DEBUG
	if (proc)
		elog(DEBUG3, "Coordinator: received %s of size %d from backend %d\n"
			 "\t(connected to db %d, local xid %d)",
			 decode_imessage_type(msg->type), msg->size, msg_sender,
			 dboid, local_xid);
	else
		elog(DEBUG3, "Coordinator: received %s of size %d from backend %d\n"
			 "\t(for which no PGPROC could be found)",
			 decode_imessage_type(msg->type), msg->size, msg_sender);
#endif

#ifdef REPLICATION
	if (dboid == TemplateDbOid)
		group = replication_group;
	else if (dboid != InvalidOid)
	{
		LWLockAcquire(CoordinatorDatabasesLock, LW_SHARED);
		codb = get_co_database(dboid);
		group = codb->group;
		LWLockRelease(CoordinatorDatabasesLock);

		if (group)
			Assert(group->dboid == dboid);
		else if (msg->type != IMSGT_READY &&
				  msg->type != IMSGT_REGISTER_WORKER &&
				  msg->type != IMSGT_FORCE_VACUUM)
		{
			elog(WARNING, "Coordinator: the db of worker %d has no replication group, skipping message %s",
				 msg_sender, decode_imessage_type(msg->type));
			IMessageRemove(msg);
			return;
		}
	}
#endif

	switch (msg->type)
	{
		/*
		 * Standard messages from background worker processes
		 */
		case IMSGT_REGISTER_WORKER:
		case IMSGT_READY:
			/* consume the message */
			IMessageRemove(msg);

			LWLockAcquire(CoordinatorDatabasesLock, LW_SHARED);
			codb = get_co_database(dboid);
			process_cached_jobs(codb);
			LWLockRelease(CoordinatorDatabasesLock);

			/*
			 * We trigger a DatabaseList rebuild if it is still empty and
			 * after a job is done. This mainly covers the initialization
			 * phase after the first background worker is done with vacuuming
			 * template1 (and thus having populated pgstat).
			 */
			if (DLGetHead(DatabaseList) == NULL)
				rebuild_database_list(InvalidOid);

			/*
			 * Rebalance cost limits, as the worker has already reported its
			 * startup to the stats collector.  However, that needs to be
			 * removed, so there's probably no point in rebalancing here.
			 * So: FIXME.
			 */
			LWLockAcquire(WorkerInfoLock, LW_EXCLUSIVE);
			autovac_balance_cost();
			LWLockRelease(WorkerInfoLock);

			break;

		case IMSGT_FORCE_VACUUM:
			/* consume the message */
			IMessageRemove(msg);

			/* trigger an autovacuum worker */
			dboid = autovacuum_select_database();
			if (dboid != InvalidOid)
			{
				LWLockAcquire(CoordinatorDatabasesLock, LW_SHARED);
				codb = get_co_database(dboid);
				msg = IMessageCreate(IMSGT_PERFORM_VACUUM, 0);
				dispatch_job(msg, codb);
				LWLockRelease(CoordinatorDatabasesLock);
			}
			break;

		/*
		 * Replication specific imessages
		 */
#ifdef REPLICATION
		case IMSGT_CSET:
			handle_imessage_cset(group, msg, local_xid);
			break;

		case IMSGT_TXN_ABORTED:
			handle_imessage_txn_aborted(group, msg);
			break;

		case IMSGT_ORDERING:
			handle_imessage_ordering_request(group, msg, local_xid);
			break;

		case IMSGT_SEQ_INCREMENT:
			handle_imessage_seq_increment(group, msg);
			break;

		case IMSGT_SEQ_SETVAL:
			handle_imessage_seq_setval(group, msg);
			break;

		case IMSGT_RECOVERY_RESTART:
			handle_imessage_recovery_restart(group, msg);
			break;

		case IMSGT_RECOVERY_DATA:
			handle_imessage_recovery_data(group, msg);
			break;

		case IMSGT_RECOVERY_REQUEST:
			handle_imessage_recovery_request(group, msg);
			break;

		case IMSGT_RELATION_RECOVERED:
			handle_imessage_recovery_lane_completed(group, msg);
			break;

		case IMSGT_SCHEMA_ADAPTION:
			handle_imessage_schema_adaption(group, msg);
			break;
#endif

		default:
			elog(WARNING, "Coordinator: unknown message type: %c, ignored!",
				 msg->type);
			IMessageRemove(msg);
	}
}



/*
 * get_co_database
 *
 * Gets or creates the database info for replication in shared memory.
 * Expects the caller to have the CoordinatorDatabasesLock.
 */
co_database *
get_co_database(Oid dboid)
{
    co_database *codb;
    bool found;

    codb = hash_search(co_databases, &dboid, HASH_ENTER, &found);
    if (!found)
        init_co_database(codb);

    return codb;
}

static bool
can_deliver_cached_job(co_database *codb, IMessage *msg, BackendId *target)
{
#ifdef COORDINATOR_DEBUG
	elog(DEBUG5, "Coordinator: checking deliverability of job type %s",
		 decode_imessage_type(msg->type));
#endif

	switch (msg->type)
	{
		case IMSGT_TERM_WORKER:
		case IMSGT_PERFORM_VACUUM:
			return true;

#ifdef REPLICATION
		case IMSGT_SEQ_INCREMENT:
		case IMSGT_SEQ_SETVAL:
		case IMSGT_CSET:
		case IMSGT_ORDERING:
		case IMSGT_TXN_ABORTED:
		case IMSGT_RECOVERY_DATA:
		case IMSGT_RECOVERY_REQUEST:
		case IMSGT_RECOVERY_RESTART:
		case IMSGT_SCHEMA_ADAPTION:
			return coordinator_replication_can_deliver(msg, codb, target);
#endif

		default:
			elog(WARNING, "Coordinator: missing deliverability check for "
				 "message type %s", decode_imessage_type(msg->type));
			return false;
	}
}

/*
 * manage_workers
 *
 * Starts background workers for databases which have at least one cached
 * job or which have less than min_background_workers connected. Within the
 * same loop, the max_background_workers is checked and terminates a worker
 * accordingly.
 * 
 * Note that at max one worker can be requested to start or stop per
 * invocation.
 */
static void
manage_workers(bool can_launch)
{
	HASH_SEQ_STATUS			hash_status;
	co_database	           *codb;
	Oid                     launch_dboid = InvalidOid;
	float                   max_score = 0.0,
		                    score;
	bool                    worker_slots_available;
	int                     idle_workers_required;
	int                     job_workers_required;

	LWLockAcquire(WorkerInfoLock, LW_SHARED);
	worker_slots_available = (CoordinatorShmem->co_freeWorkers != NULL);
	LWLockRelease(WorkerInfoLock);

	/*
	 * Terminate an unneeded worker that has been fetched from the list of
	 * idle workers in the last invocation. We defer sending the signal one
	 * invocation to make sure the coordinator had time to handle all
	 * pending messages from that worker. As idle workers don't ever send
	 * messages, we can safely assume there is no pending message from that
	 * worker by now.
	 */
	if (terminatable_worker != NULL)
	{
		IMessage *msg;

#ifdef COORDINATOR_DEBUG
		PGPROC *proc = BackendIdGetProc(terminatable_worker->wi_backend_id);
		if (proc)
			elog(DEBUG3, "Coordinator: terminating worker [%d/%d].",
				 proc->pid, terminatable_worker->wi_backend_id);
		else
			elog(WARNING, "Coordinator: terminating worker (no PGPROC, backend %d).",
				 terminatable_worker->wi_backend_id);
#endif

		msg = IMessageCreate(IMSGT_TERM_WORKER, 0);
		IMessageActivate(msg, terminatable_worker->wi_backend_id);

		terminatable_worker = NULL;
	}

#ifdef COORDINATOR_DEBUG
	elog(DEBUG3, "Coordinator: manage_workers: can_launch: %s, slots_available: %s",
		 (can_launch ? "true" : "false"), (worker_slots_available ? "true" : "false"));
#endif

	/*
	 * Check the list of databases and fire the first pending request
	 * we find.
	 */
	idle_workers_required = 0;
	job_workers_required = 0;
	LWLockAcquire(CoordinatorDatabasesLock, LW_SHARED);
	hash_seq_init(&hash_status, co_databases);
	while ((codb = (co_database*) hash_seq_search(&hash_status)))
	{
		score = ((float) codb->codb_num_cached_jobs /
				 (float) (codb->codb_num_connected_workers + 1)) * 100.0;

		if (codb->codb_num_idle_workers < min_spare_background_workers)
			score += (min_spare_background_workers -
					  codb->codb_num_idle_workers) * 10.0;

#ifdef COORDINATOR_DEBUG
		elog(DEBUG3, "Coordinator:     db %d, idle/conn: %d/%d, jobs: %d, score: %0.1f",
			 codb->codb_dboid, codb->codb_num_idle_workers,
			 codb->codb_num_connected_workers, codb->codb_num_cached_jobs,
			 score);
#endif

		if (codb->codb_num_cached_jobs &&
			(codb->codb_num_connected_workers == 0))
			job_workers_required++;

		if (codb->codb_num_idle_workers < min_spare_background_workers)
			idle_workers_required += (min_spare_background_workers -
									  codb->codb_num_idle_workers);

		/*
		 * FIXME: "misconfiguration" allows "starvation" in case the global
		 *        maximum is reached all with idle workers, but other dbs
		 *        w/o a single worker still have jobs.
		 */
		if (can_launch && ((codb->codb_num_cached_jobs > 0) ||
						   (codb->codb_num_idle_workers <
							min_spare_background_workers)))
		{
			if (can_launch && (score > max_score))
			{
				launch_dboid = codb->codb_dboid;
				max_score = score;
			}
		}

		/*
		 * If we are above limit, we fetch an idle worker from the list
		 * and mark it as terminatable. Actual termination happens in
		 * the following invocation, see above.
		 */
		if ((terminatable_worker == NULL) &&
			(codb->codb_num_idle_workers > max_spare_background_workers))
			terminatable_worker = get_idle_worker(codb);
	}
	LWLockRelease(CoordinatorDatabasesLock);

	if (!worker_slots_available && idle_workers_required > 0)
	{
		elog(WARNING, "Coordinator: no more background workers available, but requiring %d more, according to min_spare_background_workers.",
			 idle_workers_required);
	}

	if (!worker_slots_available && job_workers_required > 0)
	{
		elog(WARNING, "Coordinator: no background workers avalibale, but %d databases have background jobs pending.",
			 job_workers_required);
	}

	/* request a worker for the first database found, which needs one */
	if (OidIsValid(launch_dboid))
		do_start_worker(launch_dboid);
}

bool
CoordinatorCanLaunchWorker(TimestampTz current_time)
{
	bool		can_launch;

	/* FIXME: indentation */
	{

		/*
		 * There are some conditions that we need to check before trying to
		 * start a launcher.  First, we need to make sure that there is a
		 * launcher slot available.  Second, we need to make sure that no
		 * other worker failed while starting up.
		 */

		LWLockAcquire(WorkerInfoLock, LW_SHARED);

		can_launch = (CoordinatorShmem->co_freeWorkers != NULL);

		if (CoordinatorShmem->co_startingWorker != NULL)
		{
			int			waittime;
			WorkerInfo	worker = CoordinatorShmem->co_startingWorker;

#ifdef COORDINATOR_DEBUG
			elog(DEBUG5, "Coordinator: another worker is starting...");
#endif

			/*
			 * We can't launch another worker when another one is still
			 * starting up (or failed while doing so), so just sleep for a bit
			 * more; that worker will wake us up again as soon as it's ready.
			 * We will only wait autovacuum_naptime seconds (up to a maximum
			 * of 60 seconds) for this to happen however.  Note that failure
			 * to connect to a particular database is not a problem here,
			 * because the worker removes itself from the startingWorker
			 * pointer before trying to connect.  Problems detected by the
			 * postmaster (like fork() failure) are also reported and handled
			 * differently.  The only problems that may cause this code to
			 * fire are errors in the earlier sections of BackgroundWorkerMain,
			 * before the worker removes the WorkerInfo from the
			 * startingWorker pointer.
			 */
			waittime = Min(autovacuum_naptime, 60) * 1000;
			if (TimestampDifferenceExceeds(worker->wi_launchtime, current_time,
										   waittime))
			{
				LWLockRelease(WorkerInfoLock);
				LWLockAcquire(WorkerInfoLock, LW_EXCLUSIVE);

				/*
				 * No other process can put a worker in starting mode, so if
				 * startingWorker is still INVALID after exchanging our lock,
				 * we assume it's the same one we saw above (so we don't
				 * recheck the launch time).
				 */
				if (CoordinatorShmem->co_startingWorker != NULL)
				{
					worker = CoordinatorShmem->co_startingWorker;
					worker->wi_dboid = InvalidOid;
					worker->wi_tableoid = InvalidOid;
					worker->wi_backend_id = InvalidBackendId;
					worker->wi_launchtime = 0;
					worker->wi_links.next = (SHM_QUEUE *) CoordinatorShmem->co_freeWorkers;
					CoordinatorShmem->co_freeWorkers = worker;
					CoordinatorShmem->co_startingWorker = NULL;
					elog(WARNING, "worker took too long to start; cancelled");
				}
			}
			else
				can_launch = false;
		}
		LWLockRelease(WorkerInfoLock);	/* either shared or exclusive */
	}

	return can_launch;
}


/*
 * do_start_worker
 *
 * Bare-bones procedure for starting a background worker from the
 * coordinator. It sets up shared memory stuff and signals the postmaster to
 * start a worker.
 */
void
do_start_worker(Oid dboid)
{
	WorkerInfo	worker;

	Assert(OidIsValid(dboid));

#ifdef COORDINATOR_DEBUG
	elog(DEBUG3, "Coordinator: requesting worker for database %d.", dboid);
#endif

	LWLockAcquire(WorkerInfoLock, LW_EXCLUSIVE);

	/*
	 * Get a worker entry from the freelist.  We checked above, so there
	 * really should be a free slot -- complain very loudly if there
	 * isn't.
	 */
	worker = CoordinatorShmem->co_freeWorkers;
	if (worker == NULL)
		elog(FATAL, "no free worker found");

	CoordinatorShmem->co_freeWorkers = (WorkerInfo) worker->wi_links.next;

	worker->wi_dboid = dboid;
	worker->wi_backend_id = InvalidBackendId;
	worker->wi_launchtime = GetCurrentTimestamp();

	CoordinatorShmem->co_startingWorker = worker;

	LWLockRelease(WorkerInfoLock);

	SendPostmasterSignal(PMSIGNAL_START_BGWORKER);
}

/* SIGHUP: set flag to re-read config file at next convenient time */
static void
avl_sighup_handler(SIGNAL_ARGS)
{
	got_SIGHUP = true;
}

/* SIGUSR2: postmaster failed to fork a worker for us */
static void
avl_sigusr2_handler(SIGNAL_ARGS)
{
	got_SIGUSR2 = true;
}

/* SIGTERM: time to die */
static void
avl_sigterm_handler(SIGNAL_ARGS)
{
	got_SIGTERM = true;
}


/********************************************************************
 *					  AUTOVACUUM WORKER CODE
 ********************************************************************/

#ifdef EXEC_BACKEND
/*
 * forkexec routines for background workers.
 *
 * Format up the arglist, then fork and exec.
 */
static pid_t
bgworker_forkexec(void)
{
	char	   *av[10];
	int			ac = 0;

	av[ac++] = "postgres";
	av[ac++] = "--forkbgworker";
	av[ac++] = NULL;			/* filled in by postmaster_forkexec */
	av[ac] = NULL;

	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}

/*
 * We need this set from the outside, before InitProcess is called
 */
void
BackgroundWorkerIAm(void)
{
	am_background_worker = true;
}
#endif

/*
 * Main entry point for a background worker process.
 *
 * This code is heavily based on pgarch.c, q.v.
 */
int
StartBackgroundWorker(void)
{
	pid_t		worker_pid;

#ifdef EXEC_BACKEND
	switch ((worker_pid = bgworker_forkexec()))
#else
	switch ((worker_pid = fork_process()))
#endif
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork background worker process: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* in postmaster child ... */
			/* Close the postmaster's sockets */
			ClosePostmasterPorts(false);

			/* Lose the postmaster's on-exit routines */
			on_exit_reset();

			BackgroundWorkerMain(0, NULL);
			break;
#endif
		default:
			elog(LOG, "Replication Layer: forked background worker %d.", worker_pid);
			return (int) worker_pid;
	}

	/* shouldn't get here */
	return 0;
}

/*
 * add_as_idle_worker
 *
 * Marks the current worker as idle by adding it to the database's list of
 * idle worker backends. The caller is expected to hold the WorkerInfoLock.
 */
static void
add_as_idle_worker(Oid dbid, bool inc_connected_count)
{
	co_database *codb;

	Assert(SHMQueueIsDetached(&MyWorkerInfo->wi_links));

	/* Lookup the corresponding database, or create an entry for it */
	LWLockAcquire(CoordinatorDatabasesLock, LW_EXCLUSIVE);
	codb = get_co_database(dbid);

	if (inc_connected_count)
		codb->codb_num_connected_workers++;

	/* add as an idle worker */
	SHMQueueInsertBefore(&codb->codb_idle_workers, &MyWorkerInfo->wi_links);
	codb->codb_num_idle_workers++;

	LWLockRelease(CoordinatorDatabasesLock);
}

/*
 * bgworker_job_initialize
 *
 * Initializes the memory contexts for a background job.
 */
void
bgworker_job_initialize(worker_state new_state)
{
	/*
	 * Note that the coordinator is responsible for dequeuing the worker from
	 * the list of idle backends, but is shall *NOT* assign a worker state,
	 * we do that from the worker exclusively.
	 */
	Assert(MyWorkerInfo->wi_state == WS_IDLE);
	Assert(SHMQueueIsDetached(&MyWorkerInfo->wi_links));

	MyWorkerInfo->wi_state = new_state;
	switch (new_state)
	{
		case WS_IDLE:
			Assert(false);    /* use bgworker_job_completed instead */
			break;
		case WS_AUTOVACUUM:
			set_ps_display("bg worker: autovacuum", false);
			break;
#ifdef REPLICATION
		case WS_CSET_APPLICATOR:
			set_ps_display("bg worker: remote transaction", false);
			break;
		case WS_RECOVERY_SUBSCRIBER:
			set_ps_display("bg worker: recovery subscriber", false);
			break;
		case WS_RECOVERY_PROVIDER:
			set_ps_display("bg worker: recovery provider", false);
			break;
#endif
		default:
			set_ps_display("bg worker: unknown", false);
	}

	/*
	 * StartTransactionCommand and CommitTransactionCommand will
	 * automatically switch to other contexts.  None the less we need this
	 * one for other book-keeping of the various background jobs across
	 * transactions, for example to keep the list of relations to vacuum.
	 */
	Assert(BgWorkerMemCxt == NULL);
	BgWorkerMemCxt = AllocSetContextCreate(TopMemoryContext,
										   "Background Worker",
										   ALLOCSET_DEFAULT_MINSIZE,
										   ALLOCSET_DEFAULT_INITSIZE,
										   ALLOCSET_DEFAULT_MAXSIZE);

	MessageContext = AllocSetContextCreate(TopMemoryContext,
										   "MessageContext",
										   ALLOCSET_DEFAULT_MINSIZE,
										   ALLOCSET_DEFAULT_INITSIZE,
										   ALLOCSET_DEFAULT_MAXSIZE);

	MemoryContextSwitchTo(BgWorkerMemCxt);
}

/*
 * bgworker_job_completed
 *
 * Cleans up the memory contexts used for the worker's current job and
 * informs the coordinator.
 */
void
bgworker_job_completed(void)
{
	/* Notify the coordinator of the job completion. */
#ifdef COORDINATOR_DEBUG
	ereport(DEBUG1,
			(errmsg("bg worker [%d]: job completed.", MyProcPid)));
#endif

	/* reset the worker state */
	bgworker_reset();
}

void
bgworker_reset(void)
{
	BackendId CoordinatorId;
	IMessage *msg;

	elog(DEBUG5, "bg worker [%d/%d]: resetting",
		 MyProcPid, MyBackendId);

	Assert(MyWorkerInfo->wi_state != WS_IDLE);
	Assert(!TransactionIdIsValid(GetTopTransactionIdIfAny()));

	/* reset the worker state */
	MyWorkerInfo->wi_state = WS_IDLE;
	set_ps_display("bg worker: idle", false);

	/* clean up memory contexts */
	Assert(BgWorkerMemCxt);
	MemoryContextSwitchTo(TopMemoryContext);
	MemoryContextDelete(BgWorkerMemCxt);
	BgWorkerMemCxt = NULL;
	MemoryContextDelete(MessageContext);
	MessageContext = NULL;

	/*
	 * Reset the shared abort flag as well as the process-local handler
	 * state.
	 */
	MyProc->abortFlag = false;
	BgWorkerCleanupInProgress = false;

	/* propagate as idle worker, inform the coordinator */
	LWLockAcquire(WorkerInfoLock, LW_EXCLUSIVE);
	add_as_idle_worker(MyDatabaseId, false);
	LWLockRelease(WorkerInfoLock);

	CoordinatorId = GetCoordinatorId();
	if (CoordinatorId != InvalidBackendId)
	{
		msg = IMessageCreate(IMSGT_READY, 0);
		IMessageActivate(msg, CoordinatorId);
	}
	else
		elog(WARNING, "bg worker [%d/%d]: no coordinator?!?",
			 MyProcPid, MyBackendId);
}

void
bgworker_job_failed(int errcode)
{
	TransactionId xid;

	xid = GetTopTransactionIdIfAny();

#ifdef COORDINATOR_DEBUG
	ereport(DEBUG3,
			(errmsg("bg worker [%d/%d]: job failed (xid: %d)",
					MyProcPid, MyBackendId, xid)));
#endif

	/*
	 * Abort any transaction that might still be running and tell the
	 * coordinator that we are ready to process the next background job.
	 */
	AbortOutOfAnyTransaction();

	/*
	 * Flush the error state.
	 */
	FlushErrorState();

	/*
	 * Make sure pgstat also considers our stat data as gone.
	 */
	pgstat_clear_snapshot();

#ifdef REPLICATION
	/*
	 * If we had an open transaction we need to notify the coordinator about
	 * this abort, so it can keep track.
	 */
	if (TransactionIdIsValid(xid))
		send_txn_aborted_msg(xid, errcode);
#endif

	Assert(!IMessageCheck());
}

/*
 * BackgroundWorkerMain
 */
NON_EXEC_STATIC void
BackgroundWorkerMain(int argc, char *argv[])
{
	sigjmp_buf	local_sigjmp_buf;
	BackendId   coordinator_id;
	IMessage   *msg;
	Oid			dbid;
	char		dbname[NAMEDATALEN];
	bool		terminate_worker = false;

	/* we are a postmaster subprocess now */
	IsUnderPostmaster = true;
	am_background_worker = true;

	/* reset MyProcPid */
	MyProcPid = getpid();

	/* record Start Time for logging */
	MyStartTime = time(NULL);

	/* Identify myself via ps */
	init_ps_display("background worker process", "", "", "");

	SetProcessingMode(InitProcessing);

	/*
	 * If possible, make this process a group leader, so that the postmaster
	 * can signal any child processes too.	(autovacuum probably never has any
	 * child processes, but for consistency we make all postmaster child
	 * processes do this.)
	 */
#ifdef HAVE_SETSID
	if (setsid() < 0)
		elog(FATAL, "setsid() failed: %m");
#endif

	/*
	 * Set up signal handlers.	We operate on databases much like a regular
	 * backend, so we use the same signal handling.  See equivalent code in
	 * tcop/postgres.c.
	 *
	 * Currently, we don't pay attention to postgresql.conf changes that
	 * happen during a single daemon iteration, so we can ignore SIGHUP.
	 */
	pqsignal(SIGHUP, SIG_IGN);

	/*
	 * SIGINT is used to signal cancelling the current table's vacuum; SIGTERM
	 * means abort and exit cleanly, and SIGQUIT means abandon ship.
	 */
	pqsignal(SIGINT, StatementCancelHandler);
	pqsignal(SIGTERM, die);
	pqsignal(SIGQUIT, quickdie);
	pqsignal(SIGALRM, handle_sig_alarm);

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN);
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGCHLD, SIG_DFL);

	/* Early initialization */
	BaseInit();

	/*
	 * Create a per-backend PGPROC struct in shared memory, except in the
	 * EXEC_BACKEND case where this was done in SubPostmasterMain. We must do
	 * this before we can use LWLocks (and in the EXEC_BACKEND case we already
	 * had to do some stuff with LWLocks).
	 */
#ifndef EXEC_BACKEND
	InitProcess();
#endif

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * See notes in postgres.c about the design of this coding.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		EmitErrorReport();

		/*
		 * We can now go away.	Note that because we called InitProcess, a
		 * callback was registered to do ProcKill, which will clean up
		 * necessary state.
		 */
		proc_exit(0);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	PG_SETMASK(&UnBlockSig);

	/*
	 * Force zero_damaged_pages OFF in the background worker, even if it is
	 * set in postgresql.conf.	We don't really want such a dangerous option
	 * being applied non-interactively.
	 */
	SetConfigOption("zero_damaged_pages", "false", PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Force statement_timeout to zero to avoid a timeout setting from
	 * preventing regular maintenance from being executed.
	 */
	SetConfigOption("statement_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);

#ifdef REPLICATION
	/*
	 * Use serializable transaction isolation level, mainly important for
	 * applying change sets from remote transactions. Shouldn't affect
	 * autovacuum.
	 */
	/* liyu: so Backgroundworker still in repetable_read ? this is seemly
	 * so wrong.
	 */
	/* DefaultXactIsoLevel = XACT_REPEATABLE_READ; */
	DefaultXactIsoLevel = XACT_SERIALIZABLE;

	/*
	 * Use async commits for applying remote transactions. Shouldn't affect
	 * autovacuum, either.
	 */
	SetConfigOption("synchronous_commit", "off", PGC_SUSET, PGC_S_OVERRIDE);
#endif

	/*
	 * Get the info about the database we're going to work on.
	 */
	LWLockAcquire(WorkerInfoLock, LW_EXCLUSIVE);

	/*
	 * beware of startingWorker being INVALID; this should normally not
	 * happen, but if a worker fails after forking and before this, the
	 * launcher might have decided to remove it from the queue and start
	 * again.
	 */
	if (CoordinatorShmem->co_startingWorker == NULL)
	{
		/* no worker entry for me, go away */
		elog(WARNING, "background worker started without a worker entry");
		LWLockRelease(WorkerInfoLock);
		proc_exit(0);
	}

	MyWorkerInfo = CoordinatorShmem->co_startingWorker;
	dbid = MyWorkerInfo->wi_dboid;

	/* FIXME: indentation */
	{

		/*
		 * remove from the "starting" pointer, so that the launcher can start
		 * a new worker if required
		 */
		CoordinatorShmem->co_startingWorker = NULL;

		coordinator_id = CoordinatorShmem->co_coordinatorid;
		LWLockRelease(WorkerInfoLock);

		on_shmem_exit(FreeWorkerInfo, 0);

		/*
		 * Report autovac startup to the stats collector.  We deliberately do
		 * this before InitPostgres, so that the last_autovac_time will get
		 * updated even if the connection attempt fails.  This is to prevent
		 * autovac from getting "stuck" repeatedly selecting an unopenable
		 * database, rather than making any progress on stuff it can connect
		 * to.
		 */
		pgstat_report_autovac(dbid);

		/*
		 * Connect to the selected database
		 *
		 * Note: if we have selected a just-deleted database (due to using
		 * stale stats info), we'll fail and exit here.
		 */
		InitPostgres(NULL, dbid, NULL, dbname);
		SetProcessingMode(NormalProcessing);
		set_ps_display("bg worker: idle", false);
	}

	BgWorkerMemCxt = NULL;

	MyWorkerInfo->wi_backend_id = MyBackendId;
	MyWorkerInfo->wi_state = WS_IDLE;

#ifdef COORDINATOR_DEBUG
	elog(LOG, "bg worker [%d/%d]: connected to database %d",
		 MyProcPid, MyBackendId, dbid);
#endif

	/*
	 * Add as an idle worker and notify the coordinator only *after* having
	 * set MyProc->databaseId in InitPostgres, so the coordinator can
	 * determine which database we are connected to.
	 */
	LWLockAcquire(WorkerInfoLock, LW_EXCLUSIVE);
	add_as_idle_worker(dbid, true);
	LWLockRelease(WorkerInfoLock);

	/* register with the coordinator */
	if (coordinator_id != InvalidBackendId)
	{
		msg = IMessageCreate(IMSGT_REGISTER_WORKER, 0);
		IMessageActivate(msg, coordinator_id);
	}

    /* liyu: add some code for debuging replication bg worker process */
	if (dbid != 1 /* template1 */) {
		elog(DEBUG1, "bg worker [%d/%d]: stop wait debug ...", MyProcPid, MyBackendId);
		while(1)
		{
			sleep(1);
			FILE* fp = fopen("/var/pg_bgworker_debug.txt", "r");
			if(fp != NULL)
			{
				fclose(fp);
				break;
			}
		}
		elog(DEBUG1, "bg worker [%d/%d]: ... continue", MyProcPid, MyBackendId);
	}
    /* liyu: */

	if (PostAuthDelay)
		pg_usleep(PostAuthDelay * 1000000L);

	while (!terminate_worker)
	{
		PG_TRY();
		{
		/* FIXME: indentation */

		CHECK_FOR_INTERRUPTS();

		ImmediateInterruptOK = true;
		pg_usleep(1000000L);
		ImmediateInterruptOK = false;

		/*
		 * FIXME: check different ways of terminating a background worker
		 *        via ProcDiePending. How about postmaster initiated
		 *        restarts?
		 */
		if (ProcDiePending)
			elog(FATAL, "bg worker [%d/%d]: Terminated via ProcDie",
				 MyProcPid, MyBackendId);

		while ((msg = IMessageCheck()) && !terminate_worker)
		{
#ifdef COORDINATOR_DEBUG
			ereport(DEBUG3,
					(errmsg("bg worker [%d/%d]: received message %s of size %d "
							"from backend id %d, db %d, state: %s",
							MyProcPid, MyBackendId,
							decode_imessage_type(msg->type),
							msg->size, msg->sender,
							MyDatabaseId,
							decode_database_state(
								get_db_replication_state(MyDatabaseId)))));
#endif

			switch (msg->type)
			{
				case IMSGT_TERM_WORKER:
					IMessageRemove(msg);
					terminate_worker = true;
					break;

				case IMSGT_PERFORM_VACUUM:
					/* immediately remove the message to free shared memory */
					IMessageRemove(msg);

					bgworker_job_initialize(WS_AUTOVACUUM);

					/*
					 * Add ourselves to the list of runningWorkers
					 */
					LWLockAcquire(WorkerInfoLock, LW_EXCLUSIVE);
					SHMQueueInsertBefore(&CoordinatorShmem->co_runningWorkers,
										 &MyWorkerInfo->wi_links);
					LWLockRelease(WorkerInfoLock);

					/* do an appropriate amount of work */
					do_autovacuum();

					/*
					 * Remove ourselves from the list of runningWorkers and
					 * mark as available background worker.
					 */
					LWLockAcquire(WorkerInfoLock, LW_EXCLUSIVE);
					SHMQueueDelete(&MyWorkerInfo->wi_links);
					LWLockRelease(WorkerInfoLock);

					bgworker_job_completed();
					break;

#ifdef REPLICATION
				case IMSGT_DB_STATE:
					bgworker_db_state_change(msg);
					break;
					
				case IMSGT_CSET:
					if (csets_recvd_counter == 0)
						bgworker_job_initialize(WS_CSET_APPLICATOR);
					bgworker_apply_cset(msg);
					/*
					 * Change sets may open a transaction, but never close
					 * it. Only the following commit decision closes the
					 * transaction.
					 */
					break;

				case IMSGT_ORDERING:
					Assert(MyWorkerInfo->wi_state == WS_CSET_APPLICATOR);
					Assert(csets_recvd_counter > 0);
					bgworker_commit_request(msg);
					break;

				case IMSGT_TXN_ABORTED:
					Assert(MyWorkerInfo->wi_state == WS_CSET_APPLICATOR);
					Assert(csets_recvd_counter > 0);
					bgworker_abort_request(msg);
					bgworker_job_completed();
					break;

				case IMSGT_SEQ_INCREMENT:
					bgworker_job_initialize(WS_CSET_APPLICATOR);
					bgworker_seq_increment(msg);
					bgworker_job_completed();
					break;

				case IMSGT_SEQ_SETVAL:
					bgworker_job_initialize(WS_CSET_APPLICATOR);
					bgworker_seq_setval(msg);
					bgworker_job_completed();
					break;

				case IMSGT_RECOVERY_RESTART:
					bgworker_job_initialize(WS_RECOVERY_SUBSCRIBER);
					bgworker_recovery_restart(msg);
					break;

				case IMSGT_SCHEMA_ADAPTION:
					bgworker_job_initialize(WS_RECOVERY_SUBSCRIBER);
					bgworker_schema_adaption(msg);
					break;

				case IMSGT_RECOVERY_DATA:
					if (MyWorkerInfo->wi_state == WS_IDLE)
						bgworker_job_initialize(WS_RECOVERY_SUBSCRIBER);
					bgworker_recovery_data(msg);
					break;

				case IMSGT_RECOVERY_REQUEST:
					bgworker_job_initialize(WS_RECOVERY_PROVIDER);
					bgworker_recovery_request(msg);
					bgworker_job_completed();
					break;
#endif

				default:
					/* keep shared memory clean */
					IMessageRemove(msg);

					ereport(WARNING,
							(errmsg("bg worker [%d]: invalid message type "
									"'%c' ignored",
									MyProcPid, msg->type)));
			}

			CHECK_FOR_INTERRUPTS();
		}

		}
		PG_CATCH();
		{
			ErrorData *errdata;
			MemoryContext ecxt;

			ecxt = MemoryContextSwitchTo(BgWorkerMemCxt);
			errdata = CopyErrorData();

			elog(WARNING, "bg worker [%d/%d]: caught error '%s' in %s:%d, state %s",
				 MyProcPid, MyBackendId, errdata->message,
				 errdata->filename, errdata->lineno,
				 decode_worker_state(MyWorkerInfo->wi_state));

			/*
			 * Inform the coordinator about the failure.
			 */
			bgworker_job_failed(errdata->sqlerrcode);

			if (errdata->sqlerrcode == ERRCODE_QUERY_CANCELED)
			{
#ifdef DEBUG_CSET_APPL
				elog(DEBUG3, "bg worker [%d/%d]: cancelled active job.",
					 MyProcPid, MyBackendId);
#endif

				bgworker_reset();
			}
			else
			{
					/*
					 * FIXME: a lot of other error conditions can be pretty
					 *        fatal for a single node, i.e. out of memory or
					 *        out of disk space. As the whole cluster needs
					 *        to wait for a single node in certain occasions,
					 *        this can be fatal for a complete cluster!
					 */

				elog(WARNING, "bg worker [%s:%d]: unexpected error %d: '%s'!\n"
					 "    triggered from %s:%d (in %s)\n",
					 __FILE__, __LINE__, errdata->sqlerrcode,
					 errdata->message, errdata->filename, errdata->lineno,
					 errdata->funcname);
				/* re-throw the error, so the backend quits */
				MemoryContextSwitchTo(ecxt);
				ReThrowError(errdata);
			}
		}
		PG_END_TRY();
	}


	/* All done, go away */
	ereport(DEBUG1, (errmsg("bg worker [%d/%d]: terminating",
							MyProcPid, MyBackendId)));
	proc_exit(0);
}

/*
 * Return a WorkerInfo to the free list
 */
static void
FreeWorkerInfo(int code, Datum arg)
{
	co_database *codb;
	if (MyWorkerInfo != NULL)
	{
		LWLockAcquire(WorkerInfoLock, LW_EXCLUSIVE);

		if (!SHMQueueIsDetached(&MyWorkerInfo->wi_links))
			SHMQueueDelete(&MyWorkerInfo->wi_links);

		MyWorkerInfo->wi_links.next = (SHM_QUEUE *) CoordinatorShmem->co_freeWorkers;
		MyWorkerInfo->wi_dboid = InvalidOid;
		MyWorkerInfo->wi_tableoid = InvalidOid;
		MyWorkerInfo->wi_backend_id = InvalidBackendId;
		MyWorkerInfo->wi_launchtime = 0;
		MyWorkerInfo->wi_cost_delay = 0;
		MyWorkerInfo->wi_cost_limit = 0;
		MyWorkerInfo->wi_cost_limit_base = 0;
		CoordinatorShmem->co_freeWorkers = MyWorkerInfo;
		/* not mine anymore */
		MyWorkerInfo = NULL;

		/* decrease the conn count */
		LWLockAcquire(CoordinatorDatabasesLock, LW_EXCLUSIVE);
		codb = hash_search(co_databases, &MyDatabaseId, HASH_FIND, NULL);
		Assert(codb);
		codb->codb_num_connected_workers--;
		LWLockRelease(CoordinatorDatabasesLock);

		LWLockRelease(WorkerInfoLock);
	}
}

/*
 * get_database_list
 *		Return a list of all databases found in pg_database.
 *
 * Note: this is the only function in which the coordinator uses a
 * transaction.  Although we aren't attached to any particular database and
 * therefore can't access most catalogs, we do have enough infrastructure
 * to do a seqscan on pg_database.
 */
List *
get_database_list(void)
{
	List	   *dblist = NIL;
	Relation	rel;
	HeapScanDesc scan;
	HeapTuple	tup;

	/*
	 * Start a transaction so we can access pg_database, and get a snapshot.
	 * We don't have a use for the snapshot itself, but we're interested in
	 * the secondary effect that it sets RecentGlobalXmin.	(This is critical
	 * for anything that reads heap pages, because HOT may decide to prune
	 * them even if the process doesn't attempt to modify any tuples.)
	 */
	StartTransactionCommand();
	(void) GetTransactionSnapshot();

	/* Allocate our results in CoordinatorMemCxt, not transaction context */
	MemoryContextSwitchTo(CoordinatorMemCxt);

	rel = heap_open(DatabaseRelationId, AccessShareLock);
	scan = heap_beginscan(rel, SnapshotNow, 0, NULL);

	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_database pgdatabase = (Form_pg_database) GETSTRUCT(tup);
		avw_dbase *avdb;

		avdb = (avw_dbase *) palloc(sizeof(avw_dbase));

		avdb->adw_datid = HeapTupleGetOid(tup);
		avdb->adw_name = pstrdup(NameStr(pgdatabase->datname));
		avdb->adw_frozenxid = pgdatabase->datfrozenxid;
		/* this gets set later: */
		avdb->adw_entry = NULL;

#ifdef REPLICATION
		avdb->adw_group_name = pstrdup(NameStr(pgdatabase->datreplgroup));
#endif

		dblist = lappend(dblist, avdb);
	}

	heap_endscan(scan);
	heap_close(rel, AccessShareLock);

	CommitTransactionCommand();

	return dblist;
}

/*
 * process identification functions
 *		Return whether this is either a coordinator process or a background
 *		worker process.
 */
bool
IsCoordinatorProcess(void)
{
	return am_coordinator;
}

bool
IsBackgroundWorkerProcess(void)
{
	return am_background_worker;
}

/*
 * GetCoordinatorId
 *     Returns the backendId of the currently active coordinator process.
 */ 
BackendId
GetCoordinatorId(void)
{
	BackendId CoordinatorId;

	LWLockAcquire(WorkerInfoLock, LW_SHARED);
	CoordinatorId = CoordinatorShmem->co_coordinatorid;
	LWLockRelease(WorkerInfoLock);
   
	return CoordinatorId;
}

/*
 * CoordinatorShmemSize
 *		Compute space needed for autovacuum-related shared memory
 */
Size
CoordinatorShmemSize(void)
{
	Size		size;

	/*
	 * Need the fixed struct and the array of WorkerInfoData, plus per
	 * database entries in a hash. As we only track databases which have at
	 * least one worker attached, we won't ever need more than
	 * max_background_workers entries.
	 */
	size = sizeof(CoordinatorShmemStruct);
	size = MAXALIGN(size);
	size = add_size(size, mul_size(max_background_workers,
								   sizeof(WorkerInfoData)));
	size = add_size(size, hash_estimate_size(max_background_workers,
											 sizeof(co_database)));
	return size;
}

/*
 * CoordinatorShmemInit
 *		Allocate and initialize autovacuum-related shared memory
 */
void
CoordinatorShmemInit(void)
{
	HASHCTL     hctl;
	bool		found;

	CoordinatorShmem = (CoordinatorShmemStruct *)
		ShmemInitStruct("Background Worker Data",
						CoordinatorShmemSize(),
						&found);

	if (!IsUnderPostmaster)
	{
		WorkerInfo	worker;
		int			i;

		Assert(!found);

		CoordinatorShmem->co_coordinatorid = InvalidBackendId;
		CoordinatorShmem->co_freeWorkers = NULL;
		SHMQueueInit(&CoordinatorShmem->co_runningWorkers);
		CoordinatorShmem->co_startingWorker = NULL;

		worker = (WorkerInfo) ((char *) CoordinatorShmem +
							   MAXALIGN(sizeof(CoordinatorShmemStruct)));

		/* initialize the WorkerInfo free list */
		for (i = 0; i < max_background_workers; i++)
		{
			worker[i].wi_links.next = (SHM_QUEUE *) CoordinatorShmem->co_freeWorkers;
			CoordinatorShmem->co_freeWorkers = &worker[i];
		}
	}
	else
		Assert(found);

	hctl.keysize = sizeof(Oid);
	hctl.entrysize = sizeof(co_database);
	hctl.hash = oid_hash;
	co_databases = ShmemInitHash("Coordinator Database Info",
								 max_background_workers,
								 max_background_workers,
								 &hctl,
								 HASH_ELEM | HASH_FUNCTION);
}


