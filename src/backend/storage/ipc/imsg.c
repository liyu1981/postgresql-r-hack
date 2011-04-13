/*-------------------------------------------------------------------------
 *
 * imsg.c
 *
 *	  Internal message passing for process to process communication
 *    via shared memory.
 *
 * Portions Copyright (c) 2010, Translattice, Inc
 * Portions Copyright (c) 2005-2010, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include <unistd.h>
#include <signal.h>
#include <string.h>

#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif

#include <sys/ioctl.h>

#include "postgres.h"
#include "miscadmin.h"
#include "storage/buffer.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/imsg.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/elog.h"

/* global variable pointing to the shmem area */
IMessageCtlData *IMessageCtl = NULL;

/*
 * flag set by the signal handler, initialized to true to ensure
 * IMessageCheck is called at least once after process startup.
 */
static bool got_IMessage = false;

void printIMessageCtlInfo();


void
printIMessageCtlInfo()
{
	int i, c;
	Deque *q;
	DequeElem *p;

	for (i=0; i<MaxBackends; ++i) {
		q = &IMessageCtl->lists[i];
		c = 0;
		p = q->head.next;
		while (p != &(q->head)) {
			c += 1;
			p = p->next;
		}

		if (c > 0) {
			elog(LOG, "IMessage queue for backend %d not empty (%d children)", i, c);
		}
	}
}

/*
 * Initialization of shared memory for internal messages.
 */
int
IMessageShmemSize(void)
{
	return MAXALIGN(sizeof(IMessageCtlData) +
					sizeof(Deque) * (MaxBackends - 1));
}

void
IMessageShmemInit(void)
{
	bool		foundIMessageCtl;
	int         i;

#ifdef IMSG_DEBUG
	elog(DEBUG3, "IMessageShmemInit(): initializing shared memory");
#endif

	IMessageCtl = (IMessageCtlData *)
		ShmemInitStruct("IMsgCtl", IMessageShmemSize(),
						&foundIMessageCtl);

	if (foundIMessageCtl)
		return;

	/* initialize the per-backend message sink */
	for (i = 0; i < MaxBackends; i++)
		deque_init(&IMessageCtl->lists[i]);
}

char *
decode_imessage_type(const IMessageType msg_type)
{
	char *str;

	switch (msg_type)
	{
		case IMSGT_TEST:
			return "IMSGT_TEST";

		case IMSGT_REGISTER_WORKER:
			return "IMSGT_REGISTER_WORKER";
		case IMSGT_TERM_WORKER:
			return "IMSGT_TERM_WORKER";
		case IMSGT_READY:
			return "IMSGT_READY";

		case IMSGT_PERFORM_VACUUM:
			return "IMSGT_PERFORM_VACUUM";
		case IMSGT_FORCE_VACUUM:
			return "IMSGT_FORCE_VACUUM";

		case IMSGT_SEED:
			return "IMSGT_SEED";
		case IMSGT_REGISTER_BACKEND:
			return "IMSGT_REGISTER_BACKEND";
		case IMSGT_UNREGISTER_BACKEND:
			return "IMSGT_UNREGISTER_BACKEND";
		case IMSGT_DB_STATE:
			return "IMSGT_DB_STATE";
		case IMSGT_ORDERING:
			return "IMSGT_ORDERING";
		case IMSGT_TXN_ABORTED:
			return "IMSGT_TXN_ABORTED";
		case IMSGT_CSET:
			return "IMSGT_CSET";
		case IMSGT_SEQ_INCREMENT:
			return "IMSGT_SEQ_INCREMENT";
		case IMSGT_SEQ_SETVAL:
			return "IMSGT_SEQ_SETVAL";
		case IMSGT_RECOVERY_RESTART:
			return "IMSGT_RECOVERY_RESTART";
		case IMSGT_SCHEMA_ADAPTION:
			return "IMSGT_SCHEMA_ADAPTION";
		case IMSGT_RECOVERING_SCHEMA:
			return "IMSGT_RECOVERING_SCHEMA";
		case IMSGT_RECOVERY_DATA:
			return "IMSGT_RECOVERY_DATA";
		case IMSGT_RECOVERY_REQUEST:
			return "IMSGT_RECOVERY_REQUEST";
		case IMSGT_RELATION_RECOVERED:
			return "IMSGT_RELATION_RECOVERED";

		default:
			str = palloc0(255);
			snprintf(str, 254, "unknown message type: %d", msg_type);
			return str;
	}
}

/*
 *   IMessageCreateInternal
 *
 * Creates a new but deactivated message within the queue, returning the
 * message header of the newly created message or NULL if there's not enough
 * space in shared memory for a message of the requested size.
 */
static IMessage*
IMessageCreateInternal(IMessageType type, int msg_size)
{
	IMessage *msg;

	msg = (IMessage*) ShmemDynAlloc(sizeof(IMessage) + msg_size);

	if (msg)
	{
#ifdef MAGIC
		msg->magic = MAGIC_VALUE;
		ShmemDynCheck(msg);
#endif

		msg->sender = InvalidBackendId;
		msg->type = type;
		msg->size = msg_size;

#ifdef IMSG_DEBUG
		elog(DEBUG3, "IMessageCreateInternal(): created message type %s of size %d at %p",
			 decode_imessage_type(type), (int) msg_size, msg);
#endif
	}

	return msg;
}

/*
 *   IMessageCreate
 *
 * Creates a new but deactivated message within the queue, returning the
 * message header of the newly created message. Blocks until there is
 * enough space available for the message in shared memory, retrying every
 * 100ms.
 *
 * FIXME: this is not the best way to handle out of (imessage) memory, as
 *        the process wanting to create an IMessage may well receive more
 *        imessages while it is waiting to create a new one (previously
 *        created, but not activated ones). However, for most callers this
 *        would mean having to cache the message in local memory until its
 *        deliverable.
 */
IMessage*
IMessageCreate(IMessageType type, int msg_size)
{
	IMessage *msg;

	while (!(msg = IMessageCreateInternal(type, msg_size)))
	{
		elog(WARNING, "imessage: waiting for %d bytes to be freed",
			 (int) sizeof(IMessage) + msg_size);

		printIMessageCtlInfo();

		/* elog(PANIC, "we should stop here!"); */

		pg_usleep(100000);
	}

	return msg;
}

int
IMessageActivate(IMessage *msg, BackendId recipient)
{
	Assert(msg);
	Assert(recipient >= 0);
	Assert(recipient < MaxBackends);
#ifdef MAGIC
	Assert(msg->magic == MAGIC_VALUE);
	ShmemDynCheck(msg);
#endif

#ifdef IMSG_DEBUG
	elog(DEBUG3, "IMessageActivate(): activating message of type %s and size %d for recipient %d",
		 decode_imessage_type(msg->type), msg->size, recipient);
#endif

	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		IMessageCtlData *imsgctl = IMessageCtl;
		Deque *list = &imsgctl->lists[recipient];

		SpinLockAcquire(&list->lock);

		msg->sender = MyBackendId;

		deque_enqueue(list, &msg->list);

		SpinLockRelease(&list->lock);
	}
	END_CRIT_SECTION();

	return SendProcSignalById(PROCSIG_IMSG_INTERRUPT, recipient);
}

/*
 *   IMessageRemove
 *
 * Marks a message as removable by setting the recipient to null. The message
 * will eventually be removed during creation of new messages, see
 * IMessageCreate().
 */
void
IMessageRemove(IMessage *msg)
{
	Assert(msg);
#ifdef MAGIC
	Assert(msg->magic == MAGIC_VALUE);
#endif

	ShmemDynFree((Pointer) msg);
}

/*
 *    HandleIMessageInterrupt
 *
 * Called on PROCSIG_IMSGT_INTERRUPT, possibly within the signal handler.
 */
void
HandleIMessageInterrupt()
{
	got_IMessage = true;
}

/*
 *   IMessageCheck
 *
 * Checks if there is a message in the queue for this process. Returns null
 * if there is no message for this process, the message header otherwise. The
 * message remains in the queue and should be removed by IMessageRemove().
 */
IMessage*
IMessageCheck(void)
{
	IMessage	   *msg;

	/* short circuit in case we didn't receive a signal */
	if (!got_IMessage)
		return NULL;

#ifdef IMSG_DEBUG
	elog(DEBUG3, "IMessageCheck(): backend %d (pid %d) got imsg interrupt",
		 MyBackendId, MyProc->pid);
#endif

	msg = NULL;
	START_CRIT_SECTION();
	{
		/* use volatile pointer to prevent code rearrangement */
		IMessageCtlData *imsgctl = IMessageCtl;
		Deque *list = &imsgctl->lists[MyBackendId];

		SpinLockAcquire(&list->lock);

		msg = (IMessage*) deque_dequeue(list);

		SpinLockRelease(&list->lock);
	}
	END_CRIT_SECTION();

	/*
	 * Reset the flag, if we scanned through the list but didn't find any
	 * new message.
	 */
	if (msg == NULL)
		got_IMessage = false;

#ifdef MAGIC
	if (msg != NULL)
	{
		Assert(msg->magic == MAGIC_VALUE);
		ShmemDynCheck(msg);
	}
#endif

#ifdef IMSG_DEBUG
	if (msg != NULL)
		elog(DEBUG3, "IMessageCheck(): new message at %p of type %s and size %d for [%d/%d]",
			 msg, decode_imessage_type(msg->type), msg->size,
			 MyProcPid, MyBackendId);
#endif

	return msg;
}

#ifdef REPLICATION
/*
 *   IMessageGetReadBuffer
 *
 * gets a readable buffer for the given message
 */
void
IMessageGetReadBuffer(buffer *b, const IMessage *msg)
{
	Assert(msg);
	Assert(msg->size >= 0);
#ifdef MAGIC
	Assert(msg->magic == MAGIC_VALUE);
#endif

	init_buffer(b, IMSG_DATA(msg), msg->size);
	b->fill_size = msg->size;
}

/*
 *   IMessageGetWriteBuffer
 *
 * gets a writeable buffer for the given message
 */
void
IMessageGetWriteBuffer(buffer *b, const IMessage *msg)
{
	Assert(msg);
#ifdef MAGIC
	Assert(msg->magic == MAGIC_VALUE);
#endif

	init_buffer(b, IMSG_DATA(msg), msg->size);
}
#endif
