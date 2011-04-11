/*-------------------------------------------------------------------------
 *
 * shmem.h
 *	  shared memory management structures
 *
 * Historical note:
 * A long time ago, Postgres' shared memory region was allowed to be mapped
 * at a different address in each process, and shared memory "pointers" were
 * passed around as offsets relative to the start of the shared memory region.
 * That is no longer the case: each process must map the shared memory region
 * at the same address.  This means shared memory pointers can be passed
 * around directly between different processes.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHMEM_H
#define SHMEM_H

#include <stddef.h>
#include "utils/hsearch.h"
#include "storage/s_lock.h"


/*
 * For debug builds with assertions enabled we also enable magic value
 * checking to help detect problems in dynamically allocated shared memory
 * areas, because buffer overflows there can hurt very badly.
 */
#ifdef USE_ASSERT_CHECKING
#define MAGIC
typedef uint64_t MAGIC_TYPE;
#define MAGIC_VALUE 0x52b32fa9048c71deLL
#endif

/* shmqueue.c */
typedef struct SHM_QUEUE
{
	struct SHM_QUEUE *prev;
	struct SHM_QUEUE *next;
} SHM_QUEUE;

typedef struct DequeElem
{
	struct DequeElem *next;
} DequeElem;

typedef struct Deque
{
	DequeElem head;
	DequeElem *tail;
	slock_t lock;
} Deque;

extern void deque_init(Deque *list);
extern void deque_enqueue(Deque *list, DequeElem *elem);
extern DequeElem *deque_dequeue(Deque *list);

/* primalloc.c */
extern int prim_size(void);
extern void prim_init(void);
extern void* prim_alloc(size_t sz);
extern void prim_free(void *ptr);
extern void prim_check(void *ptr);

/* wamalloc.c */
extern int wam_size(void);
extern void wam_init(void);
extern void* wam_alloc(size_t sz);
extern void wam_free(void *ptr);
extern void wam_check(void *ptr);

/* shmem.c */

/* TODO: replace with GUC variable to be configurable */
/* #define ShmemDynBufferSize 2097152		/\* 2 MiB *\/ */

extern int replication_imsg_shmem_dyn_buffer_size;

#define ShmemDynBufferSize replication_imsg_shmem_dyn_buffer_size

extern void InitShmemAccess(void *seghdr);
extern void InitShmemAllocation(void);
extern void *ShmemAlloc(Size size);
extern bool ShmemAddrIsValid(void *addr);
extern void InitShmemIndex(void);
extern HTAB *ShmemInitHash(const char *name, long init_size, long max_size,
			  HASHCTL *infoP, int hash_flags);
extern void *ShmemInitStruct(const char *name, Size size, bool *foundPtr);
extern Size add_size(Size s1, Size s2);
extern Size mul_size(Size s1, Size s2);

extern int ShmemDynAllocSize(void);
extern void ShmemDynAllocInit(void);
extern void *ShmemDynAlloc(size_t size);
extern void ShmemDynFree(void *ptr);
#ifdef MAGIC
extern void ShmemDynCheck(void *ptr);
#endif

/* ipci.c */
extern void RequestAddinShmemSpace(Size size);

/* size constants for the shmem index table */
 /* max size of data structure string name */
#define SHMEM_INDEX_KEYSIZE		 (48)
 /* estimated size of the shmem index table (not a hard limit) */
#define SHMEM_INDEX_SIZE		 (32)

/* this is a hash bucket in the shmem index table */
typedef struct
{
	char		key[SHMEM_INDEX_KEYSIZE];		/* string name */
	void	   *location;		/* location in shared mem */
	Size		size;			/* # bytes allocated for the structure */
} ShmemIndexEnt;

/*
 * prototypes for functions in shmqueue.c
 */
extern void SHMQueueInit(SHM_QUEUE *queue);
extern bool SHMQueueIsDetached(SHM_QUEUE *queue);
extern void SHMQueueElemInit(SHM_QUEUE *queue);
extern void SHMQueueDelete(SHM_QUEUE *queue);
extern void SHMQueueInsertBefore(SHM_QUEUE *queue, SHM_QUEUE *elem);
extern Pointer SHMQueueNext(SHM_QUEUE *queue, SHM_QUEUE *curElem,
			 Size linkOffset);
extern bool SHMQueueEmpty(SHM_QUEUE *queue);

#endif   /* SHMEM_H */
