import os
from pathlib import Path
import shlex
import shutil
import sqlite3
import subprocess
import tempfile
from html.parser import HTMLParser

root = Path(__file__).resolve().parents[1]

class Todos(HTMLParser):
    def __init__(self, source):
        super().__init__()
        self.depth = 0
        self.items = []
        self.feed(source)

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == 'details':
            assert 'open' not in attrs
            self.depth += 1
        if 'data-todo-id' in attrs:
            self.items.append((attrs['data-todo-id'], self.depth))

    def handle_endtag(self, tag):
        if tag == 'details':
            self.depth -= 1

with tempfile.TemporaryDirectory(prefix='todo-folders-') as directory:
    work = Path(directory)
    (work / 'db').mkdir()
    shutil.copytree(root / 'frontend', work / 'frontend')
    db_path = work / 'db/todo.db'
    with sqlite3.connect(db_path) as db:
        db.executescript("CREATE TABLE TODOS (ID INTEGER PRIMARY KEY AUTOINCREMENT, Title TEXT NOT NULL, Content TEXT NOT NULL, Completed INTEGER NOT NULL DEFAULT 0, CreatedAt INTEGER NOT NULL); INSERT INTO TODOS (Title, Content, CreatedAt) VALUES ('legacy todo', 'notes', 1);")
    harness = work / 'folders.c'
    harness.write_text('''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "handlers.h"
#include "database.h"
int tls_write_all(TLSClient *client, const char *data, size_t length) {
    (void)client;
    return fwrite(data, 1, length, stdout) == length ? 0 : -1;
}
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    sqlite3 *db = set_db();
    if (!db) return 3;
    TLSClient client = {.user_id = atoll(argv[2])};
    if (!strcmp(argv[1], "/")) send_homepage(&client, db, "/", "");
    else handle_folder(&client, db, argv[1], argv[3]);
    sqlite3_close(db);
    return 0;
}
''')
    binary = work / 'folders'
    sources = [root / 'src' / f'{name}.c' for name in ('handlers', 'database', 'utils', 'template', 'markdown')]
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-Wall', '-Wextra', '-Werror', '-I', str(root / 'src'), str(harness),
        *map(str, sources), *map(str, (root / 'vendor/md4c').glob('*.c')),
        '-lsqlite3', '-o', str(binary)], check=True)
    env = dict(os.environ, TODO_SERVER_PATH=str(work))

    def request(path='/', user=1, body='', status=200):
        response = subprocess.check_output([str(binary), path, str(user), body], env=env, text=True)
        assert response.startswith(f'HTTP/1.1 {status} '), response
        return response

    assert Todos(request()).items == [('1', 0)]
    request('/folders', body='name=%3Cscript%3E%26+chaos', status=303)
    request('/folders', user=2, body='name=private-folder', status=303)
    request('/folders/move', body='id=1&folder_id=1', status=303)
    page = request()
    assert Todos(page).items == [('1', 1)]
    assert '&lt;script&gt;&amp; chaos/' in page
    assert 'private-folder' not in page
    assert 'legacy todo' not in request(user=2)
    request('/folders/move', body='id=1&folder_id=2', status=404)
    request('/folders/move', user=2, body='id=1&folder_id=2', status=404)
    request('/folders/move', body='id=1&folder_id=999', status=404)
    assert Todos(request()).items == [('1', 1)]
    for body in ('id=1&folder_id=-1', 'id=1&folder_id=1x', 'id=1&folder_id=0&folder_id=1', 'id=999999999999999999999&folder_id=0', 'id=1'):
        request('/folders/move', body=body, status=400)
    for body in ('name=', 'name=+++', 'name=%00', 'name=' + 'a' * 121, 'name=a&name=b'):
        request('/folders', body=body, status=400)
    request('/folders/move', body='id=1&folder_id=0', status=303)
    assert Todos(request()).items == [('1', 0)]
    with sqlite3.connect(db_path) as db:
        assert db.execute('SELECT Title, Content, UserId, FolderId FROM TODOS').fetchone() == ('legacy todo', 'notes', 1, 0)
print('Folder migration, rendering, persistence, validation and account isolation checks passed')
