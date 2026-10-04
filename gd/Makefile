CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -D_GNU_SOURCE
LDFLAGS ?=
LDLIBS ?= -lncursesw
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

TARGET := gd
BACKEND_SRC := src/gdb.c src/gdb_transport.c src/gdb_cache.c src/mi.c
UI_SRC := src/app.c src/app_frame.c src/app_views.c src/app_input.c src/app_panes.c src/app_code.c \
          src/ui_common.c src/source.c src/variables.c src/stops.c src/render.c src/dialogs.c \
          src/condition.c src/project_search.c
SRC := src/main.c $(UI_SRC) $(BACKEND_SRC)
HEADERS := $(wildcard src/*.h)

.PHONY: all clean run check check-tui bench install

all: $(TARGET)

$(TARGET): $(SRC) $(HEADERS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

check: $(TARGET)
	$(CC) $(CFLAGS) -o tests/mi_unit tests/mi_unit.c src/mi.c
	tests/mi_unit
	$(CC) $(CFLAGS) -o tests/ui_unit tests/ui_unit.c $(UI_SRC) $(BACKEND_SRC) $(LDLIBS)
	tests/ui_unit
	$(CC) $(CFLAGS) -g -O0 -o tests/attach_fixture tests/attach_fixture.c
	$(CC) $(CFLAGS) -O0 -o tests/attach_nodebug_fixture tests/attach_fixture.c
	$(CC) $(CFLAGS) -o tests/attach_smoke tests/attach_smoke.c $(BACKEND_SRC)
	tests/attach_smoke tests/attach_fixture tests/attach_nodebug_fixture
	$(CC) $(CFLAGS) -g -O0 -o tests/fixture tests/fixture.c
	$(CC) $(CFLAGS) -g -O0 -o tests/pane_fixture tests/pane_fixture.c
	$(CC) $(CFLAGS) -g -O0 -o tests/word_fixture tests/word_fixture.c
	$(CC) $(CFLAGS) -g -O0 -o tests/multi_fixture tests/multi_main.c tests/multi_helper.c
	$(CC) $(CFLAGS) -g -O0 -o tests/address_fixture tests/address_fixture.c
	$(CC) $(CFLAGS) -g -O0 -o tests/frame_vars_fixture tests/frame_vars_fixture.c
	$(CC) $(CFLAGS) -o tests/scope_smoke tests/scope_smoke.c $(BACKEND_SRC)
	tests/scope_smoke tests/frame_vars_fixture $(CURDIR)/tests/frame_vars_fixture.c
	$(CC) $(CFLAGS) -o tests/performance_smoke tests/performance_smoke.c $(BACKEND_SRC)
	tests/performance_smoke tests/fixture
	$(CC) $(CFLAGS) -g -O0 -o tests/functions_fixture tests/functions_fixture.c
	$(CC) $(CFLAGS) -o tests/cache_smoke tests/cache_smoke.c $(UI_SRC) $(BACKEND_SRC) $(LDLIBS)
	tests/cache_smoke tests/frame_vars_fixture $(CURDIR)/tests/frame_vars_fixture.c tests/functions_fixture
	$(CC) $(CFLAGS) -g -O0 -o tests/mutation_fixture tests/mutation_fixture.c
	$(CC) $(CFLAGS) -g -O0 -o tests/catch_fixture tests/catch_fixture.c
	$(CC) $(CFLAGS) -O0 -fno-inline -o tests/nodebug_fixture tests/nodebug_fixture.c
	$(CC) $(CFLAGS) -g -O0 -c -o tests/mixed_debug_main.o tests/mixed_debug_main.c
	$(CC) $(CFLAGS) -g0 -O0 -fno-inline -c -o tests/mixed_nodebug.o tests/mixed_nodebug.c
	$(CC) -o tests/mixed_debug_fixture tests/mixed_debug_main.o tests/mixed_nodebug.o
	$(CC) $(CFLAGS) -o tests/api_smoke tests/api_smoke.c $(BACKEND_SRC)
	$(CC) $(CFLAGS) -o tests/children_smoke tests/children_smoke.c $(BACKEND_SRC)
	$(CC) $(CFLAGS) -o tests/multifile_smoke tests/multifile_smoke.c $(BACKEND_SRC)
	$(CC) $(CFLAGS) -o tests/address_smoke tests/address_smoke.c $(BACKEND_SRC)
	$(CC) $(CFLAGS) -o tests/frame_vars_smoke tests/frame_vars_smoke.c $(BACKEND_SRC)
	$(CC) $(CFLAGS) -o tests/assembly_smoke tests/assembly_smoke.c $(BACKEND_SRC)
	$(CC) $(CFLAGS) -o tests/mixed_mode_smoke tests/mixed_mode_smoke.c $(BACKEND_SRC)
	$(CC) $(CFLAGS) -o tests/mutation_smoke tests/mutation_smoke.c $(BACKEND_SRC)
	$(CC) $(CFLAGS) -o tests/catch_smoke tests/catch_smoke.c $(BACKEND_SRC)
	tests/api_smoke tests/fixture $(CURDIR)/tests/fixture.c
	tests/children_smoke tests/pane_fixture $(CURDIR)/tests/pane_fixture.c
	tests/multifile_smoke tests/multi_fixture $(CURDIR)/tests/multi_main.c $(CURDIR)/tests/multi_helper.c
	tests/address_smoke tests/address_fixture $(CURDIR)/tests/address_fixture.c
	tests/frame_vars_smoke tests/frame_vars_fixture $(CURDIR)/tests/frame_vars_fixture.c
	tests/assembly_smoke tests/nodebug_fixture
	tests/mixed_mode_smoke tests/mixed_debug_fixture $(CURDIR)/tests/mixed_debug_main.c
	tests/mutation_smoke tests/mutation_fixture $(CURDIR)/tests/mutation_fixture.c
	tests/catch_smoke tests/catch_fixture

check-tui: check
	python3 tests/ui_smoke.py

bench: check
	$(CC) $(CFLAGS) -o tests/perf_refresh tests/perf_refresh.c $(BACKEND_SRC)
	tests/perf_refresh tests/fixture
	tests/perf_refresh tests/fixture source
	python3 tests/perf_idle.py

clean:
	rm -f tests/cache_smoke tests/functions_fixture
	rm -f tests/performance_smoke tests/perf_refresh
	rm -f tests/mi_unit tests/ui_unit
	rm -f tests/scope_smoke
	rm -f tests/attach_fixture tests/attach_nodebug_fixture tests/attach_smoke
	rm -f $(TARGET) tests/fixture tests/pane_fixture tests/word_fixture tests/multi_fixture tests/address_fixture tests/frame_vars_fixture tests/mutation_fixture tests/catch_fixture tests/nodebug_fixture tests/mixed_debug_fixture tests/mixed_debug_main.o tests/mixed_nodebug.o tests/api_smoke tests/children_smoke tests/multifile_smoke tests/address_smoke tests/frame_vars_smoke tests/assembly_smoke tests/mixed_mode_smoke tests/mutation_smoke tests/catch_smoke
