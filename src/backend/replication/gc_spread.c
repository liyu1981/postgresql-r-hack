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

#include <sp.h> /* spread header */

#define GC_DATA(gcsi) ((spread_data*)((gcsi)->data))
#define GC_NODE(node) ((spread_node*)((node)->gcs_node))

#define RECV_BUFFER_SIZE 2048
#define MAX_MEMBERS      100

typedef enum
{
	SPS_INITIALIZING = 0,
	SPS_AUTHENTICATING = 1,
	SPS_READY = 12
} spread_state;

typedef struct
{
	char    spread_name[MAX_GROUP_NAME];
	mailbox mbox;
	char    group_name[MAX_GROUP_NAME];
	char    private_group_name[MAX_GROUP_NAME];

	spread_state state;

	/* receive related stuff */
	buffer recv_buffer;
	char   sender[MAX_GROUP_NAME];
	char   target_groups[MAX_MEMBERS][MAX_GROUP_NAME];
	char   members[MAX_MEMBERS][MAX_GROUP_NAME];
	int    num_groups;
	int    service_type;
	int16  mess_type;
	int    endian_mismatch;
} spread_data;

typedef struct
{
	char private_group_name[MAX_GROUP_NAME];

	/* each node_id, calculated based on private_group_name */
	uint32      id;

	/* the coordinator node_id */
	uint32		node_id;
} spread_node;


/* prototypes */
uint32 pgn2id(const char *name);

void spread_recv(gcs_info *gcsi);
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


uint32 
pgn2id(const char *name)
{
	/* we assume that the name will not exceed the boundary of uint32 currently */
	int i=0;
	char c;
	uint32 r = 0;

	Assert(name != NULL);
	Assert(name[0] != '\0');

	for(c = name[i]; c != '\0'; ++i)
		r += (int)c;

	return r;
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

	strcpy(GC_DATA(gcsi)->spread_name, "4803");
}

void
spread_recv(gcs_info *gcsi)
{
	int     err;

	err = SP_poll(GC_DATA(gcsi)->mbox);
	if(err > 0)
	{
		err = SP_receive(GC_DATA(gcsi)->mbox,
		                 &GC_DATA(gcsi)->service_type,
		                 GC_DATA(gcsi)->sender,
		                 MAX_MEMBERS,
		                 &GC_DATA(gcsi)->num_groups,
		                 GC_DATA(gcsi)->target_groups,
		                 &GC_DATA(gcsi)->mess_type,
		                 &GC_DATA(gcsi)->endian_mismatch,
		                 RECV_BUFFER_SIZE,
		                 (char *)(GC_DATA(gcsi)->recv_buffer.data));

		if(err < 0)
		{
			if(err == GROUPS_TOO_SHORT || err == BUFFER_TOO_SHORT)
			{
				elog(WARNING, "GC Layer: buffers or groups too short while %s receive msg.",
				     GC_DATA(gcsi)->private_group_name);
				elog(WARNING, "GC Layer:   retry with DROP_RECV.");
				GC_DATA(gcsi)->service_type = DROP_RECV;
				err = SP_receive(GC_DATA(gcsi)->mbox,
				                 &GC_DATA(gcsi)->service_type,
				                 GC_DATA(gcsi)->sender,
				                 MAX_MEMBERS,
				                 &GC_DATA(gcsi)->num_groups,
				                 GC_DATA(gcsi)->target_groups,
				                 &GC_DATA(gcsi)->mess_type,
				                 &GC_DATA(gcsi)->endian_mismatch,
				                 RECV_BUFFER_SIZE,
				                 (char *)(GC_DATA(gcsi)->recv_buffer.data));
			}
			else
			{
				elog(WARNING, "GC Layer: error %d while %s receive msg.", err, GC_DATA(gcsi)->private_group_name);
			}
		}
		else
		{
			elog(DEBUG3, "GC Layer: %s received %d bytes.", GC_DATA(gcsi)->private_group_name, err);
		}
	}
	else
	{
		/* no message waiting or error occured, just return */
		elog(DEBUG4, "GC Layer: no msg currently.");
		return;
	}
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
		    gcsi->conn_state = GCSCS_ESTABLISHED;
		    elog(LOG, "GC Layer: connection established to spread daemon %s with private name %s.",
		         GC_DATA(gcsi)->spread_name,
		         GC_DATA(gcsi)->private_group_name);
		    gcsi_gcs_ready(gcsi);
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
		    break;
	}
}

void
spread_disconnect(gcs_info *gcsi)
{
	SP_disconnect(GC_DATA(gcsi)->mbox);
	pfree(gcsi->data);
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
		    elog(ERROR, "GC Layer: unknown error %d in joining group.", err);
		    break;
	}

	new_group = gc_create_group(gcsi, full_group_name, sizeof(int),
								sizeof(spread_node));
	new_group->parent = parent_group;
	
	strcpy(GC_DATA(gcsi)->group_name, group_name); 

	elog(LOG, "GC Layer: joined group %s.", group_name);

	return new_group;
}

void
spread_leave(gcs_group *group)
{
	Assert(group);
	Assert(group->gcsi);
	Assert(group->gcsi->conn_state == GCSCS_ESTABLISHED);

	SP_leave(GC_DATA(group->gcsi)->mbox, group->name);
	gc_destroy_group(group);
}

void
spread_broadcast(const gcs_group *group, const void *data, int size,
				 bool atomic)
{
	int       err;
	service   st;
	gcs_info *gcsi = group->gcsi;

	Assert(gcsi->conn_state == GCSCS_ESTABLISHED);

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
		    elog(WARNING, "GC Layer: error in broadcast - illegal session.");
	    case ILLEGAL_MESSAGE:
		    elog(WARNING, "GC Layer: error in broadcast - illegal message.");
	    case CONNECTION_CLOSED:
		    /* FIXME: try think again, really need to report ERROR ? */
		    elog(ERROR, "GC Layer: error in broadcast - connection closed.");
		    break;
	    default:
		    if(err >= 0)
			    elog(DEBUG4, "GC Layer: %d bytes broadcast.", err);
		    else
		    {
			    /* FIXME: try think again, really need to report ERROR ? */
			    elog(ERROR, "GC Layer: unknown error %d in broadcast.", err);
		    }
	}
}

void
spread_unicast(const gcs_group *group, const group_node *node,
			   const void *data, int size)
{
	int       err;
	gcs_info *gcsi = group->gcsi;

	Assert(gcsi->conn_state == GCSCS_ESTABLISHED);

	err = SP_multicast(GC_DATA(gcsi)->mbox,
	                   RELIABLE_MESS,
	                   GC_NODE(node)->private_group_name,
	                   0,
	                   size,
	                   (const char*)data);
	switch(err)
	{
	    case ILLEGAL_SESSION:
		    elog(WARNING, "GC Layer: error in broadcast - illegal session.");
	    case ILLEGAL_MESSAGE:
		    elog(WARNING, "GC Layer: error in broadcast - illegal message.");
	    case CONNECTION_CLOSED:
		    /* FIXME: try think again, really need to report ERROR ? */
		    elog(ERROR, "GC Layer: error in broadcast - connection closed.");
		    break;
	    default:
		    if(err >= 0)
			    elog(DEBUG4, "GC Layer: %d bytes broadcast.", err);
		    else
		    {
			    /* FIXME: try think again, really need to report ERROR ? */
			    elog(ERROR, "GC Layer: unknown error %d in broadcast.", err);
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
	int              err;
	service          st = GC_DATA(gcsi)->service_type;
	gcs_group       *group;
	group_node      *node;
	membership_info  memb_info;
	buffer          *b  = &(GC_DATA(gcsi)->recv_buffer);

	if(Is_regular_mess(st))
	{
		Assert(GC_DATA(gcsi)->num_groups >= 1);
		if(strcmp(GC_DATA(gcsi)->target_groups[0], 
		          GC_DATA(gcsi)->private_group_name) == 0)
		{
			/* since match the private_group_name, it is from unicast */
			group = gc_get_group(gcsi, GC_DATA(gcsi)->group_name);
			Assert(group);
			node = hash_search(group->nodes, pgn2id(&GC_DATA(gcsi)->sender),
			                   HASH_FIND, NULL);
			Assert(node);
			coordinator_handle_gc_message(group, node, 'F', b);
		}
		else
		{
			/* we assume msgs are sent to only one group currently */
			group = gc_get_group(gcsi, GC_DATA(gcsi)->target_groups[0]);
			Assert(group);
			node = hash_search(group->nodes, pgn2id(&GC_DATA(gcsi)->sender),
			                   HASH_FIND, NULL);
			Assert(node);
			coordinator_handle_gc_message(group, node, 'T', b);
		}
	}
	else if(Is_membership_mess(st))
	{
		err = SP_get_memb_info(GC_DATA(gcsi)->recv_buffer.data, st, &memb_info);
		if(err < 0)
		{
			elog(WARNING, "GC Layer: membership message does not have valid body.");
			/* FIXME: this is serious, then what? */
		}
		else
		{
			group = gc_get_group(gcsi, GC_DATA(gcsi)->target_groups[0]);
			Assert(group);
			node = hash_search(group->nodes, pgn2id(&GC_DATA(gcsi)->sender),
			                   HASH_FIND, NULL);
			Assert(node);

			gcsi_viewchange_start(group);
			if(Is_caused_join_mess(st))
			{
				gcsi_node_changed(group, node, GCVC_JOINED);
			}
			else if(Is_caused_leave_mess(st))
			{
				gcsi_node_changed(group, node, GCVC_LEFT);
			}
			else if(Is_caused_disconnect_mess(st))
			{
				gcsi_node_changed(group, node, GCVC_LEFT);
			}
			else if(Is_caused_network_mess(st))
			{
				elog(WARNING, "GC Layer: got membership message but caused by network.");
			}
			gcsi_viewchange_stop(group);
		}
	}
	else if(Is_transition_mess(st))
	{
		elog(DEBUG3, "GC Layer: got transition membership message for group %s.", GC_DATA(gcsi)->sender);
	}
	else if(Is_reject_mess(st))
	{
		elog(DEBUG3, 
		     "GC Layer: REJECTED msg from %s, of servicetype 0x%x messtype %d, (endian %d) to %d groups : %s",
		     GC_DATA(gcsi)->sender, 
		     GC_DATA(gcsi)->service_type, 
		     GC_DATA(gcsi)->mess_type, 
		     GC_DATA(gcsi)->endian_mismatch, 
		     GC_DATA(gcsi)->num_groups, 
		     b->data);
	}
	else
	{
		/* liyu: this can NOT be ERROR, since ERROR will cause us jump
		   back to coordinator.c:738, which turns out will continue to
		   reinvoke populate_co_database ...  ( sort of sigsetjmp and
		   siglongjmp programming, really bad in PG ... :( )
		   
		   so, brief conclusion here: 

		   1.  report ERROR will cause the coordinator try to
		       re-populate_co_database and re-connect-join spread. If
		       you feel it is a serious error in spread, just report
		       it 

		   2.  otherwise, do not report ERROR...
		 */
		elog(WARNING, "GC Layer: unknown message type 0x%x received.", st);
	}
}

bool
spread_is_local(const gcs_group *group, const group_node *node)
{
	group_node* n = hash_search(group->nodes, pgn2id(GC_NODE(node)->private_group_name),
	                            HASH_FIND, NULL);
	return n != NULL;
}

