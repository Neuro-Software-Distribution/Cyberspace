#	First, WTF Makefile? Second, I ain't remembering those order anyways so...here you go

# ========================
# Configurable variables
# ========================
MAIN      ?= serverThingy.c
SOURCES   ?= src/handlers.c src/utils.c src/router.c src/database.c src/template.c src/request.c src/markdown.c src/tls.c src/auth.c src/jukebox.c
MD4C_SOURCES = vendor/md4c/md4c.c vendor/md4c/md4c-html.c vendor/md4c/entity.c
ALL_SOURCES = $(SOURCES) $(MD4C_SOURCES)
HEADERS = $(wildcard src/*.h vendor/md4c/*.h)
TARGET    ?= server
CC        ?= gcc
CFLAGS    ?= -Wall -Wextra
LDFLAGS   ?= 
LDLIBS    ?= -lsqlite3 -lssl -lcrypto


OBJS = $(MAIN:.c=.o) $(ALL_SOURCES:.c=.o)

.PHONY: all clean run rebuild test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "→ Built $(TARGET)"

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
	@echo "→ Cleaned"

rebuild: clean all

run: $(TARGET)
	./$(TARGET)

test:
	python3 scripts/test-folders.py
	node scripts/test-folders-ui.js
	python3 scripts/test-markdown.py
	node scripts/test-navigation-ui.js
	python3 scripts/test-jukebox.py
	python3 scripts/test-music-import.py
	python3 scripts/test-build-music.py
	node scripts/test-jukebox-ui.js
	node scripts/test-editor-ui.js
