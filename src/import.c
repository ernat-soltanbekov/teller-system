#include "header.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The starter has blank lines, singular 'saving', and extra zero decimals.
 * Import those representations once; all exports use the canonical format. */
static bool legacy_money(char *text, int64_t *amount) {
    char *dot = strchr(text, '.');
    if (dot && strlen(dot + 1) > 2) {
        for (char *p = dot + 3; *p; p++)
            if (*p != '0')
                return false;
        dot[3] = '\0';
    }
    return parse_money(text, amount);
}

static int line_read(FILE *file, char line[512]) {
    size_t used = 0;
    int c;
    while ((c = fgetc(file)) != EOF && c != '\n') {
        if (!c || used >= 511)
            return -1;
        line[used++] = (char)c;
    }
    if (ferror(file))
        return -1;
    if (c == EOF && used == 0)
        return 0;
    if (used && line[used - 1] == '\r')
        used--;
    line[used] = '\0';
    return 1;
}

static bool trailing_space(const char *text) {
    for (; *text; text++)
        if (*text != ' ' && *text != '\t')
            return false;
    return true;
}

static bool import_user(App *app, char *line) {
    char id_text[32], name[NAME_CAP], password[HASH_CAP], hash[HASH_CAP];
    int end = 0, id;
    if (sscanf(line, "%31s %48s %159s%n", id_text, name, password, &end) != 3 ||
        !trailing_space(line + end) || !parse_id(id_text, &id) || !valid_name(name))
        return app_error(app, "Invalid user record.");
    bool encoded = !strncmp(password, "pbkdf2-", 7);
    if (encoded) {
        if (!password_encoding_valid(password))
            return app_error(app, "Invalid password hash in imported users.");
        snprintf(hash, sizeof(hash), "%s", password);
    } else if (!password_hash(password, hash))
        return app_error(app, "Cannot hash imported password.");
    OPENSSL_cleanse(password, sizeof(password));
    sqlite3_stmt *statement = NULL;
    if (!store_prepare(app, &statement, "INSERT INTO users(id,name,password) VALUES(?,?,?)")) {
        OPENSSL_cleanse(hash, sizeof(hash));
        return false;
    }
    sqlite3_bind_int(statement, 1, id);
    sqlite3_bind_text(statement, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, hash, -1, SQLITE_TRANSIENT);
    OPENSSL_cleanse(hash, sizeof(hash));
    return store_done(app, statement);
}

static bool import_account(App *app, char *line) {
    char rid[32], uid[32], name[NAME_CAP], number[32], date[16], country[NAME_CAP], phone[25],
        money[64], kind[10];
    int end = 0, record_id, user_id, account_id;
    int64_t amount;
    Date parsed;
    if (sscanf(line, "%31s %31s %48s %31s %15s %48s %24s %63s %9s%n", rid, uid, name, number, date,
               country, phone, money, kind, &end) != 9 ||
        !trailing_space(line + end))
        return app_error(app, "Invalid account record.");
    if (!strcmp(kind, "saving"))
        snprintf(kind, sizeof(kind), "savings");
    if (!parse_id(rid, &record_id) || !parse_id(uid, &user_id) || !parse_id(number, &account_id) ||
        !parse_date(date, &parsed) || !valid_country(country) || !valid_phone(phone) ||
        !legacy_money(money, &amount) || !valid_kind(kind))
        return app_error(app, "Invalid fields in account record.");
    sqlite3_stmt *statement = NULL;
    if (!store_prepare(app, &statement, "SELECT name FROM users WHERE id=?"))
        return false;
    sqlite3_bind_int(statement, 1, user_id);
    bool owner = sqlite3_step(statement) == SQLITE_ROW &&
                 !strcmp(name, (const char *)sqlite3_column_text(statement, 0));
    sqlite3_finalize(statement);
    if (!owner)
        return app_error(app, "Account owner ID and name do not match a user.");
    if (!store_prepare(app, &statement,
                       "INSERT INTO "
                       "accounts(record_id,number,owner,created,country,phone,balance,opening_"
                       "balance,kind) VALUES(?,?,?,?,?,?,?,?,?)"))
        return false;
    char canonical[11];
    date_text(parsed, canonical);
    sqlite3_bind_int(statement, 1, record_id);
    sqlite3_bind_int(statement, 2, account_id);
    sqlite3_bind_int(statement, 3, user_id);
    sqlite3_bind_text(statement, 4, canonical, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 5, country, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 6, phone, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 7, amount);
    sqlite3_bind_int64(statement, 8, amount);
    sqlite3_bind_text(statement, 9, kind, -1, SQLITE_TRANSIENT);
    return store_done(app, statement);
}

static bool import_transaction(App *app, char *line) {
    char number[32], kind[16], money[64], date[16];
    int end = 0, account_id;
    int64_t amount;
    Date parsed;
    if (sscanf(line, "%31s %15s %63s %15s%n", number, kind, money, date, &end) != 4 ||
        !trailing_space(line + end) || !parse_id(number, &account_id) ||
        !parse_date(date, &parsed) || !legacy_money(money, &amount) || amount <= 0 ||
        (strcmp(kind, "deposit") && strcmp(kind, "withdrawal")))
        return app_error(app, "Invalid transaction record.");
    sqlite3_stmt *statement = NULL;
    if (!store_prepare(app, &statement,
                       "INSERT INTO transactions(account_id,kind,amount,day) VALUES(?,?,?,?)"))
        return false;
    char canonical[11];
    date_text(parsed, canonical);
    sqlite3_bind_int(statement, 1, account_id);
    sqlite3_bind_text(statement, 2, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 3, amount);
    sqlite3_bind_text(statement, 4, canonical, -1, SQLITE_TRANSIENT);
    return store_done(app, statement);
}

static bool import_file(App *app, const char *name, bool (*consume)(App *, char *)) {
    char path[PATH_CAP];
    if (!app_path(app, name, path))
        return app_error(app, "Import path too long.");
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT)
            return true;
        return app_error(app, "Cannot read %s: %s", name, strerror(errno));
    }
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size > 16 * 1024 * 1024) {
        close(fd);
        return app_error(app, "%s must be a regular file of at most 16 MiB.", name);
    }
    FILE *file = fdopen(fd, "r");
    if (!file) {
        close(fd);
        return app_error(app, "Cannot open import stream.");
    }
    char line[512];
    int result;
    size_t number = 0;
    bool ok = true;
    while ((result = line_read(file, line)) > 0) {
        number++;
        if (trailing_space(line))
            continue;
        if (!consume(app, line)) {
            char reason[256];
            snprintf(reason, sizeof(reason), "%s", app->error);
            ok = app_error(app, "%s line %zu: %.160s", name, number, reason);
            break;
        }
    }
    OPENSSL_cleanse(line, sizeof(line));
    if (ok && result < 0)
        ok = app_error(app, "Unreadable, overlong or binary line in %s.", name);
    if (fclose(file) != 0 && ok)
        ok = app_error(app, "Cannot close imported file.");
    return ok;
}

bool store_import(App *app) {
    if (!import_file(app, "users.txt", import_user) ||
        !import_file(app, "records.txt", import_account) ||
        !import_file(app, "transactions.txt", import_transaction))
        return false;
    /* Imported records contain current balances. Derive opening balances once;
     * reject inconsistent history instead of silently inventing missing money. */
    return store_exec(
        app,
        "UPDATE accounts SET opening_balance=balance-COALESCE((SELECT SUM(CASE WHEN kind='deposit' "
        "THEN amount ELSE -amount END) FROM transactions WHERE account_id=accounts.number),0);");
}
