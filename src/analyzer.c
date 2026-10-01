#include "analyzer.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Only the CLI adapter has a context; calculation and file parsing are pure
 * entry points for independent tests. A process runs one terminal session. */
static App *active_app;
void analyzer_use(App *app) {
    active_app = app;
}

bool spending_ratio(int64_t deposited, int64_t withdrawn, int *ratio) {
    if (deposited < 0 || withdrawn < 0 || deposited > LEDGER_MAX ||
        withdrawn > LEDGER_MAX - deposited || deposited + withdrawn == 0)
        return false;
    int64_t total = deposited + withdrawn;
    /* Integer half-up rounding happens BEFORE classification. The ledger cap
     * makes both multiplication and addition provably fit in signed int64. */
    *ratio = (int)((deposited * 100 + total / 2) / total);
    return true;
}

const char *spending_label(int ratio) {
    return ratio >= 60 ? "saver" : ratio >= 40 ? "moderate" : "spender";
}
const char *spending_message(int ratio) {
    if (ratio >= 60)
        return "You tend to deposit more than you withdraw. Keep it up!";
    if (ratio >= 40)
        return "Your deposits and withdrawals are fairly balanced.";
    return "You withdraw more than you deposit. Consider saving more.";
}

static bool analysis_error(char error[256], const char *message) {
    snprintf(error, 256, "%s", message);
    return false;
}

bool spending_read(const char *path, int account_id, Spending *result, char error[256]) {
    *result = (Spending){0};
    error[0] = '\0';
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        snprintf(error, 256, "Cannot read transaction history: %s", strerror(errno));
        return false;
    }
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size > 16 * 1024 * 1024) {
        close(fd);
        return analysis_error(error, "History must be a regular file of at most 16 MiB.");
    }
    FILE *file = fdopen(fd, "r");
    if (!file) {
        close(fd);
        return analysis_error(error, "Cannot open transaction stream.");
    }
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    bool ok = true;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        if (length > 511 || memchr(line, '\0', (size_t)length)) {
            ok = analysis_error(error, "Invalid transaction line.");
            break;
        }
        if (length && line[length - 1] == '\n')
            line[--length] = '\0';
        if (length && line[length - 1] == '\r')
            line[--length] = '\0';
        if (length == 0)
            continue;
        char id_text[32], kind[16], amount_text[64], date[16];
        int end = 0, id;
        int64_t amount;
        Date parsed;
        if (sscanf(line, "%31s %15s %63s %15s%n", id_text, kind, amount_text, date, &end) != 4 ||
            line[end] || !parse_id(id_text, &id) || !parse_money(amount_text, &amount) ||
            amount <= 0 || !parse_date(date, &parsed) ||
            (strcmp(kind, "deposit") && strcmp(kind, "withdrawal"))) {
            ok = analysis_error(error,
                                "Malformed transaction history; no classification was produced.");
            break;
        }
        if (id != account_id)
            continue;
        if (amount > LEDGER_MAX - result->deposited - result->withdrawn) {
            ok = analysis_error(error, "Transaction totals exceed the supported limit.");
            break;
        }
        if (!strcmp(kind, "deposit")) {
            result->deposited += amount;
            result->deposits++;
        } else {
            result->withdrawn += amount;
            result->withdrawals++;
        }
    }
    if (ferror(file) || (!feof(file) && ok))
        ok = analysis_error(error, "Error while reading transaction history.");
    free(line);
    if (fclose(file) != 0)
        ok = analysis_error(error, "Cannot close transaction history.");
    if (ok && result->deposits + result->withdrawals)
        ok = spending_ratio(result->deposited, result->withdrawn, &result->ratio);
    return ok;
}

bool analyze_spending(int account_id) {
    if (!active_app)
        return false;
    char path[PATH_CAP];
    Spending spending;
    if (!app_path(active_app, "transactions.txt", path))
        return app_error(active_app, "History path too long.");
    if (!spending_read(path, account_id, &spending, active_app->error))
        return false;
    if (!spending.deposits && !spending.withdrawals) {
        puts("No transaction history available.");
        return !ferror(stdout);
    }
    char deposited[32], withdrawn[32];
    money_text(spending.deposited, deposited);
    money_text(spending.withdrawn, withdrawn);
    printf("Spending pattern: %s\n%s\n\n", spending_label(spending.ratio),
           spending_message(spending.ratio));
    printf("Transaction breakdown: %" PRIu64 " deposits ($%s) | %" PRIu64
           " withdrawals ($%s) | ratio: %d%%\n",
           spending.deposits, deposited, spending.withdrawals, withdrawn, spending.ratio);
    if (ferror(stdout))
        return app_error(active_app, "Cannot write analysis output.");
    return true;
}
