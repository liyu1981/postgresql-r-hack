/*-------------------------------------------------------------------------
 *
 * globals.c
 *	  global variable declarations
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL$
 *
 * NOTES
 *	  Globals used all over the place should be declared here and not
 *	  in other modules.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "libpq/pqcomm.h"
#include "miscadmin.h"
#include "storage/backendid.h"

#ifdef REPLICATION
#include "replication/replication.h"
#include "replication/cset.h"
#endif

ProtocolVersion FrontendProtocol;

volatile bool InterruptPending = false;
volatile bool QueryCancelPending = false;
volatile bool ProcDiePending = false;
volatile bool ImmediateInterruptOK = false;
volatile uint32 InterruptHoldoffCount = 0;
volatile uint32 CritSectionCount = 0;
volatile bool BgWorkerCleanupInProgress = false;

int			MyProcPid;
pg_time_t	MyStartTime;
struct Port *MyProcPort;
long		MyCancelKey;
int			MyPMChildSlot;

/*
 * DataDir is the absolute path to the top level of the PGDATA directory tree.
 * Except during early startup, this is also the server's working directory;
 * most code therefore can simply use relative paths and not reference DataDir
 * explicitly.
 */
char	   *DataDir = NULL;

char		OutputFileName[MAXPGPATH];	/* debugging output file */

char		my_exec_path[MAXPGPATH];	/* full path to my executable */
char		pkglib_path[MAXPGPATH];		/* full path to lib directory */

#ifdef EXEC_BACKEND
char		postgres_exec_path[MAXPGPATH];		/* full path to backend */

/* note: currently this is not valid in backend processes */
#endif

BackendId	MyBackendId = InvalidBackendId;

Oid			MyDatabaseId = InvalidOid;

Oid			MyDatabaseTableSpace = InvalidOid;

/*
 * DatabasePath is the path (relative to DataDir) of my database's
 * primary directory, ie, its directory in the default tablespace.
 */
char	   *DatabasePath = NULL;

pid_t		PostmasterPid = 0;

/*
 * IsPostmasterEnvironment is true in a postmaster process and any postmaster
 * child process; it is false in a standalone process (bootstrap or
 * standalone backend).  IsUnderPostmaster is true in postmaster child
 * processes.  Note that "child process" includes all children, not only
 * regular backends.  These should be set correctly as early as possible
 * in the execution of a process, so that error handling will do the right
 * things if an error should occur during process initialization.
 *
 * These are initialized for the bootstrap/standalone case.
 */
bool		IsPostmasterEnvironment = false;
bool		IsUnderPostmaster = false;

bool		ExitOnAnyError = false;

int			DateStyle = USE_ISO_DATES;
int			DateOrder = DATEORDER_MDY;
int			IntervalStyle = INTSTYLE_POSTGRES;
bool		HasCTZSet = false;
int			CTimeZone = 0;

bool		enableFsync = true;
bool		allowSystemTableMods = false;
int			work_mem = 1024;
int			maintenance_work_mem = 16384;

/*
 * Primary determinants of sizes of shared-memory structures.  MaxBackends is
 * MaxConnections + max_background_workers + 1 (it is computed by the GUC
 * assign hooks for those variables):
 */
int			NBuffers = 1000;
int			MaxBackends = 100;
int			MaxConnections = 90;

int			VacuumCostPageHit = 1;		/* GUC parameters for vacuum */
int			VacuumCostPageMiss = 10;
int			VacuumCostPageDirty = 20;
int			VacuumCostLimit = 200;
int			VacuumCostDelay = 0;

int			VacuumCostBalance = 0;		/* working state for vacuum */
bool		VacuumCostActive = false;

int			GinFuzzySearchLimit = 0;

#ifdef REPLICATION
/*
 * globals needed for replication
 */
ChangeSetPtr		CurrentCset = NULL;

/* number of csets sent and received per transaction */
int					csets_sent_counter = 0;
int					csets_recvd_counter = 0;

rdb_state			db_state = RDBS_UNKNOWN;

/* guc variables for replication */
bool				replication_enabled = false;
bool				replication_may_seed = false;
char			   *replication_gcs = "spread";
char			   *replication_group_name = "postgres";
int                 replication_peer_txn_entries = 1024;
int                 replication_co_txn_info_max = 1024;
int                 replication_imsg_shmem_dyn_buffer_size = 2097152;

#endif	/* REPLICATION */
