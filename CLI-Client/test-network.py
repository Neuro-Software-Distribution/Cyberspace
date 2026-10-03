import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
import shlex
import shutil
import socket
import sqlite3
import ssl
import subprocess
import tempfile
import threading
import time

root = Path(__file__).resolve().parent
server_root = root.parent / 'Server'
cc = shlex.split(os.environ.get('CC', 'cc'))

with tempfile.TemporaryDirectory(prefix='cyberspace-network-') as directory:
    work = Path(directory)
    for name in ('db', 'auth', 'tls'):
        (work / name).mkdir()
    shutil.copytree(server_root / 'frontend', work / 'frontend')
    cert, key = work / 'tls/cert.pem', work / 'tls/key.pem'
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
        '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost',
        '-out', str(cert), '-keyout', str(key)], check=True, capture_output=True)
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    sources = [server_root / 'serverThingy.c', *sorted((server_root / 'src').glob('*.c')),
        *sorted((server_root / 'vendor/md4c').glob('*.c'))]
    server_binary = work / 'server'
    subprocess.run([*cc, '-Wall', '-Wextra', '-Werror', f'-DPORT={port}', *map(str, sources),
        '-o', str(server_binary), '-lsqlite3', '-lssl', '-lcrypto'], check=True)
    with sqlite3.connect(work / 'db/todo.db') as db:
        db.executescript('CREATE TABLE TODOS (ID INTEGER PRIMARY KEY, Title TEXT NOT NULL, Content TEXT NOT NULL, Completed INTEGER NOT NULL DEFAULT 0, CreatedAt INTEGER NOT NULL, UserId INTEGER NOT NULL DEFAULT 1, FolderId INTEGER NOT NULL DEFAULT 0); CREATE TABLE FOLDERS (ID INTEGER PRIMARY KEY, UserId INTEGER NOT NULL, Name TEXT NOT NULL);')
        db.execute('INSERT INTO FOLDERS VALUES (3, 1, ?)', ('Ideas & stuff',))
        db.execute('INSERT INTO TODOS VALUES (1, ?, ?, 1, 42, 1, 3)', ('quotes " \\ café', '# Notes\n\t\x1b Unicode ✓'))
        db.execute("INSERT INTO TODOS VALUES (2, 'private', 'second account', 0, 43, 2, 0)")
    url = f'https://localhost:{port}'
    env = dict(os.environ, TODO_SERVER_PATH=str(work), NO_PROXY='localhost,127.0.0.1', no_proxy='localhost,127.0.0.1')

    def cli(*args, ca=True, origin=url, success=True):
        result = subprocess.run([str(root / 'cyberspace'), '--server', origin,
            *(['--ca', str(cert)] if ca else []), *args], env=env, text=True, capture_output=True)
        assert (result.returncode == 0) == success, (result.returncode, result.stdout, result.stderr)
        return result

    def start():
        process = subprocess.Popen([str(server_binary)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            assert process.poll() is None, 'Server exited'
            try:
                with socket.create_connection(('127.0.0.1', port), timeout=.1):
                    return process
            except OSError:
                time.sleep(.05)
        process.terminate()
        process.wait()
        raise AssertionError('Server did not start')

    server = start()
    try:
        rows = json.loads(cli('list').stdout)
        assert len(rows) == 1 and rows[0]['id'] == 1
        assert rows[0]['folder_id'] == 3 and rows[0]['folder_name'] == 'Ideas & stuff'
        assert rows[0]['title'] == 'quotes " \\ café' and rows[0]['content'] == '# Notes\n\t\x1b Unicode ✓'
        assert json.loads(cli('get', '1').stdout) == rows[0]
        assert '404' in cli('get', '2', success=False).stderr
        cli('get', '999', success=False)
        cli('get', '1x', success=False)
        cli('get', '-1', success=False)
        cli('list', ca=False, success=False)
        cli('list', origin=f'https://127.0.0.1:{port}', success=False)
        cli('list', origin=f'http://localhost:{port}', success=False)
        cli('list', origin=url + '/bad-path', success=False)
    finally:
        server.terminate()
        server.wait(timeout=5)

    users = work / 'auth/users.db'
    with sqlite3.connect(users) as db:
        db.execute('CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT, salt BLOB, password_hash BLOB)')
        for user_id, username, password in [(1, 'owner', 'p&=+% word'), (2, 'guest', 'guest-password'), (3, 'empty', 'empty-password')]:
            salt = os.urandom(32)
            db.execute('INSERT INTO users VALUES (?, ?, ?, ?)', (user_id, username, salt,
                hashlib.pbkdf2_hmac('sha256', password.encode(), salt, 600000)))
    users.chmod(0o600)
    harness = work / 'client-test.c'
    harness.write_text(r'''
#include "network.h"
#include <curl/curl.h>
#include <assert.h>
#include <string.h>
int main(int argc, char **argv) {
    assert(argc == 3);
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    TodoClient *client = todo_client_new(argv[1], argv[2]);
    assert(client);
    TodoList list = {0};
    Todo todo = {0};
    assert(todo_client_list(client, &list) == -1);
    assert(todo_client_status(client) == 401);
    assert(todo_client_login(client, "owner", "wrong") == -1);
    assert(todo_client_status(client) == 401);
    assert(todo_client_login(client, "owner", "p&=+% word") == 0);
    assert(todo_client_list(client, &list) == 0);
    assert(list.count == 1 && list.items[0].id == 1 && list.items[0].completed);
    assert(list.items[0].folder_id == 3);
    todo_list_free(&list);
    assert(todo_client_get(client, 1, &todo) == 0);
    assert(!strcmp(todo.content, "# Notes\n\t\x1b Unicode ✓"));
    char *saved = todo.content;
    assert(todo_client_get(client, 999, &todo) == -1 && todo_client_status(client) == 404);
    assert(todo.content == saved);
    todo_free(&todo);
    assert(todo_client_login(client, "guest", "guest-password") == 0);
    assert(todo_client_get(client, 1, &todo) == -1 && todo_client_status(client) == 404);
    assert(todo_client_list(client, &list) == 0 && list.count == 1 && list.items[0].id == 2);
    assert(!list.items[0].folder_name && !list.items[0].folder_id);
    todo_list_free(&list);
    assert(todo_client_login(client, "empty", "empty-password") == 0);
    assert(todo_client_list(client, &list) == 0 && list.count == 0);
    todo_list_free(&list);
    todo_client_free(client);
    curl_global_cleanup();
}
''')
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libcurl', 'json-c'], text=True))
    harness_binary = work / 'client-test'
    subprocess.run([*cc, '-Wall', '-Wextra', '-Werror', '-I', str(root), str(harness), str(root / 'network.c'),
        '-o', str(harness_binary), *flags], check=True)
    server = start()
    try:
        assert 'Sign in' in cli('list', success=False).stderr
        subprocess.run([str(harness_binary), url, str(cert)], env=env, check=True)
    finally:
        server.terminate()
        server.wait(timeout=5)

    class Handler(BaseHTTPRequestHandler):
        status = 200
        body = b'{}'
        content_type = 'application/json'

        def do_GET(self):
            self.send_response(self.status)
            self.send_header('Content-Type', self.content_type)
            self.send_header('Content-Length', str(len(self.body)))
            self.end_headers()
            try:
                self.wfile.write(self.body)
            except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                pass

        def log_message(self, *args):
            pass

    mock = HTTPServer(('127.0.0.1', 0), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    mock.socket = context.wrap_socket(mock.socket, server_side=True)
    thread = threading.Thread(target=mock.serve_forever, daemon=True)
    thread.start()
    mock_url = f'https://localhost:{mock.server_port}'
    try:
        for status, content_type, body in [(500, 'application/json', b'{}'), (303, 'text/html', b'login'),
            (200, 'text/html', b'<html>oops</html>'), (200, 'application/json', b'{bad'),
            (200, 'application/json', b'{"todos":[]}junk'), (200, 'application/json', b'{"todos":[{}]}'),
            (200, 'application/json', b'{"todos":null}'), (200, 'application/json', b'x' * (16 * 1024 * 1024 + 1))]:
            Handler.status, Handler.content_type, Handler.body = status, content_type, body
            cli('list', origin=mock_url, success=False)
        Handler.status, Handler.content_type, Handler.body = 200, 'application/json', b'{"todos":[]}\n'
        assert json.loads(cli('list', origin=mock_url).stdout) == []
    finally:
        mock.shutdown()
        mock.server_close()
        thread.join()
print('Passed: HTTPS verification, login/cookies, account isolation, list/get, folders, escaping, empty lists, malformed responses and failure preservation')
