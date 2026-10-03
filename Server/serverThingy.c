#include "src/router.h"
#include "src/handlers.h"
#include "src/database.h"
#include "src/request.h"
#include "src/auth.h"
#include "src/jukebox.h"

#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>

#ifndef PORT
#define PORT 8080
#endif
#define BACKLOG 10


/*

    'sup? I am documentation Neuro (after first successful) launch of the server
    I started with 'whatever keeps the shit together' formatting so I future me is (which is the literally me from now) formatting the 
    code before adding CSS and JS routes and daemonize the server 

*/

void sigHandler(int sigNum);

volatile sig_atomic_t flag = 1; 

/* Browsers may try plain HTTP when the scheme is omitted. Consume its headers
 * before replying so closing the socket doesn't reset the redirect response.
 * Use a fixed local destination, never an untrusted Host header. */
static int redirect_plain_http(int socket)
{
    unsigned char first;
    ssize_t count = recv(socket, &first, 1, MSG_PEEK);
    if (count <= 0) {
        return(-1);
    }
    if (first != 'G' && first != 'H') {
        return(0);
    }

    char headers[4096];
    size_t used = 0;
    while (used < sizeof(headers) - 1) {
        count = recv(socket, headers + used, 1, 0);
        if (count != 1) {
            return(-1);
        }
        used++;
        headers[used] = '\0';
        if (used >= 4 && memcmp(headers + used - 4, "\r\n\r\n", 4) == 0) {
            break;
        }
    }
    if (used < 4 || memcmp(headers + used - 4, "\r\n\r\n", 4) != 0 ||
        (strncmp(headers, "GET ", 4) != 0 && strncmp(headers, "HEAD ", 5) != 0)) {
        return(-1);
    }

    char response[256];
    int length = snprintf(response, sizeof(response),
        "HTTP/1.1 308 Permanent Redirect\r\n"
        "Location: https://localhost:%d/\r\n"
        "Content-Length: 0\r\nConnection: close\r\n\r\n", PORT);
    if (length < 0 || (size_t)length >= sizeof(response)) {
        return(-1);
    }
    size_t sent = 0;
    while (sent < (size_t)length) {
        count = send(socket, response + sent, (size_t)length - sent, 0);
        if (count <= 0) {
            return(-1);
        }
        sent += (size_t)count;
    }
    return(1);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (auth_init() != 0) {
        fprintf(stderr, "Invalid or unreadable auth credentials; refusing to start.\n");
        return(EXIT_FAILURE);
    }

    int server_sock = -1;
    int client_sock = -1;

    TLSClient client = {0};
    SSL_CTX *context = NULL;

    int exit_status = EXIT_FAILURE;

    sqlite3 *db = set_db();

    if (!db) { return(EXIT_FAILURE); }

    struct sockaddr_in server_addr = {0};
    struct sockaddr_in client_addr = {0};

    struct sigaction sa = {0};

    sa.sa_handler = sigHandler;
    sigemptyset(&sa.sa_mask);
    /* Let blocking socket calls return EINTR so shutdown can finish. */
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1)
    {
        perror("Signal Fuck Up...");
        goto rome;
    }

    struct sigaction ignore_pipe = {.sa_handler = SIG_IGN};
    sigemptyset(&ignore_pipe.sa_mask);
    if (sigaction(SIGPIPE, &ignore_pipe, NULL) == -1) {
        perror("SIGPIPE setup failed");
        goto rome;
    }
    context = tls_context();
    if (!context) {
        goto rome;
    }

    int opt = 1;

    if (route("GET",  "/",        send_homepage) < 0 ||
        route("GET",  "/*.css",   send_css) < 0 ||
        route("GET",  "/*.js",    send_js) < 0 ||
        route("GET",  "/favicon.svg", send_favicon) < 0 ||
        route("GET",  "/favicon.png", send_favicon) < 0 ||
        route("GET",  "/favicon.ico", send_favicon) < 0 ||
        route("GET",  "/api/todos", send_todos_json) < 0 ||
        route("GET",  "/api/todos/*", send_todos_json) < 0 ||
        route("GET",  "/todos/*", send_todo_page) < 0 ||
        route("POST", "/",        handle_post) < 0 ||
        route("POST", "/folders", handle_folder) < 0 ||
        route("POST", "/folders/move", handle_folder) < 0 ||
        route("POST", "/complete", handle_complete) < 0 ||
        route("POST", "/update",  handle_update) < 0 || /*Longest if statement I've ever written so far*/
        route("POST", "/delete",  handle_delete) < 0) { goto rome; } 

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock == -1) {
        perror("Socket sucked it");
        goto rome;
    }

    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt failed");
        goto rome;
    }

    server_addr.sin_family = AF_INET; // Internet As Fuck 
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_addr.sin_port = htons(PORT);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind failed");
        goto rome;
    }

    if (listen(server_sock, BACKLOG) == -1) {
        perror("listen failed");
        goto rome;
    }

    printf("Server is running on https://localhost:%d\n", PORT);
    jukebox_init(server_sock);
    while (flag) {
        jukebox_reap();
        // Reap finished audio workers while idle without interrupting TLS I/O
        // with SIGCHLD. Shutdown signals still interrupt this bounded wait.
        struct pollfd listener = {.fd = server_sock, .events = POLLIN};
        int ready = poll(&listener, 1, 1000);
        if (ready < 0 && errno != EINTR) { perror("poll failed"); goto rome; }
        if (ready <= 0) continue;
        
        socklen_t client_len = sizeof(client_addr);
        client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock == -1) {
            if (errno == EINTR) { continue; }

            perror("accept failed");
            goto rome;
        }

        //Idle client fuckery part
        struct timeval timeout = {.tv_sec = 5};
        if (setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
            setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
            perror("client timeout setup failed");
            close(client_sock);
            client_sock = -1;
            continue;
        }

        if (redirect_plain_http(client_sock) != 0 ||
            tls_accept(&client, context, client_sock) < 0) {
            tls_close(&client);
            close(client_sock);
            client_sock = -1;
            continue;
        }

        char buf[HTTP_REQUEST_CAPACITY];
        int status = read_http_request(&client, buf, sizeof(buf));
        if (!flag) { break; }
        if (status == 200) {
            printf("Request: %.*s\n", (int)strcspn(buf, "\r\n"), buf);
            handle_request(&client, db, buf);
        } 
        
        else if (status != 0) {
            send_request_error(&client, status);
        }

        printf("Server's live\n");


        tls_close(&client);
        close(client_sock);
        client_sock = -1;
    }

    exit_status = EXIT_SUCCESS;

rome:
    jukebox_shutdown();
    tls_close(&client);
    SSL_CTX_free(context);
    if (client_sock != -1) {
        close(client_sock);
    }
    if (server_sock != -1) {
        close(server_sock);
    }
    sqlite3_close(db);
    return(exit_status);
}


void sigHandler(int sigNum)
{
    if (sigNum == SIGTERM || sigNum == SIGINT) {
        flag = 0;
    }
}
