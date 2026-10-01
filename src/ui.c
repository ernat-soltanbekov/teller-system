#include "header.h"
#include <errno.h>
#include <openssl/crypto.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

int ui_line(App *app, const User *user, int64_t *cursor, const char *prompt, char *out, size_t size,
            bool secret) {
    if (size < 2)
        return -1;
    struct termios original, hidden;
    bool changed = false;
    if (secret && isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &original) != 0) {
            app_error(app, "Cannot secure password input.");
            return -1;
        }
        hidden = original;
        hidden.c_lflag &= (tcflag_t)~ECHO;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &hidden) != 0) {
            app_error(app, "Cannot hide password input.");
            return -1;
        }
        changed = true;
    }
    fputs(prompt, stdout);
    fflush(stdout);
    size_t used = 0;
    bool invalid = false;
    bool notifications_enabled = user != NULL;
    int result = 0;
    while (!teller_stop && !ferror(stdout)) {
        fd_set input;
        FD_ZERO(&input);
        FD_SET(STDIN_FILENO, &input);
        struct timeval timeout = {0, 200000};
        int ready = select(STDIN_FILENO + 1, &input, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            app_error(app, "Cannot read terminal: %s", strerror(errno));
            result = -1;
            break;
        }
        if (!ready) {
            int notifications = notifications_enabled ? show_notifications(app, user, cursor) : 0;
            if (notifications < 0) {
                fprintf(stderr, "Notification check failed: %s\n", app->error);
                notifications_enabled = false;
            } else if (notifications > 0) {
                fputs(prompt, stdout);
                fflush(stdout);
            }
            continue;
        }
        unsigned char byte;
        ssize_t count = read(STDIN_FILENO, &byte, 1);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            app_error(app, "Cannot read input.");
            result = -1;
            break;
        }
        if (count == 0 || byte == '\n') {
            if (used && out[used - 1] == '\r')
                used--;
            result = invalid ? -1 : (count == 0 && used == 0 ? 0 : 1);
            break;
        }
        if (byte == 0 || (byte < 32 && byte != '\r' && byte != '\t') || byte == 127 ||
            used >= size - 1)
            invalid = true;
        else if (!invalid)
            out[used++] = (char)byte;
    }
    out[used] = '\0';
    if (changed) {
        if (tcsetattr(STDIN_FILENO, TCSANOW, &original) != 0) {
            app_error(app, "Cannot restore terminal settings.");
            result = -1;
        }
        putchar('\n');
        fflush(stdout);
    }
    if (invalid)
        app_error(app, "Input is too long or contains unsupported control characters.");
    return result;
}

static bool field(App *app, const User *user, int64_t *cursor, const char *prompt, char *out,
                  size_t size, bool secret) {
    return ui_line(app, user, cursor, prompt, out, size, secret) == 1;
}

static bool account_number(App *app, const User *user, int64_t *cursor, int *number) {
    char value[32];
    if (!field(app, user, cursor, "Account number: ", value, sizeof(value), false))
        return false;
    if (!parse_id(value, number))
        return app_error(app, "Account number must be an integer from 0 to 2147483647.");
    return true;
}

static bool create_screen(App *app, const User *user, int64_t *cursor) {
    Account account = {0};
    char date[32], amount[64];
    Date parsed;
    if (!field(app, user, cursor, "Creation date (dd/mm/yyyy): ", date, sizeof(date), false))
        return false;
    if (!parse_date(date, &parsed))
        return app_error(app, "Invalid calendar date (dd/mm/yyyy, year 1900-9996).");
    date_text(parsed, account.date);
    if (!account_number(app, user, cursor, &account.number) ||
        !field(app, user, cursor, "Country (use '_' for spaces): ", account.country,
               sizeof(account.country), false) ||
        !field(app, user, cursor, "Phone number: ", account.phone, sizeof(account.phone), false) ||
        !field(app, user, cursor, "Opening balance ($): ", amount, sizeof(amount), false))
        return false;
    if (!parse_money(amount, &account.balance))
        return app_error(app, "Use a nonnegative amount with at most two decimal places.");
    if (!field(app, user, cursor, "Type [savings/current/fixed01/fixed02/fixed03]: ", account.kind,
               sizeof(account.kind), false))
        return false;
    return account_create(app, user, &account);
}

static bool update_screen(App *app, const User *user, int64_t *cursor) {
    int number;
    Account account;
    char choice[8], value[NAME_CAP];
    if (!account_number(app, user, cursor, &number) ||
        !account_get(app, user->id, number, &account))
        return false;
    if (!field(app, user, cursor, "Update [1] phone number or [2] country: ", choice,
               sizeof(choice), false))
        return false;
    const char *name = !strcmp(choice, "1") ? "phone" : !strcmp(choice, "2") ? "country" : NULL;
    if (!name)
        return app_error(app, "Choose 1 for phone or 2 for country.");
    if (!field(app, user, cursor, "New value: ", value, sizeof(value), false))
        return false;
    return account_update(app, user, number, name, value);
}

static bool transaction_screen(App *app, const User *user, int64_t *cursor) {
    int number;
    Account account;
    char choice[8], amount_text[64];
    int64_t amount;
    if (!account_number(app, user, cursor, &number) ||
        !account_get(app, user->id, number, &account))
        return false;
    if (fixed_kind(account.kind))
        return app_error(app, "Cannot withdraw or deposit for fixed accounts.");
    if (!field(app, user, cursor, "Transaction [1] deposit or [2] withdrawal: ", choice,
               sizeof(choice), false))
        return false;
    const char *kind = !strcmp(choice, "1")   ? "deposit"
                       : !strcmp(choice, "2") ? "withdrawal"
                                              : NULL;
    if (!kind)
        return app_error(app, "Choose 1 for deposit or 2 for withdrawal.");
    if (!field(app, user, cursor, "Amount ($): ", amount_text, sizeof(amount_text), false))
        return false;
    if (!parse_money(amount_text, &amount) || amount <= 0)
        return app_error(app, "Amount must be positive, with at most two decimal places.");
    return account_transact(app, user, number, kind, amount);
}

static bool delete_screen(App *app, const User *user, int64_t *cursor) {
    int number;
    Account account;
    char answer[8];
    if (!account_number(app, user, cursor, &number) ||
        !account_get(app, user->id, number, &account))
        return false;
    if (!field(app, user, cursor, "Type YES to remove this account (history is archived): ", answer,
               sizeof(answer), false))
        return false;
    if (strcmp(answer, "YES"))
        return app_error(app, "Deletion cancelled; nothing changed.");
    return account_delete(app, user, number);
}

static bool transfer_screen(App *app, const User *user, int64_t *cursor) {
    int number;
    Account account;
    char recipient[NAME_CAP], answer[8];
    if (!account_number(app, user, cursor, &number) ||
        !account_get(app, user->id, number, &account))
        return false;
    if (!field(app, user, cursor, "Recipient username: ", recipient, sizeof(recipient), false) ||
        !field(app, user, cursor, "Type YES to transfer this account and its history: ", answer,
               sizeof(answer), false))
        return false;
    if (strcmp(answer, "YES"))
        return app_error(app, "Transfer cancelled; nothing changed.");
    return account_transfer(app, user, number, recipient);
}

int ui_run(App *app) {
    User user = {0};
    bool logged_in = false;
    int64_t cursor = 0;
    puts("+-----------------------------------------------------------+");
    puts("| TELLER SYSTEM | GhostOfAstana | Tomorrow School           |");
    puts("| Discipline in practice. Clarity in every operation.      |");
    puts("+-----------------------------------------------------------+");
    while (!teller_stop && !app->fatal && !ferror(stdout)) {
        app->error[0] = '\0';
        char choice[16];
        if (logged_in) {
            printf("\n%s | [1] Create  [2] Update  [3] Details  [4] List\n", user.name);
            puts("[5] Transaction  [6] Remove  [7] Transfer owner");
            puts("[8] Logout  [9] Notifications  [0] Exit");
        } else
            puts("\n[1] Login  [2] Register  [3] Exit");
        int input =
            ui_line(app, logged_in ? &user : NULL, &cursor, "> ", choice, sizeof(choice), false);
        if (!input)
            break;
        if (input < 0) {
            printf("Error: %s\n", app->error);
            continue;
        }
        bool ok = false;
        if (!logged_in) {
            if (!strcmp(choice, "3"))
                break;
            if (strcmp(choice, "1") && strcmp(choice, "2")) {
                puts("Error: choose 1, 2 or 3.");
                continue;
            }
            char name[NAME_CAP], password[129];
            if (!field(app, NULL, &cursor, "Username: ", name, sizeof(name), false)) {
                if (app->error[0])
                    printf("Error: %s\n", app->error);
                else
                    break;
                continue;
            }
            if (!field(app, NULL, &cursor, "Password: ", password, sizeof(password), true)) {
                OPENSSL_cleanse(password, sizeof(password));
                if (app->error[0])
                    printf("Error: %s\n", app->error);
                else
                    break;
                continue;
            }
            if (!strcmp(choice, "2")) {
                ok = register_user(app, name, password);
                if (ok)
                    puts("Registration saved. You can now log in.");
            } else {
                ok = login_user(app, name, password, &user);
                if (ok) {
                    logged_in = true;
                    cursor = 0;
                    printf("Welcome, %s.\n", user.name);
                }
            }
            OPENSSL_cleanse(password, sizeof(password));
        } else {
            if (!strcmp(choice, "0"))
                break;
            if (!strcmp(choice, "8")) {
                logged_in = false;
                continue;
            }
            if (!strcmp(choice, "1"))
                ok = create_screen(app, &user, &cursor);
            else if (!strcmp(choice, "2"))
                ok = update_screen(app, &user, &cursor);
            else if (!strcmp(choice, "3")) {
                int number;
                ok = account_number(app, &user, &cursor, &number) &&
                     account_details(app, &user, number);
            } else if (!strcmp(choice, "4"))
                ok = account_list(app, &user);
            else if (!strcmp(choice, "5"))
                ok = transaction_screen(app, &user, &cursor);
            else if (!strcmp(choice, "6"))
                ok = delete_screen(app, &user, &cursor);
            else if (!strcmp(choice, "7"))
                ok = transfer_screen(app, &user, &cursor);
            else if (!strcmp(choice, "9")) {
                int64_t all = 0;
                int notifications = show_notifications(app, &user, &all);
                if (notifications == 0)
                    puts("No notifications.");
                ok = notifications >= 0;
            } else
                app_error(app, "Unknown menu choice.");
            if (ok && (strchr("12567", choice[0]) && choice[1] == '\0'))
                puts("Saved successfully.");
        }
        if (!ok && app->error[0])
            printf("Error: %s\n", app->error);
    }
    if (app->fatal || ferror(stdout))
        return 1;
    if (teller_stop)
        return 128 + (int)teller_stop;
    puts("Session closed.");
    return 0;
}
