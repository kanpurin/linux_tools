#define _GNU_SOURCE
#include "strace_src.h"
#include "fd_tracker.h"

#include <ctype.h>
#include <curses.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    char **lines;
    size_t count;
    char *path;
} SourceFile;

typedef enum { INPUT_NONE, INPUT_SEARCH } InputMode;
typedef enum {
    POPUP_NONE,
    POPUP_STACK,
    POPUP_HELP,
    POPUP_SYSCALLS,
    POPUP_PROCESSES,
    POPUP_PROCESS_GRAPH,
    POPUP_FD_INFO
} PopupMode;

typedef struct {
    char *name;
    bool checked;
} SyscallChoice;

typedef struct {
    int pid;
    int tid;
    char name[64];
    bool checked;
} ProcessChoice;

typedef struct {
    int pid;
    int parent_pid;
    char name[64];
    int *tids;
    size_t tid_count;
    size_t tid_cap;
    bool visited;
} ProcessInfo;

typedef struct {
    char text[256];
    bool selected;
} GraphLine;

typedef struct {
    int tid;
    int pid;
    char name[64];
} ThreadIdentity;

enum {
    COLOR_RESOLVED = 1,
    COLOR_CHECKED,
    COLOR_ERROR,
    COLOR_FD,
};

typedef struct {
    TraceModel model;
    int trace_fd;
    pid_t strace_pid;
    bool command_mode;
    bool trace_done;
    int selected;
    int trace_scroll;
    bool follow_latest;
    char *pending;
    size_t pending_len;
    size_t pending_cap;
    char stdout_path[128];
    char stderr_path[128];
    SourceFile source;
    int *visible;
    size_t visible_count;
    size_t visible_cap;
    bool view_dirty;
    char search[256];
    char syscalls[16384];
    bool syscall_filter_active;
    size_t syscall_selected_count;
    char *source_filter_path;
    int source_filter_line;
    InputMode input_mode;
    char input[256];
    size_t input_len;
    const char *save_path;
    int focused_pane;          /* 0: Trace Log, 1: Source */
    PopupMode popup;
    SyscallChoice *choices;
    size_t choice_count;
    size_t choice_cap;
    int choice_cursor;
    int choice_scroll;
    char choice_search[128];
    size_t choice_search_len;
    bool choice_search_input;
    ProcessChoice *process_filter;
    size_t process_filter_count;
    bool process_filter_active;
    ProcessChoice *process_choices;
    size_t process_choice_count;
    size_t process_choice_cap;
    int process_choice_cursor;
    int process_choice_scroll;
    ThreadIdentity *identities;
    size_t identity_count;
    size_t identity_cap;
    FdAnalysis *fd_analysis;
    int forced_event;
    int stack_cursor;
    int source_override_event;
    char *source_override_path;
    char *source_override_function;
    int source_override_line;
    int source_scroll_offset;
} App;

typedef struct {
    bool attach;
    bool replay;
    const char *attach_pid;
    const char *trace_path;
    const char *save_path;
    char **command;
} Cli;

static int spawned_id(const TraceEvent *event, bool *is_thread);

static void clear_source_override(App *app) {
    free(app->source_override_path);
    free(app->source_override_function);
    app->source_override_path = NULL;
    app->source_override_function = NULL;
    app->source_override_line = 0;
    app->source_override_event = -1;
}

static void select_event(App *app, int selected) {
    if (app->selected != selected) {
        clear_source_override(app);
        app->source_scroll_offset = 0;
    }
    app->selected = selected;
}

static FdAnalysis *ensure_fd_analysis(App *app) {
    if (!app->fd_analysis || app->fd_analysis->event_count != app->model.count) {
        fd_analysis_free(app->fd_analysis);
        app->fd_analysis = fd_analysis_build(&app->model);
    }
    return app->fd_analysis;
}

static void usage(FILE *stream) {
    fprintf(stream,
            "Usage:\n"
            "  strace-src [-o FILE.trace] COMMAND [ARG...]\n"
            "  strace-src [-o FILE.trace] -p PID\n"
            "  strace-src FILE.trace\n\n"
            "Keys: Tab, Up/k, Down/j, PgUp, PgDn, g, G, /, e, p, P, r, o, i, s, ?, Esc, q\n");
}

static bool has_trace_suffix(const char *path) {
    size_t length = strlen(path);
    return length >= 6 && strcmp(path + length - 6, ".trace") == 0;
}

static int parse_cli(int argc, char **argv, Cli *cli) {
    int i = 1;
    memset(cli, 0, sizeof(*cli));
    while (i < argc) {
        if (strcmp(argv[i], "-o") == 0) {
            if (cli->save_path || ++i >= argc) return -1;
            cli->save_path = argv[i++];
        } else if (strcmp(argv[i], "-p") == 0) {
            if (++i >= argc || cli->attach) return -1;
            cli->attach = true;
            cli->attach_pid = argv[i++];
            if (i != argc) return -1;
            break;
        } else if (strcmp(argv[i], "--") == 0) {
            if (++i >= argc) return -1;
            cli->command = &argv[i];
            i = argc;
        } else if (argv[i][0] == '-') {
            return -1;
        } else {
            cli->command = &argv[i];
            i = argc;
        }
    }
    if (cli->attach) return 0;
    if (!cli->command) return -1;
    if (!cli->save_path && !cli->command[1] && has_trace_suffix(cli->command[0])) {
        cli->replay = true;
        cli->trace_path = cli->command[0];
    }
    return 0;
}

static void source_file_clear(SourceFile *source) {
    size_t i;
    for (i = 0; i < source->count; ++i) free(source->lines[i]);
    free(source->lines);
    free(source->path);
    memset(source, 0, sizeof(*source));
}

static void source_file_load(SourceFile *source, const char *path) {
    FILE *file;
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t length;
    size_t cap = 0;
    if (source->path && strcmp(source->path, path) == 0) return;
    source_file_clear(source);
    file = fopen(path, "r");
    if (!file) return;
    source->path = strdup(path);
    while ((length = getline(&line, &line_cap, file)) >= 0) {
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = '\0';
        if (source->count == cap) {
            char **next;
            cap = cap ? cap * 2 : 128;
            next = realloc(source->lines, cap * sizeof(*source->lines));
            if (!next) break;
            source->lines = next;
        }
        source->lines[source->count++] = strdup(line);
    }
    free(line);
    fclose(file);
}

static int create_capture_file(char *path, size_t size, const char *kind) {
    int fd;
    snprintf(path, size, "/tmp/strace-src-%s-XXXXXX", kind);
    fd = mkstemp(path);
    if (fd >= 0) fchmod(fd, S_IRUSR | S_IWUSR);
    return fd;
}

static int start_strace(App *app, char **command, const char *attach_pid) {
    int pipefd[2];
    int stdout_fd;
    int stderr_fd;
    pid_t pid;
    stdout_fd = create_capture_file(app->stdout_path, sizeof(app->stdout_path), "stdout");
    stderr_fd = create_capture_file(app->stderr_path, sizeof(app->stderr_path), "stderr");
    if (stdout_fd < 0 || stderr_fd < 0 || pipe(pipefd) < 0) {
        perror("strace-src: setup");
        if (stdout_fd >= 0) close(stdout_fd);
        if (stderr_fd >= 0) close(stderr_fd);
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        perror("strace-src: fork");
        close(stdout_fd);
        close(stderr_fd);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        char **args;
        int i = 0;
        int j;
        int command_count = 0;
        if (command) while (command[command_count]) ++command_count;
        setpgid(0, 0);
        if (dup2(stdout_fd, STDOUT_FILENO) < 0 || dup2(stderr_fd, STDERR_FILENO) < 0)
            _exit(126);
        close(pipefd[0]);
        if (pipefd[1] != 3) {
            if (dup2(pipefd[1], 3) < 0) _exit(126);
            close(pipefd[1]);
        }
        if (stdout_fd != STDOUT_FILENO && stdout_fd != STDERR_FILENO && stdout_fd != 3)
            close(stdout_fd);
        if (stderr_fd != STDOUT_FILENO && stderr_fd != STDERR_FILENO && stderr_fd != 3)
            close(stderr_fd);
        setenv("LC_ALL", "C", 1);
        args = calloc((size_t)command_count + 12, sizeof(*args));
        if (!args) _exit(126);
        args[i++] = "strace";
        args[i++] = "-f";
        args[i++] = "-k";
        args[i++] = "-qq";
        args[i++] = "-s";
        args[i++] = "256";
        args[i++] = "-o";
        args[i++] = "/dev/fd/3";
        if (attach_pid) {
            args[i++] = "-p";
            args[i++] = (char *)attach_pid;
        } else {
            args[i++] = "--";
            for (j = 0; j < command_count; ++j) args[i++] = command[j];
        }
        args[i] = NULL;
        execvp(args[0], args);
        dprintf(STDERR_FILENO, "strace-src: cannot run strace: %s\n", strerror(errno));
        _exit(127);
    }
    setpgid(pid, pid);
    close(stdout_fd);
    close(stderr_fd);
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);
    app->trace_fd = pipefd[0];
    app->strace_pid = pid;
    app->command_mode = attach_pid == NULL;
    return 0;
}

static void pending_append(App *app, const char *data, size_t size) {
    size_t needed = app->pending_len + size + 1;
    if (needed > app->pending_cap) {
        size_t cap = app->pending_cap ? app->pending_cap : 8192;
        char *next;
        while (cap < needed) cap *= 2;
        next = realloc(app->pending, cap);
        if (!next) return;
        app->pending = next;
        app->pending_cap = cap;
    }
    memcpy(app->pending + app->pending_len, data, size);
    app->pending_len += size;
    app->pending[app->pending_len] = '\0';
}

static int process_tgid(int tid, bool *found) {
    char path[64];
    char line[256];
    FILE *file;
    int tgid = tid;
    *found = false;
    snprintf(path, sizeof(path), "/proc/%d/status", tid);
    file = fopen(path, "r");
    if (!file) return tgid;
    while (fgets(line, sizeof(line), file))
        if (sscanf(line, "Tgid:%d", &tgid) == 1) {
            *found = true;
            break;
        }
    fclose(file);
    return tgid;
}

static char *process_comm(int tid) {
    char path[64];
    char name[256];
    FILE *file;
    size_t length;
    snprintf(path, sizeof(path), "/proc/%d/comm", tid);
    file = fopen(path, "r");
    if (!file) return strdup("");
    if (!fgets(name, sizeof(name), file)) name[0] = '\0';
    fclose(file);
    length = strlen(name);
    while (length > 0 && (name[length - 1] == '\n' || name[length - 1] == '\r'))
        name[--length] = '\0';
    return strdup(name);
}

static ThreadIdentity *find_identity(App *app, int tid) {
    size_t i;
    for (i = 0; i < app->identity_count; ++i)
        if (app->identities[i].tid == tid) return &app->identities[i];
    return NULL;
}

static ThreadIdentity *remember_identity(App *app, int tid, int pid, const char *name) {
    ThreadIdentity *identity = find_identity(app, tid);
    ThreadIdentity *next;
    if (!identity) {
        if (app->identity_count == app->identity_cap) {
            size_t cap = app->identity_cap ? app->identity_cap * 2 : 32;
            next = realloc(app->identities, cap * sizeof(*app->identities));
            if (!next) return NULL;
            app->identities = next;
            app->identity_cap = cap;
        }
        identity = &app->identities[app->identity_count++];
        memset(identity, 0, sizeof(*identity));
        identity->tid = tid;
    }
    identity->pid = pid;
    if (name && *name) snprintf(identity->name, sizeof(identity->name), "%s", name);
    return identity;
}

static void feed_line(App *app, const char *line) {
    FeedResult result = trace_model_feed_line(&app->model, line);
    if (result == FEED_NEW_EVENT) {
        TraceEvent *event = &app->model.events[app->model.count - 1];
        ThreadIdentity *known = find_identity(app, event->tid);
        bool tgid_found = false;
        bool is_thread = false;
        int child;
        if (event->tid > 0) {
            int observed_pid = process_tgid(event->tid, &tgid_found);
            event->pid = tgid_found ? observed_pid : known ? known->pid : event->tid;
            event->comm = process_comm(event->tid);
            if ((!event->comm || !event->comm[0]) && known && known->name[0]) {
                free(event->comm);
                event->comm = strdup(known->name);
            }
        }
        if (!event->comm) event->comm = strdup("");
        remember_identity(app, event->tid, event->pid, event->comm);
        child = spawned_id(event, &is_thread);
        if (child > 0)
            remember_identity(app, child, is_thread ? event->pid : child, NULL);
        fd_analysis_free(app->fd_analysis);
        app->fd_analysis = NULL;
        app->view_dirty = true;
    } else if (result == FEED_STACK_ADDED && app->source_filter_path) {
        app->view_dirty = true;
    }
}

static void process_complete_lines(App *app, bool flush) {
    size_t start = 0;
    size_t i;
    for (i = 0; i < app->pending_len; ++i) {
        if (app->pending[i] == '\n') {
            app->pending[i] = '\0';
            feed_line(app, app->pending + start);
            app->pending[i] = '\n';
            start = i + 1;
        }
    }
    if (flush && start < app->pending_len) {
        app->pending[app->pending_len] = '\0';
        feed_line(app, app->pending + start);
        start = app->pending_len;
    }
    if (start > 0) {
        memmove(app->pending, app->pending + start, app->pending_len - start);
        app->pending_len -= start;
        if (app->pending) app->pending[app->pending_len] = '\0';
    }
}

static void read_trace(App *app) {
    char buffer[16384];
    ssize_t got;
    if (app->trace_done) return;
    for (;;) {
        got = read(app->trace_fd, buffer, sizeof(buffer));
        if (got > 0) {
            pending_append(app, buffer, (size_t)got);
            process_complete_lines(app, false);
        } else if (got == 0) {
            process_complete_lines(app, true);
            close(app->trace_fd);
            app->trace_fd = -1;
            app->trace_done = true;
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            app->trace_done = true;
            break;
        } else {
            break;
        }
    }
}

static bool event_visible(App *app, TraceEvent *event) {
    size_t i;
    if (app->process_filter_active) {
        for (i = 0; i < app->process_filter_count; ++i)
            if (app->process_filter[i].pid == event->pid &&
                app->process_filter[i].tid == event->tid)
                break;
        if (i == app->process_filter_count) return false;
    }
    if (app->syscall_filter_active && app->syscall_selected_count == 0) return false;
    return trace_event_matches(event, app->search,
                               app->syscall_filter_active ? app->syscalls : NULL,
                               app->source_filter_path, app->source_filter_line);
}

static int selected_visible_position(const App *app) {
    size_t i;
    for (i = 0; i < app->visible_count; ++i)
        if (app->visible[i] == app->selected) return (int)i;
    return -1;
}

static void rebuild_visible(App *app) {
    size_t i;
    int old_position;
    if (!app->view_dirty) return;
    app->visible_count = 0;
    for (i = 0; i < app->model.count; ++i) {
        if ((int)i != app->forced_event && !event_visible(app, &app->model.events[i])) continue;
        if (app->visible_count == app->visible_cap) {
            size_t cap = app->visible_cap ? app->visible_cap * 2 : 256;
            int *next = realloc(app->visible, cap * sizeof(*app->visible));
            if (!next) break;
            app->visible = next;
            app->visible_cap = cap;
        }
        app->visible[app->visible_count++] = (int)i;
    }
    old_position = selected_visible_position(app);
    if (app->visible_count == 0) {
        select_event(app, -1);
        app->trace_scroll = 0;
    } else if (app->follow_latest || old_position < 0) {
        select_event(app, app->visible[app->visible_count - 1]);
    }
    app->view_dirty = false;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void add_clipped(WINDOW *window, int y, int x, const char *text, int width) {
    if (width > 0) mvwaddnstr(window, y, x, text, width);
}

static void append_title(char *title, size_t size, const char *value) {
    size_t used = strlen(title);
    if (used < size) snprintf(title + used, size - used, " [%s]", value);
}

static void trace_title(App *app, char *title, size_t size) {
    char source[512];
    if (app->input_mode != INPUT_NONE) {
        snprintf(title, size, " Search: %s_ ", app->input);
        return;
    }
    snprintf(title, size, " Trace Log");
    if (app->syscall_filter_active) {
        if (app->syscall_selected_count == 1) {
            char single[256];
            snprintf(single, sizeof(single), "%.255s", app->syscalls);
            append_title(title, size, single);
        } else {
            char count[64];
            snprintf(count, sizeof(count), "syscall:%zu", app->syscall_selected_count);
            append_title(title, size, count);
        }
    }
    if (app->search[0]) append_title(title, size, app->search);
    if (app->process_filter_active) {
        char process[64];
        if (app->process_filter_count == 1) {
            ProcessChoice *choice = &app->process_filter[0];
            if (choice->pid == choice->tid)
                snprintf(process, sizeof(process), "PID %d", choice->pid);
            else
                snprintf(process, sizeof(process), "TID %d", choice->tid);
        } else {
            snprintf(process, sizeof(process), "process:%zu", app->process_filter_count);
        }
        append_title(title, size, process);
    }
    if (app->source_filter_path) {
        snprintf(source, sizeof(source), "%s:%d", base_name(app->source_filter_path),
                 app->source_filter_line);
        append_title(title, size, source);
    }
    if (strlen(title) + 2 < size) strcat(title, " ");
}

static int selected_fd_object(App *app) {
    const FdEventInfo *info;
    if (app->selected < 0 || !ensure_fd_analysis(app)) return -1;
    info = fd_analysis_event(app->fd_analysis, (size_t)app->selected);
    if (!info || info->primary_ref < 0 ||
        (size_t)info->primary_ref >= info->ref_count)
        return -1;
    return info->refs[info->primary_ref].object_id;
}

static bool related_fd_position(const FdEventInfo *info, int object_id, size_t offset) {
    size_t i;
    if (!info || object_id < 0) return false;
    for (i = 0; i < info->ref_count; ++i) {
        const FdRef *ref = &info->refs[i];
        if (ref->object_id == object_id && offset >= ref->offset &&
            offset < ref->offset + ref->length)
            return true;
    }
    return false;
}

static void draw_trace_text(int y, int x, const TraceEvent *event,
                            const FdEventInfo *fd_info, int object_id,
                            int width, attr_t base_attr, attr_t error_attr,
                            attr_t fd_attr) {
    size_t length = strlen(event->text);
    size_t error_offset = trace_event_error(event) ?
                          (size_t)(trace_event_error(event) - event->text) : length;
    size_t start = 0;
    if ((size_t)width < length) length = (size_t)width;
    while (start < length) {
        bool fd = related_fd_position(fd_info, object_id, start);
        attr_t attr = fd ? fd_attr : start >= error_offset ? error_attr : base_attr;
        size_t end = start + 1;
        while (end < length) {
            bool next_fd = related_fd_position(fd_info, object_id, end);
            attr_t next = next_fd ? fd_attr : end >= error_offset ? error_attr : base_attr;
            if (next != attr) break;
            ++end;
        }
        if (attr) wattron(stdscr, attr);
        mvwaddnstr(stdscr, y, x + (int)start, event->text + start,
                   (int)(end - start));
        if (attr) wattroff(stdscr, attr);
        start = end;
    }
}

static void draw_trace(App *app, int height, int width) {
    int content = height - 2;
    int selected_position;
    int max_scroll;
    int i;
    int object_id = selected_fd_object(app);
    char title[1024];
    box(stdscr, 0, 0);
    trace_title(app, title, sizeof(title));
    if (app->focused_pane == 0) wattron(stdscr, A_BOLD);
    add_clipped(stdscr, 0, 2, title, width - 4);
    if (app->focused_pane == 0) wattroff(stdscr, A_BOLD);
    if (content <= 0) return;
    selected_position = selected_visible_position(app);
    if (selected_position >= 0) {
        if (selected_position < app->trace_scroll) app->trace_scroll = selected_position;
        if (selected_position >= app->trace_scroll + content)
            app->trace_scroll = selected_position - content + 1;
    }
    max_scroll = (int)app->visible_count - content;
    if (max_scroll < 0) max_scroll = 0;
    if (app->trace_scroll > max_scroll) app->trace_scroll = max_scroll;
    for (i = 0; i < content && app->trace_scroll + i < (int)app->visible_count; ++i) {
        int model_index = app->visible[app->trace_scroll + i];
        TraceEvent *event = &app->model.events[model_index];
        bool selected = model_index == app->selected;
        bool resolved = trace_event_resolve(event);
        const FdEventInfo *fd_info = fd_analysis_event(app->fd_analysis, (size_t)model_index);
        attr_t base_attr = selected ? (A_REVERSE | A_BOLD) :
                           resolved ? (has_colors() ? COLOR_PAIR(COLOR_RESOLVED) : A_BOLD) : 0;
        attr_t error_attr = A_BOLD | (has_colors() ? COLOR_PAIR(COLOR_ERROR) : 0) |
                            (selected ? A_REVERSE : 0);
        attr_t fd_attr = A_BOLD | (has_colors() ? COLOR_PAIR(COLOR_FD) : A_UNDERLINE) |
                         (selected ? A_REVERSE : 0);
        int text_width = width - 3;
        if (base_attr) wattron(stdscr, base_attr);
        mvwaddch(stdscr, i + 1, 1, selected ? '>' : ' ');
        if (base_attr) wattroff(stdscr, base_attr);
        draw_trace_text(i + 1, 2, event, fd_info, object_id, text_width,
                        base_attr, error_attr, fd_attr);
    }
}

static void draw_source_unavailable(int top, int height, int width) {
    const char *message = "Source unavailable";
    int y = top + height / 2;
    int x = (width - (int)strlen(message)) / 2;
    if (x < 1) x = 1;
    add_clipped(stdscr, y, x, message, width - x - 1);
}

static bool displayed_source(App *app, TraceEvent *event, const char **path,
                             const char **function, int *line) {
    if (app->source_override_event == app->selected && app->source_override_path) {
        *path = app->source_override_path;
        *function = app->source_override_function;
        *line = app->source_override_line;
        return true;
    }
    if (!trace_event_resolve(event)) return false;
    *path = event->source_path;
    *function = event->source_function;
    *line = event->source_line;
    return true;
}

static void draw_source(App *app, int top, int height, int width) {
    TraceEvent *event;
    const char *source_path;
    const char *source_function;
    int source_line;
    int code_rows;
    int start;
    int i;
    int digits;
    char header[1024];
    char number[64];
    mvhline(top, 0, ACS_HLINE, width);
    mvaddch(top, 0, ACS_LTEE);
    mvaddch(top, width - 1, ACS_RTEE);
    mvaddch(top + height - 1, 0, ACS_LLCORNER);
    mvhline(top + height - 1, 1, ACS_HLINE, width - 2);
    mvaddch(top + height - 1, width - 1, ACS_LRCORNER);
    mvvline(top + 1, 0, ACS_VLINE, height - 2);
    mvvline(top + 1, width - 1, ACS_VLINE, height - 2);
    if (app->focused_pane == 1) wattron(stdscr, A_BOLD);
    add_clipped(stdscr, top, 2, " Source ", width - 4);
    if (app->focused_pane == 1) wattroff(stdscr, A_BOLD);
    if (app->selected < 0 || app->selected >= (int)app->model.count) {
        draw_source_unavailable(top, height, width);
        return;
    }
    event = &app->model.events[app->selected];
    if (!displayed_source(app, event, &source_path, &source_function, &source_line)) {
        draw_source_unavailable(top, height, width);
        return;
    }
    source_file_load(&app->source, source_path);
    if (!app->source.path || source_line > (int)app->source.count) {
        draw_source_unavailable(top, height, width);
        return;
    }
    snprintf(header, sizeof(header), "%s:%d  %s%s", base_name(source_path),
             source_line, source_function,
             strchr(source_function, '(') ? "" : "()");
    add_clipped(stdscr, top + 1, 2, header, width - 3);
    code_rows = height - 4;
    if (code_rows <= 0) return;
    {
        int centered = source_line - 1 - code_rows / 2;
        int max_start = (int)app->source.count - code_rows;
        long requested;
        if (centered < 0) centered = 0;
        if (max_start < 0) max_start = 0;
        if (centered > max_start) centered = max_start;
        requested = (long)centered + app->source_scroll_offset;
        if (requested < 0) requested = 0;
        if (requested > max_start) requested = max_start;
        start = (int)requested;
        app->source_scroll_offset = start - centered;
    }
    digits = snprintf(NULL, 0, "%zu", app->source.count);
    for (i = 0; i < code_rows && start + i < (int)app->source.count; ++i) {
        int line_no = start + i + 1;
        int prefix_len;
        snprintf(number, sizeof(number), "%c%*d | ",
                 line_no == source_line ? '>' : ' ', digits, line_no);
        prefix_len = (int)strlen(number);
        add_clipped(stdscr, top + 3 + i, 1, number, width - 2);
        add_clipped(stdscr, top + 3 + i, 1 + prefix_len,
                    app->source.lines[start + i], width - prefix_len - 2);
    }
}

static void draw(App *app) {
    int rows;
    int cols;
    int trace_height;
    int source_height;
    rebuild_visible(app);
    getmaxyx(stdscr, rows, cols);
    erase();
    if (rows < 10 || cols < 30) {
        add_clipped(stdscr, 0, 0, "Terminal too small (minimum 30x10)", cols);
        wnoutrefresh(stdscr);
        return;
    }
    trace_height = rows / 2;
    source_height = rows - trace_height + 1;
    draw_trace(app, trace_height, cols);
    draw_source(app, trace_height - 1, source_height, cols);
    wnoutrefresh(stdscr);
}

static void navigate(App *app, int key, int page) {
    int position;
    int last;
    if (app->forced_event >= 0) {
        app->forced_event = -1;
        app->view_dirty = true;
    }
    rebuild_visible(app);
    last = (int)app->visible_count - 1;
    if (last < 0) return;
    position = selected_visible_position(app);
    if (position < 0) position = last;
    switch (key) {
        case KEY_UP:
        case 'k': if (position > 0) --position; break;
        case KEY_DOWN:
        case 'j': if (position < last) ++position; break;
        case KEY_PPAGE:
            position -= page;
            if (position < 0) position = 0;
            break;
        case KEY_NPAGE:
            position += page;
            if (position > last) position = last;
            break;
        case 'g': position = 0; break;
        case 'G': position = last; break;
        default: return;
    }
    select_event(app, app->visible[position]);
    app->follow_latest = position == last;
}

static void scroll_source(App *app, int key, int page) {
    long next = app->source_scroll_offset;
    switch (key) {
        case KEY_UP:
        case 'k': --next; break;
        case KEY_DOWN:
        case 'j': ++next; break;
        case KEY_PPAGE: next -= page; break;
        case KEY_NPAGE: next += page; break;
        case 'g': next = INT_MIN / 4; break;
        case 'G': next = INT_MAX / 4; break;
        default: return;
    }
    if (next < INT_MIN / 4) next = INT_MIN / 4;
    if (next > INT_MAX / 4) next = INT_MAX / 4;
    app->source_scroll_offset = (int)next;
}

static void begin_input(App *app, InputMode mode) {
    app->input_mode = mode;
    snprintf(app->input, sizeof(app->input), "%s", app->search);
    app->input_len = strlen(app->input);
}

static void accept_input(App *app) {
    snprintf(app->search, sizeof(app->search), "%s", app->input);
    app->input_mode = INPUT_NONE;
    app->view_dirty = true;
    app->follow_latest = true;
}

static void handle_input(App *app, int key) {
    if (key == 27) {
        app->input_mode = INPUT_NONE;
    } else if (key == '\n' || key == '\r' || key == KEY_ENTER) {
        accept_input(app);
    } else if (key == KEY_BACKSPACE || key == 127 || key == 8) {
        if (app->input_len > 0) app->input[--app->input_len] = '\0';
    } else if (key == 21) {
        app->input_len = 0;
        app->input[0] = '\0';
    } else if (key >= 32 && key <= 255 && app->input_len + 1 < sizeof(app->input)) {
        app->input[app->input_len++] = (char)key;
        app->input[app->input_len] = '\0';
    }
}

static void reverse_from_selected(App *app) {
    TraceEvent *event;
    const char *source_path;
    const char *source_function;
    int source_line;
    char *path;
    if (app->selected < 0 || app->selected >= (int)app->model.count) return;
    event = &app->model.events[app->selected];
    if (!displayed_source(app, event, &source_path, &source_function, &source_line) ||
        source_line <= 0 || access(source_path, R_OK) != 0)
        return;
    (void)source_function;
    path = strdup(source_path);
    if (!path) return;
    free(app->source_filter_path);
    app->source_filter_path = path;
    app->source_filter_line = source_line;
    app->view_dirty = true;
}

static void clear_filters(App *app) {
    app->search[0] = '\0';
    app->syscalls[0] = '\0';
    app->syscall_filter_active = false;
    app->syscall_selected_count = 0;
    free(app->process_filter);
    app->process_filter = NULL;
    app->process_filter_count = 0;
    app->process_filter_active = false;
    free(app->source_filter_path);
    app->source_filter_path = NULL;
    app->source_filter_line = 0;
    app->forced_event = -1;
    app->view_dirty = true;
    app->follow_latest = true;
}

static void clear_choices(App *app) {
    size_t i;
    for (i = 0; i < app->choice_count; ++i) free(app->choices[i].name);
    free(app->choices);
    app->choices = NULL;
    app->choice_count = 0;
    app->choice_cap = 0;
    app->choice_cursor = 0;
    app->choice_scroll = 0;
    app->choice_search[0] = '\0';
    app->choice_search_len = 0;
    app->choice_search_input = false;
}

static bool syscall_selected(const App *app, const char *name) {
    const char *p = app->syscalls;
    size_t length = strlen(name);
    while (*p) {
        const char *end = strchr(p, ',');
        size_t item_length = end ? (size_t)(end - p) : strlen(p);
        if (item_length == length && strncmp(p, name, length) == 0) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}

static int compare_choices(const void *left, const void *right) {
    const SyscallChoice *a = left;
    const SyscallChoice *b = right;
    return strcmp(a->name, b->name);
}

static void open_syscall_menu(App *app) {
    size_t i;
    char name[256];
    clear_choices(app);
    for (i = 0; i < app->model.count; ++i) {
        size_t j;
        SyscallChoice *next;
        if (!trace_event_syscall_name(&app->model.events[i], name, sizeof(name))) continue;
        for (j = 0; j < app->choice_count; ++j)
            if (strcmp(app->choices[j].name, name) == 0) break;
        if (j < app->choice_count) continue;
        if (app->choice_count == app->choice_cap) {
            size_t cap = app->choice_cap ? app->choice_cap * 2 : 128;
            next = realloc(app->choices, cap * sizeof(*app->choices));
            if (!next) break;
            app->choices = next;
            app->choice_cap = cap;
        }
        app->choices[app->choice_count].name = strdup(name);
        if (!app->choices[app->choice_count].name) break;
        app->choices[app->choice_count].checked =
            !app->syscall_filter_active || syscall_selected(app, name);
        app->choice_count++;
    }
    if (app->choice_count > 1)
        qsort(app->choices, app->choice_count, sizeof(*app->choices), compare_choices);
    app->popup = POPUP_SYSCALLS;
}

static bool choice_matches(const App *app, size_t index) {
    return !app->choice_search[0] ||
           strstr(app->choices[index].name, app->choice_search) != NULL;
}

static int visible_choice_count(const App *app) {
    size_t i;
    int count = 0;
    for (i = 0; i < app->choice_count; ++i)
        if (choice_matches(app, i)) ++count;
    return count;
}

static int choice_index_at(const App *app, int visible_index) {
    size_t i;
    int position = 0;
    for (i = 0; i < app->choice_count; ++i) {
        if (!choice_matches(app, i)) continue;
        if (position == visible_index) return (int)i;
        ++position;
    }
    return -1;
}

static void apply_syscall_menu(App *app) {
    size_t i;
    size_t used = 0;
    size_t selected = 0;
    app->syscalls[0] = '\0';
    for (i = 0; i < app->choice_count; ++i) {
        int written;
        if (!app->choices[i].checked) continue;
        written = snprintf(app->syscalls + used, sizeof(app->syscalls) - used,
                           "%s%s", used ? "," : "", app->choices[i].name);
        if (written < 0 || (size_t)written >= sizeof(app->syscalls) - used) break;
        used += (size_t)written;
        ++selected;
    }
    app->syscall_selected_count = selected;
    app->syscall_filter_active = selected != app->choice_count;
    if (!app->syscall_filter_active) app->syscalls[0] = '\0';
    app->view_dirty = true;
    app->follow_latest = true;
    app->popup = POPUP_NONE;
    clear_choices(app);
}

static void clear_process_choices(App *app) {
    free(app->process_choices);
    app->process_choices = NULL;
    app->process_choice_count = 0;
    app->process_choice_cap = 0;
    app->process_choice_cursor = 0;
    app->process_choice_scroll = 0;
}

static bool process_is_selected(const App *app, int pid, int tid) {
    size_t i;
    for (i = 0; i < app->process_filter_count; ++i)
        if (app->process_filter[i].pid == pid && app->process_filter[i].tid == tid)
            return true;
    return false;
}

static void event_name(const TraceEvent *event, char *name, size_t size) {
    char syscall[64];
    const char *quote;
    const char *end;
    const char *base;
    size_t length;
    if (event->comm && event->comm[0]) {
        snprintf(name, size, "%s", event->comm);
    } else if (!name[0]) {
        snprintf(name, size, "process");
    }
    if (!trace_event_syscall_name(event, syscall, sizeof(syscall)) ||
        strcmp(syscall, "execve") != 0)
        return;
    quote = strchr(event->text, '"');
    if (!quote || !(end = strchr(quote + 1, '"'))) return;
    base = quote + 1;
    while (base < end) {
        const char *slash = memchr(base, '/', (size_t)(end - base));
        if (!slash) break;
        base = slash + 1;
    }
    length = (size_t)(end - base);
    if (length >= size) length = size - 1;
    memcpy(name, base, length);
    name[length] = '\0';
}

static int compare_process_choices(const void *left, const void *right) {
    const ProcessChoice *a = left;
    const ProcessChoice *b = right;
    if (a->pid != b->pid) return a->pid < b->pid ? -1 : 1;
    if (a->tid != b->tid) return a->tid < b->tid ? -1 : 1;
    return 0;
}

static void open_process_menu(App *app) {
    size_t i;
    clear_process_choices(app);
    for (i = 0; i < app->model.count; ++i) {
        TraceEvent *event = &app->model.events[i];
        size_t j;
        ProcessChoice *next;
        for (j = 0; j < app->process_choice_count; ++j)
            if (app->process_choices[j].pid == event->pid &&
                app->process_choices[j].tid == event->tid)
                break;
        if (j == app->process_choice_count) {
            if (app->process_choice_count == app->process_choice_cap) {
                size_t cap = app->process_choice_cap ? app->process_choice_cap * 2 : 32;
                next = realloc(app->process_choices, cap * sizeof(*app->process_choices));
                if (!next) break;
                app->process_choices = next;
                app->process_choice_cap = cap;
            }
            memset(&app->process_choices[j], 0, sizeof(app->process_choices[j]));
            app->process_choices[j].pid = event->pid;
            app->process_choices[j].tid = event->tid;
            snprintf(app->process_choices[j].name,
                     sizeof(app->process_choices[j].name), "process");
            app->process_choices[j].checked = !app->process_filter_active ||
                process_is_selected(app, event->pid, event->tid);
            app->process_choice_count++;
        }
        event_name(event, app->process_choices[j].name,
                   sizeof(app->process_choices[j].name));
    }
    if (app->process_choice_count > 1)
        qsort(app->process_choices, app->process_choice_count,
              sizeof(*app->process_choices), compare_process_choices);
    app->popup = POPUP_PROCESSES;
}

static void apply_process_menu(App *app) {
    size_t i;
    size_t count = 0;
    bool active;
    ProcessChoice *selected = NULL;
    for (i = 0; i < app->process_choice_count; ++i)
        if (app->process_choices[i].checked) ++count;
    active = count != app->process_choice_count;
    if (count && active) {
        size_t out = 0;
        selected = calloc(count, sizeof(*selected));
        if (!selected) return;
        for (i = 0; i < app->process_choice_count; ++i)
            if (app->process_choices[i].checked) selected[out++] = app->process_choices[i];
    }
    free(app->process_filter);
    app->process_filter = selected;
    app->process_filter_count = active ? count : 0;
    app->process_filter_active = active;
    app->view_dirty = true;
    app->follow_latest = true;
    app->popup = POPUP_NONE;
    clear_process_choices(app);
}

static WINDOW *create_popup(const char *title, int wanted_height, int wanted_width) {
    int rows;
    int cols;
    int height;
    int width;
    int x;
    int y;
    WINDOW *window;
    getmaxyx(stdscr, rows, cols);
    height = wanted_height < rows - 2 ? wanted_height : rows - 2;
    width = wanted_width < cols - 2 ? wanted_width : cols - 2;
    if (height < 5) height = rows;
    if (width < 20) width = cols;
    y = (rows - height) / 2;
    x = (cols - width) / 2;
    window = newwin(height, width, y, x);
    if (!window) return NULL;
    keypad(window, TRUE);
    box(window, 0, 0);
    if (width > (int)strlen(title) + 4)
        mvwprintw(window, 0, (width - (int)strlen(title) - 2) / 2, " %s ", title);
    return window;
}

static void draw_stack_popup(App *app) {
    TraceEvent *event;
    WINDOW *window;
    int rows;
    int cols;
    int height;
    int width;
    int content;
    int adopted;
    int start = 0;
    int i;
    if (app->selected < 0 || app->selected >= (int)app->model.count) return;
    event = &app->model.events[app->selected];
    getmaxyx(stdscr, rows, cols);
    height = (int)event->frame_count + 4;
    if (height < 7) height = 7;
    if (height > rows - 4) height = rows - 4;
    width = cols - 8;
    if (width > 100) width = 100;
    window = create_popup("Stack", height, width);
    if (!window) return;
    getmaxyx(window, height, width);
    content = height - 3;
    adopted = trace_event_source_frame(event);
    if (app->stack_cursor >= (int)event->frame_count)
        app->stack_cursor = (int)event->frame_count - 1;
    if (app->stack_cursor < 0) app->stack_cursor = 0;
    if ((int)event->frame_count > content && app->stack_cursor >= content / 2)
        start = app->stack_cursor - content / 2;
    if (start + content > (int)event->frame_count)
        start = (int)event->frame_count - content;
    if (start < 0) start = 0;
    for (i = 0; i < content && start + i < (int)event->frame_count; ++i) {
        int index = start + i;
        StackFrame *frame = &event->frames[index];
        const char *module = frame->module[0] == '.' ? frame->module : base_name(frame->module);
        char line[2048];
        bool resolved = stack_frame_resolve(frame);
        snprintf(line, sizeof(line), "%c #%d %-20s %s%s%s",
                 index == app->stack_cursor ? '>' : ' ', index, module,
                 frame->function, frame->offset[0] ? "+" : "", frame->offset);
        if (resolved) {
            size_t used = strlen(line);
            if (used < sizeof(line))
                snprintf(line + used, sizeof(line) - used, "  %s:%d",
                         base_name(frame->source_path), frame->source_line);
        }
        if (resolved) wattron(window, A_BOLD | (has_colors() ? COLOR_PAIR(COLOR_RESOLVED) : 0));
        if (index == app->stack_cursor) wattron(window, A_REVERSE);
        mvwaddnstr(window, i + 1, 1, line, width - 2);
        if (index == app->stack_cursor) wattroff(window, A_REVERSE);
        if (resolved) wattroff(window, A_BOLD | (has_colors() ? COLOR_PAIR(COLOR_RESOLVED) : 0));
    }
    (void)adopted;
    mvwaddnstr(window, height - 2, 2,
               "Up/Down Select   Enter Use Source   Esc Close", width - 4);
    wnoutrefresh(window);
    delwin(window);
}

static void draw_fd_info_popup(App *app) {
    const FdEventInfo *info;
    const FdObject *object;
    const FdRef *ref;
    TraceEvent *event;
    WINDOW *window;
    int height;
    int width;
    int row = 1;
    int creator;
    if (app->selected < 0 || !ensure_fd_analysis(app)) return;
    info = fd_analysis_event(app->fd_analysis, (size_t)app->selected);
    if (!info || info->primary_ref < 0 || (size_t)info->primary_ref >= info->ref_count)
        return;
    ref = &info->refs[info->primary_ref];
    object = fd_analysis_object(app->fd_analysis, ref->object_id);
    event = &app->model.events[app->selected];
    window = create_popup("FD Info", 18, 88);
    if (!window) return;
    getmaxyx(window, height, width);
    if (event->pid == event->tid)
        mvwprintw(window, row++, 2, "Process   %s[%d]", event->comm && event->comm[0] ? event->comm : "process", event->pid);
    else
        mvwprintw(window, row++, 2, "Process   %s[%d]  TID %d", event->comm && event->comm[0] ? event->comm : "thread", event->pid, event->tid);
    mvwprintw(window, row++, 2, "FD        %d", ref->fd);
    if (object && object->target && object->target[0]) {
        ++row;
        mvwaddnstr(window, row++, 2, "Target", width - 4);
        mvwaddnstr(window, row++, 4, object->target, width - 6);
    }
    if (object && object->origin_event >= 0 && row + 2 < height - 2) {
        TraceEvent *origin = &app->model.events[object->origin_event];
        char line[4096];
        ++row;
        mvwaddnstr(window, row++, 2, "Origin", width - 4);
        snprintf(line, sizeof(line), "#%d %s", object->origin_event + 1, origin->text);
        mvwaddnstr(window, row++, 4, line, width - 6);
    }
    creator = info->creator_event;
    if (object && creator >= 0 && creator != object->origin_event && row + 2 < height - 2) {
        ++row;
        mvwaddnstr(window, row++, 2, "History", width - 4);
        while (creator >= 0 && creator != object->origin_event && row < height - 3) {
            const FdEventInfo *creator_info = fd_analysis_event(app->fd_analysis, (size_t)creator);
            char line[4096];
            snprintf(line, sizeof(line), "#%d %s", creator + 1,
                     app->model.events[creator].text);
            mvwaddnstr(window, row++, 4, line, width - 6);
            if (!creator_info || creator_info->jump_event == creator) break;
            creator = creator_info->jump_event;
        }
    }
    if (row + 2 < height) {
        ++row;
        mvwaddnstr(window, row++, 2, "State", width - 4);
        wattron(window, A_BOLD);
        mvwaddnstr(window, row, 4, info->open_after ? "OPEN" : "CLOSED", width - 6);
        wattroff(window, A_BOLD);
    }
    wnoutrefresh(window);
    delwin(window);
}

static void draw_help_popup(void) {
    static const char *lines[] = {
        "Navigation", "  Tab         Switch pane", "  Up / k      Trace previous / Source scroll up",
        "  Down / j    Trace next / Source scroll down", "  PgUp/PgDn   Move focused pane one page",
        "  g / G       First/last in focused pane", "", "Search / Filter",
        "  /           Search trace", "  e           Syscall filter menu",
        "  p           Process/thread filter",
        "  r           Traces from current source line", "  Esc         Clear filters",
        "", "Detail", "  o           Jump to FD origin", "  i           Show FD info",
        "  s           Select stack source", "  P           Process graph", "", "Other",
        "  ?           Help", "  q           Quit"
    };
    WINDOW *window = create_popup("Help", (int)(sizeof(lines) / sizeof(lines[0])) + 2, 68);
    int height;
    int width;
    size_t i;
    if (!window) return;
    getmaxyx(window, height, width);
    for (i = 0; i < sizeof(lines) / sizeof(lines[0]) && (int)i + 1 < height - 1; ++i) {
        if (lines[i][0] && !isspace((unsigned char)lines[i][0])) wattron(window, A_BOLD);
        mvwaddnstr(window, (int)i + 1, 2, lines[i], width - 4);
        wattroff(window, A_BOLD);
    }
    wnoutrefresh(window);
    delwin(window);
}

static void draw_syscall_popup(App *app) {
    WINDOW *window;
    int rows;
    int cols;
    int visible_count = visible_choice_count(app);
    int height;
    int width;
    int item_rows;
    int max_scroll;
    int i;
    getmaxyx(stdscr, rows, cols);
    height = visible_count + 6;
    if (height < 12) height = 12;
    if (height > rows - 4) height = rows - 4;
    width = cols - 8;
    if (width > 64) width = 64;
    window = create_popup("Syscall Filter", height, width);
    if (!window) return;
    getmaxyx(window, height, width);
    mvwprintw(window, 1, 2, "Search: %s%s", app->choice_search,
              app->choice_search_input ? "_" : "");
    item_rows = height - 5;
    if (app->choice_cursor >= visible_count) app->choice_cursor = visible_count - 1;
    if (app->choice_cursor < 0) app->choice_cursor = 0;
    if (app->choice_cursor < app->choice_scroll) app->choice_scroll = app->choice_cursor;
    if (app->choice_cursor >= app->choice_scroll + item_rows)
        app->choice_scroll = app->choice_cursor - item_rows + 1;
    max_scroll = visible_count - item_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (app->choice_scroll > max_scroll) app->choice_scroll = max_scroll;
    for (i = 0; i < item_rows && app->choice_scroll + i < visible_count; ++i) {
        int visible_index = app->choice_scroll + i;
        int choice_index = choice_index_at(app, visible_index);
        SyscallChoice *choice = &app->choices[choice_index];
        if (visible_index == app->choice_cursor) wattron(window, A_REVERSE | A_BOLD);
        else if (choice->checked) wattron(window, COLOR_PAIR(COLOR_CHECKED));
        mvwprintw(window, i + 3, 2, "[%c] %s", choice->checked ? 'x' : ' ', choice->name);
        if (visible_index == app->choice_cursor) wattroff(window, A_REVERSE | A_BOLD);
        else if (choice->checked) wattroff(window, COLOR_PAIR(COLOR_CHECKED));
    }
    if (visible_count == 0) mvwaddstr(window, 3, 2, "No matching syscalls");
    mvwaddnstr(window, height - 2, 2,
               "Up/Down Move  Space Toggle  Enter Apply  Esc Cancel", width - 4);
    wnoutrefresh(window);
    delwin(window);
}

static void draw_process_popup(App *app) {
    WINDOW *window;
    int rows;
    int cols;
    int height;
    int width;
    int item_rows;
    int max_scroll;
    int i;
    getmaxyx(stdscr, rows, cols);
    height = (int)app->process_choice_count + 4;
    if (height < 9) height = 9;
    if (height > rows - 4) height = rows - 4;
    width = cols - 8;
    if (width > 76) width = 76;
    window = create_popup("Process / Thread Filter", height, width);
    if (!window) return;
    getmaxyx(window, height, width);
    item_rows = height - 4;
    if (app->process_choice_cursor >= (int)app->process_choice_count)
        app->process_choice_cursor = (int)app->process_choice_count - 1;
    if (app->process_choice_cursor < 0) app->process_choice_cursor = 0;
    if (app->process_choice_cursor < app->process_choice_scroll)
        app->process_choice_scroll = app->process_choice_cursor;
    if (app->process_choice_cursor >= app->process_choice_scroll + item_rows)
        app->process_choice_scroll = app->process_choice_cursor - item_rows + 1;
    max_scroll = (int)app->process_choice_count - item_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (app->process_choice_scroll > max_scroll) app->process_choice_scroll = max_scroll;
    for (i = 0; i < item_rows &&
                    app->process_choice_scroll + i < (int)app->process_choice_count; ++i) {
        int index = app->process_choice_scroll + i;
        ProcessChoice *choice = &app->process_choices[index];
        char line[256];
        if (choice->pid == choice->tid)
            snprintf(line, sizeof(line), "[%c] %-18s PID %d",
                     choice->checked ? 'x' : ' ', choice->name, choice->pid);
        else
            snprintf(line, sizeof(line), "[%c] %-18s PID %d  TID %d",
                     choice->checked ? 'x' : ' ', choice->name, choice->pid, choice->tid);
        if (index == app->process_choice_cursor) wattron(window, A_REVERSE | A_BOLD);
        else if (choice->checked) wattron(window, COLOR_PAIR(COLOR_CHECKED));
        mvwaddnstr(window, i + 1, 2, line, width - 4);
        if (index == app->process_choice_cursor) wattroff(window, A_REVERSE | A_BOLD);
        else if (choice->checked) wattroff(window, COLOR_PAIR(COLOR_CHECKED));
    }
    if (app->process_choice_count == 0) mvwaddstr(window, 1, 2, "No processes captured");
    mvwaddnstr(window, height - 2, 2,
               "Up/Down Move  Space Toggle  a All  n None  Enter Apply  Esc Cancel",
               width - 4);
    wnoutrefresh(window);
    delwin(window);
}

static int find_process(ProcessInfo *processes, size_t count, int pid) {
    size_t i;
    for (i = 0; i < count; ++i) if (processes[i].pid == pid) return (int)i;
    return -1;
}

static int add_process(ProcessInfo **processes, size_t *count, size_t *cap, int pid) {
    ProcessInfo *next;
    int index = find_process(*processes, *count, pid);
    if (index >= 0) return index;
    if (*count == *cap) {
        size_t new_cap = *cap ? *cap * 2 : 16;
        next = realloc(*processes, new_cap * sizeof(**processes));
        if (!next) return -1;
        *processes = next;
        *cap = new_cap;
    }
    index = (int)(*count)++;
    memset(&(*processes)[index], 0, sizeof((*processes)[index]));
    (*processes)[index].pid = pid;
    snprintf((*processes)[index].name, sizeof((*processes)[index].name), "process");
    return index;
}

static void add_process_tid(ProcessInfo *process, int tid) {
    size_t i;
    int *next;
    if (tid == process->pid) return;
    for (i = 0; i < process->tid_count; ++i) if (process->tids[i] == tid) return;
    if (process->tid_count == process->tid_cap) {
        size_t cap = process->tid_cap ? process->tid_cap * 2 : 4;
        next = realloc(process->tids, cap * sizeof(*process->tids));
        if (!next) return;
        process->tids = next;
        process->tid_cap = cap;
    }
    process->tids[process->tid_count++] = tid;
}

static int spawned_id(const TraceEvent *event, bool *is_thread) {
    char syscall[64];
    const char *equals;
    char *end;
    long value;
    if (!trace_event_syscall_name(event, syscall, sizeof(syscall))) return 0;
    if (strcmp(syscall, "fork") != 0 && strcmp(syscall, "vfork") != 0 &&
        strcmp(syscall, "clone") != 0 && strcmp(syscall, "clone3") != 0)
        return 0;
    equals = strrchr(event->text, '=');
    if (!equals) return 0;
    value = strtol(equals + 1, &end, 10);
    if (end == equals + 1 || value <= 0 || value > INT_MAX) return 0;
    *is_thread = strstr(event->text, "CLONE_THREAD") != NULL;
    return (int)value;
}

static int compare_ints(const void *left, const void *right) {
    int a = *(const int *)left;
    int b = *(const int *)right;
    return a < b ? -1 : a > b;
}

static int compare_processes(const void *left, const void *right) {
    const ProcessInfo *a = left;
    const ProcessInfo *b = right;
    return a->pid < b->pid ? -1 : a->pid > b->pid;
}

static ProcessInfo *build_process_graph(App *app, size_t *process_count) {
    ProcessInfo *processes = NULL;
    size_t count = 0;
    size_t cap = 0;
    size_t i;
    for (i = 0; i < app->model.count; ++i) {
        TraceEvent *event = &app->model.events[i];
        int index = add_process(&processes, &count, &cap, event->pid);
        if (index < 0) break;
        add_process_tid(&processes[index], event->tid);
        if (event->tid == event->pid || strcmp(processes[index].name, "process") == 0)
            event_name(event, processes[index].name, sizeof(processes[index].name));
    }
    for (i = 0; i < app->model.count; ++i) {
        TraceEvent *event = &app->model.events[i];
        bool is_thread = false;
        int child = spawned_id(event, &is_thread);
        int parent_index;
        int child_index;
        if (!child) continue;
        parent_index = find_process(processes, count, event->pid);
        if (parent_index < 0) continue;
        if (is_thread) {
            add_process_tid(&processes[parent_index], child);
        } else {
            child_index = add_process(&processes, &count, &cap, child);
            if (child_index >= 0 && child != event->pid)
                processes[child_index].parent_pid = event->pid;
        }
    }
    if (count > 1) qsort(processes, count, sizeof(*processes), compare_processes);
    for (i = 0; i < count; ++i)
        if (processes[i].tid_count > 1)
            qsort(processes[i].tids, processes[i].tid_count,
                  sizeof(*processes[i].tids), compare_ints);
    *process_count = count;
    return processes;
}

static void append_graph_process(ProcessInfo *processes, size_t process_count,
                                 size_t index, int depth, int selected_pid,
                                 int selected_tid, GraphLine *lines,
                                 size_t *line_count, size_t line_cap) {
    ProcessInfo *process = &processes[index];
    size_t i;
    if (process->visited || *line_count >= line_cap) return;
    process->visited = true;
    snprintf(lines[*line_count].text, sizeof(lines[*line_count].text), "%*s%s%s[%d]",
             depth * 3, "", depth ? "|- " : "", process->name, process->pid);
    lines[*line_count].selected = selected_pid == process->pid &&
                                  selected_tid == process->pid;
    (*line_count)++;
    for (i = 0; i < process->tid_count && *line_count < line_cap; ++i) {
        snprintf(lines[*line_count].text, sizeof(lines[*line_count].text),
                 "%*s|- TID %d", (depth + 1) * 3, "", process->tids[i]);
        lines[*line_count].selected = selected_pid == process->pid &&
                                      selected_tid == process->tids[i];
        (*line_count)++;
    }
    for (i = 0; i < process_count; ++i)
        if (processes[i].parent_pid == process->pid)
            append_graph_process(processes, process_count, i, depth + 1,
                                 selected_pid, selected_tid, lines, line_count, line_cap);
}

static void draw_process_graph_popup(App *app) {
    ProcessInfo *processes;
    size_t process_count;
    size_t line_cap;
    GraphLine *lines;
    size_t line_count = 0;
    size_t i;
    int selected_pid = 0;
    int selected_tid = 0;
    int selected_line = -1;
    int rows;
    int cols;
    int height;
    int width;
    int content;
    int start = 0;
    WINDOW *window;
    processes = build_process_graph(app, &process_count);
    line_cap = process_count + 1;
    for (i = 0; i < process_count; ++i) line_cap += processes[i].tid_count;
    lines = calloc(line_cap, sizeof(*lines));
    if (!lines) goto done;
    if (app->selected >= 0 && app->selected < (int)app->model.count) {
        selected_pid = app->model.events[app->selected].pid;
        selected_tid = app->model.events[app->selected].tid;
    }
    for (i = 0; i < process_count; ++i)
        if (processes[i].parent_pid == 0 ||
            find_process(processes, process_count, processes[i].parent_pid) < 0)
            append_graph_process(processes, process_count, i, 0, selected_pid,
                                 selected_tid, lines, &line_count, line_cap);
    for (i = 0; i < process_count; ++i)
        if (!processes[i].visited)
            append_graph_process(processes, process_count, i, 0, selected_pid,
                                 selected_tid, lines, &line_count, line_cap);
    for (i = 0; i < line_count; ++i) if (lines[i].selected) selected_line = (int)i;
    getmaxyx(stdscr, rows, cols);
    height = (int)line_count + 2;
    if (height < 8) height = 8;
    if (height > rows - 4) height = rows - 4;
    width = cols - 8;
    if (width > 76) width = 76;
    window = create_popup("Process Graph", height, width);
    if (!window) goto done;
    getmaxyx(window, height, width);
    content = height - 2;
    if ((int)line_count > content && selected_line >= content / 2)
        start = selected_line - content / 2;
    if (start + content > (int)line_count) start = (int)line_count - content;
    if (start < 0) start = 0;
    for (i = 0; i < (size_t)content && start + (int)i < (int)line_count; ++i) {
        GraphLine *line = &lines[start + i];
        if (line->selected) wattron(window, A_REVERSE | A_BOLD);
        mvwaddnstr(window, (int)i + 1, 2, line->text, width - 4);
        if (line->selected) wattroff(window, A_REVERSE | A_BOLD);
    }
    wnoutrefresh(window);
    delwin(window);
done:
    free(lines);
    for (i = 0; i < process_count; ++i) free(processes[i].tids);
    free(processes);
}

static void draw_popup(App *app) {
    int rows;
    int cols;
    getmaxyx(stdscr, rows, cols);
    if (rows < 10 || cols < 30) return;
    if (app->popup == POPUP_STACK) draw_stack_popup(app);
    else if (app->popup == POPUP_HELP) draw_help_popup();
    else if (app->popup == POPUP_SYSCALLS) draw_syscall_popup(app);
    else if (app->popup == POPUP_PROCESSES) draw_process_popup(app);
    else if (app->popup == POPUP_PROCESS_GRAPH) draw_process_graph_popup(app);
    else if (app->popup == POPUP_FD_INFO) draw_fd_info_popup(app);
}

static void open_stack_popup(App *app) {
    TraceEvent *event;
    int adopted;
    size_t i;
    if (app->selected < 0 || app->selected >= (int)app->model.count) return;
    event = &app->model.events[app->selected];
    if (event->frame_count == 0) return;
    adopted = trace_event_source_frame(event);
    app->stack_cursor = adopted >= 0 ? adopted : 0;
    if (adopted < 0) {
        for (i = 0; i < event->frame_count; ++i) {
            if (stack_frame_resolve(&event->frames[i])) {
                app->stack_cursor = (int)i;
                break;
            }
        }
    }
    app->follow_latest = false;
    app->popup = POPUP_STACK;
}

static void move_stack_cursor(App *app, int direction) {
    TraceEvent *event;
    int index;
    if (app->selected < 0 || app->selected >= (int)app->model.count) return;
    event = &app->model.events[app->selected];
    index = app->stack_cursor + direction;
    while (index >= 0 && index < (int)event->frame_count) {
        if (stack_frame_resolve(&event->frames[index])) {
            app->stack_cursor = index;
            return;
        }
        index += direction;
    }
}

static void use_stack_source(App *app) {
    TraceEvent *event;
    StackFrame *frame;
    char *path;
    char *function;
    if (app->selected < 0 || app->selected >= (int)app->model.count) return;
    event = &app->model.events[app->selected];
    if (app->stack_cursor < 0 || app->stack_cursor >= (int)event->frame_count) return;
    frame = &event->frames[app->stack_cursor];
    if (!stack_frame_resolve(frame)) return;
    path = strdup(frame->source_path);
    function = strdup(frame->source_function);
    if (!path || !function) {
        free(path);
        free(function);
        return;
    }
    clear_source_override(app);
    app->source_override_path = path;
    app->source_override_function = function;
    app->source_override_line = frame->source_line;
    app->source_override_event = app->selected;
    app->source_scroll_offset = 0;
    app->popup = POPUP_NONE;
}

static void jump_fd_origin(App *app) {
    const FdEventInfo *info;
    int target;
    if (app->selected < 0 || !ensure_fd_analysis(app)) return;
    info = fd_analysis_event(app->fd_analysis, (size_t)app->selected);
    if (!info || info->jump_event < 0 ||
        (size_t)info->jump_event >= app->model.count)
        return;
    target = info->jump_event;
    app->forced_event = target;
    app->view_dirty = true;
    app->follow_latest = false;
    select_event(app, target);
}

static void open_fd_info_popup(App *app) {
    const FdEventInfo *info;
    if (app->selected < 0 || !ensure_fd_analysis(app)) return;
    info = fd_analysis_event(app->fd_analysis, (size_t)app->selected);
    if (!info || info->primary_ref < 0 || (size_t)info->primary_ref >= info->ref_count)
        return;
    app->follow_latest = false;
    app->popup = POPUP_FD_INFO;
}

static void handle_syscall_popup(App *app, int key) {
    int visible_count = visible_choice_count(app);
    int index;
    size_t i;
    if (app->choice_search_input) {
        if (key == 27) {
            app->popup = POPUP_NONE;
            clear_choices(app);
        } else if (key == '\n' || key == '\r' || key == KEY_ENTER) {
            app->choice_search_input = false;
            app->choice_cursor = 0;
        } else if (key == KEY_BACKSPACE || key == 127 || key == 8) {
            if (app->choice_search_len > 0)
                app->choice_search[--app->choice_search_len] = '\0';
            app->choice_cursor = 0;
        } else if (key >= 32 && key <= 255 &&
                   app->choice_search_len + 1 < sizeof(app->choice_search)) {
            app->choice_search[app->choice_search_len++] = (char)key;
            app->choice_search[app->choice_search_len] = '\0';
            app->choice_cursor = 0;
        }
        return;
    }
    if (key == 27 || key == 'q') {
        app->popup = POPUP_NONE;
        clear_choices(app);
    } else if (key == '/') {
        app->choice_search_input = true;
    } else if (key == KEY_UP || key == 'k') {
        if (app->choice_cursor > 0) --app->choice_cursor;
    } else if (key == KEY_DOWN || key == 'j') {
        if (app->choice_cursor + 1 < visible_count) ++app->choice_cursor;
    } else if (key == ' ' && visible_count > 0) {
        index = choice_index_at(app, app->choice_cursor);
        if (index >= 0) app->choices[index].checked = !app->choices[index].checked;
    } else if (key == 'a') {
        for (i = 0; i < app->choice_count; ++i) app->choices[i].checked = true;
    } else if (key == 'n') {
        for (i = 0; i < app->choice_count; ++i) app->choices[i].checked = false;
    } else if (key == '\n' || key == '\r' || key == KEY_ENTER) {
        apply_syscall_menu(app);
    }
}

static void handle_process_popup(App *app, int key) {
    size_t i;
    if (key == 27 || key == 'q') {
        app->popup = POPUP_NONE;
        clear_process_choices(app);
    } else if (key == KEY_UP || key == 'k') {
        if (app->process_choice_cursor > 0) --app->process_choice_cursor;
    } else if (key == KEY_DOWN || key == 'j') {
        if (app->process_choice_cursor + 1 < (int)app->process_choice_count)
            ++app->process_choice_cursor;
    } else if (key == ' ' && app->process_choice_count > 0) {
        ProcessChoice *choice = &app->process_choices[app->process_choice_cursor];
        choice->checked = !choice->checked;
    } else if (key == 'a') {
        for (i = 0; i < app->process_choice_count; ++i)
            app->process_choices[i].checked = true;
    } else if (key == 'n') {
        for (i = 0; i < app->process_choice_count; ++i)
            app->process_choices[i].checked = false;
    } else if (key == '\n' || key == '\r' || key == KEY_ENTER) {
        apply_process_menu(app);
    }
}

static void handle_popup(App *app, int key) {
    if (key == ERR) return;
    if (app->popup == POPUP_SYSCALLS) {
        handle_syscall_popup(app, key);
    } else if (app->popup == POPUP_PROCESSES) {
        handle_process_popup(app, key);
    } else if (app->popup == POPUP_STACK && (key == KEY_UP || key == 'k')) {
        move_stack_cursor(app, -1);
    } else if (app->popup == POPUP_STACK && (key == KEY_DOWN || key == 'j')) {
        move_stack_cursor(app, 1);
    } else if (app->popup == POPUP_STACK &&
               (key == '\n' || key == '\r' || key == KEY_ENTER)) {
        use_stack_source(app);
    } else if (key == 27 || key == 'q' ||
               (app->popup == POPUP_STACK && key == 's') ||
               (app->popup == POPUP_FD_INFO && key == 'i') ||
               (app->popup == POPUP_HELP && key == '?') ||
               (app->popup == POPUP_PROCESS_GRAPH && key == 'P')) {
        app->popup = POPUP_NONE;
    }
}

static void stop_strace(App *app) {
    int status;
    int attempts;
    pid_t waited;
    if (app->strace_pid <= 0) return;
    waited = waitpid(app->strace_pid, &status, WNOHANG);
    if (waited == app->strace_pid || (waited < 0 && errno == ECHILD)) {
        app->strace_pid = -1;
        return;
    }
    if (app->command_mode) kill(-app->strace_pid, SIGTERM);
    else kill(app->strace_pid, SIGTERM);
    for (attempts = 0; attempts < 20; ++attempts) {
        if (waitpid(app->strace_pid, &status, WNOHANG) == app->strace_pid) {
            app->strace_pid = -1;
            return;
        }
        usleep(50000);
    }
    kill(app->strace_pid, SIGKILL);
    waitpid(app->strace_pid, &status, 0);
    app->strace_pid = -1;
}

static void drain_trace(App *app) {
    int attempts;
    for (attempts = 0; !app->trace_done && attempts < 100; ++attempts) {
        read_trace(app);
        if (!app->trace_done) usleep(10000);
    }
}

static void cleanup(App *app) {
    stop_strace(app);
    if (app->trace_fd >= 0) close(app->trace_fd);
    if (app->stdout_path[0]) unlink(app->stdout_path);
    if (app->stderr_path[0]) unlink(app->stderr_path);
    free(app->pending);
    free(app->visible);
    free(app->source_filter_path);
    clear_source_override(app);
    fd_analysis_free(app->fd_analysis);
    free(app->process_filter);
    free(app->identities);
    clear_process_choices(app);
    clear_choices(app);
    source_file_clear(&app->source);
    trace_model_free(&app->model);
}

int main(int argc, char **argv) {
    App app;
    Cli cli;
    int key;
    int rows;
    int status;
    int result = 0;
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage(stdout);
        return 0;
    }
    if (parse_cli(argc, argv, &cli) < 0) {
        usage(stderr);
        return 2;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "strace-src: an interactive terminal is required\n");
        return 2;
    }
    setlocale(LC_ALL, "");
    memset(&app, 0, sizeof(app));
    app.trace_fd = -1;
    app.strace_pid = -1;
    app.selected = -1;
    app.forced_event = -1;
    app.source_override_event = -1;
    app.follow_latest = true;
    app.view_dirty = true;
    app.save_path = cli.save_path;
    trace_model_init(&app.model);
    if (cli.replay) {
        if (trace_model_load(&app.model, cli.trace_path) < 0) {
            fprintf(stderr, "strace-src: cannot open %s: %s\n", cli.trace_path,
                    strerror(errno));
            cleanup(&app);
            return 1;
        }
        app.trace_done = true;
    } else if (start_strace(&app, cli.command, cli.attach_pid) < 0) {
        cleanup(&app);
        return 1;
    }
    initscr();
    /* ncurses otherwise waits about one second to distinguish a standalone
     * Escape key from the start of an arrow/function-key sequence. */
    set_escdelay(25);
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(COLOR_RESOLVED, COLOR_CYAN, -1);
        init_pair(COLOR_CHECKED, COLOR_GREEN, -1);
        init_pair(COLOR_ERROR, COLOR_RED, -1);
        init_pair(COLOR_FD, COLOR_YELLOW, -1);
    }
    timeout(100);
    for (;;) {
        read_trace(&app);
        if (app.strace_pid > 0 && waitpid(app.strace_pid, &status, WNOHANG) == app.strace_pid)
            app.strace_pid = -1;
        draw(&app);
        draw_popup(&app);
        doupdate();
        key = getch();
        if (app.popup != POPUP_NONE) {
            handle_popup(&app, key);
            continue;
        }
        if (app.input_mode != INPUT_NONE) {
            if (key != ERR) handle_input(&app, key);
            continue;
        }
        if (key == 'q') break;
        if (key == '\t') {
            app.focused_pane = 1 - app.focused_pane;
            if (app.focused_pane == 0) app.source_scroll_offset = 0;
            continue;
        }
        if (key == '/') { begin_input(&app, INPUT_SEARCH); continue; }
        if (key == 'e') { open_syscall_menu(&app); continue; }
        if (key == 'p') { open_process_menu(&app); continue; }
        if (key == 'P') { app.popup = POPUP_PROCESS_GRAPH; continue; }
        if (key == 'r') { reverse_from_selected(&app); continue; }
        if (key == 'o') { jump_fd_origin(&app); continue; }
        if (key == 'i') { open_fd_info_popup(&app); continue; }
        if (key == 's') { open_stack_popup(&app); continue; }
        if (key == '?') { app.popup = POPUP_HELP; continue; }
        if (key == 27) { clear_filters(&app); continue; }
        rows = getmaxy(stdscr);
        if (app.focused_pane == 1)
            scroll_source(&app, key, rows / 2 - 2);
        else
            navigate(&app, key, rows / 2 - 2);
    }
    endwin();
    stop_strace(&app);
    drain_trace(&app);
    if (app.save_path && trace_model_save(&app.model, app.save_path) < 0) {
        fprintf(stderr, "strace-src: cannot save %s: %s\n", app.save_path,
                strerror(errno));
        result = 1;
    } else if (app.save_path) {
        printf("Saved %zu trace events to %s\n", app.model.count, app.save_path);
    }
    cleanup(&app);
    return result;
}
