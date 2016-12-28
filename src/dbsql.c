#include "common.h"
#include "misc.h"
#include "dbsql.h"

int db_open(const int createifnotfound)
{
	int rc, createdb = 0;
	char dbfilename[512];
	struct stat filestat;

	snprintf(dbfilename, 512, "%s/%s", cfg.dbdir, DATABASEFILE);

	/* create database if file doesn't exist */
	if (stat(dbfilename, &filestat) != 0) {
		if (errno == ENOENT && createifnotfound) {
			createdb = 1;
		} else {
			if (debug)
				printf("Error: Handling database \"%s\" failed: %s\n", dbfilename, strerror(errno));
			return 0;
		}
	} else {
		if (filestat.st_size == 0) {
			if (createifnotfound) {
				createdb = 1;
			} else {
				printf("Error: Database \"%s\" contains 0 bytes and isn't a valid database, exiting.\n", dbfilename);
				exit(EXIT_FAILURE);
			}
		}
	}

#ifdef TESTDIR
	/* use ram based database when testing for shorter test execution times */
	rc = sqlite3_open(NULL, &db);
#else
	rc = sqlite3_open(dbfilename, &db);
#endif

	if (rc) {
		if (debug)
			printf("Error: Can't open database \"%s\": %s\n", dbfilename, sqlite3_errmsg(db));
		return 0;
	} else {
		if (debug)
			printf("Database \"%s\" open\n", dbfilename);
	}

	if (createdb) {
		if (!spacecheck(cfg.dbdir)) {
			printf("Error: Not enough free diskspace available in \"%s\", exiting.\n", cfg.dbdir);
			exit(EXIT_FAILURE);
		}
		if (!db_create()) {
			if (debug)
				printf("Error: Creating database \"%s\" structure failed\n", dbfilename);
			return 0;
		} else {
			if (debug)
				printf("Database \"%s\" structure created\n", dbfilename);
			if (!db_setinfo("dbversion", SQLDBVERSION, 1)) {
				if (debug)
					printf("Error: Writing version info to database \"%s\" failed\n", dbfilename);
				return 0;
			}
		}
	}

	if (createifnotfound) {
		if (!db_setinfo("vnstatversion", getversion(), 1)) {
			return 0;
		}
	}

	return 1;
}

int db_close(void)
{
	int rc;
	rc = sqlite3_close(db);
	if (rc == SQLITE_OK) {
		return 1;
	} else {
		if (debug)
			printf("Error: Closing database failed (%d): %s\n", rc, sqlite3_errmsg(db));
		return 0;
	}
}

int db_exec(const char *sql)
{
	int rc;
	sqlite3_stmt *sqlstmt;

	rc = sqlite3_prepare_v2(db, sql, -1, &sqlstmt, NULL);
	if (rc) {
		if (debug)
			printf("Error: Insert \"%s\" prepare failed (%d): %s\n", sql, rc, sqlite3_errmsg(db));
		return 0;
	}

	rc = sqlite3_step(sqlstmt);
	if (rc != SQLITE_DONE) {
		if (debug)
			printf("Error: Insert \"%s\" step failed (%d): %s\n", sql, rc, sqlite3_errmsg(db));
		sqlite3_finalize(sqlstmt);
		return 0;
	}

	rc = sqlite3_finalize(sqlstmt);
	if (rc) {
		if (debug)
			printf("Error: Finalize \"%s\" failed (%d): %s\n", sql, rc, sqlite3_errmsg(db));
		return 0;
	}

	return 1;
}

int db_create(void)
{
	int i;
	char *sql;
	char *datatables[] = {"fiveminute", "hour", "day", "month", "year"};

	/* TODO: check: COMMIT, END or ROLLBACK may be missing in error cases and return gets called before COMMIT */

	if (!db_begintransaction()) {
		return 0;
	}

	sql = "CREATE TABLE info(\n" \
		"  id       INTEGER PRIMARY KEY,\n" \
		"  name     TEXT UNIQUE NOT NULL,\n" \
		"  value    TEXT NOT NULL);";

	if (!db_exec(sql)) {
		return 0;
	}

	sql = "CREATE TABLE interface(\n" \
		"  id           INTEGER PRIMARY KEY,\n" \
		"  name         TEXT UNIQUE NOT NULL,\n" \
		"  alias        TEXT,\n" \
		"  active       INTEGER NOT NULL,\n" \
		"  created      DATE NOT NULL,\n" \
		"  updated      DATE NOT NULL,\n" \
		"  rxcounter    INTEGER NOT NULL,\n" \
		"  txcounter    INTEGER NOT NULL,\n" \
		"  rxtotal      INTEGER NOT NULL,\n" \
		"  txtotal      INTEGER NOT NULL);";

	if (!db_exec(sql)) {
		return 0;
	}

	sql = malloc(sizeof(char)*512);
	for (i=0; i<5; i++) {
		sqlite3_snprintf(512, sql, "CREATE TABLE %s(\n" \
			"  id           INTEGER PRIMARY KEY,\n" \
			"  interface    INTEGER REFERENCES interface ON DELETE CASCADE,\n" \
			"  date         DATE NOT NULL,\n" \
			"  rx           INTEGER NOT NULL,\n" \
			"  tx           INTEGER NOT NULL,\n" \
			"  CONSTRAINT u UNIQUE (interface, date));", datatables[i]);

		if (!db_exec(sql)) {
			free(sql);
			return 0;
		}
	}
	free(sql);

	return db_committransaction();
}

int db_addinterface(const char *iface)
{
	char sql[1024];

	sqlite3_snprintf(1024, sql, "insert into interface (name, active, created, updated, rxcounter, txcounter, rxtotal, txtotal) values ('%q', 1, datetime('now', 'localtime'), datetime('now', 'localtime'), 0, 0, 0, 0);", iface);
	return db_exec(sql);
}

uint64_t db_getinterfacecount(void)
{
	int rc;
	uint64_t result = 0;
	char sql[512];
	sqlite3_stmt *sqlstmt;

	sqlite3_snprintf(512, sql, "select count(*) from interface");
	rc = sqlite3_prepare_v2(db, sql, -1, &sqlstmt, NULL);
	if (rc) {
		return 0;
	}
	if (sqlite3_column_count(sqlstmt) != 1) {
		return 0;
	}
	if (sqlite3_step(sqlstmt) == SQLITE_ROW) {
		result = (uint64_t)sqlite3_column_int64(sqlstmt, 0);
	}
	sqlite3_finalize(sqlstmt);

	return result;
}

sqlite3_int64 db_getinterfaceid(const char *iface, const int createifnotfound)
{
	int rc;
	char sql[512];
	sqlite3_int64 ifaceid = 0;
	sqlite3_stmt *sqlstmt;

	sqlite3_snprintf(512, sql, "select id from interface where name='%q'", iface);
	rc = sqlite3_prepare_v2(db, sql, -1, &sqlstmt, NULL);
	if (!rc) {
		if (sqlite3_step(sqlstmt) == SQLITE_ROW) {
			ifaceid = sqlite3_column_int64(sqlstmt, 0);
		}
		sqlite3_finalize(sqlstmt);
	}

	if (ifaceid == 0 && createifnotfound) {
		if (!db_addinterface(iface)) {
			return 0;
		}
		ifaceid = sqlite3_last_insert_rowid(db);
	}

	return ifaceid;
}

int db_setactive(const char *iface, const int active)
{
	char sql[512];
	sqlite3_int64 ifaceid = 0;

	ifaceid = db_getinterfaceid(iface, 0);
	if (ifaceid == 0) {
		return 0;
	}

	sqlite3_snprintf(512, sql, "update interface set active=%d where id=%"PRId64";", active, (int64_t)ifaceid);
	return db_exec(sql);
}

int db_setcounters(const char *iface, const uint64_t rxcounter, const uint64_t txcounter)
{
	char sql[512];
	sqlite3_int64 ifaceid = 0;

	ifaceid = db_getinterfaceid(iface, 0);
	if (ifaceid == 0) {
		return 0;
	}

	sqlite3_snprintf(512, sql, "update interface set rxcounter=%"PRIu64", txcounter=%"PRIu64" where id=%"PRId64";", rxcounter, txcounter, (int64_t)ifaceid);
	return db_exec(sql);
}

int db_getcounters(const char *iface, uint64_t *rxcounter, uint64_t *txcounter)
{
	int rc;
	char sql[512];
	sqlite3_int64 ifaceid = 0;
	sqlite3_stmt *sqlstmt;

	*rxcounter = *txcounter = 0;

	ifaceid = db_getinterfaceid(iface, 0);
	if (ifaceid == 0) {
		return 0;
	}

	sqlite3_snprintf(512, sql, "select rxcounter, txcounter from interface where id=%"PRId64";", (int64_t)ifaceid);
	rc = sqlite3_prepare_v2(db, sql, -1, &sqlstmt, NULL);
	if (rc) {
		return 0;
	}
	if (sqlite3_column_count(sqlstmt) != 2) {
		return 0;
	}
	if (sqlite3_step(sqlstmt) == SQLITE_ROW) {
		*rxcounter = (uint64_t)sqlite3_column_int64(sqlstmt, 0);
		*txcounter = (uint64_t)sqlite3_column_int64(sqlstmt, 1);
	}
	sqlite3_finalize(sqlstmt);

	return 1;
}

int db_setalias(const char *iface, const char *alias)
{
	char sql[512];
	sqlite3_int64 ifaceid = 0;

	ifaceid = db_getinterfaceid(iface, 0);
	if (ifaceid == 0) {
		return 0;
	}

	sqlite3_snprintf(512, sql, "update interface set alias='%q' where id=%"PRId64";", alias, (int64_t)ifaceid);
	return db_exec(sql);
}

int db_setinfo(const char *name, const char *value, const int createifnotfound)
{
	int rc;
	char sql[512];

	sqlite3_snprintf(512, sql, "update info set value='%q' where name='%q';", value, name);
	rc = db_exec(sql);
	if (!rc || (!sqlite3_changes(db) && !createifnotfound)) {
		return 0;
	}
	if (!sqlite3_changes(db) && createifnotfound) {
		sqlite3_snprintf(512, sql, "insert into info (name, value) values ('%q', '%q');", name, value);
		rc = db_exec(sql);
	}
	return rc;
}

char *db_getinfo(const char *name)
{
	int rc;
	char sql[512];
	static char buffer[64];
	sqlite3_stmt *sqlstmt;

	buffer[0] = '\0';

	sqlite3_snprintf(512, sql, "select value from info where name='%q';", name);
	rc = sqlite3_prepare_v2(db, sql, -1, &sqlstmt, NULL);
	if (rc) {
		return buffer;
	}
	if (sqlite3_step(sqlstmt) == SQLITE_ROW) {
		strncpy_nt(buffer, (const char *)sqlite3_column_text(sqlstmt, 0), 64);
	}
	sqlite3_finalize(sqlstmt);

	return buffer;
}

int db_addtraffic(const char *iface, const uint64_t rx, const uint64_t tx)
{
	return db_addtraffic_dated(iface, rx, tx, 0);
}

int db_addtraffic_dated(const char *iface, const uint64_t rx, const uint64_t tx, const uint64_t timestamp)
{
	int i;
	char sql[1024], datebuffer[512], nowdate[64];
	sqlite3_int64 ifaceid = 0;

	char *datatables[] = {"fiveminute", "hour", "day", "month", "year"};
	char *datadates[] = {"datetime(%1$s, ('-' || (strftime('%%M', %1$s)) || ' minutes'), ('-' || (strftime('%%S', %1$s)) || ' seconds'), ('+' || (round(strftime('%%M', %1$s)/5,0)*5) || ' minutes'), 'localtime')", \
			"strftime('%%Y-%%m-%%d %%H:00:00', %s, 'localtime')", \
			"date(%s, 'localtime')", \
			"strftime('%%Y-%%m-01', %s, 'localtime')", \
			"strftime('%%Y-01-01', %s, 'localtime')"};

	if (rx == 0 && tx == 0) {
		return 1;
	}

	ifaceid = db_getinterfaceid(iface, 1);
	if (ifaceid == 0) {
		return 0;
	}

	if (timestamp > 0) {
		snprintf(nowdate, 64, "datetime(%"PRIu64", 'unixepoch')", timestamp);
	} else {
		snprintf(nowdate, 64, "'now'");
	}

	if (debug)
		printf("add %s (%"PRId64"): rx %"PRIu64" - tx %"PRIu64"\n", iface, (int64_t)ifaceid, rx, tx);

	if (!db_begintransaction()) {
		return 0;
	}

	/* total */
	sqlite3_snprintf(1024, sql, "update interface set rxtotal=rxtotal+%"PRIu64", txtotal=txtotal+%"PRIu64", updated=datetime(%s, 'localtime'), active=1 where id=%"PRId64";", rx, tx, nowdate, (int64_t)ifaceid);
	db_exec(sql);

	/* time specific */
	for (i=0; i<5; i++) {
		sqlite3_snprintf(1024, sql, "insert or ignore into %s (interface, date, rx, tx) values (%"PRId64", %s, 0, 0);", datatables[i], (int64_t)ifaceid, datadates[i]);
		db_exec(sql);
		snprintf(datebuffer, 512, datadates[i], nowdate);
		sqlite3_snprintf(1024, sql, "update %s set rx=rx+%"PRIu64", tx=tx+%"PRIu64" where interface=%"PRId64" and date=%s;", datatables[i], rx, tx, (int64_t)ifaceid, datebuffer);
		db_exec(sql);
	}

	return db_committransaction();
}

int db_removeoldentries(void)
{
	char sql[512];

	if (!db_begintransaction()) {
		return 0;
	}

	sqlite3_snprintf(512, sql, "delete from fiveminute where date < datetime('now', '-48 hours', 'localtime');");
	db_exec(sql);

	sqlite3_snprintf(512, sql, "delete from hour where date < datetime('now', '-7 days', 'localtime');");
	db_exec(sql);

	sqlite3_snprintf(512, sql, "delete from day where date < date('now', '-30 days', 'localtime');");
	db_exec(sql);

	sqlite3_snprintf(512, sql, "delete from month where date < date('now', '-12 months', 'localtime');");
	db_exec(sql);

	sqlite3_snprintf(512, sql, "delete from year where date < date('now', '-10 years', 'localtime');");
	db_exec(sql);

	return db_committransaction();
}

int db_vacuum(void)
{
	return db_exec("VACUUM;");
}

int db_begintransaction(void)
{
	int rc;

	rc = sqlite3_exec(db, "BEGIN", 0, 0, 0);
	if (rc) {
		if (debug)
			printf("Error: BEGIN failed (%d): %s\n", rc, sqlite3_errmsg(db));
		return 0;
	}
	return 1;
}

int db_committransaction(void)
{
	int rc;

	rc = sqlite3_exec(db, "COMMIT", 0, 0, 0);
	if (rc) {
		if (debug)
			printf("Error: COMMIT failed (%d): %s\n", rc, sqlite3_errmsg(db));
		return 0;
	}
	return 1;
}

int db_rollbacktransaction(void)
{
	int rc;

	rc = sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
	if (rc) {
		if (debug)
			printf("Error: ROLLBACK failed (%d): %s\n", rc, sqlite3_errmsg(db));
		return 0;
	}
	return 1;
}