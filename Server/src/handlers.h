#ifndef HANDLERS_H
#define HANDLERS_H
#include "tls.h"

#include <sqlite3.h>

typedef struct {
    int code;
    const char *meaning;
} HttpStatus;

void send_request_error(TLSClient *client, int code);

void send_404(TLSClient *client, sqlite3 *db, const char *path, const char *body);
void send_homepage(TLSClient *client, sqlite3 *db, const char *path, const char *body);
void send_css(TLSClient *client, sqlite3 *db, const char *path, const char *body);
void send_js(TLSClient *client, sqlite3 *db, const char *path, const char *body);
void send_favicon(TLSClient *client, sqlite3 *db, const char *path, const char *body);
void send_todo_page(TLSClient *client, sqlite3 *db, const char *path, const char *body);
void handle_post(TLSClient *client, sqlite3 *db, const char *path, const char *body);
void handle_update(TLSClient *client, sqlite3 *db, const char *path, const char *body);
void handle_complete(TLSClient *client, sqlite3 *db, const char *path, const char *body);
void handle_delete(TLSClient *client, sqlite3 *db, const char *path, const char *body);

void handle_folder(TLSClient *client, sqlite3 *db, const char *path, const char *body);

void send_todos_json(TLSClient *client, sqlite3 *db, const char *path, const char *body);

#endif
