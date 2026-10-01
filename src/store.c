#include "header.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char schema[] =
    "CREATE TABLE users(id INTEGER PRIMARY KEY AUTOINCREMENT CHECK(id BETWEEN 0 AND 2147483647),"
    "name TEXT NOT NULL UNIQUE COLLATE NOCASE,password TEXT NOT NULL);"
    "CREATE TABLE accounts(record_id INTEGER PRIMARY KEY AUTOINCREMENT,number INTEGER NOT NULL "
    "UNIQUE CHECK(number BETWEEN 0 AND 2147483647),"
    "owner INTEGER NOT NULL REFERENCES users(id),created TEXT NOT NULL,country TEXT NOT NULL,phone "
    "TEXT NOT NULL,"
    "balance INTEGER NOT NULL CHECK(balance BETWEEN 0 AND 10000000000000),"
    "opening_balance INTEGER NOT NULL CHECK(opening_balance BETWEEN 0 AND 10000000000000),"
    "kind TEXT NOT NULL CHECK(kind IN('savings','current','fixed01','fixed02','fixed03')),active "
    "INTEGER NOT NULL DEFAULT 1 CHECK(active IN(0,1)));"
    "CREATE TABLE transactions(id INTEGER PRIMARY KEY AUTOINCREMENT,account_id INTEGER NOT NULL "
    "REFERENCES accounts(number),"
    "kind TEXT NOT NULL CHECK(kind IN('deposit','withdrawal')),amount INTEGER NOT NULL "
    "CHECK(amount BETWEEN 1 AND 10000000000000),day TEXT NOT NULL);"
    "CREATE INDEX transaction_account ON transactions(account_id,id);"
    "CREATE TABLE notifications(id INTEGER PRIMARY KEY AUTOINCREMENT,recipient INTEGER NOT NULL "
    "REFERENCES users(id),"
    "account_id INTEGER NOT NULL REFERENCES accounts(number),sender TEXT NOT NULL,created TEXT NOT "
    "NULL DEFAULT CURRENT_TIMESTAMP);"
    "CREATE INDEX notification_recipient ON notifications(recipient,id);"
    "CREATE TABLE metadata(revision INTEGER NOT NULL CHECK(revision>=0));"
    "INSERT INTO metadata VALUES(0);PRAGMA user_version=1;";

bool store_exec(App *app, const char *sql) {
    char *message = NULL;
    int result = sqlite3_exec(app->db, sql, NULL, NULL, &message);
    if (result != SQLITE_OK)
        app_error(app, "Database error: %s", message ? message : sqlite3_errmsg(app->db));
    sqlite3_free(message);
    return result == SQLITE_OK;
}

bool store_prepare(App *app, sqlite3_stmt **statement, const char *sql) {
    if (sqlite3_prepare_v2(app->db, sql, -1, statement, NULL) != SQLITE_OK)
        return app_error(app, "Database error: %s", sqlite3_errmsg(app->db));
    return true;
}

bool store_done(App *app, sqlite3_stmt *statement) {
    int result = sqlite3_step(statement);
    if (result != SQLITE_DONE)
        app_error(app, "Database error: %s", sqlite3_errmsg(app->db));
    sqlite3_finalize(statement);
    return result == SQLITE_DONE;
}

bool store_lock(App *app) {
    if (app->locked)
        return app_error(app, "Internal error: nested storage lock.");
    struct timespec started, now, pause = {0, 10000000};
    if (clock_gettime(CLOCK_MONOTONIC, &started) != 0)
        return app_error(app, "Cannot read monotonic clock.");
    while (flock(app->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
            return app_error(app, "Cannot lock data: %s", strerror(errno));
        if (teller_stop)
            return app_error(app, "Operation interrupted before changing data.");
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec - started.tv_sec >= 5)
            return app_error(app, "Data is busy in another session; try again.");
        nanosleep(&pause, NULL);
    }
    app->locked = true;
    return true;
}

void store_unlock(App *app) {
    if (app->locked) {
        (void)flock(app->lock_fd, LOCK_UN);
        app->locked = false;
    }
}

void store_abort(App *app) {
    if (app->db && !sqlite3_get_autocommit(app->db))
        (void)sqlite3_exec(app->db, "ROLLBACK", NULL, NULL, NULL);
    store_unlock(app);
}

bool store_begin(App *app) {
    if (app->fatal)
        return app_error(app, "Storage needs recovery; restart before making another change.");
    if (!store_lock(app))
        return false;
    if (!store_ensure_views(app) || !store_exec(app, "BEGIN IMMEDIATE")) {
        store_abort(app);
        return false;
    }
    return true;
}

void store_test_crash(const char *point) {
#ifdef TELLER_TESTING
    const char *requested = getenv("TELLER_FAILPOINT");
    if (requested && !strcmp(requested, point))
        _exit(86);
#else
    (void)point;
#endif
}

bool store_finish(App *app) {
    char snapshot[PATH_CAP];
    if (!store_exec(app, "UPDATE metadata SET revision=revision+1") ||
        !store_snapshot(app, snapshot)) {
        store_abort(app);
        return false;
    }
    store_test_crash("before-commit");
    if (!store_exec(app, "COMMIT")) {
        store_abort(app);
        return false;
    }
    store_test_crash("after-commit");
    if (!store_publish(app, snapshot)) {
        char reason[256];
        snprintf(reason, sizeof(reason), "%s", app->error);
        app->fatal = true;
        store_unlock(app);
        return app_error(
            app,
            "Saved in SQLite; text publication failed (%.140s). Do not repeat. Restart to recover.",
            reason);
    }
    store_test_crash("after-publish");
    store_unlock(app);
    return true;
}

static bool check_database(App *app) {
    sqlite3_stmt *statement = NULL;
    if (!store_prepare(app, &statement, "PRAGMA quick_check"))
        return false;
    bool ok = sqlite3_step(statement) == SQLITE_ROW &&
              !strcmp((const char *)sqlite3_column_text(statement, 0), "ok");
    sqlite3_finalize(statement);
    if (!ok)
        return app_error(app, "Database integrity check failed; originals were preserved.");
    if (!store_prepare(app, &statement, "PRAGMA foreign_key_check"))
        return false;
    ok = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (!ok)
        return app_error(app, "Database contains invalid references; originals were preserved.");
    /* The ledger must explain every cent, including archived accounts. */
    const char *totals =
        "WITH totals AS (SELECT a.number,a.kind,a.balance,a.opening_balance,"
        "COALESCE(SUM(t.amount),0) volume,"
        "COALESCE(SUM(CASE WHEN t.kind='deposit' THEN t.amount ELSE -t.amount END),0) delta "
        "FROM accounts a LEFT JOIN transactions t ON t.account_id=a.number GROUP BY a.number) "
        "SELECT number FROM totals WHERE volume>? OR balance!=opening_balance+delta "
        "OR (kind LIKE 'fixed%' AND volume>0) LIMIT 1";
    if (!store_prepare(app, &statement, totals))
        return false;
    sqlite3_bind_int64(statement, 1, LEDGER_MAX);
    ok = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (!ok)
        return app_error(
            app, "Balance and transaction history are inconsistent; originals were preserved.");
    const char *running =
        "WITH history AS (SELECT a.opening_balance+SUM(CASE WHEN t.kind='deposit' "
        "THEN t.amount ELSE -t.amount END) OVER(PARTITION BY t.account_id ORDER BY t.id) balance "
        "FROM transactions t JOIN accounts a ON a.number=t.account_id) "
        "SELECT balance FROM history WHERE balance<0 OR balance>? LIMIT 1";
    if (!store_prepare(app, &statement, running))
        return false;
    sqlite3_bind_int64(statement, 1, MONEY_MAX);
    ok = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (!ok)
        return app_error(
            app, "History contains an impossible intermediate balance; originals were preserved.");
    return true;
}

bool store_open(App *app, const char *directory) {
    memset(app, 0, sizeof(*app));
    app->lock_fd = -1;
    if (!directory || !*directory || strlen(directory) > PATH_CAP - 200)
        return app_error(app, "Data directory path is too long or empty.");
    snprintf(app->directory, sizeof(app->directory), "%s", directory);
    if (mkdir(directory, 0700) != 0 && errno != EEXIST)
        return app_error(app, "Cannot create data directory: %s", strerror(errno));
    struct stat info;
    if (lstat(directory, &info) != 0 || !S_ISDIR(info.st_mode))
        return app_error(app, "Data path must be a real directory.");
    char path[PATH_CAP];
    if (!app_path(app, ".lock", path))
        return app_error(app, "Path too long.");
    app->lock_fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (app->lock_fd < 0)
        return app_error(app, "Cannot open data lock: %s", strerror(errno));
    if (!store_lock(app))
        return false;
    if (!app_path(app, "teller.db", path)) {
        store_unlock(app);
        return app_error(app, "Path too long.");
    }
    if (lstat(path, &info) == 0 && !S_ISREG(info.st_mode)) {
        store_unlock(app);
        return app_error(app, "Database path must be a regular file.");
    }
    if (sqlite3_open_v2(path, &app->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) !=
        SQLITE_OK) {
        store_unlock(app);
        return app_error(app, "Cannot open database: %s", sqlite3_errmsg(app->db));
    }
    sqlite3_busy_timeout(app->db, 2000);
    if (!store_exec(app,
                    "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;")) {
        store_abort(app);
        return false;
    }
    sqlite3_stmt *statement = NULL;
    if (!store_prepare(app, &statement, "PRAGMA user_version")) {
        store_abort(app);
        return false;
    }
    int result = sqlite3_step(statement);
    int version = result == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
    sqlite3_finalize(statement);
    if (version == 0) {
        if (!store_exec(app, "BEGIN IMMEDIATE") || !store_exec(app, schema) || !store_import(app) ||
            !check_database(app)) {
            store_abort(app);
            return false;
        }
        if (!store_finish(app))
            return false;
        if (!store_lock(app))
            return false;
    } else if (version != 1) {
        store_unlock(app);
        return app_error(app, "Unsupported database schema version %d.", version);
    }
    /* Rebuild from the authoritative database at startup, including when a
     * text projection was manually damaged without changing its version. */
    char snapshot[PATH_CAP];
    bool ok = check_database(app) && store_snapshot(app, snapshot) && store_publish(app, snapshot);
    store_unlock(app);
    return ok;
}

void store_close(App *app) {
    store_abort(app);
    if (app->db) {
        sqlite3_close(app->db);
        app->db = NULL;
    }
    if (app->lock_fd >= 0) {
        close(app->lock_fd);
        app->lock_fd = -1;
    }
}
