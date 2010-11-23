/*-------------------------------------------------------------------------
 *
 * wamalloc.c
 *    simple, lock-based dynamic memory allocator for shared memory.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "storage/pg_shmem.h"
#include "storage/shmem.h"
#include "storage/spin.h"


/*
 * The number of sizeclasses and heaps (per sizeclass) to partition the
 * shared memory area into.
 */
#define NUMBER_OF_SIZECLASSES 16
#define NUMBER_OF_HEAPS 1

/*
 * The number of bits of a pointer, which always remain zero due to the
 * machine or implementation specific alignment. Higher numbers allow more
 * bits to be used for credits, but waste more space.
 */
#define POINTER_ALIGNMENT 6

/*
 * The number of bits to use for the count field. Again, plus the maximum
 * value. It must be able to store the maximum number of blocks fitting in
 * a superblock.
 */
#define BLOCK_COUNT_BITS 15
#define MAX_BLOCK_COUNT 32768

/*
 * Size of the superblocks to allocate, not including the superblock header
 * of struct t_wam_superblock.
 */
#define SUPERBLOCK_SIZE 65536

/*
 * Descriptor state constants
 */
#define STATE_EMPTY 0
#define STATE_PARTIAL 2
#define STATE_FULL 3


struct t_wam_heap;
typedef volatile struct t_wam_heap* t_heap_ptr;

#pragma pack(push)  /* push current alignment to stack */
#pragma pack(1)     /* set alignment to 1 byte boundary */

typedef struct t_wam_anchor
{
	union
	{
		struct
		{
			uint64_t avail:(62 - BLOCK_COUNT_BITS),
				count:BLOCK_COUNT_BITS,
				state:2;
		};
		uint64_t value;
	};
} t_anchor;

#pragma pack(pop)   /* restore original alignment from stack */

struct t_wam_desc
{
	volatile struct t_wam_desc *next;
#ifdef MAGIC
	MAGIC_TYPE					magic;
#endif
	struct t_wam_anchor		Anchor;
	volatile void			   *sb;
	t_heap_ptr					heap;
	unsigned int				sz;
	unsigned int				maxcount;
	slock_t						DescLock;
};
typedef volatile struct t_wam_desc* t_desc_ptr;

struct t_wam_sizeclass
{
	t_desc_ptr		partial_desc_list;
	unsigned int	sz;
};
typedef volatile struct t_wam_sizeclass* t_sizeclass_ptr;

struct t_wam_superblock
{
	volatile struct t_wam_superblock *next;
};
typedef volatile struct t_wam_superblock* t_superblock_ptr;

struct t_wam_heap
{
	t_desc_ptr		Active;
	t_sizeclass_ptr	sc;
	slock_t         HeapLock;
};

/*
 * Global control structure, appears exactly once.
 */
struct t_wam_control
{
	t_sizeclass_ptr 	sizeclasses;		/* global sizeclasses */
	t_heap_ptr			global_heap;		/* multiple heaps per sizeclass */
	t_desc_ptr			DescAvail;			/* list of avail. descriptors */
	t_superblock_ptr	SuperblockAvail;	/* list of avail. superblocks */
	slock_t             WamLock;            /* big allocator lock */
};
typedef volatile struct t_wam_control* t_control_ptr;

/* single global entry point(er) */
static volatile struct t_wam_control *ctl;

#define POINTER_MASK ((intptr_t) (-1) << POINTER_ALIGNMENT)
#define IS_ALIGNED(ptr) (((intptr_t) (ptr) & (~POINTER_MASK)) == 0)
#define ALIGN_POINTER(ptr) ((void*) (((intptr_t) ptr + ~POINTER_MASK) & POINTER_MASK))
#define ALIGN_SIZE(i) (((intptr_t) i + ~POINTER_MASK) & POINTER_MASK)
#define USABLE_SUPERBLOCK_SIZE (SUPERBLOCK_SIZE - \
								sizeof(struct t_wam_superblock))

static t_heap_ptr find_heap(size_t request_size);
static t_superblock_ptr wam_alloc_sblock(void);
static void wam_free_sblock(t_superblock_ptr sb);
static volatile void* wam_desc_alloc(void);

static void wam_partial_list_push(t_sizeclass_ptr sc, t_desc_ptr desc);
static t_desc_ptr wam_partial_list_pop(t_sizeclass_ptr sc);

static void wam_desc_free(volatile void *ptr);
static void* wam_alloc_from_partial(t_heap_ptr heap);
static void* wam_alloc_from_sblock(t_heap_ptr heap);


static t_heap_ptr
find_heap(size_t request_size)
{	
	int sizeclass_no;
    int heap_no;

	/*
	 * We require an additional header to point to the descriptor. Possibly
	 * add 64bit for a magic value to be checked.
	 */
	request_size += sizeof(t_desc_ptr);

	/*
	 * FIXME: there certainly are more efficient ways to find the target
	 *        sizeclass.
	 */
	sizeclass_no = 0;
	while (ctl->sizeclasses[sizeclass_no].sz < request_size)
		sizeclass_no++;
	Assert(sizeclass_no < NUMBER_OF_SIZECLASSES);

	heap_no = MyProcPid % NUMBER_OF_HEAPS;

	return &ctl->global_heap[sizeclass_no * NUMBER_OF_HEAPS + heap_no];
}

static t_superblock_ptr
wam_alloc_sblock(void)
{
	t_superblock_ptr sb;

	sb = ctl->SuperblockAvail;
	if (sb)
		ctl->SuperblockAvail = sb->next;

	return sb;
}

static void
wam_free_sblock(t_superblock_ptr sb)
{
	sb->next = ctl->SuperblockAvail;
	ctl->SuperblockAvail = sb;
}

static volatile void*
wam_desc_alloc(void)
{
	t_desc_ptr desc, next, this;
    int i, max, dsize;

	desc = (t_desc_ptr) ctl->DescAvail;

	if (!desc)
	{
		desc = (t_desc_ptr) wam_alloc_sblock();

		if (desc)
		{
			dsize = ALIGN_SIZE(sizeof(struct t_wam_desc));
			max = USABLE_SUPERBLOCK_SIZE / dsize;

			this = (t_desc_ptr) (((char*) desc) + dsize * (max - 1));
			this->next = NULL;
#ifdef MAGIC
			this->magic = MAGIC_VALUE;
#endif
			for (i = max - 2; i >= 0; i--)
			{
				this = (t_desc_ptr) (((char*) desc) + dsize * (i));
				next = (t_desc_ptr) (((char*) desc) + dsize * (i+1));

				this->next = next;
#ifdef MAGIC
				this->magic = MAGIC_VALUE;
#endif
			}

		}
	}

	if (desc)
		ctl->DescAvail = desc->next;

	return (void*) desc;
}

static void
wam_partial_list_push(t_sizeclass_ptr sc, t_desc_ptr desc)
{
#ifdef MAGIC
	Assert(desc->magic == MAGIC_VALUE);
#endif

	desc->next = sc->partial_desc_list;
	sc->partial_desc_list = desc;
}

static t_desc_ptr
wam_partial_list_pop(t_sizeclass_ptr sc)
{
	t_desc_ptr desc;

	desc = (t_desc_ptr) sc->partial_desc_list;

#ifdef MAGIC
	if (desc)
		Assert(desc->magic == MAGIC_VALUE);
#endif

	if (desc)	
		sc->partial_desc_list = desc->next;

    return desc;
}

static void
wam_desc_free(volatile void *ptr)
{
	t_desc_ptr	desc = ptr;

#ifdef MAGIC
	Assert(desc->magic == MAGIC_VALUE);
#endif

	desc->next = ctl->DescAvail;
	ctl->DescAvail = desc;
}

static void*
wam_alloc_from_partial(const t_heap_ptr heap)
{
	t_desc_ptr	desc;
    t_anchor	oldanchor;
	intptr_t   *addr;

	SpinLockAcquire(&heap->HeapLock);

	desc = heap->Active;

retry:
	if (!desc)
	{
		SpinLockRelease(&heap->HeapLock);
		return NULL;
	}

	SpinLockAcquire(&desc->DescLock);

#ifdef MAGIC
	Assert(desc->magic == MAGIC_VALUE);
#endif

	if (desc->Anchor.state == STATE_FULL)
	{
		SpinLockAcquire(&ctl->WamLock);
		SpinLockRelease(&desc->DescLock);

		desc = wam_partial_list_pop(heap->sc);
		heap->Active = desc;

		SpinLockRelease(&ctl->WamLock);

		goto retry;
	}
	else if (desc->Anchor.state == STATE_EMPTY)
	{
		SpinLockAcquire(&ctl->WamLock);
		SpinLockRelease(&desc->DescLock);

		wam_desc_free(desc);
		desc = wam_partial_list_pop(heap->sc);
		heap->Active = desc;

		SpinLockRelease(&ctl->WamLock);

		goto retry;
	}

	/* release the heap lock as early as possible */
	SpinLockRelease(&heap->HeapLock);

	desc->heap = heap;

	oldanchor.value = desc->Anchor.value;

	Assert(desc->Anchor.state == STATE_PARTIAL);
	Assert(desc->Anchor.count > 0);

	desc->Anchor.count -= 1;
	desc->Anchor.state = (desc->Anchor.count > 0 ?
						  STATE_PARTIAL : STATE_FULL);

	Assert(desc->Anchor.avail < desc->maxcount);
	addr = (intptr_t*) ((Pointer) desc->sb +
						desc->Anchor.avail * desc->sz);
	Assert(*addr >= 0);
	Assert(*addr < desc->maxcount);

	desc->Anchor.avail = *addr;
	Assert(desc->Anchor.avail < desc->maxcount);

	SpinLockRelease(&desc->DescLock);

	*addr = (intptr_t) desc;

    return addr + 1;
}

static void*
wam_alloc_from_sblock(const t_heap_ptr heap)
{
    t_desc_ptr		desc;
	unsigned int	i;
	intptr_t	   *addr = NULL;

	SpinLockAcquire(&heap->HeapLock);
	SpinLockAcquire(&ctl->WamLock);

	desc = wam_desc_alloc();
	if (desc)
	{
#ifdef MAGIC
		Assert(desc->magic == MAGIC_VALUE);
#endif

		desc->heap = heap;
		Assert(IS_ALIGNED(heap->sc->sz));
		desc->sz = heap->sc->sz;
		desc->maxcount = USABLE_SUPERBLOCK_SIZE / desc->sz;
		Assert(desc->maxcount < MAX_BLOCK_COUNT);
		Assert(desc->maxcount > 2);

		desc->Anchor.avail = 1;
		desc->Anchor.count = desc->maxcount - 1;

		desc->Anchor.state = STATE_PARTIAL;

		/* allocate a superblock and initialize it */
		desc->sb = wam_alloc_sblock();

		if (desc->sb)
		{
			/* organize blocks in a linked list starting with index 0 */
			i = 0;
			addr = (intptr_t*) desc->sb;
			while (1)
			{
				i++;
				if (i < desc->maxcount)
				{
					*addr = i;
					addr += desc->sz / sizeof(Pointer);
				}
				else
				{
					*addr = 0;
					break;
				}
			}
			
			addr = (intptr_t*) desc->sb;
			*addr = (intptr_t) desc;

			if (heap->Active)
				wam_partial_list_push(heap->sc, desc);
			else
				heap->Active = desc;

			addr += 1;
		}
		else
			wam_desc_free(desc);
	}

	SpinLockRelease(&ctl->WamLock);
	SpinLockRelease(&heap->HeapLock);

	return addr;
}

void*
wam_alloc(size_t sz)
{
	t_heap_ptr	heap;
	Pointer     addr = NULL;

#ifdef MAGIC
	sz += 2 * sizeof(MAGIC_TYPE);
#endif

	heap = find_heap(sz);
	Assert(heap);

	START_CRIT_SECTION();
	{
		addr = wam_alloc_from_partial(heap);
		if (!addr)
			addr = wam_alloc_from_sblock(heap);
	}
	END_CRIT_SECTION();

#ifdef MAGIC
	*((MAGIC_TYPE*) addr) = MAGIC_VALUE;
	*((MAGIC_TYPE*) (addr + heap->sc->sz - sizeof(Pointer)
						  - sizeof(MAGIC_TYPE))) = MAGIC_VALUE;
	addr += sizeof(MAGIC_TYPE);
#endif

	if (!addr)
		elog(WARNING, "out of dynamic shared memory!");

	return addr;
}

void
wam_free(void* ptr)
{
	t_desc_ptr	desc;
	t_heap_ptr	heap = NULL;
	t_anchor	anchor, oldanchor;
	t_superblock_ptr freeable_sb = NULL;
	Pointer		addr;

	Assert(ptr);

    addr = (Pointer) ptr;

#ifdef MAGIC
	addr -= sizeof(MAGIC_TYPE);
	Assert(*((MAGIC_TYPE*) addr) == MAGIC_VALUE);
#endif

	addr -= sizeof(Pointer);
	desc = *((t_desc_ptr*) addr);

	Assert(desc->sb);
	/* liyu: based on same reason in wam_check */
    /* Assert((intptr_t) desc > 0xff); */

	START_CRIT_SECTION();
	{
		SpinLockAcquire(&desc->DescLock);

#ifdef MAGIC
		Assert(desc->magic == MAGIC_VALUE);
		Assert(*((MAGIC_TYPE*)(addr + desc->heap->sc->sz
									- sizeof(MAGIC_TYPE))) == MAGIC_VALUE);
#endif

        Assert(desc->Anchor.count < desc->maxcount);

        *((intptr_t*) addr) = desc->Anchor.avail;
		oldanchor.value = desc->Anchor.value;
        if (desc->Anchor.count == desc->maxcount - 1)
        {
			Assert(desc->Anchor.state == STATE_PARTIAL);

            desc->Anchor.avail = 0;        /* just here for the compiler */
            desc->Anchor.count = 0;        /* just here for the compiler */
            desc->Anchor.state = STATE_EMPTY;

			freeable_sb = desc->sb;
			desc->sb = NULL;
        }
        else
        {
			heap = desc->heap;

            desc->Anchor.avail = (addr - (Pointer) desc->sb) / desc->sz;
			Assert(desc->Anchor.avail < desc->maxcount);
            desc->Anchor.count++;
			Assert(desc->Anchor.count < desc->maxcount);
            desc->Anchor.state = STATE_PARTIAL;
        }

		anchor.value = desc->Anchor.value;

		SpinLockRelease(&desc->DescLock);

		if (freeable_sb)
		{
			Assert(anchor.state == STATE_EMPTY);

			SpinLockAcquire(&ctl->WamLock);
			wam_free_sblock(freeable_sb);
			SpinLockRelease(&ctl->WamLock);
		}
		else if (oldanchor.state == STATE_FULL)
		{
			Assert(heap != NULL);

			SpinLockAcquire(&ctl->WamLock);
			wam_partial_list_push(heap->sc, desc);
			SpinLockRelease(&ctl->WamLock);
		}
	}
	END_CRIT_SECTION();
}

void
wam_check(void *ptr)
{
    Pointer		addr;
	t_desc_ptr	desc;

	Assert(ptr);

    addr = (Pointer) ptr;

#ifdef MAGIC
	addr -= sizeof(MAGIC_TYPE);
	Assert(*((MAGIC_TYPE*) addr) == MAGIC_VALUE);
#endif

	addr -= sizeof(Pointer);
	desc = *((t_desc_ptr*) addr);

	/* liyu: desc is a pointer point to next imsg. This desc is owned
	   by previous msg so its value should not be some real value, not
	   just 0xff.

	   Anyway, what's the meaning of > 0xff ? Seems meaningless...
	 */
	/* Assert((intptr_t) desc > 0xff); */
	Assert(desc);

#ifdef MAGIC
	Assert(*((MAGIC_TYPE*)(addr + desc->heap->sc->sz
								- sizeof(MAGIC_TYPE))) == MAGIC_VALUE);
	Assert(desc->magic == MAGIC_VALUE);
#endif

	Assert(desc->Anchor.avail < desc->maxcount);
	Assert(desc->Anchor.count < desc->maxcount);
}

int
wam_size(void)
{
	/* FIXME: correct size calculation */
	return MAXALIGN(sizeof(struct t_wam_control) +
		sizeof(struct t_wam_sizeclass) * NUMBER_OF_SIZECLASSES +
		sizeof(struct t_wam_heap) * NUMBER_OF_HEAPS * NUMBER_OF_SIZECLASSES +
		4 * POINTER_MASK +
		ShmemDynBufferSize);
}

void
wam_init()
{
	Pointer				ptr, shmem;
	t_superblock_ptr	sb;
	int					i, j, size;
	bool				found;

	size = wam_size();
	ptr = shmem = ShmemInitStruct("ShmemDynAllocCtl", size, &found);

	if (found)
		return;

	ctl = (t_control_ptr) ptr;
	ptr += sizeof(struct t_wam_control);
	ptr = ALIGN_POINTER(ptr);

	/*
	 * Create a useful set of sizeclasses.
	 */
	ctl->sizeclasses = (t_sizeclass_ptr) ptr;
	ctl->sizeclasses[0].partial_desc_list = NULL;
	ctl->sizeclasses[0].sz = 1 << POINTER_ALIGNMENT;
	for (i = 1; i < NUMBER_OF_SIZECLASSES; i++)
	{
		ctl->sizeclasses[i].partial_desc_list = NULL;
		ctl->sizeclasses[i].sz = ctl->sizeclasses[i-1].sz * 2;

		Assert(IS_ALIGNED(ctl->sizeclasses[i].sz));

		Assert(USABLE_SUPERBLOCK_SIZE / ctl->sizeclasses[i].sz <
			   MAX_BLOCK_COUNT);
	}
	ptr += sizeof(struct t_wam_sizeclass) * NUMBER_OF_SIZECLASSES;
	ptr = ALIGN_POINTER(ptr);

	/*
	 * Initialize the global heap control structures
	 */
	ctl->global_heap = (t_heap_ptr) ptr;
	for (i = 0; i < NUMBER_OF_SIZECLASSES; i++)
	{
		for (j = 0; j < NUMBER_OF_HEAPS; j++)
		{
			t_heap_ptr heap = &ctl->global_heap[i * NUMBER_OF_HEAPS + j];

			SpinLockInit(&heap->HeapLock);
			heap->Active = NULL;
			heap->sc = &ctl->sizeclasses[i];
		}
	}
	ptr += sizeof(struct t_wam_heap) *
		NUMBER_OF_HEAPS * NUMBER_OF_SIZECLASSES;
	ptr = ALIGN_POINTER(ptr);

	/*
	 * Initialize the stack for parking available descriptors.
	 */
	ctl->DescAvail = NULL;
	SpinLockInit(&ctl->WamLock);

	/*
	 * Organize the remaining space of shared memory into a singly linked
	 * lits of superblocks of size SUPERBLOCK_SIZE.
	 */
	ptr = ALIGN_POINTER(ptr);

	ctl->SuperblockAvail = NULL;
	while ((ptr - shmem) < (size - SUPERBLOCK_SIZE))
	{
		sb = (t_superblock_ptr) ptr;
		ptr += SUPERBLOCK_SIZE;

		Assert(IS_ALIGNED(ptr));

		/* push onto the stack */
		sb->next = ctl->SuperblockAvail;
		ctl->SuperblockAvail = sb;

		i++;
	}
}
