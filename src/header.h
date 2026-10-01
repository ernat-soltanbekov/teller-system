#ifndef TELLER_HEADER_H
#define TELLER_HEADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <sqlite3.h>

#define NAME_CAP 49
#define HASH_CAP 160
#define PATH_CAP 4096
#define MONEY_MAX INT64_C(10000000000000)
#define LEDGER_MAX (INT64_MAX / 101)

typedef struct {
    sqlite3 *db;
    char directory[PATH_CAP];
    char error[256];
    int lock_fd;
    bool locked;
    bool fatal;
} App;

typedef struct {
    int id;
    char name[NAME_CAP];
} User;
typedef struct {
    int day, month, year;
} Date;
typedef struct {
    int number, owner;
    char owner_name[NAME_CAP], country[NAME_CAP], phone[25], kind[9];
    char date[11];
    int64_t balance;
} Account;

extern volatile sig_atomic_t teller_stop;

/* Parsing never changes the output argument on failure. Money is in cents. */
bool parse_id(const char *text, int *value);
bool parse_money(const char *text, int64_t *cents);
void money_text(int64_t cents, char out[32]);
bool parse_date(const char *text, Date *date);
void date_text(Date date, char out[11]);
bool today_text(char out[11]);
bool valid_name(const char *text);
bool valid_country(const char *text);
bool valid_phone(const char *text);
bool valid_kind(const char *text);
bool fixed_kind(const char *text);
void interest_text(const Account *account, char *out, size_t size);
bool app_error(App *app, const char *format, ...);
bool app_path(const App *app, const char *leaf, char out[PATH_CAP]);

/* Storage owns its connection; every successful begin needs finish or abort. */
bool store_open(App *app, const char *directory);
void store_close(App *app);
bool store_lock(App *app);
void store_unlock(App *app);
bool store_begin(App *app);
bool store_finish(App *app);
void store_abort(App *app);
bool store_exec(App *app, const char *sql);
bool store_prepare(App *app, sqlite3_stmt **statement, const char *sql);
bool store_done(App *app, sqlite3_stmt *statement);
bool store_export(App *app);
bool store_import(App *app);
bool store_snapshot(App *app, char path[PATH_CAP]);
bool store_publish(App *app, const char *path);
bool store_ensure_views(App *app);
void store_test_crash(const char *point);

bool password_hash(const char *password, char out[HASH_CAP]);
bool password_verify(const char *password, const char *encoded);
bool password_encoding_valid(const char *encoded);
bool register_user(App *app, const char *name, const char *password);
bool login_user(App *app, const char *name, const char *password, User *user);

bool account_get(App *app, int owner, int number, Account *account);
bool account_create(App *app, const User *user, const Account *account);
bool account_update(App *app, const User *user, int number, const char *field, const char *value);
bool account_transact(App *app, const User *user, int number, const char *kind, int64_t amount);
bool account_delete(App *app, const User *user, int number);
bool account_transfer(App *app, const User *user, int number, const char *recipient);
bool account_details(App *app, const User *user, int number);
bool account_list(App *app, const User *user);
/* -1: read/output error, 0: no new events, 1: events displayed. */
int show_notifications(App *app, const User *user, int64_t *cursor);

/* 1: complete line, 0: EOF/signal, -1: invalid/overlong line. */
int ui_line(App *app, const User *user, int64_t *cursor, const char *prompt, char *out, size_t size,
            bool secret);
int ui_run(App *app);

#endif
