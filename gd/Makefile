CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -D_GNU_SOURCE
LDFLAGS ?=
LDLIBS ?= -lncursesw
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

TARGET := gd
SRC := src/main.c src/gdb.c

.PHONY: all clean run check install

all: $(TARGET)

$(TARGET): $(SRC) src/gdb.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

check: $(TARGET)
	$(CC) $(CFLAGS) -g -O0 -o tests/fixture tests/fixture.c
	$(CC) $(CFLAGS) -g -O0 -o tests/pane_fixture tests/pane_fixture.c
	$(CC) $(CFLAGS) -g -O0 -o tests/word_fixture tests/word_fixture.c
	$(CC) $(CFLAGS) -g -O0 -o tests/multi_fixture tests/multi_main.c tests/multi_helper.c
	$(CC) $(CFLAGS) -g -O0 -o tests/address_fixture tests/address_fixture.c
	$(CC) $(CFLAGS) -g -O0 -o tests/frame_vars_fixture tests/frame_vars_fixture.c
	$(CC) $(CFLAGS) -g -O0 -o tests/mutation_fixture tests/mutation_fixture.c
	$(CC) $(CFLAGS) -g -O0 -o tests/catch_fixture tests/catch_fixture.c
	$(CC) $(CFLAGS) -O0 -fno-inline -o tests/nodebug_fixture tests/nodebug_fixture.c
	$(CC) $(CFLAGS) -g -O0 -c -o tests/mixed_debug_main.o tests/mixed_debug_main.c
	$(CC) $(CFLAGS) -g0 -O0 -fno-inline -c -o tests/mixed_nodebug.o tests/mixed_nodebug.c
	$(CC) -o tests/mixed_debug_fixture tests/mixed_debug_main.o tests/mixed_nodebug.o
	$(CC) $(CFLAGS) -o tests/api_smoke tests/api_smoke.c src/gdb.c
	$(CC) $(CFLAGS) -o tests/children_smoke tests/children_smoke.c src/gdb.c
	$(CC) $(CFLAGS) -o tests/multifile_smoke tests/multifile_smoke.c src/gdb.c
	$(CC) $(CFLAGS) -o tests/address_smoke tests/address_smoke.c src/gdb.c
	$(CC) $(CFLAGS) -o tests/frame_vars_smoke tests/frame_vars_smoke.c src/gdb.c
	$(CC) $(CFLAGS) -o tests/assembly_smoke tests/assembly_smoke.c src/gdb.c
	$(CC) $(CFLAGS) -o tests/mixed_mode_smoke tests/mixed_mode_smoke.c src/gdb.c
	$(CC) $(CFLAGS) -o tests/mutation_smoke tests/mutation_smoke.c src/gdb.c
	$(CC) $(CFLAGS) -o tests/catch_smoke tests/catch_smoke.c src/gdb.c
	tests/api_smoke tests/fixture $(CURDIR)/tests/fixture.c
	tests/children_smoke tests/pane_fixture $(CURDIR)/tests/pane_fixture.c
	tests/multifile_smoke tests/multi_fixture $(CURDIR)/tests/multi_main.c $(CURDIR)/tests/multi_helper.c
	tests/address_smoke tests/address_fixture $(CURDIR)/tests/address_fixture.c
	tests/frame_vars_smoke tests/frame_vars_fixture $(CURDIR)/tests/frame_vars_fixture.c
	tests/assembly_smoke tests/nodebug_fixture
	tests/mixed_mode_smoke tests/mixed_debug_fixture $(CURDIR)/tests/mixed_debug_main.c
	tests/mutation_smoke tests/mutation_fixture $(CURDIR)/tests/mutation_fixture.c
	tests/catch_smoke tests/catch_fixture

clean:
	rm -f $(TARGET) tests/fixture tests/pane_fixture tests/word_fixture tests/multi_fixture tests/address_fixture tests/frame_vars_fixture tests/mutation_fixture tests/catch_fixture tests/nodebug_fixture tests/mixed_debug_fixture tests/mixed_debug_main.o tests/mixed_nodebug.o tests/api_smoke tests/children_smoke tests/multifile_smoke tests/address_smoke tests/frame_vars_smoke tests/assembly_smoke tests/mixed_mode_smoke tests/mutation_smoke tests/catch_smoke
