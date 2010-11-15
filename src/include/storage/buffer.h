/*-------------------------------------------------------------------------
 *
 * buffer.h
 *
 *    Byte order and maximum message size aware buffer handling
 *    functions for communication.
 *
 * Copyright (c) 2006-2010, PostgreSQL Global Development Group
 * 
 *-------------------------------------------------------------------------
 */

#ifndef _BUFFSOCK_H_
#define _BUFFSOCK_H_

#include <string.h>
#include <netinet/in.h>

#include "postgres.h"

#define get_bytes_read(BUFFER) ((BUFFER)->fill_size - (BUFFER)->ptr)
#define BUFFER_POS(buffer) ((Pointer)((Pointer)((buffer)->data) + (buffer)->ptr))

typedef void* (alloc_func) (size_t size);
typedef void (buffer_fill_func) (void *buf, size_t min, const void *obj);

typedef struct
{
	void *data;
	int ptr;
	int max_size;		/* memory allocated for the buffer */
	int fill_size;		/* fill status of buffer, read until there */
} buffer;

extern void init_buffer(buffer *b, void *data, int size);
extern uint8_t get_int8(buffer *b);
extern uint16_t get_int16(buffer *b);
extern uint32_t get_int32(buffer *b);
extern uint64_t get_int64(buffer *b);
extern char *get_pstring(buffer *b);
extern char *get_p32string(buffer *b);
extern char *get_cstring(buffer *b);
extern void get_pstring_into(buffer *b, char *dest, int max_size);
extern void get_p32string_into(buffer *b, char *dest, int max_size);
extern void get_cstring_into(buffer *b, char *dest, int max_size);
extern void get_data(buffer *b, void *dest, int size);
extern void put_int8(buffer *b, const uint8_t val);
extern void put_int16(buffer *b, const uint16_t val);
extern void put_int32(buffer *b, const uint32_t val);
extern void put_int64(buffer *b, const uint64_t val);
extern void put_pstring(buffer *b, const char *str);
extern void put_p32string(buffer *b, const char *str);
extern void put_cstring(buffer *b, const char *str);
extern void put_data(buffer *b, const void *data, const int size);
extern void put_data_from_buffer(buffer *dest, const buffer *src);

#endif		// _BUFFSOCK_H_
