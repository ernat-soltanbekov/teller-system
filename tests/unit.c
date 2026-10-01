#include "header.h"
#include "analyzer.h"
#include <dirent.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void remove_tree(const char *path) {
    struct stat info;
    if (lstat(path, &info) != 0)
        return;
    if (!S_ISDIR(info.st_mode)) {
        CHECK(unlink(path) == 0);
        return;
    }
    DIR *dir = opendir(path);
    CHECK(dir != NULL);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char child[PATH_CAP];
        CHECK(snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) < PATH_CAP);
        remove_tree(child);
    }
    closedir(dir);
    CHECK(rmdir(path) == 0);
}

static int64_t scalar(App *app, const char *sql) {
    sqlite3_stmt *statement = NULL;
    CHECK(store_prepare(app, &statement, sql));
    CHECK(sqlite3_step(statement) == SQLITE_ROW);
    int64_t value = sqlite3_column_int64(statement, 0);
    CHECK(sqlite3_finalize(statement) == SQLITE_OK);
    return value;
}

static void write_bytes(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fwrite(data, 1, size, file) == size);
    CHECK(fclose(file) == 0);
}

static void test_values(void) {
    struct {
        const char *text;
        int64_t want;
    } money[] = {
        {"0", 0}, {"0.01", 1}, {"1.2", 120}, {"1001.20", 100120}, {"100000000000.00", MONEY_MAX}};
    for (size_t i = 0; i < sizeof(money) / sizeof(money[0]); i++) {
        int64_t value = -1;
        CHECK(parse_money(money[i].text, &value));
        CHECK(value == money[i].want);
    }
    const char *invalid[] = {
        "",    "-1",       "+1",  ".1", "1.", "1.001",           "1e2",
        "NaN", "Infinity", "1 2", " 2", "2 ", "100000000000.01", "99999999999999999999999999"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        int64_t value = 777;
        CHECK(!parse_money(invalid[i], &value));
        CHECK(value == 777);
    }
    uint64_t state = 2008;
    for (int i = 0; i < 100000; i++) {
        state = state * UINT64_C(6364136223846793005) + 1;
        int64_t input = (int64_t)(state % (MONEY_MAX + 1)), output = -1;
        char text[32];
        money_text(input, text);
        CHECK(parse_money(text, &output) && input == output);
    }
    int id = -1;
    CHECK(parse_id("2147483647", &id) && id == INT_MAX);
    CHECK(parse_id("0", &id) && id == 0);
    CHECK(!parse_id("2147483648", &id));
    CHECK(!parse_id("-1", &id));
    CHECK(!parse_id("1.0", &id));
    Date date;
    CHECK(parse_date("29/02/2000", &date));
    CHECK(parse_date("1/1/2026", &date));
    CHECK(!parse_date("29/02/1900", &date));
    CHECK(!parse_date("31/04/2026", &date));
    CHECK(!parse_date("10/13/2026", &date));
    CHECK(!parse_date("0/1/2026", &date));
    CHECK(!parse_date("10/10/9999", &date));
    CHECK(!parse_date("1/1/2026x", &date));
    CHECK(valid_name("GhostOfAstana"));
    CHECK(!valid_name("Alice';DROP"));
    CHECK(!valid_name("Alice Bob"));
    CHECK(valid_phone("+77001234567"));
    CHECK(valid_phone("001234"));
    CHECK(!valid_phone("12"));
    CHECK(!valid_phone("123abc"));
}

static void test_interest(void) {
    Account account = {.balance = 100120};
    strcpy(account.date, "10/10/2012");
    const char *kinds[] = {"savings", "fixed01", "fixed02", "fixed03", "current"};
    const char *expected[] = {
        "$5.84 as interest on day 10 of every month", "$40.05 as interest on 10/10/2013",
        "$100.12 as interest on 10/10/2014", "$240.29 as interest on 10/10/2015",
        "You will not get interests because the account is of type current"};
    for (int i = 0; i < 5; i++) {
        char text[256];
        strcpy(account.kind, kinds[i]);
        interest_text(&account, text, sizeof(text));
        CHECK(strstr(text, expected[i]) != NULL);
    }
    account.balance = 102320;
    strcpy(account.kind, "savings");
    char text[256];
    interest_text(&account, text, sizeof(text));
    CHECK(strstr(text, "$5.97") != NULL);
    strcpy(account.kind, "fixed01");
    strcpy(account.date, "29/02/2024");
    interest_text(&account, text, sizeof(text));
    CHECK(strstr(text, "28/02/2025") != NULL);
}

static void test_ratios(void) {
    struct {
        int64_t deposits, withdrawals;
        int ratio;
        const char *label;
    } cases[] = {{596, 404, 60, "saver"},   {594, 406, 59, "moderate"},
                 {595, 405, 60, "saver"},   {395, 605, 40, "moderate"},
                 {394, 606, 39, "spender"}, {400, 600, 40, "moderate"},
                 {600, 400, 60, "saver"},   {1, 0, 100, "saver"},
                 {0, 1, 0, "spender"},      {320000, 80000, 80, "saver"}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int ratio = -1;
        CHECK(spending_ratio(cases[i].deposits, cases[i].withdrawals, &ratio));
        CHECK(ratio == cases[i].ratio);
        CHECK(!strcmp(spending_label(ratio), cases[i].label));
    }
    int ratio;
    CHECK(!spending_ratio(0, 0, &ratio));
    CHECK(!spending_ratio(-1, 1, &ratio));
    CHECK(!spending_ratio(LEDGER_MAX, 1, &ratio));
    CHECK(spending_ratio(LEDGER_MAX, 0, &ratio) && ratio == 100);
    CHECK(!strcmp(spending_message(60), "You tend to deposit more than you withdraw. Keep it up!"));
    CHECK(!strcmp(spending_message(40), "Your deposits and withdrawals are fairly balanced."));
    CHECK(
        !strcmp(spending_message(0), "You withdraw more than you deposit. Consider saving more."));
}

static void test_analyzer_file(const char *root) {
    char path[PATH_CAP];
    snprintf(path, sizeof(path), "%s/history.txt", root);
    const char *history =
        "7 deposit 5.96 01/10/2026\n99 deposit 900.00 01/10/2026\n7 withdrawal 4.04 01/10/2026\n";
    write_bytes(path, history, strlen(history));
    Spending result;
    char error[256];
    CHECK(spending_read(path, 7, &result, error));
    CHECK(result.deposits == 1 && result.withdrawals == 1 && result.deposited == 596 &&
          result.withdrawn == 404 && result.ratio == 60);
    CHECK(spending_read(path, 123, &result, error) && result.deposits == 0 &&
          result.withdrawals == 0);
    const char *bad[] = {"x deposit 1.00 01/10/2026\n", "7 magic 1.00 01/10/2026\n",
                         "7 deposit NaN 01/10/2026\n",  "7 deposit -1.00 01/10/2026\n",
                         "7 deposit 1.00 30/02/2026\n", "7 deposit 1.00 01/10/2026 trailing\n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        write_bytes(path, bad[i], strlen(bad[i]));
        CHECK(!spending_read(path, 7, &result, error));
        CHECK(*error);
    }
    const char binary[] = "7 deposit 1.00 01/10/2026\0hidden\n";
    write_bytes(path, binary, sizeof(binary) - 1);
    CHECK(!spending_read(path, 7, &result, error));
    CHECK(unlink(path) == 0);
    CHECK(!spending_read(path, 7, &result, error));
    CHECK(mkfifo(path, 0600) == 0);
    CHECK(!spending_read(path, 7, &result, error));
    CHECK(unlink(path) == 0);
}

static void test_passwords(void) {
    char a[HASH_CAP], b[HASH_CAP];
    CHECK(password_hash("q1w2e3r4t5y6", a));
    CHECK(password_hash("q1w2e3r4t5y6", b));
    CHECK(strcmp(a, b) != 0);
    CHECK(password_verify("q1w2e3r4t5y6", a));
    CHECK(!password_verify("wrong", a));
    CHECK(!strstr(a, "q1w2e3r4t5y6"));
    CHECK(password_encoding_valid(a));
    CHECK(!password_encoding_valid("plaintext"));
    CHECK(!password_encoding_valid("pbkdf2-sha256$9999999999$00$00"));
    CHECK(!password_verify("x", "pbkdf2-sha256$1$00$00"));
}

static void test_operations(const char *root) {
    char directory[PATH_CAP];
    snprintf(directory, sizeof(directory), "%s/database", root);
    App app;
    CHECK(store_open(&app, directory));
    CHECK(register_user(&app, "Alice", "q1w2e3r4t5y6"));
    CHECK(!register_user(&app, "alice", "another-password"));
    CHECK(register_user(&app, "Laura", "different-password"));
    CHECK(!register_user(&app, "Marcus", "short"));
    CHECK(!register_user(&app, "bad name", "long-password"));
    User alice, laura;
    CHECK(login_user(&app, "ALICE", "q1w2e3r4t5y6", &alice));
    CHECK(!strcmp(alice.name, "Alice"));
    CHECK(login_user(&app, "Laura", "different-password", &laura));
    CHECK(!login_user(&app, "Alice", "wrong", &laura));
    Account account = {.number = 834213, .balance = 100120};
    strcpy(account.date, "10/10/2012");
    strcpy(account.country, "UK");
    strcpy(account.phone, "291231392");
    strcpy(account.kind, "savings");
    CHECK(account_create(&app, &alice, &account));
    CHECK(!account_create(&app, &laura, &account));
    CHECK(account_update(&app, &alice, 834213, "phone", "+77001234567"));
    CHECK(account_update(&app, &alice, 834213, "country", "Kazakhstan"));
    CHECK(!account_update(&app, &laura, 834213, "country", "UK"));
    CHECK(!account_update(&app, &alice, 834213, "balance", "999"));
    CHECK(!account_update(&app, &alice, 999, "phone", "12345"));
    CHECK(account_get(&app, alice.id, 834213, &account));
    CHECK(!strcmp(account.phone, "+77001234567") && !strcmp(account.country, "Kazakhstan"));
    CHECK(account_transact(&app, &alice, 834213, "deposit", 50000));
    CHECK(account_transact(&app, &alice, 834213, "deposit", 20000));
    CHECK(account_transact(&app, &alice, 834213, "withdrawal", 10000));
    CHECK(account_get(&app, alice.id, 834213, &account) && account.balance == 160120);
    int64_t count = scalar(&app, "SELECT COUNT(*) FROM transactions");
    CHECK(count == 3);
    CHECK(!account_transact(&app, &alice, 834213, "withdrawal", 160121));
    CHECK(!account_transact(&app, &alice, 834213, "deposit", 0));
    CHECK(!account_transact(&app, &alice, 834213, "deposit", -1));
    CHECK(!account_transact(&app, &laura, 834213, "deposit", 100));
    CHECK(scalar(&app, "SELECT COUNT(*) FROM transactions") == count);
    const char *fixed_types[] = {"fixed01", "fixed02", "fixed03"};
    for (int i = 1; i <= 3; i++) {
        account.number = 3200 + i;
        strcpy(account.kind, fixed_types[i - 1]);
        CHECK(account_create(&app, &alice, &account));
        CHECK(!account_transact(&app, &alice, account.number, "deposit", 100));
        CHECK(!account_transact(&app, &alice, account.number, "withdrawal", 100));
    }
    CHECK(!account_transfer(&app, &alice, 834213, "missing"));
    CHECK(!account_transfer(&app, &alice, 834213, "Alice"));
    CHECK(!account_transfer(&app, &laura, 834213, "Alice"));
    CHECK(account_transfer(&app, &alice, 834213, "Laura"));
    CHECK(!account_get(&app, alice.id, 834213, &account));
    CHECK(account_get(&app, laura.id, 834213, &account));
    CHECK(account.balance == 160120);
    CHECK(scalar(&app, "SELECT COUNT(*) FROM notifications") == 1);
    char path[PATH_CAP], error[256];
    CHECK(app_path(&app, "transactions.txt", path));
    Spending spending;
    CHECK(spending_read(path, 834213, &spending, error) && spending.deposits == 2 &&
          spending.withdrawals == 1 && spending.ratio == 88);
    CHECK(!account_delete(&app, &alice, 834213));
    CHECK(account_delete(&app, &laura, 834213));
    CHECK(!account_delete(&app, &laura, 834213));
    CHECK(scalar(&app, "SELECT COUNT(*) FROM transactions") == 3);
    CHECK(scalar(&app, "SELECT active FROM accounts WHERE number=834213") == 0);
    CHECK(!account_create(&app, &alice, &account));
    CHECK(sqlite3_next_stmt(app.db, NULL) == NULL);
    store_close(&app);
    CHECK(store_open(&app, directory));
    CHECK(login_user(&app, "Alice", "q1w2e3r4t5y6", &alice));
    CHECK(scalar(&app, "SELECT COUNT(*) FROM transactions") == 3);
    CHECK(sqlite3_next_stmt(app.db, NULL) == NULL);
    store_close(&app);
}

int main(void) {
    umask(0077);
    char root[] = "/tmp/teller-unit-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    test_values();
    test_interest();
    test_ratios();
    test_analyzer_file(root);
    test_passwords();
    test_operations(root);
    remove_tree(root);
    puts("PASS: money/date parsing, interest, ratio boundaries, history, password hashing, "
         "ownership and persistence.");
    return 0;
}
