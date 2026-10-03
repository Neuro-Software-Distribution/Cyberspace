#include "auth.h"
#include "utils.h"
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sqlite3.h>
#include <stdlib.h>

#define SESSION_COUNT 64
#define SESSION_SECONDS 28800
static int enabled;
static char username[65];
static unsigned char salt[32], password_hash[32];
static struct { char token[65]; time_t expires; sqlite3_int64 user_id; } sessions[SESSION_COUNT];
static sqlite3 *users_db;
static unsigned failures;
static time_t blocked_until;

static int unhex(const char *text, unsigned char *out, size_t length)
{
    if (strlen(text) != length * 2) {
        return(0);
    }

    for (size_t i = 0; i < length; i++) {
        
        unsigned value = 0;
        
        for (size_t j = 0; j < 2; j++) {
            char c = text[i * 2 + j];
            unsigned digit;
            if (c >= '0' && c <= '9') {
                digit = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                digit = c - 'a' + 10;
            } else {
                return(0);
            }
            value = value * 16 + digit;
        }
        out[i] = (unsigned char)value;
    }

    return(1);
}

int auth_init(void)
{
    char path[PATH_MAX];
    
    if (get_server_path(path, sizeof(path), "auth/credentials") != OK) {
        return(-1);
    }
    
    char users_path[PATH_MAX];
    struct stat users_info;
    if (get_server_path(users_path, sizeof(users_path), "auth/users.db") != OK) { return(-1); }
    if (lstat(users_path, &users_info) == 0) {
        if (!S_ISREG(users_info.st_mode) || (users_info.st_mode & 077) || users_info.st_uid != geteuid()) { return(-1); }
        if (sqlite3_open_v2(users_path, &users_db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) { return(-1); }
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(users_db, "SELECT id, username, salt, password_hash FROM users", -1, &stmt, NULL) != SQLITE_OK) { return(-1); }
        int rc, count = 0;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            if (sqlite3_column_int64(stmt, 0) <= 0 || !sqlite3_column_text(stmt, 1) ||
                sqlite3_column_bytes(stmt, 2) != 32 || sqlite3_column_bytes(stmt, 3) != 32) {
                sqlite3_finalize(stmt); return(-1);
            }
            count++;
        }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE || !count) { return(-1); }
        enabled = 1;
        return(0);
    }
    if (errno != ENOENT) { return(-1); }

    struct stat info;
    
    if (lstat(path, &info) != 0) {
        if (errno == ENOENT) {
            return(0);
        }
        return(-1);
    }
    
    if (!S_ISREG(info.st_mode) || (info.st_mode & 077) || info.st_uid != geteuid()) {
        return(-1);
    }
    
    FILE *file = fopen(path, "r");
    
    if (!file) {
        return(-1);
    }
    
    char version[32], salt_hex[65], hash_hex[65], extra;
    
    int fields = fscanf(file, "%31s %64s %64s %64s %c", version, username, salt_hex, hash_hex, &extra);
    
    fclose(file);
    
    if (fields != 4 || strcmp(version, "pbkdf2-sha256-600000") ||
        !unhex(salt_hex, salt, 32) || !unhex(hash_hex, password_hash, 32)) {
        return(-1);
    }
    
    enabled = 1;
    
    return(0);
}

int auth_enabled(void) { return(enabled); }

static void reply(TLSClient *client, int status, const char *type, const char *headers, const char *body)
{
    const char *reason = status == 200 ? "OK" : status == 303 ? "See Other" :
        status == 401 ? "Unauthorized" : status == 403 ? "Forbidden" :
        status == 429 ? "Too Many Requests" : "Service Unavailable";
    
    char head[1024];
    
    int n = snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
        "Content-Length: %zu\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'self'; frame-ancestors 'none'; form-action 'self'; base-uri 'none'\r\n"
        "%sConnection: close\r\n\r\n", status, reason, type, strlen(body), headers);
    
    if (n > 0 && (size_t)n < sizeof(head) && tls_write_all(client, head, (size_t)n) == 0) {
        tls_write_all(client, body, strlen(body));
    }
}

/* Reject duplicate/oversized headers rather than disagree with a proxy. */
static int header(const char *raw, const char *name, char *out, size_t capacity)
{

    const char *line = strstr(raw, "\r\n");

    int found = 0;

    out[0] = '\0';

    while (line && line[2] && strncmp(line + 2, "\r\n", 2)) {
        line += 2;
        const char *end = strstr(line, "\r\n");
        if (!end) { return(-1); }

        size_t n = strlen(name);
        
        if ((size_t)(end - line) > n && line[n] == ':' && !strncasecmp(line, name, n)) {
            if (found) { return(-1); }
            
            const char *value = line + n + 1;
            
            while (value < end && (*value == ' ' || *value == '\t')) {
                value++;
            }
            
            const char *trim = end;
            
            while (trim > value && (trim[-1] == ' ' || trim[-1] == '\t')) {
                trim--;
            }
            
            size_t length = (size_t)(trim - value);
            
            if (length >= capacity) {
                return(-1);
            }
            
            memcpy(out, value, length); out[length] = '\0'; found = 1;
        }
        
        line = end;
    }
    
    return(found);
}

static int same_origin(const char *raw)
{
    char origin[512], host[256], site[64], expected[512];
    
    int o = header(raw, "Origin", origin, sizeof(origin));
    int h = header(raw, "Host", host, sizeof(host));
    int s = header(raw, "Sec-Fetch-Site", site, sizeof(site));
    
    if (o < 0 || h != 1 || s < 0 || !strcmp(site, "cross-site")) { return(0); }
    
    snprintf(expected, sizeof(expected), "https://%s", host);
    
    return(o == 0 || !strcmp(origin, expected));
}

static int session_for(const char *raw)
{
    char cookies[4096];
    
    if (header(raw, "Cookie", cookies, sizeof(cookies)) != 1) { return(-1); }
    char *save = NULL, *token = NULL;
    
    for (char *part = strtok_r(cookies, ";", &save); part; part = strtok_r(NULL, ";", &save)) {
        while (*part == ' ') {
            part++;
        }
    
        if (!strncmp(part, "__Host-todo_session=", 20)) {
    
            if (token) {
                return(-1);
            }
            token = part + 20;
        }
    }
    
    if (!token || strlen(token) != 64) { return(-1); }
    
    for (int i = 0; i < SESSION_COUNT; i++) {
        if (sessions[i].expires > time(NULL) && !CRYPTO_memcmp(token, sessions[i].token, 64)) { return(i); }
    }

    return(-1);
}

static int field(const char *body, const char *name, char *out, size_t capacity)
{
    size_t n = strlen(name); int found = 0;
    
    for (const char *p = body; *p;) {
        const char *end = strchr(p, '&');
        if (!end) { end = p + strlen(p); }
        
        if ((size_t)(end - p) > n && !strncmp(p, name, n) && p[n] == '=') {
            if (found++) { return(0); }
            
            size_t used = 0;
            
            for (const char *v = p + n + 1; v < end; v++) {
                unsigned char c = (unsigned char)*v;
                if (c == '+') {
                    c = ' ';
                } 
                
                else if (c == '%') {
                    if (end - v < 3) { return(0); }
                    
                    char hex[3] = {v[1], v[2], 0};
                    
                    for (int j = 0; j < 2; j++) {
                        if (hex[j] >= 'A' && hex[j] <= 'F') {
                            hex[j] += 'a' - 'A';
                        }
                    }
                    
                    if (!unhex(hex, &c, 1)) { return(0); }
                    v += 2;
                }
                if (!c || used + 1 >= capacity) { return(0); }
                
                out[used++] = (char)c;
            }

            out[used] = '\0';
        }

        p = *end ? end + 1 : end;
    }
    
    return(found == 1);
}

int auth_handle(TLSClient *client, const char *method, const char *path, const char *raw, const char *body)
{
    client->user_id = 1; /* The original/local account owns legacy todos. */
    int get = !strcmp(method, "GET"), post = !strcmp(method, "POST");
    
    if (get && !strcmp(path, "/auth/status")) {
        reply(client, 200, "application/json", "", enabled ? "{\"enabled\":true}" : "{\"enabled\":false}");
        return(1);
    }
    
    if (get && !strcmp(path, "/login")) {
        char filename[PATH_MAX], page[16384];
    
        FILE *file = NULL;
    
        if (get_server_path(filename, sizeof(filename), "frontend/login.html") == OK) {
            file = fopen(filename, "r");
        }
    
        if (!file) {
            reply(client, 503, "text/plain", "", "Login page unavailable. Run build.sh update.");
        } 
        
        else {
            size_t n = fread(page, 1, sizeof(page) - 1, file); page[n] = 0;
            fclose(file); reply(client, 200, "text/html; charset=utf-8", "", page);
        }
     
        return(1);
    }
    
    if (!enabled) {
        if (post && !strcmp(path, "/logout")) {
            reply(client, 303, "text/plain", "Location: /\r\n", ""); return(1);
        }
    
        if (post && !strcmp(path, "/login")) {
            reply(client, 503, "application/json", "", "{\"error\":\"Login is not configured on this server yet.\"}"); return(1);
        }
    
        return(0);
    }
    if (!get && strcmp(method, "HEAD") && !same_origin(raw)) {
        reply(client, 403, "application/json", "", "{\"error\":\"Request origin rejected.\"}"); return(1);
    }
    
    if (get && (!strcmp(path, "/style.css") || !strcmp(path, "/login.js") || !strcmp(path, "/intro.js") || !strcmp(path, "/favicon.png"))) {
        return(0);
    }
    
    int session = session_for(raw);
    
    if (post && !strcmp(path, "/login")) {
        if (blocked_until > time(NULL)) {
            reply(client, 429, "application/json", "Retry-After: 60\r\n", "{\"error\":\"Too many attempts. Give it a minute, then try again.\"}"); return(1);
        }
        char user[65] = {0}, password[1025] = {0}; unsigned char derived[32];
    
        int valid = field(body, "username", user, sizeof(user)) && field(body, "password", password, sizeof(password));
        sqlite3_int64 user_id = 1;
        unsigned char login_salt[32], login_hash[32];
        memcpy(login_salt, salt, 32);
        memcpy(login_hash, password_hash, 32);
        int found = !strcmp(user, username);
        if (users_db) {
            sqlite3_stmt *stmt = NULL;
            found = 0;
            if (sqlite3_prepare_v2(users_db, "SELECT id, salt, password_hash FROM users WHERE username = ?", -1, &stmt, NULL) != SQLITE_OK) {
                OPENSSL_cleanse(password, sizeof(password));
                reply(client, 503, "application/json", "", "{\"error\":\"User database unavailable.\"}"); return(1);
            }
            sqlite3_bind_text(stmt, 1, user, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_bytes(stmt, 1) == 32 && sqlite3_column_bytes(stmt, 2) == 32) {
                user_id = sqlite3_column_int64(stmt, 0);
                memcpy(login_salt, sqlite3_column_blob(stmt, 1), 32);
                memcpy(login_hash, sqlite3_column_blob(stmt, 2), 32);
                found = user_id > 0;
            }
            sqlite3_finalize(stmt);
        }
        int hashed = PKCS5_PBKDF2_HMAC(password, (int)strlen(password), login_salt, 32, 600000, EVP_sha256(), 32, derived);
    
        OPENSSL_cleanse(password, sizeof(password));
    
        int matches = hashed == 1 && CRYPTO_memcmp(derived, login_hash, 32) == 0;
    
        OPENSSL_cleanse(derived, sizeof(derived));
    
        if (!valid || !matches || !found) {
            if (++failures >= 5) { blocked_until = time(NULL) + 60; failures = 0; }
            reply(client, 401, "application/json", "", "{\"error\":\"Username or password didn't match. Try again.\"}"); return(1);
        }
    
        failures = 0;
    
        if (session >= 0) {
            sessions[session].expires = 0;
        }
    
        int slot = 0;
    
        for (int i = 0; i < SESSION_COUNT; i++) {
            if (sessions[i].expires < sessions[slot].expires) {
                slot = i;
            }
        }
    
        unsigned char random[32];
    
        if (RAND_bytes(random, sizeof(random)) != 1) {
            reply(client, 503, "application/json", "", "{\"error\":\"Could not start a session.\"}"); return(1);
        }
    
        for (size_t i = 0; i < sizeof(random); i++) {
            sprintf(sessions[slot].token + i * 2, "%02x", random[i]);
        }
    
        sessions[slot].user_id = user_id;
        sessions[slot].expires = time(NULL) + SESSION_SECONDS;
    
        char cookie[256];
    
        snprintf(cookie, sizeof(cookie), "Set-Cookie: __Host-todo_session=%s; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=%d\r\n", sessions[slot].token, SESSION_SECONDS);
    
        reply(client, 200, "application/json", cookie, "{\"ok\":true}"); return(1);
    }
    
    if (post && !strcmp(path, "/logout")) {
        if (session >= 0) { sessions[session].expires = 0; }
        
        reply(client, 303, "text/plain", "Location: /login\r\nSet-Cookie: __Host-todo_session=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0\r\n", ""); return(1);
    }
    
    if (session < 0) {
        int redirect = get && strncmp(path, "/api/", 5);
        reply(client, redirect ? 303 : 401, "application/json", redirect ? "Location: /login\r\n" : "", "{\"error\":\"Please sign in.\"}"); return(1);
    }
    
    client->user_id = sessions[session].user_id;
    return(0);
}
