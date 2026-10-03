CC ?= cc
PKG_CONFIG ?= pkg-config
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libcurl json-c)
CFLAGS ?= -O2 -Wall -Wextra
LDLIBS += $(shell $(PKG_CONFIG) --libs libcurl json-c)

.PHONY: all clean test
all: cyberspace

cyberspace: main.o network.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

main.o network.o: network.h

clean:
	rm -f cyberspace main.o network.o

test: cyberspace
	python3 test-network.py
