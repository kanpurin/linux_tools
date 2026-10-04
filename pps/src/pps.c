#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define PPS_VERSION "1.0.0"
#define TEXT_MAX 8192
#define USER_CACHE_SIZE 256

typedef struct {
    pid_t pid, ppid, session, tpgid;
    uid_t uid;
    char user[64], comm[256];
    char *command;
    char state;
    long tty_nr, nice, threads;
    unsigned long long utime, stime, start_ticks, vsize;
    long long rss_pages;
    double cpu, mem;
    time_t started;
    char *row;
    bool metadata_loaded;
} Process;

typedef struct {
    Process *v;
    size_t n, cap;
    unsigned long long snapshot_ticks;
} ProcessList;

typedef enum { SORT_PID, SORT_CPU, SORT_MEM, SORT_START, SORT_TIME } SortKey;
typedef enum { ACT_INFO, ACT_FD, ACT_THREADS, ACT_TREE, ACT_LIMITS, ACT_SIGNAL } Action;
typedef enum { SCAN_DETAILS, SCAN_RELATIONS } ScanMode;

typedef struct {
    int tty;
    struct termios saved;
    bool raw, alt;
    bool buffering;
    char output[32768];
    size_t output_len;
    ProcessList list;
    size_t selected, top;
    int hscroll, rows, cols;
    SortKey sort;
    bool descending;
    char filter[4096];
    char search[1024];
    char message[1024];
    bool lazy;
} UI;

static long g_hz;
static long g_pagesize;
static double g_uptime;
static unsigned long long g_memtotal_kb;
static time_t g_boot_time;
static UI *g_ui;
static volatile sig_atomic_t g_resized;
static struct {
    uid_t uid;
    char name[64];
} g_user_cache[USER_CACHE_SIZE];
static size_t g_user_cache_count, g_user_cache_next;

static void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "pps: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}

static bool numeric(const char *s) {
    if (!s || !*s) return false;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return false;
    return true;
}

static void sanitize_text(char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c == 127) *s = ' ';
    }
}

static bool read_text(const char *path, char *buf, size_t cap, ssize_t *length) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, cap - 1);
    int saved = errno;
    close(fd);
    errno = saved;
    if (n < 0) return false;
    buf[n] = '\0';
    if (length) *length = n;
    return true;
}

static void load_system_info(void) {
    char buf[4096];
    g_hz = sysconf(_SC_CLK_TCK);
    g_pagesize = sysconf(_SC_PAGESIZE);
    if (g_hz <= 0) g_hz = 100;
    if (g_pagesize <= 0) g_pagesize = 4096;
    if (read_text("/proc/uptime", buf, sizeof buf, NULL))
        (void)sscanf(buf, "%lf", &g_uptime);
    FILE *f = fopen("/proc/meminfo", "re");
    if (f) {
        while (fgets(buf, sizeof buf, f)) {
            if (sscanf(buf, "MemTotal: %llu kB", &g_memtotal_kb) == 1) break;
        }
        fclose(f);
    }
    g_boot_time = time(NULL) - (time_t)g_uptime;
}

static void user_name(uid_t uid, char *out, size_t cap) {
    for (size_t i = 0; i < g_user_cache_count; i++) {
        if (g_user_cache[i].uid == uid) {
            snprintf(out, cap, "%s", g_user_cache[i].name);
            return;
        }
    }
    struct passwd pw, *result = NULL;
    char buf[16384];
    if (getpwuid_r(uid, &pw, buf, sizeof buf, &result) == 0 && result)
        snprintf(out, cap, "%s", pw.pw_name);
    else
        snprintf(out, cap, "%u", (unsigned)uid);
    sanitize_text(out);
    size_t at = g_user_cache_next;
    g_user_cache[at].uid = uid;
    snprintf(g_user_cache[at].name, sizeof g_user_cache[at].name, "%s", out);
    g_user_cache_next = (at + 1) % USER_CACHE_SIZE;
    if (g_user_cache_count < USER_CACHE_SIZE) g_user_cache_count++;
}

static bool parse_stat(pid_t pid, Process *p) {
    char path[64], buf[8192];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    if (!read_text(path, buf, sizeof buf, NULL)) return false;
    char *openp = strchr(buf, '('), *closep = strrchr(buf, ')');
    if (!openp || !closep || closep <= openp) return false;
    size_t cn = (size_t)(closep - openp - 1);
    if (cn >= sizeof p->comm) cn = sizeof p->comm - 1;
    memcpy(p->comm, openp + 1, cn);
    p->comm[cn] = '\0';
    sanitize_text(p->comm);

    char *rest = closep + 2, *save = NULL;
    int field = 3;
    for (char *tok = strtok_r(rest, " ", &save); tok; tok = strtok_r(NULL, " ", &save), field++) {
        switch (field) {
        case 3: p->state = tok[0]; break;
        case 4: p->ppid = (pid_t)strtol(tok, NULL, 10); break;
        case 6: p->session = (pid_t)strtol(tok, NULL, 10); break;
        case 7: p->tty_nr = strtol(tok, NULL, 10); break;
        case 8: p->tpgid = (pid_t)strtol(tok, NULL, 10); break;
        case 14: p->utime = strtoull(tok, NULL, 10); break;
        case 15: p->stime = strtoull(tok, NULL, 10); break;
        case 19: p->nice = strtol(tok, NULL, 10); break;
        case 20: p->threads = strtol(tok, NULL, 10); break;
        case 22: p->start_ticks = strtoull(tok, NULL, 10); break;
        case 23: p->vsize = strtoull(tok, NULL, 10); break;
        case 24: p->rss_pages = strtoll(tok, NULL, 10); break;
        default: break;
        }
        if (field >= 24) break;
    }
    return field >= 24;
}

static void read_command(pid_t pid, Process *p) {
    char path[64], buf[4096];
    ssize_t n = 0;
    snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    if (read_text(path, buf, sizeof buf, &n) && n > 0) {
        for (ssize_t i = 0; i < n - 1; i++) if (buf[i] == '\0') buf[i] = ' ';
        while (n > 0 && (buf[n - 1] == '\0' || buf[n - 1] == ' ')) n--;
        buf[n] = '\0';
        sanitize_text(buf);
    } else {
        snprintf(buf, sizeof buf, "[%s]", p->comm);
    }
    p->command = strdup(buf);
    if (!p->command) die("out of memory");
}

static void read_exe(pid_t pid, char *out, size_t cap) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/exe", pid);
    ssize_t r = readlink(path, out, cap - 1);
    if (r >= 0) { out[r] = '\0'; sanitize_text(out); }
    else out[0] = '\0';
}

static void format_start(time_t t, char out[16]) {
    time_t now = time(NULL);
    struct tm tmv, ntm;
    localtime_r(&t, &tmv);
    localtime_r(&now, &ntm);
    if (tmv.tm_year == ntm.tm_year && tmv.tm_yday == ntm.tm_yday)
        strftime(out, 16, "%H:%M", &tmv);
    else if (tmv.tm_year == ntm.tm_year)
        strftime(out, 16, "%b%d", &tmv);
    else
        strftime(out, 16, "%Y", &tmv);
}

static void stat_label(const Process *p, char out[16]) {
    size_t n = 0;
    out[n++] = p->state ? p->state : '?';
    if (p->nice < 0) out[n++] = '<';
    else if (p->nice > 0) out[n++] = 'N';
    if (p->session == p->pid) out[n++] = 's';
    if (p->threads > 1) out[n++] = 'l';
    if (p->tty_nr && p->tpgid == p->pid) out[n++] = '+';
    out[n] = '\0';
}

static void tty_label(long tty_nr, char out[24]) {
    if (!tty_nr) { strcpy(out, "?"); return; }
    dev_t dev = (dev_t)(unsigned long)tty_nr;
    unsigned maj = major(dev), min = minor(dev);
    if (maj == 136) snprintf(out, 24, "pts/%u", min);
    else if (maj == 4 && min < 64) snprintf(out, 24, "tty%u", min);
    else snprintf(out, 24, "%u,%u", maj, min);
}

static void format_row(Process *p) {
    char tty[24], stat[16], start[16], cputime[32];
    char row[TEXT_MAX];
    tty_label(p->tty_nr, tty);
    stat_label(p, stat);
    format_start(p->started, start);
    unsigned long long sec = (p->utime + p->stime) / (unsigned long long)g_hz;
    if (sec >= 3600)
        snprintf(cputime, sizeof cputime, "%llu:%02llu:%02llu", sec / 3600, (sec / 60) % 60, sec % 60);
    else
        snprintf(cputime, sizeof cputime, "%llu:%02llu", sec / 60, sec % 60);
    snprintf(row, sizeof row,
             "%-10.10s %7d %5.1f %5.1f %8llu %7lld %-8.8s %-5.5s %-6.6s %8s %s",
             p->user, p->pid, p->cpu, p->mem, p->vsize / 1024,
             p->rss_pages * (long long)g_pagesize / 1024, tty, stat, start, cputime,
             p->command);
    p->row = strdup(row);
    if (!p->row) die("out of memory");
}

static bool load_process_metadata(pid_t pid, Process *p) {
    memset(p, 0, sizeof *p);
    p->pid = pid;
    char path[64];
    snprintf(path, sizeof path, "/proc/%d", pid);
    struct stat st;
    if (stat(path, &st) != 0) return false;
    p->uid = st.st_uid;
    if (!parse_stat(pid, p)) return false;
    p->metadata_loaded = true;
    return true;
}

static bool load_process_basic(pid_t pid, Process *p) {
    if (!load_process_metadata(pid, p)) return false;
    read_command(pid, p);
    return true;
}

static void calculate_process_metrics(Process *p) {
    double age = g_uptime - (double)p->start_ticks / g_hz;
    double cpu_seconds = (double)(p->utime + p->stime) / g_hz;
    p->cpu = age > 0.01 ? 100.0 * cpu_seconds / age : 0.0;
    unsigned long long rss_kb = (unsigned long long)(p->rss_pages > 0 ? p->rss_pages : 0) * g_pagesize / 1024;
    p->mem = g_memtotal_kb ? 100.0 * rss_kb / g_memtotal_kb : 0.0;
    p->started = g_boot_time + (time_t)(p->start_ticks / (unsigned long long)g_hz);
}

static void complete_process(Process *p, bool make_row) {
    calculate_process_metrics(p);
    user_name(p->uid, p->user, sizeof p->user);
    if (make_row) format_row(p);
}

static void process_dispose(Process *p) {
    free(p->command);
    free(p->row);
    p->command = NULL;
    p->row = NULL;
}

static void list_free(ProcessList *l) {
    for (size_t i = 0; i < l->n; i++) process_dispose(&l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

static void list_add(ProcessList *l, const Process *p) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 128;
        l->v = xrealloc(l->v, l->cap * sizeof *l->v);
    }
    l->v[l->n++] = *p;
}

static void list_remove(ProcessList *l, size_t at) {
    process_dispose(&l->v[at]);
    l->n--;
    if (at < l->n)
        memmove(&l->v[at], &l->v[at + 1], (l->n - at) * sizeof *l->v);
}

static bool materialize_process(ProcessList *l, size_t at) {
    Process *p = &l->v[at];
    if (p->row) return true;
    Process loaded;
    if (!load_process_basic(p->pid, &loaded)) return false;
    /* A PID reused after enumeration must not become a row in this snapshot. */
    if (loaded.start_ticks > l->snapshot_ticks ||
        (p->metadata_loaded && loaded.start_ticks != p->start_ticks)) {
        process_dispose(&loaded);
        return false;
    }
    complete_process(&loaded, true);
    *p = loaded;
    return true;
}

static bool materialize_metadata(ProcessList *l, size_t at) {
    Process *p = &l->v[at];
    if (p->metadata_loaded) return true;
    Process loaded;
    if (!load_process_metadata(p->pid, &loaded)) return false;
    if (loaded.start_ticks > l->snapshot_ticks) return false;
    calculate_process_metrics(&loaded);
    *p = loaded;
    return true;
}

static void materialize_all(ProcessList *l, bool rows) {
    size_t kept = 0;
    for (size_t i = 0; i < l->n; i++) {
        bool loaded = rows ? materialize_process(l, i) : materialize_metadata(l, i);
        if (!loaded) {
            process_dispose(&l->v[i]);
            continue;
        }
        if (kept != i) l->v[kept] = l->v[i];
        kept++;
    }
    l->n = kept;
}

static bool same_process(const Process *p) {
    Process current = {0};
    return p->row && parse_stat(p->pid, &current) &&
           current.start_ticks == p->start_ticks;
}

static int signal_selected_process(const Process *p, int sig) {
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    int fd = (int)syscall(SYS_pidfd_open, p->pid, 0);
    if (fd >= 0) {
        if (!same_process(p)) {
            close(fd);
            errno = ESTALE;
            return -1;
        }
        int rc = (int)syscall(SYS_pidfd_send_signal, fd, sig, NULL, 0);
        int saved = errno;
        close(fd);
        if (rc == 0) return 0;
        if (saved != ENOSYS) { errno = saved; return -1; }
    } else if (errno != ENOSYS && errno != EINVAL) return -1;
#endif
    /* Older kernels lack pidfds; keep the identity check immediately before kill. */
    if (!same_process(p)) { errno = ESTALE; return -1; }
    return kill(p->pid, sig);
}

static bool matches(const Process *p, const char *needle) {
    return !needle || !*needle || strstr(p->comm, needle) || strstr(p->command, needle);
}

static bool scan_processes(ProcessList *out, const char *filter, ScanMode mode) {
    DIR *d = opendir("/proc");
    if (!d) return false;
    if (mode == SCAN_DETAILS) load_system_info();
    pid_t self = getpid();
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!numeric(de->d_name)) continue;
        char *end;
        long n = strtol(de->d_name, &end, 10);
        if (*end || n <= 0 || n > INT_MAX) continue;
        if ((pid_t)n == self) continue;
        Process p = { .pid = (pid_t)n };
        if (mode == SCAN_RELATIONS) {
            if (parse_stat(p.pid, &p)) list_add(out, &p);
            continue;
        }
        if (!load_process_basic(p.pid, &p)) continue;
        bool hit = matches(&p, filter);
        if (!hit) {
            char exe[PATH_MAX];
            read_exe((pid_t)n, exe, sizeof exe);
            hit = strstr(exe, filter) != NULL;
        }
        if (!hit) { process_dispose(&p); continue; }
        complete_process(&p, true);
        list_add(out, &p);
    }
    closedir(d);
    return true;
}

static bool scan_pids(ProcessList *out) {
    DIR *d = opendir("/proc");
    if (!d) return false;
    load_system_info();
    pid_t self = getpid();
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!numeric(de->d_name)) continue;
        char *end;
        long n = strtol(de->d_name, &end, 10);
        if (*end || n <= 0 || n > INT_MAX || (pid_t)n == self) continue;
        Process p = {0};
        p.pid = (pid_t)n;
        list_add(out, &p);
    }
    closedir(d);
    struct timespec now;
    if (clock_gettime(CLOCK_BOOTTIME, &now) == 0)
        out->snapshot_ticks = (unsigned long long)now.tv_sec * (unsigned long long)g_hz +
                              (unsigned long long)now.tv_nsec * (unsigned long long)g_hz / 1000000000ULL;
    else
        out->snapshot_ticks = ULLONG_MAX;
    return true;
}

static SortKey g_sort_key;
static bool g_sort_desc;
static int process_cmp(const void *av, const void *bv) {
    const Process *a = av, *b = bv;
    int r = 0;
    switch (g_sort_key) {
    case SORT_CPU: r = (a->cpu > b->cpu) - (a->cpu < b->cpu); break;
    case SORT_MEM: r = (a->mem > b->mem) - (a->mem < b->mem); break;
    case SORT_START: r = (a->start_ticks > b->start_ticks) - (a->start_ticks < b->start_ticks); break;
    case SORT_TIME: {
        unsigned long long ac = a->utime + a->stime, bc = b->utime + b->stime;
        r = (ac > bc) - (ac < bc); break;
    }
    case SORT_PID: r = (a->pid > b->pid) - (a->pid < b->pid); break;
    }
    if (!r) r = (a->pid > b->pid) - (a->pid < b->pid);
    return g_sort_desc ? -r : r;
}

static void sort_list(ProcessList *l, SortKey key, bool desc) {
    g_sort_key = key; g_sort_desc = desc;
    qsort(l->v, l->n, sizeof *l->v, process_cmp);
}

static ssize_t find_pid(const ProcessList *l, pid_t pid) {
    for (size_t i = 0; i < l->n; i++) if (l->v[i].pid == pid) return (ssize_t)i;
    return -1;
}

static void fd_write_all(int fd, const char *s, size_t n) {
    while (n) {
        ssize_t written = write(fd, s, n);
        if (written < 0) {
            if (errno == EINTR) continue;
            return;
        }
        s += written;
        n -= (size_t)written;
    }
}

static void ui_flush(UI *ui) {
    if (ui->output_len) {
        fd_write_all(ui->tty, ui->output, ui->output_len);
        ui->output_len = 0;
    }
}

static void ui_write(UI *ui, const char *s, size_t n) {
    if (!ui->buffering) {
        fd_write_all(ui->tty, s, n);
        return;
    }
    while (n) {
        if (ui->output_len == sizeof ui->output) ui_flush(ui);
        size_t space = sizeof ui->output - ui->output_len;
        size_t chunk = n < space ? n : space;
        memcpy(ui->output + ui->output_len, s, chunk);
        ui->output_len += chunk;
        s += chunk;
        n -= chunk;
    }
}

static void tty_write(UI *ui, const char *s) {
    ui_write(ui, s, strlen(s));
}

static void ui_restore(void) {
    if (!g_ui) return;
    g_ui->buffering = false;
    g_ui->output_len = 0;
    if (g_ui->raw) {
        tcsetattr(g_ui->tty, TCSAFLUSH, &g_ui->saved);
        g_ui->raw = false;
    }
    if (g_ui->alt) {
        tty_write(g_ui, "\033[?25h\033[?1049l");
        g_ui->alt = false;
    }
}

static void signal_handler(int sig) {
    if (sig == SIGWINCH) { g_resized = 1; return; }
    ui_restore();
    _exit(128 + sig);
}

static bool ui_begin(UI *ui) {
    ui->tty = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (ui->tty < 0) return false;
    if (tcgetattr(ui->tty, &ui->saved) != 0) return false;
    struct termios raw = ui->saved;
    raw.c_lflag &= ~(ICANON | ECHO | IEXTEN | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_cflag |= CS8;
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(ui->tty, TCSAFLUSH, &raw) != 0) return false;
    ui->raw = true; g_ui = ui;
    atexit(ui_restore);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    tty_write(ui, "\033[?1049h\033[?25l\033[2J\033[H");
    ui->alt = true;
    return true;
}

static void ui_size(UI *ui) {
    struct winsize ws;
    if (ioctl(ui->tty, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
        ui->rows = ws.ws_row;
        ui->cols = ws.ws_col;
    } else { ui->rows = 24; ui->cols = 80; }
}

static void slice_line(UI *ui, const char *line, bool reverse) {
    size_t len = strlen(line), off = (size_t)ui->hscroll;
    if (off > len) off = len;
    if (reverse) tty_write(ui, "\033[7m");
    size_t n = len - off;
    if (n > (size_t)ui->cols) n = (size_t)ui->cols;
    ui_write(ui, line + off, n);
    if (reverse) {
        static const char spaces[128] = "                                                                                                                               ";
        size_t padding = (size_t)ui->cols - n;
        while (padding) {
            size_t chunk = padding < sizeof spaces - 1 ? padding : sizeof spaces - 1;
            ui_write(ui, spaces, chunk);
            padding -= chunk;
        }
        tty_write(ui, "\033[0m");
    }
}

static void keep_visible(UI *ui) {
    size_t page = ui->rows > 2 ? (size_t)ui->rows - 2 : 1;
    if (ui->selected < ui->top) ui->top = ui->selected;
    if (ui->selected >= ui->top + page) ui->top = ui->selected - page + 1;
    if (ui->top > ui->list.n) ui->top = ui->list.n;
}

static void ui_remove_process(UI *ui, size_t at) {
    list_remove(&ui->list, at);
    if (ui->selected > at) ui->selected--;
    if (ui->list.n == 0) ui->selected = 0;
    else if (ui->selected >= ui->list.n) ui->selected = ui->list.n - 1;
    if (ui->top > at) ui->top--;
    if (ui->top > ui->list.n) ui->top = ui->list.n;
}

static void ui_materialize_visible(UI *ui) {
    size_t page = ui->rows > 2 ? (size_t)ui->rows - 2 : 1;
    for (;;) {
        keep_visible(ui);
        bool removed = false;
        for (size_t i = ui->top; i < ui->list.n && i - ui->top < page; ) {
            if (materialize_process(&ui->list, i)) i++;
            else { ui_remove_process(ui, i); removed = true; }
        }
        if (!removed) break;
    }
}

static void ui_materialize_all(UI *ui) {
    pid_t selected = ui->list.n ? ui->list.v[ui->selected].pid : 0;
    size_t previous = ui->selected;
    materialize_all(&ui->list, true);
    ssize_t at = find_pid(&ui->list, selected);
    if (at >= 0) ui->selected = (size_t)at;
    else if (ui->list.n) ui->selected = previous < ui->list.n ? previous : ui->list.n - 1;
    else ui->selected = 0;
    keep_visible(ui);
}

static void ui_draw_status(UI *ui) {
    char status[1200];
    if (*ui->message) snprintf(status, sizeof status, "%s", ui->message);
    else if (ui->list.n) snprintf(status, sizeof status, ":  %zu/%zu", ui->selected + 1, ui->list.n);
    else snprintf(status, sizeof status, ":  (no processes)");
    tty_write(ui, "\033[999;1H\033[K");
    size_t n = strlen(status);
    if (n > (size_t)ui->cols) n = (size_t)ui->cols;
    ui_write(ui, status, n);
}

static void ui_draw_process_row(UI *ui, size_t index) {
    size_t page = ui->rows > 2 ? (size_t)ui->rows - 2 : 1;
    if (index < ui->top || index >= ui->top + page) return;
    char pos[64];
    snprintf(pos, sizeof pos, "\033[%zu;1H", index - ui->top + 2);
    tty_write(ui, pos);
    if (index < ui->list.n) slice_line(ui, ui->list.v[index].row, index == ui->selected);
    tty_write(ui, "\033[K");
}

static void ui_draw(UI *ui) {
    ui_size(ui);
    keep_visible(ui);
    if (ui->lazy) ui_materialize_visible(ui);
    ui->buffering = true;
    /* The alternate screen is cleared once in ui_begin().  Clearing it again
       for every key press produces a very visible flash, especially over
       SSH.  Repaint in place and erase each line's unused tail instead. */
    tty_write(ui, "\033[H");
    const char *header = "USER           PID  %CPU  %MEM      VSZ     RSS TTY      STAT  START      TIME COMMAND";
    slice_line(ui, header, false);
    tty_write(ui, "\033[K\r\n");
    int page = ui->rows > 2 ? ui->rows - 2 : 1;
    for (int r = 0; r < page; r++) {
        size_t i = ui->top + (size_t)r;
        if (i < ui->list.n) slice_line(ui, ui->list.v[i].row, i == ui->selected);
        tty_write(ui, "\033[K");
        if (r + 1 < page) tty_write(ui, "\r\n");
    }
    ui_draw_status(ui);
    ui_flush(ui);
    ui->buffering = false;
}

static int read_key(UI *ui) {
    unsigned char c;
    if (read(ui->tty, &c, 1) != 1) return -1;
    if (c != 27) return c;
    struct pollfd pfd = { .fd = ui->tty, .events = POLLIN };
    if (poll(&pfd, 1, 40) <= 0) return 27;
    unsigned char seq[3] = {0};
    if (read(ui->tty, &seq[0], 1) != 1) return 27;
    if (seq[0] != '[' && seq[0] != 'O') return 27;
    if (read(ui->tty, &seq[1], 1) != 1) return 27;
    if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(ui->tty, &seq[2], 1) != 1) return 27;
        if (seq[2] == '~') {
            if (seq[1] == '5') return 1005;
            if (seq[1] == '6') return 1006;
        }
    }
    if (seq[1] == 'A') return 1001;
    if (seq[1] == 'B') return 1002;
    if (seq[1] == 'C') return 1003;
    if (seq[1] == 'D') return 1004;
    return 27;
}

static bool prompt_input(UI *ui, const char *label, char *out, size_t cap) {
    size_t n = 0; out[0] = '\0';
    for (;;) {
        char line[1400];
        snprintf(line, sizeof line, "%s%s", label, out);
        tty_write(ui, "\033[999;1H\033[K");
        fd_write_all(ui->tty, line, strlen(line));
        tty_write(ui, "\033[?25h");
        int k = read_key(ui);
        if (k == 27) { tty_write(ui, "\033[?25l"); return false; }
        if (k == '\r' || k == '\n') { tty_write(ui, "\033[?25l"); return true; }
        if ((k == 127 || k == 8) && n) out[--n] = '\0';
        else if (k >= 32 && k < 127 && n + 1 < cap) { out[n++] = (char)k; out[n] = '\0'; }
    }
}

static bool search_move(UI *ui, bool forward) {
    if (!*ui->search || !ui->list.n) return false;
    if (ui->lazy) ui_materialize_all(ui);
    if (!ui->list.n) return false;
    size_t i = ui->selected;
    for (size_t step = 0; step < ui->list.n; step++) {
        i = forward ? (i + 1) % ui->list.n : (i + ui->list.n - 1) % ui->list.n;
        if (strstr(ui->list.v[i].row, ui->search)) { ui->selected = i; return true; }
    }
    snprintf(ui->message, sizeof ui->message, "Pattern not found: %.1000s", ui->search);
    return false;
}

static void show_help(UI *ui) {
    static const char *help[] = {
        "PROCESS SELECT", "", "  Enter       select process", "  P           store selected PID in $PID", "",
        "MOVEMENT", "", "  j / Down    forward one line", "  k / Up      backward one line",
        "  Space / f   forward one window", "  b           backward one window",
        "  d / u       forward / backward half window", "  g / G       first / last line", "  Left/Right  horizontal scroll", "",
        "SEARCH", "", "  /pattern    search forward", "  n / N       next / previous match", "",
        "PROCESS", "", "  o           sort", "  r           refresh", "  s           send signal", "",
        "EXIT", "", "  q / Esc     quit help or cancel", NULL
    };
    size_t top = 0;
    for (;;) {
        ui_size(ui); tty_write(ui, "\033[H");
        int page = ui->rows > 1 ? ui->rows - 1 : 1;
        for (int r = 0; r < page; r++) {
            size_t i = top + (size_t)r;
            if (help[i]) fd_write_all(ui->tty, help[i], strnlen(help[i], (size_t)ui->cols));
            tty_write(ui, "\033[K");
            if (r + 1 < page) tty_write(ui, "\r\n");
            if (!help[i]) break;
        }
        tty_write(ui, "\033[999;1H\033[KHELP -- q to return");
        int k = read_key(ui);
        if (k == 'q' || k == 27 || k == 'h' || k == '?') break;
        if (k == 'j' || k == 1002 || k == ' ' || k == 'f' || k == 1006) {
            if (help[top + 1]) top++;
        } else if ((k == 'k' || k == 1001 || k == 'b' || k == 1005) && top) top--;
        else if (k == 'g') top = 0;
    }
}

static int signal_number(const char *name) {
    if (!name || !*name) return -1;
    if (numeric(name)) {
        long n = strtol(name, NULL, 10);
        return n > 0 && n < NSIG ? (int)n : -1;
    }
    if (!strncasecmp(name, "SIG", 3)) name += 3;
    struct { const char *n; int s; } names[] = {
        {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"ABRT", SIGABRT},
        {"KILL", SIGKILL}, {"SEGV", SIGSEGV}, {"PIPE", SIGPIPE}, {"ALRM", SIGALRM},
        {"TERM", SIGTERM}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {"CHLD", SIGCHLD},
        {"CONT", SIGCONT}, {"STOP", SIGSTOP}, {NULL, 0}
    };
    for (int i = 0; names[i].n; i++) if (!strcasecmp(name, names[i].n)) return names[i].s;
    return -1;
}

static void refresh_ui(UI *ui) {
    pid_t oldpid = ui->list.n ? ui->list.v[ui->selected].pid : 0;
    size_t oldpos = ui->selected;
    ProcessList fresh = {0};
    if (!(ui->lazy ? scan_pids(&fresh) : scan_processes(&fresh, ui->filter, SCAN_DETAILS))) {
        snprintf(ui->message, sizeof ui->message, "pps: cannot read /proc: %s", strerror(errno));
        return;
    }
    if (ui->lazy && ui->sort != SORT_PID) materialize_all(&fresh, false);
    sort_list(&fresh, ui->sort, ui->descending);
    list_free(&ui->list); ui->list = fresh;
    ssize_t at = find_pid(&ui->list, oldpid);
    if (at >= 0) ui->selected = (size_t)at;
    else if (ui->list.n) ui->selected = oldpos < ui->list.n ? oldpos : ui->list.n - 1;
    else ui->selected = 0;
}

static void sort_menu(UI *ui) {
    snprintf(ui->message, sizeof ui->message, "Sort: [p]ID [c]PU [m]EM [s]TART [t]IME   Esc:cancel");
    ui_draw(ui);
    int k = read_key(ui), valid = 1;
    SortKey key = ui->sort;
    if (k == 'p') key = SORT_PID; else if (k == 'c') key = SORT_CPU;
    else if (k == 'm') key = SORT_MEM; else if (k == 's') key = SORT_START;
    else if (k == 't') key = SORT_TIME; else valid = 0;
    if (valid) {
        pid_t pid = ui->list.n ? ui->list.v[ui->selected].pid : 0;
        if (key == ui->sort) ui->descending = !ui->descending;
        else { ui->sort = key; ui->descending = key == SORT_CPU || key == SORT_MEM || key == SORT_TIME; }
        if (ui->lazy && ui->sort != SORT_PID) materialize_all(&ui->list, false);
        sort_list(&ui->list, ui->sort, ui->descending);
        ssize_t at = find_pid(&ui->list, pid);
        if (at >= 0) ui->selected = (size_t)at;
        else if (ui->list.n && ui->selected >= ui->list.n) ui->selected = ui->list.n - 1;
        else if (!ui->list.n) ui->selected = 0;
    }
    ui->message[0] = '\0';
}

static void send_ui_signal(UI *ui) {
    if (!ui->list.n) return;
    pid_t pid = ui->list.v[ui->selected].pid;
    unsigned long long start_ticks = ui->list.v[ui->selected].start_ticks;
    snprintf(ui->message, sizeof ui->message,
             "Signal: [t]TERM [k]KILL [s]STOP [c]CONT [h]HUP [1]USR1 [2]USR2 [o]Other  Esc:cancel");
    ui_draw(ui);
    int k = read_key(ui), sig = -1;
    if (k == 't') sig = SIGTERM; else if (k == 'k') sig = SIGKILL;
    else if (k == 's') sig = SIGSTOP; else if (k == 'c') sig = SIGCONT;
    else if (k == 'h') sig = SIGHUP; else if (k == '1') sig = SIGUSR1;
    else if (k == '2') sig = SIGUSR2;
    else if (k == 'o') {
        char name[64];
        if (prompt_input(ui, "Signal: ", name, sizeof name)) sig = signal_number(name);
        if (sig < 0) { snprintf(ui->message, sizeof ui->message, "pps: invalid signal '%s'", name); return; }
    }
    if (sig < 0) { ui->message[0] = '\0'; return; }
    ssize_t at = find_pid(&ui->list, pid);
    if (at < 0 || ui->list.v[at].start_ticks != start_ticks) {
        refresh_ui(ui);
        snprintf(ui->message, sizeof ui->message, "pps: process %d no longer exists or changed", pid);
        return;
    }
    Process *selected = &ui->list.v[at];
    if (signal_selected_process(selected, sig) != 0) {
        int failure = errno;
        if (failure == ESRCH || failure == ESTALE) {
            refresh_ui(ui);
            snprintf(ui->message, sizeof ui->message, "pps: process %d no longer exists or changed", pid);
        } else
            snprintf(ui->message, sizeof ui->message, "pps: cannot signal %d: %s", pid, strerror(failure));
        return;
    }
    usleep(30000);
    ui->message[0] = '\0';
    refresh_ui(ui);
}

static int process_select(ProcessList *list, const char *filter, pid_t *selected, bool *setvar,
                          bool lazy) {
    UI ui;
    memset(&ui, 0, sizeof ui);
    ui.tty = -1; ui.list = *list; memset(list, 0, sizeof *list);
    ui.sort = SORT_PID; ui.descending = false;
    ui.lazy = lazy;
    snprintf(ui.filter, sizeof ui.filter, "%s", filter ? filter : "");
    sort_list(&ui.list, ui.sort, ui.descending);
    if (!ui_begin(&ui)) { list_free(&ui.list); fprintf(stderr, "pps: interactive terminal is required\n"); return 1; }
    bool need_draw = true;
    for (;;) {
        if (g_resized) { g_resized = 0; need_draw = true; }
        if (need_draw) ui_draw(&ui);
        need_draw = true;
        ui.message[0] = '\0';
        size_t old_selected = ui.selected, old_top = ui.top;
        bool one_line_move = false;
        int k = read_key(&ui);
        if (k < 0 || k == 'q' || k == 27 || k == 3) { ui_restore(); close(ui.tty); list_free(&ui.list); g_ui = NULL; return 125; }
        size_t page = ui.rows > 2 ? (size_t)ui.rows - 2 : 1;
        if ((k == 'j' || k == 1002) && ui.selected + 1 < ui.list.n) { ui.selected++; one_line_move = true; }
        else if ((k == 'k' || k == 1001) && ui.selected) { ui.selected--; one_line_move = true; }
        else if (k == ' ' || k == 'f' || k == 1006) ui.selected = ui.list.n ? (ui.selected + page < ui.list.n ? ui.selected + page : ui.list.n - 1) : 0;
        else if (k == 'b' || k == 1005) ui.selected = ui.selected > page ? ui.selected - page : 0;
        else if (k == 'd') { size_t n = page / 2 ? page / 2 : 1; ui.selected = ui.list.n ? (ui.selected + n < ui.list.n ? ui.selected + n : ui.list.n - 1) : 0; }
        else if (k == 'u') { size_t n = page / 2 ? page / 2 : 1; ui.selected = ui.selected > n ? ui.selected - n : 0; }
        else if (k == 'g') ui.selected = 0;
        else if (k == 'G' && ui.list.n) ui.selected = ui.list.n - 1;
        else if (k == 1003) ui.hscroll += ui.cols > 10 ? ui.cols / 4 : 1;
        else if (k == 1004) { ui.hscroll -= ui.cols > 10 ? ui.cols / 4 : 1; if (ui.hscroll < 0) ui.hscroll = 0; }
        else if (k == '/') { if (prompt_input(&ui, "/", ui.search, sizeof ui.search)) search_move(&ui, true); }
        else if (k == 'n') search_move(&ui, true);
        else if (k == 'N') search_move(&ui, false);
        else if (k == 'h' || k == '?') show_help(&ui);
        else if (k == 'o') sort_menu(&ui);
        else if (k == 'r') refresh_ui(&ui);
        else if (k == 's') send_ui_signal(&ui);
        else if ((k == '\r' || k == '\n' || k == 'P') && ui.list.n) {
            if (!same_process(&ui.list.v[ui.selected])) {
                pid_t stale = ui.list.v[ui.selected].pid;
                refresh_ui(&ui);
                snprintf(ui.message, sizeof ui.message,
                         "pps: process %d no longer exists or changed", stale);
                continue;
            }
            *selected = ui.list.v[ui.selected].pid; *setvar = k == 'P';
            ui_restore(); close(ui.tty); list_free(&ui.list); g_ui = NULL; return 0;
        }
        if (one_line_move && ui.selected != old_selected) {
            keep_visible(&ui);
            if (ui.top == old_top) {
                ui.buffering = true;
                ui_draw_process_row(&ui, old_selected);
                ui_draw_process_row(&ui, ui.selected);
                ui_draw_status(&ui);
                ui_flush(&ui);
                ui.buffering = false;
                need_draw = false;
            }
        }
    }
}

static const char *state_name(char s) {
    switch (s) { case 'R': return "Running"; case 'S': return "Sleeping"; case 'D': return "Disk sleep";
    case 'T': case 't': return "Stopped"; case 'Z': return "Zombie"; case 'I': return "Idle"; default: return "Unknown"; }
}

static void human_bytes(unsigned long long bytes, char out[64]) {
    static const char *u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = (double)bytes; int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    if (i == 0) snprintf(out, 64, "%.0f %s", v, u[i]); else snprintf(out, 64, "%.1f %s", v, u[i]);
}

static int count_dir_entries(const char *path) {
    DIR *d = opendir(path); if (!d) return -1;
    int n = 0; struct dirent *de;
    while ((de = readdir(d))) if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) n++;
    closedir(d); return n;
}

static unsigned long long open_file_limit(pid_t pid) {
    char path[64], line[1024]; unsigned long long limit = 0;
    snprintf(path, sizeof path, "/proc/%d/limits", pid);
    FILE *f = fopen(path, "re"); if (!f) return 0;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "Max open files", 14)) {
        char *p = line + 14; while (isspace((unsigned char)*p)) p++;
        if (strncmp(p, "unlimited", 9)) limit = strtoull(p, NULL, 10);
        break;
    }
    fclose(f); return limit;
}

static int show_info(pid_t pid) {
    load_system_info();
    Process p;
    if (!load_process_basic(pid, &p)) { fprintf(stderr, "pps: process %d no longer exists\n", pid); return 1; }
    char exe[PATH_MAX];
    read_exe(pid, exe, sizeof exe);
    complete_process(&p, false);
    char path[64], cwd[PATH_MAX] = "<permission denied>", start[64], up[64], rss[64], vsz[64];
    snprintf(path, sizeof path, "/proc/%d/cwd", pid);
    ssize_t n = readlink(path, cwd, sizeof cwd - 1);
    if (n >= 0) { cwd[n] = '\0'; sanitize_text(cwd); }
    struct tm tmv; localtime_r(&p.started, &tmv); strftime(start, sizeof start, "%Y-%m-%d %H:%M:%S", &tmv);
    unsigned long long age = (unsigned long long)(g_uptime - (double)p.start_ticks / g_hz);
    snprintf(up, sizeof up, "%lluh%02llum%02llus", age / 3600, (age / 60) % 60, age % 60);
    human_bytes((unsigned long long)(p.rss_pages > 0 ? p.rss_pages : 0) * g_pagesize, rss);
    human_bytes(p.vsize, vsz);
    int fds; snprintf(path, sizeof path, "/proc/%d/fd", pid); fds = count_dir_entries(path);
    unsigned long long fdlim = open_file_limit(pid);
    printf("PID        %d\nPPID       %d\nUser       %s\nCommand    %s\nExe        %s\nCwd        %s\nState      %s\nStarted    %s\nUptime     %s\n\n",
           p.pid, p.ppid, p.user, p.command, *exe ? exe : "<permission denied>", cwd, state_name(p.state), start, up);
    printf("CPU        %.1f%%\nMemory     %.1f%%\nRSS        %s\nVSZ        %s\n\n", p.cpu, p.mem, rss, vsz);
    printf("Threads    %ld\nFDs        ", p.threads);
    if (fds < 0) printf("<permission denied>"); else printf("%d", fds);
    if (fdlim) printf(" / %llu", fdlim);
    putchar('\n');
    if (p.ppid > 0) {
        Process parent; printf("\nParent\n");
        if (load_process_basic(p.ppid, &parent)) {
            printf("  %s [%d]\n", parent.comm, parent.pid);
            process_dispose(&parent);
        } else printf("  <unavailable>\n");
    }
    ProcessList all = {0};
    if (scan_processes(&all, NULL, SCAN_RELATIONS)) {
        bool heading = false;
        for (size_t i = 0; i < all.n; i++) if (all.v[i].ppid == pid) {
            if (!heading) { printf("\nChildren\n"); heading = true; }
            printf("  %s [%d]\n", all.v[i].comm, all.v[i].pid);
        }
    }
    list_free(&all);
    process_dispose(&p);
    return 0;
}

static const char *fd_type(const char *target, const struct stat *st, bool have_stat) {
    if (!strncmp(target, "socket:[", 8)) return "socket";
    if (!strncmp(target, "pipe:[", 6)) return "pipe";
    if (!strncmp(target, "anon_inode:", 11)) return "anon";
    if (!have_stat) return "unknown";
    if (S_ISREG(st->st_mode)) return "file";
    if (S_ISDIR(st->st_mode)) return "directory";
    if (S_ISCHR(st->st_mode)) return "char";
    if (S_ISBLK(st->st_mode)) return "block";
    if (S_ISSOCK(st->st_mode)) return "socket";
    if (S_ISFIFO(st->st_mode)) return "pipe";
    return "unknown";
}

static int name_cmp(const struct dirent **aa, const struct dirent **bb) {
    long x = strtol((*aa)->d_name, NULL, 10), y = strtol((*bb)->d_name, NULL, 10);
    return (x > y) - (x < y);
}

static int show_fds(pid_t pid) {
    char dirpath[64]; snprintf(dirpath, sizeof dirpath, "/proc/%d/fd", pid);
    struct dirent **ents = NULL; int n = scandir(dirpath, &ents, NULL, name_cmp);
    if (n < 0) { fprintf(stderr, "pps: cannot read process %d file descriptors: %s\n", pid, strerror(errno)); return 1; }
    printf("FD   TYPE       TARGET\n");
    for (int i = 0; i < n; i++) {
        if (!numeric(ents[i]->d_name)) { free(ents[i]); continue; }
        char path[PATH_MAX], target[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", dirpath, ents[i]->d_name);
        ssize_t r = readlink(path, target, sizeof target - 1);
        if (r < 0) snprintf(target, sizeof target, "<%s>", strerror(errno));
        else { target[r] = '\0'; sanitize_text(target); }
        struct stat st; bool ok = stat(path, &st) == 0;
        printf("%-4s %-10s %s\n", ents[i]->d_name, fd_type(target, &st, ok), target);
        free(ents[i]);
    }
    free(ents); return 0;
}

static int show_threads(pid_t pid) {
    char path[64]; snprintf(path, sizeof path, "/proc/%d/task", pid);
    DIR *d = opendir(path);
    if (!d) { fprintf(stderr, "pps: cannot read process %d threads: %s\n", pid, strerror(errno)); return 1; }
    printf("TID       CPU STATE NAME\n");
    struct dirent *de;
    load_system_info();
    while ((de = readdir(d))) {
        if (!numeric(de->d_name)) continue;
        pid_t tid = (pid_t)strtol(de->d_name, NULL, 10);
        Process t = { .pid = tid };
        char statpath[96]; snprintf(statpath, sizeof statpath, "/proc/%d/task/%d/stat", pid, tid);
        char buf[8192]; if (!read_text(statpath, buf, sizeof buf, NULL)) continue;
        char *op = strchr(buf, '('), *cp = strrchr(buf, ')'); if (!op || !cp) continue;
        size_t cn = (size_t)(cp - op - 1); if (cn >= sizeof t.comm) cn = sizeof t.comm - 1;
        memcpy(t.comm, op + 1, cn); t.comm[cn] = '\0';
        char *rest = cp + 2, *save = NULL; int field = 3;
        for (char *tok = strtok_r(rest, " ", &save); tok; tok = strtok_r(NULL, " ", &save), field++) {
            if (field == 3) t.state = tok[0]; else if (field == 14) t.utime = strtoull(tok, NULL, 10);
            else if (field == 15) t.stime = strtoull(tok, NULL, 10); else if (field == 22) { t.start_ticks = strtoull(tok, NULL, 10); break; }
        }
        double age = g_uptime - (double)t.start_ticks / g_hz;
        double cpu = age > .01 ? 100.0 * ((double)(t.utime + t.stime) / g_hz) / age : 0;
        printf("%-9d %5.1f%% %-5c %s\n", tid, cpu, t.state, t.comm);
    }
    closedir(d); return 0;
}

static void tree_print(const ProcessList *l, pid_t pid, const char *prefix, bool last, bool root, int depth) {
    ssize_t at = find_pid(l, pid); if (at < 0) return;
    if (root) printf("%s[%d]\n", l->v[at].comm, pid);
    else printf("%s%s%s[%d]\n", prefix, last ? "└─ " : "├─ ", l->v[at].comm, pid);
    if (depth > 128) return;
    size_t count = 0;
    for (size_t i = 0; i < l->n; i++) if (l->v[i].ppid == pid) count++;
    size_t seen = 0; char next[4096];
    snprintf(next, sizeof next, "%s%s", prefix, root ? "" : (last ? "   " : "│  "));
    for (size_t i = 0; i < l->n; i++) if (l->v[i].ppid == pid) tree_print(l, l->v[i].pid, next, ++seen == count, false, depth + 1);
}

static int show_tree(pid_t pid) {
    ProcessList all = {0};
    if (!scan_processes(&all, NULL, SCAN_RELATIONS)) { fprintf(stderr, "pps: cannot read /proc: %s\n", strerror(errno)); return 1; }
    if (find_pid(&all, pid) < 0) { list_free(&all); fprintf(stderr, "pps: process %d no longer exists\n", pid); return 1; }
    sort_list(&all, SORT_PID, false); tree_print(&all, pid, "", true, true, 0); list_free(&all); return 0;
}

static int show_limits(pid_t pid) {
    char path[64], line[2048]; snprintf(path, sizeof path, "/proc/%d/limits", pid);
    FILE *f = fopen(path, "re");
    if (!f) { fprintf(stderr, "pps: cannot read process %d limits: %s\n", pid, strerror(errno)); return 1; }
    while (fgets(line, sizeof line, f)) fputs(line, stdout);
    fclose(f); return 0;
}

static int send_signal_cli(pid_t pid, int sig) {
    char path[64]; snprintf(path, sizeof path, "/proc/%d", pid);
    if (access(path, F_OK) != 0) { fprintf(stderr, "pps: process %d no longer exists\n", pid); return 1; }
    if (kill(pid, sig) != 0) { fprintf(stderr, "pps: cannot signal %d: %s\n", pid, strerror(errno)); return 1; }
    return 0;
}

static void usage(FILE *out) {
    fprintf(out, "usage: pps [TARGET] [OPTION]\n"
                 "  TARGET              PID or case-sensitive process search\n"
                 "  --fd                 list file descriptors\n"
                 "  -t, --threads        list threads\n"
                 "  --tree               show descendants\n"
                 "  --limits             show resource limits\n"
                 "  --term|--kill|--stop|--cont\n"
                 "  --signal SIGNAL      send a signal\n");
}

static int perform(pid_t pid, Action action, int sig) {
    switch (action) {
    case ACT_INFO: return show_info(pid); case ACT_FD: return show_fds(pid);
    case ACT_THREADS: return show_threads(pid); case ACT_TREE: return show_tree(pid);
    case ACT_LIMITS: return show_limits(pid); case ACT_SIGNAL: return send_signal_cli(pid, sig);
    }
    return 1;
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    const char *target = NULL, *filter = NULL;
    Action action = ACT_INFO; int sig = 0; bool option_seen = false;
    int i = 1;
    if (i < argc && argv[i][0] != '-') target = argv[i++];
    while (i < argc) {
        const char *a = argv[i++];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(stdout); return 0; }
        if (!strcmp(a, "--version")) { printf("pps %s\n", PPS_VERSION); return 0; }
        if (option_seen) { fprintf(stderr, "pps: only one operation may be specified\n"); return 2; }
        option_seen = true;
        if (!strcmp(a, "--fd")) action = ACT_FD;
        else if (!strcmp(a, "--threads") || !strcmp(a, "-t")) action = ACT_THREADS;
        else if (!strcmp(a, "--tree")) action = ACT_TREE;
        else if (!strcmp(a, "--limits")) action = ACT_LIMITS;
        else if (!strcmp(a, "--term")) { action = ACT_SIGNAL; sig = SIGTERM; }
        else if (!strcmp(a, "--kill")) { action = ACT_SIGNAL; sig = SIGKILL; }
        else if (!strcmp(a, "--stop")) { action = ACT_SIGNAL; sig = SIGSTOP; }
        else if (!strcmp(a, "--cont")) { action = ACT_SIGNAL; sig = SIGCONT; }
        else if (!strcmp(a, "--signal")) {
            if (i >= argc) { fprintf(stderr, "pps: --signal requires an argument\n"); return 2; }
            action = ACT_SIGNAL; sig = signal_number(argv[i++]);
            if (sig < 0) { fprintf(stderr, "pps: invalid signal '%s'\n", argv[i - 1]); return 2; }
        } else { fprintf(stderr, "pps: unknown option '%s'\n", a); return 2; }
    }

    if (target && numeric(target)) {
        long n = strtol(target, NULL, 10);
        if (n <= 0 || n > INT_MAX) { fprintf(stderr, "pps: invalid PID '%s'\n", target); return 2; }
        return perform((pid_t)n, action, sig);
    }
    filter = target;
    ProcessList list = {0};
    bool lazy = !filter;
    if (!(lazy ? scan_pids(&list) : scan_processes(&list, filter, SCAN_DETAILS)))
        die("cannot read /proc: %s", strerror(errno));
    if (target && list.n == 0) { fprintf(stderr, "pps: no process matched '%s'\n", target); list_free(&list); return 1; }
    if (target && list.n == 1) { pid_t pid = list.v[0].pid; list_free(&list); return perform(pid, action, sig); }
    pid_t chosen = 0; bool setvar = false;
    int rc = process_select(&list, filter, &chosen, &setvar, lazy);
    if (rc == 0) printf(setvar ? "PPS_SETVAR\t%d\n" : "PPS_SELECT\t%d\n", chosen);
    return rc;
}
