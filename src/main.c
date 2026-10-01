#include "header.h"
#include <string.h>
#include <sys/stat.h>

static void stop_session(int signal_number) {
    teller_stop = signal_number;
}

int main(int argc, char **argv) {
    const char *directory = "data";
    bool check = false, export = false, seen_directory = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) {
            puts("Usage: ./teller-system [--data-dir DIRECTORY] [--check | --export]\n"
                 "Default data directory: ./data\n"
                 "--check   Validate storage and recover text snapshots, then exit.\n"
                 "--export  Regenerate the three text snapshots from SQLite, then exit.\n"
                 "Dates: dd/mm/yyyy. Money: decimal dollars, at most two decimals.");
            return 0;
        }
        if (!strcmp(argv[i], "--data-dir") && !seen_directory && i + 1 < argc) {
            directory = argv[++i];
            seen_directory = true;
        } else if (!strcmp(argv[i], "--check") && !check && !export)
            check = true;
        else if (!strcmp(argv[i], "--export") && !check && !export)
            export = true;
        else {
            fprintf(stderr, "Invalid arguments. Use --help.\n");
            return 2;
        }
    }
    umask(0077);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_session;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) {
        perror("Cannot install signal handlers");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    App app;
    if (!store_open(&app, directory)) {
        fprintf(stderr, "Storage error: %s\n", app.error);
        store_close(&app);
        return 1;
    }
    int result = 0;
    if (check)
        puts("Storage OK. Text snapshots synchronized.");
    else if (export) {
        if (store_export(&app))
            puts("Text snapshots exported.");
        else {
            fprintf(stderr, "Export error: %s\n", app.error);
            result = 1;
        }
    } else
        result = ui_run(&app);
    store_close(&app);
    if (fflush(stdout) != 0 || ferror(stdout))
        result = 1;
    return result;
}
