#ifndef CYBERSPACE_NETWORK_H
#define CYBERSPACE_NETWORK_H

#include <stddef.h>
#include <stdint.h>

typedef struct TodoClient TodoClient;

typedef struct {
    int64_t id;
    char *title;
    char *content;
    int completed;
    int64_t created_at;
    int64_t folder_id;
    char *folder_name;
} Todo;

typedef struct {
    Todo *items;
    size_t count;
} TodoList;

TodoClient *todo_client_new(const char *server_url, const char *ca_file);
void todo_client_free(TodoClient *client);
const char *todo_client_error(const TodoClient *client);
long todo_client_status(const TodoClient *client);
int todo_client_login(TodoClient *client, const char *username, const char *password);
int todo_client_list(TodoClient *client, TodoList *out);
int todo_client_get(TodoClient *client, int64_t id, Todo *out);
void todo_free(Todo *todo);
void todo_list_free(TodoList *list);

#endif
