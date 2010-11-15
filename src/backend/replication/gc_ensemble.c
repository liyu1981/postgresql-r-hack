/*-------------------------------------------------------------------------
 *
 * gc_ensemble.c
 *
 * Interface to ensemble, a group communication system developed at
 * Cornell University, as well as at the Hebrew University of Jerusalem.
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
	char   *hostname;
	int		port;
	int 	socket;
	int		mid;		/* a connection-long unique member id */
	buffer	recv_buffer;
	buffer	send_buffer; /* in case of block */
	bool	blocked;
} ens_data;

/* Downcalls into Ensemble */
typedef enum ens_dn_msg_t {
    ENS_DN_JOIN = 1,
    ENS_DN_CAST = 2,
    ENS_DN_SEND = 3,
    ENS_DN_SEND1 = 4,
    ENS_DN_SUSPECT = 5,
    ENS_DN_LEAVE = 6,
    ENS_DN_BLOCK_OK = 7
} ens_dn_msg_t;

/* Upcalls from Ensemble */
typedef enum ens_up_msg_t {
    ENS_UP_VIEW = 1,
    ENS_UP_CAST = 2,
    ENS_UP_SEND = 3,
    ENS_UP_BLOCK = 4,
    ENS_UP_EXIT = 5
} ens_up_msg_t;

typedef struct {
	uint32_t header_size;
	uint32_t data_size;
	uint32_t member_id;
	uint32_t message_type;
} ens_message;

typedef struct {
	char *addr;
	char *endpt;
} ens_node_in_view;

#define ENS_MAX_ENDPT_SIZE 128

typedef struct
{
	/* the ensemble node identifier */
	char		ens_endpt[ENS_MAX_ENDPT_SIZE];

	/* the rank in the current view */
	int			curr_view_rank;

	/* the coordinator's node_id */
	uint32		gcsi_node_id;
} ens_node;

#define GC_DATA(gcsi) ((ens_data*)((gcsi)->data))
#define GC_NODE(gnode) ((ens_node*)((gnode)->gcs_node))
#define RECV_BUFFER_SIZE 65536
#define SEND_BUFFER_SIZE 65536

/* prototypes */
static void ens_buffer_fill(void *buf, size_t min, const void *obj);
static void ens_send_block_ok(const gcs_info *gc);
static void ens_send_buffered_data(gcs_info *gcsi);

/* interface routines */
void ens_connect(gcs_info *gcsi);
void ens_disconnect(gcs_info *gcsi);
void ens_set_socks(const gcs_info *gcsi, fd_set *socks,
				   int *max_socks);
void ens_handle_message(gcs_info *gcsi, const fd_set *socks);

gcs_group *ens_join(gcs_info *gcsi, const char *group_name,
						   gcs_group *parent_group);
void ens_leave(gcs_group *group);
bool ens_is_local(const gcs_group *group, const group_node *node);

void ens_broadcast(const gcs_group *group, const void *data,
				   int size, bool atomic);
void ens_unicast(const gcs_group *group, const group_node *node,
				 const void *data, int size);


void
ens_init(gcs_info *gcsi, char **params)
{
	int		i;

	/* set all the methods for the coordinator to interact with the GCS */
	gcsi->funcs.connect = &ens_connect;
	gcsi->funcs.disconnect = &ens_disconnect;
	gcsi->funcs.set_socks = &ens_set_socks;
	gcsi->funcs.handle_message = &ens_handle_message;

	gcsi->funcs.join = &ens_join;
	gcsi->funcs.leave = &ens_leave;
	gcsi->funcs.is_local = &ens_is_local;
	gcsi->funcs.broadcast = &ens_broadcast;
	gcsi->funcs.unicast = &ens_unicast;

	/* initialize the group communication data object */
	(gcsi)->data = palloc(sizeof(ens_data));

	/* initialize the buffers */
	init_buffer(&GC_DATA(gcsi)->recv_buffer, palloc(RECV_BUFFER_SIZE),
				RECV_BUFFER_SIZE);

	init_buffer(&GC_DATA(gcsi)->send_buffer, palloc(SEND_BUFFER_SIZE),
				SEND_BUFFER_SIZE);

	/* start with a member id of 1 */
	GC_DATA(gcsi)->mid = 1;

	gc_init_groups_hash(gcsi);

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
ens_connect(gcs_info *gcsi)
{
	int					err;
    struct sockaddr_in	sin;

	Assert(gcsi);
	Assert(gcsi->conn_state == GCSCS_DOWN);

	elog(DEBUG3, "GC Layer: connecting to ensemble at host %s on port %d",
		 GC_DATA(gcsi)->hostname, GC_DATA(gcsi)->port);

	/* connect to the ensemble daemon */
	GC_DATA(gcsi)->socket = socket(AF_INET, SOCK_STREAM, 0);
	if (!GC_DATA(gcsi)->socket)
	{
		elog(DEBUG3, "GC Layer: error creating a socket");
		gcsi_gcs_failed(gcsi);
		return;
	}

	/* connect */
    sin.sin_family = AF_INET;
    inet_aton(GC_DATA(gcsi)->hostname, &(sin.sin_addr));
    sin.sin_port = ntohs(GC_DATA(gcsi)->port);

	err = connect(GC_DATA(gcsi)->socket, (struct sockaddr*) &sin,
				  sizeof(sin));
	if (err < 0)
	{
		elog(DEBUG1, "GC Layer: error connecting to the group "
			 "communication system!");
		gcsi_gcs_failed(gcsi);
		return;
	}

	/*
	 * We assume the GCS to be ready as soon as we are connected. Maybe we
	 * could also check the version or something to ensure we are, but I'm
	 * unaware of any way to do so with ensemble. Instead, the ensemble
	 * version is sent with every view change. So actually, the ensemble
	 * never is in the GCSCS_REQUESTED state.
	 */
	gcsi->conn_state = GCSCS_ESTABLISHED;
	gcsi_gcs_ready(gcsi);
}

void
ens_disconnect(gcs_info *gcsi)
{
	close(GC_DATA(gcsi)->socket);
	pfree(gcsi->data);
}

gcs_group *
ens_join(gcs_info *gcsi, const char *group_name, gcs_group *parent_group)
{
	/* FIXME: handle parent groups */
	buffer		b;
	char	   *joinmsg;
	int			data_size;
	int			size;
	int			err;

	gcs_group  *new_group;

	elog(DEBUG3, "GC Layer: joining group '%s'", group_name);

	Assert(gcsi);
	Assert(gcsi->conn_state == GCSCS_ESTABLISHED);

	size = strlen(group_name) + 200;

	/* send join message */
	joinmsg = palloc(size);
	init_buffer(&b, joinmsg, size);

	/* write the message size header */
	put_int32(&b, 8);					/* header size */
	put_int32(&b, 0);					/* data length */

	/* write the join message header */
	put_int32(&b, GC_DATA(gcsi)->mid);	/* member id */
	put_int32(&b, ENS_DN_JOIN);			/* message type */

	put_p32string(&b, group_name);		/* group name */
	put_p32string(&b, "Total:Gmp:Sync:Heal:Switch:Frag:"
 				  "Suspect:Flow:Primary:Local");	/* properties */
	put_p32string(&b, "primary_quorum=15:int;suspect_max_idle=50:"
				  "int;suspect_sweep=1.0:time");	/* params */
	put_p32string(&b, "");				/* princ */
	put_int32(&b, 0);					/* secure connection: false */

	/* now write the header length */
	data_size = b.fill_size - 8;
	b.ptr = 0;							/* pos of data length field */
	b.fill_size -= 4;					/* make fill_size match again */
	put_int32(&b, data_size);

	/* send the join message */
	err = send(GC_DATA(gcsi)->socket, joinmsg, b.fill_size, MSG_NOSIGNAL);
	if (err != b.fill_size)
	{
		elog(DEBUG1, "GC Layer: error sending join message to server (%s)!\n",
			 strerror(errno));
		return NULL;
	}

	pfree(joinmsg);

	new_group = gc_create_group(gcsi, group_name, ENS_MAX_ENDPT_SIZE,
								sizeof(ens_node));
	return new_group;
}

void
ens_leave(gcs_group *group)
{
	Assert(group);
	Assert(group->gcsi);
	Assert(group->gcsi->conn_state == GCSCS_ESTABLISHED);
	gc_destroy_group(group);
}

void
ens_broadcast(const gcs_group *group, const void *data, int size,
			  bool atomic)
{
	int		err;
	buffer *b;
	char   *castmsg;
	int		data_size = strlen(group->name) + 1 + size;

	if (GC_DATA(group->gcsi)->blocked)
	{
		b = &GC_DATA(group->gcsi)->send_buffer;
		castmsg = NULL;
	}
	else
	{
		castmsg = palloc(sizeof(ens_message) + data_size);
		b = palloc0(sizeof(buffer));
		init_buffer(b, castmsg, sizeof(ens_message) + data_size);
	}

	/* write the message size header */
	put_int32(b, 8);								/* header size */
	put_int32(b, data_size);						/* data length */

	/* write the join message header */
	put_int32(b, GC_DATA(group->gcsi)->mid);		/* member id */
	put_int32(b, ENS_DN_CAST);						/* message type */

	/* the group name, should be tracked by ensemble, no? */
	put_pstring(b, group->name);

	put_data(b, data, size);

	if (!GC_DATA(group->gcsi)->blocked)
	{
		/* send the message */
		Assert(b->fill_size == sizeof(ens_message) + data_size);
		err = send(GC_DATA(group->gcsi)->socket, castmsg, b->fill_size,
				   MSG_NOSIGNAL);
		if (err != b->fill_size)
		{
			elog(ERROR, "GC Layer: error sending to group %s (%s)!\n",
				 group->name, strerror(errno));
		}

		pfree(b);
		pfree(castmsg);
	}
}

void
ens_unicast(const gcs_group *group, const group_node *node, const void *data,
			int size)
{
	int		err;
	buffer *b;
	char   *castmsg;
	int		data_size = strlen(group->name) + 1 + size;

	if (GC_DATA(group->gcsi)->blocked)
	{
		b = &GC_DATA(group->gcsi)->send_buffer;
		castmsg = NULL;
	}
	else
	{
		castmsg = palloc(sizeof(ens_message) + 4 + data_size);
		b = palloc0(sizeof(buffer));
		init_buffer(b, castmsg, sizeof(ens_message) + 4 + data_size);
	}

	/* write the message size header */
	put_int32(b, 12);								/* header size */
	put_int32(b, data_size);						/* data length */

	/* write the send1 message header */
	put_int32(b, GC_DATA(group->gcsi)->mid);		/* member id */
	put_int32(b, ENS_DN_SEND1);					/* message type */
	put_int32(b, GC_NODE(node)->curr_view_rank);	/* recipient node id */

	/* the group name, should be tracked by ensemble, no? */
	put_pstring(b, group->name);

	put_data(b, data, size);

	if (!GC_DATA(group->gcsi)->blocked)
	{
		/* send the message */
		Assert(b->fill_size == sizeof(ens_message) + 4 + data_size);
		err = send(GC_DATA(group->gcsi)->socket, castmsg, b->fill_size,
				   MSG_NOSIGNAL);
		if (err != b->fill_size)
		{
			elog(ERROR, "GC Layer: error sending to node %d of group "
				 "%s (%s)!\n", GC_NODE(node)->curr_view_rank,
				 group->name, strerror(errno));
		}

		pfree(b);
		pfree(castmsg);
	}
}

static void
ens_send_block_ok(const gcs_info *gcsi)
{
	buffer	b;
	char   *msg;
	int		size;
	int		err;

	size = 4 * sizeof(int);
	msg = (char*) palloc(size);
	init_buffer(&b, msg, size);

	/* write the message size header */
	put_int32(&b, 8);
	put_int32(&b, 0);

	/* write the block ok message header */
	put_int32(&b, GC_DATA(gcsi)->mid);
	put_int32(&b, ENS_DN_BLOCK_OK);

	/* send the message */
	err = send(GC_DATA(gcsi)->socket, msg, size, MSG_NOSIGNAL);
	if (err != size)
	{
		elog(ERROR, "GC Layer: error sending BLOCK_OK message (%s)!\n",
					 strerror(errno));
	}
}

void
ens_set_socks(const gcs_info *gcsi, fd_set *socks, int *max_socks)
{
	FD_SET(GC_DATA(gcsi)->socket, socks);
	if (GC_DATA(gcsi)->socket > *max_socks)
		*max_socks = GC_DATA(gcsi)->socket;
}

void
ens_buffer_fill(void *buf, size_t min, const void *obj)
{
	int bytes_read;
	buffer *b = buf;

	Assert(b->max_size - b->ptr > min);

	bytes_read = recv(GC_DATA((gcs_info*) obj)->socket,
					  (void*)((Pointer) b->data + b->fill_size),
					  b->max_size - b->fill_size, 0);
	b->fill_size += bytes_read;

	if (bytes_read <= 0)
	{
		elog(WARNING, "GC Layer: group communication system "
			 "terminated connection.");
		Assert(false);
	}
}

void
ens_send_buffered_data(gcs_info *gcsi)
{
	int err;
	buffer *b = &GC_DATA(gcsi)->send_buffer;

	err = send(GC_DATA(gcsi)->socket, b->data, b->fill_size, MSG_NOSIGNAL);
	if (err != b->fill_size)
	{
		elog(ERROR, "GC Layer: error sending buffered data (%s)!\n",
					 strerror(errno));
	}

	/* reset the send buffer */
	b->fill_size = 0;
	b->ptr = 0;
}

void
ens_handle_message(gcs_info *gcsi, const fd_set *socks)
{
	ens_message	   *msg;
	int				nmembers, origin, old_size, min_ptr;
	buffer		   *b = &GC_DATA(gcsi)->recv_buffer;

	char		   *str_version;
	char		   *str_group_name;
	char		   *str_proto;
	char		   *str_params;
	char		   *parsed_group_name;
	int				ltime;
	int				my_rank;
	int				start_of_msg;
	char		   *my_endpt;
	bool			primary, found;

	ens_node_in_view *view;

	int				count_addrs;
	int				count_endpts;
	int				i;

	gcs_group	   *group;
	ens_node	   *gc_node;
	group_node	   *node;

	HASH_SEQ_STATUS		hash_status;

	/* read from the socket at least once */
	Assert(FD_ISSET(GC_DATA(gcsi)->socket, socks));
	ens_buffer_fill((void*) b, sizeof(ens_message), gcsi);	

	while (1)
	{
		elog(DEBUG3, "GC Layer: remaining bytes in the buffer: %d",
			 b->fill_size - b->ptr);

		if ((b->fill_size - b->ptr) < sizeof(ens_message))
		{
			elog(DEBUG5, "GC Layer: bytes in the buffer: %d, "
				 "required: %lu (A)",
				 b->fill_size - b->ptr,
				 (unsigned long) sizeof(ens_message));
			return;
		}

		start_of_msg = b->ptr;
		msg = palloc(sizeof(ens_message));
		msg->header_size = get_int32(b);
		msg->data_size = get_int32(b);
		msg->member_id = get_int32(b);

		if (msg->member_id != GC_DATA(gcsi)->mid)
			elog(WARNING, "a message with different member id: %d "
				 "instead of %d", msg->member_id, GC_DATA(gcsi)->mid);

		msg->message_type = get_int32(b);

		if ((b->fill_size - b->ptr) < (msg->header_size - 8 + msg->data_size))
		{
			elog(DEBUG5, "GC Layer: bytes in the buffer: %d, "
				 "required %d (B)",
				 b->fill_size - b->ptr,
				 (msg->header_size - 8 + msg->data_size));
			pfree(msg);
			b->ptr = start_of_msg;
			return;
		}

		elog(DEBUG3, "GC Layer: received msg from GCS:");
		elog(DEBUG3, "          header_size: %d", msg->header_size);
		elog(DEBUG3, "          data_size: %d", msg->data_size);
		elog(DEBUG3, "          member_id: %d", msg->member_id);
		elog(DEBUG3, "          message_type: %d", msg->message_type);

		switch (msg->message_type)
		{
			case ENS_UP_VIEW:
				/* what to skip at the minimum */
				min_ptr = b->ptr + msg->header_size - 8 + msg->data_size;

				elog(DEBUG3, "      UP_VIEW message:");
				nmembers = get_int32(b);
				elog(DEBUG3, "          nmembers: %d", nmembers);

				view = palloc(sizeof(ens_node_in_view) * nmembers);

				str_version = get_p32string(b);
				elog(DEBUG3, "          version: '%s'", str_version);
				str_group_name = get_p32string(b);
				elog(DEBUG3, "          group name: '%s'", str_group_name);

				/*
				 * The str_group_name we get from ensemble is something
				 * like: "{Group:Named:markus:test}" for named groups. We
				 * never join anonymous groups, thus we assume to only get
				 * viewchange messages for named ones.
				 */
				Assert(strncmp("{Group:Named:", str_group_name, 13) == 0);

				/* skip the username, or whatever that is */
				for (i = 13; i < strlen(str_group_name); i++)
					if (str_group_name[i] == ':') break;
				i++;
				Assert(i < strlen(str_group_name));

				/* remove the ending '}' */
				str_group_name[strlen(str_group_name) - 1] = 0;

				parsed_group_name = &str_group_name[i];

				elog(DEBUG3, "          parsed group name: '%s'",
					 parsed_group_name);

				str_proto = get_p32string(b);
				elog(DEBUG3, "          protocol: '%s'", str_proto);
				ltime = get_int32(b);
				elog(DEBUG3, "          ltime: %d", ltime);
				primary = get_int32(b);
				elog(DEBUG3, "          primary: %d", primary);
				str_params = get_p32string(b);
				elog(DEBUG3, "          params: '%s'", str_params);

				count_addrs = get_int32(b);
				Assert(count_addrs == nmembers);
				for (i = 0; i < count_addrs; i++)
					view[i].addr = get_p32string(b);

				count_endpts = get_int32(b);
				Assert(count_endpts == nmembers);
				for (i = 0; i < count_endpts; i++)
					view[i].endpt = get_p32string(b);

				my_endpt = get_p32string(b);
				elog(DEBUG3, "          my endpt: '%s'", my_endpt);
				elog(DEBUG3, "          my addr: '%s'", get_p32string(b));
				my_rank = get_int32(b);
				elog(DEBUG3, "          my rank: %d", my_rank);
				elog(DEBUG3, "          my name: '%s'", get_p32string(b));
				elog(DEBUG3, "          view id: ltime: %d", get_int32(b));
				elog(DEBUG3, "                   endpt: '%s'",
					 get_p32string(b));

				/*
				 * Parse the actual view, comparing it with our current view,
				 * first removing nodes which obviously left, then adding new
				 * nodes.
				 */
				group = gc_get_group(gcsi, parsed_group_name);
				Assert(group);

				gcsi_viewchange_start(group);

				/* mark all nodes with an invalid rank */
				hash_seq_init(&hash_status, group->gcs_nodes);
				while ((gc_node = (ens_node*) hash_seq_search(&hash_status)))
					gc_node->curr_view_rank = -1;

				elog(DEBUG3, "          %d members:", nmembers);
				for (i = 0; i < nmembers; i++)
				{
					elog(DEBUG3, "          %d:", i);
					elog(DEBUG3, "             addr: '%s'", view[i].addr);
					elog(DEBUG3, "             endpt: '%s'", view[i].endpt);

					gc_node = hash_search(group->gcs_nodes, view[i].endpt,
									 	  HASH_ENTER, &found);

					if (!found)
					{
						/*
						 * we have just added a new node, tell the
						 * coordinator, too.
						 */
						node = gcsi_add_node(group, /* FIXME */ 0);
						node->gcs_node = gc_node;

						strlcpy(gc_node->ens_endpt, view[i].endpt,
								ENS_MAX_ENDPT_SIZE);
						/* FIXME: check return value of strlcpy */
					}
					else
					{
						node = gcsi_get_node(group, gc_node->gcsi_node_id);
						Assert(node);
					}

					gc_node->curr_view_rank = i;
					gc_node->gcsi_node_id = node->id;

					if ((i == my_rank) ||
						(strcmp(my_endpt, view[i].endpt) == 0))
					{
						Assert((strcmp(my_endpt, view[i].endpt) == 0));
						Assert(i == my_rank);
						elog(DEBUG3, "             (local node)");

						/* set the groups self_ref node_id */
						group->node_id_self_ref = node->id;
					}

					/*
					 * gcsi_node_changed() relies on a correctly set
					 * group->node_id_self_ref, thus we call it after setting
					 * that.
					 */
					if (!found)
						gcsi_node_changed(group, node, GCVC_JOINED);
				}

				/* remove all nodes, which still have an invalid rank */
				hash_seq_init(&hash_status, group->gcs_nodes);
				while ((gc_node = (ens_node*) hash_seq_search(&hash_status)))
				{
					if (gc_node->curr_view_rank < 0)
					{
						node = gcsi_get_node(group, gc_node->gcsi_node_id);
						Assert(node);
						gcsi_node_changed(group, node, GCVC_LEFT);
					}
				}

#ifdef DEBUG
				Assert(b->ptr == min_ptr);
#endif

				gcsi_viewchange_stop(group);
				pfree(view);

				GC_DATA(gcsi)->blocked = false;
				ens_send_buffered_data(gcsi);

				break;
			case ENS_UP_CAST:
				/* what to skip at the minimum */
				min_ptr = b->ptr + msg->header_size - 8 + msg->data_size;

				elog(DEBUG3, "      UP_CAST message:");
				origin = get_int32(b);
				elog(DEBUG3, "          origin: %d", origin);

				/* FIXME: ensemble should tell us the group name, no? */
				str_group_name = get_pstring(b);
				elog(DEBUG3, "          group: %s", str_group_name);

				group = gc_get_group(gcsi, str_group_name);
				Assert(group);

				/* get the sender node */
				hash_seq_init(&hash_status, group->gcs_nodes);
				while ((gc_node = (ens_node*) hash_seq_search(&hash_status)))
				{
					if (gc_node->curr_view_rank == origin)
						break;
				}

				if (gc_node)
				{
					node = gcsi_get_node(group, gc_node->gcsi_node_id);
					Assert(node);

					elog(DEBUG3, "          sender endpt: %s",
						 gc_node->ens_endpt);

					/*
					 * call the coordinator handle function
					 *    the handle function depends on a buffer with
					 *    correct fill_size parameter.
					 */
					old_size = b->fill_size;
					b->fill_size = msg->data_size - strlen(str_group_name) -
								   1 + b->ptr;
					coordinator_handle_gc_message(group, node, 'T', b);
					b->fill_size = old_size;
				}
				else
				{
					elog(DEBUG3, "GC Layer: node %d unknown, "
						 "message ignored!", origin);
				}

				/* check if enough data is skipped */
				if (b->ptr < min_ptr)
				{
					elog(DEBUG3,
						"GC Layer: skipping %d bytes of data",
						min_ptr - b->ptr);
					b->ptr = min_ptr;
				}

#ifdef DEBUG
				Assert(b->ptr == min_ptr);
#endif
				break;

			case ENS_UP_BLOCK:
				elog(DEBUG3, "      BLOCK message.");

				/* what to skip at the minimum */
				min_ptr = b->ptr + msg->header_size - 8 + msg->data_size;

				GC_DATA(gcsi)->blocked = true;
				ens_send_block_ok(gcsi);
				elog(DEBUG3, "        -> sent BLOCK_OK.");

#ifdef DEBUG
				Assert(b->ptr == min_ptr);
#endif
				break;

			case ENS_UP_SEND:
				/* what to skip at the minimum */
				min_ptr = b->ptr + msg->header_size - 8 + msg->data_size;

				elog(DEBUG3, "      UP_SEND message:");
				origin = get_int32(b);
				elog(DEBUG3, "          origin: %d", origin);

				/* FIXME: ensemble should tell us the group name, no? */
				str_group_name = get_pstring(b);
				elog(DEBUG3, "          group: %s", str_group_name);

				group = gc_get_group(gcsi, str_group_name);
				Assert(group);

				/* get the sender node */
				hash_seq_init(&hash_status, group->gcs_nodes);
				while ((gc_node = (ens_node*) hash_seq_search(&hash_status)))
				{
					if (gc_node->curr_view_rank == origin)
						break;
				}

				if (gc_node)
				{
					node = gcsi_get_node(group, gc_node->gcsi_node_id);
					Assert(node);

					elog(DEBUG3, "          sender endpt: %s",
						 gc_node->ens_endpt);

					/*
					 * call the coordinator handle function
					 *    the handle function depends on a buffer with correct
					 *    fill_size parameter.
					 */
					old_size = b->fill_size;
					b->fill_size = msg->data_size - strlen(str_group_name) -
								   1 + b->ptr;
					coordinator_handle_gc_message(group, node, 'T', b);
					b->fill_size = old_size;
				}
				else
				{
					elog(DEBUG3, "GC Layer: node %d unknown, "
						 "message ignored!", origin);
				}

				/* check if enough data is skipped */
				if (b->ptr < min_ptr)
				{
					elog(DEBUG3,
						"GC Layer: skipping %d bytes of data",
						min_ptr - b->ptr);
					b->ptr = min_ptr;
				}

#ifdef DEBUG
				Assert(b->ptr == min_ptr);
#endif
				break;

			case ENS_UP_EXIT:
				elog(ERROR, "      EXIT message (NYI)!");
				break;

			default:
				Assert(0);
		}

		pfree(msg);

		/*
		 * Recycle the receive buffer if possible.
		 */
		if (b->fill_size == b->ptr)
		{
			b->fill_size = 0;
			b->ptr = 0;
		}
	}
}

bool
ens_is_local(const gcs_group *group, const group_node *node)
{
	group_node *self_node = gcsi_get_node(group, group->node_id_self_ref);
	Assert(self_node);
	return (strcmp(GC_NODE(node)->ens_endpt,
				   GC_NODE(self_node)->ens_endpt) == 0);
}

