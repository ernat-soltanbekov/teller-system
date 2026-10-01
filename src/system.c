#include "header.h"
#include "analyzer.h"
#include <string.h>

bool account_get(App *app, int owner, int number, Account *account) {
    sqlite3_stmt *statement = NULL;
    if (!store_prepare(
            app, &statement,
            "SELECT a.number,a.owner,u.name,a.created,a.country,a.phone,a.balance,a.kind FROM "
            "accounts a JOIN users u ON u.id=a.owner WHERE a.number=? AND a.owner=? AND "
            "a.active=1"))
        return false;
    sqlite3_bind_int(statement, 1, number);
    sqlite3_bind_int(statement, 2, owner);
    int result = sqlite3_step(statement);
    if (result == SQLITE_ROW) {
        *account = (Account){0};
        account->number = sqlite3_column_int(statement, 0);
        account->owner = sqlite3_column_int(statement, 1);
        snprintf(account->owner_name, sizeof(account->owner_name), "%s",
                 sqlite3_column_text(statement, 2));
        snprintf(account->date, sizeof(account->date), "%s", sqlite3_column_text(statement, 3));
        snprintf(account->country, sizeof(account->country), "%s",
                 sqlite3_column_text(statement, 4));
        snprintf(account->phone, sizeof(account->phone), "%s", sqlite3_column_text(statement, 5));
        account->balance = sqlite3_column_int64(statement, 6);
        snprintf(account->kind, sizeof(account->kind), "%s", sqlite3_column_text(statement, 7));
    }
    sqlite3_finalize(statement);
    if (result == SQLITE_DONE) {
        app_error(app, "Account does not exist for this user.");
        return false;
    }
    if (result != SQLITE_ROW) {
        app_error(app, "Cannot read account: %s", sqlite3_errmsg(app->db));
        return false;
    }
    return true;
}

bool account_create(App *app, const User *user, const Account *account) {
    Date parsed;
    if (account->number < 0 || !parse_date(account->date, &parsed) ||
        !valid_country(account->country) || !valid_phone(account->phone) ||
        !valid_kind(account->kind) || account->balance < 0 || account->balance > MONEY_MAX)
        return app_error(app, "Invalid account fields; nothing was saved.");
    if (!store_begin(app))
        return false;
    sqlite3_stmt *statement = NULL;
    if (!store_prepare(
            app, &statement,
            "INSERT INTO accounts(number,owner,created,country,phone,balance,opening_balance,kind) "
            "VALUES(?,?,?,?,?,?,?,?)")) {
        store_abort(app);
        return false;
    }
    char date[11];
    date_text(parsed, date);
    sqlite3_bind_int(statement, 1, account->number);
    sqlite3_bind_int(statement, 2, user->id);
    sqlite3_bind_text(statement, 3, date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, account->country, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 5, account->phone, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 6, account->balance);
    sqlite3_bind_int64(statement, 7, account->balance);
    sqlite3_bind_text(statement, 8, account->kind, -1, SQLITE_TRANSIENT);
    int result = sqlite3_step(statement);
    if (result != SQLITE_DONE)
        app_error(app,
                  result == SQLITE_CONSTRAINT
                      ? "Account number already exists or is archived; choose a unique number."
                      : "Cannot create account: %s",
                  sqlite3_errmsg(app->db));
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        store_abort(app);
        return false;
    }
    return store_finish(app);
}

bool account_update(App *app, const User *user, int number, const char *field, const char *value) {
    bool phone = !strcmp(field, "phone"), country = !strcmp(field, "country");
    if ((!phone && !country) || (phone && !valid_phone(value)) ||
        (country && !valid_country(value)))
        return app_error(app, "Only a valid phone number or country may be updated.");
    if (!store_begin(app))
        return false;
    Account account;
    if (!account_get(app, user->id, number, &account)) {
        store_abort(app);
        return false;
    }
    sqlite3_stmt *statement = NULL;
    const char *sql = phone
                          ? "UPDATE accounts SET phone=? WHERE number=? AND owner=? AND active=1"
                          : "UPDATE accounts SET country=? WHERE number=? AND owner=? AND active=1";
    if (!store_prepare(app, &statement, sql)) {
        store_abort(app);
        return false;
    }
    sqlite3_bind_text(statement, 1, value, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, number);
    sqlite3_bind_int(statement, 3, user->id);
    if (!store_done(app, statement)) {
        store_abort(app);
        return false;
    }
    return store_finish(app);
}

bool account_transact(App *app, const User *user, int number, const char *kind, int64_t amount) {
    bool deposit = !strcmp(kind, "deposit"), withdrawal = !strcmp(kind, "withdrawal");
    if ((!deposit && !withdrawal) || amount <= 0 || amount > MONEY_MAX)
        return app_error(app,
                         "Transaction amount must be positive and within the supported limit.");
    char date[11];
    if (!today_text(date))
        return app_error(app, "Cannot read today's date; nothing was saved.");
    if (!store_begin(app))
        return false;
    Account account;
    if (!account_get(app, user->id, number, &account)) {
        store_abort(app);
        return false;
    }
    if (fixed_kind(account.kind)) {
        store_abort(app);
        return app_error(app, "Cannot withdraw or deposit for fixed accounts.");
    }
    if (withdrawal && amount > account.balance) {
        store_abort(app);
        return app_error(app, "Insufficient funds: withdrawal exceeds the available balance.");
    }
    if (deposit && amount > MONEY_MAX - account.balance) {
        store_abort(app);
        return app_error(app, "Deposit would exceed the supported balance limit.");
    }
    sqlite3_stmt *statement = NULL;
    if (!store_prepare(app, &statement,
                       "SELECT COALESCE(SUM(amount),0) FROM transactions WHERE account_id=?")) {
        store_abort(app);
        return false;
    }
    sqlite3_bind_int(statement, 1, number);
    int result = sqlite3_step(statement);
    int64_t total = result == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : LEDGER_MAX;
    sqlite3_finalize(statement);
    if (result != SQLITE_ROW || total < 0 || amount > LEDGER_MAX - total) {
        store_abort(app);
        return app_error(app,
                         "Transaction history total is invalid or exceeds the supported limit.");
    }
    if (!store_prepare(app, &statement, "UPDATE accounts SET balance=? WHERE number=?")) {
        store_abort(app);
        return false;
    }
    sqlite3_bind_int64(statement, 1, deposit ? account.balance + amount : account.balance - amount);
    sqlite3_bind_int(statement, 2, number);
    if (!store_done(app, statement)) {
        store_abort(app);
        return false;
    }
    store_test_crash("after-balance");
    if (!store_prepare(app, &statement,
                       "INSERT INTO transactions(account_id,kind,amount,day) VALUES(?,?,?,?)")) {
        store_abort(app);
        return false;
    }
    sqlite3_bind_int(statement, 1, number);
    sqlite3_bind_text(statement, 2, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 3, amount);
    sqlite3_bind_text(statement, 4, date, -1, SQLITE_TRANSIENT);
    if (!store_done(app, statement)) {
        store_abort(app);
        return false;
    }
    return store_finish(app);
}

bool account_delete(App *app, const User *user, int number) {
    if (!store_begin(app))
        return false;
    Account account;
    if (!account_get(app, user->id, number, &account)) {
        store_abort(app);
        return false;
    }
    sqlite3_stmt *statement = NULL;
    if (!store_prepare(app, &statement, "UPDATE accounts SET active=0 WHERE number=?")) {
        store_abort(app);
        return false;
    }
    sqlite3_bind_int(statement, 1, number);
    if (!store_done(app, statement)) {
        store_abort(app);
        return false;
    }
    return store_finish(app);
}

bool account_transfer(App *app, const User *user, int number, const char *recipient) {
    if (!valid_name(recipient))
        return app_error(app, "Invalid recipient name.");
    if (!store_begin(app))
        return false;
    Account account;
    if (!account_get(app, user->id, number, &account)) {
        store_abort(app);
        return false;
    }
    sqlite3_stmt *statement = NULL;
    if (!store_prepare(app, &statement, "SELECT id FROM users WHERE name=? COLLATE NOCASE")) {
        store_abort(app);
        return false;
    }
    sqlite3_bind_text(statement, 1, recipient, -1, SQLITE_TRANSIENT);
    int result = sqlite3_step(statement);
    int target = result == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
    sqlite3_finalize(statement);
    if (result != SQLITE_ROW && result != SQLITE_DONE) {
        store_abort(app);
        return app_error(app, "Cannot read recipient: %s", sqlite3_errmsg(app->db));
    }
    if (target < 0 || target == user->id) {
        store_abort(app);
        return app_error(app, "Recipient must be another existing user.");
    }
    if (!store_prepare(app, &statement, "UPDATE accounts SET owner=? WHERE number=?")) {
        store_abort(app);
        return false;
    }
    sqlite3_bind_int(statement, 1, target);
    sqlite3_bind_int(statement, 2, number);
    if (!store_done(app, statement)) {
        store_abort(app);
        return false;
    }
    if (!store_prepare(app, &statement,
                       "INSERT INTO notifications(recipient,account_id,sender) VALUES(?,?,?)")) {
        store_abort(app);
        return false;
    }
    sqlite3_bind_int(statement, 1, target);
    sqlite3_bind_int(statement, 2, number);
    sqlite3_bind_text(statement, 3, account.owner_name, -1, SQLITE_TRANSIENT);
    if (!store_done(app, statement)) {
        store_abort(app);
        return false;
    }
    return store_finish(app);
}

bool account_details(App *app, const User *user, int number) {
    if (!store_lock(app))
        return false;
    Account account;
    if (!store_ensure_views(app) || !account_get(app, user->id, number, &account)) {
        store_unlock(app);
        return false;
    }
    char amount[32], interest[200];
    money_text(account.balance, amount);
    interest_text(&account, interest, sizeof(interest));
    printf(
        "\nACCOUNT %d | %s\nOwner: %s\nCreated: %s\nCountry: %s\nPhone: %s\nBalance: $%s\n%s\n\n",
        account.number, account.kind, account.owner_name, account.date, account.country,
        account.phone, amount, interest);
    analyzer_use(app);
    bool ok = analyze_spending(number);
    store_unlock(app);
    return ok;
}

bool account_list(App *app, const User *user) {
    sqlite3_stmt *statement = NULL;
    if (!store_prepare(app, &statement,
                       "SELECT number,kind,country,balance FROM accounts WHERE owner=? AND "
                       "active=1 ORDER BY number"))
        return false;
    sqlite3_bind_int(statement, 1, user->id);
    puts("\nACCOUNT      TYPE       COUNTRY                  BALANCE");
    puts("-----------------------------------------------------------");
    int result, count = 0;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        char amount[32];
        money_text(sqlite3_column_int64(statement, 3), amount);
        printf("%-12d %-10s %-24s $%s\n", sqlite3_column_int(statement, 0),
               sqlite3_column_text(statement, 1), sqlite3_column_text(statement, 2), amount);
        count++;
    }
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE)
        return app_error(app, "Cannot list accounts: %s", sqlite3_errmsg(app->db));
    if (!count)
        puts("No accounts yet. Choose 1 to create one.");
    if (ferror(stdout))
        return app_error(app, "Cannot write account list.");
    return true;
}

int show_notifications(App *app, const User *user, int64_t *cursor) {
    sqlite3_stmt *statement = NULL;
    if (!store_prepare(app, &statement,
                       "SELECT id,account_id,sender,created FROM notifications WHERE recipient=? "
                       "AND id>? ORDER BY id"))
        return -1;
    sqlite3_bind_int(statement, 1, user->id);
    sqlite3_bind_int64(statement, 2, *cursor);
    int result;
    bool displayed = false;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        printf("\n[Notification] Account %d transferred to you by %s. (%s)\n",
               sqlite3_column_int(statement, 1), sqlite3_column_text(statement, 2),
               sqlite3_column_text(statement, 3));
        *cursor = sqlite3_column_int64(statement, 0);
        displayed = true;
    }
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        app_error(app, "Cannot read notifications: %s", sqlite3_errmsg(app->db));
        return -1;
    }
    if (displayed && (fflush(stdout) != 0 || ferror(stdout))) {
        app_error(app, "Cannot display notifications.");
        return -1;
    }
    return displayed;
}
