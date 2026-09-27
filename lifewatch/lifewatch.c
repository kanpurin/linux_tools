#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <curses.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/audit.h>
#include <linux/cn_proc.h>
#include <linux/connector.h>
#include <linux/fanotify.h>
#include <linux/netlink.h>
#include <langinfo.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fanotify.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utmpx.h>
#include <wchar.h>

#define VERSION "0.1.0"
#define MAX_TARGETS 64
#define MAX_LABEL 64
#define MAX_VALUE 4096
#define MAX_SUMMARY 2048
#define SIGNAL_HISTORY 64
#define PROCESS_HISTORY 256
#define CONFIG_DEFAULT "/etc/lifewatch.conf"
#define LOG_DEFAULT "/var/log/lifewatch/notifications.log"

enum {
    COLOR_ACCENT = 1,
    COLOR_OK,
    COLOR_ERROR,
    COLOR_WARN,
};

typedef enum {
    LEVEL_INFO = 0,
    LEVEL_NOTICE = 1,
    LEVEL_WARNING = 2,
    LEVEL_CRITICAL = 3,
} Severity;

typedef enum {
    MATCH_PID,
} MatchType;

typedef enum {
    LANGUAGE_AUTO,
    LANGUAGE_JA,
    LANGUAGE_EN,
} Language;

typedef struct {
    char label[MAX_LABEL];
    MatchType match_type;
    char value[PATH_MAX];
    pid_t target_pid;
    unsigned long long target_start_ticks;
} ProcessConfig;

typedef struct {
    pid_t pid;
    pid_t ppid;
    unsigned long long start_ticks;
    char comm[128];
    char exe[PATH_MAX];
} ProcessChoice;

typedef struct {
    char label[MAX_LABEL];
    char path[PATH_MAX];
} FileConfig;

typedef struct {
    int interval_ms;
    bool notify_ssh;
    bool terminal_bell;
    Severity min_level;
    Language ui_language;
    Language notification_language;
    char log_file[PATH_MAX];
    ProcessConfig processes[MAX_TARGETS];
    size_t process_count;
    FileConfig files[MAX_TARGETS];
    size_t file_count;
} Config;

typedef struct {
    bool initialized;
    bool present;
    pid_t pid;
    unsigned long long start_ticks;
    int pidfd;
} ProcessState;

typedef struct {
    bool initialized;
    bool exists;
    int wd_file;
    int wd_parent;
    struct timespec last_event;
    uint32_t last_mask;
    pid_t actor_pid;
    pid_t actor_ppid;
    char actor_exe[PATH_MAX];
    char actor_cmd[MAX_VALUE];
    struct timespec actor_time;
} FileState;

typedef struct {
    bool used;
    pid_t target_pid;
    int signo;
    pid_t sender_pid;
    pid_t sender_ppid;
    char sender_exe[PATH_MAX];
    char sender_comm[128];
    char sender_cmd[MAX_VALUE];
    char parent_exe[PATH_MAX];
    struct timespec when;
} SignalEvent;

typedef struct {
    int listener_fd;
    int control_fd;
    bool rule_installed;
    SignalEvent history[SIGNAL_HISTORY];
    size_t next_history;
} AuditState;

typedef struct {
    bool used;
    pid_t pid;
    pid_t ppid;
    unsigned long long start_ticks;
    char exe[PATH_MAX];
    char comm[17];
    char cmd[MAX_VALUE];
    struct timespec when;
} ProcessRecord;

typedef struct {
    int fd;
    ProcessRecord history[PROCESS_HISTORY];
    size_t next_history;
} ProcState;

typedef struct {
    Config config;
    ProcessState processes[MAX_TARGETS];
    FileState files[MAX_TARGETS];
    int inotify_fd;
    int fanotify_fd;
    AuditState audit;
    ProcState proc;
    char config_path[PATH_MAX];
} Runtime;

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t reload_requested;
static unsigned long long event_sequence;
static bool tui_japanese;

static void on_signal(int signo)
{
    if (signo == SIGHUP)
        reload_requested = 1;
    else
        stop_requested = 1;
}

static const char *severity_name(Severity level)
{
    switch (level) {
    case LEVEL_INFO: return "INFO";
    case LEVEL_NOTICE: return "NOTICE";
    case LEVEL_WARNING: return "WARNING";
    case LEVEL_CRITICAL: return "CRITICAL";
    }
    return "UNKNOWN";
}

static bool parse_bool(const char *value, bool *result)
{
    if (strcasecmp(value, "yes") == 0 || strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0) {
        *result = true;
        return true;
    }
    if (strcasecmp(value, "no") == 0 || strcasecmp(value, "false") == 0 || strcmp(value, "0") == 0) {
        *result = false;
        return true;
    }
    return false;
}

static bool parse_severity(const char *value, Severity *result)
{
    if (strcasecmp(value, "info") == 0) *result = LEVEL_INFO;
    else if (strcasecmp(value, "notice") == 0) *result = LEVEL_NOTICE;
    else if (strcasecmp(value, "warning") == 0 || strcasecmp(value, "warn") == 0) *result = LEVEL_WARNING;
    else if (strcasecmp(value, "critical") == 0 || strcasecmp(value, "crit") == 0) *result = LEVEL_CRITICAL;
    else return false;
    return true;
}

static bool parse_language(const char *value, Language *result)
{
    if (strcasecmp(value, "auto") == 0) *result = LANGUAGE_AUTO;
    else if (strcasecmp(value, "ja") == 0 || strcasecmp(value, "japanese") == 0) *result = LANGUAGE_JA;
    else if (strcasecmp(value, "en") == 0 || strcasecmp(value, "english") == 0) *result = LANGUAGE_EN;
    else return false;
    return true;
}

static const char *language_name(Language language)
{
    switch (language) {
    case LANGUAGE_AUTO: return "auto";
    case LANGUAGE_JA: return "ja";
    case LANGUAGE_EN: return "en";
    }
    return "auto";
}

static bool locale_is_utf8(void)
{
    const char *codeset = nl_langinfo(CODESET);
    return codeset && (strcasecmp(codeset, "UTF-8") == 0 || strcasecmp(codeset, "UTF8") == 0);
}

static bool locale_is_japanese(void)
{
    const char *locale = setlocale(LC_MESSAGES, NULL);
    return locale_is_utf8() && locale && strncasecmp(locale, "ja", 2) == 0;
}

static bool language_is_japanese(Language language)
{
    if (!locale_is_utf8()) return false;
    if (language == LANGUAGE_JA) return true;
    if (language == LANGUAGE_EN) return false;
    return locale_is_japanese();
}

static const char *tui_text(const char *english, const char *japanese)
{
    return tui_japanese ? japanese : english;
}

static const char *severity_display(Severity level)
{
    if (!tui_japanese) return severity_name(level);
    switch (level) {
    case LEVEL_INFO: return "情報";
    case LEVEL_NOTICE: return "通知";
    case LEVEL_WARNING: return "警告";
    case LEVEL_CRITICAL: return "重大";
    }
    return "不明";
}

static void config_defaults(Config *config)
{
    memset(config, 0, sizeof(*config));
    config->interval_ms = 500;
    config->notify_ssh = true;
    config->terminal_bell = true;
    config->min_level = LEVEL_WARNING;
    config->ui_language = LANGUAGE_AUTO;
    config->notification_language = LANGUAGE_AUTO;
    snprintf(config->log_file, sizeof(config->log_file), "%s", LOG_DEFAULT);
}

static char *trim(char *text)
{
    char *end;
    while (isspace((unsigned char)*text)) text++;
    if (*text == '\0') return text;
    end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) *end-- = '\0';
    return text;
}

static bool safe_config_text(const char *text)
{
    return text[0] != '\0' && strchr(text, '|') == NULL && strchr(text, '\n') == NULL && strchr(text, '\r') == NULL;
}

static bool parse_pid_selector(const char *selector, pid_t *pid, unsigned long long *start_ticks)
{
    char copy[128], *separator, *pid_end = NULL, *ticks_end = NULL;
    long parsed_pid;
    unsigned long long parsed_ticks;
    if (strlen(selector) >= sizeof(copy)) return false;
    snprintf(copy, sizeof(copy), "%s", selector);
    separator = strchr(copy, '@');
    if (!separator || strchr(separator + 1, '@')) return false;
    *separator++ = '\0';
    errno = 0;
    parsed_pid = strtol(copy, &pid_end, 10);
    if (errno || !pid_end || *pid_end || parsed_pid <= 0 || parsed_pid > INT_MAX) return false;
    errno = 0;
    parsed_ticks = strtoull(separator, &ticks_end, 10);
    if (errno || !ticks_end || *ticks_end || parsed_ticks == 0) return false;
    *pid = (pid_t)parsed_pid;
    *start_ticks = parsed_ticks;
    return true;
}

static int parse_process(Config *config, char *value, char *error, size_t error_size)
{
    char *save = NULL;
    char *label = strtok_r(value, "|", &save);
    char *kind = strtok_r(NULL, "|", &save);
    char *selector = strtok_r(NULL, "", &save);
    ProcessConfig *process;

    if (!label || !kind || !selector || config->process_count >= MAX_TARGETS) {
        snprintf(error, error_size, "invalid process entry");
        return -1;
    }
    label = trim(label);
    kind = trim(kind);
    selector = trim(selector);
    if (!safe_config_text(label) || !safe_config_text(selector)) {
        snprintf(error, error_size, "process label and selector must not be empty or contain '|'");
        return -1;
    }
    process = &config->processes[config->process_count];
    memset(process, 0, sizeof(*process));
    snprintf(process->label, sizeof(process->label), "%s", label);
    if (strcmp(kind, "pid") != 0 ||
        !parse_pid_selector(selector, &process->target_pid, &process->target_start_ticks)) {
        snprintf(error, error_size, "process selector must be pid|PID@START_TICKS");
        return -1;
    }
    process->match_type = MATCH_PID;
    snprintf(process->value, sizeof(process->value), "%s", selector);
    config->process_count++;
    return 0;
}

static int parse_file(Config *config, char *value, char *error, size_t error_size)
{
    char *separator = strchr(value, '|');
    FileConfig *file;
    if (!separator || config->file_count >= MAX_TARGETS) {
        snprintf(error, error_size, "invalid file entry");
        return -1;
    }
    *separator++ = '\0';
    value = trim(value);
    separator = trim(separator);
    if (!safe_config_text(value) || !safe_config_text(separator) || separator[0] != '/') {
        snprintf(error, error_size, "file label must be non-empty and path must be absolute");
        return -1;
    }
    file = &config->files[config->file_count++];
    memset(file, 0, sizeof(*file));
    snprintf(file->label, sizeof(file->label), "%s", value);
    snprintf(file->path, sizeof(file->path), "%s", separator);
    return 0;
}

static int validate_config(const Config *config, char *error, size_t error_size)
{
    size_t i, j;
    if (config->interval_ms < 100 || config->interval_ms > 60000) {
        snprintf(error, error_size, "interval_ms must be between 100 and 60000");
        return -1;
    }
    if (config->log_file[0] != '/') {
        snprintf(error, error_size, "log_file must be an absolute path");
        return -1;
    }
    for (i = 0; i < config->process_count; i++) {
        for (j = i + 1; j < config->process_count; j++) {
            if (strcmp(config->processes[i].label, config->processes[j].label) == 0) {
                snprintf(error, error_size, "duplicate monitor label: %s", config->processes[i].label);
                return -1;
            }
        }
        for (j = 0; j < config->file_count; j++) {
            if (strcmp(config->processes[i].label, config->files[j].label) == 0) {
                snprintf(error, error_size, "duplicate monitor label: %s", config->processes[i].label);
                return -1;
            }
        }
    }
    for (i = 0; i < config->file_count; i++) {
        for (j = i + 1; j < config->file_count; j++) {
            if (strcmp(config->files[i].label, config->files[j].label) == 0) {
                snprintf(error, error_size, "duplicate monitor label: %s", config->files[i].label);
                return -1;
            }
        }
    }
    return 0;
}

static int load_config(const char *path, Config *config, char *error, size_t error_size)
{
    FILE *stream;
    char line[8192];
    unsigned line_number = 0;

    config_defaults(config);
    stream = fopen(path, "r");
    if (!stream) {
        snprintf(error, error_size, "%s: %s", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), stream)) {
        char *entry, *equals, *key, *value;
        line_number++;
        entry = trim(line);
        if (*entry == '\0' || *entry == '#') continue;
        equals = strchr(entry, '=');
        if (!equals) {
            snprintf(error, error_size, "%s:%u: expected key=value", path, line_number);
            fclose(stream);
            return -1;
        }
        *equals++ = '\0';
        key = trim(entry);
        value = trim(equals);
        if (strcmp(key, "interval_ms") == 0) {
            char *end = NULL;
            long number = strtol(value, &end, 10);
            if (!end || *end != '\0' || number < INT_MIN || number > INT_MAX) goto invalid_value;
            config->interval_ms = (int)number;
        } else if (strcmp(key, "notify_ssh") == 0) {
            if (!parse_bool(value, &config->notify_ssh)) goto invalid_value;
        } else if (strcmp(key, "terminal_bell") == 0) {
            if (!parse_bool(value, &config->terminal_bell)) goto invalid_value;
        } else if (strcmp(key, "min_level") == 0) {
            if (!parse_severity(value, &config->min_level)) goto invalid_value;
        } else if (strcmp(key, "ui_language") == 0) {
            if (!parse_language(value, &config->ui_language)) goto invalid_value;
        } else if (strcmp(key, "notification_language") == 0) {
            if (!parse_language(value, &config->notification_language)) goto invalid_value;
        } else if (strcmp(key, "log_file") == 0) {
            snprintf(config->log_file, sizeof(config->log_file), "%s", value);
        } else if (strcmp(key, "process") == 0) {
            if (parse_process(config, value, error, error_size) < 0) goto invalid_with_detail;
        } else if (strcmp(key, "file") == 0) {
            if (parse_file(config, value, error, error_size) < 0) goto invalid_with_detail;
        } else {
            snprintf(error, error_size, "%s:%u: unknown key '%s'", path, line_number, key);
            fclose(stream);
            return -1;
        }
        continue;
invalid_value:
        snprintf(error, error_size, "%s:%u: invalid value for %s", path, line_number, key);
        fclose(stream);
        return -1;
invalid_with_detail:
        {
            char detail[512];
            snprintf(detail, sizeof(detail), "%s", error);
            snprintf(error, error_size, "%s:%u: %.300s", path, line_number, detail);
        }
        fclose(stream);
        return -1;
    }
    if (ferror(stream)) {
        snprintf(error, error_size, "%s: read failed: %s", path, strerror(errno));
        fclose(stream);
        return -1;
    }
    fclose(stream);
    return validate_config(config, error, error_size);
}

static int ensure_parent_directory(const char *path, mode_t mode)
{
    char copy[PATH_MAX];
    char *slash, *p;
    struct stat st;
    snprintf(copy, sizeof(copy), "%s", path);
    slash = strrchr(copy, '/');
    if (!slash || slash == copy) return 0;
    *slash = '\0';
    for (p = copy + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(copy, mode) < 0 && errno != EEXIST) return -1;
        if (lstat(copy, &st) < 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
            errno = ENOTDIR;
            return -1;
        }
        *p = '/';
    }
    if (mkdir(copy, mode) < 0 && errno != EEXIST) return -1;
    if (lstat(copy, &st) < 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

static int save_config(const char *path, const Config *config, char *error, size_t error_size)
{
    char temporary[PATH_MAX];
    FILE *stream;
    size_t i;
    struct stat st;

    if (validate_config(config, error, error_size) < 0) return -1;
    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) {
        snprintf(error, error_size, "%s is a symbolic link", path);
        return -1;
    }
    if (ensure_parent_directory(path, 0750) < 0) {
        snprintf(error, error_size, "cannot create parent directory: %s", strerror(errno));
        return -1;
    }
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(temporary)) {
        snprintf(error, error_size, "configuration path is too long");
        return -1;
    }
    stream = fopen(temporary, "wx");
    if (!stream) {
        snprintf(error, error_size, "cannot create temporary configuration: %s", strerror(errno));
        return -1;
    }
    fprintf(stream, "# Generated by lifewatch setup\n");
    fprintf(stream, "interval_ms=%d\n", config->interval_ms);
    fprintf(stream, "notify_ssh=%s\n", config->notify_ssh ? "yes" : "no");
    fprintf(stream, "terminal_bell=%s\n", config->terminal_bell ? "yes" : "no");
    fprintf(stream, "min_level=%s\n", severity_name(config->min_level));
    fprintf(stream, "ui_language=%s\n", language_name(config->ui_language));
    fprintf(stream, "notification_language=%s\n", language_name(config->notification_language));
    fprintf(stream, "log_file=%s\n", config->log_file);
    for (i = 0; i < config->process_count; i++) {
        const ProcessConfig *p = &config->processes[i];
        fprintf(stream, "process=%s|pid|%ld@%llu\n", p->label, (long)p->target_pid,
                p->target_start_ticks);
    }
    for (i = 0; i < config->file_count; i++)
        fprintf(stream, "file=%s|%s\n", config->files[i].label, config->files[i].path);
    if (fflush(stream) != 0 || fsync(fileno(stream)) != 0 || fclose(stream) != 0) {
        int saved_errno = errno;
        unlink(temporary);
        snprintf(error, error_size, "cannot write configuration: %s", strerror(saved_errno));
        return -1;
    }
    if (chmod(temporary, 0640) < 0 || rename(temporary, path) < 0) {
        int saved_errno = errno;
        unlink(temporary);
        snprintf(error, error_size, "cannot install configuration: %s", strerror(saved_errno));
        return -1;
    }
    return 0;
}

static void sanitize_text(const char *source, char *destination, size_t size)
{
    size_t out = 0;
    const unsigned char *p = (const unsigned char *)source;
    if (size == 0) return;
    while (*p && out + 1 < size) {
        unsigned char ch = *p++;
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            if (out + 2 >= size) break;
            destination[out++] = '\\';
            destination[out++] = ch == '\n' ? 'n' : ch == '\r' ? 'r' : 't';
        } else if (ch == '\\' || ch == '"') {
            if (out + 2 >= size) break;
            destination[out++] = '\\';
            destination[out++] = (char)ch;
        } else if (ch < 0x20 || ch == 0x7f || ch == 0x1b) {
            destination[out++] = '?';
        } else {
            destination[out++] = (char)ch;
        }
    }
    destination[out] = '\0';
}

static void timestamp_now(char *buffer, size_t size)
{
    struct timespec now;
    struct tm local;
    char base[64];
    char zone[16];
    clock_gettime(CLOCK_REALTIME, &now);
    localtime_r(&now.tv_sec, &local);
    strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &local);
    strftime(zone, sizeof(zone), "%z", &local);
    {
        char formatted_zone[16];
        if (strlen(zone) == 5)
            snprintf(formatted_zone, sizeof(formatted_zone), "%.3s:%.2s", zone, zone + 3);
        else
            snprintf(formatted_zone, sizeof(formatted_zone), "%s", zone);
        snprintf(buffer, size, "%s.%03ld%s", base, now.tv_nsec / 1000000L, formatted_zone);
    }
}

static const char *notification_event_name(const char *event, bool japanese)
{
    if (!japanese) return event;
    if (strcmp(event, "process_missing") == 0) return "プロセス消失";
    if (strcmp(event, "process_restarted") == 0) return "プロセス再起動";
    if (strcmp(event, "process_exited") == 0) return "プロセス終了";
    if (strcmp(event, "process_recovered") == 0) return "プロセス復旧";
    if (strcmp(event, "file_missing") == 0) return "ファイル消失";
    if (strcmp(event, "file_recovered") == 0) return "ファイル復旧";
    if (strcmp(event, "file_created") == 0) return "ファイル作成";
    if (strcmp(event, "file_edited") == 0) return "ファイル編集";
    if (strcmp(event, "file_attributes_changed") == 0) return "ファイル属性変更";
    if (strcmp(event, "file_deleted") == 0) return "ファイル削除";
    if (strcmp(event, "file_moved") == 0) return "ファイル移動";
    if (strcmp(event, "config_reload_failed") == 0) return "設定再読込失敗";
    if (strcmp(event, "monitor_error") == 0) return "監視エラー";
    return event;
}

static const char *notification_severity_name(Severity level, bool japanese)
{
    if (!japanese) return severity_name(level);
    switch (level) {
    case LEVEL_INFO: return "情報";
    case LEVEL_NOTICE: return "通知";
    case LEVEL_WARNING: return "警告";
    case LEVEL_CRITICAL: return "重大";
    }
    return "不明";
}

static int notify_ssh_sessions(const Config *config, Severity level, unsigned long long id,
                               const char *event, const char *target, const char *summary)
{
    struct utmpx *entry;
    int notified = 0;
    bool japanese = language_is_japanese(config->notification_language);
    char clean[MAX_SUMMARY];
    char message[MAX_SUMMARY + 256];

    if (!config->notify_ssh || level < config->min_level) return 0;
    sanitize_text(summary, clean, sizeof(clean));
    if (japanese)
        snprintf(message, sizeof(message), "%s\r\n[LIFEWATCH %s イベント:%llu] %.100s: %s: %.500s\r\n",
                 config->terminal_bell ? "\a" : "", notification_severity_name(level, true), id,
                 target, notification_event_name(event, true), clean);
    else
        snprintf(message, sizeof(message), "%s\r\n[LIFEWATCH %s event:%llu] %.100s: %.500s\r\n",
                 config->terminal_bell ? "\a" : "", severity_name(level), id, target, clean);
    setutxent();
    while ((entry = getutxent()) != NULL) {
        char tty_path[PATH_MAX];
        int fd;
        struct stat st;
        if (entry->ut_type != USER_PROCESS || entry->ut_host[0] == '\0') continue;
        if (strncmp(entry->ut_line, "pts/", 4) != 0) continue;
        if (snprintf(tty_path, sizeof(tty_path), "/dev/%.*s", (int)sizeof(entry->ut_line), entry->ut_line) >= (int)sizeof(tty_path)) continue;
        fd = open(tty_path, O_WRONLY | O_NONBLOCK | O_NOCTTY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) continue;
        if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode)) {
            ssize_t ignored = write(fd, message, strlen(message));
            if (ignored >= 0) notified++;
        }
        close(fd);
    }
    endutxent();
    return notified;
}

static int append_log(const Config *config, const char *line)
{
    int fd;
    struct stat st;
    size_t length = strlen(line);
    ssize_t written;

    if (ensure_parent_directory(config->log_file, 0750) < 0) return -1;
    fd = open(config->log_file, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0640);
    if (fd < 0) return -1;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    written = write(fd, line, length);
    if (written == (ssize_t)length) written = write(fd, "\n", 1);
    close(fd);
    return written == 1 ? 0 : -1;
}

static void emit_event(const Config *config, Severity level, const char *event, const char *target, const char *format, ...)
{
    char summary[MAX_SUMMARY];
    char clean_summary[MAX_SUMMARY];
    char clean_target[PATH_MAX];
    char timestamp[96];
    char line[8192];
    va_list args;
    unsigned long long id;
    int tty_count;

    va_start(args, format);
    vsnprintf(summary, sizeof(summary), format, args);
    va_end(args);
    sanitize_text(summary, clean_summary, sizeof(clean_summary));
    sanitize_text(target, clean_target, sizeof(clean_target));
    id = ((unsigned long long)time(NULL) * 1000ULL) + (++event_sequence % 1000ULL);
    tty_count = notify_ssh_sessions(config, level, id, event, clean_target, clean_summary);
    timestamp_now(timestamp, sizeof(timestamp));
    snprintf(line, sizeof(line), "%s id=%llu level=%s event=%s target=\"%s\" summary=\"%s\" tty_notified=%d",
             timestamp, id, severity_name(level), event, clean_target, clean_summary, tty_count);
    if (append_log(config, line) < 0)
        fprintf(stderr, "lifewatch: cannot append %s: %s\n", config->log_file, strerror(errno));
}

static int read_text_file(const char *path, char *buffer, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t count;
    if (fd < 0) return -1;
    count = read(fd, buffer, size - 1);
    close(fd);
    if (count < 0) return -1;
    buffer[count] = '\0';
    while (count > 0 && (buffer[count - 1] == '\n' || buffer[count - 1] == '\r')) buffer[--count] = '\0';
    return 0;
}

static int read_process_exe(pid_t pid, char *buffer, size_t size)
{
    char path[64];
    ssize_t count;
    snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid);
    count = readlink(path, buffer, size - 1);
    if (count < 0) return -1;
    buffer[count] = '\0';
    return 0;
}

static int read_process_comm(pid_t pid, char *buffer, size_t size)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/comm", (long)pid);
    return read_text_file(path, buffer, size);
}

static int read_process_cmdline(pid_t pid, char *buffer, size_t size)
{
    char path[64];
    int fd;
    ssize_t count;
    size_t i;
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    count = read(fd, buffer, size - 1);
    close(fd);
    if (count < 0) return -1;
    for (i = 0; i < (size_t)count; i++) if (buffer[i] == '\0') buffer[i] = ' ';
    while (count > 0 && buffer[count - 1] == ' ') count--;
    buffer[count] = '\0';
    return 0;
}

static int read_process_ppid(pid_t pid, pid_t *ppid)
{
    char path[64], line[256];
    FILE *stream;
    snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
    stream = fopen(path, "r");
    if (!stream) return -1;
    while (fgets(line, sizeof(line), stream)) {
        long value;
        if (sscanf(line, "PPid:%ld", &value) == 1) {
            *ppid = (pid_t)value;
            fclose(stream);
            return 0;
        }
    }
    fclose(stream);
    return -1;
}

static int read_process_start_ticks(pid_t pid, unsigned long long *ticks)
{
    char path[64], buffer[8192], *right, *save = NULL, *token;
    int field = 3;
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    if (read_text_file(path, buffer, sizeof(buffer)) < 0) return -1;
    right = strrchr(buffer, ')');
    if (!right || right[1] != ' ') return -1;
    token = strtok_r(right + 2, " ", &save);
    while (token) {
        if (field == 22) {
            *ticks = strtoull(token, NULL, 10);
            return 0;
        }
        field++;
        token = strtok_r(NULL, " ", &save);
    }
    return -1;
}

static int proc_connector_set(int fd, enum proc_cn_mcast_op operation)
{
    struct {
        struct nlmsghdr header;
        struct cn_msg connector;
        enum proc_cn_mcast_op operation;
    } __attribute__((aligned(NLMSG_ALIGNTO))) message;
    memset(&message, 0, sizeof(message));
    message.header.nlmsg_len = NLMSG_LENGTH(sizeof(struct cn_msg) + sizeof(operation));
    message.header.nlmsg_type = NLMSG_DONE;
    message.header.nlmsg_pid = (uint32_t)getpid();
    message.connector.id.idx = CN_IDX_PROC;
    message.connector.id.val = CN_VAL_PROC;
    message.connector.len = sizeof(operation);
    message.operation = operation;
    return send(fd, &message, message.header.nlmsg_len, 0) == (ssize_t)message.header.nlmsg_len ? 0 : -1;
}

static int proc_connector_start(ProcState *state)
{
    struct sockaddr_nl address;
    memset(state, 0, sizeof(*state));
    state->fd = socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_CONNECTOR);
    if (state->fd < 0) return -1;
    memset(&address, 0, sizeof(address));
    address.nl_family = AF_NETLINK;
    address.nl_pid = (uint32_t)getpid();
    address.nl_groups = CN_IDX_PROC;
    if (bind(state->fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        proc_connector_set(state->fd, PROC_CN_MCAST_LISTEN) < 0) {
        int saved_errno = errno;
        close(state->fd);
        state->fd = -1;
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static void proc_connector_stop(ProcState *state)
{
    if (state->fd >= 0) {
        proc_connector_set(state->fd, PROC_CN_MCAST_IGNORE);
        close(state->fd);
    }
    state->fd = -1;
}

static void remember_process_exec(ProcState *state, pid_t pid)
{
    ProcessRecord *record = &state->history[state->next_history++ % PROCESS_HISTORY];
    memset(record, 0, sizeof(*record));
    record->used = true;
    record->pid = pid;
    read_process_ppid(pid, &record->ppid);
    read_process_start_ticks(pid, &record->start_ticks);
    read_process_exe(pid, record->exe, sizeof(record->exe));
    read_process_cmdline(pid, record->cmd, sizeof(record->cmd));
    clock_gettime(CLOCK_MONOTONIC, &record->when);
}

static void remember_process_comm(ProcState *state, pid_t pid, const char *comm)
{
    ProcessRecord *record = NULL;
    size_t i;
    for (i = 0; i < PROCESS_HISTORY; i++) {
        if (state->history[i].used && state->history[i].pid == pid) {
            record = &state->history[i];
            break;
        }
    }
    if (!record) {
        record = &state->history[state->next_history++ % PROCESS_HISTORY];
        memset(record, 0, sizeof(*record));
        record->used = true;
        record->pid = pid;
    }
    snprintf(record->comm, sizeof(record->comm), "%.16s", comm);
    clock_gettime(CLOCK_MONOTONIC, &record->when);
}

static void proc_connector_receive(ProcState *state)
{
    unsigned char buffer[16384];
    ssize_t count;
    while ((count = recv(state->fd, buffer, sizeof(buffer), MSG_DONTWAIT)) > 0) {
        struct nlmsghdr *header;
        int remaining = (int)count;
        for (header = (struct nlmsghdr *)buffer; NLMSG_OK(header, remaining); header = NLMSG_NEXT(header, remaining)) {
            struct cn_msg *connector;
            struct proc_event *event;
            if (header->nlmsg_type != NLMSG_DONE || header->nlmsg_len < NLMSG_LENGTH(sizeof(struct cn_msg))) continue;
            connector = (struct cn_msg *)NLMSG_DATA(header);
            if (connector->id.idx != CN_IDX_PROC || connector->id.val != CN_VAL_PROC ||
                connector->len < sizeof(struct proc_event)) continue;
            {
                struct proc_event aligned_event;
                memcpy(&aligned_event, connector->data, sizeof(aligned_event));
                event = &aligned_event;
                if (event->what == PROC_EVENT_EXEC)
                    remember_process_exec(state, (pid_t)event->event_data.exec.process_pid);
                else if (event->what == PROC_EVENT_COMM)
                    remember_process_comm(state, (pid_t)event->event_data.comm.process_pid,
                                          event->event_data.comm.comm);
            }
        }
    }
}

static const ProcessRecord *find_process_record(const ProcState *state, pid_t pid)
{
    struct timespec now;
    const ProcessRecord *best = NULL;
    size_t i;
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (i = 0; i < PROCESS_HISTORY; i++) {
        const ProcessRecord *record = &state->history[i];
        double age;
        if (!record->used || record->pid != pid) continue;
        age = (double)(now.tv_sec - record->when.tv_sec) + (double)(now.tv_nsec - record->when.tv_nsec) / 1e9;
        if (age < 0.0 || age > 60.0) continue;
        if (!best || record->when.tv_sec > best->when.tv_sec ||
            (record->when.tv_sec == best->when.tv_sec && record->when.tv_nsec > best->when.tv_nsec)) best = record;
    }
    return best;
}

static pid_t find_process(const ProcessConfig *config, unsigned long long *start_ticks)
{
    unsigned long long current_ticks = 0;
    if (config->match_type != MATCH_PID || config->target_pid <= 0) return 0;
    if (read_process_start_ticks(config->target_pid, &current_ticks) < 0 ||
        current_ticks != config->target_start_ticks) return 0;
    *start_ticks = current_ticks;
    return config->target_pid;
}

static int open_pidfd(pid_t pid)
{
#ifdef SYS_pidfd_open
    return (int)syscall(SYS_pidfd_open, pid, 0);
#else
    (void)pid;
    errno = ENOSYS;
    return -1;
#endif
}

static int netlink_send(int fd, uint16_t type, const void *payload, size_t payload_size)
{
    static uint32_t sequence;
    struct {
        struct nlmsghdr header;
        unsigned char payload[sizeof(struct audit_rule_data) + 256];
    } message;
    struct sockaddr_nl address;
    ssize_t sent;
    uint32_t request_sequence;

    if (payload_size > sizeof(message.payload)) {
        errno = E2BIG;
        return -1;
    }
    memset(&message, 0, sizeof(message));
    memset(&address, 0, sizeof(address));
    address.nl_family = AF_NETLINK;
    message.header.nlmsg_len = NLMSG_LENGTH(payload_size);
    message.header.nlmsg_type = type;
    message.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    request_sequence = ++sequence;
    message.header.nlmsg_seq = request_sequence;
    if (payload_size) memcpy(NLMSG_DATA(&message.header), payload, payload_size);
    sent = sendto(fd, &message, message.header.nlmsg_len, 0, (struct sockaddr *)&address, sizeof(address));
    if (sent != (ssize_t)message.header.nlmsg_len) return -1;
    for (;;) {
        struct pollfd descriptor = {fd, POLLIN, 0};
        unsigned char reply[8192];
        ssize_t count;
        struct nlmsghdr *header;
        int remaining;
        if (poll(&descriptor, 1, 1000) <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        count = recv(fd, reply, sizeof(reply), 0);
        if (count < 0) return -1;
        remaining = (int)count;
        for (header = (struct nlmsghdr *)reply; NLMSG_OK(header, remaining); header = NLMSG_NEXT(header, remaining)) {
            struct nlmsgerr *result;
            if (header->nlmsg_seq != request_sequence || header->nlmsg_type != NLMSG_ERROR) continue;
            result = (struct nlmsgerr *)NLMSG_DATA(header);
            if (result->error == 0) return 0;
            errno = -result->error;
            return -1;
        }
    }
}

static int audit_control_open(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_AUDIT);
    struct sockaddr_nl address;
    if (fd < 0) return -1;
    memset(&address, 0, sizeof(address));
    address.nl_family = AF_NETLINK;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int audit_listener_open(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_AUDIT);
    struct sockaddr_nl address;
    if (fd < 0) return -1;
    memset(&address, 0, sizeof(address));
    address.nl_family = AF_NETLINK;
    /* Let the kernel allocate a unique port id; the control socket is also
       bound by this process and normally owns getpid(). */
    address.nl_pid = 0;
#ifdef AUDIT_NLGRP_READLOG
    address.nl_groups = 1U << (AUDIT_NLGRP_READLOG - 1);
#else
    address.nl_groups = 1U;
#endif
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static struct audit_rule_data *make_signal_rule(size_t *rule_size)
{
    static const char key[] = "lifewatch_signal";
    size_t size = sizeof(struct audit_rule_data) + sizeof(key) - 1;
    struct audit_rule_data *rule = calloc(1, size);
    int syscalls[] = {
#ifdef __NR_kill
        __NR_kill,
#endif
#ifdef __NR_tkill
        __NR_tkill,
#endif
#ifdef __NR_tgkill
        __NR_tgkill,
#endif
#ifdef __NR_pidfd_send_signal
        __NR_pidfd_send_signal,
#endif
    };
    size_t i;
    if (!rule) return NULL;
    rule->flags = AUDIT_FILTER_EXIT;
    rule->action = AUDIT_ALWAYS;
    for (i = 0; i < sizeof(syscalls) / sizeof(syscalls[0]); i++) {
        unsigned number = (unsigned)syscalls[i];
        rule->mask[number >> 5] |= 1U << (number & 31U);
    }
    rule->fields[0] = AUDIT_ARCH;
#if defined(__x86_64__)
    rule->values[0] = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    rule->values[0] = AUDIT_ARCH_AARCH64;
#elif defined(__i386__)
    rule->values[0] = AUDIT_ARCH_I386;
#elif defined(__arm__)
    rule->values[0] = AUDIT_ARCH_ARM;
#else
#error "lifewatch does not know the Linux Audit architecture for this target"
#endif
    rule->fieldflags[0] = AUDIT_EQUAL;
    rule->fields[1] = AUDIT_FILTERKEY;
    rule->values[1] = sizeof(key) - 1;
    rule->fieldflags[1] = AUDIT_EQUAL;
    rule->field_count = 2;
    rule->buflen = sizeof(key) - 1;
    memcpy(rule->buf, key, sizeof(key) - 1);
    *rule_size = size;
    return rule;
}

static int audit_set_rule(AuditState *state, bool add)
{
    size_t size;
    struct audit_rule_data *rule = make_signal_rule(&size);
    int result;
    if (!rule) return -1;
    result = netlink_send(state->control_fd, add ? AUDIT_ADD_RULE : AUDIT_DEL_RULE, rule, size);
    free(rule);
    return result;
}

static int audit_start(AuditState *state)
{
    struct audit_status status;
    memset(state, 0, sizeof(*state));
    state->listener_fd = -1;
    state->control_fd = -1;
    state->control_fd = audit_control_open();
    if (state->control_fd < 0) return -1;
    memset(&status, 0, sizeof(status));
    status.mask = AUDIT_STATUS_ENABLED;
    status.enabled = 1;
    if (netlink_send(state->control_fd, AUDIT_SET, &status, sizeof(status)) < 0) goto failure;
    /* Remove an identical stale rule left by an unclean previous exit. */
    audit_set_rule(state, false);
    if (audit_set_rule(state, true) < 0) goto failure;
    state->rule_installed = true;
    state->listener_fd = audit_listener_open();
    if (state->listener_fd < 0) goto failure;
    return 0;
failure:
    if (state->rule_installed) audit_set_rule(state, false);
    if (state->control_fd >= 0) close(state->control_fd);
    state->control_fd = -1;
    return -1;
}

static void audit_stop(AuditState *state)
{
    if (state->rule_installed && state->control_fd >= 0) audit_set_rule(state, false);
    if (state->listener_fd >= 0) close(state->listener_fd);
    if (state->control_fd >= 0) close(state->control_fd);
    state->listener_fd = state->control_fd = -1;
    state->rule_installed = false;
}

static bool extract_audit_token(const char *text, const char *key, char *output, size_t output_size)
{
    size_t key_length = strlen(key);
    const char *position = text;
    while ((position = strstr(position, key)) != NULL) {
        const char *value;
        size_t length = 0;
        if (position != text && position[-1] != ' ' && position[-1] != ':') {
            position += key_length;
            continue;
        }
        value = position + key_length;
        if (*value == '"') {
            value++;
            while (value[length] && value[length] != '"') length++;
        } else {
            while (value[length] && !isspace((unsigned char)value[length])) length++;
        }
        if (length >= output_size) length = output_size - 1;
        memcpy(output, value, length);
        output[length] = '\0';
        return true;
    }
    return false;
}

static long audit_number(const char *text, const char *key, int base, long fallback)
{
    char value[128], *end = NULL;
    long result;
    if (!extract_audit_token(text, key, value, sizeof(value))) return fallback;
    errno = 0;
    result = strtol(value, &end, base);
    return errno == 0 && end && *end == '\0' ? result : fallback;
}

static void record_audit_signal(AuditState *state, const char *text)
{
    long syscall_number, target = -1, signal_number = 0;
    SignalEvent *event;
    char value[PATH_MAX];

    if (!strstr(text, "lifewatch_signal")) return;
    syscall_number = audit_number(text, "syscall=", 10, -1);
#ifdef __NR_kill
    if (syscall_number == __NR_kill) {
        target = audit_number(text, "a0=", 16, -1);
        signal_number = audit_number(text, "a1=", 16, 0);
    } else
#endif
#ifdef __NR_tkill
    if (syscall_number == __NR_tkill) {
        target = audit_number(text, "a0=", 16, -1);
        signal_number = audit_number(text, "a1=", 16, 0);
    } else
#endif
#ifdef __NR_tgkill
    if (syscall_number == __NR_tgkill) {
        target = audit_number(text, "a1=", 16, -1);
        signal_number = audit_number(text, "a2=", 16, 0);
    } else
#endif
    {
        return;
    }
    event = &state->history[state->next_history++ % SIGNAL_HISTORY];
    memset(event, 0, sizeof(*event));
    event->used = true;
    event->target_pid = (pid_t)target;
    event->signo = (int)signal_number;
    event->sender_pid = (pid_t)audit_number(text, "pid=", 10, 0);
    event->sender_ppid = (pid_t)audit_number(text, "ppid=", 10, 0);
    if (extract_audit_token(text, "exe=", value, sizeof(value))) snprintf(event->sender_exe, sizeof(event->sender_exe), "%s", value);
    if (extract_audit_token(text, "comm=", value, sizeof(value))) snprintf(event->sender_comm, sizeof(event->sender_comm), "%.127s", value);
    if (event->sender_pid > 0) read_process_cmdline(event->sender_pid, event->sender_cmd, sizeof(event->sender_cmd));
    if (event->sender_ppid > 0) read_process_exe(event->sender_ppid, event->parent_exe, sizeof(event->parent_exe));
    clock_gettime(CLOCK_MONOTONIC, &event->when);
}

static void audit_receive(AuditState *state)
{
    unsigned char buffer[32768];
    ssize_t count;
    while ((count = recv(state->listener_fd, buffer, sizeof(buffer), MSG_DONTWAIT)) > 0) {
        struct nlmsghdr *header;
        int remaining = (int)count;
        for (header = (struct nlmsghdr *)buffer; NLMSG_OK(header, remaining); header = NLMSG_NEXT(header, remaining)) {
            if (header->nlmsg_type == AUDIT_SYSCALL) {
                size_t length = header->nlmsg_len - NLMSG_HDRLEN;
                char text[16384];
                if (length >= sizeof(text)) length = sizeof(text) - 1;
                memcpy(text, NLMSG_DATA(header), length);
                text[length] = '\0';
                record_audit_signal(state, text);
            }
        }
    }
}

static const SignalEvent *find_recent_signal(const AuditState *state, pid_t target_pid)
{
    struct timespec now;
    const SignalEvent *best = NULL;
    size_t i;
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (i = 0; i < SIGNAL_HISTORY; i++) {
        const SignalEvent *event = &state->history[i];
        double age;
        if (!event->used || event->target_pid != target_pid) continue;
        age = (double)(now.tv_sec - event->when.tv_sec) + (double)(now.tv_nsec - event->when.tv_nsec) / 1e9;
        if (age >= 0.0 && age <= 5.0 && (!best || event->when.tv_sec > best->when.tv_sec ||
            (event->when.tv_sec == best->when.tv_sec && event->when.tv_nsec > best->when.tv_nsec)))
            best = event;
    }
    return best;
}

static void wait_for_audit_signal(AuditState *state, pid_t target_pid)
{
    struct pollfd descriptor;
    if (state->listener_fd < 0 || find_recent_signal(state, target_pid)) return;
    descriptor = (struct pollfd){state->listener_fd, POLLIN, 0};
    if (poll(&descriptor, 1, 100) > 0 && (descriptor.revents & POLLIN))
        audit_receive(state);
}

static void describe_exit(const AuditState *audit, pid_t pid, char *summary, size_t size)
{
    const SignalEvent *event = find_recent_signal(audit, pid);
    if (!event) {
        snprintf(summary, size, "process disappeared pid=%ld cause=unknown", (long)pid);
        return;
    }
    snprintf(summary, size,
             "process disappeared pid=%ld signal=%d sender_pid=%ld sender_exe=%.300s sender_cmd=%.120s parent_exe=%.300s confidence=confirmed",
             (long)pid, event->signo, (long)event->sender_pid,
             event->sender_exe[0] ? event->sender_exe : "unknown",
             event->sender_cmd[0] ? event->sender_cmd : event->sender_comm,
             event->parent_exe[0] ? event->parent_exe : "unknown");
}

static void split_path(const char *path, char *directory, size_t directory_size, char *base, size_t base_size)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(directory, directory_size, ".");
        snprintf(base, base_size, "%s", path);
    } else if (slash == path) {
        snprintf(directory, directory_size, "/");
        snprintf(base, base_size, "%s", slash + 1);
    } else {
        size_t length = (size_t)(slash - path);
        if (length >= directory_size) length = directory_size - 1;
        memcpy(directory, path, length);
        directory[length] = '\0';
        snprintf(base, base_size, "%s", slash + 1);
    }
}

static void reset_file_state(FileState *state)
{
    memset(state, 0, sizeof(*state));
    state->wd_file = -1;
    state->wd_parent = -1;
}

static int setup_file_watch(Runtime *runtime, size_t index)
{
    FileState *state = &runtime->files[index];
    const FileConfig *file = &runtime->config.files[index];
    char directory[PATH_MAX], base[NAME_MAX + 1];
    uint64_t fan_mask = FAN_MODIFY | FAN_CLOSE_WRITE | FAN_ATTRIB | FAN_DELETE_SELF | FAN_MOVE_SELF;
    split_path(file->path, directory, sizeof(directory), base, sizeof(base));
    if (state->wd_parent < 0)
        state->wd_parent = inotify_add_watch(runtime->inotify_fd, directory,
            IN_CLOSE_WRITE | IN_ATTRIB | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
    if (access(file->path, F_OK) == 0 && state->wd_file < 0)
        state->wd_file = inotify_add_watch(runtime->inotify_fd, file->path,
            IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF);
    if (runtime->fanotify_fd >= 0 && access(file->path, F_OK) == 0)
        fanotify_mark(runtime->fanotify_fd, FAN_MARK_ADD, fan_mask, AT_FDCWD, file->path);
    return state->wd_parent >= 0 ? 0 : -1;
}

static void remember_file_actor(Runtime *runtime, pid_t pid, const char *path)
{
    size_t i;
    char normalized[PATH_MAX];
    snprintf(normalized, sizeof(normalized), "%s", path ? path : "");
    {
        char *deleted = strstr(normalized, " (deleted)");
        if (deleted) *deleted = '\0';
    }
    for (i = 0; i < runtime->config.file_count; i++) {
        FileState *state = &runtime->files[i];
        if (normalized[0] && strcmp(normalized, runtime->config.files[i].path) != 0) continue;
        state->actor_pid = pid;
        read_process_ppid(pid, &state->actor_ppid);
        read_process_exe(pid, state->actor_exe, sizeof(state->actor_exe));
        read_process_cmdline(pid, state->actor_cmd, sizeof(state->actor_cmd));
        if (!state->actor_exe[0] || !state->actor_cmd[0]) {
            const ProcessRecord *record = find_process_record(&runtime->proc, pid);
            if (record) {
                if (!state->actor_ppid) state->actor_ppid = record->ppid;
                if (!state->actor_exe[0]) snprintf(state->actor_exe, sizeof(state->actor_exe), "%s",
                                                    record->exe[0] ? record->exe : record->comm);
                if (!state->actor_cmd[0]) snprintf(state->actor_cmd, sizeof(state->actor_cmd), "%s", record->cmd);
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &state->actor_time);
    }
}

static void handle_fanotify(Runtime *runtime)
{
    char buffer[65536];
    ssize_t count;
    while ((count = read(runtime->fanotify_fd, buffer, sizeof(buffer))) > 0) {
        struct fanotify_event_metadata *metadata;
        for (metadata = (struct fanotify_event_metadata *)buffer; FAN_EVENT_OK(metadata, count); metadata = FAN_EVENT_NEXT(metadata, count)) {
            char link[64], path[PATH_MAX] = "";
            if (metadata->vers != FANOTIFY_METADATA_VERSION) continue;
            if (metadata->fd >= 0) {
                ssize_t length;
                snprintf(link, sizeof(link), "/proc/self/fd/%d", metadata->fd);
                length = readlink(link, path, sizeof(path) - 1);
                if (length >= 0) path[length] = '\0';
            }
            if (metadata->pid > 0) remember_file_actor(runtime, metadata->pid, path);
            if (metadata->fd >= 0) close(metadata->fd);
        }
    }
}

static bool recent_file_event(FileState *state, uint32_t mask)
{
    struct timespec now;
    double age;
    clock_gettime(CLOCK_MONOTONIC, &now);
    age = (double)(now.tv_sec - state->last_event.tv_sec) + (double)(now.tv_nsec - state->last_event.tv_nsec) / 1e9;
    if (state->last_mask == mask && age >= 0.0 && age < 0.15) return true;
    state->last_event = now;
    state->last_mask = mask;
    return false;
}

static void emit_file_change(Runtime *runtime, size_t index, uint32_t mask)
{
    FileState *state = &runtime->files[index];
    const FileConfig *file = &runtime->config.files[index];
    const char *event;
    Severity level;
    char actor[PATH_MAX + MAX_VALUE + 128];
    struct timespec now;
    double actor_age;

    if (mask & (IN_DELETE | IN_DELETE_SELF)) {
        if (!state->exists) return;
        event = "file_deleted";
        level = LEVEL_CRITICAL;
        state->exists = false;
    } else if (mask & (IN_MOVED_FROM | IN_MOVE_SELF)) {
        if (!state->exists) return;
        event = "file_moved";
        level = LEVEL_WARNING;
        state->exists = false;
    } else if (mask & (IN_CREATE | IN_MOVED_TO)) {
        event = "file_created";
        level = LEVEL_NOTICE;
        state->exists = true;
    } else if (mask & IN_ATTRIB) {
        event = "file_attributes_changed";
        level = LEVEL_WARNING;
    } else {
        event = "file_edited";
        level = LEVEL_WARNING;
    }
    if (recent_file_event(state, mask)) return;
    clock_gettime(CLOCK_MONOTONIC, &now);
    actor_age = (double)(now.tv_sec - state->actor_time.tv_sec) + (double)(now.tv_nsec - state->actor_time.tv_nsec) / 1e9;
    if (state->actor_pid > 0 && actor_age >= 0.0 && actor_age <= 2.0) {
        snprintf(actor, sizeof(actor), "actor_pid=%ld actor_exe=%s actor_cmd=%s confidence=correlated",
                 (long)state->actor_pid,
                 state->actor_exe[0] ? state->actor_exe : "unknown",
                 state->actor_cmd[0] ? state->actor_cmd : "unknown");
    } else {
        snprintf(actor, sizeof(actor), "actor_pid=unknown confidence=unknown");
    }
    emit_event(&runtime->config, level, event, file->label, "%s path=%s %s", event, file->path, actor);
}

static void handle_inotify(Runtime *runtime)
{
    char buffer[65536];
    ssize_t count;
    if (runtime->proc.fd >= 0) proc_connector_receive(&runtime->proc);
    if (runtime->fanotify_fd >= 0) {
        struct pollfd descriptor = {runtime->fanotify_fd, POLLIN, 0};
        if (poll(&descriptor, 1, 50) > 0 && (descriptor.revents & POLLIN)) {
            if (runtime->proc.fd >= 0) proc_connector_receive(&runtime->proc);
            handle_fanotify(runtime);
        }
    }
    while ((count = read(runtime->inotify_fd, buffer, sizeof(buffer))) > 0) {
        char *position = buffer;
        while (position < buffer + count) {
            struct inotify_event *event = (struct inotify_event *)position;
            size_t i;
            for (i = 0; i < runtime->config.file_count; i++) {
                FileState *state = &runtime->files[i];
                bool matches = event->wd == state->wd_file;
                if (event->wd == state->wd_parent && event->len > 0) {
                    char directory[PATH_MAX], base[NAME_MAX + 1];
                    split_path(runtime->config.files[i].path, directory, sizeof(directory), base, sizeof(base));
                    if (strcmp(event->name, base) == 0) matches = true;
                }
                if (!matches) continue;
                if (event->mask & IN_IGNORED) state->wd_file = -1;
                if (event->mask & (IN_CLOSE_WRITE | IN_ATTRIB | IN_CREATE | IN_DELETE | IN_DELETE_SELF |
                                   IN_MOVED_FROM | IN_MOVED_TO | IN_MOVE_SELF))
                    emit_file_change(runtime, i, event->mask);
                setup_file_watch(runtime, i);
            }
            position += sizeof(*event) + event->len;
        }
    }
}

static void close_process_state(ProcessState *state)
{
    if (state->pidfd >= 0) close(state->pidfd);
    state->pidfd = -1;
}

static void scan_processes(Runtime *runtime)
{
    size_t i;
    for (i = 0; i < runtime->config.process_count; i++) {
        const ProcessConfig *config = &runtime->config.processes[i];
        ProcessState *state = &runtime->processes[i];
        unsigned long long start_ticks = 0;
        pid_t pid = find_process(config, &start_ticks);
        bool present = pid > 0;

        if (!state->initialized) {
            state->initialized = true;
            state->present = present;
            state->pid = pid;
            state->start_ticks = start_ticks;
            state->pidfd = present ? open_pidfd(pid) : -1;
            if (!present)
                emit_event(&runtime->config, LEVEL_CRITICAL, "process_missing", config->label,
                           "process missing selector=%s", config->value);
            continue;
        }
        if (state->present && (!present || pid != state->pid || start_ticks != state->start_ticks)) {
            pid_t old_pid = state->pid;
            char details[MAX_SUMMARY];
            close_process_state(state);
            wait_for_audit_signal(&runtime->audit, old_pid);
            describe_exit(&runtime->audit, old_pid, details, sizeof(details));
            if (present) {
                emit_event(&runtime->config, LEVEL_WARNING, "process_restarted", config->label,
                           "%s new_pid=%ld", details, (long)pid);
            } else {
                emit_event(&runtime->config, LEVEL_CRITICAL, "process_exited", config->label, "%s", details);
            }
        } else if (!state->present && present) {
            emit_event(&runtime->config, LEVEL_NOTICE, "process_recovered", config->label,
                       "process recovered new_pid=%ld", (long)pid);
        }
        if (present && (!state->present || state->pid != pid || state->start_ticks != start_ticks)) {
            close_process_state(state);
            state->pidfd = open_pidfd(pid);
        }
        state->present = present;
        state->pid = pid;
        state->start_ticks = start_ticks;
    }
}

static void scan_files(Runtime *runtime)
{
    size_t i;
    for (i = 0; i < runtime->config.file_count; i++) {
        FileState *state = &runtime->files[i];
        const FileConfig *file = &runtime->config.files[i];
        struct stat st;
        bool exists = lstat(file->path, &st) == 0;
        if (!state->initialized) {
            state->initialized = true;
            state->exists = exists;
            if (!exists)
                emit_event(&runtime->config, LEVEL_CRITICAL, "file_missing", file->label,
                           "file missing path=%s", file->path);
        } else if (state->exists && !exists) {
            emit_event(&runtime->config, LEVEL_CRITICAL, "file_missing", file->label,
                       "file disappeared path=%s actor_pid=unknown", file->path);
        } else if (!state->exists && exists) {
            emit_event(&runtime->config, LEVEL_NOTICE, "file_recovered", file->label,
                       "file recovered path=%s", file->path);
        }
        state->exists = exists;
        setup_file_watch(runtime, i);
    }
}

static int runtime_watch_init(Runtime *runtime)
{
    size_t i;
    runtime->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (runtime->inotify_fd < 0) return -1;
    runtime->fanotify_fd = fanotify_init(FAN_CLASS_NOTIF | FAN_CLOEXEC | FAN_NONBLOCK | FAN_REPORT_FID,
                                         O_RDONLY | O_LARGEFILE | O_CLOEXEC);
    if (runtime->fanotify_fd < 0)
        fprintf(stderr, "lifewatch: fanotify unavailable; file actor attribution disabled: %s\n", strerror(errno));
    runtime->proc.fd = -1;
    if (proc_connector_start(&runtime->proc) < 0)
        fprintf(stderr, "lifewatch: proc connector unavailable; short-lived process details may be incomplete: %s\n", strerror(errno));
    for (i = 0; i < runtime->config.file_count; i++) {
        reset_file_state(&runtime->files[i]);
        setup_file_watch(runtime, i);
    }
    return 0;
}

static void runtime_close(Runtime *runtime)
{
    size_t i;
    for (i = 0; i < runtime->config.process_count; i++) close_process_state(&runtime->processes[i]);
    if (runtime->inotify_fd >= 0) close(runtime->inotify_fd);
    if (runtime->fanotify_fd >= 0) close(runtime->fanotify_fd);
    proc_connector_stop(&runtime->proc);
    audit_stop(&runtime->audit);
    runtime->inotify_fd = runtime->fanotify_fd = -1;
}

static int reload_runtime(Runtime *runtime)
{
    Config replacement;
    char error[512];
    static Runtime fresh;
    if (load_config(runtime->config_path, &replacement, error, sizeof(error)) < 0) {
        emit_event(&runtime->config, LEVEL_CRITICAL, "config_reload_failed", "lifewatch", "%s", error);
        return -1;
    }
    runtime_close(runtime);
    memset(&fresh, 0, sizeof(fresh));
    fresh.config = replacement;
    fresh.inotify_fd = fresh.fanotify_fd = -1;
    fresh.audit.listener_fd = fresh.audit.control_fd = -1;
    fresh.proc.fd = -1;
    snprintf(fresh.config_path, sizeof(fresh.config_path), "%s", runtime->config_path);
    if (runtime_watch_init(&fresh) < 0) {
        int saved_errno = errno;
        fresh.inotify_fd = fresh.fanotify_fd = -1;
        fresh.audit.listener_fd = fresh.audit.control_fd = -1;
        fresh.proc.fd = -1;
        *runtime = fresh;
        emit_event(&runtime->config, LEVEL_CRITICAL, "config_reload_failed", "lifewatch",
                   "cannot recreate file watches: %s; daemon will stop", strerror(saved_errno));
        stop_requested = 1;
        return -1;
    }
    if (audit_start(&fresh.audit) < 0)
        fprintf(stderr, "lifewatch: Linux Audit unavailable after reload: %s\n", strerror(errno));
    *runtime = fresh;
    fprintf(stderr, "lifewatch: configuration reloaded\n");
    return 0;
}

static int run_daemon(const char *config_path)
{
    static Runtime runtime;
    char error[512];
    struct sigaction action;
    struct timespec last_scan = {0, 0};

    memset(&runtime, 0, sizeof(runtime));
    runtime.inotify_fd = runtime.fanotify_fd = -1;
    runtime.audit.listener_fd = runtime.audit.control_fd = -1;
    runtime.proc.fd = -1;
    snprintf(runtime.config_path, sizeof(runtime.config_path), "%s", config_path);
    if (load_config(config_path, &runtime.config, error, sizeof(error)) < 0) {
        fprintf(stderr, "lifewatch: %s\n", error);
        return 1;
    }
    if (runtime_watch_init(&runtime) < 0) {
        fprintf(stderr, "lifewatch: cannot initialize inotify: %s\n", strerror(errno));
        return 1;
    }
    if (audit_start(&runtime.audit) < 0)
        fprintf(stderr, "lifewatch: Linux Audit unavailable; signal attribution disabled: %s\n", strerror(errno));

    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);

    while (!stop_requested) {
        struct pollfd descriptors[4 + MAX_TARGETS];
        nfds_t count = 0;
        int audit_index = -1, proc_index = -1, fan_index = -1, inotify_index = -1;
        struct timespec now;
        bool scan_now = false;
        size_t i;

        if (runtime.audit.listener_fd >= 0) {
            audit_index = (int)count;
            descriptors[count++] = (struct pollfd){runtime.audit.listener_fd, POLLIN, 0};
        }
        if (runtime.proc.fd >= 0) {
            proc_index = (int)count;
            descriptors[count++] = (struct pollfd){runtime.proc.fd, POLLIN, 0};
        }
        if (runtime.fanotify_fd >= 0) {
            fan_index = (int)count;
            descriptors[count++] = (struct pollfd){runtime.fanotify_fd, POLLIN, 0};
        }
        inotify_index = (int)count;
        descriptors[count++] = (struct pollfd){runtime.inotify_fd, POLLIN, 0};
        for (i = 0; i < runtime.config.process_count; i++) {
            if (runtime.processes[i].pidfd >= 0)
                descriptors[count++] = (struct pollfd){runtime.processes[i].pidfd, POLLIN, 0};
        }
        if (poll(descriptors, count, runtime.config.interval_ms) < 0 && errno != EINTR) {
            emit_event(&runtime.config, LEVEL_CRITICAL, "monitor_error", "lifewatch", "poll failed: %s", strerror(errno));
            break;
        }
        if (audit_index >= 0 && descriptors[audit_index].revents & POLLIN) audit_receive(&runtime.audit);
        if (proc_index >= 0 && descriptors[proc_index].revents & POLLIN) proc_connector_receive(&runtime.proc);
        if (fan_index >= 0 && descriptors[fan_index].revents & POLLIN) handle_fanotify(&runtime);
        if (inotify_index >= 0 && descriptors[inotify_index].revents & POLLIN) handle_inotify(&runtime);
        for (i = 0; i < count; i++) {
            if ((int)i == audit_index || (int)i == proc_index || (int)i == fan_index || (int)i == inotify_index) continue;
            if (descriptors[i].revents & (POLLIN | POLLHUP | POLLERR)) scan_now = true;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - last_scan.tv_sec) * 1000L + (now.tv_nsec - last_scan.tv_nsec) / 1000000L >= runtime.config.interval_ms)
            scan_now = true;
        if (scan_now) {
            if (runtime.audit.listener_fd >= 0) audit_receive(&runtime.audit);
            scan_processes(&runtime);
            scan_files(&runtime);
            last_scan = now;
        }
        if (reload_requested) {
            reload_requested = 0;
            reload_runtime(&runtime);
            last_scan.tv_sec = last_scan.tv_nsec = 0;
        }
    }
    runtime_close(&runtime);
    return 0;
}

static int check_targets(const char *config_path)
{
    Config config;
    char error[512];
    size_t i;
    int failures = 0;
    if (load_config(config_path, &config, error, sizeof(error)) < 0) {
        fprintf(stderr, "lifewatch: %s\n", error);
        return 2;
    }
    for (i = 0; i < config.process_count; i++) {
        unsigned long long ticks;
        pid_t pid = find_process(&config.processes[i], &ticks);
        printf("process %-24s %s", config.processes[i].label, pid > 0 ? "present" : "missing");
        if (pid > 0) printf(" pid=%ld", (long)pid);
        putchar('\n');
        if (pid <= 0) failures++;
    }
    for (i = 0; i < config.file_count; i++) {
        struct stat st;
        bool exists = lstat(config.files[i].path, &st) == 0;
        printf("file    %-24s %s path=%s\n", config.files[i].label, exists ? "present" : "missing", config.files[i].path);
        if (!exists) failures++;
    }
    return failures ? 1 : 0;
}

static void tui_add_clipped(WINDOW *window, int y, int x, const char *text, int width);

static int tui_display_width(const char *text)
{
    wchar_t wide[256];
    size_t length = mbstowcs(wide, text, sizeof(wide) / sizeof(wide[0]) - 1);
    int width;
    if (length == (size_t)-1) return (int)strlen(text);
    wide[length] = L'\0';
    width = wcswidth(wide, length);
    return width >= 0 ? width : (int)length;
}

static WINDOW *tui_popup(const char *title, int wanted_height, int wanted_width)
{
    int rows, columns, height, width, y, x, title_width;
    WINDOW *window;
    getmaxyx(stdscr, rows, columns);
    height = wanted_height < rows - 2 ? wanted_height : rows - 2;
    width = wanted_width < columns - 2 ? wanted_width : columns - 2;
    if (height < 5) height = rows;
    if (width < 24) width = columns;
    y = (rows - height) / 2;
    x = (columns - width) / 2;
    window = newwin(height, width, y, x);
    if (!window) return NULL;
    keypad(window, true);
    box(window, 0, 0);
    title_width = tui_display_width(title);
    if (width > title_width + 4) {
        wattron(window, A_BOLD);
        mvwprintw(window, 0, (width - title_width - 2) / 2, " %s ", title);
        wattroff(window, A_BOLD);
    }
    return window;
}

static bool tui_prompt(const char *label, char *buffer, size_t size)
{
    WINDOW *window = tui_popup(tui_text("Input", "入力"), 7, 76);
    size_t length = 0;
    int height, width, key, result;
    wint_t input;
    if (!window || size == 0) return false;
    buffer[0] = '\0';
    getmaxyx(window, height, width);
    curs_set(1);
    for (;;) {
        tui_add_clipped(window, 1, 2, label, width - 4);
        wattron(window, A_REVERSE);
        mvwhline(window, 3, 2, ' ', width - 4);
        tui_add_clipped(window, 3, 2, buffer, width - 4);
        wattroff(window, A_REVERSE);
        mvwaddnstr(window, height - 2, 2,
                   tui_text("Enter Apply   Esc Cancel", "Enter 確定   Esc キャンセル"), width - 4);
        {
            int cursor = tui_display_width(buffer);
            if (cursor > width - 5) cursor = width - 5;
            wmove(window, 3, 2 + cursor);
        }
        wrefresh(window);
        result = wget_wch(window, &input);
        if (result == ERR) continue;
        key = (int)input;
        if (key == '\n' || key == '\r' || key == KEY_ENTER) break;
        if (key == 27) {
            buffer[0] = '\0';
            length = 0;
            curs_set(0);
            delwin(window);
            touchwin(stdscr);
            clearok(stdscr, true);
            return false;
        }
        if (key == KEY_BACKSPACE || key == 127 || key == 8) {
            if (length > 0) {
                length--;
                while (length > 0 && ((unsigned char)buffer[length] & 0xc0) == 0x80) length--;
                buffer[length] = '\0';
            }
        } else if (result == OK && input >= L' ' && input != 0x7f) {
            char encoded[MB_LEN_MAX];
            mbstate_t state;
            size_t encoded_length;
            memset(&state, 0, sizeof(state));
            encoded_length = wcrtomb(encoded, (wchar_t)input, &state);
            if (encoded_length != (size_t)-1 && length + encoded_length < size) {
                memcpy(buffer + length, encoded, encoded_length);
                length += encoded_length;
                buffer[length] = '\0';
            }
        }
    }
    curs_set(0);
    delwin(window);
    touchwin(stdscr);
    clearok(stdscr, true);
    return true;
}

static int compare_process_choice(const void *left, const void *right)
{
    const ProcessChoice *a = left;
    const ProcessChoice *b = right;
    return a->pid < b->pid ? -1 : a->pid > b->pid ? 1 : 0;
}

static int load_process_choices(ProcessChoice **result, size_t *result_count)
{
    DIR *directory = opendir("/proc");
    struct dirent *entry;
    ProcessChoice *choices = NULL;
    size_t count = 0, capacity = 0;
    if (!directory) return -1;
    while ((entry = readdir(directory)) != NULL) {
        char *end = NULL;
        long parsed_pid;
        ProcessChoice choice;
        if (!isdigit((unsigned char)entry->d_name[0])) continue;
        parsed_pid = strtol(entry->d_name, &end, 10);
        if (!end || *end || parsed_pid <= 0 || parsed_pid > INT_MAX || parsed_pid == (long)getpid()) continue;
        memset(&choice, 0, sizeof(choice));
        choice.pid = (pid_t)parsed_pid;
        if (read_process_start_ticks(choice.pid, &choice.start_ticks) < 0 ||
            read_process_comm(choice.pid, choice.comm, sizeof(choice.comm)) < 0) continue;
        read_process_ppid(choice.pid, &choice.ppid);
        if (read_process_exe(choice.pid, choice.exe, sizeof(choice.exe)) < 0)
            snprintf(choice.exe, sizeof(choice.exe), "%s", tui_text("[unavailable]", "[取得不可]"));
        if (count == capacity) {
            size_t next_capacity = capacity ? capacity * 2 : 128;
            ProcessChoice *replacement = realloc(choices, next_capacity * sizeof(*choices));
            if (!replacement) {
                free(choices);
                closedir(directory);
                return -1;
            }
            choices = replacement;
            capacity = next_capacity;
        }
        choices[count++] = choice;
    }
    closedir(directory);
    qsort(choices, count, sizeof(*choices), compare_process_choice);
    *result = choices;
    *result_count = count;
    return 0;
}

static bool process_choice_matches(const ProcessChoice *choice, const char *filter)
{
    char pid[32];
    if (!filter[0]) return true;
    snprintf(pid, sizeof(pid), "%ld", (long)choice->pid);
    return strcasestr(choice->comm, filter) || strcasestr(choice->exe, filter) || strstr(pid, filter);
}

static bool tui_pick_process(ProcessChoice *picked)
{
    ProcessChoice *choices = NULL;
    size_t *visible = NULL, count = 0, visible_count, selected = 0, start, i;
    char filter[128] = "", title[256], footer[256];
    WINDOW *window;
    int screen_rows, screen_columns, height, width, page, key;

    if (load_process_choices(&choices, &count) < 0) return false;
    visible = malloc((count ? count : 1) * sizeof(*visible));
    if (!visible) {
        free(choices);
        return false;
    }
    getmaxyx(stdscr, screen_rows, screen_columns);
    window = tui_popup(tui_text("Select process", "プロセスを選択"), screen_rows - 2, screen_columns - 2);
    if (!window) {
        free(visible);
        free(choices);
        return false;
    }
    getmaxyx(window, height, width);
    page = height - 4;
    if (page < 1) page = 1;
    for (;;) {
        visible_count = 0;
        for (i = 0; i < count; i++)
            if (process_choice_matches(&choices[i], filter)) visible[visible_count++] = i;
        if (visible_count == 0) selected = 0;
        else if (selected >= visible_count) selected = visible_count - 1;
        start = selected >= (size_t)page ? selected - (size_t)page + 1 : 0;

        werase(window);
        box(window, 0, 0);
        snprintf(title, sizeof(title), tui_text(" Select process (%zu/%zu) ", " プロセスを選択 (%zu/%zu) "),
                 visible_count, count);
        wattron(window, A_BOLD);
        tui_add_clipped(window, 0, 2, title, width - 4);
        wattroff(window, A_BOLD);
        wattron(window, A_DIM);
        tui_add_clipped(window, 1, 2, "PID", 8);
        tui_add_clipped(window, 1, 11, "PPID", 8);
        tui_add_clipped(window, 1, 20, tui_text("NAME", "名前"), 18);
        tui_add_clipped(window, 1, 40, tui_text("EXECUTABLE", "実行ファイル"), width - 42);
        wattroff(window, A_DIM);
        if (visible_count == 0)
            tui_add_clipped(window, 3, 2, tui_text("No matching processes", "一致するプロセスがありません"), width - 4);
        for (i = 0; i < (size_t)page && start + i < visible_count; i++) {
            const ProcessChoice *choice = &choices[visible[start + i]];
            int row = (int)i + 2;
            if (start + i == selected) wattron(window, A_REVERSE | A_BOLD);
            mvwhline(window, row, 1, ' ', width - 2);
            snprintf(footer, sizeof(footer), "%ld", (long)choice->pid);
            tui_add_clipped(window, row, 2, footer, 8);
            snprintf(footer, sizeof(footer), "%ld", (long)choice->ppid);
            tui_add_clipped(window, row, 11, footer, 8);
            tui_add_clipped(window, row, 20, choice->comm, 18);
            tui_add_clipped(window, row, 40, choice->exe, width - 42);
            if (start + i == selected) wattroff(window, A_REVERSE | A_BOLD);
        }
        snprintf(footer, sizeof(footer), tui_text(
                 "Enter Select  / Filter [%s]  r Refresh  Esc Cancel",
                 "Enter 選択  / 絞り込み [%s]  r 更新  Esc キャンセル"), filter);
        wattron(window, A_DIM);
        tui_add_clipped(window, height - 2, 2, footer, width - 4);
        wattroff(window, A_DIM);
        wrefresh(window);
        key = wgetch(window);
        if ((key == KEY_DOWN || key == 'j') && selected + 1 < visible_count) selected++;
        else if ((key == KEY_UP || key == 'k') && selected > 0) selected--;
        else if (key == KEY_NPAGE && visible_count > 0)
            selected = selected + (size_t)page < visible_count ? selected + (size_t)page : visible_count - 1;
        else if (key == KEY_PPAGE && selected > 0)
            selected = selected > (size_t)page ? selected - (size_t)page : 0;
        else if (key == 'g' && visible_count > 0) selected = 0;
        else if (key == 'G' && visible_count > 0) selected = visible_count - 1;
        else if (key == '/') {
            char replacement[sizeof(filter)] = "";
            if (tui_prompt(tui_text("Filter by PID, name, or executable", "PID・名前・実行ファイルで絞り込み"),
                           replacement, sizeof(replacement))) {
                snprintf(filter, sizeof(filter), "%s", replacement);
                selected = 0;
            }
        } else if (key == 'r') {
            ProcessChoice *replacement = NULL;
            size_t replacement_count = 0;
            if (load_process_choices(&replacement, &replacement_count) == 0) {
                size_t *replacement_visible = realloc(visible,
                    (replacement_count ? replacement_count : 1) * sizeof(*visible));
                if (replacement_visible) {
                    free(choices);
                    choices = replacement;
                    count = replacement_count;
                    visible = replacement_visible;
                    selected = 0;
                } else free(replacement);
            }
        } else if ((key == '\n' || key == '\r' || key == KEY_ENTER) && visible_count > 0) {
            unsigned long long current_ticks = 0;
            ProcessChoice choice = choices[visible[selected]];
            if (read_process_start_ticks(choice.pid, &current_ticks) == 0 && current_ticks == choice.start_ticks) {
                *picked = choice;
                delwin(window);
                free(visible);
                free(choices);
                touchwin(stdscr);
                clearok(stdscr, true);
                return true;
            }
        } else if (key == 27 || key == 'q') break;
    }
    delwin(window);
    free(visible);
    free(choices);
    touchwin(stdscr);
    clearok(stdscr, true);
    return false;
}

static bool tui_add_process(Config *config, char *status, size_t status_size)
{
    ProcessChoice choice;
    char label[MAX_LABEL];
    size_t i;
    bool duplicate = false;
    ProcessConfig *process;
    if (config->process_count >= MAX_TARGETS) {
        snprintf(status, status_size, "%s", tui_text("Maximum process monitor count reached", "プロセス監視数が上限に達しました"));
        return false;
    }
    if (!tui_pick_process(&choice)) {
        snprintf(status, status_size, "%s", tui_text("Add process cancelled", "プロセスの追加をキャンセルしました"));
        return false;
    }
    for (i = 0; i < config->process_count; i++) {
        if (config->processes[i].target_pid == choice.pid &&
            config->processes[i].target_start_ticks == choice.start_ticks) {
            snprintf(status, status_size, "%s", tui_text("That process is already monitored", "そのプロセスはすでに監視されています"));
            return false;
        }
    }
    snprintf(label, sizeof(label), "%.48s", choice.comm);
    for (i = 0; i < config->process_count; i++)
        if (strcmp(config->processes[i].label, label) == 0) duplicate = true;
    for (i = 0; i < config->file_count; i++)
        if (strcmp(config->files[i].label, label) == 0) duplicate = true;
    if (duplicate) snprintf(label, sizeof(label), "%.40s-%ld", choice.comm, (long)choice.pid);
    duplicate = false;
    for (i = 0; i < config->process_count; i++)
        if (strcmp(config->processes[i].label, label) == 0) duplicate = true;
    for (i = 0; i < config->file_count; i++)
        if (strcmp(config->files[i].label, label) == 0) duplicate = true;
    if (duplicate) snprintf(label, sizeof(label), "PID-%ld", (long)choice.pid);
    process = &config->processes[config->process_count++];
    memset(process, 0, sizeof(*process));
    snprintf(process->label, sizeof(process->label), "%s", label);
    process->match_type = MATCH_PID;
    process->target_pid = choice.pid;
    process->target_start_ticks = choice.start_ticks;
    snprintf(process->value, sizeof(process->value), "%ld@%llu", (long)choice.pid, choice.start_ticks);
    snprintf(status, status_size,
             tui_text("Added process monitor '%s' (PID %ld)", "プロセス監視「%s」を追加しました（PID %ld）"),
             label, (long)choice.pid);
    return true;
}

static bool tui_add_file(Config *config, char *status, size_t status_size)
{
    char label[MAX_LABEL] = "", path[PATH_MAX] = "";
    FileConfig *file;
    if (config->file_count >= MAX_TARGETS) {
        snprintf(status, status_size, "%s", tui_text("Maximum file monitor count reached", "ファイル監視数が上限に達しました"));
        return false;
    }
    if (!tui_prompt(tui_text("Monitor label", "監視名"), label, sizeof(label)) ||
        !tui_prompt(tui_text("Absolute file path", "ファイルの絶対パス"), path, sizeof(path))) {
        snprintf(status, status_size, "%s", tui_text("Add file cancelled", "ファイルの追加をキャンセルしました"));
        return false;
    }
    if (!safe_config_text(label) || !safe_config_text(path) || path[0] != '/') {
        snprintf(status, status_size, "%s", tui_text("Label is required and file path must be absolute", "監視名は必須で、ファイルパスは絶対パスで指定してください"));
        return false;
    }
    file = &config->files[config->file_count++];
    memset(file, 0, sizeof(*file));
    snprintf(file->label, sizeof(file->label), "%s", label);
    snprintf(file->path, sizeof(file->path), "%s", path);
    snprintf(status, status_size, tui_text("Added file monitor '%s'", "ファイル監視「%s」を追加しました"), label);
    return true;
}

static void tui_delete(Config *config, size_t selected, char *status, size_t status_size)
{
    if (selected < config->process_count) {
        snprintf(status, status_size, tui_text("Deleted '%s'", "「%s」を削除しました"), config->processes[selected].label);
        memmove(&config->processes[selected], &config->processes[selected + 1],
                (config->process_count - selected - 1) * sizeof(config->processes[0]));
        config->process_count--;
    } else if (selected - config->process_count < config->file_count) {
        size_t index = selected - config->process_count;
        snprintf(status, status_size, tui_text("Deleted '%s'", "「%s」を削除しました"), config->files[index].label);
        memmove(&config->files[index], &config->files[index + 1],
                (config->file_count - index - 1) * sizeof(config->files[0]));
        config->file_count--;
    }
}

static bool tui_edit(Config *config, size_t selected, char *status, size_t status_size)
{
    char label[MAX_LABEL] = "", value[PATH_MAX] = "";
    if (selected < config->process_count) {
        ProcessConfig *process = &config->processes[selected];
        if (!tui_prompt(tui_text("New label (empty keeps current)", "新しい監視名（空欄なら変更なし）"), label, sizeof(label))) {
            snprintf(status, status_size, "%s", tui_text("Edit cancelled", "編集をキャンセルしました"));
            return false;
        }
        if (label[0] && !safe_config_text(label)) goto invalid;
        if (label[0]) snprintf(process->label, sizeof(process->label), "%s", label);
        snprintf(status, status_size, tui_text("Updated process monitor '%s'", "プロセス監視「%s」を更新しました"), process->label);
        return true;
    }
    if (selected - config->process_count < config->file_count) {
        FileConfig *file = &config->files[selected - config->process_count];
        if (!tui_prompt(tui_text("New label (empty keeps current)", "新しい監視名（空欄なら変更なし）"), label, sizeof(label)) ||
            !tui_prompt(tui_text("New absolute file path (empty keeps current)", "新しいファイルの絶対パス（空欄なら変更なし）"), value, sizeof(value))) {
            snprintf(status, status_size, "%s", tui_text("Edit cancelled", "編集をキャンセルしました"));
            return false;
        }
        if (label[0] && !safe_config_text(label)) goto invalid;
        if (value[0] && (!safe_config_text(value) || value[0] != '/')) goto invalid;
        if (label[0]) snprintf(file->label, sizeof(file->label), "%s", label);
        if (value[0]) snprintf(file->path, sizeof(file->path), "%s", value);
        snprintf(status, status_size, tui_text("Updated file monitor '%s'", "ファイル監視「%s」を更新しました"), file->label);
        return true;
    }
invalid:
    snprintf(status, status_size, "%s", tui_text("Invalid value", "入力値が不正です"));
    return false;
}

static void tui_test_monitor(const Config *config, size_t selected, char *status, size_t status_size)
{
    if (selected < config->process_count) {
        unsigned long long ticks = 0;
        pid_t pid = find_process(&config->processes[selected], &ticks);
        if (pid > 0) snprintf(status, status_size,
                              tui_text("%s is present (PID %ld)", "%s は稼働中です（PID %ld）"),
                              config->processes[selected].label, (long)pid);
        else snprintf(status, status_size, tui_text("%s is missing", "%s は見つかりません"), config->processes[selected].label);
    } else if (selected - config->process_count < config->file_count) {
        const FileConfig *file = &config->files[selected - config->process_count];
        struct stat st;
        if (lstat(file->path, &st) == 0) snprintf(status, status_size, tui_text("%s is present", "%s は存在します"), file->label);
        else snprintf(status, status_size, tui_text("%s is missing: %s", "%s は見つかりません: %s"), file->label, strerror(errno));
    } else {
        snprintf(status, status_size, "%s", tui_text("No monitor selected", "監視対象が選択されていません"));
    }
}

static void tui_add_clipped(WINDOW *window, int y, int x, const char *text, int width)
{
    wchar_t wide[MAX_VALUE + 256];
    size_t length, i;
    int cells = 0;
    if (width <= 0) return;
    length = mbstowcs(wide, text, sizeof(wide) / sizeof(wide[0]) - 1);
    if (length == (size_t)-1) {
        mvwaddnstr(window, y, x, text, width);
        return;
    }
    for (i = 0; i < length; i++) {
        int character_width = wcwidth(wide[i]);
        if (character_width < 0) character_width = 1;
        if (cells + character_width > width) break;
        cells += character_width;
    }
    wide[i] = L'\0';
    mvwaddnwstr(window, y, x, wide, (int)i);
}

static attr_t tui_status_attr(const char *status)
{
    if (strstr(status, "failed") || strstr(status, "Invalid") || strstr(status, "missing") ||
        strstr(status, "失敗") || strstr(status, "不正") || strstr(status, "見つかりません"))
        return A_BOLD | (has_colors() ? COLOR_PAIR(COLOR_ERROR) : 0);
    if (strstr(status, "Saved") || strstr(status, "updated") || strstr(status, "present") ||
        strstr(status, "保存しました") || strstr(status, "更新しました") || strstr(status, "存在します") || strstr(status, "稼働中"))
        return A_BOLD | (has_colors() ? COLOR_PAIR(COLOR_OK) : 0);
    if (strstr(status, "Unsaved") || strstr(status, "New configuration") ||
        strstr(status, "未保存") || strstr(status, "新しい設定"))
        return A_BOLD | (has_colors() ? COLOR_PAIR(COLOR_WARN) : 0);
    return 0;
}

static void tui_help(void)
{
    static const char *english[] = {
        "Navigation", "  Up / k      Previous monitor", "  Down / j    Next monitor",
        "  PgUp/PgDn   Move one page", "  g / G       First / last monitor", "",
        "Monitor", "  p           Select a running process", "  f           Add file monitor",
        "  Enter / e   Edit selected monitor", "  d           Delete selected monitor",
        "  t           Test selected monitor", "", "Configuration",
        "  i           Check interval", "  n / b       SSH notify / terminal bell",
        "  v           Minimum notification level", "  l           Notification log path",
        "  N           Notification language", "  L           Switch UI language",
        "  s           Save", "", "Other", "  ?           Help", "  q           Quit"
    };
    static const char *japanese[] = {
        "移動", "  Up / k      前の監視対象", "  Down / j    次の監視対象",
        "  PgUp/PgDn   1ページ移動", "  g / G       先頭 / 末尾", "",
        "監視対象", "  p           実行中プロセスを選択", "  f           ファイル監視を追加",
        "  Enter / e   選択した監視対象を編集", "  d           選択した監視対象を削除",
        "  t           選択した監視対象をテスト", "", "設定",
        "  i           確認間隔", "  n / b       SSH通知 / 端末ベル",
        "  v           最低通知レベル", "  l           通知ログのパス",
        "  N           通知言語", "  L           表示言語を切り替え",
        "  s           保存", "", "その他", "  ?           ヘルプ", "  q           終了"
    };
    const char **lines = tui_japanese ? japanese : english;
    size_t count = sizeof(english) / sizeof(english[0]);
    const char *title = tui_text("Help", "ヘルプ");
    WINDOW *window = tui_popup(title, (int)count + 3, 68);
    int height, width, key, page, title_width;
    size_t i, offset = 0;
    if (!window) return;
    getmaxyx(window, height, width);
    page = height - 3;
    if (page < 1) page = 1;
    title_width = tui_display_width(title);
    for (;;) {
        werase(window);
        box(window, 0, 0);
        if (width > title_width + 4) {
            wattron(window, A_BOLD);
            mvwprintw(window, 0, (width - title_width - 2) / 2, " %s ", title);
            wattroff(window, A_BOLD);
        }
        for (i = 0; i < (size_t)page && offset + i < count; i++) {
            const char *line = lines[offset + i];
            if (line[0] && !isspace((unsigned char)line[0])) wattron(window, A_BOLD);
            tui_add_clipped(window, (int)i + 1, 2, line, width - 4);
            wattroff(window, A_BOLD);
        }
        wattron(window, A_DIM);
        tui_add_clipped(window, height - 2, 2,
                        tui_text("Up/Down/PgUp/PgDn Scroll   q/Esc Close",
                                 "↑/↓/PgUp/PgDn スクロール   q/Esc 閉じる"), width - 4);
        wattroff(window, A_DIM);
        wrefresh(window);
        key = wgetch(window);
        if ((key == KEY_DOWN || key == 'j') && offset + (size_t)page < count) offset++;
        else if ((key == KEY_UP || key == 'k') && offset > 0) offset--;
        else if (key == KEY_NPAGE && offset + (size_t)page < count) {
            offset += (size_t)page;
            if (offset + (size_t)page > count) offset = count > (size_t)page ? count - (size_t)page : 0;
        } else if (key == KEY_PPAGE) {
            offset = offset > (size_t)page ? offset - (size_t)page : 0;
        } else if (key == 'g') offset = 0;
        else if (key == 'G') offset = count > (size_t)page ? count - (size_t)page : 0;
        else break;
    }
    delwin(window);
    touchwin(stdscr);
    clearok(stdscr, true);
}

static void tui_draw(const Config *config, size_t selected, const char *path,
                     const char *status, bool dirty)
{
    int rows, columns, monitor_height, settings_height, content_rows, start = 0, row;
    size_t total = config->process_count + config->file_count;
    size_t index;
    WINDOW *monitors, *settings;
    char title[256], line[MAX_VALUE + 256];

    getmaxyx(stdscr, rows, columns);
    erase();
    if (rows < 16 || columns < 60) {
        tui_add_clipped(stdscr, 0, 0,
                        tui_text("Terminal too small (minimum 60x16)", "端末が小さすぎます（最低60x16）"), columns);
        wnoutrefresh(stdscr);
        doupdate();
        return;
    }
    settings_height = 9;
    monitor_height = rows - settings_height;
    monitors = newwin(monitor_height, columns, 0, 0);
    settings = newwin(settings_height, columns, monitor_height, 0);
    if (!monitors || !settings) {
        if (monitors) delwin(monitors);
        if (settings) delwin(settings);
        return;
    }
    box(monitors, 0, 0);
    box(settings, 0, 0);
    snprintf(title, sizeof(title), tui_text(" Monitors (%zu)%s ", " 監視対象 (%zu)%s "),
             total, dirty ? tui_text("  [unsaved]", "  [未保存]") : "");
    wattron(monitors, A_BOLD | (dirty && has_colors() ? COLOR_PAIR(COLOR_WARN) : 0));
    tui_add_clipped(monitors, 0, 2, title, columns - 4);
    wattroff(monitors, A_BOLD | (dirty && has_colors() ? COLOR_PAIR(COLOR_WARN) : 0));
    wattron(settings, A_BOLD);
    tui_add_clipped(settings, 0, 2, tui_text(" Configuration ", " 設定 "), columns - 4);
    wattroff(settings, A_BOLD);

    wattron(monitors, A_DIM);
    tui_add_clipped(monitors, 1, 3, tui_text("TYPE", "種別"), 8);
    tui_add_clipped(monitors, 1, 13, tui_text("STATE", "状態"), 10);
    tui_add_clipped(monitors, 1, 25, tui_text("LABEL", "監視名"), 20);
    tui_add_clipped(monitors, 1, 47, tui_text("SELECTOR", "対象"), columns - 48);
    wattroff(monitors, A_DIM);
    content_rows = monitor_height - 3;
    if (content_rows < 1) content_rows = 1;
    if (selected >= (size_t)content_rows) start = (int)selected - content_rows + 1;
    if ((size_t)start + (size_t)content_rows > total && total > (size_t)content_rows)
        start = (int)total - content_rows;
    if (total == 0) {
        wattron(monitors, A_DIM);
        tui_add_clipped(monitors, 3, 3,
                        tui_text("No monitors configured. Press p or f to add one.",
                                 "監視対象は未登録です。p または f で追加できます。"), columns - 6);
        wattroff(monitors, A_DIM);
    }
    row = 2;
    for (index = (size_t)start; index < total && row < monitor_height - 1; index++, row++) {
        bool is_process = index < config->process_count;
        bool present;
        const char *label;
        const char *selector;
        char selector_storage[64];
        const char *type = is_process ? tui_text("process", "プロセス") : tui_text("file", "ファイル");
        const char *state_text;
        unsigned long long ticks = 0;
        attr_t state_attr;
        if (is_process) {
            const ProcessConfig *process = &config->processes[index];
            present = find_process(process, &ticks) > 0;
            label = process->label;
            snprintf(selector_storage, sizeof(selector_storage), "PID %ld", (long)process->target_pid);
            selector = selector_storage;
        } else {
            const FileConfig *file = &config->files[index - config->process_count];
            struct stat st;
            present = lstat(file->path, &st) == 0;
            label = file->label;
            selector = file->path;
        }
        state_text = present ? tui_text("present", is_process ? "稼働" : "存在") : tui_text("missing", "消失");
        if (index == selected) wattron(monitors, A_REVERSE | A_BOLD);
        else if (has_colors()) wattron(monitors, COLOR_PAIR(COLOR_ACCENT));
        mvwhline(monitors, row, 1, ' ', columns - 2);
        mvwaddch(monitors, row, 1, index == selected ? '>' : ' ');
        tui_add_clipped(monitors, row, 3, type, 8);
        tui_add_clipped(monitors, row, 13, state_text, 10);
        tui_add_clipped(monitors, row, 25, label, 20);
        tui_add_clipped(monitors, row, 47, selector, columns - 48);
        if (index == selected) wattroff(monitors, A_REVERSE | A_BOLD);
        else if (has_colors()) wattroff(monitors, COLOR_PAIR(COLOR_ACCENT));
        state_attr = A_BOLD | (has_colors() ? COLOR_PAIR(present ? COLOR_OK : COLOR_ERROR) : 0);
        if (index != selected) {
            wattron(monitors, state_attr);
            mvwaddnstr(monitors, row, 13, state_text, 10);
            wattroff(monitors, state_attr);
        }
    }

    snprintf(line, sizeof(line), tui_text("Config     %s", "設定ファイル  %s"), path);
    tui_add_clipped(settings, 1, 2, line, columns - 4);
    snprintf(line, sizeof(line),
             tui_text("Runtime    interval=%dms  SSH notify=%s  bell=%s  minimum=%s",
                      "動作        間隔=%dms  SSH通知=%s  ベル=%s  最低=%s"),
             config->interval_ms,
             config->notify_ssh ? tui_text("on", "有効") : tui_text("off", "無効"),
             config->terminal_bell ? tui_text("on", "有効") : tui_text("off", "無効"),
             severity_display(config->min_level));
    tui_add_clipped(settings, 2, 2, line, columns - 4);
    snprintf(line, sizeof(line), tui_text("Log        %s", "ログ        %s"), config->log_file);
    tui_add_clipped(settings, 3, 2, line, columns - 4);
    snprintf(line, sizeof(line),
             tui_text("Language   UI=%s (%s)  notification=%s",
                      "言語        表示=%s（%s）  通知=%s"),
             language_name(config->ui_language), tui_japanese ? "ja" : "en",
             language_name(config->notification_language));
    tui_add_clipped(settings, 4, 2, line, columns - 4);
    if (total > 0 && selected < total) {
        if (selected < config->process_count) {
            const ProcessConfig *process = &config->processes[selected];
            snprintf(line, sizeof(line), tui_text("Selected   process %s  PID=%ld",
                                                  "選択        プロセス %s  PID=%ld"), process->label,
                     (long)process->target_pid);
        } else {
            const FileConfig *file = &config->files[selected - config->process_count];
            snprintf(line, sizeof(line), tui_text("Selected   file %s  path=%s",
                                                  "選択        ファイル %s  パス=%s"), file->label, file->path);
        }
        if (has_colors()) wattron(settings, COLOR_PAIR(COLOR_ACCENT));
        tui_add_clipped(settings, 5, 2, line, columns - 4);
        if (has_colors()) wattroff(settings, COLOR_PAIR(COLOR_ACCENT));
    }
    {
        attr_t attr = tui_status_attr(status);
        if (attr) wattron(settings, attr);
        snprintf(line, sizeof(line), tui_text("Status     %s", "状態        %s"), status);
        tui_add_clipped(settings, 6, 2, line, columns - 4);
        if (attr) wattroff(settings, attr);
    }
    /* Mark the blank stdscr background as refreshed before layering the
     * independent panel windows on top.  Otherwise the following getch() on
     * stdscr refreshes its still-dirty contents and clears both panels. */
    wnoutrefresh(stdscr);
    wnoutrefresh(monitors);
    wnoutrefresh(settings);
    doupdate();
    delwin(monitors);
    delwin(settings);
}

static int run_setup(const char *config_path, bool has_language_override, Language language_override)
{
    Config config;
    char error[512], status[512];
    size_t selected = 0;
    bool dirty = false;
    bool new_configuration = false;
    int key;
    if (load_config(config_path, &config, error, sizeof(error)) < 0) {
        if (errno != ENOENT && access(config_path, F_OK) == 0) {
            fprintf(stderr, "lifewatch: %s\n", error);
            return 1;
        }
        config_defaults(&config);
        new_configuration = true;
        dirty = true;
    }
    tui_japanese = language_is_japanese(has_language_override ? language_override : config.ui_language);
    snprintf(status, sizeof(status), "%s",
             new_configuration ? tui_text("New configuration", "新しい設定") : tui_text("Ready", "準備完了"));
    if (has_language_override && language_override == LANGUAGE_JA && !locale_is_utf8())
        snprintf(status, sizeof(status), "Japanese UI requires a UTF-8 locale; using English");
    if (initscr() == NULL) return 1;
    set_escdelay(25);
    cbreak();
    noecho();
    keypad(stdscr, true);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(COLOR_ACCENT, COLOR_CYAN, -1);
        init_pair(COLOR_OK, COLOR_GREEN, -1);
        init_pair(COLOR_ERROR, COLOR_RED, -1);
        init_pair(COLOR_WARN, COLOR_YELLOW, -1);
    }
    for (;;) {
        size_t total = config.process_count + config.file_count;
        int page = getmaxy(stdscr) - 12;
        if (page < 1) page = 1;
        if (total == 0) selected = 0;
        else if (selected >= total) selected = total - 1;
        tui_draw(&config, selected, config_path, status, dirty);
        key = getch();
        if ((key == 'j' || key == KEY_DOWN) && total > 0 && selected + 1 < total) selected++;
        else if ((key == 'k' || key == KEY_UP) && selected > 0) selected--;
        else if (key == KEY_PPAGE && selected > 0) selected = selected > (size_t)page ? selected - (size_t)page : 0;
        else if (key == KEY_NPAGE && total > 0) selected = selected + (size_t)page < total ? selected + (size_t)page : total - 1;
        else if (key == 'g' && total > 0) selected = 0;
        else if (key == 'G' && total > 0) selected = total - 1;
        else if (key == 'p') dirty |= tui_add_process(&config, status, sizeof(status));
        else if (key == 'f') dirty |= tui_add_file(&config, status, sizeof(status));
        else if ((key == 'e' || key == '\n' || key == '\r' || key == KEY_ENTER) && total > 0)
            dirty |= tui_edit(&config, selected, status, sizeof(status));
        else if (key == 'd' && total > 0) { tui_delete(&config, selected, status, sizeof(status)); dirty = true; }
        else if (key == 't') tui_test_monitor(&config, selected, status, sizeof(status));
        else if (key == '?') tui_help();
        else if (key == 'L') {
            if (!tui_japanese && !locale_is_utf8()) {
                snprintf(status, sizeof(status), "Japanese UI requires a UTF-8 locale");
            } else {
                tui_japanese = !tui_japanese;
                config.ui_language = tui_japanese ? LANGUAGE_JA : LANGUAGE_EN;
                dirty = true;
                snprintf(status, sizeof(status), "%s", tui_text("Display language changed to English", "表示言語を日本語に変更しました"));
            }
        }
        else if (key == 'N') {
            config.notification_language = (Language)((config.notification_language + 1) % 3);
            dirty = true;
            snprintf(status, sizeof(status), tui_text("Notification language: %s", "通知言語: %s"),
                     language_name(config.notification_language));
        }
        else if (key == 'n') {
            config.notify_ssh = !config.notify_ssh;
            dirty = true;
            snprintf(status, sizeof(status), tui_text("SSH notification %s", "SSH通知を%sにしました"),
                     config.notify_ssh ? tui_text("enabled", "有効") : tui_text("disabled", "無効"));
        }
        else if (key == 'b') {
            config.terminal_bell = !config.terminal_bell;
            dirty = true;
            snprintf(status, sizeof(status), tui_text("Terminal bell %s", "端末ベルを%sにしました"),
                     config.terminal_bell ? tui_text("enabled", "有効") : tui_text("disabled", "無効"));
        }
        else if (key == 'v') {
            config.min_level = (Severity)((config.min_level + 1) % 4);
            dirty = true;
            snprintf(status, sizeof(status), tui_text("Minimum notification level: %s", "最低通知レベル: %s"),
                     severity_display(config.min_level));
        }
        else if (key == 'l') {
            char value[PATH_MAX] = "";
            if (tui_prompt(tui_text("Absolute notification log path", "通知ログの絶対パス"), value, sizeof(value))) {
                if (value[0] == '/') {
                    snprintf(config.log_file, sizeof(config.log_file), "%s", value);
                    dirty = true;
                    snprintf(status, sizeof(status), "%s", tui_text("Log path updated", "ログのパスを更新しました"));
                } else snprintf(status, sizeof(status), "%s", tui_text("Log path must be absolute", "ログのパスは絶対パスで指定してください"));
            } else snprintf(status, sizeof(status), "%s", tui_text("Log path edit cancelled", "ログパスの編集をキャンセルしました"));
        }
        else if (key == 'i') {
            char value[32] = "";
            long interval;
            if (tui_prompt(tui_text("Check interval in milliseconds (100-60000)", "確認間隔（ミリ秒、100～60000）"), value, sizeof(value))) {
                interval = strtol(value, NULL, 10);
                if (interval >= 100 && interval <= 60000) {
                    config.interval_ms = (int)interval;
                    dirty = true;
                    snprintf(status, sizeof(status), "%s", tui_text("Interval updated", "確認間隔を更新しました"));
                } else snprintf(status, sizeof(status), "%s", tui_text("Invalid interval", "確認間隔が不正です"));
            } else snprintf(status, sizeof(status), "%s", tui_text("Interval edit cancelled", "確認間隔の編集をキャンセルしました"));
        } else if (key == 's') {
            if (save_config(config_path, &config, error, sizeof(error)) == 0) {
                dirty = false;
                snprintf(status, sizeof(status), "%s", tui_text(
                         "Saved. Run 'systemctl reload lifewatch' if the service is active.",
                         "保存しました。サービス稼働中は systemctl reload lifewatch を実行してください。"));
            } else snprintf(status, sizeof(status), tui_text("Save failed: %.490s", "保存に失敗しました: %.490s"), error);
        } else if (key == 'q') {
            if (!dirty) break;
            snprintf(status, sizeof(status), "%s", tui_text(
                     "Unsaved changes: press q again to discard, or s to save",
                     "未保存の変更があります。破棄するには再度 q、保存するには s を押してください"));
            tui_draw(&config, selected, config_path, status, dirty);
            key = getch();
            if (key == 'q') break;
            if (key == 's' && save_config(config_path, &config, error, sizeof(error)) == 0) break;
        }
    }
    endwin();
    return 0;
}

static int show_events(const char *config_path, const char *wanted_id)
{
    Config config;
    char error[512], line[8192];
    FILE *stream;
    if (load_config(config_path, &config, error, sizeof(error)) < 0) {
        fprintf(stderr, "lifewatch: %s\n", error);
        return 1;
    }
    stream = fopen(config.log_file, "r");
    if (!stream) {
        fprintf(stderr, "lifewatch: %s: %s\n", config.log_file, strerror(errno));
        return 1;
    }
    while (fgets(line, sizeof(line), stream)) {
        if (!wanted_id) fputs(line, stdout);
        else {
            char needle[128];
            snprintf(needle, sizeof(needle), "id=%s ", wanted_id);
            if (strstr(line, needle)) fputs(line, stdout);
        }
    }
    fclose(stream);
    return 0;
}

static int validate_config_path(const char *config_path, bool print_success)
{
    Config config;
    char error[512];
    if (load_config(config_path, &config, error, sizeof(error)) < 0) {
        fprintf(stderr, "lifewatch: %s\n", error);
        return 1;
    }
    if (print_success)
        printf("valid: %zu process monitor(s), %zu file monitor(s)\n",
               config.process_count, config.file_count);
    return 0;
}

static int invoke_systemctl(const char *action)
{
    const char *path = access("/usr/bin/systemctl", X_OK) == 0 ? "/usr/bin/systemctl" : "/bin/systemctl";
    pid_t child = fork();
    int status;
    if (child < 0) return -1;
    if (child == 0) {
        if (strcmp(action, "is-active") == 0)
            execl(path, "systemctl", "is-active", "--quiet", "lifewatch.service", (char *)NULL);
        else
            execl(path, "systemctl", action, "lifewatch.service", (char *)NULL);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run_default_workflow(const char *config_path)
{
    const char *action;
    int result = run_setup(config_path, false, LANGUAGE_AUTO);
    if (result != 0) return result;
    if (validate_config_path(config_path, true) != 0) return 1;
    action = invoke_systemctl("is-active") == 0 ? "reload" : "start";
    result = invoke_systemctl(action);
    if (result != 0) {
        fprintf(stderr, "lifewatch: systemctl %s lifewatch.service failed\n", action);
        return 1;
    }
    printf("%s\n", tui_text(action[0] == 'r' ? "lifewatch service reloaded" : "lifewatch service started",
                             action[0] == 'r' ? "lifewatchサービスを再読み込みしました" : "lifewatchサービスを起動しました"));
    return 0;
}

static void usage(FILE *stream)
{
    fprintf(stream,
        "lifewatch %s\n"
        "Usage:\n"
        "  lifewatch\n"
        "  lifewatch setup [--config PATH] [--lang auto|ja|en]\n"
        "  lifewatch daemon [--config PATH]\n"
        "  lifewatch validate [--config PATH]\n"
        "  lifewatch check [--config PATH]\n"
        "  lifewatch events [--config PATH]\n"
        "  lifewatch show EVENT_ID [--config PATH]\n", VERSION);
}

int main(int argc, char **argv)
{
    const char *command;
    const char *config_path = CONFIG_DEFAULT;
    const char *event_id = NULL;
    Language language_override = LANGUAGE_AUTO;
    bool has_language_override = false;
    bool default_workflow = argc < 2;
    int i;
    setlocale(LC_ALL, "");
    if (default_workflow) return run_default_workflow(CONFIG_DEFAULT);
    command = argv[1];
    if (strcmp(command, "show") == 0) {
        if (argc < 3) { usage(stderr); return 2; }
        event_id = argv[2];
        i = 3;
    } else {
        i = 2;
    }
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
            if (!parse_language(argv[++i], &language_override)) {
                fprintf(stderr, "lifewatch: --lang must be auto, ja, or en\n");
                return 2;
            }
            has_language_override = true;
        }
        else if (strcmp(argv[i], "--version") == 0) { printf("%s\n", VERSION); return 0; }
        else { fprintf(stderr, "lifewatch: unknown argument: %s\n", argv[i]); return 2; }
    }
    if (has_language_override && strcmp(command, "setup") != 0) {
        fprintf(stderr, "lifewatch: --lang is only valid with setup\n");
        return 2;
    }
    if (strcmp(command, "setup") == 0) return run_setup(config_path, has_language_override, language_override);
    if (strcmp(command, "daemon") == 0) return run_daemon(config_path);
    if (strcmp(command, "check") == 0) return check_targets(config_path);
    if (strcmp(command, "events") == 0) return show_events(config_path, NULL);
    if (strcmp(command, "show") == 0) return show_events(config_path, event_id);
    if (strcmp(command, "validate") == 0) {
        return validate_config_path(config_path, true);
    }
    usage(stderr);
    return 2;
}
