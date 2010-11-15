/*-------------------------------------------------------------------------
 *
 * gc_spread.c
 *
 *	  An async interface to the spread toolkit, a group communication
 *    system developed by Spread Concepts LLC.
 *
 * Copyright (c) 2003-2010, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <unistd.h>
#include <errno.h>

#include "postgres.h"
#include "storage/buffer.h"

#include "replication/coordinator.h"
#include "replication/replication.h"
#include "replication/gc.h"


#define GC_DATA(gcsi) ((spread_data*)((gcsi)->data))
#define RECV_BUFFER_SIZE 2048

#define SPREAD_VERSION 4
#define SPREAD_SUBVERSION 0
#define SPREAD_PATCHLEVEL 0

#define MAX_AUTH_NAME 30
#define MAX_AUTH_METHODS 3
#define MAX_GROUP_NAME_SIZE 32

#define ACCEPT_SESSION 1


typedef enum
{
	SPS_INITIALIZING = 0,
	SPS_AUTHENTICATING = 1,
	SPS_READY = 12
} spread_state;

typedef enum
{
	SPST_JOIN = 0x00010000,
	SPST_LEAVE = 0x00020000,
} spread_service_type;

typedef struct
{
	int socket;
	buffer recv_buffer;
	spread_state state;
	char private_group_name[MAX_GROUP_NAME_SIZE];
} spread_data;

typedef struct
{
	/* the node identifier */
	char		spreadid[128];

	/* the coordinator node_id */
	uint32		node_id;
} spread_node;

/* endian stuff, copy'n'pasted, correct that */
#ifdef WORDS_BIGENDIAN
#define ARCH_ENDIAN 0x00000000
#else
#define ARCH_ENDIAN 0x80000080
#endif

#define ENDIAN_TYPE 0x80000080

#define Get_endian(type) ((type) & ENDIAN_TYPE)
#define Set_endian(type) (((type) & ~ENDIAN_TYPE) | ARCH_ENDIAN)
#define Same_endian(type) (((type) & ENDIAN_TYPE) == ARCH_ENDIAN)
#define Clear_endian(type) ((type) & ~ENDIAN_TYPE)

/* prototypes */
void spread_recv(gcs_info *gcsi);
void spread_multicast(const gcs_info *gcsi,
					  const spread_service_type service_type,
					  const gcs_group *group, const void *data,
					  const int size);

void spread_connect(gcs_info *gcsi);
void spread_disconnect(gcs_info *gcsi);
void spread_set_socks(const gcs_info *gcsi, fd_set *socks, int *max_socks);
void spread_handle_message(gcs_info *gcsi, const fd_set *socks);

gcs_group *spread_join(gcs_info *gcsi, const char *group_name,
					   gcs_group *parent_group);
void spread_leave(gcs_group *group);
bool spread_is_local(const gcs_group *group, const group_node *node);

void spread_broadcast(const gcs_group *group, const void *data,
					  int size, bool atomic);
void spread_unicast(const gcs_group *group, const group_node *node,
					const void *data, int size);


void
spread_recv(gcs_info *gcsi)
{
	int err;
	buffer *b = &GC_DATA(gcsi)->recv_buffer;

	err = recv(GC_DATA(gcsi)->socket,
			  (void*)((Pointer) b->data + b->fill_size),
			  b->max_size - b->ptr, 0);

	b->fill_size += err;

	if (err == 0)
		elog(ERROR, "GC Layer: GCS terminated connection.");
}

void
spread_init(gcs_info *gcsi, char **params)
{
	/* set all the methods for the coordinator to interact with the GCS */
	gcsi->funcs.connect = &spread_connect;
	gcsi->funcs.disconnect = &spread_disconnect;
	gcsi->funcs.set_socks = &spread_set_socks;
	gcsi->funcs.handle_message = &spread_handle_message;

	gcsi->funcs.join = &spread_join;
	gcsi->funcs.leave = &spread_leave;
	gcsi->funcs.is_local = &spread_is_local;
	gcsi->funcs.broadcast = &spread_broadcast;
	gcsi->funcs.unicast = &spread_unicast;
}

void
spread_connect(gcs_info *gcsi)
{
#ifdef THIS_CODE_IS_NOT_CURRENTLY_FUNCTIONAL
	buffer	b;
	int		err;
    struct sockaddr_in sin;
	char   *connectmsg;

	Assert(gcsi);
	Assert(gcsi->conn_state == GCSCS_DOWN);

	elog(DEBUG3, "GC Layer: connecting to the spread daemon");

	if (gcsi->socket_path && (strcmp(gcsi->socket_path, "") != 0))
		elog(WARNING, "GC Layer: the spread interface does not suppport "
			 "unix sockets. Using the portnumber.");

	/* initialize the group communication data object */
	gcsi->data = palloc(sizeof(spread_data));

	/* set correct state */
	GC_DATA(gcsi)->state = SPS_INITIALIZING;

	/* initialize the recieve buffer */
	init_buffer(&GC_DATA(gcsi)->recv_buffer, palloc(RECV_BUFFER_SIZE),
					RECV_BUFFER_SIZE, &spread_alloc, NULL, (void*) gcsi);

	gc_init_groups_hash(gcsi);

	/* connect to the spread daemon */
	GC_DATA(gcsi)->socket = socket(AF_INET, SOCK_STREAM, 0);
	if (!GC_DATA(gcsi)->socket)
	{
		elog(DEBUG3, "GC Layer: error creating a socket");
		gcsi_gcs_failed(gcsi);
		return;
	}

	/* connect */
    sin.sin_family = AF_INET;
    inet_aton("127.0.0.1", &(sin.sin_addr));
    sin.sin_port = ntohs(gcsi->port);

	err = connect(GC_DATA(gcsi)->socket,
				  (struct sockaddr*) &sin, sizeof(sin));
	if (err < 0)
	{
		elog(DEBUG1, "GC Layer: error connecting to the group "
			 "communication system!");
		gcsi_gcs_failed(gcsi);
		return;
	}

	/* send join message */
	connectmsg = palloc(10);
	init_buffer(&b, connectmsg, 10, &spread_alloc, NULL, NULL);

	put_int8(&b, SPREAD_VERSION);
	put_int8(&b, SPREAD_SUBVERSION);
	put_int8(&b, SPREAD_PATCHLEVEL);
	/* with groups, priority = 0 -- (priority << 4) || with_groups) */
	put_int8(&b, 1);
	put_pstring(&b, "");  /* the private name */

	/* send the connect message */
	err = send(GC_DATA(gcsi)->socket, connectmsg, b.fill_size, MSG_NOSIGNAL);
	if (err != b.fill_size)
	{
		elog(DEBUG1, "GC Layer: error sending join message to server (%s)!",
					 strerror(errno));
		gcsi_gcs_failed(gcsi);
		return;
	}

	gcsi->conn_state == GCSCS_REQUESTED;

	pfree(connectmsg);
#else
	/* non-functional */
	gcsi_gcs_failed(gcsi);
	return;
#endif
}

void
spread_disconnect(gcs_info *gcsi)
{
	/* FIXME: we should really send a KILL_MESS here */

	close(GC_DATA(gcsi)->socket);
	pfree(gcsi->data);
}

void
spread_multicast(const gcs_info *gcsi,
				 const spread_service_type service_type,
				 const gcs_group *group,
				 const void *data, const int size)
{
	int			msg_size;
	char	   *msg;
	buffer		b;

	int			msg_type = 0;

	msg_size = 110;
	msg = palloc(msg_size);
	init_buffer(&b, msg, msg_size);

	put_int32(&b, Set_endian(service_type));
	put_data(&b, &GC_DATA(gcsi)->private_group_name, MAX_GROUP_NAME_SIZE);
	put_int32(&b, 1);		/* number of groups is always one, so far */
	put_int32(&b, Set_endian((msg_type << 8) && 0x00FFFF00));
	put_int32(&b, size);

	/* repeat this for multiple groups, if necessary */
	put_data(&b, group->name, MAX_GROUP_NAME_SIZE);

	if (size > 0)
	{
		/* send the real data */
		Assert(data != NULL);
		put_data(&b, data, size);
	}
}

gcs_group *
spread_join(gcs_info *gcsi, const char *group_name, gcs_group *parent_group)
{
	/* FIXME: handle parent groups */
	gcs_group *new_group;

	/*
	 * FIXME: better error checking, as this is a user supplied argument.
	 * allowed characters are between 0x24 and 0x7E for spread
	 */
	Assert(NAMEDATALEN > MAX_GROUP_NAME_SIZE);
	Assert(strlen(group_name) < MAX_GROUP_NAME_SIZE);
	Assert(strlen(group_name) > 0);

	new_group = gc_create_group(gcsi, group_name, 128,
								sizeof(spread_node));

	spread_multicast(gcsi, SPST_JOIN, new_group, NULL, 0);

	return new_group;
}

void
spread_leave(gcs_group *group)
{
	Assert(group);
	Assert(group->gcsi);
	Assert(group->gcsi->conn_state == GCSCS_ESTABLISHED);

	spread_multicast(group->gcsi, SPST_LEAVE, group, NULL, 0);
	gc_destroy_group(group);
}

void
spread_broadcast(const gcs_group *group, const void *data, int size,
				 bool atomic)
{
}

void
spread_unicast(const gcs_group *group, const group_node *node,
			   const void *data, int size)
{
}

void
spread_set_socks(const gcs_info *gcsi, fd_set *socks, int *max_socks)
{
	FD_SET(GC_DATA(gcsi)->socket, socks);
	if (GC_DATA(gcsi)->socket > *max_socks)
		*max_socks = GC_DATA(gcsi)->socket;
}

void
spread_handle_message(gcs_info *gcsi, const fd_set *socks)
{
	buffer *b;
	int		err;
	int		size;
	int		msg_start;
	int		auth_answer;
	char   *authstring;
	char   *str;

	int		server_version;

	b = &GC_DATA(gcsi)->recv_buffer;

	if (FD_ISSET(GC_DATA(gcsi)->socket, socks))
	{
		elog(NOTICE, "GC Layer: retrieving a message.");
		spread_recv(gcsi);
		msg_start = b->ptr;

		if (GC_DATA(gcsi)->state == SPS_INITIALIZING)
		{
			elog(NOTICE, "GC Layer: %d bytes in the buffer.",
				 b->fill_size - b->ptr);

			size = get_int8(b);

			if (get_bytes_read(b) < size)
			{
				elog(DEBUG4, "GC Layer: waiting for more data");
				b->ptr = msg_start;
				return;
			}

			if (size > 0)
			{
				authstring = palloc(size + 1);
				get_data(b, authstring, size);
				authstring[size] = 0;

				if (strncmp(authstring, "NULL", 4) == 0)
				{
					pfree(authstring);

					size = MAX_AUTH_NAME * MAX_AUTH_METHODS;
					authstring = palloc0(size);
					strcpy(authstring, "NULL");

					err = send(GC_DATA(gcsi)->socket, authstring, size,
							   MSG_NOSIGNAL);
					if (err != size)
					{
						elog(ERROR,
							 "GC Layer: error sending join message to "
							 "server (%s)!\n",
							 strerror(errno));
					}

					GC_DATA(gcsi)->state = SPS_AUTHENTICATING;
					elog(NOTICE, "GC Layer: connected, now authenticating.");
				}
				else
					elog(ERROR, "GC Layer: unsupported auth method: '%s'",
						 authstring);
			}
			else
			{
				elog(ERROR, "GC Layer: invalid auth method (size: %d)", size);
			}
		}
		else if (GC_DATA(gcsi)->state == SPS_AUTHENTICATING)
		{
			elog(NOTICE, "GC Layer: %d bytes in the buffer.",
				 b->fill_size - b->ptr);

			auth_answer = get_int8(b);

			elog(NOTICE, "GC Layer: auth answer: %d", auth_answer);

			if (auth_answer != ACCEPT_SESSION)
				elog(ERROR, "GC Layer: unable to authenticate.");
			else
				elog(DEBUG3, "GC Layer: authenticated.");

			if (get_bytes_read(b) < 4)
			{
				elog(DEBUG3, "GC Layer: waiting for more data");
				b->ptr = msg_start;
				return;
			}

			server_version = get_int8(b) << 16;
			server_version |= get_int8(b) << 8;
			server_version |= get_int8(b);

			if (server_version < 0x030F00)
				elog(ERROR, "GC Layer: at least spread version 3.15 "
					 "is required");

			size = get_int8(b);

			if ((b->fill_size - b->ptr) < size)
			{
				elog(DEBUG3, "GC Layer: waiting for more data");
				b->ptr = msg_start;
				return;
			}

			b->ptr--;

			str = get_pstring(b);
			strlcpy(GC_DATA(gcsi)->private_group_name, str,
					MAX_GROUP_NAME_SIZE);
			/* FIXME: check return value of strlcpy */
			pfree(str);

			elog(NOTICE, "GC Layer: private group: %s",
				 GC_DATA(gcsi)->private_group_name);
			elog(NOTICE, "GC Layer: %d bytes remaining in the buffer.",
				 b->fill_size - b->ptr);

			GC_DATA(gcsi)->state = SPS_READY;
		}
		else if (GC_DATA(gcsi)->state == SPS_READY)
		{
			elog(NOTICE, "GC Layer: received a message in state READY?!?");
			elog(NOTICE, "GC Layer: %d bytes remaining in the buffer.",
				 b->fill_size - b->ptr);
		}
		else
		{
			elog(ERROR, "GC Layer: undefined state");
		}

		if (get_bytes_read(b) == 0)
		{
			/* recycle the recv_buffer */
			b->ptr = 0;
			b->fill_size = 0;
		}
	}
}


bool
spread_is_local(const gcs_group *group, const group_node *node)
{
	return 0;
}

