/*-------------------------------------------------------------------------
 *
 * gc_utils.c
 *
 *	  Utility functions used by multiple GCS interfaces.
 *
 * Copyright (c) 2006-2010, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include <unistd.h>

#include "postgres.h"
#include "utils/hsearch.h"
#include "replication/gc.h"
#include "storage/buffer.h"
#include "storage/imsg.h"

void
gc_init_groups_hash(gcs_info *gcsi)
{
	HASHCTL hash_ctl;

	/* init the groups hash */
	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = NAMEDATALEN;
	hash_ctl.entrysize = sizeof(gcs_group);
	gcsi->groups = hash_create("GC Layer: groups",
							   8, &hash_ctl, HASH_ELEM);
}

char **
gc_parse_params(char *str)
{
	char *pos = str, *start = str;
	int params_pos = 0, params_max = 16;
	char **result = palloc0(params_max * sizeof(char*));

	/* skip all leading ':' */
	while (*pos == ':')
		pos++;

	while (*pos != '\0')
	{
		if (*pos == ';' || *pos == '=')
		{
			*pos = 0;
			result[params_pos] = start;
			params_pos++;
			pos++;
			start = pos;
		}
		else
		{
			pos++;
		}
	}

	return result;
}

gcs_group *
gc_create_group (gcs_info *gcsi, const char *group_name,
				 int ksize, int esize)
{
	bool		found;
	HASHCTL		hash_ctl;
	gcs_group  *new_group;

	new_group = hash_search(gcsi->groups, group_name, HASH_ENTER,
							&found);

	Assert(!found);

	strncpy(new_group->name, group_name, NAMEDATALEN);
	new_group->gcsi = gcsi;
	new_group->dboid = InvalidOid;
	new_group->parent = NULL;

	new_group->coid = FirstNormalCommitOrderId;
	new_group->db_state = RDBS_UNKNOWN;
	new_group->rstate = NULL;

	/* create the gcs_nodes hash table */
	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = ksize;
	hash_ctl.entrysize = esize;
	new_group->gcs_nodes = hash_create("GCS Layer: group gcs nodes",
									   32, &hash_ctl, HASH_ELEM);

	/* create the nodes hash table */
	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(uint32);
	hash_ctl.entrysize = sizeof(group_node);
	hash_ctl.hash = oid_hash;
	new_group->nodes = hash_create("GCS Layer: group coordinator nodes",
								   32, &hash_ctl,
								   HASH_ELEM | HASH_FUNCTION);

	new_group->num_nodes_operating = 0;
	new_group->last_vc_remotes_joined = 0;
	new_group->last_vc_remotes_left = 0;

	new_group->last_vc_join_pending = false;
	new_group->last_vc_leave_pending = false;

	return new_group;
}

void
gc_destroy_group(gcs_group *group)
{
	bool	found;

	/* destroy the node hashes */
	hash_destroy(group->nodes);
	hash_destroy(group->gcs_nodes);

	hash_search(group->gcsi->groups, group->name, HASH_REMOVE,
				&found);
	Assert(found);
}

gcs_group *
gc_get_group(const gcs_info *gcsi, const char *group_name)
{
	return hash_search(gcsi->groups, group_name, HASH_FIND, NULL);
}
