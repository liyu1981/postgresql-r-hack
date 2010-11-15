/*-------------------------------------------------------------------------
 *
 * imsg.h
 *
 *	  Internal message passing for process to process communication
 *    via shared memory.
 *
 * Copyright (c) 2005-2010, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#ifndef IMSG_H
#define IMSG_H

#include <sys/types.h>

#include "c.h"
#include "storage/backendid.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "storage/buffer.h"

/* get a data pointer from the header */
#define IMSG_DATA(imsg) ((Pointer) ((Pointer) imsg + sizeof(IMessage)))

/*
 * Message types
 */
typedef enum
{
	/*
	 * messages from worker to coordinator, for background worker
	 * registration and job management.
	 */
	IMSGT_REGISTER_WORKER = 'W',
	IMSGT_TERM_WORKER = 'M',
	IMSGT_READY = 'r',

	/* inform the coordinator about a database that needs vacuuming to
	 * prevent transaction wrap around, from backends to coordinator */
	IMSGT_FORCE_VACUUM = 'v',

	/* messages from coordinator to worker */
	IMSGT_PERFORM_VACUUM = 'V',
	/* replication: messages from worker backends to the coordinator */
	IMSGT_SEED = 'E',

	IMSGT_REGISTER_BACKEND = 'B',
	IMSGT_UNREGISTER_BACKEND = 'b',

	/* replication: messages between nodes */
	IMSGT_DB_STATE = 'd',
	IMSGT_CSET = 'c',
	IMSGT_ORDERING = 'o',
	IMSGT_TXN_ABORTED = 'a',

	IMSGT_SEQ_INCREMENT = 's',
	IMSGT_SEQ_SETVAL = 't',

	/* replication: messages for schema recovery */
	IMSGT_RECOVERY_RESTART = 'S',
	IMSGT_SCHEMA_ADAPTION = 'A',
	IMSGT_RECOVERING_SCHEMA = 'h',

	/* replication: messages for data recovery */
	IMSGT_RECOVERY_DATA = 'D',
	IMSGT_RECOVERY_REQUEST = 'Q',
	IMSGT_RELATION_RECOVERED = 'R',

	IMSGT_TEST = 'T'				/* test message type */
} IMessageType;

/*
 * Message descriptor in front of the message
 */
typedef struct
{
	DequeElem       list;

#ifdef MAGIC
	uint64          magic;
#endif

	/* backend id of the sender, null means not yet activated message */
	BackendId	    sender;

	/* message type */
	IMessageType    type;

	/* message size following, but not including this header */
	int		    	size;
} IMessage;

/*
 * shared-memory pool for internal messages.
 */
typedef struct
{
	/* a singly linked list per backend */
	Deque lists[1];
} IMessageCtlData;

/* routines to send and receive internal messages */
extern int IMessageShmemSize(void);
extern void IMessageShmemInit(void);
extern IMessage* IMessageCreate(IMessageType type, int msg_size);
extern int IMessageActivate(IMessage *msg, BackendId recipient);
extern void IMessageRemove(IMessage *msg);
extern void HandleIMessageInterrupt(void);
extern IMessage* IMessageCheck(void);

extern void IMessageGetReadBuffer(buffer *b, const IMessage *msg);
extern void IMessageGetWriteBuffer(buffer *b, const IMessage *msg);

/* mainly for debugging purposes */
extern char* decode_imessage_type(const IMessageType msg_type);

#endif   /* IMSG_H */
