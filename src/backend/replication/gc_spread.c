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
#define GC_NODE(node) ((spread_node*)((node)->gcs_node))

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
	char spread_name[MAX_PRIVATE_NAME];
	mailbox mbox;
	char private_group_name[MAX_GROUP_NAME_SIZE];

	buffer recv_buffer;
	spread_state state;
} spread_data;

typedef struct
{
	/* private_group_name got by connect to spread daemon, as id to each connectioin */
	char unicast_group_name[MAX_GROUP_NAME_SIZE];

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
	int     err;
	buffer *b = &GC_DATA(gcsi)->recv_buffer;

	err = SP_poll(GC_DATA(gcsi)->mbox);
	if(err > 0)
	{
		
	}
	else
	{
		/* no message waiting or error occured, just return */
		return;
	}
}

void
spread_init(gcs_info *gcsi, char **params)
{
	gcsi->data = palloc(sizeof(spread_data));
	init_buffer(&GC_DATA(gcsi)->recv_buffer, palloc(RECV_BUFFER_SIZE),
	            RECV_BUFFER_SIZE);
	gc_init_groups_hash(gcsi);

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

	GC_DATA(gcsi)->spread_name = "4803";
}

void
spread_connect(gcs_info *gcsi)
{
	int		err;

	Assert(gcsi);
	Assert(gcsi->conn_state == GCSCS_DOWN);

	elog(DEBUG3, "GC Layer: connecting to the spread daemon");

	/* set correct state */
	GC_DATA(gcsi)->state = SPS_INITIALIZING;

	err = SP_connect(GC_DATA(gcsi)->spread_name,         /* spread_name: local daemon @ 4803 will be connected */ 
	                 NULL,                               /* private_name: let spread gives one, so use NULL */
	                 0,                                  /* priority: this has no effect according to spread document */
	                 1,                                  /* group_membership: 1 - yes, we want membership messages */
	                 &GC_DATA(gcsi)->mbox,               /* mbox: */
	                 GC_DATA(gcsi)->private_group_name);

	switch(err) 
	{
	    case ACCEPT_SESSION:
		    elog(DEBUG3, "GC Layer: accept seesion.");
		    break;
	    case ILLEGAL_SPREAD:
		    elog(ERROR, "GC Layer: connect error - illegal spread.");
	    case COULD_NOT_CONNECT:
		    elog(ERROR, "GC Layer: connect error - could not connect.");
	    case CONNECTION_CLOSED:
		    elog(ERROR, "GC Layer: connect error - connection closed.");
	    case REJECT_VERSION:
		    elog(ERROR, "GC Layer: connect error - reject version.");
	    case REJECT_NO_NAME:
		    elog(ERROR, "GC Layer: connect error - reject no name.");
	    case REJECT_ILLEGAL_NAME:
		    elog(ERROR, "GC Layer: connect error - reject illegal name.");
	    case REJECT_NOT_UNIQUE:
		    elog(ERROR, "GC Layer: connect error - reject not unique.");
	    default:
		    gcsi_gcs_failed(gcsi);
		    return;
	}

	gcsi->conn_state == GCSCS_REQUESTED;
}

void
spread_disconnect(gcs_info *gcsi)
{
	SP_disconnect(GC_DATA(gcsi)->mbox);
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
	int        err;
	gcs_group *new_group;
	gcs_group *pgroup;
	int		   i, j, full_group_name_len;
	char	  *full_group_name;

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

	err = SP_join(GC_DATA(gcsi)->mbox, full_group_name);

	switch(err)
	{
	    case 0:
		    break;
	    case ILLEGAL_GROUP:
		    elog(ERROR, "GC Layer: error in joining group: illegal group.");
	    case ILLEGAL_SESSION:
		    elog(ERROR, "GC Layer: error in joining group: illegal session.");
	    case CONNECTION_CLOSED:
		    elog(ERROR, "GC Layer: error in joining group: connection closed.");
	    default:
		    /* FIXME: */
		    break;
	}

	new_group = gc_create_group(gcsi, full_group_name, sizeof(int),
								sizeof(spread_node));
	new_group->parent = parent_group;

	return new_group;
}

void
spread_leave(gcs_group *group)
{
	Assert(group);
	Assert(group->gcsi);
	Assert(group->gcsi->conn_state == GCSCS_ESTABLISHED);

	SP_leave(GC_DATA(gcsi)->mbox, group->name);
	gc_destroy_group(group);
}

void
spread_broadcast(const gcs_group *group, const void *data, int size,
				 bool atomic)
{
	int     err;
	service st;

	Assert(group->gcsi->conn_state == GCSCS_ESTABLISHED);

	if(atomic)
		st = RELIABLE_MESS;
	else
		st = UNRELIABLE_MESS;

	err = SP_multicast(GC_DATA(gcsi)->mbox,
	                   st,
	                   group->name,
	                   0, /* mess_type: short int, indicate what the message is, not used now */
	                   size,
	                   (const char*)data);

	switch(err)
	{
	    case ILLEGAL_SESSION:
		    elog(ERROR, "GC Layer: error in broadcast - illegal session.");
	    case ILLEGAL_MESSAGE:
		    elog(ERROR, "GC Layer: error in broadcast - illegal message.");
	    case CONNECTION_CLOSED:
		    elog(ERROR, "GC Layer: error in broadcast - connection closed.");
		    /* FIXME: so what? */
		    break;
	    default:
		    if(err >= 0)
			    elog(DEBUG4, "GC Layer: %d bytes broadcast.", err);
		    else
		    {
			    elog(ERROR, "GC Layer: unknown error %d in broadcast.", err);
			    /* FIXME: so what? */
		    }
	}
}

void
spread_unicast(const gcs_group *group, const group_node *node,
			   const void *data, int size)
{
	int     err;
	service st;

	err = SP_multicast(GC_DATA(gcsi)->mbox,
	                   RELIABLE_MESS,
	                   GC_NODE(node)->unicast_group_name,
	                   0,
	                   size,
	                   (const char*)data);
	switch(err)
	{
	    case ILLEGAL_SESSION:
		    elog(ERROR, "GC Layer: error in broadcast - illegal session.");
	    case ILLEGAL_MESSAGE:
		    elog(ERROR, "GC Layer: error in broadcast - illegal message.");
	    case CONNECTION_CLOSED:
		    elog(ERROR, "GC Layer: error in broadcast - connection closed.");
		    /* FIXME: so what? */
		    break;
	    default:
		    if(err >= 0)
			    elog(DEBUG4, "GC Layer: %d bytes broadcast.", err);
		    else
		    {
			    elog(ERROR, "GC Layer: unknown error %d in broadcast.", err);
			    /* FIXME: so what? */
		    }
	}
}

void
spread_set_socks(const gcs_info *gcsi, fd_set *socks, int *max_socks)
{
	/* spread does not use socks for communication, so nothing to do */
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

