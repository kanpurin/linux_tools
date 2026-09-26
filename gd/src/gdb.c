#include "gdb.h"

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

static void copy_text(char *dst, size_t size, const char *src) {
    if (!size) return;
    if (!src) src = "";
    size_t len = strlen(src);
    if (len >= size) len = size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static const char *gtext(const Gdb *g, const char *english, const char *japanese) {
    return g->japanese ? japanese : english;
}

static void append_output(Gdb *g, const char *text) {
    size_t n = strlen(text);
    if (n + g->output_len >= sizeof(g->output)) {
        size_t drop = n + g->output_len - sizeof(g->output) + 1;
        if (drop > g->output_len) drop = g->output_len;
        memmove(g->output, g->output + drop, g->output_len - drop);
        g->output_len -= drop;
    }
    memcpy(g->output + g->output_len, text, n);
    g->output_len += n;
    g->output[g->output_len] = '\0';
}

static bool mi_string(const char *line, const char *key, char *out, size_t size) {
    char needle[128];
    snprintf(needle, sizeof(needle), "%s=\"", key);
    const char *p = strstr(line, needle);
    if (!p) return false;
    p += strlen(needle);
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < size) {
        if (*p == '\\' && p[1]) {
            ++p;
            switch (*p) {
                case 'n': out[n++] = '\n'; break;
                case 'r': out[n++] = '\r'; break;
                case 't': out[n++] = '\t'; break;
                default: out[n++] = *p; break;
            }
            ++p;
        } else out[n++] = *p++;
    }
    out[n] = '\0';
    return true;
}

static int mi_int(const char *line, const char *key, int fallback) {
    char value[64];
    if (!mi_string(line, key, value, sizeof(value))) return fallback;
    return atoi(value);
}

static const char *mi_record_end(const char *start);
static const char *mi_list_end(const char *start);

static void quote_mi(const char *src, char *dst, size_t size) {
    size_t n = 0;
    if (size) dst[n++] = '"';
    for (; *src && n + 2 < size; ++src) {
        if (*src == '\\' || *src == '"') dst[n++] = '\\';
        dst[n++] = *src;
    }
    if (n + 1 < size) dst[n++] = '"';
    dst[n < size ? n : size - 1] = '\0';
}

static void handle_async(Gdb *g, const char *line) {
    if (strstr(line, "*running")) {
        g->state = GDB_RUNNING;
        copy_text(g->message, sizeof(g->message), gtext(g, "Program running", "プログラムを実行中"));
        g->changed = true;
    } else if (strstr(line, "*stopped")) {
        char reason[128] = "stopped";
        mi_string(line, "reason", reason, sizeof(reason));
        copy_text(g->stop_reason, sizeof(g->stop_reason), reason);
        g->stop_breakpoint = mi_int(line, "bkptno", 0);
        g->watch_old[0] = g->watch_new[0] = '\0';
        g->catch_event[0] = g->catch_target[0] = g->catch_phase[0] = '\0';
        g->catch_detail[0] = g->stop_thread[0] = '\0';
        mi_string(line, "thread-id", g->stop_thread, sizeof(g->stop_thread));
        if (strstr(reason, "watchpoint")) {
            if (!g->stop_breakpoint) g->stop_breakpoint = mi_int(line, "number", 0);
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
                     g->catch_target[0] ? g->catch_target : "syscall",
                     number[0] ? " (#" : "", number[0] ? number : "");
            if (number[0]) strncat(g->catch_detail, ")",
                                   sizeof(g->catch_detail) - strlen(g->catch_detail) - 1);
        } else if (!strcmp(reason, "fork") || !strcmp(reason, "vfork") ||
                   !strcmp(reason, "exec") || !strcmp(reason, "solib-event")) {
            copy_text(g->catch_event, sizeof(g->catch_event),
                      !strcmp(reason, "solib-event") ? "load" : reason);
            if (!mi_string(line, "new-exec", g->catch_target, sizeof(g->catch_target)) &&
                !mi_string(line, "new-inferior", g->catch_target, sizeof(g->catch_target)))
                mi_string(line, "new-thread-id", g->catch_target, sizeof(g->catch_target));
            snprintf(g->catch_detail, sizeof(g->catch_detail), "%s%s%s",
                     g->catch_event, g->catch_target[0] ? ": " : "", g->catch_target);
        } else if (!strcmp(reason, "signal-received")) {
            copy_text(g->catch_event, sizeof(g->catch_event), "signal");
            mi_string(line, "signal-name", g->catch_target, sizeof(g->catch_target));
            mi_string(line, "signal-meaning", g->catch_detail, sizeof(g->catch_detail));
        }
        if (!strcmp(reason, "exited-normally") || !strcmp(reason, "exited")) {
            g->state = GDB_EXITED;
            g->exit_code = mi_int(line, "exit-code", 0);
            snprintf(g->message, sizeof(g->message), gtext(g, "Program exited (status=%d)", "プログラムが終了しました（status=%d）"), g->exit_code);
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
                snprintf(g->message, sizeof(g->message), gtext(g, "Caught %s%s%s%s%s", "捕捉: %s%s%s%s%s"),
                         g->catch_event,
                         g->catch_target[0] ? ": " : "", g->catch_target,
                         g->catch_phase[0] ? "  " : "", g->catch_phase);
            else snprintf(g->message, sizeof(g->message), gtext(g, "Stopped: %s", "停止: %s"), reason);
        }
        g->changed = true;
    } else if (line[0] == '@' || line[0] == '~' || line[0] == '&') {
        char text[4096];
        if (mi_string(line, line[0] == '@' ? "@" : (line[0] == '~' ? "~" : "&"), text, sizeof(text)))
            append_output(g, text);
        else if (line[1] == '"') {
            char fake[8192];
            snprintf(fake, sizeof(fake), "x=%s", line + 1);
            if (mi_string(fake, "x", text, sizeof(text))) append_output(g, text);
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
        copy_text(g->message, sizeof(g->message), gtext(g, "Failed to write to GDB", "GDBへの書込みに失敗しました"));
        return -1;
    }
    return token;
}

static int wait_result(Gdb *g, int token, char *result, size_t size) {
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%d^", token);
    for (int elapsed = 0; elapsed < 5000; elapsed += 50) {
        fd_set fds;
        FD_ZERO(&fds); FD_SET(g->from_gdb, &fds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
        int ready = select(g->from_gdb + 1, &fds, NULL, NULL, &tv);
        if (ready < 0 && errno != EINTR) return -1;
        if (ready > 0) {
            char buf[8192];
            ssize_t n = read(g->from_gdb, buf, sizeof(buf));
            if (n <= 0) return -1;
            if (g->readlen + (size_t)n >= sizeof(g->readbuf)) g->readlen = 0;
            memcpy(g->readbuf + g->readlen, buf, (size_t)n); g->readlen += (size_t)n;
            size_t start = 0;
            for (size_t i = 0; i < g->readlen; ++i) if (g->readbuf[i] == '\n') {
                g->readbuf[i] = '\0';
                char *line = g->readbuf + start;
                if (strncmp(line, prefix, strlen(prefix)) == 0) {
                    copy_text(result, size, line);
                    size_t remain = g->readlen - i - 1;
                    memmove(g->readbuf, g->readbuf + i + 1, remain); g->readlen = remain;
                    if (strstr(line, "^error")) {
                        char msg[GD_TEXT_MAX];
                        if (mi_string(line, "msg", msg, sizeof(msg)))
                            snprintf(g->message, sizeof(g->message), gtext(g, "ERROR: %.1000s", "エラー: %.1000s"), msg);
                        return -1;
                    }
                    return 0;
                }
                handle_async(g, line);
                start = i + 1;
            }
            if (start) {
                size_t remain = g->readlen - start;
                memmove(g->readbuf, g->readbuf + start, remain); g->readlen = remain;
            }
        }
    }
    copy_text(g->message, sizeof(g->message), gtext(g, "ERROR: GDB response timed out", "エラー: GDBの応答がタイムアウトしました"));
    return -1;
}

static int request(Gdb *g, char *result, size_t size, const char *fmt, ...) {
    char cmd[8192];
    va_list ap;
    va_start(ap, fmt); vsnprintf(cmd, sizeof(cmd), fmt, ap); va_end(ap);
    int token = send_cmd(g, "%s", cmd);
    return token < 0 ? -1 : wait_result(g, token, result, size);
}

int gdb_start(Gdb *g, const char *program, char *const program_argv[]) {
    memset(g, 0, sizeof(*g)); g->to_gdb = g->from_gdb = -1; g->state = GDB_NOT_STARTED;
    copy_text(g->executable, sizeof(g->executable), program);
    int in[2], out[2];
    if (pipe(in) || pipe(out)) return -1;
    g->pid = fork();
    if (g->pid < 0) return -1;
    if (g->pid == 0) {
        dup2(in[0], STDIN_FILENO); dup2(out[1], STDOUT_FILENO); dup2(out[1], STDERR_FILENO);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execlp("gdb", "gdb", "--interpreter=mi2", "--quiet", program, (char *)NULL);
        _exit(127);
    }
    close(in[0]); close(out[1]); g->to_gdb = in[1]; g->from_gdb = out[0];
    char result[16384];
    if (request(g, result, sizeof(result), "-gdb-set pagination off")) return -1;
    request(g, result, sizeof(result), "-gdb-set confirm off");
    /* Make source-level step stop at the first instruction of a function that
       has no line information.  The UI then selects Source or Assembly from
       the stopped frame's fullname + line instead of silently stepping over it. */
    request(g, result, sizeof(result), "-gdb-set step-mode on");
    if (program_argv && program_argv[0]) {
        char command[8192] = "-exec-arguments";
        for (int i = 0; program_argv[i]; ++i) {
            char q[2048]; quote_mi(program_argv[i], q, sizeof(q));
            if (strlen(command) + strlen(q) + 2 < sizeof(command)) { strcat(command, " "); strcat(command, q); }
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
        if (number > 0) request(g, result, sizeof(result), "-break-delete %d", number);
    }
    if (!g->fullname[0] &&
        !request(g, result, sizeof(result), "-symbol-info-functions --name ^main$ --max-results 1")) {
        mi_string(result, "fullname", g->fullname, sizeof(g->fullname));
        mi_string(result, "filename", g->file, sizeof(g->file));
        if (!g->file[0]) mi_string(result, "file", g->file, sizeof(g->file));
        g->line = mi_int(result, "line", 1);
        copy_text(g->function, sizeof(g->function), "main");
    }
    g->source_available = g->fullname[0] && g->line > 0;
    copy_text(g->message, sizeof(g->message), "Ready - press r to run");
    return 0;
}

void gdb_shutdown(Gdb *g) {
    if (g->to_gdb >= 0) { char r[1024]; request(g, r, sizeof(r), "-gdb-exit"); close(g->to_gdb); }
    if (g->from_gdb >= 0) close(g->from_gdb);
    if (g->pid > 0) { int status; if (waitpid(g->pid, &status, WNOHANG) == 0) { kill(g->pid, SIGTERM); waitpid(g->pid, &status, 0); } }
    g->pid = 0;
}

static bool process_async_buffer(Gdb *g) {
    size_t start = 0;
    bool processed = false;
    for (size_t i = 0; i < g->readlen; ++i) {
        if (g->readbuf[i] != '\n') continue;
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
        if (g->changed && g->state == GDB_STOPPED) gdb_refresh(g);
        return 1;
    }
    fd_set fds; FD_ZERO(&fds); FD_SET(g->from_gdb, &fds);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int ready = select(g->from_gdb + 1, &fds, NULL, NULL, &tv);
    if (ready <= 0) return ready;
    char buf[8192]; ssize_t n = read(g->from_gdb, buf, sizeof(buf));
    if (n <= 0) { g->state = GDB_FAILED; return -1; }
    if (g->readlen + (size_t)n >= sizeof(g->readbuf)) g->readlen = 0;
    memcpy(g->readbuf + g->readlen, buf, (size_t)n); g->readlen += (size_t)n;
    process_async_buffer(g);
    if (g->changed && g->state == GDB_STOPPED) gdb_refresh(g);
    return 1;
}

static int exec_simple(Gdb *g, const char *cmd) {
    char result[4096];
    if (request(g, result, sizeof(result), "%s", cmd)) return -1;
    g->state = GDB_RUNNING; g->changed = true; return 0;
}
int gdb_run(Gdb *g) {
    if (g->state == GDB_RUNNING) {
        copy_text(g->message, sizeof(g->message), gtext(g, "Program is already running", "プログラムはすでに実行中です"));
        return -1;
    }
    if (g->state == GDB_STOPPED) {
        copy_text(g->message, sizeof(g->message),
                  gtext(g, "Program already started; press c to continue", "プログラムは開始済みです。cで続行してください"));
        return -1;
    }
    if (g->state == GDB_FAILED) {
        copy_text(g->message, sizeof(g->message), gtext(g, "ERROR: GDB is unavailable", "エラー: GDBを使用できません"));
        return -1;
    }
    return exec_simple(g, "-exec-run");
}
int gdb_continue(Gdb *g) { return exec_simple(g, "-exec-continue"); }
int gdb_next(Gdb *g) { return exec_simple(g, "-exec-next"); }
int gdb_step(Gdb *g) { return exec_simple(g, "-exec-step"); }
int gdb_next_instruction(Gdb *g) { return exec_simple(g, "-exec-next-instruction"); }
int gdb_step_instruction(Gdb *g) { return exec_simple(g, "-exec-step-instruction"); }
int gdb_finish(Gdb *g) {
    /* GDB rejects "finish" in the outermost frame.  For main(), continuing
       has the intended user-facing meaning: run until main returns/exits. */
    if (g->frame_count == 1 && g->frames[0].level == 0) {
        int rc = exec_simple(g, "-exec-continue");
        if (!rc) copy_text(g->message, sizeof(g->message),
                           gtext(g, "Outermost frame: continuing until program exit", "最外フレームです。プログラム終了まで続行します"));
        return rc;
    }
    return exec_simple(g, "-exec-finish");
}

int gdb_select_frame(Gdb *g, int level) {
    char result[4096];
    if (g->state != GDB_STOPPED) {
        copy_text(g->message, sizeof(g->message),
                  gtext(g, "Stop the program before selecting a stack frame", "スタックフレームを選択する前にプログラムを停止してください"));
        return -1;
    }
    if (request(g, result, sizeof(result), "-stack-select-frame %d", level)) return -1;
    g->selected_frame = level;
    if (gdb_refresh(g)) return -1;
    snprintf(g->message, sizeof(g->message), gtext(g, "Selected stack frame #%d", "スタックフレーム #%d を選択しました"), level);
    return 0;
}

static int break_at(Gdb *g, const char *file, int line, const char *condition) {
    char loc[GD_PATH_MAX + 64], qloc[GD_PATH_MAX * 2], qcond[1200], result[16384];
    snprintf(loc, sizeof(loc), "%s:%d", file, line); quote_mi(loc, qloc, sizeof(qloc));
    int rc = condition ? (quote_mi(condition, qcond, sizeof(qcond)), request(g, result, sizeof(result), "-break-insert -c %s %s", qcond, qloc))
                       : request(g, result, sizeof(result), "-break-insert %s", qloc);
    if (!rc) { snprintf(g->message, sizeof(g->message), gtext(g, "Breakpoint set at %.700s:%d", "Breakpointを設定しました: %.700s:%d"), file, line); gdb_refresh_breakpoints(g); }
    return rc;
}

int gdb_toggle_breakpoint(Gdb *g, const char *file, int line) {
    for (int i = 0; i < g->break_count; ++i)
        if (!g->breaks[i].watchpoint && g->breaks[i].line == line &&
            (!strcmp(g->breaks[i].file, file) || strstr(file, g->breaks[i].file)))
            return gdb_delete_breakpoint(g, g->breaks[i].number);
    return break_at(g, file, line, NULL);
}
int gdb_set_cond_breakpoint(Gdb *g, const char *file, int line, const char *condition) { return break_at(g, file, line, condition); }

int gdb_set_function_breakpoint(Gdb *g, const char *function) {
    if (!function || !*function) return -1;
    char quoted[2048], result[16384];
    quote_mi(function, quoted, sizeof(quoted));
    if (request(g, result, sizeof(result), "-break-insert %s", quoted)) return -1;
    bool pending = strstr(result, "<PENDING>") || strstr(result, "pending=");
    int rc = gdb_refresh_breakpoints(g);
    snprintf(g->message, sizeof(g->message), pending ?
             gtext(g, "Breakpoint pending: %.850s", "Breakpointを保留しました: %.850s") : gtext(g, "Breakpoint set: %.900s", "Breakpointを設定しました: %.900s"), function);
    return rc;
}

static int console_command(Gdb *g, const char *command, char *output,
                           size_t output_size) {
    char quoted[4096], result[16384];
    char saved_output[sizeof(g->output)];
    size_t before = g->output_len;
    memcpy(saved_output, g->output, before + 1);
    quote_mi(command, quoted, sizeof(quoted));
    if (request(g, result, sizeof(result), "-interpreter-exec console %s", quoted)) {
        memcpy(g->output, saved_output, before + 1); g->output_len = before;
        return -1;
    }
    if (output && output_size) {
        if (before > g->output_len) before = 0;
        copy_text(output, output_size, g->output + before);
    }
    size_t saved_len = strlen(saved_output);
    memcpy(g->output, saved_output, saved_len + 1); g->output_len = saved_len;
    return 0;
}

static bool valid_catch_event(const char *event) {
    return event && (!strcmp(event, "syscall") || !strcmp(event, "signal") ||
                     !strcmp(event, "fork") || !strcmp(event, "vfork") ||
                     !strcmp(event, "exec") || !strcmp(event, "load"));
}

int gdb_set_catchpoint(Gdb *g, const char *event, const char *target) {
    if (!valid_catch_event(event)) {
        copy_text(g->message, sizeof(g->message), gtext(g, "ERROR: unsupported catch event", "エラー: 未対応のCatchイベントです"));
        return -1;
    }
    if ((!strcmp(event, "syscall") || !strcmp(event, "signal")) &&
        (!target || !target[0])) {
        snprintf(g->message, sizeof(g->message), gtext(g, "ERROR: %s name is required", "エラー: %s名が必要です"), event);
        return -1;
    }
    char command[1024], output[4096];
    snprintf(command, sizeof(command), "catch %s%s%s", event,
             target && target[0] ? " " : "", target && target[0] ? target : "");
    if (console_command(g, command, output, sizeof(output))) return -1;
    if (gdb_refresh_breakpoints(g)) return -1;
    snprintf(g->message, sizeof(g->message), gtext(g, "Catchpoint set: %s%s%s", "Catchpointを設定しました: %s%s%s"), event,
             target && target[0] ? " " : "", target && target[0] ? target : "");
    return 0;
}

int gdb_list_catch_candidates(Gdb *g, const char *event, const char *filter,
                              char candidates[][128], int max_candidates,
                              int *candidate_count) {
    if (candidate_count) *candidate_count = 0;
    if (!candidate_count || !candidates || max_candidates < 1 ||
        (!event || (strcmp(event, "syscall") && strcmp(event, "signal"))))
        return -1;
    char command[512], output[32768];
    snprintf(command, sizeof(command), "complete catch %s %s", event,
             filter ? filter : "");
    if (console_command(g, command, output, sizeof(output))) return -1;
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "catch %s ", event);
    size_t prefix_len = strlen(prefix);
    char *line = output;
    while (*line && *candidate_count < max_candidates) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        while (*line == ' ' || *line == '\t') line++;
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' '))
            line[--len] = '\0';
        if (!strncmp(line, prefix, prefix_len) && line[prefix_len]) {
            const char *name = line + prefix_len;
            bool duplicate = false;
            for (int i = 0; i < *candidate_count; ++i)
                if (!strcmp(candidates[i], name)) duplicate = true;
            if (!duplicate) copy_text(candidates[(*candidate_count)++], 128, name);
        }
        if (!end) break;
        line = end + 1;
    }
    return 0;
}

typedef struct {
    unsigned long long from;
    unsigned long long to;
    char name[GD_PATH_MAX];
} GdbModuleRange;

static const char *base_name(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : path ? path : "";
}

static int list_module_ranges(Gdb *g, GdbModuleRange *modules, int max) {
    char result[65536];
    if (request(g, result, sizeof(result), "-file-list-shared-libraries")) return 0;
    int count = 0;
    const char *p = result;
    while (count < max && (p = strstr(p, "from=\""))) {
        const char *record = p;
        while (record > result && *record != '{') --record;
        const char *end = *record == '{' ? mi_record_end(record) : NULL;
        if (!end) break;
        char item[8192], from[128] = "", to[128] = "", path[GD_PATH_MAX] = "";
        size_t len = (size_t)(end - record + 1);
        if (len >= sizeof(item)) len = sizeof(item) - 1;
        memcpy(item, record, len); item[len] = '\0';
        mi_string(item, "from", from, sizeof(from));
        mi_string(item, "to", to, sizeof(to));
        mi_string(item, "shared-lib", path, sizeof(path));
        if (from[0] && to[0] && path[0]) {
            modules[count].from = strtoull(from, NULL, 16);
            modules[count].to = strtoull(to, NULL, 16);
            copy_text(modules[count].name, sizeof(modules[count].name),
                      base_name(path));
            count++;
        }
        p = end + 1;
    }
    return count;
}

static void literal_function_regex(const char *filter, char *out, size_t size) {
    static const char *special = ".^$*+?()[]{}|\\";
    size_t n = 0;
    if (n + 2 < size) { out[n++] = '.'; out[n++] = '*'; }
    for (const char *p = filter; p && *p && n + 3 < size; ++p) {
        if (strchr(special, *p)) out[n++] = '\\';
        out[n++] = *p;
    }
    if (n + 2 < size) { out[n++] = '.'; out[n++] = '*'; }
    out[n] = '\0';
}

static bool same_function(const GdbFunction *a, const GdbFunction *b) {
    return !strcmp(a->name, b->name) && !strcmp(a->module, b->module) &&
           !strcmp(a->address, b->address);
}

static void add_function(GdbFunction *functions, int *count, int max,
                         const GdbFunction *candidate) {
    if (!candidate->name[0] || *count >= max) return;
    for (int i = 0; i < *count; ++i)
        if (same_function(&functions[i], candidate)) return;
    functions[(*count)++] = *candidate;
}

int gdb_list_functions(Gdb *g, const char *filter, GdbFunction *functions,
                       int max_functions, int *function_count) {
    char result[65536], regex[1200], quoted[2400];
    *function_count = 0;
    literal_function_regex(filter ? filter : "", regex, sizeof(regex));
    quote_mi(regex, quoted, sizeof(quoted));
    if (request(g, result, sizeof(result),
                "-symbol-info-functions --include-nondebug --name %s --max-results %d",
                quoted, max_functions)) return -1;

    const char *nondebug = strstr(result, "nondebug=[");
    const char *debug = strstr(result, "debug=[");
    const char *debug_list = debug ? strchr(debug, '[') : NULL;
    const char *debug_end = debug_list ? mi_list_end(debug_list) : NULL;
    const char *p = debug_list ? debug_list + 1 : NULL;
    while (p && debug_end && p < debug_end && *function_count < max_functions) {
        const char *record = strchr(p, '{');
        if (!record || record >= debug_end) break;
        const char *end = mi_record_end(record);
        if (!end || end > debug_end) break;
        char item[32768], module[GD_PATH_MAX] = "", source[GD_PATH_MAX] = "";
        size_t len = (size_t)(end - record + 1);
        if (len >= sizeof(item)) len = sizeof(item) - 1;
        memcpy(item, record, len); item[len] = '\0';
        mi_string(item, "filename", module, sizeof(module));
        mi_string(item, "fullname", source, sizeof(source));
        if (!source[0]) copy_text(source, sizeof(source), module);
        const char *symbols = strstr(item, "symbols=[");
        const char *list = symbols ? strchr(symbols, '[') : NULL;
        const char *list_end = list ? mi_list_end(list) : NULL;
        const char *symbol = list ? list + 1 : NULL;
        while (symbol && list_end && symbol < list_end &&
               *function_count < max_functions) {
            const char *symbol_record = strchr(symbol, '{');
            if (!symbol_record || symbol_record >= list_end) break;
            const char *symbol_end = mi_record_end(symbol_record);
            if (!symbol_end || symbol_end > list_end) break;
            char symbol_item[8192];
            size_t symbol_len = (size_t)(symbol_end - symbol_record + 1);
            if (symbol_len >= sizeof(symbol_item)) symbol_len = sizeof(symbol_item) - 1;
            memcpy(symbol_item, symbol_record, symbol_len);
            symbol_item[symbol_len] = '\0';
            GdbFunction candidate;
            memset(&candidate, 0, sizeof(candidate));
            mi_string(symbol_item, "name", candidate.name, sizeof(candidate.name));
            mi_string(symbol_item, "address", candidate.address,
                      sizeof(candidate.address));
            copy_text(candidate.source, sizeof(candidate.source), source);
            candidate.line = mi_int(symbol_item, "line", 0);
            copy_text(candidate.module, sizeof(candidate.module), base_name(module));
            candidate.debug_symbol = true;
            add_function(functions, function_count, max_functions, &candidate);
            symbol = symbol_end + 1;
        }
        p = end + 1;
    }

    GdbModuleRange modules[128];
    int module_count = list_module_ranges(g, modules, 128);
    const char *nondebug_list = nondebug ? strchr(nondebug, '[') : NULL;
    const char *nondebug_end = nondebug_list ? mi_list_end(nondebug_list) : NULL;
    p = nondebug_list ? nondebug_list + 1 : NULL;
    while (p && nondebug_end && p < nondebug_end &&
           *function_count < max_functions) {
        const char *record = strchr(p, '{');
        if (!record || record >= nondebug_end) break;
        const char *end = mi_record_end(record);
        if (!end || end > nondebug_end) break;
        char item[8192];
        size_t len = (size_t)(end - record + 1);
        if (len >= sizeof(item)) len = sizeof(item) - 1;
        memcpy(item, record, len); item[len] = '\0';
        GdbFunction candidate;
        memset(&candidate, 0, sizeof(candidate));
        mi_string(item, "name", candidate.name, sizeof(candidate.name));
        mi_string(item, "address", candidate.address, sizeof(candidate.address));
        unsigned long long address = strtoull(candidate.address, NULL, 16);
        copy_text(candidate.module, sizeof(candidate.module),
                  base_name(g->executable));
        for (int i = 0; i < module_count; ++i)
            if (address >= modules[i].from && address < modules[i].to) {
                copy_text(candidate.module, sizeof(candidate.module), modules[i].name);
                break;
            }
        add_function(functions, function_count, max_functions, &candidate);
        p = end + 1;
    }
    if (*function_count)
        snprintf(g->message, sizeof(g->message), gtext(g, "%d function candidate%s", "関数候補: %d件%s"),
                 *function_count, g->japanese ? "" : (*function_count == 1 ? "" : "s"));
    else if (filter && filter[0])
        copy_text(g->message, sizeof(g->message), gtext(g, "No matching function symbols.", "一致する関数シンボルがありません。"));
    else
        copy_text(g->message, sizeof(g->message),
                  gtext(g, "No function symbols available. Use address breakpoint in Assembly Mode.", "関数シンボルを取得できません。Assembly ModeでアドレスBreakpointを使用してください。"));
    return 0;
}

int gdb_list_source_files(Gdb *g, GdbSourceFile *files, int max_files,
                          int *file_count) {
    char result[65536];
    *file_count = 0;
    if (request(g, result, sizeof(result), "-file-list-exec-source-files"))
        return -1;
    const char *files_key = strstr(result, "files=[");
    const char *list = files_key ? strchr(files_key, '[') : NULL;
    const char *list_end = list ? mi_list_end(list) : NULL;
    const char *p = list ? list + 1 : NULL;
    while (p && list_end && p < list_end && *file_count < max_files) {
        const char *record = strchr(p, '{');
        if (!record || record >= list_end) break;
        const char *end = mi_record_end(record);
        if (!end || end > list_end) break;
        char item[8192];
        size_t len = (size_t)(end - record + 1);
        if (len >= sizeof(item)) len = sizeof(item) - 1;
        memcpy(item, record, len); item[len] = '\0';
        GdbSourceFile candidate;
        memset(&candidate, 0, sizeof(candidate));
        mi_string(item, "file", candidate.file, sizeof(candidate.file));
        mi_string(item, "fullname", candidate.fullname,
                  sizeof(candidate.fullname));
        bool duplicate = false;
        for (int i = 0; i < *file_count; ++i)
            if ((candidate.fullname[0] &&
                 !strcmp(files[i].fullname, candidate.fullname)) ||
                (!candidate.fullname[0] &&
                 !strcmp(files[i].file, candidate.file))) {
                duplicate = true;
                break;
            }
        if (!duplicate && (candidate.file[0] || candidate.fullname[0]))
            files[(*file_count)++] = candidate;
        p = end + 1;
    }
    return 0;
}

int gdb_toggle_address_breakpoint(Gdb *g, const char *address) {
    if (!address || strncmp(address, "0x", 2)) {
        copy_text(g->message, sizeof(g->message), gtext(g, "ERROR: invalid instruction address", "エラー: 命令アドレスが不正です"));
        return -1;
    }
    unsigned long long target = strtoull(address, NULL, 16);
    for (int i = 0; i < g->break_count; ++i) {
        GdbBreakpoint *b = &g->breaks[i];
        if (!b->watchpoint && b->address[0] &&
            strtoull(b->address, NULL, 16) == target)
            return gdb_delete_breakpoint(g, b->number);
    }
    char result[16384];
    if (request(g, result, sizeof(result), "-break-insert *%s", address)) return -1;
    snprintf(g->message, sizeof(g->message), gtext(g, "Breakpoint set: *%.900s", "Breakpointを設定しました: *%.900s"), address);
    return gdb_refresh_breakpoints(g);
}

int gdb_watch(Gdb *g, const char *expression) {
    char q[2048], result[8192]; quote_mi(expression, q, sizeof(q));
    int rc = request(g, result, sizeof(result), "-break-watch %s", q);
    if (!rc) { snprintf(g->message, sizeof(g->message), gtext(g, "Watchpoint set: %.900s", "Watchpointを設定しました: %.900s"), expression); gdb_refresh_breakpoints(g); }
    return rc;
}

static int evaluate_expression(Gdb *g, const char *expression, char *out,
                               size_t out_size) {
    char q[2048], result[16384]; quote_mi(expression, q, sizeof(q));
    if (request(g, result, sizeof(result), "-data-evaluate-expression %s", q)) return -1;
    if (!mi_string(result, "value", out, out_size)) copy_text(out, out_size, "<unavailable>");
    return 0;
}

int gdb_print(Gdb *g, const char *expression, char *out, size_t out_size) {
    if (evaluate_expression(g, expression, out, out_size)) return -1;
    snprintf(g->message, sizeof(g->message), "%.300s = %.650s", expression, out);
    return 0;
}

int gdb_assign_expression(Gdb *g, const char *expression, const char *new_value,
                          char *actual, size_t actual_size) {
    if (g->state != GDB_STOPPED) {
        copy_text(g->message, sizeof(g->message),
                  gtext(g, "Stop the program before modifying a value", "値を変更する前にプログラムを停止してください"));
        return -1;
    }
    if (!expression || !expression[0] || !new_value || !new_value[0]) {
        copy_text(g->message, sizeof(g->message), gtext(g, "ERROR: expression and value are required", "エラー: 式と値が必要です"));
        return -1;
    }
    char assignment[4096], quoted[8192], result[16384];
    snprintf(assignment, sizeof(assignment), "(%.*s) = (%.*s)",
             1800, expression, 1800, new_value);
    quote_mi(assignment, quoted, sizeof(quoted));
    if (request(g, result, sizeof(result), "-data-evaluate-expression %s", quoted))
        return -1;
    if (!mi_string(result, "value", actual, actual_size))
        copy_text(actual, actual_size, new_value);
    if (gdb_refresh(g)) return -1;
    /* Read back through GDB so casts, aliases, and truncation are reflected. */
    if (evaluate_expression(g, expression, actual, actual_size)) return -1;
    return 0;
}

int gdb_assign_register(Gdb *g, const char *name, const char *new_value,
                        char *actual, size_t actual_size) {
    if (!name || !name[0]) {
        copy_text(g->message, sizeof(g->message), gtext(g, "ERROR: register name is required", "エラー: レジスタ名が必要です"));
        return -1;
    }
    char expression[128];
    snprintf(expression, sizeof(expression), "$%s", name);
    return gdb_assign_expression(g, expression, new_value, actual, actual_size);
}

int gdb_current_return_type(Gdb *g, char *type, size_t type_size,
                            bool *is_void) {
    copy_text(type, type_size, "<unknown>");
    if (is_void) *is_void = false;
    if (!g->function[0] || !strcmp(g->function, "??")) return -1;
    char quoted[1024], result[16384], object[256] = "", function_type[512] = "";
    quote_mi(g->function, quoted, sizeof(quoted));
    if (request(g, result, sizeof(result), "-var-create - * %s", quoted)) return -1;
    mi_string(result, "name", object, sizeof(object));
    mi_string(result, "type", function_type, sizeof(function_type));
    if (object[0]) {
        char cleanup[1024];
        request(g, cleanup, sizeof(cleanup), "-var-delete %s", object);
    }
    if (!function_type[0] || strstr(function_type, "no debug info")) return -1;
    char *paren = strchr(function_type, '(');
    if (paren) *paren = '\0';
    size_t len = strlen(function_type);
    while (len && isspace((unsigned char)function_type[len - 1]))
        function_type[--len] = '\0';
    if (!function_type[0]) return -1;
    copy_text(type, type_size, function_type);
    if (is_void) *is_void = !strcmp(function_type, "void");
    return 0;
}

int gdb_force_return(Gdb *g, const char *value) {
    if (g->state != GDB_STOPPED) {
        copy_text(g->message, sizeof(g->message),
                  gtext(g, "Stop the program before forcing a return", "強制returnの前にプログラムを停止してください"));
        return -1;
    }
    char command[4096], quoted[8192], result[16384];
    if (value && value[0])
        snprintf(command, sizeof(command), "return %.*s", 4000, value);
    else
        copy_text(command, sizeof(command), "return");
    quote_mi(command, quoted, sizeof(quoted));
    size_t output_before = g->output_len;
    int return_rc = request(g, result, sizeof(result),
                            "-interpreter-exec console %s", quoted);
    const char *new_output = output_before <= g->output_len ?
                             g->output + output_before : g->output;
    bool type_unavailable = strstr(new_output,
                                   "Return value type not available") != NULL;
    if (return_rc || type_unavailable) {
        /* Without debug type information GDB rejects `return VALUE`.  On the
           supported x86-64 System V target, perform a valueless frame return
           and then place the user-supplied raw integer/pointer result in rax.
           Do not guess floating-point or aggregate return conventions. */
        if (!value || !value[0] || !g->amd64_sysv || !type_unavailable) return -1;
        quote_mi("return", quoted, sizeof(quoted));
        if (request(g, result, sizeof(result), "-interpreter-exec console %s", quoted) ||
            gdb_refresh(g)) return -1;
        char actual[GD_TEXT_MAX];
        if (gdb_assign_register(g, "rax", value, actual, sizeof(actual))) return -1;
        copy_text(g->message, sizeof(g->message),
                  gtext(g, "Forced return using raw System V integer result register rax", "System V整数戻り値レジスタraxを使用して強制returnしました"));
        return 0;
    }
    return gdb_refresh(g);
}

static bool normalize_address(const char *value, char *out, size_t out_size) {
    const char *hex = value ? strstr(value, "0x") : NULL;
    if (!hex) {
        if (value && (!strcmp(value, "0") || strstr(value, "(nil)"))) {
            copy_text(out, out_size, "0x0");
            return true;
        }
        if (out_size) out[0] = '\0';
        return false;
    }
    const char *p = hex + 2;
    while (*p == '0') p++;
    const char *end = p;
    while (isxdigit((unsigned char)*end)) end++;
    if (end == p) {
        copy_text(out, out_size, "0x0");
        return true;
    }
    size_t digits = (size_t)(end - p);
    if (digits + 3 > out_size) digits = out_size > 3 ? out_size - 3 : 0;
    if (out_size < 4 || !digits) return false;
    out[0] = '0'; out[1] = 'x';
    for (size_t i = 0; i < digits; i++) out[i + 2] = (char)tolower((unsigned char)p[i]);
    out[digits + 2] = '\0';
    return true;
}

int gdb_expression_address(Gdb *g, const char *expression, char *address,
                           size_t address_size) {
    if (address_size) address[0] = '\0';
    if (g->state != GDB_STOPPED) return -1;
    char address_expression[1200], value[GD_TEXT_MAX];
    snprintf(address_expression, sizeof(address_expression), "&(%.*s)", 1100,
             expression);
    if (evaluate_expression(g, address_expression, value, sizeof(value))) return -1;
    if (strstr(value, "<optimized out>")) return -1;
    return normalize_address(value, address, address_size) ? 0 : -1;
}

int gdb_inspect_address(Gdb *g, const char *expression, GdbAddressInfo *info) {
    memset(info, 0, sizeof(*info));
    copy_text(info->expression, sizeof(info->expression), expression);
    if (g->state != GDB_STOPPED) {
        copy_text(g->message, sizeof(g->message),
                  gtext(g, "Stop the program before inspecting addresses", "アドレスを確認する前にプログラムを停止してください"));
        return -1;
    }

    char quoted[2048], result[16384], object[256] = "";
    quote_mi(expression, quoted, sizeof(quoted));
    if (request(g, result, sizeof(result), "-var-create - * %s", quoted)) return -1;
    if (!mi_string(result, "name", object, sizeof(object))) {
        copy_text(g->message, sizeof(g->message),
                  gtext(g, "ERROR: GDB did not create a variable object", "エラー: GDBが変数オブジェクトを作成できませんでした"));
        return -1;
    }
    mi_string(result, "type", info->type, sizeof(info->type));
    mi_string(result, "value", info->value, sizeof(info->value));
    char cleanup[1024];
    request(g, cleanup, sizeof(cleanup), "-var-delete %s", object);
    if (!info->type[0]) copy_text(info->type, sizeof(info->type), "<unknown>");
    if (!info->value[0]) copy_text(info->value, sizeof(info->value), "<unavailable>");

    info->optimized_out = strstr(info->value, "<optimized out>") != NULL;
    info->pointer = strchr(info->type, '*') != NULL;
    if (info->optimized_out) {
        copy_text(info->address, sizeof(info->address), "<optimized out>");
        copy_text(info->points_to, sizeof(info->points_to), "<optimized out>");
        snprintf(g->message, sizeof(g->message),
                 gtext(g, "Address unavailable (optimized out): %.700s", "アドレスを取得できません（最適化により削除）: %.700s"), expression);
        return 0;
    }

    if (info->pointer) {
        info->address_available = normalize_address(info->value, info->address,
                                                    sizeof(info->address));
        info->is_null = info->address_available && !strcmp(info->address, "0x0");
        if (info->is_null) {
            copy_text(info->points_to, sizeof(info->points_to), "NULL");
            info->points_to_available = true;
        } else if (info->address_available) {
            char dereference[1200];
            snprintf(dereference, sizeof(dereference), "*(%.*s)", 1100, expression);
            if (!evaluate_expression(g, dereference, info->points_to,
                                     sizeof(info->points_to))) {
                info->points_to_available = true;
            } else {
                copy_text(info->points_to, sizeof(info->points_to), "<unavailable>");
            }
        } else {
            copy_text(info->address, sizeof(info->address), "<unavailable>");
            copy_text(info->points_to, sizeof(info->points_to), "<unavailable>");
        }
    } else if (!gdb_expression_address(g, expression, info->address,
                                       sizeof(info->address))) {
        info->address_available = true;
    } else {
        copy_text(info->address, sizeof(info->address), "<unavailable>");
    }
    snprintf(g->message, sizeof(g->message), gtext(g, "Address details: %.800s", "アドレス詳細: %.800s"), expression);
    return 0;
}

static const char *mi_record_end(const char *start) {
    int depth = 0;
    bool quoted = false, escaped = false;
    for (const char *p = start; *p; ++p) {
        if (escaped) { escaped = false; continue; }
        if (quoted && *p == '\\') { escaped = true; continue; }
        if (*p == '"') { quoted = !quoted; continue; }
        if (quoted) continue;
        if (*p == '{') depth++;
        else if (*p == '}' && --depth == 0) return p;
    }
    return NULL;
}

int gdb_list_children(Gdb *g, const char *expression, GdbChild *children,
                      int max_children, int *child_count) {
    char quoted[2048], result[65536], object[256], parent_type[256] = "";
    *child_count = 0;
    quote_mi(expression, quoted, sizeof(quoted));
    if (request(g, result, sizeof(result), "-var-create - * %s", quoted)) return -1;
    if (!mi_string(result, "name", object, sizeof(object))) {
        copy_text(g->message, sizeof(g->message), gtext(g, "ERROR: GDB did not create a variable object", "エラー: GDBが変数オブジェクトを作成できませんでした"));
        return -1;
    }
    mi_string(result, "type", parent_type, sizeof(parent_type));
    int rc = request(g, result, sizeof(result), "-var-list-children --simple-values %s", object);
    if (!rc) {
        const char *p = result;
        while (*child_count < max_children && (p = strstr(p, "child={"))) {
            const char *end = mi_record_end(strchr(p, '{'));
            if (!end) break;
            char item[8192], exp[256] = "";
            size_t n = (size_t)(end - p + 1);
            if (n >= sizeof(item)) n = sizeof(item) - 1;
            memcpy(item, p, n); item[n] = '\0';
            GdbChild *c = &children[(*child_count)++];
            memset(c, 0, sizeof(*c));
            mi_string(item, "exp", exp, sizeof(exp));
            copy_text(c->name, sizeof(c->name), exp[0] ? exp : "?");
            mi_string(item, "value", c->value, sizeof(c->value));
            if (!c->value[0]) copy_text(c->value, sizeof(c->value), "{...}");
            mi_string(item, "type", c->type, sizeof(c->type));
            c->numchild = mi_int(item, "numchild", 0);
            if (exp[0] == '*') snprintf(c->expression, sizeof(c->expression), "*(%.*s)", 500, expression);
            else if (exp[0] == '[') snprintf(c->expression, sizeof(c->expression), "%.*s%.*s", 350, expression, 150, exp);
            else if (strchr(parent_type, '*')) snprintf(c->expression, sizeof(c->expression), "(%.*s)->%.*s", 330, expression, 150, exp);
            else snprintf(c->expression, sizeof(c->expression), "(%.*s).%.*s", 340, expression, 150, exp);
            p = end + 1;
        }
    }
    char cleanup[1024];
    request(g, cleanup, sizeof(cleanup), "-var-delete %s", object);
    if (!rc) {
        snprintf(g->message, sizeof(g->message), *child_count ?
                 gtext(g, "Expanded %.700s (%d children)", "展開しました: %.700s（子要素%d件）") : gtext(g, "No children: %.900s", "子要素がありません: %.900s"),
                 expression, *child_count);
    }
    return rc;
}

int gdb_delete_breakpoint(Gdb *g, int number) {
    char result[4096];
    if (request(g, result, sizeof(result), "-break-delete %d", number)) return -1;
    snprintf(g->message, sizeof(g->message), gtext(g, "Breakpoint %d deleted", "Breakpoint %dを削除しました"), number); return gdb_refresh_breakpoints(g);
}
int gdb_enable_breakpoint(Gdb *g, int number, bool enable) {
    char result[4096];
    if (request(g, result, sizeof(result), "-break-%s %d", enable ? "enable" : "disable", number)) return -1;
    snprintf(g->message, sizeof(g->message), gtext(g, "Breakpoint %d %s", "Breakpoint %dを%sにしました"), number, enable ? gtext(g,"enabled","有効") : gtext(g,"disabled","無効")); return gdb_refresh_breakpoints(g);
}

static const char *mi_list_end(const char *start) {
    int depth = 0;
    bool quoted = false, escaped = false;
    for (const char *p = start; *p; ++p) {
        if (escaped) { escaped = false; continue; }
        if (quoted && *p == '\\') { escaped = true; continue; }
        if (*p == '"') { quoted = !quoted; continue; }
        if (quoted) continue;
        if (*p == '[') depth++;
        else if (*p == ']' && --depth == 0) return p;
    }
    return NULL;
}

static bool same_var(const GdbVar *a, const GdbVar *b) {
    return !strcmp(a->name, b->name) && !strcmp(a->type, b->type) &&
           !strcmp(a->value, b->value);
}

static int parse_var_list(const char *list, GdbVar *vars, int max) {
    if (!list || *list != '[') return 0;
    const char *list_end = mi_list_end(list);
    if (!list_end) return 0;
    int count = 0;
    const char *p = list + 1;
    while (p < list_end && count < max) {
        const char *record = strchr(p, '{');
        if (!record || record >= list_end) break;
        const char *end = mi_record_end(record);
        if (!end || end > list_end) break;
        char item[8192];
        size_t len = (size_t)(end - record + 1);
        if (len >= sizeof(item)) len = sizeof(item) - 1;
        memcpy(item, record, len); item[len] = '\0';
        GdbVar candidate;
        memset(&candidate, 0, sizeof(candidate));
        if (mi_string(item, "name", candidate.name, sizeof(candidate.name))) {
            if (!mi_string(item, "value", candidate.value,
                           sizeof(candidate.value)))
                copy_text(candidate.value, sizeof(candidate.value), "?");
            mi_string(item, "type", candidate.type, sizeof(candidate.type));
            bool duplicate = false;
            for (int i = 0; i < count; ++i)
                if (same_var(&vars[i], &candidate)) { duplicate = true; break; }
            if (!duplicate) vars[count++] = candidate;
        }
        p = end + 1;
    }
    return count;
}

static int parse_stack_arguments(const char *result, int selected_level,
                                 GdbVar *vars, int max) {
    const char *stack_args = strstr(result, "stack-args=[");
    if (!stack_args) return 0;
    const char *outer = strchr(stack_args, '[');
    const char *outer_end = outer ? mi_list_end(outer) : NULL;
    if (!outer_end) return 0;
    const char *p = outer + 1;
    while (p < outer_end) {
        const char *frame = strstr(p, "frame={");
        if (!frame || frame >= outer_end) break;
        const char *record = strchr(frame, '{');
        const char *end = record ? mi_record_end(record) : NULL;
        if (!end || end > outer_end) break;
        char item[16384];
        size_t len = (size_t)(end - record + 1);
        if (len >= sizeof(item)) len = sizeof(item) - 1;
        memcpy(item, record, len); item[len] = '\0';
        if (mi_int(item, "level", -1) == selected_level) {
            const char *args = strstr(item, "args=[");
            return args ? parse_var_list(strchr(args, '['), vars, max) : 0;
        }
        p = end + 1;
    }
    return 0;
}

static int parse_stack_locals(const char *result, GdbVar *vars, int max) {
    const char *locals = strstr(result, "locals=[");
    return locals ? parse_var_list(strchr(locals, '['), vars, max) : 0;
}

static int argument_register(const char *name) {
    static const char *args[] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };
    for (int i = 0; i < 6; ++i) if (!strcmp(name, args[i])) return i + 1;
    return 0;
}

static int parse_register_names(const char *result, char names[][32], int max) {
    const char *p = strstr(result, "register-names=[");
    if (!p) return 0;
    p = strchr(p, '[');
    const char *end = p ? mi_list_end(p) : NULL;
    if (!end) return 0;
    int count = 0;
    for (++p; p < end && count < max; ++p) {
        if (*p != '"') continue;
        ++p;
        int n = 0;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) ++p;
            if (n + 1 < 32) names[count][n++] = *p;
            ++p;
        }
        names[count][n] = '\0';
        count++;
    }
    return count;
}

static int find_register_number(char names[][32], int count, const char *name) {
    for (int i = 0; i < count; ++i) if (!strcmp(names[i], name)) return i;
    return -1;
}

static void refresh_registers(Gdb *g) {
    char result[65536];
    char names[256][32] = {{0}};
    GdbRegister old[GD_MAX_REGISTERS];
    int old_count = g->register_count;
    memcpy(old, g->registers, sizeof(old));
    g->register_count = 0;
    g->amd64_sysv = false;
    if (request(g, result, sizeof(result), "-data-list-register-names")) return;
    int name_count = parse_register_names(result, names, 256);
    static const char *normal_wanted[] = {
        "rdi", "rsi", "rdx", "rcx", "r8", "r9",
        "rax", "rbx", "rbp", "rsp", "rip",
        "r10", "r11", "r12", "r13", "r14", "r15", "eflags"
    };
    static const char *syscall_entry_wanted[] = {
        "orig_rax", "rdi", "rsi", "rdx", "r10", "r8", "r9", "rax",
        "rip", "rsp", "rbp", "rbx", "rcx", "r11",
        "r12", "r13", "r14", "r15", "eflags"
    };
    static const char *syscall_return_wanted[] = {
        "rax", "rdi", "rsi", "rdx", "r10", "r8", "r9", "orig_rax",
        "rip", "rsp", "rbp", "rbx", "rcx", "r11",
        "r12", "r13", "r14", "r15", "eflags"
    };
    const char **wanted = !strcmp(g->catch_event, "syscall") ?
                          (!strcmp(g->catch_phase, "Return") ?
                           syscall_return_wanted : syscall_entry_wanted) :
                          normal_wanted;
    size_t wanted_count = !strcmp(g->catch_event, "syscall") ?
                          sizeof(syscall_entry_wanted) / sizeof(syscall_entry_wanted[0]) :
                          sizeof(normal_wanted) / sizeof(normal_wanted[0]);
    bool amd64 = find_register_number(names, name_count, "rip") >= 0 &&
                 find_register_number(names, name_count, "rdi") >= 0 &&
                 find_register_number(names, name_count, "r9") >= 0;
    g->amd64_sysv = amd64;
    if (amd64) {
        for (size_t i = 0; i < wanted_count &&
                           g->register_count < GD_MAX_REGISTERS; ++i) {
            int number = find_register_number(names, name_count, wanted[i]);
            if (number < 0) continue;
            GdbRegister *r = &g->registers[g->register_count++];
            memset(r, 0, sizeof(*r));
            r->number = number;
            r->argument = argument_register(wanted[i]);
            copy_text(r->name, sizeof(r->name), wanted[i]);
        }
    } else {
        for (int i = 0; i < name_count && g->register_count < GD_MAX_REGISTERS; ++i) {
            if (!names[i][0]) continue;
            GdbRegister *r = &g->registers[g->register_count++];
            memset(r, 0, sizeof(*r));
            r->number = i;
            copy_text(r->name, sizeof(r->name), names[i]);
        }
    }
    if (request(g, result, sizeof(result), "-data-list-register-values x")) return;
    const char *values = strstr(result, "register-values=[");
    const char *list = values ? strchr(values, '[') : NULL;
    const char *list_end = list ? mi_list_end(list) : NULL;
    const char *p = list ? list + 1 : NULL;
    while (p && list_end && p < list_end) {
        const char *record = strchr(p, '{');
        if (!record || record >= list_end) break;
        const char *end = mi_record_end(record);
        if (!end || end > list_end) break;
        char item[2048];
        size_t len = (size_t)(end - record + 1);
        if (len >= sizeof(item)) len = sizeof(item) - 1;
        memcpy(item, record, len); item[len] = '\0';
        int number = mi_int(item, "number", -1);
        for (int i = 0; i < g->register_count; ++i) {
            GdbRegister *r = &g->registers[i];
            if (r->number != number) continue;
            mi_string(item, "value", r->value, sizeof(r->value));
            for (int j = 0; j < old_count; ++j) if (!strcmp(old[j].name, r->name)) {
                copy_text(r->previous, sizeof(r->previous), old[j].value);
                r->changed = old[j].value[0] && strcmp(old[j].value, r->value);
                break;
            }
            break;
        }
        p = end + 1;
    }
    if (!g->pc[0]) {
        for (int i = 0; i < g->register_count; ++i)
            if (!strcmp(g->registers[i].name, "rip") ||
                !strcmp(g->registers[i].name, "pc")) {
                copy_text(g->pc, sizeof(g->pc), g->registers[i].value);
                break;
            }
    }
}

static void call_details(GdbInstruction *instruction) {
    const char *p = instruction->instruction;
    while (isspace((unsigned char)*p)) ++p;
    const char *word_end = p;
    while (*word_end && !isspace((unsigned char)*word_end)) ++word_end;
    size_t mnemonic = (size_t)(word_end - p);
    if (!((mnemonic == 4 && !strncmp(p, "call", 4)) ||
          (mnemonic == 5 && !strncmp(p, "callq", 5)))) return;
    instruction->call = true;
    const char *left = strchr(word_end, '<');
    const char *right = left ? strchr(left + 1, '>') : NULL;
    if (left && right) {
        size_t len = (size_t)(right - left - 1);
        if (len >= sizeof(instruction->call_target))
            len = sizeof(instruction->call_target) - 1;
        memcpy(instruction->call_target, left + 1, len);
        instruction->call_target[len] = '\0';
    } else {
        while (isspace((unsigned char)*word_end)) ++word_end;
        copy_text(instruction->call_target, sizeof(instruction->call_target), word_end);
    }
}

static void refresh_disassembly_at_address(Gdb *g, const char *address) {
    g->instruction_count = 0;
    g->current_instruction = -1;
    g->disassembly_function[0] = '\0';
    if (!address || !address[0] || !strncmp(address, "<", 1)) return;
    char result[65536], saved_message[GD_TEXT_MAX];
    copy_text(saved_message, sizeof(saved_message), g->message);
    int rc = request(g, result, sizeof(result), "-data-disassemble -a %s -- 0", address);
    if (rc) {
        unsigned long long target = strtoull(address, NULL, 16);
        unsigned long long start = target > 64 ? target - 64 : 0;
        rc = request(g, result, sizeof(result),
                     "-data-disassemble -s 0x%llx -e 0x%llx -- 0",
                     start, target + 128);
    }
    copy_text(g->message, sizeof(g->message), saved_message);
    if (rc) return;
    unsigned long long pc = strtoull(g->pc, NULL, 16);
    const char *p = result;
    while (g->instruction_count < GD_MAX_INSTRUCTIONS &&
           (p = strstr(p, "address=\""))) {
        const char *record = p;
        while (record > result && *record != '{') --record;
        const char *end = *record == '{' ? mi_record_end(record) : NULL;
        if (!end) break;
        char item[4096];
        size_t len = (size_t)(end - record + 1);
        if (len >= sizeof(item)) len = sizeof(item) - 1;
        memcpy(item, record, len); item[len] = '\0';
        GdbInstruction *ins = &g->instructions[g->instruction_count];
        memset(ins, 0, sizeof(*ins));
        mi_string(item, "address", ins->address, sizeof(ins->address));
        mi_string(item, "func-name", ins->function, sizeof(ins->function));
        mi_string(item, "inst", ins->instruction, sizeof(ins->instruction));
        if (ins->address[0] && ins->instruction[0]) {
            if (!g->disassembly_function[0] && ins->function[0])
                copy_text(g->disassembly_function, sizeof(g->disassembly_function),
                          ins->function);
            ins->current = strtoull(ins->address, NULL, 16) == pc;
            call_details(ins);
            if (ins->current) g->current_instruction = g->instruction_count;
            g->instruction_count++;
        }
        p = end + 1;
    }
}

static void refresh_disassembly(Gdb *g) {
    refresh_disassembly_at_address(g, g->pc);
}

int gdb_disassemble_at(Gdb *g, const char *address) {
    if (!address || !address[0] || address[0] == '<') {
        copy_text(g->message, sizeof(g->message),
                  gtext(g, "Function address is not currently available", "関数アドレスを現在取得できません"));
        return -1;
    }
    refresh_disassembly_at_address(g, address);
    if (!g->instruction_count) {
        copy_text(g->message, sizeof(g->message),
                  gtext(g, "Disassembly is unavailable for this function", "この関数を逆アセンブルできません"));
        return -1;
    }
    snprintf(g->message, sizeof(g->message), gtext(g, "Viewing function at %.900s", "関数を表示中: %.900s"), address);
    return 0;
}

static bool source_break_location(const char *location) {
    const char *colon = location ? strrchr(location, ':') : NULL;
    if (!colon || !isdigit((unsigned char)colon[1])) return false;
    for (const char *p = colon + 1; *p; ++p)
        if (!isdigit((unsigned char)*p)) return false;
    return true;
}

int gdb_refresh_breakpoints(Gdb *g) {
    char result[65536];
    if (request(g, result, sizeof(result), "-break-list")) return -1;
    g->break_count = 0; const char *p = result;
    while (g->break_count < GD_MAX_BREAKS) {
        const char *bp = strstr(p, "bkpt={");
        const char *wp = strstr(p, "wpt={");
        if (!bp || (wp && wp < bp)) bp = wp;
        if (!bp) break;
        p = bp;
        const char *end = strchr(p, '}'); if (!end) break;
        char item[8192]; size_t n = (size_t)(end - p + 1); if (n >= sizeof(item)) n = sizeof(item) - 1;
        memcpy(item, p, n); item[n] = '\0';
        GdbBreakpoint *b = &g->breaks[g->break_count]; memset(b, 0, sizeof(*b));
        b->number = mi_int(item, "number", 0); char enabled[16] = "y", type[128] = "";
        mi_string(item, "enabled", enabled, sizeof(enabled)); b->enabled = enabled[0] == 'y';
        mi_string(item, "type", type, sizeof(type)); b->watchpoint = (p[0] == 'w') || strstr(type, "watchpoint") != NULL;
        b->catchpoint = strstr(type, "catchpoint") != NULL;
        mi_string(item, "fullname", b->file, sizeof(b->file)); if (!b->file[0]) mi_string(item, "file", b->file, sizeof(b->file));
        mi_string(item, "addr", b->address, sizeof(b->address));
        mi_string(item, "func", b->function, sizeof(b->function));
        mi_string(item, "original-location", b->original_location,
                  sizeof(b->original_location));
        b->pending = strstr(b->address, "<PENDING>") != NULL ||
                     strstr(item, "pending=") != NULL;
        b->function_breakpoint = !b->catchpoint && b->original_location[0] &&
                                 b->original_location[0] != '*' &&
                                 !source_break_location(b->original_location);
        if (b->function_breakpoint && !b->function[0])
            copy_text(b->function, sizeof(b->function), b->original_location);
        b->line = mi_int(item, "line", 0); mi_string(item, "cond", b->condition, sizeof(b->condition));
        if (b->watchpoint && !b->condition[0]) {
            if (!mi_string(item, "exp", b->condition, sizeof(b->condition)))
                mi_string(item, "what", b->condition, sizeof(b->condition));
        }
        if (b->catchpoint) {
            char what[512] = "";
            mi_string(item, "what", what, sizeof(what));
            if (!mi_string(item, "catch-type", b->catch_type,
                           sizeof(b->catch_type)))
                copy_text(b->catch_type, sizeof(b->catch_type), "event");
            if (what[0] && strcmp(what, "load of library"))
                copy_text(b->catch_target, sizeof(b->catch_target), what);
        }
        ++g->break_count; p = end + 1;
    }
    g->changed = true; return 0;
}

int gdb_refresh(Gdb *g) {
    char result[65536];
    g->fullname[0] = g->file[0] = g->function[0] = g->pc[0] = '\0';
    g->line = 0;
    if (!request(g, result, sizeof(result), "-stack-info-frame")) {
        g->selected_frame = mi_int(result, "level", g->selected_frame);
        mi_string(result, "fullname", g->fullname, sizeof(g->fullname)); mi_string(result, "file", g->file, sizeof(g->file));
        mi_string(result, "func", g->function, sizeof(g->function)); g->line = mi_int(result, "line", 0);
        mi_string(result, "addr", g->pc, sizeof(g->pc));
    }
    g->source_available = g->fullname[0] && g->line > 0;
    g->arg_count = 0;
    g->local_count = 0;
    if (g->source_available) {
        if (!request(g, result, sizeof(result),
                     "-stack-list-arguments --simple-values %d %d",
                     g->selected_frame, g->selected_frame))
            g->arg_count = parse_stack_arguments(result, g->selected_frame, g->args,
                                                  GD_MAX_VARS);
        /* Locals are taken only from the selected frame.  Do not merge
           -stack-list-variables here: it includes arguments and causes duplicates. */
        if (!request(g, result, sizeof(result), "-stack-list-locals --simple-values"))
            g->local_count = parse_stack_locals(result, g->locals, GD_MAX_VARS);
    }
    g->frame_count = 0;
    if (!request(g, result, sizeof(result), "-stack-list-frames")) {
        const char *p = result;
        while (g->frame_count < GD_MAX_FRAMES && (p = strstr(p, "frame={"))) {
            const char *end = strchr(p, '}'); if (!end) break;
            char item[8192]; size_t n = (size_t)(end - p + 1); if (n >= sizeof(item)) n = sizeof(item)-1;
            memcpy(item, p, n); item[n] = '\0'; GdbFrame *f = &g->frames[g->frame_count++]; memset(f, 0, sizeof(*f));
            f->level = mi_int(item, "level", g->frame_count - 1); mi_string(item, "func", f->func, sizeof(f->func));
            mi_string(item, "fullname", f->file, sizeof(f->file)); if (!f->file[0]) mi_string(item, "file", f->file, sizeof(f->file));
            mi_string(item, "addr", f->address, sizeof(f->address));
            f->line = mi_int(item, "line", 0); p = end + 1;
        }
    }
    refresh_registers(g);
    refresh_disassembly(g);
    gdb_refresh_breakpoints(g); g->changed = true; return 0;
}
