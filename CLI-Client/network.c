#include "network.h"
#include <curl/curl.h>
#include <json-c/json.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESPONSE_LIMIT (16 * 1024 * 1024)

struct TodoClient {
    CURL *curl;
    char *base;
    char error[CURL_ERROR_SIZE];
    long status;
};

struct response {
    char *data;
    size_t length;
};

static int fail(TodoClient *client, const char *message)
{
    snprintf(client->error, sizeof(client->error), "%s", message);
    return -1;
}

static size_t receive(char *data, size_t size, size_t count, void *userdata)
{
    struct response *response = userdata;
    if (size && count > SIZE_MAX / size) return 0;
    size_t length = size * count;
    if (length > RESPONSE_LIMIT - response->length) return 0;
    char *grown = realloc(response->data, response->length + length + 1);
    if (!grown) return 0;
    response->data = grown;
    memcpy(grown + response->length, data, length);
    response->length += length;
    grown[response->length] = 0;
    return length;
}

TodoClient *todo_client_new(const char *server_url, const char *ca_file)
{
    if (!server_url) return NULL;
    CURLU *url = curl_url();
    char *scheme = NULL, *host = NULL, *path = NULL, *extra = NULL;
    int valid = url && curl_url_set(url, CURLUPART_URL, server_url, CURLU_DISALLOW_USER) == CURLUE_OK &&
        curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK && !strcmp(scheme, "https") &&
        curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
        curl_url_get(url, CURLUPART_PATH, &path, 0) == CURLUE_OK && !strcmp(path, "/");
    if (url && curl_url_get(url, CURLUPART_QUERY, &extra, 0) == CURLUE_OK) valid = 0;
    curl_free(extra); extra = NULL;
    if (url && curl_url_get(url, CURLUPART_FRAGMENT, &extra, 0) == CURLUE_OK) valid = 0;
    curl_free(extra); curl_free(scheme); curl_free(host); curl_free(path); curl_url_cleanup(url);
    if (!valid) return NULL;
    TodoClient *client = calloc(1, sizeof(*client));
    if (!client) return NULL;
    client->base = strdup(server_url);
    client->curl = curl_easy_init();
    if (!client->base || !client->curl) { todo_client_free(client); return NULL; }
    size_t length = strlen(client->base);
    if (length && client->base[length - 1] == '/') client->base[length - 1] = 0;
#define SET(option, value) if (curl_easy_setopt(client->curl, option, value) != CURLE_OK) { todo_client_free(client); return NULL; }
    SET(CURLOPT_PROTOCOLS_STR, "https");
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_CONNECTTIMEOUT, 10L);
    SET(CURLOPT_TIMEOUT, 30L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_COOKIEFILE, "");
    SET(CURLOPT_WRITEFUNCTION, receive);
    SET(CURLOPT_ERRORBUFFER, client->error);
    SET(CURLOPT_USERAGENT, "Cyberspace-CLI/0.1");
    if (ca_file) { SET(CURLOPT_CAINFO, ca_file); }
#undef SET
    return client;
}

void todo_client_free(TodoClient *client)
{
    if (!client) return;
    curl_easy_cleanup(client->curl);
    free(client->base);
    free(client);
}

const char *todo_client_error(const TodoClient *client) { return client->error; }
long todo_client_status(const TodoClient *client) { return client->status; }

static json_object *request(TodoClient *client, const char *path, const char *body)
{
    client->error[0] = 0;
    client->status = 0;
    size_t length = strlen(client->base) + strlen(path) + 1;
    char *url = malloc(length);
    if (!url) { fail(client, "Out of memory"); return NULL; }
    snprintf(url, length, "%s%s", client->base, path);
    struct response response = {0};
    CURLcode code = curl_easy_setopt(client->curl, CURLOPT_URL, url);
    free(url);
    if (code == CURLE_OK) code = curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, &response);
    if (code == CURLE_OK) code = curl_easy_setopt(client->curl, CURLOPT_POSTFIELDS, body);
    if (code == CURLE_OK && !body) code = curl_easy_setopt(client->curl, CURLOPT_HTTPGET, 1L);
    if (code == CURLE_OK) code = curl_easy_perform(client->curl);
    curl_easy_getinfo(client->curl, CURLINFO_RESPONSE_CODE, &client->status);
    curl_easy_setopt(client->curl, CURLOPT_POSTFIELDS, NULL);
    curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, NULL);
    json_object *json = NULL;
    if (code != CURLE_OK) {
        if (!client->error[0]) fail(client, curl_easy_strerror(code));
    } else if (client->status == 401 || client->status == 303) {
        fail(client, "Sign in required, session expired, or incorrect credentials");
    } else if (client->status != 200) {
        snprintf(client->error, sizeof(client->error), "Server returned HTTP %ld", client->status);
    } else {
        char *type = NULL;
        curl_easy_getinfo(client->curl, CURLINFO_CONTENT_TYPE, &type);
        if (!type || (strcmp(type, "application/json") && strncmp(type, "application/json;", 17))) {
            fail(client, "Expected JSON; update the server to support /api/todos");
        } else {
            json_tokener *parser = json_tokener_new();
            if (parser) {
                json_tokener_set_flags(parser, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
                json = json_tokener_parse_ex(parser, response.data ? response.data : "", (int)response.length);
                size_t end = json_tokener_get_parse_end(parser);
                while (end < response.length && isspace((unsigned char)response.data[end])) end++;
                if (json_tokener_get_error(parser) != json_tokener_success || end != response.length) {
                    json_object_put(json); json = NULL;
                }
                json_tokener_free(parser);
            }
            if (!json) fail(client, "Invalid JSON response");
        }
    }
    free(response.data);
    return json;
}

int todo_client_login(TodoClient *client, const char *username, const char *password)
{
    if (!username || !password || strlen(username) > 64 || strlen(password) > 1024)
        return fail(client, "Invalid credential length");
    char *user = curl_easy_escape(client->curl, username, 0);
    char *pass = curl_easy_escape(client->curl, password, 0);
    if (!user || !pass) { curl_free(user); curl_free(pass); return fail(client, "Out of memory"); }
    size_t length = strlen(user) + strlen(pass) + 20;
    char *body = malloc(length);
    if (body) snprintf(body, length, "username=%s&password=%s", user, pass);
    curl_free(user);
    volatile char *wipe = pass;
    size_t pass_length = strlen(pass);
    for (size_t i = 0; i < pass_length; i++) wipe[i] = 0;
    curl_free(pass);
    if (!body) return fail(client, "Out of memory");
    json_object *json = request(client, "/login", body);
    wipe = body;
    for (size_t i = 0; i < length; i++) wipe[i] = 0;
    free(body);
    if (!json) return -1;
    json_object *ok = NULL;
    int valid = json_object_object_get_ex(json, "ok", &ok) &&
        json_object_is_type(ok, json_type_boolean) && json_object_get_boolean(ok);
    json_object_put(json);
    return valid ? 0 : fail(client, "Invalid login response");
}

void todo_free(Todo *todo)
{
    free(todo->title); free(todo->content); free(todo->folder_name);
    memset(todo, 0, sizeof(*todo));
}

void todo_list_free(TodoList *list)
{
    for (size_t i = 0; i < list->count; i++) todo_free(&list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int parse_todo(json_object *json, Todo *todo)
{
    const char *names[] = {"id", "title", "content", "completed", "created_at", "folder_id", "folder_name"};
    enum json_type types[] = {json_type_int, json_type_string, json_type_string, json_type_boolean,
        json_type_int, json_type_int, json_type_string};
    json_object *fields[7];
    for (size_t i = 0; i < 7; i++) {
        if (!json_object_object_get_ex(json, names[i], &fields[i]) ||
            (!json_object_is_type(fields[i], types[i]) && !(i == 6 && !fields[i]))) return -1;
        if (types[i] == json_type_string && fields[i] &&
            strlen(json_object_get_string(fields[i])) != (size_t)json_object_get_string_len(fields[i])) return -1;
    }
    todo->id = json_object_get_int64(fields[0]);
    todo->created_at = json_object_get_int64(fields[4]);
    todo->folder_id = json_object_get_int64(fields[5]);
    if (todo->id <= 0 || todo->folder_id < 0) return -1;
    todo->title = strdup(json_object_get_string(fields[1]));
    todo->content = strdup(json_object_get_string(fields[2]));
    todo->completed = json_object_get_boolean(fields[3]);
    todo->folder_name = fields[6] ? strdup(json_object_get_string(fields[6])) : NULL;
    if (!todo->title || !todo->content || (fields[6] && !todo->folder_name)) { todo_free(todo); return -1; }
    return 0;
}

int todo_client_list(TodoClient *client, TodoList *out)
{
    json_object *json = request(client, "/api/todos", NULL), *array = NULL;
    if (!json) return -1;
    TodoList result = {0};
    int valid = json_object_object_get_ex(json, "todos", &array) && json_object_is_type(array, json_type_array);
    if (valid) {
        result.count = json_object_array_length(array);
        result.items = calloc(result.count ? result.count : 1, sizeof(*result.items));
        valid = result.items != NULL;
        if (!valid) result.count = 0;
        for (size_t i = 0; valid && i < result.count; i++) valid = parse_todo(json_object_array_get_idx(array, i), &result.items[i]) == 0;
    }
    json_object_put(json);
    if (!valid) { todo_list_free(&result); return fail(client, "Invalid todo list or out of memory"); }
    *out = result;
    return 0;
}

int todo_client_get(TodoClient *client, int64_t id, Todo *out)
{
    if (id <= 0) return fail(client, "Invalid todo id");
    char path[64];
    snprintf(path, sizeof(path), "/api/todos/%lld", (long long)id);
    json_object *json = request(client, path, NULL);
    if (!json) return -1;
    Todo result = {0};
    int valid = parse_todo(json, &result) == 0 && result.id == id;
    json_object_put(json);
    if (!valid) { todo_free(&result); return fail(client, "Invalid todo response"); }
    *out = result;
    return 0;
}
