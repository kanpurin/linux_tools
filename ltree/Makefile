CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?=
LDFLAGS ?=
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

.PHONY: all clean install uninstall test

all: ltree

SOURCES = ltree.c options.c format.c listing.c

ltree: $(SOURCES) ltree.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -D_DEFAULT_SOURCE -o $@ $(SOURCES) $(LDFLAGS)

install: ltree
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 ltree "$(DESTDIR)$(BINDIR)/ltree"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/ltree"

test: ltree
	sh tests/run.sh ./ltree

clean:
	rm -f ltree
