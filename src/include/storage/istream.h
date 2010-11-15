/*-------------------------------------------------------------------------
 *
 * istream.h
 *
 *	  Internal data streaming for process to process communication via
 *    imessages.
 *
 * Copyright (c) 2010, Translattice, Inc
 *
 *-------------------------------------------------------------------------
 */

#ifndef ISTREAM_H
#define ISTREAM_H

#include <sys/types.h>

#include "c.h"
#include "storage/backendid.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "storage/buffer.h"

/*
 * output stream writer definitions
 */
typedef void (istream_prepend_header_func) (buffer *buf);
typedef void (istream_parse_header_func) (buffer *buf);

/*
 * reader and writer structs
 */
typedef struct
{
	IMessage *msg;
	IMessageType msg_type;
	BackendId target_backend_id;
	int size_limit;
	buffer buf;
	istream_prepend_header_func *prepend_header_func;
} IStreamWriterData;

typedef IStreamWriterData* IStreamWriter;

typedef struct
{
	IMessage *msg;
	IMessageType msg_type;
	buffer buf;
	istream_parse_header_func *parse_header_func;
} IStreamReaderData;

typedef IStreamReaderData* IStreamReader;

/* routines to write to a stream */
extern void istream_init_writer(IStreamWriter writer, IMessageType msg_type,
								BackendId target_backend_id, int size_limit,
								istream_prepend_header_func *prep_func);
extern void istream_write_int8(IStreamWriter writer, int8 value);
extern void istream_write_int32(IStreamWriter writer, int32 value);
extern void istream_write_pstring(IStreamWriter writer, const char *str);
extern void istream_write_data(IStreamWriter writer, const void *ptr,
							   int size);
extern void istream_close_writer(IStreamWriter writer);

/* routines to read from a stream */
extern void istream_init_reader(IStreamReader reader,
								istream_parse_header_func *parse_func,
								IMessage *msg);
extern int8 istream_read_int8(IStreamReader reader);
extern int32 istream_read_int32(IStreamReader reader);
extern char *istream_read_pstring(IStreamReader reader);
extern void istream_read_data(IStreamReader reader, void *ptr, int size);
extern void istream_close_reader(IStreamReader reader);

#endif   /* ISTREAM_H */
