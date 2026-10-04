#include "gdb_cache.h"
#include "gdb_internal.h"
#include "mi.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

void copy_text(char *dst, size_t size, const char *src) {
    if (!size)
        return;
    if (!src)
        src = "";
    size_t len = strlen(src);
    if (len >= size)
        len = size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

const char *gtext(const Gdb *g, const char *english, const char *japanese) {
    return g->japanese ? japanese : english;
}

void append_output(Gdb *g, const char *text) {
    size_t n = strlen(text);
    if (n >= sizeof(g->output)) {
        text += n - (sizeof(g->output) - 1);
        n = sizeof(g->output) - 1;
    }
    if (n + g->output_len >= sizeof(g->output)) {
        size_t drop = n + g->output_len - sizeof(g->output) + 1;
        if (drop > g->output_len)
            drop = g->output_len;
        memmove(g->output, g->output + drop, g->output_len - drop);
        g->output_len -= drop;
    }
    memcpy(g->output + g->output_len, text, n);
    g->output_len += n;
    g->output[g->output_len] = '\0';
    g->output_revision++;
}

static void handle_async(Gdb *g, const char *line) {
    if (!strncmp(line, "=library-loaded,", 16) || !strncmp(line, "=library-unloaded,", 18) ||
        !strncmp(line, "=thread-group-started", 21) || !strncmp(line, "=thread-group-exited", 20))
        gdb_invalidate_symbols(g);
    if (!strncmp(line, "=thread-group-started", 21) || !strncmp(line, "=thread-group-exited", 20))
        g->register_names_valid = false;
    if (strstr(line, "*running")) {
        gdb_invalidate_values(g);
        g->state = GDB_RUNNING;
        copy_text(g->message, sizeof(g->message),
                  gtext(g, "Program running", "プログラムを実行中"));
        g->changed = true;
    } else if (strstr(line, "*stopped")) {
        gdb_invalidate_values(g);
        char reason[128] = "stopped";
        mi_string(line, "reason", reason, sizeof(reason));
        copy_text(g->stop_reason, sizeof(g->stop_reason), reason);
        if (!strcmp(reason, "exec")) {
            g->register_names_valid = false;
            gdb_invalidate_symbols(g);
        }
        g->stop_breakpoint = mi_int(line, "bkptno", 0);
        g->watch_old[0] = g->watch_new[0] = '\0';
        g->catch_event[0] = g->catch_target[0] = g->catch_phase[0] = '\0';
        g->catch_detail[0] = g->stop_thread[0] = '\0';
        mi_string(line, "thread-id", g->stop_thread, sizeof(g->stop_thread));
        if (strstr(reason, "watchpoint")) {
            if (!g->stop_breakpoint)
                g->stop_breakpoint = mi_int(line, "number", 0);
            mi_string(line, "old", g->watch_old, sizeof(g->watch_old));
            mi_string(line, "new", g->watch_new, sizeof(g->watch_new));
        }
        if (!strcmp(reason, "syscall-entry") || !strcmp(reason, "syscall-return")) {
            copy_text(g->catch_event, sizeof(g->catch_event), "syscall");
            mi_string(line, "syscall-name", g->catch_target, sizeof(g->catch_target));
            copy_text(g->catch_phase, sizeof(g->catch_phase),
                      !strcmp(reason, "syscall-entry") ? "Entry" : "Return");
            char number[64] = "";
            mi_string(line, "syscall-number", number, sizeof(number));
            snprintf(g->catch_detail, sizeof(g->catch_detail), "%s%s%s",
                     g->catch_target[0] ? g->catch_target : "syscall", number[0] ? " (#" : "",
                     number[0] ? number : "");
            if (number[0])
                strncat(g->catch_detail, ")",
                        sizeof(g->catch_detail) - strlen(g->catch_detail) - 1);
        } else if (!strcmp(reason, "fork") || !strcmp(reason, "vfork") || !strcmp(reason, "exec") ||
                   !strcmp(reason, "solib-event")) {
            copy_text(g->catch_event, sizeof(g->catch_event),
                      !strcmp(reason, "solib-event") ? "load" : reason);
            if (!mi_string(line, "new-exec", g->catch_target, sizeof(g->catch_target)) &&
                !mi_string(line, "new-inferior", g->catch_target, sizeof(g->catch_target)))
                mi_string(line, "new-thread-id", g->catch_target, sizeof(g->catch_target));
            snprintf(g->catch_detail, sizeof(g->catch_detail), "%s%s%s", g->catch_event,
                     g->catch_target[0] ? ": " : "", g->catch_target);
        } else if (!strcmp(reason, "signal-received")) {
            copy_text(g->catch_event, sizeof(g->catch_event), "signal");
            mi_string(line, "signal-name", g->catch_target, sizeof(g->catch_target));
            mi_string(line, "signal-meaning", g->catch_detail, sizeof(g->catch_detail));
        }
        if (!strcmp(reason, "exited-normally") || !strcmp(reason, "exited")) {
            g->state = GDB_EXITED;
            g->exit_code = mi_int(line, "exit-code", 0);
            snprintf(
                g->message, sizeof(g->message),
                gtext(g, "Program exited (status=%d)", "プログラムが終了しました（status=%d）"),
                g->exit_code);
        } else {
            g->state = GDB_STOPPED;
            g->selected_frame = 0;
            g->fullname[0] = g->file[0] = g->function[0] = '\0';
            g->line = 0;
            mi_string(line, "fullname", g->fullname, sizeof(g->fullname));
            mi_string(line, "file", g->file, sizeof(g->file));
            mi_string(line, "func", g->function, sizeof(g->function));
            g->line = mi_int(line, "line", 0);
            g->source_available = g->fullname[0] && g->line > 0;
            if (g->catch_event[0])
                snprintf(g->message, sizeof(g->message),
                         gtext(g, "Caught %s%s%s%s%s", "捕捉: %s%s%s%s%s"), g->catch_event,
                         g->catch_target[0] ? ": " : "", g->catch_target,
                         g->catch_phase[0] ? "  " : "", g->catch_phase);
            else
                snprintf(g->message, sizeof(g->message), gtext(g, "Stopped: %s", "停止: %s"),
                         reason);
        }
        g->changed = true;
    } else if (line[0] == '@' || line[0] == '~' || line[0] == '&') {
        char text[4096];
        if (mi_string(line, line[0] == '@' ? "@" : (line[0] == '~' ? "~" : "&"), text,
                      sizeof(text)))
            append_output(g, text);
        else if (line[1] == '"') {
            char fake[8192];
            snprintf(fake, sizeof(fake), "x=%s", line + 1);
            if (mi_string(fake, "x", text, sizeof(text)))
                append_output(g, text);
        }
    } else if (line[0] && line[0] != '^' && line[0] != '*' && line[0] != '=' &&
               strcmp(line, "(gdb) ") != 0 && strcmp(line, "(gdb)") != 0) {
        append_output(g, line);
        append_output(g, "\n");
    }
}

static int send_cmd(Gdb *g, const char *fmt, ...) {
    char command[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(command, sizeof(command), fmt, ap);
    va_end(ap);
    int token = ++g->token;
    char line[8448];
    int n = snprintf(line, sizeof(line), "%d%s\n", token, command);
    if (write(g->to_gdb, line, (size_t)n) != n) {
        copy_text(g->message, sizeof(g->message),
                  gtext(g, "Failed to write to GDB", "GDBへの書込みに失敗しました"));
        return -1;
    }
    return token;
}

static int append_input(Gdb *g, const char *text, size_t size) {
    size_t needed = g->readlen + size + 1;
    if (needed > g->readcap) {
        size_t capacity = g->readcap ? g->readcap : 65536;
        while (capacity < needed && capacity < 16 * 1024 * 1024)
            capacity *= 2;
        char *buffer = capacity >= needed ? realloc(g->readbuf, capacity) : NULL;
        if (!buffer) {
            copy_text(
                g->message, sizeof(g->message),
                gtext(g, "ERROR: GDB response is too large", "エラー: GDBの応答が大きすぎます"));
            g->state = GDB_FAILED;
            return -1;
        }
        g->readbuf = buffer;
        g->readcap = capacity;
    }
    memcpy(g->readbuf + g->readlen, text, size);
    g->readlen += size;
    g->readbuf[g->readlen] = 0;
    return 0;
}

static int wait_result(Gdb *g, int token, char *result, size_t size) {
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%d^", token);
    for (int elapsed = 0; elapsed < 5000; elapsed += 50) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g->from_gdb, &fds);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 50000};
        int ready = select(g->from_gdb + 1, &fds, NULL, NULL, &tv);
        if (ready < 0 && errno != EINTR)
            return -1;
        if (ready > 0) {
            char buf[8192];
            ssize_t n = read(g->from_gdb, buf, sizeof(buf));
            if (n <= 0)
                return -1;
            if (append_input(g, buf, (size_t)n))
                return -1;
            size_t start = 0;
            for (size_t i = 0; i < g->readlen; ++i)
                if (g->readbuf[i] == '\n') {
                    g->readbuf[i] = '\0';
                    char *line = g->readbuf + start;
                    if (strncmp(line, prefix, strlen(prefix)) == 0) {
                        bool oversized = strlen(line) >= size;
                        copy_text(result, size, line);
                        size_t remain = g->readlen - i - 1;
                        memmove(g->readbuf, g->readbuf + i + 1, remain);
                        g->readlen = remain;
                        if (oversized) {
                            copy_text(g->message, sizeof(g->message),
                                      gtext(g, "ERROR: GDB result exceeds command capacity",
                                            "エラー: GDBの結果が取得容量を超えました"));
                            return -1;
                        }
                        if (strstr(result, "^error")) {
                            char msg[GD_TEXT_MAX];
                            if (mi_string(result, "msg", msg, sizeof(msg)))
                                snprintf(g->message, sizeof(g->message),
                                         gtext(g, "ERROR: %.1000s", "エラー: %.1000s"), msg);
                            return -1;
                        }
                        return 0;
                    }
                    handle_async(g, line);
                    start = i + 1;
                }
            if (start) {
                size_t remain = g->readlen - start;
                memmove(g->readbuf, g->readbuf + start, remain);
                g->readlen = remain;
            }
        }
    }
    copy_text(g->message, sizeof(g->message),
              gtext(g, "ERROR: GDB response timed out", "エラー: GDBの応答がタイムアウトしました"));
    return -1;
}

int request(Gdb *g, char *result, size_t size, const char *fmt, ...) {
    char cmd[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    int token = send_cmd(g, "%s", cmd);
    return token < 0 ? -1 : wait_result(g, token, result, size);
}

static int start_session(Gdb *g, const char *program, char *const program_argv[],
                         pid_t attach_pid) {
    memset(g, 0, sizeof(*g));
    g->to_gdb = g->from_gdb = -1;
    g->state = GDB_NOT_STARTED;
    g->data_revision = g->symbol_revision = 1;
    copy_text(g->executable, sizeof(g->executable), program);
    int in[2], out[2];
    if (pipe(in) || pipe(out))
        return -1;
    g->pid = fork();
    if (g->pid < 0)
        return -1;
    if (g->pid == 0) {
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        dup2(out[1], STDERR_FILENO);
        close(in[0]);
        close(in[1]);
        close(out[0]);
        close(out[1]);
        execlp("gdb", "gdb", "--interpreter=mi2", "--quiet", program, (char *)NULL);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    g->to_gdb = in[1];
    g->from_gdb = out[0];
    char result[16384];
    if (request(g, result, sizeof(result), "-gdb-set pagination off"))
        return -1;
    request(g, result, sizeof(result), "-gdb-set confirm off");
    /* Make source-level step stop at the first instruction of a function that
       has no line information.  The UI then selects Source or Assembly from
       the stopped frame's fullname + line instead of silently stepping over it. */
    request(g, result, sizeof(result), "-gdb-set step-mode on");
    if (attach_pid > 0) {
        /* Accept interrupt/detach commands while the inferior is running. */
        if (request(g, result, sizeof(result), "-gdb-set mi-async on"))
            return -1;
        if (request(g, result, sizeof(result), "-target-attach %ld", (long)attach_pid))
            return -1;
        g->attached_pid = attach_pid;
        /* MI may deliver *stopped after ^done. Consume it before drawing. */
        for (int i = 0; i < 100 && g->state != GDB_STOPPED; ++i)
            if (gdb_poll(g, 50) < 0)
                return -1;
        if (g->state != GDB_STOPPED) {
            copy_text(g->message, sizeof(g->message), "ERROR: attach did not stop the process");
            return -1;
        }
        if (gdb_refresh(g))
            return -1;
        snprintf(g->message, sizeof(g->message), "Attached to PID %ld - press c to continue",
                 (long)attach_pid);
        return 0;
    }
    if (program_argv && program_argv[0]) {
        char command[8192] = "-exec-arguments";
        for (int i = 0; program_argv[i]; ++i) {
            char q[2048];
            quote_mi(program_argv[i], q, sizeof(q));
            if (strlen(command) + strlen(q) + 2 < sizeof(command)) {
                strcat(command, " ");
                strcat(command, q);
            }
        }
        request(g, result, sizeof(result), "%s", command);
    }
    /* Ask GDB for main's first executable line.  Symbol metadata often points
       at the declaration/brace one line earlier, which makes a new breakpoint
       appear to jump away from the cursor. */
    if (!request(g, result, sizeof(result), "-break-insert -t main")) {
        int number = mi_int(result, "number", 0);
        mi_string(result, "fullname", g->fullname, sizeof(g->fullname));
        mi_string(result, "file", g->file, sizeof(g->file));
        mi_string(result, "func", g->function, sizeof(g->function));
        g->line = mi_int(result, "line", 1);
        if (number > 0)
            request(g, result, sizeof(result), "-break-delete %d", number);
    }
    if (!g->fullname[0] && !request(g, result, sizeof(result),
                                    "-symbol-info-functions --name ^main$ --max-results 1")) {
        mi_string(result, "fullname", g->fullname, sizeof(g->fullname));
        mi_string(result, "filename", g->file, sizeof(g->file));
        if (!g->file[0])
            mi_string(result, "file", g->file, sizeof(g->file));
        g->line = mi_int(result, "line", 1);
        copy_text(g->function, sizeof(g->function), "main");
    }
    g->source_available = g->fullname[0] && g->line > 0;
    copy_text(g->message, sizeof(g->message), "Ready - press r to run");
    return 0;
}

int gdb_start(Gdb *g, const char *program, char *const program_argv[]) {
    return start_session(g, program, program_argv, 0);
}

int gdb_attach(Gdb *g, pid_t pid) {
    char path[64], program[GD_PATH_MAX];
    memset(g, 0, sizeof(*g));
    g->to_gdb = g->from_gdb = -1;
    if (pid <= 0) {
        copy_text(g->message, sizeof(g->message), "ERROR: PID must be positive");
        return -1;
    }
    snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid);
    ssize_t n = readlink(path, program, sizeof(program) - 1);
    if (n < 0) {
        snprintf(g->message, sizeof(g->message), "ERROR: cannot access PID %ld: %s", (long)pid,
                 strerror(errno));
        return -1;
    }
    program[n] = '\0';
    return start_session(g, program, NULL, pid);
}

void gdb_shutdown(Gdb *g) {
    if (g->attached_pid && g->to_gdb >= 0 && g->state != GDB_EXITED) {
        char result[4096];
        if (g->state == GDB_RUNNING) {
            request(g, result, sizeof(result), "-exec-interrupt --all");
            for (int i = 0; i < 100 && g->state == GDB_RUNNING; ++i)
                if (gdb_poll(g, 50) < 0)
                    break;
        }
        request(g, result, sizeof(result), "-target-detach");
    }
    if (g->to_gdb >= 0) {
        char r[1024];
        request(g, r, sizeof(r), "-gdb-exit");
        close(g->to_gdb);
    }
    if (g->from_gdb >= 0)
        close(g->from_gdb);
    if (g->pid > 0) {
        int status;
        if (waitpid(g->pid, &status, WNOHANG) == 0) {
            kill(g->pid, SIGTERM);
            waitpid(g->pid, &status, 0);
        }
    }
    g->pid = 0;
    free(g->cache);
    g->cache = NULL;
    free(g->readbuf);
    g->readbuf = NULL;
    g->readlen = g->readcap = 0;
    g->attached_pid = 0;
    g->to_gdb = g->from_gdb = -1;
}

static bool process_async_buffer(Gdb *g) {
    size_t start = 0;
    bool processed = false;
    for (size_t i = 0; i < g->readlen; ++i) {
        if (g->readbuf[i] != '\n')
            continue;
        g->readbuf[i] = '\0';
        handle_async(g, g->readbuf + start);
        start = i + 1;
        processed = true;
    }
    if (start) {
        size_t remain = g->readlen - start;
        memmove(g->readbuf, g->readbuf + start, remain);
        g->readlen = remain;
    }
    return processed;
}

int gdb_poll(Gdb *g, int timeout_ms) {
    /* A synchronous MI request can read past its own ^result and leave a
       complete *stopped record in readbuf.  Process that record before
       waiting for more fd data, otherwise a fast step can miss its stop. */
    if (process_async_buffer(g)) {
        if (g->changed && g->state == GDB_STOPPED)
            gdb_refresh(g);
        return 1;
    }
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(g->from_gdb, &fds);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int ready = select(g->from_gdb + 1, &fds, NULL, NULL, &tv);
    if (ready <= 0)
        return ready;
    char buf[8192];
    ssize_t n = read(g->from_gdb, buf, sizeof(buf));
    if (n <= 0) {
        g->state = GDB_FAILED;
        return -1;
    }
    if (append_input(g, buf, (size_t)n))
        return -1;
    process_async_buffer(g);
    if (g->changed && g->state == GDB_STOPPED)
        gdb_refresh(g);
    return 1;
}
