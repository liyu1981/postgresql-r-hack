/*-------------------------------------------------------------------------
 *
 * gc.h
 *
 *	  Group communication system interfaces.
 *
 * Copyright (c) 2006-2010, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#ifndef GC_H
#define GC_H

#include "replication/replication.h"

/*
 * Inteface to the group communication system layers.
 */
struct s_gcs_info;
struct co_database;

typedef struct s_group_node
{
	/* the coordinator node_id, unique within a group */
	NodeId		id;

	/* database state of this node */
	rdb_state	state;

	/* pointer to the GCS node info */
	void	   *gcs_node;
} group_node;

typedef void (gc_connect_func) (struct s_gcs_info *gcdata);
typedef void (gc_disconnect_func) (struct s_gcs_info *gcdata);
typedef void (gc_set_socks_func) (const struct s_gcs_info *gcsi,
								  fd_set *socks, int *max_socks);
typedef void (gc_handle_message_func) (struct s_gcs_info *gcsi,
								       const fd_set *socks);

typedef gcs_group *(gc_join_func) (struct s_gcs_info *gcsi,
								   const char *group_name,
								   gcs_group *parent_group);
typedef void (gc_leave_func) (gcs_group *group);
typedef group_node *(gc_get_local_node_func) (const gcs_group *group);
typedef bool (gc_is_local_func) (const gcs_group *group,
								 group_node const *node);

typedef void (gc_broadcast_func) (const gcs_group *group, const void *data,
								  const int size, bool atomic);
typedef void (gc_unicast_func) (const gcs_group *group,
								const group_node *node, const void *data,
								const int size);
#ifdef COORDINATOR_DEBUG
typedef void (gc_get_node_desc) (const gcs_group *group,
								 group_node const *node, char *str);
#endif

typedef struct
{
	gc_connect_func		   *connect;
	gc_disconnect_func	   *disconnect;
	gc_set_socks_func	   *set_socks;
	gc_handle_message_func *handle_message;

	gc_join_func		   *join;
	gc_leave_func		   *leave;
	gc_get_local_node_func *get_local_node;
	gc_is_local_func	   *is_local;
#ifdef COORDINATOR_DEBUG
	gc_get_node_desc       *get_node_desc;
#endif

	gc_broadcast_func	   *broadcast;
	gc_unicast_func	       *unicast;
} gcs_funcs;


typedef enum {
	GCVC_JOINED = 'J',
	GCVC_LEFT = 'L'
} gcs_viewchange_type;

typedef enum {
	GCSCS_DOWN = 'D',
	GCSCS_REQUESTED = 'R',
	GCSCS_ESTABLISHED = 'E'
} gcs_conn_state;

typedef struct s_gcs_info {
	Oid						oid;
	HTAB				   *groups;
	gcs_conn_state          conn_state;
	gcs_funcs				funcs;
	int						connection_tries;
	void				   *data;
} gcs_info;

/* utility routines for the coordinator */
extern char **gc_parse_params(char *str);

/* utility routines for the GCS interfaces */
extern gcs_group *gc_create_group(gcs_info *gcsi, const char *group_name,
								  int ksize, int esize);
extern void gc_destroy_group(gcs_group *group);
extern gcs_group *gc_get_group(const gcs_info *gcsi, const char *group_name);
extern void gc_init_groups_hash(gcs_info *gcsi);

/* initialization routines per GCS */
extern void egcs_init(gcs_info *gcsi, char **params);
extern void ens_init(gcs_info *gcsi, char **params);
extern void spread_init(gcs_info *gcsi, char **params);

#endif   /* GC_H */
