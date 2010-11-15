/*-------------------------------------------------------------------------
 *
 * procarray.h
 *	  POSTGRES process array definitions.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROCARRAY_H
#define PROCARRAY_H

#include "storage/lock.h"
#include "storage/procsignal.h"
#include "storage/standby.h"
#include "utils/snapshot.h"


extern Size ProcArrayShmemSize(void);
extern void CreateSharedProcArray(void);
extern void ProcArrayAdd(PGPROC *proc);
extern void ProcArrayRemove(PGPROC *proc, TransactionId latestXid);

extern void ProcArrayEndTransaction(PGPROC *proc, TransactionId latestXid);
extern void ProcArrayClearTransaction(PGPROC *proc);

extern void ProcArrayInitRecoveryInfo(TransactionId oldestActiveXid);
extern void ProcArrayApplyRecoveryInfo(RunningTransactions running);
extern void ProcArrayApplyXidAssignment(TransactionId topxid,
							int nsubxids, TransactionId *subxids);

extern void RecordKnownAssignedTransactionIds(TransactionId xid);
extern void ExpireTreeKnownAssignedTransactionIds(TransactionId xid,
									  int nsubxids, TransactionId *subxids,
									  TransactionId max_xid);
extern void ExpireAllKnownAssignedTransactionIds(void);
extern void ExpireOldKnownAssignedTransactionIds(TransactionId xid);

extern RunningTransactions GetRunningTransactionData(void);

extern Snapshot GetSnapshotData(Snapshot snapshot);

extern bool TransactionIdIsInProgress(TransactionId xid);
extern bool TransactionIdIsActive(TransactionId xid);
extern TransactionId GetOldestXmin(bool allDbs, bool ignoreVacuum);

extern int	GetTransactionsInCommit(TransactionId **xids_p);
extern bool HaveTransactionsInCommit(TransactionId *xids, int nxids);

extern PGPROC *BackendPidGetProc(int pid);
extern PGPROC *BackendIdGetProc(BackendId id);
extern BackendId BackendXidGetBackendId(TransactionId xid);
extern bool IsBackendPid(int pid);

extern VirtualTransactionId *GetCurrentVirtualXIDs(TransactionId limitXmin,
					  bool excludeXmin0, bool allDbs, int excludeVacuum,
					  int *nvxids);
extern VirtualTransactionId *GetConflictingVirtualXIDs(TransactionId limitXmin, Oid dbOid);
extern pid_t CancelConcurrentTransaction(TransactionId xid);
extern pid_t CancelVirtualTransaction(VirtualTransactionId vxid, ProcSignalReason sigmode);

extern int	CountActiveBackends(void);
extern int	CountDBBackends(Oid databaseid);
extern void CancelDBBackends(Oid databaseid, ProcSignalReason sigmode, bool conflictPending);
extern int	CountUserBackends(Oid roleid);
extern bool CountOtherDBBackends(Oid databaseId,
					 int *nbackends, int *nprepared);

extern void XidCacheRemoveRunningXids(TransactionId xid,
						  int nxids, const TransactionId *xids,
						  TransactionId latestXid);

#ifdef REPLICATION
typedef bool perBackendFunc(int pid, BackendId id, void *data);

extern void ForeachDBBackend(Oid databaseid, perBackendFunc *cb, void *data);
extern PGPROC *GetIdleHelperBackend(Oid databaseId);
#endif

#endif   /* PROCARRAY_H */
