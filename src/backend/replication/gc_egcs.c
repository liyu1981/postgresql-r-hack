/*-------------------------------------------------------------------------
 *
 * gc_egcs.c
 *
 * Interface to an emulated group communication system, mainly used
 * for testing.
 *
 * Portions Copyright (c) 2010, Translattice, Inc
 * Portions Copyright (c) 2001-2010, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <unistd.h>
#include <errno.h>

#include "postgres.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "libpq/pqsignal.h"
#include "replication/coordinator.h"
#include "replication/replication.h"
#include "replication/gc.h"
#include "storage/buffer.h"
#include "storage/imsg.h"
#include "storage/ipc.h"

#include "replication/replication.h"
#include "replication/gc.h"

typedef struct
{
	int		socket;
	char   *hostname;
	int		port;
	uint32	egcs_socket_id;
	buffer	recv_buffer;
} egcs_data;

typedef struct
{
	int		socket_id;
	uint32	gcsi_node_id;
} egcs_node;

#define GC_DATA(gcsi) ((egcs_data*)((gcsi)->data))
#define GC_NODE(gnode) ((egcs_node*)((gnode)->gcs_node))
#define RECV_BUFFER_SIZE (65536 * 8)

/* prototypes */
void *egcs_alloc(size_t sz);
void egcs_recv(gcs_info *gcsi);

/* interface routines */
void egcs_connect(gcs_info *gcsi);
void egcs_disconnect(gcs_info *gcsi);
void egcs_set_socks(const gcs_info *gcsi, fd_set *socks, int *max_socks);
void egcs_handle_message(gcs_info *gcsi, const fd_set *socks);

gcs_group *egcs_join(gcs_info *gcsi, const char *group_name,
					 gcs_group *parent_group);
void egcs_leave(gcs_group *group);
group_node *egcs_get_local_node(const gcs_group *group);
bool egcs_is_local(const gcs_group *group, const group_node *node);
void egcs_get_node_desc(const gcs_group *group, const group_node *node,
						char *str);

void egcs_broadcast(const gcs_group *group, const void *data,
					int size, bool atomic);
void egcs_unicast(const gcs_group *group, const group_node *node,
				  const void *data, int size);

static void egcs_blocking_send(gcs_info *gcsi, char *data, int size);


void
egcs_init(gcs_info *gcsi, char **params)
{
	int		i;

	/* initialize the group communication data object */
	gcsi->data = palloc(sizeof(egcs_data));

	/* initialize the recieve buffer */
	init_buffer(&GC_DATA(gcsi)->recv_buffer, palloc(RECV_BUFFER_SIZE),
				RECV_BUFFER_SIZE);

	gc_init_groups_hash(gcsi);

	/* set all the methods for the coordinator to interact with the GCS */
	gcsi->funcs.connect = &egcs_connect;
	gcsi->funcs.disconnect = &egcs_disconnect;
	gcsi->funcs.set_socks = &egcs_set_socks;
	gcsi->funcs.handle_message = &egcs_handle_message;

	gcsi->funcs.join = &egcs_join;
	gcsi->funcs.leave = &egcs_leave;
	gcsi->funcs.get_local_node = &egcs_get_local_node;
	gcsi->funcs.is_local = &egcs_is_local;
#ifdef COORDINATOR_DEBUG
	gcsi->funcs.get_node_desc = &egcs_get_node_desc;
#endif
	gcsi->funcs.broadcast = &egcs_broadcast;
	gcsi->funcs.unicast = &egcs_unicast;

	/* set default for parameters */
	GC_DATA(gcsi)->hostname = "127.0.0.1";
	GC_DATA(gcsi)->port = 54320;

	/* process user provided parameters */
	for (i = 0; params[i+1] != NULL; i++)
	{
		if (pg_strcasecmp(params[i], "host") == 0)
			GC_DATA(gcsi)->hostname = params[i+1];
		else if (pg_strcasecmp(params[i], "port") == 0)
			GC_DATA(gcsi)->port = atoi(params[i+1]);
		else
			elog(ERROR, "GC Layer: invalid parameter given: '%s'",
				 params[i]);
	}
}

void
egcs_connect(gcs_info *gcsi)
{
	char					msg[2];
	buffer					b;
	int						err;
#if 0
	struct sockaddr_un	   *sa;
#endif
	struct sockaddr_in		sin;

	Assert(gcsi);
	Assert(gcsi->conn_state == GCSCS_DOWN);

	elog(DEBUG3, "GC Layer: connecting to egcs at host %s on port %d",
		 GC_DATA(gcsi)->hostname, GC_DATA(gcsi)->port);

	/* init a message buffer for a hello message */
	init_buffer(&b, msg, 2);

	GC_DATA(gcsi)->socket = socket(AF_INET, SOCK_STREAM, 0);
	if (!GC_DATA(gcsi)->socket)
	{
		elog(WARNING, "GC Layer: error creating a socket");
		gcsi_gcs_failed(gcsi);
		return;
	}

	/* connect */
#if 0

	unix socket connections not currently supported...

	if (gcsi->socket_path && (strlen(gcsi->socket_path) > 0))
	{
		sa = (struct sockaddr_un*) egcs_alloc(64);
		sa->sun_family = AF_UNIX;
		strcpy(sa->sun_path, gcsi->socket_path);
		err = connect(GC_DATA(gcsi)->socket, (struct sockaddr*) sa, 64);
		pfree(sa);
		if (err)
		{
			elog(WARNING, "GC Layer: error connecting to the group "
				 "communication system!");
			gcsi_gcs_failed(gcsi);
			return;
		}
	}
	else
	{
#endif
	    sin.sin_family = AF_INET;
		inet_aton(GC_DATA(gcsi)->hostname, &(sin.sin_addr));
		sin.sin_port = ntohs(GC_DATA(gcsi)->port);
		err = connect(GC_DATA(gcsi)->socket,
					  (struct sockaddr*) &sin, sizeof(sin));
		if (err < 0)
		{
			elog(WARNING, "GC Layer: error connecting to the group "
 				 "communication system!");
			gcsi_gcs_failed(gcsi);
			return;
		}
#if 0
	}
#endif

	/* send initialization message */
	put_int8(&b, 'H');	/* 'h'ello */
	put_int8(&b, 2);	/* protocol revision 2 */

	egcs_blocking_send(gcsi, msg, b.ptr);

	gcsi->conn_state = GCSCS_REQUESTED;
}

void
egcs_disconnect(gcs_info *gcsi)
{
	elog(DEBUG3, "GC Layer: disconnecting from egcs");

	hash_destroy(gcsi->groups);

	close(GC_DATA(gcsi)->socket);
	pfree(gcsi->data);
}

static void
egcs_blocking_send(gcs_info *gcsi, char *data, int size)
{
	int bytes_sent;

	while (size > 0)
	{
		bytes_sent = send(GC_DATA(gcsi)->socket, data, size,
						  MSG_NOSIGNAL);
		if (bytes_sent < 0) {
			/* FIXME: better error handling */
			elog(FATAL, "GC Layer: error sending message to server (%s)!\n",
				 strerror(errno));

		}
		size -= bytes_sent;
		data += bytes_sent;
	}
}

gcs_group *
egcs_join(gcs_info *gcsi, const char *group_name, gcs_group *parent_group)
{
	/* FIXME: handle parent groups */
	buffer		b;
	char	   *joinmsg;
	int			size;
	gcs_group  *new_group;
	gcs_group  *pgroup;
	int			i, j, full_group_name_len;
	char	   *full_group_name;

	Assert(gcsi->conn_state == GCSCS_ESTABLISHED);

	full_group_name_len = strlen(group_name);

	pgroup = parent_group;
	while (pgroup)
	{
		full_group_name_len += 1 + strlen(pgroup->name);
		pgroup = pgroup->parent;
	}

	full_group_name = palloc(full_group_name_len + 1);
	i = full_group_name_len;

	/* group name itself as last element */
	i -= strlen(group_name);
	strcpy(&full_group_name[i], group_name);
	i--;

	/* then walk up parent names */
	pgroup = parent_group;
	while (pgroup)
	{
		j = i - strlen(pgroup->name);
		strcpy(&full_group_name[j], pgroup->name);
		full_group_name[i] = '.';
		pgroup = pgroup->parent;
		i = j - 1;
	}

	elog(DEBUG3, "GC Layer: joining group '%s'", full_group_name);

	size = strlen(full_group_name) + 10;
	joinmsg = palloc(size);

	init_buffer(&b, joinmsg, size);
	put_int8(&b, 'J');
	put_pstring(&b, full_group_name);
	egcs_blocking_send(gcsi, joinmsg, b.fill_size);

	pfree(joinmsg);

	new_group = gc_create_group(gcsi, full_group_name, sizeof(int),
								sizeof(egcs_node));
	new_group->parent = parent_group;
	return new_group;
}

void
egcs_leave(gcs_group *group)
{
	Assert(group);
	Assert(group->gcsi);
	Assert(group->gcsi->conn_state == GCSCS_ESTABLISHED);
	gc_destroy_group(group);
}

void
egcs_broadcast(const gcs_group *group, const void *data, int size,
			   bool totally_ordered)
{
	int	msg_size;
	char   *msg;
	buffer	b;

	Assert(group->gcsi->conn_state == GCSCS_ESTABLISHED);
	Assert(size < 65536);

	msg_size = 2 + 2 * sizeof(int) + strlen(group->name) + size;
	msg = palloc(msg_size);
	init_buffer(&b, msg, msg_size);

	put_int8(&b, 'M');
	put_int32(&b, GC_DATA(group->gcsi)->egcs_socket_id);
	put_pstring(&b, group->name);

	put_int32(&b, size);
	put_data(&b, data, size);

	egcs_blocking_send(group->gcsi, b.data, b.ptr);

	pfree(msg);
}

void
egcs_unicast(const gcs_group *group, const group_node *node,
			 const void *data, int size)
{
	int	msg_size;
	char   *msg;
	buffer	b;

	Assert(group->gcsi->conn_state == GCSCS_ESTABLISHED);
	Assert(size <= 65536);

	msg_size = 2 + 3 * sizeof(int) + strlen(group->name) + size;
	msg = palloc(msg_size);
	init_buffer(&b, msg, msg_size);

	put_int8(&b, 'm');

	/* sender node id */
	put_int32(&b, GC_DATA(group->gcsi)->egcs_socket_id);

	/* recipient node id */
	put_int32(&b, GC_NODE(node)->socket_id);

	/* group name */
	put_pstring(&b, group->name);

	put_int32(&b, size);
	put_data(&b, data, size);

	egcs_blocking_send(group->gcsi, b.data, b.ptr);

	pfree(msg);
}

void
egcs_set_socks(const gcs_info *gcsi, fd_set *socks, int *max_socks)
{
	FD_SET(GC_DATA(gcsi)->socket, socks);
	if (GC_DATA(gcsi)->socket > *max_socks)
		*max_socks = GC_DATA(gcsi)->socket;
}

void
egcs_recv(gcs_info *gcsi)
{
	int err;
	buffer *b = &GC_DATA(gcsi)->recv_buffer;

	/*
	 * FIXME: hm.. having a static buffer size isn't that great,
	 *        especially when b->ptr can be much bigger than 0,
	 *        i.e. the buffer potentionally holding multiple,
	 *        but already processed messages.
	 *
	 *        To really solve this, we should better use some sort of
	 *        dynamic buffer. Maybe with the mem_alloc function.
	 */
	Assert(b->fill_size <= b->max_size);

	/* 
	 * If the buffer already *is* full, we don't need to read from the
	 * socket.
	 */
	if (b->max_size - b->fill_size == 0)
		return;

	err = recv(GC_DATA(gcsi)->socket,
			  ((Pointer) b->data) + b->fill_size,
			  b->max_size - b->fill_size, 0);

	b->fill_size += err;

	if (err == 0)
		elog(ERROR, "GC Layer: group communication system terminated "
			 " connection.");
}

void
egcs_handle_message(gcs_info *gcsi, const fd_set *socks)
{
	int			size;
	int			old_size;
	int			min_ptr;
	char	   *group_name;
	gcs_group  *group;
	egcs_node  *gc_node;
	group_node *node;
	char		msg_type;
	char		vc_type;
	int			socket_id;
	int			msg_start;
	bool		found;

	buffer *b = &(GC_DATA(gcsi)->recv_buffer);

	if (FD_ISSET(GC_DATA(gcsi)->socket, socks))
	{
		/* read from the socket at least once */
		egcs_recv(gcsi);

		while (b->ptr < b->fill_size)
		{
			msg_start = b->ptr;
			msg_type = get_int8(b);
			switch (msg_type)
			{
				case 'H':
					if (get_bytes_read(b) < 5)
					{
						elog(DEBUG4, "GC Layer: waiting for more data");
						b->ptr = msg_start;
						return;
					}

					/* check the protocol revision */
					Assert(get_int8(b) == 2);

					/* save the socket id the server gave us */
					GC_DATA(gcsi)->egcs_socket_id = get_int32(b);

					Assert(gcsi->conn_state == GCSCS_REQUESTED);

					elog(DEBUG3,
						"GC Layer: received a hello message (node %d)",
						GC_DATA(gcsi)->egcs_socket_id);

					gcsi->conn_state = GCSCS_ESTABLISHED;
					gcsi_gcs_ready(gcsi);

					break;

				case 'V':
					if (get_bytes_read(b) < 6)
					{
						elog(DEBUG4, "GC Layer: waiting for more data");
						b->ptr = msg_start;
						return;
					}

					vc_type = get_int8(b);

					elog(DEBUG3, "GC Layer: viewchange message received.");
					socket_id = get_int32(b);

					size = get_int8(b);
					if (get_bytes_read(b) < size)
					{
						elog(DEBUG4, "GC Layer: waiting for more data");
						b->ptr = msg_start;
						return;
					}
					b->ptr--;

					group_name = get_pstring(b);

					if (vc_type != 'I')
						elog(DEBUG5, "GC Layer: viewchange: node %d "
							 "%s group %s",
							 socket_id, (vc_type == 'J' ? "joins" : "leaves"),
							 group_name);
					else
						elog(DEBUG5, "GC Layer: initial view contains "
							 "node %d", socket_id);

					group = gc_get_group(gcsi, group_name);
					Assert(group);

					gc_node = hash_search(group->gcs_nodes, &socket_id,
									 	  HASH_ENTER, &found);

					if (!found)
					{
						/*
						 * we have just added a new node, tell the
						 * coordinator, too.
						 */
						node = gcsi_add_node(group, socket_id);
						node->gcs_node = gc_node;
						gc_node->socket_id = socket_id;
						gc_node->gcsi_node_id = node->id;

						if (socket_id == GC_DATA(gcsi)->egcs_socket_id)
							group->node_id_self_ref = node->id;
					}
					else
					{
						node = gcsi_get_node(group, gc_node->gcsi_node_id);
						Assert(node);
					}

					gcsi_viewchange_start(group);

					if (vc_type == 'J')
						gcsi_node_changed(group, node, GCVC_JOINED);
					else if (vc_type == 'L')
						gcsi_node_changed(group, node, GCVC_LEFT);
					else if (vc_type == 'I')
						gcsi_node_changed(group, node, GCVC_JOINED);
					else
						elog(ERROR, "GC Layer: unknown viewchange type: %c",
							 vc_type);

					gcsi_viewchange_stop(group);

					break;

				case 'M':
					if (get_bytes_read(b) < 5)
					{
						elog(DEBUG4, "GC Layer: waiting for more data");
						b->ptr = msg_start;
						return;
					}

					socket_id = get_int32(b);

					size = get_int8(b);
					if (get_bytes_read(b) < size + sizeof(int))
					{
						elog(DEBUG4, "GC Layer: waiting for more data");
						b->ptr = msg_start;
						return;
					}
					b->ptr--;

					group_name = get_pstring(b);
					size = get_int32(b);

					if (get_bytes_read(b) < size)
					{
						elog(DEBUG4, "GC Layer: waiting for more data");
						b->ptr = msg_start;
						return;
					}

					group = gc_get_group(gcsi, group_name);
					Assert(group);

					/* what to skip at the minimum */
					min_ptr = b->ptr + size;
					elog(DEBUG3,
						"GC Layer: received a message for group %s of size %d",
						group_name, size);

					/* call the coordinator handle function
					 *    the handle function depends on a buffer with correct
					 *    fill_size parameter.
					 */
					old_size = b->fill_size;
					b->fill_size = size + b->ptr;

					/* get the coordinator node */
					gc_node = hash_search(group->gcs_nodes, &socket_id,
									 	  HASH_FIND, NULL);
					Assert(gc_node);
					node = hash_search(group->nodes, &gc_node->gcsi_node_id,
									   HASH_FIND, NULL);
					Assert(node);

					coordinator_handle_gc_message(group, node, 'T', b);

					b->fill_size = old_size;
					pfree(group_name);

					/* check if enough data is skipped */
					if (b->ptr < min_ptr)
					{
						elog(DEBUG3,
							"GC Layer: skipping %d bytes",
							min_ptr - b->ptr);
						b->ptr = min_ptr;
					}
					Assert(b->ptr == min_ptr);
					break;

				case 'm':
					if (get_bytes_read(b) < 9)
					{
						elog(DEBUG4, "GC Layer: waiting for more data");
						b->ptr = msg_start;
						return;
					}

					socket_id = get_int32(b);

					/* skip the recipient node id */
					get_int32(b);

					size = get_int8(b);
					if (get_bytes_read(b) < size + sizeof(int))
					{
						elog(DEBUG4, "GC Layer: waiting for more data");
						b->ptr = msg_start;
						return;
					}
					b->ptr--;

					group_name = get_pstring(b);
					size = get_int32(b);

					if (get_bytes_read(b) < size)
					{
						elog(DEBUG4, "GC Layer: waiting for more data");
						b->ptr = msg_start;
						return;
					}

					group = gc_get_group(gcsi, group_name);
					Assert(group);

					/* what to skip at the minimum */
					min_ptr = b->ptr + size;
					elog(DEBUG3,
						"GC Layer: received a single node message "
						"through group %s of size %d",
						group_name, size);

					/* call the coordinator handle function
					 *    the handle function depends on a buffer with
					 *    correct fill_size parameter.
					 */
					old_size = b->fill_size;
					b->fill_size = size + b->ptr;

					/* get the coordinator node */
					gc_node = hash_search(group->gcs_nodes, &socket_id,
									 	 HASH_FIND, NULL);
					Assert(gc_node);
					node = hash_search(group->nodes, &gc_node->gcsi_node_id,
									   HASH_FIND, NULL);
					Assert(node);

					coordinator_handle_gc_message(group, node, 'F', b);

					b->fill_size = old_size;
					pfree(group_name);

					/* check if enough data is skipped */
					if (b->ptr < min_ptr)
					{
						elog(DEBUG3,
							"GC Layer: skipping %d bytes",
							min_ptr - b->ptr);
						b->ptr = min_ptr;
					}
					Assert(b->ptr == min_ptr);
					break;

				default:
					elog(WARNING, "GC Layer: ignored unknown message: '%c'",
							msg_type);
					break;
			}

			elog(DEBUG1, "GC Layer: remaining bytes: %d",
				 b->fill_size - b->ptr);
		}

		if (get_bytes_read(b) == 0)
		{
			/* recycle the recv_buffer */
			b->ptr = 0;
			b->fill_size = 0;
		}
	}
}

group_node *
egcs_get_local_node(const gcs_group *group)
{
	Assert(group->gcsi->conn_state == GCSCS_ESTABLISHED);
	return gcsi_get_node(group, GC_DATA(group->gcsi)->egcs_socket_id);
}

bool
egcs_is_local(const gcs_group *group, const group_node *node)
{
	if (group->gcsi->conn_state != GCSCS_ESTABLISHED)
		return false;

	return (GC_DATA(group->gcsi)->egcs_socket_id == GC_NODE(node)->socket_id);
}

#ifdef COORDINATOR_DEBUG
void
egcs_get_node_desc(const gcs_group *group, const group_node *node,
				   char *str)
{
	sprintf(str, "%d <gcsi, socket id %d>",
			GC_NODE(node)->gcsi_node_id, GC_NODE(node)->socket_id);
}
#endif

