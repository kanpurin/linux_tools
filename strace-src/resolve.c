#define _GNU_SOURCE
#include "strace_src.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static bool starts_with(const char *text, const char *prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static bool is_runtime_frame(const StackFrame *frame) {
    const char *base = strrchr(frame->module, '/');
    const char *fn = frame->function;
    base = base ? base + 1 : frame->module;
    if (starts_with(base, "libc.so") || starts_with(base, "libpthread.so") ||
        starts_with(base, "libdl.so") || starts_with(base, "librt.so") ||
        starts_with(base, "ld-linux") || starts_with(base, "ld-musl") ||
        strcmp(base, "linux-vdso.so.1") == 0) {
        return true;
    }
    return strcmp(fn, "_start") == 0 || starts_with(fn, "__libc_start") ||
           starts_with(fn, "__GI_");
}

static bool read_all(int fd, char *buffer, size_t size) {
    size_t used = 0;
    ssize_t got;
    while (used + 1 < size) {
        got = read(fd, buffer + used, size - used - 1);
        if (got > 0) {
            used += (size_t)got;
        } else if (got == 0) {
            break;
        } else if (errno != EINTR) {
            break;
        }
    }
    buffer[used] = '\0';
    return used > 0;
}

static bool addr2line_lookup(const StackFrame *frame, char **function_out,
                             char **path_out, int *line_out) {
    int fds[2];
    pid_t pid;
    int status;
    char output[8192];
    char *first;
    char *second;
    char *newline;
    char *colon;
    char *end;
    long line;

    if (!frame->module[0] || !frame->address[0] || pipe(fds) < 0) {
        return false;
    }
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        int nullfd;
        dup2(fds[1], STDOUT_FILENO);
        nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) {
            dup2(nullfd, STDERR_FILENO);
        }
        close(fds[0]);
        close(fds[1]);
        execlp("addr2line", "addr2line", "-f", "-C", "-e", frame->module,
               frame->address, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    read_all(fds[0], output, sizeof(output));
    close(fds[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return false;
    }
    first = output;
    newline = strchr(first, '\n');
    if (!newline) {
        return false;
    }
    *newline = '\0';
    second = newline + 1;
    newline = strchr(second, '\n');
    if (newline) {
        *newline = '\0';
    }
    if (strcmp(first, "??") == 0 || starts_with(second, "??:")) {
        return false;
    }
    end = strstr(second, " (discriminator ");
    if (end) {
        *end = '\0';
    }
    colon = strrchr(second, ':');
    if (!colon) {
        return false;
    }
    line = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || line <= 0) {
        return false;
    }
    *colon = '\0';
    *function_out = strdup(first);
    *path_out = realpath(second, NULL);
    if (!*path_out) *path_out = strdup(second);
    *line_out = (int)line;
    if (!*function_out || !*path_out) {
        free(*function_out);
        free(*path_out);
        *function_out = NULL;
        *path_out = NULL;
        return false;
    }
    return true;
}

bool stack_frame_resolve(StackFrame *frame) {
    if (frame->resolution_state != 0) return frame->resolution_state == 1;
    if (addr2line_lookup(frame, &frame->source_function, &frame->source_path,
                         &frame->source_line)) {
        frame->resolution_state = 1;
        return true;
    }
    frame->resolution_state = 2;
    return false;
}

bool trace_event_resolve(TraceEvent *event) {
    size_t i;
    if (event->resolution_state != 0) {
        return event->resolution_state == 1;
    }
    for (i = 0; i < event->frame_count; ++i) {
        if (is_runtime_frame(&event->frames[i])) {
            continue;
        }
        if (stack_frame_resolve(&event->frames[i])) {
            event->source_function = strdup(event->frames[i].source_function);
            event->source_path = strdup(event->frames[i].source_path);
            event->source_line = event->frames[i].source_line;
            if (!event->source_function || !event->source_path) {
                free(event->source_function);
                free(event->source_path);
                event->source_function = NULL;
                event->source_path = NULL;
                event->source_line = 0;
                event->resolution_state = 2;
                return false;
            }
            event->resolution_state = 1;
            event->source_frame = i + 1;
            return true;
        }
    }
    event->resolution_state = 2;
    return false;
}

int trace_event_source_frame(TraceEvent *event) {
    size_t i;
    if (!trace_event_resolve(event)) return -1;
    if (event->source_frame > 0 && event->source_frame <= event->frame_count) {
        StackFrame *frame = &event->frames[event->source_frame - 1];
        if (frame->resolution_state == 0 && event->source_path &&
            event->source_function) {
            frame->source_path = strdup(event->source_path);
            frame->source_function = strdup(event->source_function);
            frame->source_line = event->source_line;
            if (frame->source_path && frame->source_function) {
                frame->resolution_state = 1;
            } else {
                free(frame->source_path);
                free(frame->source_function);
                frame->source_path = NULL;
                frame->source_function = NULL;
            }
        }
        return (int)event->source_frame - 1;
    }
    if (!event->source_function) return -1;
    for (i = 0; i < event->frame_count; ++i) {
        if (strcmp(event->frames[i].function, event->source_function) == 0) {
            event->source_frame = i + 1;
            return (int)i;
        }
    }
    return -1;
}
