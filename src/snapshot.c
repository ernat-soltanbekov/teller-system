#include "header.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const files[] = {"users.txt", "records.txt", "transactions.txt", "version"};

static bool join_path(const char *base, const char *leaf, char out[PATH_CAP]) {
    int n = snprintf(out, PATH_CAP, "%s/%s", base, leaf);
    return n >= 0 && n < PATH_CAP;
}

static bool sync_directory(App *app, const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return app_error(app, "Cannot open snapshot directory: %s", strerror(errno));
    int result = fsync(fd), saved = errno;
    close(fd);
    if (result != 0)
        return app_error(app, "Cannot sync snapshot directory: %s", strerror(saved));
    return true;
}

static bool revision(App *app, int64_t *value) {
    sqlite3_stmt *statement = NULL;
    if (!store_prepare(app, &statement, "SELECT revision FROM metadata"))
        return false;
    int result = sqlite3_step(statement);
    if (result == SQLITE_ROW)
        *value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    if (result != SQLITE_ROW) {
        app_error(app, "Missing database revision.");
        return false;
    }
    return true;
}

static bool write_table(App *app, FILE *out, unsigned table) {
    static const char *const queries[] = {
        "SELECT id,name,password FROM users ORDER BY id",
        "SELECT a.record_id,a.owner,u.name,a.number,a.created,a.country,a.phone,a.balance,a.kind "
        "FROM accounts a JOIN users u ON u.id=a.owner WHERE a.active=1 ORDER BY a.record_id",
        "SELECT t.account_id,t.kind,t.amount,t.day FROM transactions t JOIN accounts a ON "
        "a.number=t.account_id WHERE a.active=1 ORDER BY t.id"};
    sqlite3_stmt *statement = NULL;
    if (!store_prepare(app, &statement, queries[table]))
        return false;
    int result;
    bool ok = true;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        int printed;
        char amount[32];
        if (table == 0) {
            printed = fprintf(out, "%d %s %s\n", sqlite3_column_int(statement, 0),
                              sqlite3_column_text(statement, 1), sqlite3_column_text(statement, 2));
        } else if (table == 1) {
            money_text(sqlite3_column_int64(statement, 7), amount);
            printed =
                fprintf(out, "%lld %d %s %d %s %s %s %s %s\n", sqlite3_column_int64(statement, 0),
                        sqlite3_column_int(statement, 1), sqlite3_column_text(statement, 2),
                        sqlite3_column_int(statement, 3), sqlite3_column_text(statement, 4),
                        sqlite3_column_text(statement, 5), sqlite3_column_text(statement, 6),
                        amount, sqlite3_column_text(statement, 8));
        } else {
            money_text(sqlite3_column_int64(statement, 2), amount);
            printed = fprintf(out, "%d %s %s %s\n", sqlite3_column_int(statement, 0),
                              sqlite3_column_text(statement, 1), amount,
                              sqlite3_column_text(statement, 3));
        }
        if (printed < 0) {
            ok = app_error(app, "Cannot write text snapshot: %s", strerror(errno));
            break;
        }
    }
    if (ok && result != SQLITE_DONE)
        ok = app_error(app, "Cannot read snapshot data: %s", sqlite3_errmsg(app->db));
    sqlite3_finalize(statement);
    return ok;
}

bool store_snapshot(App *app, char path[PATH_CAP]) {
    char root[PATH_CAP];
    int64_t version;
    if (!revision(app, &version) || !app_path(app, ".snapshots", root))
        return false;
    if (mkdir(root, 0700) != 0 && errno != EEXIST)
        return app_error(app, "Cannot create snapshot directory: %s", strerror(errno));
    struct stat info;
    if (lstat(root, &info) != 0 || !S_ISDIR(info.st_mode))
        return app_error(app, "Snapshot path must be a real directory.");
    if (!join_path(root, "v-XXXXXX", path) || !mkdtemp(path))
        return app_error(app, "Cannot stage text snapshot: %s", strerror(errno));
    for (unsigned i = 0; i < 4; i++) {
        char file_path[PATH_CAP];
        if (!join_path(path, files[i], file_path))
            return app_error(app, "Snapshot path too long.");
        int fd = open(file_path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        if (fd < 0)
            return app_error(app, "Cannot create text snapshot: %s", strerror(errno));
        FILE *out = fdopen(fd, "w");
        if (!out) {
            close(fd);
            return app_error(app, "Cannot open snapshot stream.");
        }
        bool ok = i < 3 ? write_table(app, out, i) : fprintf(out, "%" PRId64 "\n", version) > 0;
        if (!ok && i == 3)
            app_error(app, "Cannot write snapshot revision: %s", strerror(errno));
        if (ok && (fflush(out) != 0 || fsync(fileno(out)) != 0))
            ok = app_error(app, "Cannot flush text snapshot: %s", strerror(errno));
        if (fclose(out) != 0)
            ok = app_error(app, "Cannot close text snapshot: %s", strerror(errno));
        if (!ok)
            return false;
    }
    return sync_directory(app, path) && sync_directory(app, root);
}

static bool replace_link(App *app, const char *name, const char *target) {
    char path[PATH_CAP], temporary[PATH_CAP], leaf[100];
    if (!app_path(app, name, path))
        return app_error(app, "Path too long.");
    snprintf(leaf, sizeof(leaf), ".link-%s-%ld", name, (long)getpid());
    if (!app_path(app, leaf, temporary))
        return app_error(app, "Path too long.");
    (void)unlink(temporary);
    if (symlink(target, temporary) != 0)
        return app_error(app, "Cannot create text link: %s", strerror(errno));
    if (rename(temporary, path) != 0) {
        int saved = errno;
        unlink(temporary);
        return app_error(app, "Cannot publish text link: %s", strerror(saved));
    }
    return true;
}

static bool aliases(App *app) {
    for (unsigned i = 0; i < 3; i++) {
        char target[80], path[PATH_CAP], existing[100];
        snprintf(target, sizeof(target), ".current/%s", files[i]);
        if (!app_path(app, files[i], path))
            return app_error(app, "Path too long.");
        ssize_t n = readlink(path, existing, sizeof(existing) - 1);
        if (n >= 0) {
            existing[n] = '\0';
            if (!strcmp(existing, target))
                continue;
        }
        if (!replace_link(app, files[i], target))
            return false;
    }
    return sync_directory(app, app->directory);
}

/* Only our known, private snapshot files are removed; never follow directories
 * supplied as symlinks. The database keeps the actual archived account history. */
static void collect_old_snapshots(App *app, const char *keep) {
    char root[PATH_CAP];
    if (!app_path(app, ".snapshots", root))
        return;
    DIR *directory = opendir(root);
    if (!directory)
        return;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (strncmp(entry->d_name, "v-", 2))
            continue;
        char path[PATH_CAP];
        struct stat info;
        if (!join_path(root, entry->d_name, path) || !strcmp(path, keep) ||
            lstat(path, &info) != 0 || !S_ISDIR(info.st_mode))
            continue;
        for (unsigned i = 0; i < 4; i++) {
            char leaf[PATH_CAP];
            if (join_path(path, files[i], leaf))
                (void)unlink(leaf);
        }
        (void)rmdir(path);
    }
    closedir(directory);
}

bool store_publish(App *app, const char *path) {
    const char *name = strrchr(path, '/');
    if (!name)
        return app_error(app, "Invalid snapshot path.");
    char relative[100];
    snprintf(relative, sizeof(relative), ".snapshots/%s", name + 1);
    if (!replace_link(app, ".current", relative) || !aliases(app))
        return false;
    collect_old_snapshots(app, path);
    return true;
}

bool store_ensure_views(App *app) {
    int64_t current = -1, wanted;
    if (!revision(app, &wanted))
        return false;
    char path[PATH_CAP];
    if (!app_path(app, ".current/version", path))
        return app_error(app, "Path too long.");
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        struct stat info;
        char text[80] = {0};
        if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 &&
            info.st_size < 79) {
            ssize_t length = read(fd, text, sizeof(text) - 1);
            if (length == info.st_size) {
                char *end = NULL;
                errno = 0;
                int64_t parsed = strtoll(text, &end, 10);
                if (!errno && parsed >= 0 && end && *end == '\n' && end[1] == '\0')
                    current = parsed;
            }
        }
        close(fd);
    }
    if (current == wanted) {
        bool complete = true;
        for (unsigned i = 0; i < 3; i++) {
            char leaf[80];
            struct stat info;
            snprintf(leaf, sizeof(leaf), ".current/%s", files[i]);
            if (!app_path(app, leaf, path) || stat(path, &info) != 0 || !S_ISREG(info.st_mode))
                complete = false;
        }
        if (complete)
            return aliases(app);
    }
    return store_snapshot(app, path) && store_publish(app, path);
}

bool store_export(App *app) {
    if (!store_lock(app))
        return false;
    char path[PATH_CAP];
    bool ok = store_snapshot(app, path) && store_publish(app, path);
    store_unlock(app);
    return ok;
}
