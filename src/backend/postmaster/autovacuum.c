/*-------------------------------------------------------------------------
 *
 * autovacuum.c
 *
 * PostgreSQL Integrated Autovacuum
 *
 * The autovacuum system now is a user of the generalized background worker
 * system.  When the autovacuum GUC parameter is set, the coordinator begins
 * to schedule autovacuum jobs for background workers when appropriate. The
 * workers are then executing the actual vacuuming; once they receive their
 * job, they examine the catalogs to select the table(s) to vacuum.
 *
 * Whenever a worker is done vacuuming the coordinator wakes up and rebalances
 * the settings for the various remaining workers' cost-based vacuum delay
 * feature. Note that there can well be more than one worker in a database
 * performing vacuum concurrently.
 *
 * They will store the table they are currently vacuuming in shared memory, so
 * that other workers avoid being blocked waiting for the vacuum lock for that
 * table.  They will also reload the pgstats data just before vacuuming each
 * table, to avoid vacuuming a table that was just finished being vacuumed by
 * another worker and thus is no longer noted in shared memory.  However,
 * there is a window (caused by pgstat delay) on which a worker may choose a
 * table that was already vacuumed; this is a bug in the current design.
 *
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
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
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
bool		autovacuum_enabled = false;
int			autovacuum_naptime;
int			autovacuum_vac_thresh;
double		autovacuum_vac_scale;
int			autovacuum_anl_thresh;
double		autovacuum_anl_scale;
int			autovacuum_freeze_max_age;

int			autovacuum_vac_cost_delay;
int			autovacuum_vac_cost_limit;

int			Log_autovacuum_min_duration = -1;

/* how long to keep pgstat data in the coordinator, in milliseconds */
#define STATS_READ_DELAY 1000

/* the minimum allowed time between two awakenings of the coordinator */
#define MIN_AUTOVAC_SLEEPTIME 100.0		/* milliseconds */

/* Comparison point for determining whether freeze_max_age is exceeded */
static TransactionId recentXid;

/* Default freeze ages to use for autovacuum (varies by database) */
static int	default_freeze_min_age;
static int	default_freeze_table_age;

/* struct to keep track of tables to vacuum and/or analyze, in 1st pass */
typedef struct av_relation
{
	Oid			ar_toastrelid;	/* hash key - must be first */
	Oid			ar_relid;
	bool		ar_hasrelopts;
	AutoVacOpts ar_reloptions;	/* copy of AutoVacOpts from the main table's
								 * reloptions, or NULL if none */
} av_relation;

/* struct to keep track of tables to vacuum and/or analyze, after rechecking */
typedef struct autovac_table
{
	Oid			at_relid;
	bool		at_dovacuum;
	bool		at_doanalyze;
	int			at_freeze_min_age;
	int			at_freeze_table_age;
	int			at_vacuum_cost_delay;
	int			at_vacuum_cost_limit;
	bool		at_wraparound;
	char	   *at_relname;
	char	   *at_nspname;
	char	   *at_datname;
} autovac_table;

static int	db_comparator(const void *a, const void *b);

static autovac_table *table_recheck_autovac(Oid relid, HTAB *table_toast_map,
					  TupleDesc pg_class_desc);
static void relation_needs_vacanalyze(Oid relid, AutoVacOpts *relopts,
						  Form_pg_class classForm,
						  PgStat_StatTabEntry *tabentry,
						  bool *dovacuum, bool *doanalyze, bool *wraparound);

static void autovacuum_do_vac_analyze(autovac_table *tab,
						  BufferAccessStrategy bstrategy);
static AutoVacOpts *extract_autovac_opts(HeapTuple tup,
					 TupleDesc pg_class_desc);
static PgStat_StatTabEntry *get_pgstat_tabentry_relid(Oid relid, bool isshared,
						  PgStat_StatDBEntry *shared,
						  PgStat_StatDBEntry *dbentry);
static void autovac_report_activity(autovac_table *tab);
static void autovac_refresh_stats(void);



/********************************************************************
 *					      COORDINATOR CODE
 ********************************************************************/

void
autovacuum_maybe_trigger_job(TimestampTz current_time, bool can_launch)
{
	Oid         dboid = InvalidOid;
	Dlelem	   *elem;
	IMessage   *msg;
	co_database *codb;

	/* FIXME: indentation */
	{

		/* We're OK to start a new worker */

		elem = DLGetTail(DatabaseList);
		if (elem != NULL)
		{
			avl_dbase  *avdb = DLE_VAL(elem);

			/*
			 * launch a worker if next_worker is right now or it is in the
			 * past
			 */
			if (TimestampDifferenceExceeds(avdb->adl_next_worker,
										   current_time, 0))
			{
				dboid = autovacuum_select_database();
				if (OidIsValid(dboid))
				{
					LWLockAcquire(CoordinatorDatabasesLock, LW_EXCLUSIVE);
                    codb = get_co_database(dboid);

					/*
					 * Only dispatch a job, if it can be processed immediately
					 * so we don't end up having lots of autovacuum requests
					 * in the job cache.
					 */
					if (can_launch || codb->codb_num_idle_workers > 0)
					{
						msg = IMessageCreate(IMSGT_PERFORM_VACUUM, 0);
						dispatch_job(msg, codb);
					}

					LWLockRelease(CoordinatorDatabasesLock);
				}
			}
		}
		else
		{
			/*
			 * If the list is still empty, this is a no-op. Instead we simply
			 * wait until the initial vacuum on the template database is
			 * done. That will populate pg_stat.
			 */
#ifdef COORDINATOR_DEBUG
			elog(DEBUG5, "Coordinator: DatabaseList is still empty.");
#endif
		}
	}
}

/*
 * Determine the time to sleep, based on the database list.
 *
 * The "can_launch" parameter indicates whether we can start a worker right now,
 * for example due to the workers being all busy.  If this is false, we will
 * cause a long sleep, which will be interrupted when a worker exits.
 */
void
coordinator_determine_sleep(bool can_launch, bool recursing, struct timespec *nap)
{
	Dlelem	   *elem;

	/*
	 * We sleep until the next scheduled vacuum.  We trust that when the
	 * database list was built, care was taken so that no entries have times
	 * in the past; if the first entry has too close a next_worker value, or a
	 * time in the past, we will sleep a small nominal time.
	 */
	if (!can_launch)
	{
		nap->tv_sec = autovacuum_naptime;
		nap->tv_nsec = 0;
	}
	else if ((elem = DLGetTail(DatabaseList)) != NULL)
	{
		avl_dbase  *avdb = DLE_VAL(elem);
		TimestampTz current_time = GetCurrentTimestamp();
		TimestampTz next_wakeup;
		long		secs;
		int			usecs;

		next_wakeup = avdb->adl_next_worker;
		TimestampDifference(current_time, next_wakeup, &secs, &usecs);

		nap->tv_sec = secs;
		nap->tv_nsec = usecs * 1000;
	}
	else
	{
		/* list is empty, sleep for whole autovacuum_naptime seconds  */
		nap->tv_sec = autovacuum_naptime;
		nap->tv_nsec = 0;
	}

	/*
	 * If the result is exactly zero, it means a database had an entry with
	 * time in the past.  Rebuild the list so that the databases are evenly
	 * distributed again, and recalculate the time to sleep.  This can happen
	 * if there are more tables needing vacuum than workers, and they all take
	 * longer to vacuum than autovacuum_naptime.
	 *
	 * We only recurse once.  rebuild_database_list should always return times
	 * in the future, but it seems best not to trust too much on that.
	 */
	if (nap->tv_sec == 0 && nap->tv_nsec == 0 && !recursing)
	{
		rebuild_database_list(InvalidOid);
		coordinator_determine_sleep(can_launch, true, nap);
		return;
	}

	/* The smallest time we'll allow the launcher to sleep. */
	if (nap->tv_sec <= 0 && nap->tv_nsec <= MIN_AUTOVAC_SLEEPTIME * 1000000)
	{
		nap->tv_sec = 0;
		nap->tv_nsec = MIN_AUTOVAC_SLEEPTIME * 1000000;
	}
}

/*
 * Build an updated DatabaseList.  It must only contain databases that appear
 * in pgstats, and must be sorted by next_worker from highest to lowest,
 * distributed regularly across the next autovacuum_naptime interval.
 *
 * Receives the Oid of the database that made this list be generated (we call
 * this the "new" database, because when the database was already present on
 * the list, we expect that this function is not called at all).  The
 * preexisting list, if any, will be used to preserve the order of the
 * databases in the autovacuum_naptime period.	The new database is put at the
 * end of the interval.  The actual values are not saved, which should not be
 * much of a problem.
 */
void
rebuild_database_list(Oid newdb)
{
	List	   *dblist;
	ListCell   *cell;
	MemoryContext newcxt;
	MemoryContext oldcxt;
	MemoryContext tmpcxt;
	HASHCTL		hctl;
	int			score;
	int			nelems;
	HTAB	   *dbhash;

	/* use fresh stats */
	autovac_refresh_stats();

	newcxt = AllocSetContextCreate(CoordinatorMemCxt,
								   "AV dblist",
								   ALLOCSET_DEFAULT_MINSIZE,
								   ALLOCSET_DEFAULT_INITSIZE,
								   ALLOCSET_DEFAULT_MAXSIZE);
	tmpcxt = AllocSetContextCreate(newcxt,
								   "tmp AV dblist",
								   ALLOCSET_DEFAULT_MINSIZE,
								   ALLOCSET_DEFAULT_INITSIZE,
								   ALLOCSET_DEFAULT_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	/*
	 * Implementing this is not as simple as it sounds, because we need to put
	 * the new database at the end of the list; next the databases that were
	 * already on the list, and finally (at the tail of the list) all the
	 * other databases that are not on the existing list.
	 *
	 * To do this, we build an empty hash table of scored databases.  We will
	 * start with the lowest score (zero) for the new database, then
	 * increasing scores for the databases in the existing list, in order, and
	 * lastly increasing scores for all databases gotten via
	 * get_database_list() that are not already on the hash.
	 *
	 * Then we will put all the hash elements into an array, sort the array by
	 * score, and finally put the array elements into the new doubly linked
	 * list.
	 */
	hctl.keysize = sizeof(Oid);
	hctl.entrysize = sizeof(avl_dbase);
	hctl.hash = oid_hash;
	hctl.hcxt = tmpcxt;
	dbhash = hash_create("db hash", 20, &hctl,	/* magic number here FIXME */
						 HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);

	/* start by inserting the new database */
	score = 0;
	if (OidIsValid(newdb))
	{
		avl_dbase  *db;
		PgStat_StatDBEntry *entry;

		/* only consider this database if it has a pgstat entry */
		entry = pgstat_fetch_stat_dbentry(newdb);
		if (entry != NULL)
		{
			/* we assume it isn't found because the hash was just created */
			db = hash_search(dbhash, &newdb, HASH_ENTER, NULL);

			/* hash_search already filled in the key */
			db->adl_score = score++;
			/* next_worker is filled in later */
		}
	}

	/* Now insert the databases from the existing list */
	if (DatabaseList != NULL)
	{
		Dlelem	   *elem;

		elem = DLGetHead(DatabaseList);
		while (elem != NULL)
		{
			avl_dbase  *avdb = DLE_VAL(elem);
			avl_dbase  *db;
			bool		found;
			PgStat_StatDBEntry *entry;

			elem = DLGetSucc(elem);

			/*
			 * skip databases with no stat entries -- in particular, this gets
			 * rid of dropped databases
			 */
			entry = pgstat_fetch_stat_dbentry(avdb->adl_datid);
			if (entry == NULL)
				continue;

			db = hash_search(dbhash, &(avdb->adl_datid), HASH_ENTER, &found);

			if (!found)
			{
				/* hash_search already filled in the key */
				db->adl_score = score++;
				/* next_worker is filled in later */
			}
		}
	}

	/* finally, insert all qualifying databases not previously inserted */
	dblist = get_database_list();
	foreach(cell, dblist)
	{
		avw_dbase  *avdb = lfirst(cell);
		avl_dbase  *db;
		bool		found;
		PgStat_StatDBEntry *entry;

		/* only consider databases with a pgstat entry */
		entry = pgstat_fetch_stat_dbentry(avdb->adw_datid);
		if (entry == NULL)
			continue;

		db = hash_search(dbhash, &(avdb->adw_datid), HASH_ENTER, &found);
		/* only update the score if the database was not already on the hash */
		if (!found)
		{
			/* hash_search already filled in the key */
			db->adl_score = score++;
			/* next_worker is filled in later */
		}
	}
	nelems = score;

	/* from here on, the allocated memory belongs to the new list */
	MemoryContextSwitchTo(newcxt);
	DatabaseList = DLNewList();

	if (nelems > 0)
	{
		TimestampTz current_time;
		int			millis_increment;
		avl_dbase  *dbary;
		avl_dbase  *db;
		HASH_SEQ_STATUS seq;
		int			i;

		/* put all the hash elements into an array */
		dbary = palloc(nelems * sizeof(avl_dbase));

		i = 0;
		hash_seq_init(&seq, dbhash);
		while ((db = hash_seq_search(&seq)) != NULL)
			memcpy(&(dbary[i++]), db, sizeof(avl_dbase));

		/* sort the array */
		qsort(dbary, nelems, sizeof(avl_dbase), db_comparator);

		/*
		 * Determine the time interval between databases in the schedule. If
		 * we see that the configured naptime would take us to sleep times
		 * lower than our min sleep time (which coordinator_determine_sleep is
		 * coded not to allow), silently use a larger naptime (but don't touch
		 * the GUC variable).
		 */
		millis_increment = 1000.0 * autovacuum_naptime / nelems;
		if (millis_increment <= MIN_AUTOVAC_SLEEPTIME)
			millis_increment = MIN_AUTOVAC_SLEEPTIME * 1.1;

		current_time = GetCurrentTimestamp();

		/*
		 * move the elements from the array into the dllist, setting the
		 * next_worker while walking the array
		 */
		for (i = 0; i < nelems; i++)
		{
			avl_dbase  *db = &(dbary[i]);
			Dlelem	   *elem;

			current_time = TimestampTzPlusMilliseconds(current_time,
													   millis_increment);
			db->adl_next_worker = current_time;

			elem = DLNewElem(db);
			/* later elements should go closer to the head of the list */
			DLAddHead(DatabaseList, elem);
		}
	}

	/* all done, clean up memory */
	if (DatabaseListCxt != NULL)
		MemoryContextDelete(DatabaseListCxt);
	MemoryContextDelete(tmpcxt);
	DatabaseListCxt = newcxt;
	MemoryContextSwitchTo(oldcxt);
}

/* qsort comparator for avl_dbase, using adl_score */
static int
db_comparator(const void *a, const void *b)
{
	if (((avl_dbase *) a)->adl_score == ((avl_dbase *) b)->adl_score)
		return 0;
	else
		return (((avl_dbase *) a)->adl_score < ((avl_dbase *) b)->adl_score) ? 1 : -1;
}

/*
 * autovacuum_select_database
 *
 * It determines what database to work on and sets up shared memory stuff. It
 * fails gracefully if invoked when autovacuum_workers are already active.
 *
 * Returns a pointer to the coordinator info struct of the database that the
 * next worker should process, or NULL if no database needs vacuuming.
 */
Oid
autovacuum_select_database(void)
{
	List		*dblist;
	ListCell    *cell;
	TransactionId xidForceLimit;
	bool		for_xid_wrap;
	avw_dbase  *avdb;
	TimestampTz current_time;
	bool		skipit = false;
	Oid         retval = InvalidOid;
	MemoryContext tmpcxt,
				oldcxt;

	/* return quickly when there are no free workers */
	LWLockAcquire(WorkerInfoLock, LW_SHARED);
	if (CoordinatorShmem->co_freeWorkers == NULL)
	{
		LWLockRelease(WorkerInfoLock);
		return InvalidOid;
	}
	LWLockRelease(WorkerInfoLock);

	/*
	 * Create and switch to a temporary context to avoid leaking the memory
	 * allocated for the database list.
	 */
	tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "Start worker tmp cxt",
								   ALLOCSET_DEFAULT_MINSIZE,
								   ALLOCSET_DEFAULT_INITSIZE,
								   ALLOCSET_DEFAULT_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	/* use fresh stats */
	autovac_refresh_stats();

	/* Get a list of databases */
	dblist = get_database_list();

	/*
	 * Determine the oldest datfrozenxid/relfrozenxid that we will allow to
	 * pass without forcing a vacuum.  (This limit can be tightened for
	 * particular tables, but not loosened.)
	 */
	recentXid = ReadNewTransactionId();
	xidForceLimit = recentXid - autovacuum_freeze_max_age;
	/* ensure it's a "normal" XID, else TransactionIdPrecedes misbehaves */
	if (xidForceLimit < FirstNormalTransactionId)
		xidForceLimit -= FirstNormalTransactionId;

	/*
	 * Choose a database to connect to.  We pick the database that was least
	 * recently auto-vacuumed, or one that needs vacuuming to prevent Xid
	 * wraparound-related data loss.  If any db at risk of wraparound is
	 * found, we pick the one with oldest datfrozenxid, independently of
	 * autovacuum times.
	 *
	 * Note that a database with no stats entry is not considered, except for
	 * Xid wraparound purposes.  The theory is that if no one has ever
	 * connected to it since the stats were last initialized, it doesn't need
	 * vacuuming.
	 *
	 * XXX This could be improved if we had more info about whether it needs
	 * vacuuming before connecting to it.  Perhaps look through the pgstats
	 * data for the database's tables?  One idea is to keep track of the
	 * number of new and dead tuples per database in pgstats.  However it
	 * isn't clear how to construct a metric that measures that and not cause
	 * starvation for less busy databases.
	 */
	avdb = NULL;
	for_xid_wrap = false;
	current_time = GetCurrentTimestamp();
	foreach(cell, dblist)
	{
		avw_dbase  *tmp = lfirst(cell);
		Dlelem	   *elem;

		/* Check to see if this one is at risk of wraparound */
		if (TransactionIdPrecedes(tmp->adw_frozenxid, xidForceLimit))
		{
			if (avdb == NULL ||
			  TransactionIdPrecedes(tmp->adw_frozenxid, avdb->adw_frozenxid))
				avdb = tmp;
			for_xid_wrap = true;
			continue;
		}
		else if (for_xid_wrap)
			continue;			/* ignore not-at-risk DBs */

		/* Find pgstat entry if any */
		tmp->adw_entry = pgstat_fetch_stat_dbentry(tmp->adw_datid);

		/*
		 * Skip a database with no pgstat entry; it means it hasn't seen any
		 * activity.
		 */
		if (!tmp->adw_entry)
			continue;

		/*
		 * Also, skip a database that appears on the database list as having
		 * been processed recently (less than autovacuum_naptime seconds ago).
		 * We do this so that we don't select a database which we just
		 * selected, but that pgstat hasn't gotten around to updating the last
		 * autovacuum time yet.
		 */
		skipit = false;
		elem = DatabaseList ? DLGetTail(DatabaseList) : NULL;

		while (elem != NULL)
		{
			avl_dbase  *dbp = DLE_VAL(elem);

			if (dbp->adl_datid == tmp->adw_datid)
			{
				/*
				 * Skip this database if its next_worker value falls between
				 * the current time and the current time plus naptime.
				 */
				if (!TimestampDifferenceExceeds(dbp->adl_next_worker,
												current_time, 0) &&
					!TimestampDifferenceExceeds(current_time,
												dbp->adl_next_worker,
												autovacuum_naptime * 1000))
					skipit = true;

				break;
			}
			elem = DLGetPred(elem);
		}
		if (skipit)
			continue;

		/*
		 * Remember the db with oldest autovac time.  (If we are here, both
		 * tmp->entry and db->entry must be non-null.)
		 */
		if (avdb == NULL ||
			tmp->adw_entry->last_autovac_time < avdb->adw_entry->last_autovac_time)
			avdb = tmp;
	}

	if (avdb != NULL)
	{
		/* We've found a database that needs vacuuming, return its id */
		retval = avdb->adw_datid;
	}
	else if (skipit)
	{
		/*
		 * If we skipped all databases on the list, rebuild it, because it
		 * probably contains a dropped database.
		 */
		rebuild_database_list(InvalidOid);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(tmpcxt);

	return retval;
}

/*
 * autovacuum_update_timing
 *
 * After having started an autovacuum job, the coordinator needs to update
 * the database list to reflect the next time that another one will need to
 * be started on the selected database.
 *
 * This routine is also expected to insert an entry into the database list if
 * the selected database was previously absent from the list.
 */

void
autovacuum_update_timing(Oid dbid, TimestampTz now)
{
	Dlelem		   *elem;

	/* FIXME: indentation */
	{

		/*
		 * Walk the database list and update the corresponding entry.  If the
		 * database is not on the list, we'll recreate the list.
		 */
		elem = (DatabaseList == NULL) ? NULL : DLGetHead(DatabaseList);
		while (elem != NULL)
		{
			avl_dbase  *avdb = DLE_VAL(elem);

			if (avdb->adl_datid == dbid)
			{
				/*
				 * add autovacuum_naptime seconds to the current time, and use
				 * that as the new "next_worker" field for this database.
				 */
				avdb->adl_next_worker =
					TimestampTzPlusMilliseconds(now, autovacuum_naptime * 1000);

				DLMoveToFront(elem);
				break;
			}
			elem = DLGetSucc(elem);
		}

		/*
		 * If the database was not present in the database list, we rebuild
		 * the list.  It's possible that the database does not get into the
		 * list anyway, for example if it's a database that doesn't have a
		 * pgstat entry, but this is not a problem because we don't want to
		 * schedule workers regularly into those in any case.
		 */
		if (elem == NULL)
			rebuild_database_list(dbid);
	}
}

/********************************************************************
 *					  AUTOVACUUM WORKER CODE
 ********************************************************************/

/*
 * Update the cost-based delay parameters, so that multiple workers consume
 * each a fraction of the total available I/O.
 */
void
AutoVacuumUpdateDelay(void)
{
	if (MyWorkerInfo)
	{
		VacuumCostDelay = MyWorkerInfo->wi_cost_delay;
		VacuumCostLimit = MyWorkerInfo->wi_cost_limit;
	}
}

/*
 * autovac_balance_cost
 *		Recalculate the cost limit setting for each active workers.
 *
 * Caller must hold the WorkerInfoLock in exclusive mode.
 */
void
autovac_balance_cost(void)
{
	WorkerInfo	worker;

	/*
	 * note: in cost_limit, zero also means use value from elsewhere, because
	 * zero is not a valid value.
	 */
	int			vac_cost_limit = (autovacuum_vac_cost_limit > 0 ?
								autovacuum_vac_cost_limit : VacuumCostLimit);
	int			vac_cost_delay = (autovacuum_vac_cost_delay >= 0 ?
								autovacuum_vac_cost_delay : VacuumCostDelay);
	double		cost_total;
	double		cost_avail;

	/* not set? nothing to do */
	if (vac_cost_limit <= 0 || vac_cost_delay <= 0)
		return;

	/* caculate the total base cost limit of active workers */
	cost_total = 0.0;
	worker = (WorkerInfo) SHMQueueNext(&CoordinatorShmem->co_runningWorkers,
									   &CoordinatorShmem->co_runningWorkers,
									   offsetof(WorkerInfoData, wi_links));
	while (worker)
	{
		if (worker->wi_backend_id != InvalidBackendId &&
			worker->wi_cost_limit_base > 0 && worker->wi_cost_delay > 0)
			cost_total +=
				(double) worker->wi_cost_limit_base / worker->wi_cost_delay;

		worker = (WorkerInfo) SHMQueueNext(&CoordinatorShmem->co_runningWorkers,
										   &worker->wi_links,
										 offsetof(WorkerInfoData, wi_links));
	}
	/* there are no cost limits -- nothing to do */
	if (cost_total <= 0)
		return;

	/*
	 * Adjust each cost limit of active workers to balance the total of cost
	 * limit to autovacuum_vacuum_cost_limit.
	 */
	cost_avail = (double) vac_cost_limit / vac_cost_delay;
	worker = (WorkerInfo) SHMQueueNext(&CoordinatorShmem->co_runningWorkers,
									   &CoordinatorShmem->co_runningWorkers,
									   offsetof(WorkerInfoData, wi_links));
	while (worker)
	{
		if (worker->wi_backend_id != InvalidBackendId &&
			worker->wi_cost_limit_base > 0 && worker->wi_cost_delay > 0)
		{
			int			limit = (int)
			(cost_avail * worker->wi_cost_limit_base / cost_total);

			/*
			 * We put a lower bound of 1 to the cost_limit, to avoid division-
			 * by-zero in the vacuum code.
			 */
			worker->wi_cost_limit = Max(Min(limit, worker->wi_cost_limit_base), 1);

			elog(DEBUG2, "autovac_balance_cost(backend_id=%u db=%u, rel=%u, cost_limit=%d, cost_delay=%d)",
				 worker->wi_backend_id, worker->wi_dboid,
				 worker->wi_tableoid, worker->wi_cost_limit, worker->wi_cost_delay);
		}

		worker = (WorkerInfo) SHMQueueNext(&CoordinatorShmem->co_runningWorkers,
										   &worker->wi_links,
										   offsetof(WorkerInfoData, wi_links));
	}
}

/*
 * Process a database table-by-table
 *
 * Note that CHECK_FOR_INTERRUPTS is supposed to be used in certain spots in
 * order not to ignore shutdown commands for too long.
 */
void
do_autovacuum(void)
{
	Relation	classRel;
	HeapTuple	tuple;
	HeapScanDesc relScan;
	Form_pg_database dbForm;
	List	   *table_oids = NIL;
	HASHCTL		ctl;
	HTAB	   *table_toast_map;
	ListCell   *volatile cell;
	PgStat_StatDBEntry *shared;
	PgStat_StatDBEntry *dbentry;
	BufferAccessStrategy bstrategy;
	ScanKeyData key;
	TupleDesc	pg_class_desc;

	recentXid = ReadNewTransactionId();

	/*
	 * may be NULL if we couldn't find an entry (only happens if we are
	 * forcing a vacuum for anti-wrap purposes).
	 */
	dbentry = pgstat_fetch_stat_dbentry(MyDatabaseId);

	/* Start a transaction so our commands have one to play into. */
	StartTransactionCommand();

	/*
	 * Clean up any dead statistics collector entries for this DB. We always
	 * want to do this exactly once per DB-processing cycle, even if we find
	 * nothing worth vacuuming in the database.
	 */
	pgstat_vacuum_stat();

	/*
	 * Find the pg_database entry and select the default freeze ages. We use
	 * zero in template and nonconnectable databases, else the system-wide
	 * default.
	 */
	tuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	dbForm = (Form_pg_database) GETSTRUCT(tuple);

	if (dbForm->datistemplate || !dbForm->datallowconn)
	{
		default_freeze_min_age = 0;
		default_freeze_table_age = 0;
	}
	else
	{
		default_freeze_min_age = vacuum_freeze_min_age;
		default_freeze_table_age = vacuum_freeze_table_age;
	}

	ReleaseSysCache(tuple);

	ereport(DEBUG1,
			(errmsg("autovacuum: processing database \"%s\"",
					NameStr(dbForm->datname))));

	/* StartTransactionCommand changed elsewhere */
	MemoryContextSwitchTo(BgWorkerMemCxt);

	/* The database hash where pgstat keeps shared relations */
	shared = pgstat_fetch_stat_dbentry(InvalidOid);

	classRel = heap_open(RelationRelationId, AccessShareLock);

	/* create a copy so we can use it after closing pg_class */
	pg_class_desc = CreateTupleDescCopy(RelationGetDescr(classRel));

	/* create hash table for toast <-> main relid mapping */
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(av_relation);
	ctl.hash = oid_hash;

	table_toast_map = hash_create("TOAST to main relid map",
								  100,
								  &ctl,
								  HASH_ELEM | HASH_FUNCTION);

	/*
	 * Scan pg_class to determine which tables to vacuum.
	 *
	 * We do this in two passes: on the first one we collect the list of plain
	 * relations, and on the second one we collect TOAST tables. The reason
	 * for doing the second pass is that during it we want to use the main
	 * relation's pg_class.reloptions entry if the TOAST table does not have
	 * any, and we cannot obtain it unless we know beforehand what's the main
	 * table OID.
	 *
	 * We need to check TOAST tables separately because in cases with short,
	 * wide tables there might be proportionally much more activity in the
	 * TOAST table than in its parent.
	 */
	ScanKeyInit(&key,
				Anum_pg_class_relkind,
				BTEqualStrategyNumber, F_CHAREQ,
				CharGetDatum(RELKIND_RELATION));

	relScan = heap_beginscan(classRel, SnapshotNow, 1, &key);

	/*
	 * On the first pass, we collect main tables to vacuum, and also the main
	 * table relid to TOAST relid mapping.
	 */
	while ((tuple = heap_getnext(relScan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classForm = (Form_pg_class) GETSTRUCT(tuple);
		PgStat_StatTabEntry *tabentry;
		AutoVacOpts *relopts;
		Oid			relid;
		bool		dovacuum;
		bool		doanalyze;
		bool		wraparound;

		relid = HeapTupleGetOid(tuple);

		/* Fetch reloptions and the pgstat entry for this table */
		relopts = extract_autovac_opts(tuple, pg_class_desc);
		tabentry = get_pgstat_tabentry_relid(relid, classForm->relisshared,
											 shared, dbentry);

		/* Check if it needs vacuum or analyze */
		relation_needs_vacanalyze(relid, relopts, classForm, tabentry,
								  &dovacuum, &doanalyze, &wraparound);

		/*
		 * Check if it is a temp table (presumably, of some other backend's).
		 * We cannot safely process other backends' temp tables.
		 */
		if (classForm->relistemp)
		{
			int			backendID;

			backendID = GetTempNamespaceBackendId(classForm->relnamespace);

			/* We just ignore it if the owning backend is still active */
			if (backendID == MyBackendId || !BackendIdIsActive(backendID))
			{
				/*
				 * We found an orphan temp table (which was probably left
				 * behind by a crashed backend).  If it's so old as to need
				 * vacuum for wraparound, forcibly drop it.  Otherwise just
				 * log a complaint.
				 */
				if (wraparound)
				{
					ObjectAddress object;

					ereport(LOG,
							(errmsg("autovacuum: dropping orphan temp table \"%s\".\"%s\" in database \"%s\"",
								 get_namespace_name(classForm->relnamespace),
									NameStr(classForm->relname),
									get_database_name(MyDatabaseId))));
					object.classId = RelationRelationId;
					object.objectId = relid;
					object.objectSubId = 0;
					performDeletion(&object, DROP_CASCADE);
				}
				else
				{
					ereport(LOG,
							(errmsg("autovacuum: found orphan temp table \"%s\".\"%s\" in database \"%s\"",
								 get_namespace_name(classForm->relnamespace),
									NameStr(classForm->relname),
									get_database_name(MyDatabaseId))));
				}
			}
		}
		else
		{
			/* relations that need work are added to table_oids */
			if (dovacuum || doanalyze)
				table_oids = lappend_oid(table_oids, relid);

			/*
			 * Remember the association for the second pass.  Note: we must do
			 * this even if the table is going to be vacuumed, because we
			 * don't automatically vacuum toast tables along the parent table.
			 */
			if (OidIsValid(classForm->reltoastrelid))
			{
				av_relation *hentry;
				bool		found;

				hentry = hash_search(table_toast_map,
									 &classForm->reltoastrelid,
									 HASH_ENTER, &found);

				if (!found)
				{
					/* hash_search already filled in the key */
					hentry->ar_relid = relid;
					hentry->ar_hasrelopts = false;
					if (relopts != NULL)
					{
						hentry->ar_hasrelopts = true;
						memcpy(&hentry->ar_reloptions, relopts,
							   sizeof(AutoVacOpts));
					}
				}
			}
		}
	}

	heap_endscan(relScan);

	/* second pass: check TOAST tables */
	ScanKeyInit(&key,
				Anum_pg_class_relkind,
				BTEqualStrategyNumber, F_CHAREQ,
				CharGetDatum(RELKIND_TOASTVALUE));

	relScan = heap_beginscan(classRel, SnapshotNow, 1, &key);
	while ((tuple = heap_getnext(relScan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classForm = (Form_pg_class) GETSTRUCT(tuple);
		PgStat_StatTabEntry *tabentry;
		Oid			relid;
		AutoVacOpts *relopts = NULL;
		bool		dovacuum;
		bool		doanalyze;
		bool		wraparound;

		/*
		 * We cannot safely process other backends' temp tables, so skip 'em.
		 */
		if (classForm->relistemp)
			continue;

		relid = HeapTupleGetOid(tuple);

		/*
		 * fetch reloptions -- if this toast table does not have them, try the
		 * main rel
		 */
		relopts = extract_autovac_opts(tuple, pg_class_desc);
		if (relopts == NULL)
		{
			av_relation *hentry;
			bool		found;

			hentry = hash_search(table_toast_map, &relid, HASH_FIND, &found);
			if (found && hentry->ar_hasrelopts)
				relopts = &hentry->ar_reloptions;
		}

		/* Fetch the pgstat entry for this table */
		tabentry = get_pgstat_tabentry_relid(relid, classForm->relisshared,
											 shared, dbentry);

		relation_needs_vacanalyze(relid, relopts, classForm, tabentry,
								  &dovacuum, &doanalyze, &wraparound);

		/* ignore analyze for toast tables */
		if (dovacuum)
			table_oids = lappend_oid(table_oids, relid);
	}

	heap_endscan(relScan);
	heap_close(classRel, AccessShareLock);

	/*
	 * Create a buffer access strategy object for VACUUM to use.  We want to
	 * use the same one across all the vacuum operations we perform, since the
	 * point is for VACUUM not to blow out the shared cache.
	 */
	bstrategy = GetAccessStrategy(BAS_VACUUM);

	/*
	 * create a memory context to act as fake PortalContext, so that the
	 * contexts created in the vacuum code are cleaned up for each table.
	 */
	PortalContext = AllocSetContextCreate(BgWorkerMemCxt,
										  "Autovacuum Portal",
										  ALLOCSET_DEFAULT_INITSIZE,
										  ALLOCSET_DEFAULT_MINSIZE,
										  ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * Perform operations on collected tables.
	 */
	foreach(cell, table_oids)
	{
		Oid			relid = lfirst_oid(cell);
		autovac_table *tab;
		WorkerInfo	worker;
		bool		skipit;

		CHECK_FOR_INTERRUPTS();

		/*
		 * hold schedule lock from here until we're sure that this table still
		 * needs vacuuming.  We also need the WorkerInfoLock to walk the
		 * worker array, but we'll let go of that one quickly.
		 */
		LWLockAcquire(WorkerScheduleLock, LW_EXCLUSIVE);
		LWLockAcquire(WorkerInfoLock, LW_SHARED);

		/*
		 * Check whether the table is being vacuumed concurrently by another
		 * worker.
		 */
		skipit = false;
		worker = (WorkerInfo) SHMQueueNext(&CoordinatorShmem->co_runningWorkers,
										 &CoordinatorShmem->co_runningWorkers,
										 offsetof(WorkerInfoData, wi_links));
		while (worker)
		{
			/* ignore myself */
			if (worker == MyWorkerInfo)
				goto next_worker;

			/* ignore workers in other databases */
			if (worker->wi_dboid != MyDatabaseId)
				goto next_worker;

			if (worker->wi_tableoid == relid)
			{
				skipit = true;
				break;
			}

	next_worker:
			worker = (WorkerInfo) SHMQueueNext(&CoordinatorShmem->co_runningWorkers,
											   &worker->wi_links,
										 offsetof(WorkerInfoData, wi_links));
		}
		LWLockRelease(WorkerInfoLock);
		if (skipit)
		{
			LWLockRelease(WorkerScheduleLock);
			continue;
		}

		/*
		 * Check whether pgstat data still says we need to vacuum this table.
		 * It could have changed if something else processed the table while
		 * we weren't looking.
		 *
		 * Note: we have a special case in pgstat code to ensure that the
		 * stats we read are as up-to-date as possible, to avoid the problem
		 * that somebody just finished vacuuming this table.  The window to
		 * the race condition is not closed but it is very small.
		 */
		MemoryContextSwitchTo(BgWorkerMemCxt);
		tab = table_recheck_autovac(relid, table_toast_map, pg_class_desc);
		if (tab == NULL)
		{
			/* someone else vacuumed the table, or it went away */
			LWLockRelease(WorkerScheduleLock);
			continue;
		}

		/*
		 * Ok, good to go.	Store the table in shared memory before releasing
		 * the lock so that other workers don't vacuum it concurrently.
		 */
		MyWorkerInfo->wi_tableoid = relid;
		LWLockRelease(WorkerScheduleLock);

		/* Set the initial vacuum cost parameters for this table */
		VacuumCostDelay = tab->at_vacuum_cost_delay;
		VacuumCostLimit = tab->at_vacuum_cost_limit;

		/* Last fixups before actually starting to work */
		LWLockAcquire(WorkerInfoLock, LW_EXCLUSIVE);

		/* advertise my cost delay parameters for the balancing algorithm */
		MyWorkerInfo->wi_cost_delay = tab->at_vacuum_cost_delay;
		MyWorkerInfo->wi_cost_limit = tab->at_vacuum_cost_limit;
		MyWorkerInfo->wi_cost_limit_base = tab->at_vacuum_cost_limit;

		/* do a balance */
		autovac_balance_cost();

		/* done */
		LWLockRelease(WorkerInfoLock);

		/* clean up memory before each iteration */
		MemoryContextResetAndDeleteChildren(PortalContext);

		/*
		 * Save the relation name for a possible error message, to avoid a
		 * catalog lookup in case of an error.	If any of these return NULL,
		 * then the relation has been dropped since last we checked; skip it.
		 * Note: they must live in a long-lived memory context because we call
		 * vacuum and analyze in different transactions.
		 */

		tab->at_relname = get_rel_name(tab->at_relid);
		tab->at_nspname = get_namespace_name(get_rel_namespace(tab->at_relid));
		tab->at_datname = get_database_name(MyDatabaseId);
		if (!tab->at_relname || !tab->at_nspname || !tab->at_datname)
			goto deleted;

		/*
		 * We will abort vacuuming the current table if something errors out,
		 * and continue with the next one in schedule; in particular, this
		 * happens if we are interrupted with SIGINT.
		 */
		PG_TRY();
		{
			/* have at it */
			MemoryContextSwitchTo(TopTransactionContext);
			autovacuum_do_vac_analyze(tab, bstrategy);

			/*
			 * Clear a possible query-cancel signal, to avoid a late reaction
			 * to an automatically-sent signal because of vacuuming the
			 * current table (we're done with it, so it would make no sense to
			 * cancel at this point.)
			 */
			QueryCancelPending = false;
		}
		PG_CATCH();
		{
			/*
			 * Abort the transaction, start a new one, and proceed with the
			 * next table in our list.
			 */
			HOLD_INTERRUPTS();
			if (tab->at_dovacuum)
				errcontext("automatic vacuum of table \"%s.%s.%s\"",
						   tab->at_datname, tab->at_nspname, tab->at_relname);
			else
				errcontext("automatic analyze of table \"%s.%s.%s\"",
						   tab->at_datname, tab->at_nspname, tab->at_relname);
			EmitErrorReport();

			/* this resets the PGPROC flags too */
			AbortOutOfAnyTransaction();
			FlushErrorState();
			MemoryContextResetAndDeleteChildren(PortalContext);

			/* restart our transaction for the following operations */
			StartTransactionCommand();
			RESUME_INTERRUPTS();
		}
		PG_END_TRY();

		/* the PGPROC flags are reset at the next end of transaction */

		/* be tidy */
deleted:
		if (tab->at_datname != NULL)
			pfree(tab->at_datname);
		if (tab->at_nspname != NULL)
			pfree(tab->at_nspname);
		if (tab->at_relname != NULL)
			pfree(tab->at_relname);
		pfree(tab);

		/* remove my info from shared memory */
		LWLockAcquire(WorkerInfoLock, LW_EXCLUSIVE);
		MyWorkerInfo->wi_tableoid = InvalidOid;
		LWLockRelease(WorkerInfoLock);
	}

	/*
	 * We leak table_toast_map here (among other things), but since we're
	 * going away soon, it's not a problem.
	 */

	/*
	 * Update pg_database.datfrozenxid, and truncate pg_clog if possible. We
	 * only need to do this once, not after each table.
	 */
	vac_update_datfrozenxid();

	/* Finally close out the last transaction. */
	CommitTransactionCommand();
}

/*
 * extract_autovac_opts
 *
 * Given a relation's pg_class tuple, return the AutoVacOpts portion of
 * reloptions, if set; otherwise, return NULL.
 */
static AutoVacOpts *
extract_autovac_opts(HeapTuple tup, TupleDesc pg_class_desc)
{
	bytea	   *relopts;
	AutoVacOpts *av;

	Assert(((Form_pg_class) GETSTRUCT(tup))->relkind == RELKIND_RELATION ||
		   ((Form_pg_class) GETSTRUCT(tup))->relkind == RELKIND_TOASTVALUE);

	relopts = extractRelOptions(tup, pg_class_desc, InvalidOid);
	if (relopts == NULL)
		return NULL;

	av = palloc(sizeof(AutoVacOpts));
	memcpy(av, &(((StdRdOptions *) relopts)->autovacuum), sizeof(AutoVacOpts));
	pfree(relopts);

	return av;
}

/*
 * get_pgstat_tabentry_relid
 *
 * Fetch the pgstat entry of a table, either local to a database or shared.
 */
static PgStat_StatTabEntry *
get_pgstat_tabentry_relid(Oid relid, bool isshared, PgStat_StatDBEntry *shared,
						  PgStat_StatDBEntry *dbentry)
{
	PgStat_StatTabEntry *tabentry = NULL;

	if (isshared)
	{
		if (PointerIsValid(shared))
			tabentry = hash_search(shared->tables, &relid,
								   HASH_FIND, NULL);
	}
	else if (PointerIsValid(dbentry))
		tabentry = hash_search(dbentry->tables, &relid,
							   HASH_FIND, NULL);

	return tabentry;
}

/*
 * table_recheck_autovac
 *
 * Recheck whether a table still needs vacuum or analyze.  Return value is a
 * valid autovac_table pointer if it does, NULL otherwise.
 *
 * Note that the returned autovac_table does not have the name fields set.
 */
static autovac_table *
table_recheck_autovac(Oid relid, HTAB *table_toast_map,
					  TupleDesc pg_class_desc)
{
	Form_pg_class classForm;
	HeapTuple	classTup;
	bool		dovacuum;
	bool		doanalyze;
	autovac_table *tab = NULL;
	PgStat_StatTabEntry *tabentry;
	PgStat_StatDBEntry *shared;
	PgStat_StatDBEntry *dbentry;
	bool		wraparound;
	AutoVacOpts *avopts;

	/* use fresh stats */
	autovac_refresh_stats();

	shared = pgstat_fetch_stat_dbentry(InvalidOid);
	dbentry = pgstat_fetch_stat_dbentry(MyDatabaseId);

	/* fetch the relation's relcache entry */
	classTup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(classTup))
		return NULL;
	classForm = (Form_pg_class) GETSTRUCT(classTup);

	/*
	 * Get the applicable reloptions.  If it is a TOAST table, try to get the
	 * main table reloptions if the toast table itself doesn't have.
	 */
	avopts = extract_autovac_opts(classTup, pg_class_desc);
	if (classForm->relkind == RELKIND_TOASTVALUE &&
		avopts == NULL && table_toast_map != NULL)
	{
		av_relation *hentry;
		bool		found;

		hentry = hash_search(table_toast_map, &relid, HASH_FIND, &found);
		if (found && hentry->ar_hasrelopts)
			avopts = &hentry->ar_reloptions;
	}

	/* fetch the pgstat table entry */
	tabentry = get_pgstat_tabentry_relid(relid, classForm->relisshared,
										 shared, dbentry);

	relation_needs_vacanalyze(relid, avopts, classForm, tabentry,
							  &dovacuum, &doanalyze, &wraparound);

	/* ignore ANALYZE for toast tables */
	if (classForm->relkind == RELKIND_TOASTVALUE)
		doanalyze = false;

	/* OK, it needs something done */
	if (doanalyze || dovacuum)
	{
		int			freeze_min_age;
		int			freeze_table_age;
		int			vac_cost_limit;
		int			vac_cost_delay;

		/*
		 * Calculate the vacuum cost parameters and the freeze ages.  If there
		 * are options set in pg_class.reloptions, use them; in the case of a
		 * toast table, try the main table too.  Otherwise use the GUC
		 * defaults, autovacuum's own first and plain vacuum second.
		 */

		/* -1 in autovac setting means use plain vacuum_cost_delay */
		vac_cost_delay = (avopts && avopts->vacuum_cost_delay >= 0)
			? avopts->vacuum_cost_delay
			: (autovacuum_vac_cost_delay >= 0)
			? autovacuum_vac_cost_delay
			: VacuumCostDelay;

		/* 0 or -1 in autovac setting means use plain vacuum_cost_limit */
		vac_cost_limit = (avopts && avopts->vacuum_cost_limit > 0)
			? avopts->vacuum_cost_limit
			: (autovacuum_vac_cost_limit > 0)
			? autovacuum_vac_cost_limit
			: VacuumCostLimit;

		/* these do not have autovacuum-specific settings */
		freeze_min_age = (avopts && avopts->freeze_min_age >= 0)
			? avopts->freeze_min_age
			: default_freeze_min_age;

		freeze_table_age = (avopts && avopts->freeze_table_age >= 0)
			? avopts->freeze_table_age
			: default_freeze_table_age;

		tab = palloc(sizeof(autovac_table));
		tab->at_relid = relid;
		tab->at_dovacuum = dovacuum;
		tab->at_doanalyze = doanalyze;
		tab->at_freeze_min_age = freeze_min_age;
		tab->at_freeze_table_age = freeze_table_age;
		tab->at_vacuum_cost_limit = vac_cost_limit;
		tab->at_vacuum_cost_delay = vac_cost_delay;
		tab->at_wraparound = wraparound;
		tab->at_relname = NULL;
		tab->at_nspname = NULL;
		tab->at_datname = NULL;
	}

	heap_freetuple(classTup);

	return tab;
}

/*
 * relation_needs_vacanalyze
 *
 * Check whether a relation needs to be vacuumed or analyzed; return each into
 * "dovacuum" and "doanalyze", respectively.  Also return whether the vacuum is
 * being forced because of Xid wraparound.
 *
 * relopts is a pointer to the AutoVacOpts options (either for itself in the
 * case of a plain table, or for either itself or its parent table in the case
 * of a TOAST table), NULL if none; tabentry is the pgstats entry, which can be
 * NULL.
 *
 * A table needs to be vacuumed if the number of dead tuples exceeds a
 * threshold.  This threshold is calculated as
 *
 * threshold = vac_base_thresh + vac_scale_factor * reltuples
 *
 * For analyze, the analysis done is that the number of tuples inserted,
 * deleted and updated since the last analyze exceeds a threshold calculated
 * in the same fashion as above.  Note that the collector actually stores
 * the number of tuples (both live and dead) that there were as of the last
 * analyze.  This is asymmetric to the VACUUM case.
 *
 * We also force vacuum if the table's relfrozenxid is more than freeze_max_age
 * transactions back.
 *
 * A table whose autovacuum_enabled option is false is
 * automatically skipped (unless we have to vacuum it due to freeze_max_age).
 * Thus autovacuum can be disabled for specific tables. Also, when the stats
 * collector does not have data about a table, it will be skipped.
 *
 * A table whose vac_base_thresh value is < 0 takes the base value from the
 * autovacuum_vacuum_threshold GUC variable.  Similarly, a vac_scale_factor
 * value < 0 is substituted with the value of
 * autovacuum_vacuum_scale_factor GUC variable.  Ditto for analyze.
 */
static void
relation_needs_vacanalyze(Oid relid,
						  AutoVacOpts *relopts,
						  Form_pg_class classForm,
						  PgStat_StatTabEntry *tabentry,
 /* output params below */
						  bool *dovacuum,
						  bool *doanalyze,
						  bool *wraparound)
{
	bool		force_vacuum;
	bool		av_enabled;
	float4		reltuples;		/* pg_class.reltuples */

	/* constants from reloptions or GUC variables */
	int			vac_base_thresh,
				anl_base_thresh;
	float4		vac_scale_factor,
				anl_scale_factor;

	/* thresholds calculated from above constants */
	float4		vacthresh,
				anlthresh;

	/* number of vacuum (resp. analyze) tuples at this time */
	float4		vactuples,
				anltuples;

	/* freeze parameters */
	int			freeze_max_age;
	TransactionId xidForceLimit;

	AssertArg(classForm != NULL);
	AssertArg(OidIsValid(relid));

	/*
	 * Determine vacuum/analyze equation parameters.  We have two possible
	 * sources: the passed reloptions (which could be a main table or a toast
	 * table), or the autovacuum GUC variables.
	 */

	/* -1 in autovac setting means use plain vacuum_cost_delay */
	vac_scale_factor = (relopts && relopts->vacuum_scale_factor >= 0)
		? relopts->vacuum_scale_factor
		: autovacuum_vac_scale;

	vac_base_thresh = (relopts && relopts->vacuum_threshold >= 0)
		? relopts->vacuum_threshold
		: autovacuum_vac_thresh;

	anl_scale_factor = (relopts && relopts->analyze_scale_factor >= 0)
		? relopts->analyze_scale_factor
		: autovacuum_anl_scale;

	anl_base_thresh = (relopts && relopts->analyze_threshold >= 0)
		? relopts->analyze_threshold
		: autovacuum_anl_thresh;

	freeze_max_age = (relopts && relopts->freeze_max_age >= 0)
		? Min(relopts->freeze_max_age, autovacuum_freeze_max_age)
		: autovacuum_freeze_max_age;

	av_enabled = (relopts ? relopts->enabled : true);

	/* Force vacuum if table is at risk of wraparound */
	xidForceLimit = recentXid - freeze_max_age;
	if (xidForceLimit < FirstNormalTransactionId)
		xidForceLimit -= FirstNormalTransactionId;
	force_vacuum = (TransactionIdIsNormal(classForm->relfrozenxid) &&
					TransactionIdPrecedes(classForm->relfrozenxid,
										  xidForceLimit));
	*wraparound = force_vacuum;

	/* User disabled it in pg_class.reloptions?  (But ignore if at risk) */
	if (!force_vacuum && !av_enabled)
	{
		*doanalyze = false;
		*dovacuum = false;
		return;
	}

	if (PointerIsValid(tabentry))
	{
		reltuples = classForm->reltuples;
		vactuples = tabentry->n_dead_tuples;
		anltuples = tabentry->changes_since_analyze;

		vacthresh = (float4) vac_base_thresh + vac_scale_factor * reltuples;
		anlthresh = (float4) anl_base_thresh + anl_scale_factor * reltuples;

		/*
		 * Note that we don't need to take special consideration for stat
		 * reset, because if that happens, the last vacuum and analyze counts
		 * will be reset too.
		 */
		elog(DEBUG3, "%s: vac: %.0f (threshold %.0f), anl: %.0f (threshold %.0f)",
			 NameStr(classForm->relname),
			 vactuples, vacthresh, anltuples, anlthresh);

		/* Determine if this table needs vacuum or analyze. */
		*dovacuum = force_vacuum || (vactuples > vacthresh);
		*doanalyze = (anltuples > anlthresh);
	}
	else
	{
		/*
		 * Skip a table not found in stat hash, unless we have to force vacuum
		 * for anti-wrap purposes.	If it's not acted upon, there's no need to
		 * vacuum it.
		 */
		*dovacuum = force_vacuum;
		*doanalyze = false;
	}

	/* ANALYZE refuses to work with pg_statistics */
	if (relid == StatisticRelationId)
		*doanalyze = false;
}

/*
 * autovacuum_do_vac_analyze
 *		Vacuum and/or analyze the specified table
 */
static void
autovacuum_do_vac_analyze(autovac_table *tab,
						  BufferAccessStrategy bstrategy)
{
	VacuumStmt	vacstmt;

	/* Set up command parameters --- use a local variable instead of palloc */
	MemSet(&vacstmt, 0, sizeof(vacstmt));

	vacstmt.type = T_VacuumStmt;
	vacstmt.options = 0;
	if (tab->at_dovacuum)
		vacstmt.options |= VACOPT_VACUUM;
	if (tab->at_doanalyze)
		vacstmt.options |= VACOPT_ANALYZE;
	vacstmt.freeze_min_age = tab->at_freeze_min_age;
	vacstmt.freeze_table_age = tab->at_freeze_table_age;
	vacstmt.relation = NULL;	/* not used since we pass a relid */
	vacstmt.va_cols = NIL;

	/* Let pgstat know what we're doing */
	autovac_report_activity(tab);

	vacuum(&vacstmt, tab->at_relid, false, bstrategy, tab->at_wraparound, true);
}

/*
 * autovac_report_activity
 *		Report to pgstat what autovacuum is doing
 *
 * We send a SQL string corresponding to what the user would see if the
 * equivalent command was to be issued manually.
 *
 * Note we assume that we are going to report the next command as soon as we're
 * done with the current one, and exit right after the last one, so we don't
 * bother to report "<IDLE>" or some such.
 */
static void
autovac_report_activity(autovac_table *tab)
{
#define MAX_AUTOVAC_ACTIV_LEN (NAMEDATALEN * 2 + 56)
	char		activity[MAX_AUTOVAC_ACTIV_LEN];
	int			len;

	/* Report the command and possible options */
	if (tab->at_dovacuum)
		snprintf(activity, MAX_AUTOVAC_ACTIV_LEN,
				 "autovacuum: VACUUM%s",
				 tab->at_doanalyze ? " ANALYZE" : "");
	else
		snprintf(activity, MAX_AUTOVAC_ACTIV_LEN,
				 "autovacuum: ANALYZE");

	/*
	 * Report the qualified name of the relation.
	 */
	len = strlen(activity);

	snprintf(activity + len, MAX_AUTOVAC_ACTIV_LEN - len,
			 " %s.%s%s", tab->at_nspname, tab->at_relname,
			 tab->at_wraparound ? " (to prevent wraparound)" : "");

	/* Set statement_timestamp() to current time for pg_stat_activity */
	SetCurrentStatementStartTimestamp();

	pgstat_report_activity(activity);
}

/*
 * autovac_init
 *		This is called at postmaster initialization.
 *
 * All we do here is annoy the user if he got it wrong.
 */
void
autovac_init(void)
{
	if (autovacuum_enabled && !pgstat_track_counts)
		ereport(WARNING,
				(errmsg("autovacuum disabled because of misconfiguration"),
				 errhint("Enable the \"track_counts\" option.")));
}

/*
 * autovac_refresh_stats
 *		Refresh pgstats data for an autovacuum process
 *
 * Cause the next pgstats read operation to obtain fresh data, but throttle
 * such refreshing in the coordinator.	This is mostly to avoid rereading
 * the pgstats files too many times in quick succession when there are many
 * databases.
 *
 * Note: we avoid throttling in the autovac worker, as it would be
 * counterproductive in the recheck logic.
 */
static void
autovac_refresh_stats(void)
{
	if (IsCoordinatorProcess())
	{
		static TimestampTz last_read = 0;
		TimestampTz current_time;

		current_time = GetCurrentTimestamp();

		if (!TimestampDifferenceExceeds(last_read, current_time,
										STATS_READ_DELAY))
			return;

		last_read = current_time;
	}

	pgstat_clear_snapshot();
}
