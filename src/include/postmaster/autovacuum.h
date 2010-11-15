/*-------------------------------------------------------------------------
 *
 * autovacuum.h
 *	  header file for integrated autovacuum feature
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#ifndef AUTOVACUUM_H
#define AUTOVACUUM_H

#include "postgres.h"
#include "pgstat.h"
#include "lib/dllist.h"
#include "storage/lock.h"

/* GUC variables */
extern bool autovacuum_enabled;
extern int	autovacuum_naptime;
extern int	autovacuum_vac_thresh;
extern double autovacuum_vac_scale;
extern int	autovacuum_anl_thresh;
extern double autovacuum_anl_scale;
extern int	autovacuum_freeze_max_age;
extern int	autovacuum_vac_cost_delay;
extern int	autovacuum_vac_cost_limit;
extern int	Log_autovacuum_min_duration;

/* Functions to start autovacuum process, called from postmaster */
extern void autovac_init(void);

/* Functions called from the coordinator */
extern void autovacuum_maybe_trigger_job(TimestampTz current_time,
										 bool can_launch);
extern void coordinator_determine_sleep(bool canlaunch, bool recursing,
										struct timespec *nap);
extern void autovacuum_update_timing(Oid dbid, TimestampTz now);
extern void autovacuum_check_timings(void);
extern void rebuild_database_list(Oid newdb);
extern void do_autovacuum(void);
extern void autovac_balance_cost(void);
extern Oid autovacuum_select_database(void);

/* autovacuum cost-delay balancer */
extern void AutoVacuumUpdateDelay(void);

#endif   /* AUTOVACUUM_H */
