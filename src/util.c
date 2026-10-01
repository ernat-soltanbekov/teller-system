#include "header.h"
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

volatile sig_atomic_t teller_stop = 0;

bool app_error(App *app, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(app->error, sizeof(app->error), format, args);
    va_end(args);
    return false;
}

bool app_path(const App *app, const char *leaf, char out[PATH_CAP]) {
    int n = snprintf(out, PATH_CAP, "%s/%s", app->directory, leaf);
    return n >= 0 && n < PATH_CAP;
}

bool parse_id(const char *text, int *value) {
    if (!text || !*text)
        return false;
    unsigned int result = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < '0' || *p > '9' || result > ((unsigned)INT_MAX - (*p - '0')) / 10)
            return false;
        result = result * 10 + (*p - '0');
    }
    *value = (int)result;
    return true;
}

bool parse_money(const char *text, int64_t *cents) {
    if (!text || !*text)
        return false;
    uint64_t whole = 0, fraction = 0;
    const unsigned char *p = (const unsigned char *)text;
    if (*p < '0' || *p > '9')
        return false;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = *p++ - '0';
        if (whole > ((uint64_t)MONEY_MAX / 100 - digit) / 10)
            return false;
        whole = whole * 10 + digit;
    }
    if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9')
            return false;
        fraction = (*p++ - '0') * 10;
        if (*p >= '0' && *p <= '9')
            fraction += *p++ - '0';
    }
    uint64_t result = whole * 100 + fraction;
    if (*p || result > (uint64_t)MONEY_MAX)
        return false;
    *cents = (int64_t)result;
    return true;
}

void money_text(int64_t cents, char out[32]) {
    snprintf(out, 32, "%" PRId64 ".%02" PRId64, cents / 100, cents % 100);
}

static int month_days(int month, int year) {
    static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12)
        return 0;
    return days[month] + (month == 2 && (year % 400 == 0 || (year % 4 == 0 && year % 100 != 0)));
}

bool parse_date(const char *text, Date *date) {
    if (!text || strlen(text) < 8 || strlen(text) > 10)
        return false;
    for (const char *p = text; *p; p++)
        if ((*p < '0' || *p > '9') && *p != '/')
            return false;
    Date d;
    int end = 0;
    if (sscanf(text, "%2d/%2d/%4d%n", &d.day, &d.month, &d.year, &end) != 3 || text[end])
        return false;
    if (d.year < 1900 || d.year > 9996 || d.day < 1 || d.day > month_days(d.month, d.year))
        return false;
    *date = d;
    return true;
}

void date_text(Date date, char out[11]) {
    /* All public entry points validate dates before formatting. */
    out[0] = (char)('0' + date.day / 10);
    out[1] = (char)('0' + date.day % 10);
    out[2] = '/';
    out[3] = (char)('0' + date.month / 10);
    out[4] = (char)('0' + date.month % 10);
    out[5] = '/';
    out[6] = (char)('0' + date.year / 1000);
    out[7] = (char)('0' + date.year / 100 % 10);
    out[8] = (char)('0' + date.year / 10 % 10);
    out[9] = (char)('0' + date.year % 10);
    out[10] = '\0';
}

bool today_text(char out[11]) {
    time_t now = time(NULL);
    struct tm parts;
    if (now == (time_t)-1 || !localtime_r(&now, &parts))
        return false;
    Date date = {parts.tm_mday, parts.tm_mon + 1, parts.tm_year + 1900};
    if (date.year < 1900 || date.year > 9996)
        return false;
    date_text(date, out);
    return true;
}

bool valid_name(const char *text) {
    size_t n = text ? strlen(text) : 0;
    if (n < 1 || n >= NAME_CAP)
        return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '-'))
            return false;
    }
    return true;
}

bool valid_country(const char *text) {
    return valid_name(text);
}

bool valid_phone(const char *text) {
    if (!text)
        return false;
    const char *p = text + (text[0] == '+');
    size_t n = strlen(p);
    if (n < 3 || n > 20)
        return false;
    for (; *p; p++)
        if (*p < '0' || *p > '9')
            return false;
    return true;
}

bool valid_kind(const char *text) {
    return text && (!strcmp(text, "savings") || !strcmp(text, "current") || fixed_kind(text));
}

bool fixed_kind(const char *text) {
    return text &&
           (!strcmp(text, "fixed01") || !strcmp(text, "fixed02") || !strcmp(text, "fixed03"));
}

void interest_text(const Account *account, char *out, size_t size) {
    Date date;
    if (!parse_date(account->date, &date) || account->balance < 0 || account->balance > MONEY_MAX ||
        !valid_kind(account->kind)) {
        snprintf(out, size, "Invalid account data.");
        return;
    }
    if (!strcmp(account->kind, "current")) {
        snprintf(out, size, "You will not get interests because the account is of type current");
        return;
    }
    char amount[32];
    if (!strcmp(account->kind, "savings")) {
        money_text((account->balance * 7 + 600) / 1200, amount);
        snprintf(out, size, "You will get $%s as interest on day %d of every month", amount,
                 date.day);
        return;
    }
    int years = account->kind[6] - '0';
    int rate = years == 1 ? 4 : years == 2 ? 5 : 8;
    money_text((account->balance * rate * years + 50) / 100, amount);
    date.year += years;
    if (date.day > month_days(date.month, date.year))
        date.day = month_days(date.month, date.year);
    char maturity[11];
    date_text(date, maturity);
    snprintf(out, size, "You will get $%s as interest on %s", amount, maturity);
}
