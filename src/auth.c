#include "header.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <string.h>

#define HASH_ROUNDS 600000
#define SALT_BYTES 16
#define KEY_BYTES 32

static void hex_encode(const unsigned char *input, size_t size, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; i++) {
        out[i * 2] = digits[input[i] >> 4];
        out[i * 2 + 1] = digits[input[i] & 15];
    }
    out[size * 2] = '\0';
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static bool hex_decode(const char *input, unsigned char *out, size_t size) {
    if (strlen(input) != size * 2)
        return false;
    for (size_t i = 0; i < size; i++) {
        int a = hex_digit(input[i * 2]), b = hex_digit(input[i * 2 + 1]);
        if (a < 0 || b < 0)
            return false;
        out[i] = (unsigned char)(a * 16 + b);
    }
    return true;
}

static bool decode_hash(const char *encoded, int *rounds, unsigned char salt[16],
                        unsigned char key[32]) {
    if (!encoded || strlen(encoded) >= HASH_CAP)
        return false;
    char count[12], salt_hex[33], key_hex[65];
    int end = 0;
    if (sscanf(encoded, "pbkdf2-sha256$%11[^$]$%32[^$]$%64s%n", count, salt_hex, key_hex, &end) !=
            3 ||
        encoded[end])
        return false;
    if (!parse_id(count, rounds) || *rounds < HASH_ROUNDS || *rounds > 2000000)
        return false;
    return hex_decode(salt_hex, salt, 16) && hex_decode(key_hex, key, 32);
}

bool password_encoding_valid(const char *encoded) {
    unsigned char salt[16], key[32];
    int rounds;
    bool valid = decode_hash(encoded, &rounds, salt, key);
    OPENSSL_cleanse(key, sizeof(key));
    return valid;
}

bool password_hash(const char *password, char out[HASH_CAP]) {
    if (!password || strlen(password) > 128 || !*password)
        return false;
    unsigned char salt[SALT_BYTES], key[KEY_BYTES];
    if (RAND_bytes(salt, sizeof(salt)) != 1)
        return false;
    if (PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, sizeof(salt), HASH_ROUNDS,
                          EVP_sha256(), sizeof(key), key) != 1)
        return false;
    char salt_hex[33], key_hex[65];
    hex_encode(salt, sizeof(salt), salt_hex);
    hex_encode(key, sizeof(key), key_hex);
    snprintf(out, HASH_CAP, "pbkdf2-sha256$%d$%s$%s", HASH_ROUNDS, salt_hex, key_hex);
    OPENSSL_cleanse(key, sizeof(key));
    OPENSSL_cleanse(key_hex, sizeof(key_hex));
    return true;
}

bool password_verify(const char *password, const char *encoded) {
    unsigned char salt[16], expected[32], actual[32];
    int rounds;
    if (!password || strlen(password) > 128 || !decode_hash(encoded, &rounds, salt, expected))
        return false;
    bool ok = PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, sizeof(salt), rounds,
                                EVP_sha256(), sizeof(actual), actual) == 1;
    ok = ok && CRYPTO_memcmp(expected, actual, sizeof(actual)) == 0;
    OPENSSL_cleanse(expected, sizeof(expected));
    OPENSSL_cleanse(actual, sizeof(actual));
    return ok;
}

bool register_user(App *app, const char *name, const char *password) {
    if (!valid_name(name))
        return app_error(app, "Name must contain 1-48 ASCII letters, digits, '_' or '-'.");
    if (!password || strlen(password) < 8 || strlen(password) > 128)
        return app_error(app, "Password must contain 8-128 characters.");
    char encoded[HASH_CAP];
    if (!password_hash(password, encoded))
        return app_error(app, "Password hashing failed; nothing was saved.");
    if (!store_begin(app))
        return false;
    sqlite3_stmt *statement = NULL;
    bool ok = store_prepare(app, &statement, "INSERT INTO users(name,password) VALUES(?,?)");
    if (ok) {
        sqlite3_bind_text(statement, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, encoded, -1, SQLITE_TRANSIENT);
        int result = sqlite3_step(statement);
        if (result != SQLITE_DONE)
            ok = app_error(app,
                           result == SQLITE_CONSTRAINT
                               ? "User already exists (names are case-insensitive)."
                               : "Cannot save user: %s",
                           sqlite3_errmsg(app->db));
        sqlite3_finalize(statement);
    }
    OPENSSL_cleanse(encoded, sizeof(encoded));
    if (!ok) {
        store_abort(app);
        return false;
    }
    return store_finish(app);
}

bool login_user(App *app, const char *name, const char *password, User *user) {
    if (!valid_name(name) || !password || strlen(password) > 128)
        return app_error(app, "Invalid username or password.");
    sqlite3_stmt *statement = NULL;
    if (!store_prepare(app, &statement,
                       "SELECT id,name,password FROM users WHERE name=? COLLATE NOCASE"))
        return false;
    sqlite3_bind_text(statement, 1, name, -1, SQLITE_TRANSIENT);
    char encoded[HASH_CAP] = {0};
    User found = {0};
    int result = sqlite3_step(statement);
    if (result == SQLITE_ROW) {
        found.id = sqlite3_column_int(statement, 0);
        snprintf(found.name, sizeof(found.name), "%s", sqlite3_column_text(statement, 1));
        snprintf(encoded, sizeof(encoded), "%s", sqlite3_column_text(statement, 2));
    }
    sqlite3_finalize(statement);
    if (result != SQLITE_ROW && result != SQLITE_DONE)
        return app_error(app, "Cannot read users: %s", sqlite3_errmsg(app->db));
    bool ok = result == SQLITE_ROW && password_verify(password, encoded);
    OPENSSL_cleanse(encoded, sizeof(encoded));
    if (!ok)
        return app_error(app, "Invalid username or password.");
    *user = found;
    return true;
}
