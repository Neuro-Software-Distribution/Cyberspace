#include "network.h"
#include <curl/curl.h>
#include <json-c/json.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static json_object *todo_json(const Todo *todo)
{
    json_object *json = json_object_new_object();
    json_object_object_add(json, "id", json_object_new_int64(todo->id));
    json_object_object_add(json, "title", json_object_new_string(todo->title));
    json_object_object_add(json, "content", json_object_new_string(todo->content));
    json_object_object_add(json, "completed", json_object_new_boolean(todo->completed));
    json_object_object_add(json, "created_at", json_object_new_int64(todo->created_at));
    json_object_object_add(json, "folder_id", json_object_new_int64(todo->folder_id));
    json_object_object_add(json, "folder_name", todo->folder_name ? json_object_new_string(todo->folder_name) : NULL);
    return json;
}

int main(int argc, char **argv)
{
    const char *server = NULL, *ca = NULL, *user = NULL, *command = "list";
    int64_t id = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--help")) goto usage;
        if (i + 1 >= argc) goto invalid;
        if (!strcmp(argv[i], "--server")) server = argv[++i];
        else if (!strcmp(argv[i], "--ca")) ca = argv[++i];
        else if (!strcmp(argv[i], "--user")) user = argv[++i];
        else goto invalid;
    }
    if (i < argc) command = argv[i++];
    if (!strcmp(command, "get")) {
        if (i >= argc || !argv[i][0] || strspn(argv[i], "0123456789") != strlen(argv[i])) goto invalid;
        errno = 0;
        id = strtoll(argv[i++], NULL, 10);
        if (errno || id <= 0) goto invalid;
    } else if (strcmp(command, "list")) goto invalid;
    if (!server || i != argc) goto invalid;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
    TodoClient *client = todo_client_new(server, ca);
    if (!client) {
        fputs("Could not initialize client. Use an HTTPS server origin (https://host:port).\n", stderr);
        curl_global_cleanup();
        return 1;
    }
    int result = 0;
    if (user) {
        if (!isatty(STDIN_FILENO)) {
            fputs("Login requires a terminal for the password prompt.\n", stderr);
            result = -1;
            goto done;
        }
        char *password = getpass("Password: ");
        if (!password) { result = -1; goto done; }
        result = todo_client_login(client, user, password);
        size_t length = strlen(password);
        volatile char *wipe = password;
        while (length) wipe[--length] = 0;
        if (result) goto error;
    }
    json_object *output = NULL;
    if (id) {
        Todo todo = {0};
        result = todo_client_get(client, id, &todo);
        if (!result) output = todo_json(&todo);
        todo_free(&todo);
    } else {
        TodoList list = {0};
        result = todo_client_list(client, &list);
        if (!result) {
            output = json_object_new_array();
            for (size_t n = 0; n < list.count; n++) json_object_array_add(output, todo_json(&list.items[n]));
        }
        todo_list_free(&list);
    }
    if (output) {
        if (puts(json_object_to_json_string_ext(output, JSON_C_TO_STRING_PRETTY)) == EOF || fflush(stdout) == EOF) result = -1;
        json_object_put(output);
    }
error:
    if (result) fprintf(stderr, "Fetch failed: %s\n", todo_client_error(client));
done:
    todo_client_free(client);
    curl_global_cleanup();
    return result ? 1 : 0;
invalid:
    fputs("Invalid arguments.\n", stderr);
usage:
    fprintf(stderr, "Usage: %s --server https://host:port [--ca cert.pem] [--user name] [list | get ID]\n", argv[0]);
    return i < argc && !strcmp(argv[i], "--help") ? 0 : 2;
}
