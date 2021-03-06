/*-------------------------------------------------------------------------
 *
 * connection.c
 *		  Connection management functions for postgres_fdw
 *
 * Portions Copyright (c) 2012-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/postgres_fdw/connection.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "foreign/fdwapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postgres_fdw.h"
#include "storage/fd.h"
#include "storage/latch.h"
#include "utils/datetime.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/snapmgr.h"

/*
 * Connection cache hash table entry
 *
 * The lookup key in this hash table is the user mapping OID. We use just one
 * connection per user mapping ID, which ensures that all the scans use the
 * same snapshot during a query.  Using the user mapping OID rather than
 * the foreign server OID + user OID avoids creating multiple connections when
 * the public user mapping applies to all user OIDs.
 *
 * The "conn" pointer can be NULL if we don't currently have a live connection.
 * When we do have a connection, xact_depth tracks the current depth of
 * transactions and subtransactions open on the remote side.  We need to issue
 * commands at the same nesting depth on the remote as we're executing at
 * ourselves, so that rolling back a subtransaction will kill the right
 * queries and not the wrong ones.
 */
typedef Oid ConnCacheKey;

typedef struct ConnCacheEntry
{
	ConnCacheKey key;			/* hash key (must be first) */
	PGconn	   *conn;			/* connection to foreign server, or NULL */
	/* Remaining fields are invalid when conn is NULL: */
	int			xact_depth;		/* 0 = no xact open, 1 = main xact open, 2 =
								 * one level of subxact open, etc */
	bool		have_prep_stmt; /* have we prepared any stmts in this xact? */
	bool		have_error;		/* have any subxacts aborted in this xact? */
	bool		changing_xact_state;	/* xact state change in process */
	bool		invalidated;	/* true if reconnect is pending */
	bool		modified;		/* true if data on the foreign server is modified */
	uint32		server_hashvalue;	/* hash value of foreign server OID */
	uint32		mapping_hashvalue;	/* hash value of user mapping OID */
	CSN			imported_csn;
} ConnCacheEntry;

/*
 * Connection cache (initialized on first use)
 */
static HTAB *ConnectionHash = NULL;

/* for assigning cursor numbers and prepared statement numbers */
static unsigned int cursor_number = 0;
static unsigned int prep_stmt_number = 0;

/* tracks whether any work is needed in callback functions */
static bool xact_got_connection = false;

/* prototypes of private functions */
static void make_new_connection(ConnCacheEntry *entry, UserMapping *user);
static PGconn *connect_pg_server(ForeignServer *server, UserMapping *user);
static void disconnect_pg_server(ConnCacheEntry *entry);
static void check_conn_params(const char **keywords, const char **values, UserMapping *user);
static void configure_remote_session(PGconn *conn);
static void do_sql_command(PGconn *conn, const char *sql);
static void begin_remote_xact(ConnCacheEntry *entry, UserMapping *user);
static void pgfdw_subxact_callback(SubXactEvent event,
								   SubTransactionId mySubid,
								   SubTransactionId parentSubid,
								   void *arg);
static void pgfdw_inval_callback(Datum arg, int cacheid, uint32 hashvalue);
static void pgfdw_reject_incomplete_xact_state_change(ConnCacheEntry *entry);
static bool pgfdw_cancel_query(PGconn *conn);
static bool pgfdw_exec_cleanup_query(PGconn *conn, const char *query,
									 bool ignore_errors);
static bool pgfdw_get_cleanup_result(PGconn *conn, TimestampTz endtime,
									 PGresult **result);
static bool UserMappingPasswordRequired(UserMapping *user);
static void pgfdw_cleanup_after_transaction(ConnCacheEntry *entry);
static ConnCacheEntry *GetConnectionCacheEntry(Oid umid);
static void pgfdw_end_prepared_xact(ConnCacheEntry *entry, UserMapping *usermapping,
									char *fdwxact_id, bool is_commit);
static void SyncCSNSnapshot(ConnCacheEntry *entry);
/*
 * Get a PGconn which can be used to execute queries on the remote PostgreSQL
 * server with the user's authorization.  A new connection is established
 * if we don't already have a suitable one, and a transaction is opened at
 * the right subtransaction nesting depth if we didn't do that already.
 *
 * will_prep_stmt must be true if caller intends to create any prepared
 * statements.  Since those don't go away automatically at transaction end
 * (not even on error), we need this flag to cue manual cleanup.
 */
PGconn *
GetConnection(UserMapping *user, bool will_prep_stmt)
{
	bool		retry = false;
	ConnCacheEntry *entry;
	MemoryContext ccxt = CurrentMemoryContext;

	entry = GetConnectionCacheEntry(user->umid);

	/* Reject further use of connections which failed abort cleanup. */
	pgfdw_reject_incomplete_xact_state_change(entry);

	/*
	 * If the connection needs to be remade due to invalidation, disconnect as
	 * soon as we're out of all transactions.
	 */
	if (entry->conn != NULL && entry->invalidated && entry->xact_depth == 0)
	{
		elog(DEBUG3, "closing connection %p for option changes to take effect",
			 entry->conn);
		disconnect_pg_server(entry);
	}

	/*
	 * If cache entry doesn't have a connection, we have to establish a new
	 * connection.  (If connect_pg_server throws an error, the cache entry
	 * will remain in a valid empty state, ie conn == NULL.)
	 */
	if (entry->conn == NULL)
		make_new_connection(entry, user);

	/*
	 * We check the health of the cached connection here when starting a new
	 * remote transaction. If a broken connection is detected, we try to
	 * reestablish a new connection later.
	 */
	PG_TRY();
	{
		/* Start a new transaction or subtransaction if needed. */
		begin_remote_xact(entry, user);
	}
	PG_CATCH();
	{
		MemoryContext ecxt = MemoryContextSwitchTo(ccxt);
		ErrorData  *errdata = CopyErrorData();

		/*
		 * If connection failure is reported when starting a new remote
		 * transaction (not subtransaction), new connection will be
		 * reestablished later.
		 *
		 * After a broken connection is detected in libpq, any error other
		 * than connection failure (e.g., out-of-memory) can be thrown
		 * somewhere between return from libpq and the expected ereport() call
		 * in pgfdw_report_error(). In this case, since PQstatus() indicates
		 * CONNECTION_BAD, checking only PQstatus() causes the false detection
		 * of connection failure. To avoid this, we also verify that the
		 * error's sqlstate is ERRCODE_CONNECTION_FAILURE. Note that also
		 * checking only the sqlstate can cause another false detection
		 * because pgfdw_report_error() may report ERRCODE_CONNECTION_FAILURE
		 * for any libpq-originated error condition.
		 */
		if (errdata->sqlerrcode != ERRCODE_CONNECTION_FAILURE ||
			PQstatus(entry->conn) != CONNECTION_BAD ||
			entry->xact_depth > 0)
		{
			MemoryContextSwitchTo(ecxt);
			PG_RE_THROW();
		}

		/* Clean up the error state */
		FlushErrorState();
		FreeErrorData(errdata);
		errdata = NULL;

		retry = true;
	}
	PG_END_TRY();

	/*
	 * If a broken connection is detected, disconnect it, reestablish a new
	 * connection and retry a new remote transaction. If connection failure is
	 * reported again, we give up getting a connection.
	 */
	if (retry)
	{
		Assert(entry->xact_depth == 0);

		ereport(DEBUG3,
				(errmsg_internal("could not start remote transaction on connection %p",
								 entry->conn)),
				errdetail_internal("%s", pchomp(PQerrorMessage(entry->conn))));

		elog(DEBUG3, "closing connection %p to reestablish a new one",
			 entry->conn);
		disconnect_pg_server(entry);

		if (entry->conn == NULL)
			make_new_connection(entry, user);

		begin_remote_xact(entry, user);
	}

	/* Remember if caller will prepare statements */
	entry->have_prep_stmt |= will_prep_stmt;

	return entry->conn;
}

static ConnCacheEntry *
GetConnectionCacheEntry(Oid umid)
{
	bool		found;
	ConnCacheEntry *entry;
	ConnCacheKey key;

	/* First time through, initialize connection cache hashtable */
	if (ConnectionHash == NULL)
	{
		HASHCTL		ctl;

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(ConnCacheKey);
		ctl.entrysize = sizeof(ConnCacheEntry);
		/* allocate ConnectionHash in the cache context */
		ctl.hcxt = CacheMemoryContext;
		ConnectionHash = hash_create("postgres_fdw connections", 8,
									 &ctl,
									 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

		/*
		 * Register some callback functions that manage connection cleanup.
		 * This should be done just once in each backend.
		 */
		RegisterSubXactCallback(pgfdw_subxact_callback, NULL);
		CacheRegisterSyscacheCallback(FOREIGNSERVEROID,
									  pgfdw_inval_callback, (Datum) 0);
		CacheRegisterSyscacheCallback(USERMAPPINGOID,
									  pgfdw_inval_callback, (Datum) 0);
	}

	/* Set flag that we did GetConnection during the current transaction */
	xact_got_connection = true;

	/* Create hash key for the entry.  Assume no pad bytes in key struct */
	key = umid;

	/*
	 * Find or create cached entry for requested connection.
	 */
	entry = hash_search(ConnectionHash, &key, HASH_ENTER, &found);
	if (!found)
	{
		/*
		 * We need only clear "conn" here; remaining fields will be filled
		 * later when "conn" is set.
		 */
		entry->conn = NULL;
	}

	return entry;
}

/*
 * Reset all transient state fields in the cached connection entry and
 * establish new connection to the remote server.
 */
static void
make_new_connection(ConnCacheEntry *entry, UserMapping *user)
{
	ForeignServer *server = GetForeignServer(user->serverid);

	Assert(entry->conn == NULL);

	/* Reset all transient state fields, to be sure all are clean */
	entry->xact_depth = 0;
	entry->have_prep_stmt = false;
	entry->have_error = false;
	entry->changing_xact_state = false;
	entry->invalidated = false;
	entry->modified = false;
	entry->imported_csn = InvalidCSN;
	entry->server_hashvalue =
		GetSysCacheHashValue1(FOREIGNSERVEROID,
							  ObjectIdGetDatum(server->serverid));
	entry->mapping_hashvalue =
		GetSysCacheHashValue1(USERMAPPINGOID,
							  ObjectIdGetDatum(user->umid));

	/* Now try to make the connection */
	entry->conn = connect_pg_server(server, user);

	elog(DEBUG3, "new postgres_fdw connection %p for server \"%s\" (user mapping oid %u, userid %u)",
		 entry->conn, server->servername, user->umid, user->userid);
}

void
MarkConnectionModified(UserMapping *user)
{
	ConnCacheEntry *entry;

	entry = GetConnectionCacheEntry(user->umid);

	if (entry && !entry->modified)
	{
		FdwXactRegisterXact(user->serverid, user->userid, true);
		entry->modified = true;
	}
}

/*
 * Connect to remote server using specified server and user mapping properties.
 */
static PGconn *
connect_pg_server(ForeignServer *server, UserMapping *user)
{
	PGconn	   *volatile conn = NULL;

	/*
	 * Use PG_TRY block to ensure closing connection on error.
	 */
	PG_TRY();
	{
		const char **keywords;
		const char **values;
		int			n;

		/*
		 * Construct connection params from generic options of ForeignServer
		 * and UserMapping.  (Some of them might not be libpq options, in
		 * which case we'll just waste a few array slots.)  Add 3 extra slots
		 * for fallback_application_name, client_encoding, end marker.
		 */
		n = list_length(server->options) + list_length(user->options) + 3;
		keywords = (const char **) palloc(n * sizeof(char *));
		values = (const char **) palloc(n * sizeof(char *));

		n = 0;
		n += ExtractConnectionOptions(server->options,
									  keywords + n, values + n);
		n += ExtractConnectionOptions(user->options,
									  keywords + n, values + n);

		/* Use "postgres_fdw" as fallback_application_name. */
		keywords[n] = "fallback_application_name";
		values[n] = "postgres_fdw";
		n++;

		/* Set client_encoding so that libpq can convert encoding properly. */
		keywords[n] = "client_encoding";
		values[n] = GetDatabaseEncodingName();
		n++;

		keywords[n] = values[n] = NULL;

		/* verify the set of connection parameters */
		check_conn_params(keywords, values, user);

		/*
		 * We must obey fd.c's limit on non-virtual file descriptors.  Assume
		 * that a PGconn represents one long-lived FD.  (Doing this here also
		 * ensures that VFDs are closed if needed to make room.)
		 */
		if (!AcquireExternalFD())
		{
#ifndef WIN32					/* can't write #if within ereport() macro */
			ereport(ERROR,
					(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
					 errmsg("could not connect to server \"%s\"",
							server->servername),
					 errdetail("There are too many open files on the local server."),
					 errhint("Raise the server's max_files_per_process and/or \"ulimit -n\" limits.")));
#else
			ereport(ERROR,
					(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
					 errmsg("could not connect to server \"%s\"",
							server->servername),
					 errdetail("There are too many open files on the local server."),
					 errhint("Raise the server's max_files_per_process setting.")));
#endif
		}

		/* OK to make connection */
		conn = PQconnectdbParams(keywords, values, false);

		if (!conn)
			ReleaseExternalFD();	/* because the PG_CATCH block won't */

		if (!conn || PQstatus(conn) != CONNECTION_OK)
			ereport(ERROR,
					(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
					 errmsg("could not connect to server \"%s\"",
							server->servername),
					 errdetail_internal("%s", pchomp(PQerrorMessage(conn)))));

		/*
		 * Check that non-superuser has used password to establish connection;
		 * otherwise, he's piggybacking on the postgres server's user
		 * identity. See also dblink_security_check() in contrib/dblink and
		 * check_conn_params.
		 */
		if (!superuser_arg(user->userid) && UserMappingPasswordRequired(user) &&
			!PQconnectionUsedPassword(conn))
			ereport(ERROR,
					(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
					 errmsg("password is required"),
					 errdetail("Non-superuser cannot connect if the server does not request a password."),
					 errhint("Target server's authentication method must be changed or password_required=false set in the user mapping attributes.")));

		/* Prepare new session for use */
		configure_remote_session(conn);

		pfree(keywords);
		pfree(values);
	}
	PG_CATCH();
	{
		/* Release PGconn data structure if we managed to create one */
		if (conn)
		{
			PQfinish(conn);
			ReleaseExternalFD();
		}
		PG_RE_THROW();
	}
	PG_END_TRY();

	return conn;
}

/*
 * Disconnect any open connection for a connection cache entry.
 */
static void
disconnect_pg_server(ConnCacheEntry *entry)
{
	if (entry->conn != NULL)
	{
		PQfinish(entry->conn);
		entry->conn = NULL;
		ReleaseExternalFD();
	}
}

/*
 * Return true if the password_required is defined and false for this user
 * mapping, otherwise false. The mapping has been pre-validated.
 */
static bool
UserMappingPasswordRequired(UserMapping *user)
{
	ListCell   *cell;

	foreach(cell, user->options)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (strcmp(def->defname, "password_required") == 0)
			return defGetBoolean(def);
	}

	return true;
}

/*
 * For non-superusers, insist that the connstr specify a password.  This
 * prevents a password from being picked up from .pgpass, a service file, the
 * environment, etc.  We don't want the postgres user's passwords,
 * certificates, etc to be accessible to non-superusers.  (See also
 * dblink_connstr_check in contrib/dblink.)
 */
static void
check_conn_params(const char **keywords, const char **values, UserMapping *user)
{
	int			i;

	/* no check required if superuser */
	if (superuser_arg(user->userid))
		return;

	/* ok if params contain a non-empty password */
	for (i = 0; keywords[i] != NULL; i++)
	{
		if (strcmp(keywords[i], "password") == 0 && values[i][0] != '\0')
			return;
	}

	/* ok if the superuser explicitly said so at user mapping creation time */
	if (!UserMappingPasswordRequired(user))
		return;

	ereport(ERROR,
			(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
			 errmsg("password is required"),
			 errdetail("Non-superusers must provide a password in the user mapping.")));
}

/*
 * Issue SET commands to make sure remote session is configured properly.
 *
 * We do this just once at connection, assuming nothing will change the
 * values later.  Since we'll never send volatile function calls to the
 * remote, there shouldn't be any way to break this assumption from our end.
 * It's possible to think of ways to break it at the remote end, eg making
 * a foreign table point to a view that includes a set_config call ---
 * but once you admit the possibility of a malicious view definition,
 * there are any number of ways to break things.
 */
static void
configure_remote_session(PGconn *conn)
{
	int			remoteversion = PQserverVersion(conn);

	/* Force the search path to contain only pg_catalog (see deparse.c) */
	do_sql_command(conn, "SET search_path = pg_catalog");

	/*
	 * Set remote timezone; this is basically just cosmetic, since all
	 * transmitted and returned timestamptzs should specify a zone explicitly
	 * anyway.  However it makes the regression test outputs more predictable.
	 *
	 * We don't risk setting remote zone equal to ours, since the remote
	 * server might use a different timezone database.  Instead, use UTC
	 * (quoted, because very old servers are picky about case).
	 */
	do_sql_command(conn, "SET timezone = 'UTC'");

	/*
	 * Set values needed to ensure unambiguous data output from remote.  (This
	 * logic should match what pg_dump does.  See also set_transmission_modes
	 * in postgres_fdw.c.)
	 */
	do_sql_command(conn, "SET datestyle = ISO");
	if (remoteversion >= 80400)
		do_sql_command(conn, "SET intervalstyle = postgres");
	if (remoteversion >= 90000)
		do_sql_command(conn, "SET extra_float_digits = 3");
	else
		do_sql_command(conn, "SET extra_float_digits = 2");
}

/*
 * Convenience subroutine to issue a non-data-returning SQL command to remote
 */
static void
do_sql_command(PGconn *conn, const char *sql)
{
	PGresult   *res;

	if (!PQsendQuery(conn, sql))
		pgfdw_report_error(ERROR, NULL, conn, false, sql);
	res = pgfdw_get_result(conn, sql);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pgfdw_report_error(ERROR, res, conn, true, sql);
	PQclear(res);
}

/*
 * Start remote transaction or subtransaction, if needed.
 *
 * Note that we always use at least REPEATABLE READ in the remote session.
 * This is so that, if a query initiates multiple scans of the same or
 * different foreign tables, we will get snapshot-consistent results from
 * those scans.  A disadvantage is that we can't provide sane emulation of
 * READ COMMITTED behavior --- it would be nice if we had some other way to
 * control which remote queries share a snapshot.
 */
static void
begin_remote_xact(ConnCacheEntry *entry, UserMapping *user)
{
	int			curlevel = GetCurrentTransactionNestLevel();

	/* Start main transaction if we haven't yet */
	if (entry->xact_depth <= 0)
	{
		const char *sql;

		elog(DEBUG3, "starting remote transaction on connection %p",
			 entry->conn);

		if (is_global_snapshot_enabled() && (!IsolationUsesXactSnapshot() ||
                                          IsolationIsSerializable()))
            ereport(ERROR,
                    (errmsg("Global snapshots are only supported with REPEATABLE READ isolation level")));

		/* Register the foreign server to the transaction */
		FdwXactRegisterXact(user->serverid, user->userid, false);

		if (IsolationIsSerializable())
			sql = "START TRANSACTION ISOLATION LEVEL SERIALIZABLE";
		else
			sql = "START TRANSACTION ISOLATION LEVEL REPEATABLE READ";
		entry->changing_xact_state = true;
		do_sql_command(entry->conn, sql);
		entry->xact_depth = 1;
		entry->modified = false;
		entry->changing_xact_state = false;
	}
	/*
	 * if global snapshot is enabled we need
	 * to import the CSN in the foreign transaction
	 */
	SyncCSNSnapshot(entry);

	/*
	 * If we're in a subtransaction, stack up savepoints to match our level.
	 * This ensures we can rollback just the desired effects when a
	 * subtransaction aborts.
	 */
	while (entry->xact_depth < curlevel)
	{
		char		sql[64];

		snprintf(sql, sizeof(sql), "SAVEPOINT s%d", entry->xact_depth + 1);
		entry->changing_xact_state = true;
		do_sql_command(entry->conn, sql);
		entry->xact_depth++;
		entry->changing_xact_state = false;
	}
}

static void
SyncCSNSnapshot(ConnCacheEntry *entry)
{
	char sql[128];
	PGresult   *res;
	CSN	 csn;

	if (!is_global_snapshot_enabled())
		return;

	csn = ExportCSNSnapshot();

	if (csn != entry->imported_csn)
	{
		entry->imported_csn = csn;
		snprintf(sql, sizeof(sql),
			"SELECT pg_csn_snapshot_import("UINT64_FORMAT")",entry->imported_csn);

		entry->changing_xact_state = true;
		res = pgfdw_exec_query(entry->conn, sql);
		entry->changing_xact_state = false;

		if (PQresultStatus(res) != PGRES_TUPLES_OK)
				ereport(ERROR,
							(errmsg("failed to import CSN snapshot in remote server")));
		PQclear(res);
	}
}

/*
 * Release connection reference count created by calling GetConnection.
 */
void
ReleaseConnection(PGconn *conn)
{
	/*
	 * Currently, we don't actually track connection references because all
	 * cleanup is managed on a transaction or subtransaction basis instead. So
	 * there's nothing to do here.
	 */
}

/*
 * Assign a "unique" number for a cursor.
 *
 * These really only need to be unique per connection within a transaction.
 * For the moment we ignore the per-connection point and assign them across
 * all connections in the transaction, but we ask for the connection to be
 * supplied in case we want to refine that.
 *
 * Note that even if wraparound happens in a very long transaction, actual
 * collisions are highly improbable; just be sure to use %u not %d to print.
 */
unsigned int
GetCursorNumber(PGconn *conn)
{
	return ++cursor_number;
}

/*
 * Assign a "unique" number for a prepared statement.
 *
 * This works much like GetCursorNumber, except that we never reset the counter
 * within a session.  That's because we can't be 100% sure we've gotten rid
 * of all prepared statements on all connections, and it's not really worth
 * increasing the risk of prepared-statement name collisions by resetting.
 */
unsigned int
GetPrepStmtNumber(PGconn *conn)
{
	return ++prep_stmt_number;
}

/*
 * Submit a query and wait for the result.
 *
 * This function is interruptible by signals.
 *
 * Caller is responsible for the error handling on the result.
 */
PGresult *
pgfdw_exec_query(PGconn *conn, const char *query)
{
	/*
	 * Submit a query.  Since we don't use non-blocking mode, this also can
	 * block.  But its risk is relatively small, so we ignore that for now.
	 */
	if (!PQsendQuery(conn, query))
		pgfdw_report_error(ERROR, NULL, conn, false, query);

	/* Wait for the result. */
	return pgfdw_get_result(conn, query);
}

/*
 * Wait for the result from a prior asynchronous execution function call.
 *
 * This function offers quick responsiveness by checking for any interruptions.
 *
 * This function emulates PQexec()'s behavior of returning the last result
 * when there are many.
 *
 * Caller is responsible for the error handling on the result.
 */
PGresult *
pgfdw_get_result(PGconn *conn, const char *query)
{
	PGresult   *volatile last_res = NULL;

	/* In what follows, do not leak any PGresults on an error. */
	PG_TRY();
	{
		for (;;)
		{
			PGresult   *res;

			while (PQisBusy(conn))
			{
				int			wc;

				/* Sleep until there's something to do */
				wc = WaitLatchOrSocket(MyLatch,
									   WL_LATCH_SET | WL_SOCKET_READABLE |
									   WL_EXIT_ON_PM_DEATH,
									   PQsocket(conn),
									   -1L, PG_WAIT_EXTENSION);
				ResetLatch(MyLatch);

				CHECK_FOR_INTERRUPTS();

				/* Data available in socket? */
				if (wc & WL_SOCKET_READABLE)
				{
					if (!PQconsumeInput(conn))
						pgfdw_report_error(ERROR, NULL, conn, false, query);
				}
			}

			res = PQgetResult(conn);
			if (res == NULL)
				break;			/* query is complete */

			PQclear(last_res);
			last_res = res;
		}
	}
	PG_CATCH();
	{
		PQclear(last_res);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return last_res;
}

/*
 * Report an error we got from the remote server.
 *
 * elevel: error level to use (typically ERROR, but might be less)
 * res: PGresult containing the error
 * conn: connection we did the query on
 * clear: if true, PQclear the result (otherwise caller will handle it)
 * sql: NULL, or text of remote command we tried to execute
 *
 * Note: callers that choose not to throw ERROR for a remote error are
 * responsible for making sure that the associated ConnCacheEntry gets
 * marked with have_error = true.
 */
void
pgfdw_report_error(int elevel, PGresult *res, PGconn *conn,
				   bool clear, const char *sql)
{
	/* If requested, PGresult must be released before leaving this function. */
	PG_TRY();
	{
		char	   *diag_sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
		char	   *message_primary = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
		char	   *message_detail = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
		char	   *message_hint = PQresultErrorField(res, PG_DIAG_MESSAGE_HINT);
		char	   *message_context = PQresultErrorField(res, PG_DIAG_CONTEXT);
		int			sqlstate;

		if (diag_sqlstate)
			sqlstate = MAKE_SQLSTATE(diag_sqlstate[0],
									 diag_sqlstate[1],
									 diag_sqlstate[2],
									 diag_sqlstate[3],
									 diag_sqlstate[4]);
		else
			sqlstate = ERRCODE_CONNECTION_FAILURE;

		/*
		 * If we don't get a message from the PGresult, try the PGconn.  This
		 * is needed because for connection-level failures, PQexec may just
		 * return NULL, not a PGresult at all.
		 */
		if (message_primary == NULL)
			message_primary = pchomp(PQerrorMessage(conn));

		ereport(elevel,
				(errcode(sqlstate),
				 message_primary ? errmsg_internal("%s", message_primary) :
				 errmsg("could not obtain message string for remote error"),
				 message_detail ? errdetail_internal("%s", message_detail) : 0,
				 message_hint ? errhint("%s", message_hint) : 0,
				 message_context ? errcontext("%s", message_context) : 0,
				 sql ? errcontext("remote SQL command: %s", sql) : 0));
	}
	PG_FINALLY();
	{
		if (clear)
			PQclear(res);
	}
	PG_END_TRY();
}

/*
 * pgfdw_subxact_callback --- cleanup at subtransaction end.
 */
static void
pgfdw_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
					   SubTransactionId parentSubid, void *arg)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;
	int			curlevel;

	/* Nothing to do at subxact start, nor after commit. */
	if (!(event == SUBXACT_EVENT_PRE_COMMIT_SUB ||
		  event == SUBXACT_EVENT_ABORT_SUB))
		return;

	/* Quick exit if no connections were touched in this transaction. */
	if (!xact_got_connection)
		return;

	/*
	 * Scan all connection cache entries to find open remote subtransactions
	 * of the current level, and close them.
	 */
	curlevel = GetCurrentTransactionNestLevel();
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		char		sql[100];

		/*
		 * We only care about connections with open remote subtransactions of
		 * the current level.
		 */
		if (entry->conn == NULL || entry->xact_depth < curlevel)
			continue;

		if (entry->xact_depth > curlevel)
			elog(ERROR, "missed cleaning up remote subtransaction at level %d",
				 entry->xact_depth);

		if (event == SUBXACT_EVENT_PRE_COMMIT_SUB)
		{
			/*
			 * If abort cleanup previously failed for this connection, we
			 * can't issue any more commands against it.
			 */
			pgfdw_reject_incomplete_xact_state_change(entry);

			/* Commit all remote subtransactions during pre-commit */
			snprintf(sql, sizeof(sql), "RELEASE SAVEPOINT s%d", curlevel);
			entry->changing_xact_state = true;
			do_sql_command(entry->conn, sql);
			entry->changing_xact_state = false;
		}
		else if (in_error_recursion_trouble())
		{
			/*
			 * Don't try to clean up the connection if we're already in error
			 * recursion trouble.
			 */
			entry->changing_xact_state = true;
		}
		else if (!entry->changing_xact_state)
		{
			bool		abort_cleanup_failure = false;

			/* Remember that abort cleanup is in progress. */
			entry->changing_xact_state = true;

			/* Assume we might have lost track of prepared statements */
			entry->have_error = true;

			/*
			 * If a command has been submitted to the remote server by using
			 * an asynchronous execution function, the command might not have
			 * yet completed.  Check to see if a command is still being
			 * processed by the remote server, and if so, request cancellation
			 * of the command.
			 */
			if (PQtransactionStatus(entry->conn) == PQTRANS_ACTIVE &&
				!pgfdw_cancel_query(entry->conn))
				abort_cleanup_failure = true;
			else
			{
				/* Rollback all remote subtransactions during abort */
				snprintf(sql, sizeof(sql),
						 "ROLLBACK TO SAVEPOINT s%d; RELEASE SAVEPOINT s%d",
						 curlevel, curlevel);
				if (!pgfdw_exec_cleanup_query(entry->conn, sql, false))
					abort_cleanup_failure = true;
			}

			/* Disarm changing_xact_state if it all worked. */
			entry->changing_xact_state = abort_cleanup_failure;
		}

		/* OK, we're outta that level of subtransaction */
		entry->xact_depth--;
	}
}

/*
 * Connection invalidation callback function
 *
 * After a change to a pg_foreign_server or pg_user_mapping catalog entry,
 * mark connections depending on that entry as needing to be remade.
 * We can't immediately destroy them, since they might be in the midst of
 * a transaction, but we'll remake them at the next opportunity.
 *
 * Although most cache invalidation callbacks blow away all the related stuff
 * regardless of the given hashvalue, connections are expensive enough that
 * it's worth trying to avoid that.
 *
 * NB: We could avoid unnecessary disconnection more strictly by examining
 * individual option values, but it seems too much effort for the gain.
 */
static void
pgfdw_inval_callback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;

	Assert(cacheid == FOREIGNSERVEROID || cacheid == USERMAPPINGOID);

	/* ConnectionHash must exist already, if we're registered */
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		/* Ignore invalid entries */
		if (entry->conn == NULL)
			continue;

		/* hashvalue == 0 means a cache reset, must clear all state */
		if (hashvalue == 0 ||
			(cacheid == FOREIGNSERVEROID &&
			 entry->server_hashvalue == hashvalue) ||
			(cacheid == USERMAPPINGOID &&
			 entry->mapping_hashvalue == hashvalue))
			entry->invalidated = true;
	}
}

/*
 * Raise an error if the given connection cache entry is marked as being
 * in the middle of an xact state change.  This should be called at which no
 * such change is expected to be in progress; if one is found to be in
 * progress, it means that we aborted in the middle of a previous state change
 * and now don't know what the remote transaction state actually is.
 * Such connections can't safely be further used.  Re-establishing the
 * connection would change the snapshot and roll back any writes already
 * performed, so that's not an option, either. Thus, we must abort.
 */
static void
pgfdw_reject_incomplete_xact_state_change(ConnCacheEntry *entry)
{
	HeapTuple	tup;
	Form_pg_user_mapping umform;
	ForeignServer *server;

	/* nothing to do for inactive entries and entries of sane state */
	if (entry->conn == NULL || !entry->changing_xact_state)
		return;

	/* make sure this entry is inactive */
	disconnect_pg_server(entry);

	/* find server name to be shown in the message below */
	tup = SearchSysCache1(USERMAPPINGOID,
						  ObjectIdGetDatum(entry->key));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for user mapping %u", entry->key);
	umform = (Form_pg_user_mapping) GETSTRUCT(tup);
	server = GetForeignServer(umform->umserver);
	ReleaseSysCache(tup);

	ereport(ERROR,
			(errcode(ERRCODE_CONNECTION_EXCEPTION),
			 errmsg("connection to server \"%s\" was lost",
					server->servername)));
}

/*
 * Cancel the currently-in-progress query (whose query text we do not have)
 * and ignore the result.  Returns true if we successfully cancel the query
 * and discard any pending result, and false if not.
 */
static bool
pgfdw_cancel_query(PGconn *conn)
{
	PGcancel   *cancel;
	char		errbuf[256];
	PGresult   *result = NULL;
	TimestampTz endtime;

	/*
	 * If it takes too long to cancel the query and discard the result, assume
	 * the connection is dead.
	 */
	endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), 30000);

	/*
	 * Issue cancel request.  Unfortunately, there's no good way to limit the
	 * amount of time that we might block inside PQgetCancel().
	 */
	if ((cancel = PQgetCancel(conn)))
	{
		if (!PQcancel(cancel, errbuf, sizeof(errbuf)))
		{
			ereport(WARNING,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not send cancel request: %s",
							errbuf)));
			PQfreeCancel(cancel);
			return false;
		}
		PQfreeCancel(cancel);
	}

	/* Get and discard the result of the query. */
	if (pgfdw_get_cleanup_result(conn, endtime, &result))
		return false;
	PQclear(result);

	return true;
}

/*
 * Submit a query during (sub)abort cleanup and wait up to 30 seconds for the
 * result.  If the query is executed without error, the return value is true.
 * If the query is executed successfully but returns an error, the return
 * value is true if and only if ignore_errors is set.  If the query can't be
 * sent or times out, the return value is false.
 */
static bool
pgfdw_exec_cleanup_query(PGconn *conn, const char *query, bool ignore_errors)
{
	PGresult   *result = NULL;
	TimestampTz endtime;

	/*
	 * If it takes too long to execute a cleanup query, assume the connection
	 * is dead.  It's fairly likely that this is why we aborted in the first
	 * place (e.g. statement timeout, user cancel), so the timeout shouldn't
	 * be too long.
	 */
	endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), 30000);

	/*
	 * Submit a query.  Since we don't use non-blocking mode, this also can
	 * block.  But its risk is relatively small, so we ignore that for now.
	 */
	if (!PQsendQuery(conn, query))
	{
		pgfdw_report_error(WARNING, NULL, conn, false, query);
		return false;
	}

	/* Get the result of the query. */
	if (pgfdw_get_cleanup_result(conn, endtime, &result))
		return false;

	/* Issue a warning if not successful. */
	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		pgfdw_report_error(WARNING, result, conn, true, query);
		return ignore_errors;
	}
	PQclear(result);

	return true;
}

/*
 * Get, during abort cleanup, the result of a query that is in progress.  This
 * might be a query that is being interrupted by transaction abort, or it might
 * be a query that was initiated as part of transaction abort to get the remote
 * side back to the appropriate state.
 *
 * It's not a huge problem if we throw an ERROR here, but if we get into error
 * recursion trouble, we'll end up slamming the connection shut, which will
 * necessitate failing the entire toplevel transaction even if subtransactions
 * were used.  Try to use WARNING where we can.
 *
 * endtime is the time at which we should give up and assume the remote
 * side is dead.  Returns true if the timeout expired, otherwise false.
 * Sets *result except in case of a timeout.
 */
static bool
pgfdw_get_cleanup_result(PGconn *conn, TimestampTz endtime, PGresult **result)
{
	volatile bool timed_out = false;
	PGresult   *volatile last_res = NULL;

	/* In what follows, do not leak any PGresults on an error. */
	PG_TRY();
	{
		for (;;)
		{
			PGresult   *res;

			while (PQisBusy(conn))
			{
				int			wc;
				TimestampTz now = GetCurrentTimestamp();
				long		secs;
				int			microsecs;
				long		cur_timeout;

				/* If timeout has expired, give up, else get sleep time. */
				if (now >= endtime)
				{
					timed_out = true;
					goto exit;
				}
				TimestampDifference(now, endtime, &secs, &microsecs);

				/* To protect against clock skew, limit sleep to one minute. */
				cur_timeout = Min(60000, secs * USECS_PER_SEC + microsecs);

				/* Sleep until there's something to do */
				wc = WaitLatchOrSocket(MyLatch,
									   WL_LATCH_SET | WL_SOCKET_READABLE |
									   WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
									   PQsocket(conn),
									   cur_timeout, PG_WAIT_EXTENSION);
				ResetLatch(MyLatch);

				CHECK_FOR_INTERRUPTS();

				/* Data available in socket? */
				if (wc & WL_SOCKET_READABLE)
				{
					if (!PQconsumeInput(conn))
					{
						/* connection trouble; treat the same as a timeout */
						timed_out = true;
						goto exit;
					}
				}
			}

			res = PQgetResult(conn);
			if (res == NULL)
				break;			/* query is complete */

			PQclear(last_res);
			last_res = res;
		}
exit:	;
	}
	PG_CATCH();
	{
		PQclear(last_res);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (timed_out)
		PQclear(last_res);
	else
		*result = last_res;
	return timed_out;
}

void
postgresCommitForeignTransaction(FdwXactRslvState *frstate)
{
	ConnCacheEntry *entry;
	bool		is_onephase = (frstate->flags & FDWXACT_FLAG_ONEPHASE) != 0;
	PGresult   *res;

	entry = GetConnectionCacheEntry(frstate->usermapping->umid);

	if (!is_onephase)
	{
		/* COMMIT PREPARED the transaction and cleanup */
		pgfdw_end_prepared_xact(entry, frstate->usermapping,
								frstate->fdwxact_id, true);
		return;
	}

	Assert(entry->conn);

	/*
	 * If abort cleanup previously failed for this connection, we can't issue
	 * any more commands against it.
	 */
	pgfdw_reject_incomplete_xact_state_change(entry);

	entry->changing_xact_state = true;
	res = pgfdw_exec_query(entry->conn, "COMMIT TRANSACTION");
	entry->changing_xact_state = false;

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		ereport(ERROR, (errmsg("could not commit transaction on server %s",
							   frstate->server->servername)));

	/*
	 * If there were any errors in subtransactions, and we ma
	 * made prepared statements, do a DEALLOCATE ALL to make
	 * sure we get rid of all prepared statements. This is
	 * annoying and not terribly bulletproof, but it's
	 * probably not worth trying harder.
	 *
	 * DEALLOCATE ALL only exists in 8.3 and later, so this
	 * constrains how old a server postgres_fdw can
	 * communicate with.  We intentionally ignore errors in
	 * the DEALLOCATE, so that we can hobble along to some
	 * extent with older servers (leaking prepared statements
	 * as we go; but we don't really support update operations
	 * pre-8.3 anyway).
	 */
	if (entry->have_prep_stmt && entry->have_error)
	{
		res = PQexec(entry->conn, "DEALLOCATE ALL");
		PQclear(res);
	}

	/* Cleanup transaction status */
	pgfdw_cleanup_after_transaction(entry);
}

void
postgresRollbackForeignTransaction(FdwXactRslvState *frstate)
{
	ConnCacheEntry *entry = NULL;
	bool is_onephase = (frstate->flags & FDWXACT_FLAG_ONEPHASE) != 0;
	bool abort_cleanup_failure = false;

	/*
	 * In simple rollback case, we must have a connection to the foreign server
	 * because the foreign transaction is not closed yet. We get the connection
	 * entry from the cache.
	 */
	entry = GetConnectionCacheEntry(frstate->usermapping->umid);

	if (!is_onephase)
	{
		/* ROLLBACK PREPARED the transaction and cleanup */
		pgfdw_end_prepared_xact(entry, frstate->usermapping,
								frstate->fdwxact_id, false);
		return;
	}

	Assert(entry);

	/*
	 * Cleanup connection entry transaction if transaction fails before
	 * establishing a connection.
	 */
	if (!entry->conn)
	{
		pgfdw_cleanup_after_transaction(entry);
		return;
	}

	/*
	 * Don't try to clean up the connection if we're already
	 * in error recursion trouble.
	 */
	if (in_error_recursion_trouble())
		entry->changing_xact_state = true;

	/*
	 * If connection is before starting transaction or is already unsalvageable,
	 * do only the cleanup and don't touch it further.
	 */
	if (entry->changing_xact_state)
	{
		pgfdw_cleanup_after_transaction(entry);
		return;
	}

	/*
	 * Mark this connection as in the process of changing
	 * transaction state.
	 */
	entry->changing_xact_state = true;

	/* Assume we might have lost track of prepared statements */
	entry->have_error = true;

	/*
	 * If a command has been submitted to the remote server by
	 * using an asynchronous execution function, the command
	 * might not have yet completed.  Check to see if a
	 * command is still being processed by the remote server,
	 * and if so, request cancellation of the command.
	 */
	if (PQtransactionStatus(entry->conn) == PQTRANS_ACTIVE &&
		!pgfdw_cancel_query(entry->conn))
	{
		/* Unable to cancel running query. */
		abort_cleanup_failure = true;
	}
	else if (!pgfdw_exec_cleanup_query(entry->conn,
									   "ABORT TRANSACTION",
									   false))
	{
		/* Unable to abort remote transaction. */
		abort_cleanup_failure = true;
	}
	else if (entry->have_prep_stmt && entry->have_error &&
			 !pgfdw_exec_cleanup_query(entry->conn,
									   "DEALLOCATE ALL",
									   true))
	{
		/* Trouble clearing prepared statements. */
		abort_cleanup_failure = true;
	}

	/* Disarm changing_xact_state if it all worked. */
	entry->changing_xact_state = abort_cleanup_failure;

	/* Cleanup transaction status */
	pgfdw_cleanup_after_transaction(entry);

	return;
}

static CSN
pgfdw_prepare_remote_csn_snapshot(ConnCacheEntry *entry, char *fdwxact_id)
{
	PGresult	*res;
	CSN		csn = 0;
	char 	*resp;
	char	sql[256];

	snprintf(sql, sizeof(sql),
		"SELECT pg_csn_snapshot_prepare('%s')",fdwxact_id);

	/* Do prepare foreign transaction */
	entry->changing_xact_state = true;
	res = pgfdw_exec_query(entry->conn, sql);
	entry->changing_xact_state = false;

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		ereport(ERROR,
				(errmsg("could not prepare CSN snapshot with ID %s",fdwxact_id)));
	resp = PQgetvalue(res, 0, 0);

	if (resp == NULL || (*resp) == '\0' ||
			sscanf(resp, UINT64_FORMAT, &csn) != 1)
		ereport(ERROR,
				(errmsg("pg_csn_snapshot_prepare returned invalid data for prepared transaction with ID %s",
							   fdwxact_id)));
	return csn;
}

static void
pgfdw_assign_global_snapshot_to_xact(ConnCacheEntry *entry, char *fdwxact_id, CSN csn)
{
	PGresult	*res;
	char	sql[256];

	snprintf(sql, sizeof(sql),
			 "SELECT pg_csn_snapshot_assign('%s', "UINT64_FORMAT")", fdwxact_id, csn);
	res = pgfdw_exec_query(entry->conn, sql);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		ereport(ERROR,
				(errmsg("could not assign global CSN to prepared transaction with ID %s",
						fdwxact_id)));
	PQclear(res);
}
/*
 * Prepare a transaction on foreign server.
 */
void
postgresPrepareForeignTransaction(FdwXactRslvState *frstate)
{
	ConnCacheEntry *entry = NULL;
	PGresult	*res;
	StringInfo	command;

	/* The transaction should have started already get the cache entry */
	entry = GetConnectionCacheEntry(frstate->usermapping->umid);
	Assert(entry->conn);

	pgfdw_reject_incomplete_xact_state_change(entry);

	command = makeStringInfo();
	appendStringInfo(command, "PREPARE TRANSACTION '%s'", frstate->fdwxact_id);

	/* Do prepare foreign transaction */
	entry->changing_xact_state = true;
	res = pgfdw_exec_query(entry->conn, command->data);
	entry->changing_xact_state = false;

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		ereport(ERROR, (errmsg("could not prepare transaction on server %s with ID %s",
							   frstate->server->servername, frstate->fdwxact_id)));

	elog(DEBUG1, "prepared foreign transaction on server %s with ID %s",
		 frstate->server->servername, frstate->fdwxact_id);

	if (entry->have_prep_stmt && entry->have_error)
	{
		res = PQexec(entry->conn, "DEALLOCATE ALL");
		PQclear(res);
	}

	pgfdw_cleanup_after_transaction(entry);
}

/* Cleanup at main-transaction end */
static void
pgfdw_cleanup_after_transaction(ConnCacheEntry *entry)
{
	/* Reset state to show we're out of a transaction */
	entry->xact_depth = 0;
	entry->have_prep_stmt = false;
	entry->have_error  = false;
	entry->imported_csn = InvalidCSN;

	/*
	 * If the connection isn't in a good idle state, discard it to
	 * recover. Next GetConnection will open a new connection.
	 */
	if (PQstatus(entry->conn) != CONNECTION_OK ||
		PQtransactionStatus(entry->conn) != PQTRANS_IDLE ||
		entry->changing_xact_state)
	{
		elog(DEBUG3, "discarding connection %p", entry->conn);
		disconnect_pg_server(entry);
	}

	entry->changing_xact_state = false;

	/* Also reset cursor numbering for next transaction */
	cursor_number = 0;
}

CSN
postgresPrepareForeignCSNSnapshot(FdwXactRslvState *frstate)
{
	uint64 csn;
	ConnCacheEntry *entry = NULL;
	/*
	 * The foreign transaction must already have been prepared
	 * and we might not have a connection to it. So We get a connection
	 * but don't start transaction.
	 */
	entry = GetConnectionCacheEntry(frstate->usermapping->umid);
	csn = pgfdw_prepare_remote_csn_snapshot(entry, frstate->fdwxact_id);
	/* Cleanup transaction status */
	pgfdw_cleanup_after_transaction(entry);
	return csn;
}

void
postgresAssignGlobalCSN(FdwXactRslvState *frstate, CSN max_csn)
{
	ConnCacheEntry *entry = NULL;
	/*
	 * The foreign transaction must already have been prepared
	 * and we might not have a connection to it. So We get a connection
	 * but don't start transaction.
	 */

	entry = GetConnectionCacheEntry(frstate->usermapping->umid);
	pgfdw_assign_global_snapshot_to_xact(entry, frstate->fdwxact_id, max_csn);

	elog(DEBUG1, "global CSN "UINT64_FORMAT" assigned to prepared foreign transaction with ID %s",
		 max_csn, frstate->fdwxact_id);

	/* Cleanup transaction status */
	pgfdw_cleanup_after_transaction(entry);

}

/*
 * Commit or rollback prepared transaction on the foreign server.
 */
static void
pgfdw_end_prepared_xact(ConnCacheEntry *entry, UserMapping *usermapping,
						char *fdwxact_id, bool is_commit)
{
	StringInfo	command;
	PGresult	*res;

	/*
	 * Check the connection status for the case the previous attempt
	 * failed.
	 */
	if (entry->conn && PQstatus(entry->conn) != CONNECTION_OK)
		disconnect_pg_server(entry);

	/*
	 * In two-phase commit case, since the transaction is about to be
	 * resolved by a different process than the process who prepared it,
	 * we might not have a connection yet.
	 */
	if (!entry->conn)
		make_new_connection(entry, usermapping);

	command = makeStringInfo();
	appendStringInfo(command, "%s PREPARED '%s'",
					 is_commit ? "COMMIT" : "ROLLBACK",
					 fdwxact_id);

	/*
	 * Once the transaction is prepared, further transaction callback is not
	 * called even when an error occurred during resolving it.  Therefore, we
	 * don't need to set changing_xact_state here.  On failure the new connection
	 * will be established either when the new transaction is started or when
	 * checking the connection status above.
	 */
	res = pgfdw_exec_query(entry->conn, command->data);

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		int		sqlstate;
		char	*diag_sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);

		if (diag_sqlstate)
		{
			sqlstate = MAKE_SQLSTATE(diag_sqlstate[0],
									 diag_sqlstate[1],
									 diag_sqlstate[2],
									 diag_sqlstate[3],
									 diag_sqlstate[4]);
		}
		else
			sqlstate = ERRCODE_CONNECTION_FAILURE;

		/*
		 * As core global transaction manager states, it's possible that the
		 * given foreign transaction doesn't exist on the foreign server. So
		 * we should accept an UNDEFINED_OBJECT error.
		 */
		if (sqlstate != ERRCODE_UNDEFINED_OBJECT)
			pgfdw_report_error(ERROR, res, entry->conn, false, command->data);
	}

	elog(DEBUG1, "%s prepared foreign transaction with ID %s",
		 is_commit ? "commit" : "rollback",
		 fdwxact_id);

	/* Cleanup transaction status */
	pgfdw_cleanup_after_transaction(entry);
}
