CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -lncursesw

TARGET := strace-src
SOURCES := main.c parser.c resolve.c filters.c trace_file.c fd_tracker.c
OBJECTS := $(SOURCES:.c=.o)

.PHONY: all clean test install uninstall

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

%.o: %.c strace_src.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

tests/parser_test: tests/parser_test.c parser.c strace_src.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/parser_test.c parser.c

tests/fixture: tests/fixture.c
	$(CC) $(CFLAGS) -g -O0 -fno-omit-frame-pointer -o $@ $<

tests/process_fixture: tests/process_fixture.c
	$(CC) $(CFLAGS) -g -O0 -fno-omit-frame-pointer -pthread -o $@ $<

tests/fd_fixture: tests/fd_fixture.c
	$(CC) $(CFLAGS) -g -O0 -fno-omit-frame-pointer -o $@ $<

tests/resolve_test: tests/resolve_test.c resolve.c strace_src.h tests/fixture
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/resolve_test.c resolve.c

tests/filter_test: tests/filter_test.c filters.c resolve.c strace_src.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/filter_test.c filters.c resolve.c

tests/trace_file_test: tests/trace_file_test.c trace_file.c parser.c resolve.c strace_src.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/trace_file_test.c trace_file.c parser.c resolve.c

tests/fd_tracker_test: tests/fd_tracker_test.c fd_tracker.c filters.c resolve.c parser.c strace_src.h fd_tracker.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/fd_tracker_test.c fd_tracker.c filters.c resolve.c parser.c

test: $(TARGET) tests/parser_test tests/fixture tests/fd_fixture tests/resolve_test tests/filter_test tests/trace_file_test tests/fd_tracker_test
	./tests/parser_test
	./tests/resolve_test
	./tests/filter_test
	./tests/trace_file_test
	./tests/fd_tracker_test
	@echo "All unit tests passed. Run ./strace-src ./tests/fixture in a terminal for the TUI smoke test."

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/$(TARGET)

clean:
	rm -f $(TARGET) $(OBJECTS) tests/parser_test tests/resolve_test tests/filter_test tests/trace_file_test tests/fd_tracker_test tests/fixture tests/process_fixture tests/fd_fixture
