#include "database.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include "utils.h"

sqlite3 *set_db(void)
{
    sqlite3 *database = NULL;
    char db_path[PATH_MAX];

    if (get_server_path(db_path, sizeof(db_path), "db/todo.db") != OK) {
        return(NULL);
    }

    int rc = sqlite3_open(db_path, &database);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(database));
        if (database){ sqlite3_close(database); }
        return(NULL);
    }

    const char *create_sql =
        "CREATE TABLE IF NOT EXISTS TODOS ("
        "ID INTEGER PRIMARY KEY AUTOINCREMENT,"
        "Title TEXT NOT NULL,"
        "Content TEXT NOT NULL,"
        "Completed INTEGER NOT NULL DEFAULT 0,"
        "CreatedAt INTEGER NOT NULL"
        ");";

    char *errmsg = NULL;
    rc = sqlite3_exec(database, create_sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Create table failed: %s\n", errmsg);
        sqlite3_free(errmsg);
        sqlite3_close(database);
        return(NULL);
    }

    /* Old single-user data belongs to account 1, including local-mode data. */
    sqlite3_stmt *columns = NULL;
    int has_owner = 0, has_folder = 0;
    if (sqlite3_prepare_v2(database, "PRAGMA table_info(TODOS)", -1, &columns, NULL) != SQLITE_OK) {
        sqlite3_close(database); return(NULL);
    }
    while (sqlite3_step(columns) == SQLITE_ROW) {
        if (!strcmp((const char *)sqlite3_column_text(columns, 1), "FolderId")) { has_folder = 1; }
        if (!strcmp((const char *)sqlite3_column_text(columns, 1), "UserId")) { has_owner = 1; }
    }
    sqlite3_finalize(columns);
    if ((!has_owner && sqlite3_exec(database, "ALTER TABLE TODOS ADD COLUMN UserId INTEGER NOT NULL DEFAULT 1", NULL, NULL, NULL) != SQLITE_OK) ||
        sqlite3_exec(database, "CREATE INDEX IF NOT EXISTS todos_owner ON TODOS(UserId, CreatedAt)", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(database); return(NULL);
    }
    if ((!has_folder && sqlite3_exec(database, "ALTER TABLE TODOS ADD COLUMN FolderId INTEGER NOT NULL DEFAULT 0", NULL, NULL, NULL) != SQLITE_OK) ||
        sqlite3_exec(database,
            "CREATE TABLE IF NOT EXISTS FOLDERS (ID INTEGER PRIMARY KEY AUTOINCREMENT, UserId INTEGER NOT NULL, Name TEXT NOT NULL);"
            "CREATE INDEX IF NOT EXISTS folders_owner ON FOLDERS(UserId);"
            "CREATE INDEX IF NOT EXISTS todos_folder ON TODOS(UserId, FolderId);", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(database); return(NULL);
    }
    return(database);
}

enum STATUS add_todo(sqlite3 *db, sqlite3_int64 user_id, const char *title, const char *content)
{
    const char *sql = "INSERT INTO TODOS (Title, Content, CreatedAt, UserId) VALUES (?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "add_todo prepare: %s\n", sqlite3_errmsg(db));
        return(U_FUCKED);
    }

    time_t now = time(NULL);
    sqlite3_bind_text(stmt, 1, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, content, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)now);
    sqlite3_bind_int64(stmt, 4, user_id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "add_todo step: %s\n", sqlite3_errmsg(db));
        return(U_FUCKED);
    }
    return(OK);
}

enum STATUS delete_todo(sqlite3 *db, sqlite3_int64 user_id, int id)
{
    const char *sql = "DELETE FROM TODOS WHERE ID = ? AND UserId = ?;";
    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return(U_FUCKED);
    }

    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, user_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return(rc != SQLITE_DONE ? U_FUCKED : sqlite3_changes(db) ? OK : TODO_NOT_FOUND);
}

enum STATUS update_todo(sqlite3 *db, sqlite3_int64 user_id, int id, const char *content)
{
    const char *sql = "UPDATE TODOS SET Content = ? WHERE ID = ? AND UserId = ?;";
    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK){
        return(U_FUCKED);
    }

    sqlite3_bind_text(stmt, 1, content, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, id);
    sqlite3_bind_int64(stmt, 3, user_id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return(rc != SQLITE_DONE ? U_FUCKED : sqlite3_changes(db) ? OK : TODO_NOT_FOUND);
}

enum STATUS set_todo_completed(sqlite3 *db, sqlite3_int64 user_id, int id, int completed)
{
    if (!db || id <= 0 || (completed != 0 && completed != 1)) {
        return(U_FUCKED);
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE TODOS SET Completed = ? WHERE ID = ? AND UserId = ?;",
                           -1, &stmt, NULL) != SQLITE_OK) {
        return(U_FUCKED);
    }

    sqlite3_bind_int(stmt, 1, completed);
    sqlite3_bind_int(stmt, 2, id);
    sqlite3_bind_int64(stmt, 3, user_id);
    int rc = sqlite3_step(stmt);
    int changed = sqlite3_changes(db);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return(U_FUCKED);
    }
    return(changed ? OK : TODO_NOT_FOUND);
}

enum STATUS get_todo(sqlite3 *db, sqlite3_int64 user_id, int id, struct todo_data *out)
{
    const char *sql = "SELECT ID, Title, Content, Completed, CreatedAt FROM TODOS WHERE ID = ? AND UserId = ?;";
    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return(U_FUCKED);
    }

    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, user_id);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out->id        = sqlite3_column_int(stmt, 0);
        out->title     = strdup((const char *)sqlite3_column_text(stmt, 1));
        out->content   = strdup((const char *)sqlite3_column_text(stmt, 2));
        out->is_done   = sqlite3_column_int(stmt, 3);
        out->created_at = sqlite3_column_int64(stmt, 4);
        sqlite3_finalize(stmt);
        return(OK);
    }

    sqlite3_finalize(stmt);
    return(U_FUCKED);
}


enum STATUS foreach_todo(sqlite3 *db, sqlite3_int64 user_id, todo_callback cb, void *userdata)
{
    return foreach_folder_todo(db, user_id, -1, cb, userdata);
}

enum STATUS foreach_folder_todo(sqlite3 *db, sqlite3_int64 user_id, int folder_id, todo_callback cb, void *userdata)
{
    const char *sql = "SELECT ID, Title, Content, Completed, CreatedAt FROM TODOS WHERE UserId = ? AND (? < 0 OR FolderId = ?) ORDER BY CreatedAt DESC, ID DESC;";
    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return(U_FUCKED);
    }

    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_bind_int(stmt, 2, folder_id);
    sqlite3_bind_int(stmt, 3, folder_id);
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        struct todo_data t = {0};
        t.id         = sqlite3_column_int(stmt, 0);
        t.title      = (char *)sqlite3_column_text(stmt, 1);   
        t.content    = (char *)sqlite3_column_text(stmt, 2);
        t.is_done    = sqlite3_column_int(stmt, 3);
        t.created_at = sqlite3_column_int64(stmt, 4);

        cb(&t, userdata);
    }

    sqlite3_finalize(stmt);
    return(rc == SQLITE_DONE ? OK : U_FUCKED);
}
