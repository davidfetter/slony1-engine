/*-------------------------------------------------------------------------
 * sync_thread.c
 *
 *	Implementation of the thread that generates SYNC events.
 *
 *	Copyright (c) 2003-2009, PostgreSQL Global Development Group
 *	Author: Jan Wieck, Afilias USA INC.
 *
 *	$Id: sync_thread.c,v 1.20.2.1 2009-08-17 17:09:58 devrim Exp $
 *-------------------------------------------------------------------------
 */


#include <pthread.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>

#include "slon.h"


/* ---------- 
 * Global variables 
 * ----------
 */
int			sync_interval;
int			sync_interval_timeout;


/* ---------- 
 * slon_localSyncThread
 *
 * Generate SYNC event if local database activity created new log info.
 * ----------
 */
void *
syncThread_main(void *dummy)
{
	SlonConn   *conn;
	char		last_actseq_buf[64];
	SlonDString query1;
	SlonDString query2;
	SlonDString query3;
	PGconn	   *dbconn;
	PGresult   *res;
	int			timeout_count;
	bool first_time = TRUE;

	slon_log(SLON_INFO,
			 "syncThread: thread starts\n");

	/*
	 * Connect to the local database
	 */
	if ((conn = slon_connectdb(rtcfg_conninfo, "local_sync")) == NULL)
		slon_retry();
	dbconn = conn->dbconn;

	/*
	 * We don't initialize the last known action sequence to the actual value.
	 * This causes that we create a SYNC event allways on startup, just in
	 * case.
	 */
	last_actseq_buf[0] = '\0';

	/*
	 * Build the query that starts a transaction and retrieves the last value
	 * from the action sequence.
	 */
	dstring_init(&query1);
	slon_mkquery(&query1,
				 "start transaction;"
				 "set transaction isolation level serializable;"
				 "select last_value from %s.sl_action_seq;",
				 rtcfg_namespace);

	/*
	 * Build the query that calls createEvent() for the SYNC
	 */
	dstring_init(&query2);
	slon_mkquery(&query2,
		     "select %s.createEvent('_%s', 'SYNC', NULL)"
		     " from %s.sl_node where no_id = %s.getLocalNodeId('_%s') "
		     " and exists (select 1 from %s.sl_set where set_origin= no_id);",
		     rtcfg_namespace, rtcfg_cluster_name,
		     rtcfg_namespace, rtcfg_namespace, rtcfg_cluster_name, rtcfg_namespace);

	dstring_init(&query3);
	slon_mkquery(&query3,
		     "select %s.createEvent('_%s', 'SYNC', NULL);",
		     rtcfg_namespace, rtcfg_cluster_name);

	timeout_count = (sync_interval_timeout == 0) ? 0 :
		sync_interval_timeout - sync_interval;
	while (sched_wait_time(conn, SCHED_WAIT_SOCK_READ, sync_interval) == SCHED_STATUS_OK)
	{
		/*
		 * Start a serializable transaction and get the last value from the
		 * action sequence number.
		 */
		res = PQexec(dbconn, dstring_data(&query1));
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			slon_log(SLON_FATAL,
					 "syncThread: \"%s\" - %s",
					 dstring_data(&query1), PQresultErrorMessage(res));
			PQclear(res);
			slon_retry();
			break;
		}

		/*
		 * Check if it's identical to the last known seq or if the sync
		 * interval timeout has arrived.
		 */
		if (sync_interval_timeout != 0)
			timeout_count -= sync_interval;

		if (strcmp(last_actseq_buf, PQgetvalue(res, 0, 0)) != 0 ||
			timeout_count < 0)
		{
			/*
			 * Action sequence has changed, generate a SYNC event and read the
			 * resulting currval of the event sequence.
			 */
			strcpy(last_actseq_buf, PQgetvalue(res, 0, 0));

			PQclear(res);
			res = PQexec(dbconn, dstring_data(first_time? &query3:&query2));
			if (PQresultStatus(res) != PGRES_TUPLES_OK)
			{
				slon_log(SLON_FATAL,
						 "syncThread: \"%s\" - %s",
					 dstring_data(first_time?&query3:&query2), PQresultErrorMessage(res));
				PQclear(res);
				slon_retry();
				break;
			}
			if (first_time) {
				slon_log(SLON_DEBUG2,
					 "syncThread: new sl_action_seq %s - SYNC %s\n",
					 last_actseq_buf, PQgetvalue(res, 0, 0));
			}
			PQclear(res);
			first_time = FALSE;

			/*
			 * Commit the transaction
			 */
			res = PQexec(dbconn, "commit transaction;");
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				slon_log(SLON_FATAL,
						 "syncThread: \"commit transaction;\" - %s",
						 PQresultErrorMessage(res));
				PQclear(res);
				slon_retry();
			}
			PQclear(res);

			/*
			 * Restart the timeout on a sync.
			 */
			timeout_count = (sync_interval_timeout == 0) ? 0 :
				sync_interval_timeout - sync_interval;
		}
		else
		{
			/*
			 * No database activity detected - rollback.
			 */
			PQclear(res);
			res = PQexec(dbconn, "rollback transaction;");
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				slon_log(SLON_FATAL,
						 "syncThread: \"rollback transaction;\" - %s",
						 PQresultErrorMessage(res));
				PQclear(res);
				slon_retry();
			}
			PQclear(res);
		}
	}

	dstring_free(&query1);
	dstring_free(&query2);
	dstring_free(&query3);
	slon_disconnectdb(conn);

	slon_log(SLON_INFO, "syncThread: thread done\n");
	pthread_exit(NULL);
}

/*
 * Local Variables:
 *	tab-width: 4
 *	c-indent-level: 4
 *	c-basic-offset: 4
 * End:
 */
