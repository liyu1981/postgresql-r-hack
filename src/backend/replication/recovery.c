/*-------------------------------------------------------------------------
 *
 * recovery.c
 *
 *	  Replication recovery routines.
 *
 * Portions Copyright (c) 2010, Translattice, Inc
 * Portions Copyright (c) 2005-2010, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 *
 *
 * A node which joins a group needs to recover, either by requesting and
 * applying a full database dump from the operating nodes (initialization)
 * or by applying all missed changesets since it left the group.
 *
 * As that's not an atomic operation, but can take quite some time, there
 * are multiple difficulties involved: first, the operating nodes continue
 * to process transactions, thus they constantly create more data to
 * recover. Second, at any time, any other node might fail, thus a recovery
 * subscriber must be prepared to interrupt recovery and continue with
 * another recovery provider.
 *
 * To adress those problems, the recovery process is split into three
 * stages: schema transmission, schema adaption and data recovery. Schema
 * transmission and adaption should be as universal as possible, meaning it
 * should not be bound to a certain Postgres or system catalog version. As
 * the schema is much more static than the data, we simply restart schema
 * transmission, if the schema changes in between or if we loose connection
 * to the recovery subscriber during transmission. To ensure consistency
 * and keep it simple, we decide on a single schema recovery provider and
 * request all schema information from that one node.
 *
 * After schema transmission, we have a full copy of a remote schema,
 * including it's local oids. A single schema recovery backend then does
 * the adoption and applies all schema changes necessary, so that our local
 * schema equals the remote one. Keeping parallelism out of this stage
 * keeps it simple enough.
 *
 * Once the schema transmission and adoption is completed, a node does not
 * only start to recover data, but also tries to replay transactions, using
 * MVCC and a limit on the primary key to distinguish between tuples which
 * have already been recovered and tuples which have not. This avoids having
 * to queue lots of changesets on the subscriber and prevents long running
 * transactions on the provider.
 *
 * When replaying a remote transaction for a tuple, which has a primary
 * key below that recovery limit, we can simply perform the requested
 * operation as normal, because we know it's fully recovered data.
 *
 * Otherwise we don't have the old tuple which needs to get changed by the
 * transaction to be replicated. Without that, we cannot apply the
 * changeset. Thus we have two options: either we wait for the recovery
 * process to provide us with the underlying (old) data and apply the
 * changeset then, or we discard the change for that tuple and make sure
 * recovery later provides us the new tuple right away (instead of the
 * old one).
 *
 * To simplify changeset application during recovery, we defer application
 * slightly. We maintain an lower limit of the global transaction ids of
 * the RECOVERY_DATA packets. All future packets will contain data from
 * snapshots newer than that. Additionally, we keep track of the upper
 * limit of the pkey of recovered data. By deferring changesets, until the
 * lower gid bound is above the changeset's gid, we can be sure that every
 * tuple to be changed by the changeset is in one of the following three
 * states:
 *
 *  - way below the pkey upper bound, already recovered to a gid before
 *    the changeset. We can safely apply changes from the changeset to
 *    that tuple.
 *  - slightly below the pkey upper bound, already recovered to a gid
 *    equal or above the changeset's. We can ignore that change, since
 *    recovery has already written it.
 *  - above the pkey boundary. We can ignore that change as well, because
 *    recovery will take take of it later on.
 *
 * Pretty much the same applies for new tuples in the changeset from
 * INSERT operations. Of course, changesets still need to be applied in
 * correct order. Additionally, a node needs to await remote confirmation
 * or abortion of the changeset, as it does not have enough data to
 * decide itself.
 */

#include <unistd.h>
#include <errno.h>

#include "postgres.h"
#include "miscadmin.h"

#include "access/heapam.h"
#include "access/genam.h"
#include "access/relscan.h"
#include "access/xact.h"
#include "catalog/catversion.h"
#include "catalog/namespace.h"
#include "catalog/indexing.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/nodeModifyTable.h"
#include "executor/spi.h"
#include "mb/pg_wchar.h"
#include "nodes/makefuncs.h"
#include "parser/parse_oper.h"
#include "postmaster/autovacuum.h"
#include "replication/coordinator.h"
#include "replication/cset.h"
#include "replication/replication.h"
#include "replication/recovery.h"
#include "replication/utils.h"
#include "storage/imsg.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#define MAX_RECOVERY_CHUNK_SIZE 8100
#define SCHEMA_RECOVERY_LANES 1
#define DATA_RECOVERY_LANES 1

/* global to pass around current recovery state */
recovery_request_info *CurrentRecoveryRequest;

static Oid relationGetPrimaryKeyIndexOid(Relation rel);

static repl_recovery_state *new_recovery_state(void);
static void gc_send_recovery_request(gcs_group *group, group_node *node,
									 recovery_request_info *ri);
static void InitiateRelationRecovery(const char *schemaname,
                                     const char *relname, int round_no,
									 int num_lanes);

static void initialize_database_schema_transfer(int round_no);
static void initialize_shared_schema_transfer(int round_no);

static void SendRelationRecoveredMsg(const char *schemaname,
									 const char *relname,
									 int round_no, int lane_no,
									 int num_lanes);
static void SendRecoveryRequest(const char *schemaname, const char *relname,
                                int round_no, int lane_no, int num_lanes,
								HeapTuple tuple, const IndexInfo *indexInfo,
								TupleDesc tdesc);




/**************************************************************************
 * recovery functions for the coordinator
 **************************************************************************/

repl_recovery_state *
new_recovery_state(void)
{
	repl_recovery_state *rs;

	rs = (repl_recovery_state *) palloc(sizeof(repl_recovery_state));

	rs->round_no = 1;
	DLInitList(&rs->request_list);
	rs->recovery_backend_id = InvalidBackendId;

	rs->open_streams = 0;
	rs->total_requests = 0;

	return rs;
}

void
gc_send_recovery_request(gcs_group *group, group_node *node,
						 recovery_request_info *ri)
{
	char	   *gc_msg;
	buffer		b;
	int			size = 1 + ri->msg->size;

	gc_msg = palloc(size);
	init_buffer(&b, gc_msg, size);

	put_int8(&b, IMSGT_RECOVERY_REQUEST);
	put_data(&b, IMSG_DATA(ri->msg), ri->msg->size);

	IMessageRemove(ri->msg);

	Assert(size <= 65536);
	group->gcsi->funcs.unicast(group, node, gc_msg, size);
	pfree(gc_msg);
}

/*
 *    coordinator_restart_schema_recovery
 *
 * After a viewchange (during which recovery is interrupted) or after a
 * schema update, we restart schema recovery.
 */
void
coordinator_restart_schema_recovery(gcs_group *group)
{
	co_database *codb;
	IMessage *msg;

	Assert(group->db_state == RDBS_RECOVERING_SCHEMA);

	if (!group->rstate)
	{
#ifdef COORDINATOR_DEBUG
		elog(DEBUG5, "Coordinator: starting recovery for database %d",
			 group->dboid);
#endif

		group->rstate = new_recovery_state();
	}
	else
	{
#ifdef COORDINATOR_DEBUG
		elog(DEBUG5, "Coordinator: restarting recovery for database %d",
			 group->dboid);
#endif

		/* we don't currently support restarting schema recovery... */
		Assert(false);

		group->rstate->round_no++;

		/*
		 * As we are restarting, there must already be a recovery
		 * helper backend.
		 */
		Assert(group->rstate->recovery_backend_id != InvalidBackendId);

		/*
		 * FIXME: there's a little race condition for restarting of schema
		 *        recovery: we need to make sure that no RECOVERY_DATA
		 *        from an earlier recovery round arrives *after* having
		 *        restarted schema recovery (and thus recreated the
		 *        pg_remote_catalog schema).
		 */
	}

	/*
	 * Prepare a buffer to a schema recovery restart command message.
	 */
	msg = IMessageCreate(IMSGT_RECOVERY_RESTART, 0);

	LWLockAcquire(CoordinatorDatabasesLock, LW_SHARED);
	codb = hash_search(co_databases, &group->dboid, HASH_FIND, NULL);
	Assert(codb);
	dispatch_job(msg, codb);
	LWLockRelease(CoordinatorDatabasesLock);
}

/*
 *    coordinator_restart_data_recovery
 *
 * After a viewchange (during which recovery is interrupted) we check
 * the current state and start or continue recovering.
 */
void
coordinator_restart_data_recovery(gcs_group *group)
{
	Assert(group);
	Assert(group->db_state == RDBS_RECOVERING_DATA);
	Assert(group->rstate);

#ifdef COORDINATOR_DEBUG
	elog(DEBUG2, "Coordinator: restarting data recovery for database %d",
		 group->dboid);
#endif

	coordinator_fire_recovery_requests(group);
}

void
coordinator_stop_data_recovery(gcs_group *group)
{
	Assert(group);

	pfree(group->rstate);
	group->rstate = NULL;
}

void
coordinator_fire_recovery_requests(gcs_group *group)
{
	Dlelem			   *curr_req;

	Assert(group->rstate);

	/*
	 * Walk the list of cached or pending requests, fire cached
	 * requests until we reach the limit of concurrent requests.
	 */
	curr_req = DLGetHead(&group->rstate->request_list);
	while (curr_req &&
		   (group->rstate->open_streams <
		    MAX_CONCURRENT_RECOVERY_REQUESTS_PER_DB))
	{
		recovery_request_info  *ri = DLE_VAL(curr_req);
		group_node			   *node;
		HASH_SEQ_STATUS			hash_status;

		elog(DEBUG5, "Coordinator: checking request for %s.%s:",
			 ri->schemaname, ri->relname);

		if (ri->state == RLS_REQUEST_QUEUED)
		{
			if (group->db_state == RDBS_RECOVERING_SCHEMA &&
				ri->provider_node != InvalidNodeId)
			{
				/*
				 * Direct all requests to the very same node during schema
				 * recovery, just to make very sure we have a consistent
				 * schema copy (Oids and major version may differ between
				 * nodes).
				 */
				node = ri->provider_node;
			}
			else
			{
				/* FIXME: indentation */
			/*
			 * Choose a recovery provider.
			 *
			 * FIXME: we simply take the first node we can find which is
			 *        in RDBS_OPERATING mode. This is not optimal and should
			 *        be extended to hop between all OPERATING nodes.
			 */
			hash_seq_init(&hash_status, group->nodes);
			while ((node = (group_node*) hash_seq_search(&hash_status)))
			{
				if (node->state == RDBS_OPERATING)
				{
					hash_seq_term(&hash_status);
					break;
				}
			}
			}

 #ifdef COORDINATOR_DEBUG
			 elog(DEBUG5, "Coordinator: sending data recovery request for db %d, relation: %s.%s, round %d, lane %d of %d to node %d",
				  group->dboid, ri->schemaname, ri->relname, ri->round_no,
				  ri->curr_lane, ri->num_lanes, node->id);
 #endif

            Assert(node);
			gc_send_recovery_request(group, node, ri);

			/*
			 * Mark the request as fired and waiting for the recovery
			 * provider
			 */
			ri->state = RLS_REQUEST_FIRED;
			ri->provider_node = node;
			group->rstate->open_streams++;
		}

		curr_req = DLGetSucc(curr_req);
	}
}


/**************************************************************************
 * recovery provider functions
 **************************************************************************/

static Oid
getSystemCatalogPrimaryKey(const char *relname)
{
	if (strcmp("pg_attrdef", relname) == 0)
		return AttrDefaultOidIndexId;
	else if (strcmp("pg_attribute", relname) == 0)
		return AttributeRelidNameIndexId;
	else if (strcmp("pg_authid", relname) == 0)
		return AuthIdOidIndexId;
	else if (strcmp("pg_auth_members", relname) == 0)
		return AuthMemRoleMemIndexId;
	else if (strcmp("pg_class", relname) == 0)
		return ClassOidIndexId;
	else if (strcmp("pg_constraint", relname) == 0)
		return ConstraintOidIndexId;
	else if (strcmp("pg_database", relname) == 0)
		return DatabaseOidIndexId;
	else if (strcmp("pg_description", relname) == 0)
		return DescriptionObjIndexId;
	else if (strcmp("pg_enum", relname) == 0)
		return EnumOidIndexId;
	else if (strcmp("pg_index", relname) == 0)
		return IndexRelidIndexId;
	else if (strcmp("pg_inherits", relname) == 0)
		return InheritsRelidSeqnoIndexId;
	else if (strcmp("pg_largeobject", relname) == 0)
		return LargeObjectLOidPNIndexId;
	else if (strcmp("pg_namespace", relname) == 0)
		return NamespaceOidIndexId;
	else if (strcmp("pg_proc", relname) == 0)
		return ProcedureOidIndexId;
	else if (strcmp("pg_type", relname) == 0)
		return TypeOidIndexId;
	else
		return InvalidOid;
}

/*
 * This is mostly stolen from indexcmds.c, relationHasPrimaryKey()
 *
 * It returns an open HeapTuple to the index. The caller is responsible
 * for releasing it via ReleaseSysCache()
 */
static Oid
relationGetPrimaryKeyIndexOid(Relation rel)
{
	Oid				result = InvalidOid;
	List		   *indexoidlist;
	ListCell	   *indexoidscan;

	/*
	 * Get the list of index OIDs for the table from the relcache, and look up
	 * each one in the pg_index syscache until we find one marked primary key
	 * (hopefully there isn't more than one such).
	 */
	indexoidlist = RelationGetIndexList(rel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		HeapTuple	indexTuple;

		indexTuple = SearchSysCache(INDEXRELID,
									ObjectIdGetDatum(indexoid),
									0, 0, 0);
		if (!HeapTupleIsValid(indexTuple))		/* should not happen */
			elog(ERROR, "cache lookup failed for index %u", indexoid);

		if (((Form_pg_index) GETSTRUCT(indexTuple))->indisprimary)
			result = indexoid;

		ReleaseSysCache(indexTuple);

		if (result != InvalidOid)
			break;
	}

	list_free(indexoidlist);

	return result;
}

void
ProcessSchemaRecoveryRestart(void)
{
	IMessage *msg;
	bool shared = (MyDatabaseId == TemplateDbOid);

    /*
     * FIXME: we do not support schema recovery restarts, yet.
     */
	int round_no = 1;

	/*
	 * create a schema pg_remote_catalog, which takes up a complete
	 * copy of the remote schema.
	 */
	StartTransactionCommand();
	NamespaceCreate("pg_remote_catalog", GetUserId());
	CommandCounterIncrement();
	CommitTransactionCommand();

	/*
	 * FIXME: this is only the current development system catalog
	 *        schema. We should introduce a switch for different Postgres
	 *        versions here, to make sure we provide a correct copy of
	 *        the remote catalog.
	 */
#if CATALOG_VERSION_NO != 201007151
#warning Catalog Version mismatch. Please make sure you are using a Postgres version which fits the replication patch (by date).
#endif

	if (shared)
		initialize_shared_schema_transfer(round_no);
	else
		initialize_database_schema_transfer(round_no);

	msg = IMessageCreate(IMSGT_RECOVERY_RESTART, 0);
	IMessageActivate(msg, GetCoordinatorId());

	bgworker_job_completed();
}

void
initialize_shared_schema_transfer(int round_no)
{
	exec_simple_query("CREATE TABLE pg_remote_catalog.pg_authid ("
					  "    oid OID PRIMARY KEY,"
					  "    rolname NAME,"
					  "    rolsuper BOOL,"
					  "    rolinherit BOOL,"
					  "    rolcreaterole BOOL,"
					  "    rolcreatedb BOOL,"
					  "    rolcatupdate BOOL,"
					  "    rolcanlogin BOOL,"
					  "    rolconnlimit INT4,"
					  "    rolpassword TEXT,"
					  "    rolvaliduntil TIMESTAMPTZ);");

    InitiateRelationRecovery("pg_catalog", "pg_authid",
                             round_no, SCHEMA_RECOVERY_LANES);

	exec_simple_query("CREATE TABLE pg_remote_catalog.pg_auth_members ("
					  "    roleid OID PRIMARY KEY,"
					  "    member OID,"
					  "    grantor OID,"
					  "    admin_option BOOL);");

    InitiateRelationRecovery("pg_catalog", "pg_auth_members",
                             round_no, SCHEMA_RECOVERY_LANES);

	exec_simple_query("CREATE TABLE pg_remote_catalog.pg_database ("
					  "    oid OID PRIMARY KEY,"
					  "    datname NAME,"
					  "    datdba OID,"
					  "    encoding INT4,"
					  "    datcollate NAME,"
					  "    datctype NAME,"
					  "    datistemplate BOOL,"
					  "    datallowconn BOOL,"
					  "    datconnlimit INT4,"
					  "    datlastsysoid OID,"
					  "    datfrozenxid XID,"
					  "    dattablespace OID,"
					  "    datreplgroup NAME,"
					  "    datacl ACLITEM[]);");

	InitiateRelationRecovery("pg_catalog", "pg_database",
							 round_no, SCHEMA_RECOVERY_LANES);

	/* missing:  pg_pltemplate */

	/* unneeded: pg_shdepend (per node derived data) */

	/* missing:  pg_shdescription (maybe sync database description?) */

	/* unneeded: pg_tablespace (per node config) */
}

void
initialize_database_schema_transfer(int round_no)
{
	/* missing:  pg_aggregate (maybe sync custom aggregates?) */
	/* missing:  pg_am        (no to custom AMs) */
	/* missing:  pg_amop      (no to custom AMs) */
	/* missing:  pg_amproc    (no to custom AMs) */

	exec_simple_query("CREATE TABLE pg_remote_catalog.pg_attrdef ("
					  "    oid OID PRIMARY KEY,"
					  "    adrelid OID,"
					  "    adnum INT2,"
					  "    adbin TEXT,"
					  "    adsrc TEXT);");

    InitiateRelationRecovery("pg_catalog", "pg_attrdef",
                             round_no, SCHEMA_RECOVERY_LANES);

	exec_simple_query("CREATE TABLE pg_remote_catalog.pg_attribute ("
					  "    attrelid OID,"
					  "    attname NAME,"
					  "    atttypid OID,"
					  "    attstattarget INT4,"
					  "    attlen INT2,"
					  "    attnum INT2,"
					  "    attndims INT4,"
					  "    attcacheoff INT4,"
					  "    atttypmod INT4,"
					  "    attbyval BOOL,"
					  "    attstorage CHAR,"
					  "    attalign CHAR,"
					  "    attnotnull BOOL,"
					  "    atthasdef BOOL,"
					  "    attisdropped BOOL,"
					  "    attislocal BOOL,"
					  "    attinhcount INT4,"
					  "    attacl ACLITEM[],"
					  "    attoptions TEXT[],"
					  "    PRIMARY KEY(attrelid, attname));");

    InitiateRelationRecovery("pg_catalog", "pg_attribute",
                             round_no, SCHEMA_RECOVERY_LANES);

	/* unneeded: pg_autovacuum (per node settings) */

	/* missing:  pg_cast      (maybe sync custom casts?) */

	exec_simple_query("CREATE TABLE pg_remote_catalog.pg_class ("
					  "    oid OID PRIMARY KEY,"
					  "    relname NAME,"
					  "    relnamespace OID,"
					  "    reltype OID,"
					  "    reloftype OID,"
					  "    relowner OID,"
					  "    relam OID,"
					  "    relfilenode OID,"
					  "    reltablespace OID,"
					  "    relpages INT4,"
					  "    reltuples FLOAT4,"
					  "    reltoastrelid OID,"
					  "    reltoastidxid OID,"
					  "    relhasindex BOOL,"
					  "    relisshared BOOL,"
					  "    relistemp BOOL,"
					  "    relkind CHAR,"
					  "    relnatts INT2,"
					  "    relchecks INT2,"
					  "    relhasoids BOOL,"
					  "    relhaspkey BOOL,"
					  "    relhasexclusion BOOL,"
					  "    relhasrules BOOL,"
					  "    relhastriggers BOOL,"
					  "    relhassubclass BOOL,"
					  "    relfrozenxid XID,"
					  "    relacl ACLITEM[],"
					  "    reloptions TEXT[]);");

    InitiateRelationRecovery("pg_catalog", "pg_class",
                             round_no, SCHEMA_RECOVERY_LANES);

	exec_simple_query("CREATE TABLE pg_remote_catalog.pg_constraint ("
					  "    oid OID PRIMARY KEY,"
					  "    conname NAME,"
					  "    connamespace OID,"
					  "    contype CHAR,"
					  "    conndeferrable BOOL,"
					  "    condeferred BOOL,"
					  "    conrelid OID,"
					  "    contypid OID,"
					  "    conindid OID,"
					  "    confrelid OID,"
					  "    confupdtype CHAR,"
					  "    confdeltype CHAR,"
					  "    confmatchtype CHAR,"
					  "    conislocal BOOL,"
					  "    coninhcount INT4,"
					  "    conkey INT2[],"
					  "    confkey INT2[],"
					  "    conpfeqop OID[],"
					  "    conppeeqop OID[],"
					  "    conffeqop OID[],"
                      "    conexclop OID[],"
					  "    conbin TEXT,"
					  "    consrc TEXT);");

    InitiateRelationRecovery("pg_catalog", "pg_constraint",
                             round_no, SCHEMA_RECOVERY_LANES);

	/* unneeded: pg_control (per node settings) */

	/* missing:  pg_conversion (maybe sync custom conversions?) */

	/* missing:  pg_default_acl */

	/* unneeded: pg_depend (automatically restored, maintained per node) */

	exec_simple_query("CREATE TABLE pg_remote_catalog.pg_description ("
					  "    objoid OID,"
					  "    classoid OID,"
					  "    objsubid INT4,"
					  "    description TEXT,"
					  "    PRIMARY KEY (objoid, classoid, objsubid));");

    InitiateRelationRecovery("pg_catalog", "pg_description",
                             round_no, SCHEMA_RECOVERY_LANES);

	exec_simple_query("CREATE TABLE pg_remote_catalog.pg_enum ("
					  "    oid OID PRIMARY KEY,"
					  "    enumtypid OID,"
					  "    enumlabel NAME);");

    InitiateRelationRecovery("pg_catalog", "pg_enum",
                             round_no, SCHEMA_RECOVERY_LANES);

	/* missing:  pg_foreign_data_wrapper */
	/* missing:  pg_foreign_server */

	exec_simple_query("CREATE TABLE pg_remote_catalog.pg_index ("
					  "    indexrelid OID PRIMARY KEY,"
					  "    indrelid OID,"
					  "    indnatts INT2,"
					  "    indisunique BOOL,"
					  "    indisprimary BOOL,"
					  "    indimmediate BOOL,"
					  "    indisclustered BOOL,"
					  "    indisvalid BOOL,"
					  "    indcheckxmin BOOL,"
					  "    indisready BOOL,"
					  "    indkey INT2VECTOR,"
					  "    indclass OIDVECTOR,"
					  "    indoption INT2VECTOR,"
					  "    indexprs TEXT,"
					  "    indpred TEXT);");

    InitiateRelationRecovery("pg_catalog", "pg_index",
                             round_no, SCHEMA_RECOVERY_LANES);

	exec_simple_query("CREATE TABLE pg_remote_catalog.pg_inherits ("
					  "    inhrelid OID,"
					  "    inhparent OID,"
					  "    inhseqno INT4,"
					  "    PRIMARY KEY (inhrelid, inhseqno));");

    InitiateRelationRecovery("pg_catalog", "pg_inherits",
                             round_no, SCHEMA_RECOVERY_LANES);

	/* missing:  pg_language */

	/* unneeded: pg_largeobject (should be synced during data recovery) */

	/* missing:  pg_listener (replication and listeners??) */

	/* missing:  pg_opclass (maybe sync custom operators?) */

	/* missing:  pg_operator (maybe sync custom operators?) */

	/* missing:  pg_opfamily (maybe sync custom operators?) */

	exec_simple_query("CREATE TABLE pg_remote_catalog.pg_namespace ("
					  "    oid OID PRIMARY KEY,"
					  "    nspname NAME,"
					  "    nspowner OID,"
					  "    nspacl ACLITEM[]);");

    InitiateRelationRecovery("pg_catalog", "pg_namespace",
                             round_no, SCHEMA_RECOVERY_LANES);

	/* missing:  pg_pltemplate */

	exec_simple_query("CREATE TABLE pg_remote_catalog.pg_proc ("
					  "    oid OID PRIMARY KEY,"
					  "    proname NAME,"
					  "    pronamespace OID,"
					  "    proowner OID,"
					  "    prolang OID,"
					  "    procost FLOAT4,"
					  "    prorows FLOAT4,"
					  "    provariadic OID,"
					  "    proisagg BOOL,"
					  "    proiswindow BOOL,"
					  "    prosecdef BOOL,"
					  "    proisstrict BOOL,"
					  "    proretset BOOL,"
					  "    provolatile CHAR,"
					  "    pronargs INT2,"
					  "    pronargdefaults INT2,"
					  "    prorettype OID,"
					  "    proargtypes OIDVECTOR,"
					  "    proallargtypes OID[],"
					  "    proargmodes CHAR[],"
					  "    proargnames TEXT[],"
					  "    proargdefaults TEXT,"
					  "    prosrc TEXT,"
					  "    probin TEXT,"
					  "    proconfig TEXT[],"
					  "    aclitem ACLITEM[]);");

    InitiateRelationRecovery("pg_catalog", "pg_proc",
                             round_no, SCHEMA_RECOVERY_LANES);

	/* missing:  pg_rewrite */

	/* unneeded: pg_statistic (per node statistics (?)) */

	/* missing:  pg_trigger */

	/* missing:  pg_ts_config */
	/* missing:  pg_ts_config_map */
	/* missing:  pg_ts_dict */
	/* missing:  pg_ts_parser */
	/* missing:  pg_ts_template */

	exec_simple_query("CREATE TABLE pg_remote_catalog.pg_type ("
					  "    oid OID PRIMARY KEY,"
					  "    typname NAME,"
					  "    typnamespace OID,"
					  "    typowner OID,"
					  "    typlen INT2,"
					  "    typbyval BOOL,"
					  "    typtype CHAR,"
					  "    typcategory CHAR,"
					  "    typispreferred BOOL,"
					  "    typisdefined BOOL,"
					  "    typdelim CHAR,"
					  "    typrelid OID,"
					  "    typelem OID,"
					  "    typarray OID,"
					  "    typinput REGPROC,"
					  "    typoutput REGPROC,"
					  "    typreceive REGPROC,"
					  "    typsend REGPROC,"
					  "    typmodin REGPROC,"
					  "    typmodout REGPROC,"
					  "    typanalyze REGPROC,"
					  "    typalign CHAR,"
					  "    typstorage CHAR,"
					  "    typnotnull BOOL,"
					  "    typbasetype OID,"
					  "    typtypmod INT4,"
					  "    typndims INT4,"
					  "    typdefaultbin TEXT,"
					  "    typdefault TEXT);");

    InitiateRelationRecovery("pg_catalog", "pg_type",
                             round_no, SCHEMA_RECOVERY_LANES);

	/* missing:  pg_user_mapping */
}

void
InitiateRelationRecovery(const char *schemaname, const char *relname,
                         int round_no, int num_lanes)
{
    int i;

    for (i = 0; i < num_lanes; i++)
    {
        SendRecoveryRequest(schemaname, relname, round_no, i, num_lanes,
                            NULL, NULL, NULL);
    }
}

void
SendRelationRecoveredMsg(const char *schemaname, const char *relname,
						 int round_no, int lane_no, int num_lanes)
{
	IMessage   *msg;
	buffer		b;
	int			size = 5 + strlen(schemaname) + strlen(relname);

	msg = IMessageCreate(IMSGT_RELATION_RECOVERED, size);
	IMessageGetWriteBuffer(&b, msg);

	put_int8(&b, round_no);
	put_int8(&b, lane_no);
	put_int8(&b, num_lanes);
	put_pstring(&b, schemaname);
	put_pstring(&b, relname);

	IMessageActivate(msg, GetCoordinatorId());
}

static void
add_recovery_request_header(buffer *b)
{
	put_int8(b, CurrentRecoveryRequest->round_no);
	put_int8(b, CurrentRecoveryRequest->curr_lane);
	put_int8(b, CurrentRecoveryRequest->num_lanes);
	put_pstring(b, CurrentRecoveryRequest->schemaname);
	put_pstring(b, CurrentRecoveryRequest->relname);
}

static void
parse_recovery_request_header(buffer *b)
{
	CurrentRecoveryRequest->subscriber_node_id = get_int32(b);
	CurrentRecoveryRequest->round_no = get_int8(b);
	CurrentRecoveryRequest->curr_lane = get_int8(b);
	CurrentRecoveryRequest->num_lanes = get_int8(b);
	CurrentRecoveryRequest->schemaname = get_pstring(b);
	CurrentRecoveryRequest->relname = get_pstring(b);
}


static void
SendRecoveryRequest(const char *schemaname, const char *relname,
                    int round_no, int lane_no, int num_lanes,
					HeapTuple tuple, const IndexInfo *indexInfo,
					TupleDesc tdesc)
{
	IStreamWriterData writer;

	/*
	 * Store the information for add_recovery_request_header, so it
	 * can fill the header of the IMSGT_RECOVERY_REQUEST.
	 */
	CurrentRecoveryRequest = palloc(sizeof(recovery_request_info));
	CurrentRecoveryRequest->round_no = round_no;
	CurrentRecoveryRequest->curr_lane = lane_no;
	CurrentRecoveryRequest->num_lanes = num_lanes;
	CurrentRecoveryRequest->schemaname = schemaname;
	CurrentRecoveryRequest->relname = relname;

	elog(DEBUG5, "bgworker[%d/%d]: preparing recovery request for %s.%s",
		 MyProcPid, MyBackendId, schemaname, relname);

	istream_init_writer(&writer, IMSGT_RECOVERY_REQUEST, GetCoordinatorId(),
						MAX_RECOVERY_CHUNK_SIZE, &add_recovery_request_header);

	if (indexInfo)
	{
		/*
		 * Serialize the tuple's primary key.
		 */
		istream_write_int8(&writer, indexInfo->ii_NumIndexAttrs);
		serialize_tuple_pkey(&writer, indexInfo, tdesc, tuple);
	}
	else
	{
		/*
		 * An initial request for the table without a starting key
		 */
		istream_write_int8(&writer, 0);
	}

	istream_close_writer(&writer);
}

static void
add_recovery_data_header(buffer *b)
{
	put_int32(b, CurrentRecoveryRequest->subscriber_node_id);
	put_int8(b, CurrentRecoveryRequest->round_no);
	put_int8(b, CurrentRecoveryRequest->curr_lane);
	put_int8(b, CurrentRecoveryRequest->num_lanes);
	put_pstring(b, CurrentRecoveryRequest->schemaname);
	put_pstring(b, CurrentRecoveryRequest->relname);
	put_int32(b, CurrentRecoveryRequest->chunk_no++);
}

void
ProcessRecoveryRequest(IMessage *msg)
{
	int				i,
					attnum,
					natts,
					tcount,
					num_key_atts;
	TupleDesc		tdesc;

	Relation		rel,
					indexrel;
	HeapTuple		tuple;
    Oid				reloid,
					indexoid,
					gt_func,
					atttypid;

	IndexScanDesc	iscan;
	ScanKeyData	   *skeys = NULL;

	char		   *response;
	bool			is_catalog_table;

	Datum		   *values = NULL,
					tupleoid;

	IStreamReaderData reader;
	IStreamWriterData writer;

	CurrentRecoveryRequest = palloc(sizeof(recovery_request_info));
	istream_init_reader(&reader, &parse_recovery_request_header, msg);

	num_key_atts = istream_read_int8(&reader);

#ifdef COORDINATOR_DEBUG
	elog(DEBUG3, "ProcessRecoveryRequest: from node id %d, round %d, lane %d of %d, relation %s.%s",
		 CurrentRecoveryRequest->subscriber_node_id,
		 CurrentRecoveryRequest->round_no,
		 CurrentRecoveryRequest->curr_lane,
		 CurrentRecoveryRequest->num_lanes,
		 CurrentRecoveryRequest->schemaname,
		 CurrentRecoveryRequest->relname);
#endif

	CurrentRecoveryRequest->chunk_no = 0;

	StartTransactionCommand();

	XactIsoLevel = XACT_SERIALIZABLE;

	reloid = RangeVarGetRelid(makeRangeVar((char*) CurrentRecoveryRequest->schemaname,
										   (char*) CurrentRecoveryRequest->relname, -1), FALSE);
	rel = relation_open(reloid, AccessShareLock);

	if (strcmp(CurrentRecoveryRequest->schemaname, "pg_catalog") == 0)
		indexoid = getSystemCatalogPrimaryKey(CurrentRecoveryRequest->relname);
	else
		indexoid = relationGetPrimaryKeyIndexOid(rel);

	if (indexoid != InvalidOid)
	{
		indexrel = index_open(indexoid, NoLock);
		tdesc = RelationGetDescr(rel);

		/*
		 * Loop over each attribute in the primary key index
		 */
		elog(DEBUG1, "Relation %s has primary key index with %d attrs",
 			 CurrentRecoveryRequest->relname, indexrel->rd_index->indnatts);

		if (num_key_atts > 0)
		{
			/* We assert matching number of attributes on the remote side. */
			Assert(indexrel->rd_index->indnatts == num_key_atts);

			values = (Datum *) palloc(tdesc->natts * sizeof(Datum));
			deserialize_tuple_pkey(&reader, indexrel, tdesc, values, &tupleoid);

			skeys = palloc(num_key_atts * sizeof(ScanKeyData));
			for (i = 0; i < num_key_atts; i++)
			{
			    attnum = indexrel->rd_index->indkey.values[i];
				if (attnum >= 0)
					atttypid = tdesc->attrs[attnum - 1]->atttypid;
				else
					atttypid = OIDOID;

				get_sort_group_operators(atttypid, false, false, true,
										 NULL, NULL, &gt_func);

				if (attnum >= 0)
					ScanKeyInit(&skeys[i], i + 1, BTGreaterStrategyNumber,
								get_opcode(gt_func), values[attnum - 1]);
				else
					ScanKeyInit(&skeys[i], i + 1, BTGreaterStrategyNumber,
								get_opcode(gt_func), tupleoid);
			}
		}

		/*
		 * It's now safe to close the reader (and thus free the data
		 * in the imessage, which we *must* do before starting a stream
		 * writer, which might allocate space for new imessages).
		 */
		istream_close_reader(&reader);

		istream_init_writer(&writer, IMSGT_RECOVERY_DATA, GetCoordinatorId(),
							MAX_RECOVERY_CHUNK_SIZE,
							&add_recovery_data_header);

		natts = RelationGetNumberOfAttributes(rel);
		elog(DEBUG1, "Relation %s has %d attributes",
			 CurrentRecoveryRequest->relname, natts);

		/*
		 * To be sure we sync correctly, we add the number of attributes
		 * we expect in the target relation, special casing the system
		 * catalog's oid attribute.
		 */
		is_catalog_table = ((indexrel->rd_index->indnatts == 1) &&
							(indexrel->rd_index->indkey.values[0] == -2));
		Assert(natts < 65534);
		if (is_catalog_table)
			istream_write_int32(&writer, natts + 1);
		else
			istream_write_int32(&writer, natts);

		tcount = 0;
        iscan = index_beginscan(rel, indexrel, SnapshotNow,
								(skeys ? num_key_atts : 0), skeys);
		while ((tuple = index_getnext(iscan, ForwardScanDirection)) != NULL)
		{
			if (HeapTupleSatisfiesVisibility(tuple, SnapshotNow,
											 iscan->xs_cbuf))
			{
				istream_write_int8(&writer, 't');
				serialize_tuple_changes(&writer, tdesc, NULL, tuple,
										is_catalog_table);
				tcount++;
			}
		}

		index_endscan(iscan);
		index_close(indexrel, NoLock);

		if (num_key_atts > 0)
		{
			pfree(skeys);
			pfree(values);
		}

		/*
		 * Add an end-of-recovery stream identifier and close the
		 * stream.
		 */
		istream_write_int8(&writer, 'E');
		istream_close_writer(&writer);
	}
	else
	{
		istream_close_reader(&reader);

		elog(DEBUG1, "Unable to recover relation %s, it has no primary key!",
			 CurrentRecoveryRequest->relname);

		response = "";
		tcount = 0;
	}

	relation_close(rel, AccessShareLock);

	CommitTransactionCommand();
}



/**************************************************************************
 * recovery subscriber functions
 **************************************************************************/

static void
parse_recovery_data_header(buffer *b)
{
	get_int32(b); /* skip the provider node id */
	CurrentRecoveryRequest->round_no = get_int8(b);
	CurrentRecoveryRequest->curr_lane = get_int8(b);
	CurrentRecoveryRequest->num_lanes = get_int8(b);
	CurrentRecoveryRequest->schemaname = get_pstring(b);
	CurrentRecoveryRequest->relname = get_pstring(b);
	CurrentRecoveryRequest->chunk_no = get_int32(b);

#ifdef COORDINATOR_DEBUG
	elog(DEBUG3, "bg worker [%d/%d]: recovery: got data for %s.%s, lane %d of %d, round no %d, chunk %d",
		 MyProcPid, MyBackendId,
		 CurrentRecoveryRequest->schemaname,
		 CurrentRecoveryRequest->relname,
		 CurrentRecoveryRequest->curr_lane,
		 CurrentRecoveryRequest->num_lanes,
		 CurrentRecoveryRequest->round_no,
		 CurrentRecoveryRequest->chunk_no);
#endif
}

void
ProcessRecoveryData(IMessage *msg)
{
	int				natts,
					check_natts,
					i,
					tfound,
					indnatts,
					round_no,
					curr_lane, num_lanes;
	const char	   *relname,
				   *real_schemaname;
	char		   *schemaname;
	Oid				reloid,
					indexoid,
					eq_func;
	Relation		rel,
					indexrel;
	TupleDesc		tdesc;
	IndexScanDesc 	iscan = NULL;
	ScanKeyData		skeys[INDEX_MAX_KEYS];
	HeapTuple		old_tuple;
	IndexInfo	   *indexInfo;

	Datum		   *values;
	bool		   *nulls;

	TupleTableSlot	slot;
	EState		   *estate;
	ResultRelInfo  *rr_info;

	IStreamReaderData	reader;
	char			xtype;

	CurrentRecoveryRequest = palloc(sizeof(recovery_request_info));
	istream_init_reader(&reader, &parse_recovery_data_header, msg);

	round_no = CurrentRecoveryRequest->round_no;
	curr_lane = CurrentRecoveryRequest->curr_lane;
	num_lanes = CurrentRecoveryRequest->num_lanes;
	real_schemaname = CurrentRecoveryRequest->schemaname;
	relname = CurrentRecoveryRequest->relname;

	check_natts = istream_read_int32(&reader);

	StartTransactionCommand();

	/*
	 * Add the recovery data. As both, real data as well as schema
	 * data dripples in here, we need to take special care to redirect
	 * schema data to another namespace: pg_remote_catalog.
	 */
	if (strcmp(real_schemaname, "pg_catalog") == 0)
		schemaname = "pg_remote_catalog";
	else
		schemaname = (char*) real_schemaname;

	reloid = RangeVarGetRelid(makeRangeVar(schemaname, (char*) relname, -1), FALSE);
	rel = relation_open(reloid, AccessShareLock);

	tdesc = RelationGetDescr(rel);
	natts = RelationGetNumberOfAttributes(rel);
	elog(DEBUG1, "bg worker [%d/%d]: Relation %s has %d attributes (vs %d)",
		 MyProcPid, MyBackendId, relname, natts, check_natts);
	Assert(natts == check_natts);

	indexoid = relationGetPrimaryKeyIndexOid(rel);

	if (indexoid != InvalidOid)
	{
		indexrel = index_open(indexoid, NoLock);
		indexInfo = BuildIndexInfo(indexrel);
		Assert(indexInfo);
		indnatts = indexInfo->ii_NumIndexAttrs;

		/*
		 * Loop over each attribute in the primary key index
		 */
		elog(DEBUG1, "Relation %s has primary key index %d with %d attrs",
			 relname, indexoid, indnatts);

		/*
		 * Reserve space for the key attributes of the last tuple.
		 */
		values = (Datum *) palloc(tdesc->natts * sizeof(Datum));
		nulls = (bool*) palloc(tdesc->natts * sizeof(bool));

		/*
		 * Read tuples (xtype 't') from the stream, deserialize and apply
		 * them until we hit the end of the stream (xtype 'E').
		 */
		while ((xtype = istream_read_int8(&reader)) != 'E')
		{
			Assert(xtype == 't');

			deserialize_tuple_changes(&reader, tdesc, NULL, values, nulls);

			for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
			{
				int attnum = indexInfo->ii_KeyAttrNumbers[i];

				get_sort_group_operators(tdesc->attrs[attnum - 1]->atttypid,
										 false, true, false,
										 NULL, &eq_func, NULL);

				Assert(!nulls[attnum - 1]);

				ScanKeyInit(&skeys[i], i + 1, BTEqualStrategyNumber,
							get_opcode(eq_func), values[attnum - 1]);
			}

			if (!iscan)
				iscan = index_beginscan(rel, indexrel, SnapshotNow,
										indexInfo->ii_NumIndexAttrs, skeys);
			else
				index_rescan(iscan, skeys);

			tfound = 0;
			while ((old_tuple = index_getnext(iscan, ForwardScanDirection)))
			{
				if (HeapTupleSatisfiesVisibility(old_tuple, SnapshotNow,
												 iscan->xs_cbuf))
				{
					elog(ERROR, "..updates are not implemented, yet.");
					tfound++;
				}
			}

			Assert(tfound <= 1);
			if (tfound == 0)
			{
				/* dummy slot */
				slot.tts_isempty = FALSE;
				slot.tts_shouldFree = TRUE;
				slot.tts_mintuple = NULL;
				slot.tts_tuple = heap_form_tuple(tdesc, values, nulls);

				estate = CreateExecutorState();

				rr_info = (ResultRelInfo *) palloc0(sizeof(ResultRelInfo));

				Assert(rel);
				Assert(rel->rd_rel->relkind == RELKIND_RELATION);
				Assert(rel->rd_rel->relhasindex);

				rr_info->type = T_ResultRelInfo;
				rr_info->ri_RelationDesc = rel;

				estate->es_result_relation_info = rr_info;

				/* FIXME: uhm.. slot and planSlot are the same?? */
				ExecInsert(&slot, &slot, estate);
			}
			else
			{
				/* should update the tuple here... */
				Assert(FALSE);
			}
		}

		if (iscan)
			index_endscan(iscan);

		index_close(indexrel, NoLock);

		SendRelationRecoveredMsg(real_schemaname, relname,
								 round_no, curr_lane, num_lanes);
	}
	else
	{
		/* no such index found??? */
        Assert(FALSE);
	}
	relation_close(rel, AccessShareLock);

	istream_close_reader(&reader);

	CommitTransactionCommand();

	/*
	 * Release this background worker to the list of idle workers in
	 * any case. Schema recovery is initiated from a single backend,
	 * but application of data is parallelized now.
	 */
	bgworker_job_completed();
}

/*
 * ProcessSchemaAdaption
 *
 * After having mirrored the complete remote system catalogs, the
 * coordinator requests a single schema recovery backend to
 * adapt to the remote schema. This is done sequentially and does not
 * allow concurrent connections, which simplifies the process a lot.
 *
 * After having completed the adoption, we send an IMSGT_SCHEMA_ADAPTION
 * message to the coordinator, so it knows we are done and
 * switches to data recovery.
 */
void
ProcessSchemaAdaption(IMessage *msg)
{
	bool shared = (MyDatabaseId == TemplateDbOid);

	IMessageRemove(msg);

	set_ps_display("schema adaption", false);

	/*
	 * Start a transaction and acquire a snapshot.
	 */
	StartTransactionCommand();

	elog(NOTICE, "SCHEMA_ADAPTION: after having started a transaction");

	PushActiveSnapshot(GetTransactionSnapshot());

	elog(NOTICE, "SCHEMA_ADAPTION: after having pushed a snapshot");

	/* do the schema adaption */
	if (shared)
		DirectFunctionCall1(pg_adapt_from_pg90_shared_schema,
			DirectFunctionCall1(textin,
				CStringGetDatum("pg_remote_catalog")));
	else
		DirectFunctionCall1(pg_adapt_from_pg90_schema,
			DirectFunctionCall1(textin, CStringGetDatum("pg_remote_catalog")));

	/* ensure we do at least one CHECK_FOR_INTERRUPTS */
	CHECK_FOR_INTERRUPTS();

	/*
	 * Pop the snapshot and commit the transaction.
	 */
	PopActiveSnapshot();

	CommitTransactionCommand();

	/* drop the copy of the remote catalogs again */
	exec_simple_query("DROP SCHEMA pg_remote_catalog CASCADE;");

	/*
	 * Answer the coordinator that we've done the schema adaption.
	 */
	msg = IMessageCreate(IMSGT_SCHEMA_ADAPTION, 0);
	IMessageActivate(msg, GetCoordinatorId());

	/*
	 * As this is the last and terminating step of schema recovery for this
	 * database, we can now release this background worker.
	 */
	bgworker_job_completed();	
}


Datum
pg_adapt_from_pg90_shared_schema(PG_FUNCTION_ARGS)
{
	elog(WARNING, "pg_adapt_from_pg90_shared_schema: missing implementation");
	PG_RETURN_POINTER(CStringGetTextDatum("SELECT 'hello schema adaption';"));
}

Datum
pg_adapt_from_pg90_schema(PG_FUNCTION_ARGS)
{
	elog(WARNING, "pg_adapt_from_pg90_schema: missing implementation");
	PG_RETURN_POINTER(CStringGetTextDatum("SELECT 'hello schema adaption';"));
};

