/*-------------------------------------------------------------------------
 *
 * istream.c
 *
 *	  Internal streaming for process to process communication via
 *	  imessages.
 *
 * Copyright (c) 2010, Translattice, Inc
 *
 *-------------------------------------------------------------------------
 */

#include <unistd.h>
#include <signal.h>
#include <string.h>

#include "postgres.h"
#include "miscadmin.h"
#include "storage/buffer.h"
#include "storage/imsg.h"
#include "storage/istream.h"


static void istream_prepare_for_reading(IStreamReader reader, int size);
static void istream_flush_write_buffer(IStreamWriter writer);
static void istream_prepare_for_writing(IStreamWriter writer, int size);

void
istream_init_reader(IStreamReader reader,
					istream_parse_header_func *parse_func, IMessage *msg)
{
	Assert(msg);

	/*
	 * Initialize the reader data structure... 
	 */
	reader->msg = msg;
	reader->msg_type = msg->type;
	IMessageGetReadBuffer(&reader->buf, msg);
	reader->parse_header_func = parse_func;

	/*
	 * ..and let the handler parse the header of the first message
	 * received right away.
	 */
	reader->parse_header_func(&reader->buf);
}

/* FIXME: duplicated method... */
static IMessage*
await_imessage()
{
	IMessage   *msg;

	while (!(msg = IMessageCheck()))
	{
		ImmediateInterruptOK = true;
		CHECK_FOR_INTERRUPTS();

		pg_usleep(1000000L);

		elog(DEBUG5, "bg worker [%d/%d]: istream: waiting for next imessage",
			 MyProcPid, MyBackendId);

		ImmediateInterruptOK = false;

#if 0
		if (MyProc->abortFlag)
			break;
#endif
	}

	return msg;
}

static void
istream_prepare_for_reading(IStreamReader reader, int size)
{
	int bytes_remaining = get_bytes_read(&reader->buf);

	Assert(reader->msg);

	if (bytes_remaining < size)
	{
		/*
		 * this shouldn't normally happen, but is a good sign for a
		 * mismatch in the expected message contents.
		 */
		if (bytes_remaining > 0)
			elog(WARNING, "bg worker [%d/%d]: istream: skipped %d bytes",
				 MyProcPid, MyBackendId, bytes_remaining);
//		Assert(bytes_remaining == 0);

		IMessageRemove(reader->msg);
		reader->msg = NULL;

		elog(DEBUG5, "bg worker [%d/%d]: istream: waiting for next imessage (remaining_bytes: %d, req size: %d)",
			 MyProcPid, MyBackendId, bytes_remaining, size);

		/* FIXME: wait should be able to time out or something */
		reader->msg = await_imessage();
		Assert(reader->msg);

		/* FIXME: error detection */
		if (reader->msg->type != reader->msg_type)
			elog(WARNING, "bg worker [%d/%d]: unexpected message type: %s",
				 MyProcPid, MyBackendId,
				 decode_imessage_type(reader->msg->type));
		Assert(reader->msg->type == reader->msg_type);

		IMessageGetReadBuffer(&reader->buf, reader->msg);

		/* let the istream user parse the message header */
		reader->parse_header_func(&reader->buf);
	}

	Assert(reader->msg != NULL);
	Assert(get_bytes_read(&reader->buf) >= size);
}

int8
istream_read_int8(IStreamReader reader)
{
	istream_prepare_for_reading(reader, sizeof(int8));
	return get_int8(&reader->buf);
}

int32
istream_read_int32(IStreamReader reader)
{
	istream_prepare_for_reading(reader, sizeof(int32));
	return get_int32(&reader->buf);
}

char *
istream_read_pstring(IStreamReader reader)
{
	int size;
	char *str;

	size = istream_read_int8(reader);
	str = palloc(size + 1);
	istream_read_data(reader, str, size);
	str[size] = 0;
	Assert(strlen(str) == size);
	return str;
}

void
istream_read_data(IStreamReader reader, void *ptr, int size)
{
	buffer *b = &reader->buf;
	Pointer rest = ptr;
	int avail;

	while (size > 0)
	{
#ifdef USE_ASSERT_CHECKING
		/* prepare for reading at least one byte */
		istream_prepare_for_reading(reader, 5);

		/*
		 * Read the remaining bytes from the buffer, or up to as many as
		 * still required by the specified data size.
		 */
		avail = get_bytes_read(b) - 4;
#else
		istream_prepare_for_reading(reader, 1);
		avail = get_bytes_read(b);
#endif
		if (size < avail)
			avail = size;

		get_data(b, rest, avail);
		size -= avail;
		rest += avail;

		if (size > 0)
			elog(DEBUG5, "bg worker [%d/%d]: istream: got data of size %d (remaining %d)",
				 MyProcPid, MyBackendId, avail, size);

#ifdef USE_ASSERT_CHECKING
		/* liyu: the attached string size will only be double checked
		 * which is set by istream_write_data. This should be DEBUG
		 * only. */
		Assert(get_int32(b) == size);
		Assert((get_bytes_read(b) == 0) || (size == 0));
#endif
	}
}

void
istream_close_reader(IStreamReader reader)
{
	if (reader->msg)
		IMessageRemove(reader->msg);
	reader->msg = NULL;
}


void
istream_init_writer(IStreamWriter writer, IMessageType msg_type,
					BackendId target_backend_id, int size_limit,
					istream_prepend_header_func *prep_func)
{
	writer->msg = NULL;
	writer->msg_type = msg_type;
	writer->target_backend_id = target_backend_id;
	writer->size_limit = size_limit;
	writer->buf.data = NULL;
	writer->prepend_header_func = prep_func;
}

static void
istream_flush_write_buffer(IStreamWriter writer)
{
	Assert(writer->msg);

#ifdef USE_PRIMALLOC
	/* FIXME: bad hack */
	if (writer->msg->size != writer->buf.fill_size)
	{
		void *data;
		IMessage *new_msg;

		data = palloc(writer->buf.fill_size);
		memcpy(data, writer->buf.data, writer->buf.fill_size);
		IMessageRemove(writer->msg);

		new_msg = IMessageCreate(writer->msg_type, writer->buf.fill_size);
		memcpy(IMSG_DATA(new_msg), data, writer->buf.fill_size);
		writer->msg = new_msg;
	}
#else
	/* trim the message a bit */
	/* FIMXE: the imessage API hopefully supports that */
	writer->msg->size = writer->buf.fill_size;
#endif

	IMessageActivate(writer->msg, writer->target_backend_id);
	writer->msg = NULL;
}

static void
istream_prepare_for_writing(IStreamWriter writer, int size)
{
	buffer *b = &writer->buf;

	if (b->max_size - b->ptr < size && writer->msg != NULL)
		istream_flush_write_buffer(writer);

	if (writer->msg == NULL)
	{
		writer->msg = IMessageCreate(writer->msg_type, writer->size_limit);
		IMessageGetWriteBuffer(&writer->buf, writer->msg);
		writer->prepend_header_func(&writer->buf);
	}

	Assert(b->max_size - b->ptr >= size);

#ifdef MAGIC
	Assert(writer->msg->magic == MAGIC_VALUE);
	ShmemDynCheck(writer->msg);
#endif
}

void
istream_write_int8(IStreamWriter writer, int8 value)
{
	istream_prepare_for_writing(writer, sizeof(int8));
	put_int8(&writer->buf, value);
}

void
istream_write_int32(IStreamWriter writer, int32 value)
{
	istream_prepare_for_writing(writer, sizeof(int32));
	put_int32(&writer->buf, value);
}

void
istream_write_pstring(IStreamWriter writer, const char *str)
{
	int size = strlen(str);

	istream_write_int8(writer, size);
	istream_write_data(writer, str, size);
}

void
istream_write_data(IStreamWriter writer, const void *ptr, int size)
{
	buffer *b = &writer->buf;
	const char *rest = ptr;
	int avail;

	while (size > 0)
	{
#ifdef USE_ASSERT_CHECKING
		/* prepare for writing at least one byte */
		istream_prepare_for_writing(writer, 5);

		/* fill the current buffer (i.e. message) */
		avail = b->max_size - b->ptr - 4;
#else
		istream_prepare_for_writing(writer, 1);
		avail = b->max_size - b->ptr;
#endif
		if (size < avail)
			avail = size;

		put_data(b, rest, avail);
		size -= avail;
		rest += avail;

		if (size > 0)
			elog(DEBUG5, "bg worker [%d/%d]: istream: put data of size %d (remaining %d)",
				 MyProcPid, MyBackendId, avail, size);

#ifdef USE_ASSERT_CHECKING
		/* liyu: the attached string size will only be double checked
		 * when istream_read_data. This should be DEBUG only, so wrap
		 * it by Assert. */
		put_int32(b, size);
		Assert((b->max_size == b->ptr) || (size == 0));
#endif
	}
}

void
istream_close_writer(IStreamWriter writer)
{
	if (writer->msg)
		istream_flush_write_buffer(writer);
}
