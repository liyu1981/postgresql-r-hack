/*-------------------------------------------------------------------------
 *
 * buffer.c
 *
 *	  Byte order and maximum message size aware buffer handling.
 *
 * Copyright (c) 2005-2010, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include <endian.h>
#include <string.h>
#include <netinet/in.h>


#include "postgres.h"
#include "miscadmin.h"
#include "storage/buffer.h"

void
init_buffer(buffer *b, void *data, int size)
{
	b->data = data;
	b->ptr = 0;
	b->max_size = size;
	b->fill_size = 0;
}

uint8_t
get_int8(buffer *b)
{
	int8_t res;

	Assert(b->ptr + sizeof(uint8_t) <= b->fill_size);
	res = *((uint8_t*) BUFFER_POS(b));
	b->ptr += sizeof(uint8_t);
	return res;
}

uint16_t
get_int16(buffer *b)
{
	int16_t res;

	Assert(b->ptr + sizeof(uint16_t) <= b->fill_size);
	res = *((uint16_t*) BUFFER_POS(b));
	b->ptr += sizeof(uint16_t);
	return res;
}

uint32_t
get_int32(buffer *b)
{
	uint32_t res;

	Assert(b->ptr + sizeof(uint32_t) <= b->fill_size);
	res = *((uint32_t*) BUFFER_POS(b));
	b->ptr += sizeof(uint32_t);
	
#if __BYTE_ORDER == __LITTLE_ENDIAN
	return __bswap_32(res);
#elif __BYTE_ORDER == __BIG_ENDIAN
	return res;
#else
#error __BYTE_ORDER not specified!
#endif
}

uint64_t
get_int64(buffer *b)
{
	uint64_t res;

	Assert(b->ptr + sizeof(uint64_t) <= b->fill_size);
	res = *((uint64_t*) BUFFER_POS(b));
	b->ptr += sizeof(uint64_t);
	
#if __BYTE_ORDER == __LITTLE_ENDIAN
	return __bswap_64(res);
#elif __BYTE_ORDER == __BIG_ENDIAN
	return res;
#else
#error __BYTE_ORDER not specified!
#endif
}

char *
get_pstring(buffer *b)
{
	int size;
	char *res;

	Assert(b->ptr + sizeof(uint8_t) <= b->fill_size);
	size = *((char*) BUFFER_POS(b));
	Assert(b->ptr + sizeof(uint8_t) + size <= b->fill_size);
	res = palloc(size + 1);
	b->ptr += sizeof(char);
	memcpy(res, (void*) BUFFER_POS(b), size);
	res[size] = 0;
	b->ptr += size;
	return res;
}

void
get_pstring_into(buffer *b, char *dest, int max_size)
{
	int size;

	Assert(b->ptr + sizeof(uint8_t) <= b->fill_size);
	size = *((char*) BUFFER_POS(b));
	Assert(b->ptr + sizeof(uint8_t) + size <= b->fill_size);
	Assert(size + 1 <= max_size);
	b->ptr += sizeof(char);
	memcpy(dest, BUFFER_POS(b), size);
	dest[size] = 0;
	b->ptr += size;
}

char *
get_p32string(buffer *b)
{
	int size;
	char *res;

	size = get_int32(b);
	Assert(b->ptr + size <= b->fill_size);
	res = palloc(size + 1);
	memcpy(res, BUFFER_POS(b), size);
	res[size] = 0;
	b->ptr += size;
	return res;
}

void
get_p32string_into(buffer *b, char *dest, int max_size)
{
	int size;

	size = get_int32(b);
	Assert(b->ptr + sizeof(uint32_t) + size <= b->fill_size);
	Assert(size + 1 <= max_size);
	memcpy(dest, (void*) BUFFER_POS(b), size);
	dest[size] = 0;
	b->ptr += size;
}

char *
get_cstring(buffer *b)
{
	int size;
	char *res;

	size = strlen((char *) BUFFER_POS(b));
	Assert(b->ptr + size <= b->fill_size);
	res = palloc(size + 1);
	memcpy(res, (void*) BUFFER_POS(b), size);
	res[size] = 0;
	b->ptr += size + 1;
	return res;
}

void
get_cstring_into(buffer *b, char *dest, int max_size)
{
	int size;

	size = strlen((char *) BUFFER_POS(b));
	Assert(size <= max_size);
	Assert(b->ptr + size <= b->fill_size);
	memcpy(dest, (void*) BUFFER_POS(b), size);
	dest[size] = 0;
	b->ptr += size + 1;
}

void
get_data(buffer *b, void *dest, int size)
{
	Assert(b->ptr + size <= b->fill_size);
	memcpy(dest, (void*) BUFFER_POS(b), size);
	b->ptr += size;
}

void
put_int8(buffer *b, const uint8_t val)
{
	Assert(b->ptr + sizeof(uint8_t) <= b->max_size);
	*((uint8_t*) BUFFER_POS(b)) = val;
	b->ptr += sizeof(uint8_t);
	b->fill_size += sizeof(uint8_t);
}

void
put_int16(buffer *b, const uint16_t val)
{
	Assert(b->ptr + sizeof(uint16_t) <= b->max_size);
	*((uint16_t*) BUFFER_POS(b)) = val;
	b->ptr += sizeof(uint16_t);
	b->fill_size += sizeof(uint16_t);
}

void
put_int32(buffer *b, const uint32_t val)
{
	Assert(b->ptr + sizeof(uint32_t) <= b->max_size);

#if __BYTE_ORDER == __LITTLE_ENDIAN
	*((uint32_t*) BUFFER_POS(b)) = __bswap_32(val);
#elif __BYTE_ORDER == __BIG_ENDIAN
	*((uint32_t*) BUFFER_POS(b)) = val;
#else
#error __BYTE_ORDER not specified!
#endif
	b->ptr += sizeof(uint32_t);
	b->fill_size += sizeof(uint32_t);
}

void
put_int64(buffer *b, const uint64_t val)
{
	/* dirty workaround */
	union {
		uint64_t i64;
		struct
		{
			uint32_t lo;
			uint32_t hi;
		} i32;
	} my_int64;

	my_int64.i64 = val;

	Assert(sizeof(uint64_t) == 8);
	Assert(b->ptr + sizeof(uint64_t) <= b->max_size);

	put_int32(b, my_int64.i32.hi);
	put_int32(b, my_int64.i32.lo);
}

void
put_pstring(buffer *b, const char *str)
{
	int size = strlen(str);

	Assert(b->ptr + size + 1 <= b->max_size);
	*((uint8_t*) BUFFER_POS(b)) = size;
	b->ptr += sizeof(uint8_t);
	b->fill_size += sizeof(uint8_t);
	memcpy((void*) BUFFER_POS(b), str, size);
	b->ptr += size;
	b->fill_size += size;
}

void
put_p32string(buffer *b, const char *str)
{
	int size = strlen(str);

	put_int32(b, size);
	Assert(b->ptr + size + 1 <= b->max_size);
	memcpy((void*) BUFFER_POS(b), str, size);
	b->ptr += size;
	b->fill_size += size;
}

void
put_cstring(buffer *b, const char *str)
{
	int size = strlen(str);

	Assert(b->ptr + size + 1 <= b->max_size);
	memcpy((void*) BUFFER_POS(b), str, size);
	b->ptr += size;
	b->fill_size += size;

	*((uint8_t*) BUFFER_POS(b)) = 0;
	b->ptr += 1;
	b->fill_size += 1;
}

void
put_data(buffer *b, const void *data, const int size)
{
	Assert(b->ptr + size <= b->max_size);
	memcpy((void*) BUFFER_POS(b), data, size);
	b->ptr += size;
	b->fill_size += size;
}

void
put_data_from_buffer(buffer *dest, const buffer *src)
{
	int size;

	size = src->fill_size - src->ptr;
	Assert(dest->ptr + size <= dest->max_size);
	memcpy((void*) BUFFER_POS(dest), (void*) BUFFER_POS(src), size);
	dest->ptr += size;
	dest->fill_size += size;
}

