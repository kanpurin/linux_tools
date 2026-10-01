#include "../src/gdb.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int heartbeat(int fd) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    char byte;
    return poll(&p, 1, 3000) > 0 && read(fd, &byte, 1) == 1 ? 0 : -1;
}

static bool detached(pid_t pid) {
    char path[64], line[256];
    snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
    FILE *file = fopen(path, "r");
    if (!file) return false;
    bool result = false;
    while (fgets(line, sizeof(line), file)) {
        long tracer;
        if (sscanf(line, "TracerPid: %ld", &tracer) == 1) { result = tracer == 0; break; }
    }
    fclose(file);
    return result;
}

static int scenario(const char *fixture, bool running, bool debug) {
    int channel[2];
    if (pipe(channel)) return 1;
    pid_t child = fork();
    if (child == 0) {
        close(channel[0]);
        dup2(channel[1], STDOUT_FILENO);
        close(channel[1]);
        execl(fixture, fixture, (char *)NULL);
        _exit(127);
    }
    close(channel[1]);
    if (child < 0) { close(channel[0]); return 1; }
    Gdb g = {.to_gdb = -1, .from_gdb = -1};
    int rc = 1;
    if (heartbeat(channel[0]) || gdb_attach(&g, child)) goto cleanup;
    if (g.attached_pid != child || g.state != GDB_STOPPED || !g.frame_count || !g.pc[0]) goto cleanup;
    if (gdb_run(&g) == 0 || !strstr(g.message, "run is disabled")) goto cleanup;
    /* Stop in our own function to check Source/Assembly and frame refresh. */
    if (debug && gdb_watch(&g, "counter")) goto cleanup;
    if (debug) {
        if (gdb_continue(&g)) goto cleanup;
        for (int i = 0; i < 100 && g.state != GDB_STOPPED; ++i)
            if (gdb_poll(&g, 50) < 0) goto cleanup;
        if (g.state != GDB_STOPPED || !g.source_available) goto cleanup;
        char value[128];
        if (gdb_print(&g, "counter", value, sizeof(value))) goto cleanup;
        for (int i = g.break_count - 1; i >= 0; --i)
            if (gdb_delete_breakpoint(&g, g.breaks[i].number)) goto cleanup;
    } else if (!g.instruction_count || !g.register_count) goto cleanup;
    /* Drain old heartbeats so only activity after detach can pass. */
    fcntl(channel[0], F_SETFL, O_NONBLOCK);
    char buffer[1024];
    while (read(channel[0], buffer, sizeof(buffer)) > 0) {}
    if (running && gdb_continue(&g)) goto cleanup;
    gdb_shutdown(&g);
    if (kill(child, 0) || !detached(child) || heartbeat(channel[0])) goto cleanup;
    rc = 0;
cleanup:
    if (rc) fprintf(stderr, "attach failed (running=%d debug=%d): %s\n", running, debug, g.message);
    gdb_shutdown(&g);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    close(channel[0]);
    return rc;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    if (scenario(argv[1], false, true) || scenario(argv[1], true, true) ||
        scenario(argv[2], false, false) || scenario(argv[2], true, false)) return 1;
    Gdb g;
    if (!gdb_attach(&g, 0)) return 1;
    gdb_shutdown(&g);
    if (!gdb_attach(&g, 2147483647)) return 1;
    gdb_shutdown(&g);
    puts("attach smoke: ok");
    return 0;
}
