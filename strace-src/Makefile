CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -lncursesw

TARGET := strace-src
SOURCES := main.c parser.c resolve.c resolve_cache.c filters.c trace_file.c fd_tracker.c
OBJECTS := $(SOURCES:.c=.o)

.PHONY: all clean test perf perf-ui example install uninstall

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

%.o: %.c strace_src.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

main.o fd_tracker.o: fd_tracker.h
main.o resolve.o resolve_cache.o: resolve_cache.h

tests/parser_test: tests/parser_test.c parser.c strace_src.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/parser_test.c parser.c

tests/fixture: tests/fixture.c
	$(CC) $(CFLAGS) -g -O0 -fno-omit-frame-pointer -o $@ $<

tests/process_fixture: tests/process_fixture.c
	$(CC) $(CFLAGS) -g -O0 -fno-omit-frame-pointer -pthread -o $@ $<

tests/fd_fixture: tests/fd_fixture.c
	$(CC) $(CFLAGS) -g -O0 -fno-omit-frame-pointer -o $@ $<

tests/resolve_test: tests/resolve_test.c resolve.c resolve_cache.c resolve_cache.h strace_src.h tests/fixture
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/resolve_test.c resolve.c resolve_cache.c

tests/filter_test: tests/filter_test.c filters.c resolve.c resolve_cache.c resolve_cache.h strace_src.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/filter_test.c filters.c resolve.c resolve_cache.c

tests/trace_file_test: tests/trace_file_test.c trace_file.c parser.c resolve.c resolve_cache.c resolve_cache.h strace_src.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/trace_file_test.c trace_file.c parser.c resolve.c resolve_cache.c

tests/fd_tracker_test: tests/fd_tracker_test.c fd_tracker.c filters.c resolve.c resolve_cache.c resolve_cache.h parser.c strace_src.h fd_tracker.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/fd_tracker_test.c fd_tracker.c filters.c resolve.c resolve_cache.c parser.c

test: $(TARGET) tests/parser_test tests/fixture tests/fd_fixture tests/resolve_test tests/filter_test tests/trace_file_test tests/fd_tracker_test
	./tests/parser_test
	./tests/resolve_test
	./tests/filter_test
	./tests/trace_file_test
	./tests/fd_tracker_test
	sh ./tests/language_test.sh ./$(TARGET)
	@echo "All unit tests passed. Run ./strace-src ./tests/fixture in a terminal for the TUI smoke test."

example: $(TARGET)
	$(MAKE) -C examples/shared-library

tests/resolve_performance: tests/resolve_performance.c resolve.c resolve_cache.c resolve_cache.h strace_src.h tests/fixture
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/resolve_performance.c resolve.c resolve_cache.c

perf: tests/resolve_performance
	./tests/resolve_performance

perf-ui: $(TARGET) tests/fd_fixture
	python3 tests/perf_idle.py

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/$(TARGET)

clean:
	rm -f $(TARGET) $(OBJECTS) tests/parser_test tests/resolve_test tests/filter_test tests/trace_file_test tests/fd_tracker_test tests/fixture tests/process_fixture tests/fd_fixture
	rm -f tests/resolve_performance
	$(MAKE) -C examples/shared-library clean
