#include "fd_tracker.h"

#include <assert.h>
#include <stdio.h>

static void add(TraceModel *model, const char *line) {
    assert(trace_model_feed_line(model, line) == FEED_NEW_EVENT);
}

static int object_at(const FdAnalysis *analysis, size_t event) {
    const FdEventInfo *info = fd_analysis_event(analysis, event);
    assert(info && info->primary_ref >= 0);
    assert((size_t)info->primary_ref < info->ref_count);
    return info->refs[info->primary_ref].object_id;
}

static void test_dup_and_reuse(void) {
    TraceModel model;
    FdAnalysis *analysis;
    const FdEventInfo *info;
    trace_model_init(&model);
    add(&model, "[pid 100] openat(AT_FDCWD, \"/tmp/a\", O_RDONLY) = 3");
    add(&model, "[pid 100] read(3, \"a\", 1) = 1");
    add(&model, "[pid 100] dup(3) = 7");
    add(&model, "[pid 100] read(7, \"b\", 1) = 1");
    add(&model, "[pid 100] close(3) = 0");
    add(&model, "[pid 100] openat(AT_FDCWD, \"/tmp/b\", O_RDONLY) = 3");
    add(&model, "[pid 100] read(3, \"c\", 1) = 1");
    analysis = fd_analysis_build(&model);
    assert(analysis);
    assert(object_at(analysis, 0) == object_at(analysis, 1));
    assert(object_at(analysis, 0) == object_at(analysis, 2));
    assert(object_at(analysis, 0) == object_at(analysis, 3));
    assert(object_at(analysis, 0) != object_at(analysis, 5));
    assert(object_at(analysis, 5) == object_at(analysis, 6));
    info = fd_analysis_event(analysis, 3);
    assert(info->jump_event == 2);
    info = fd_analysis_event(analysis, 2);
    assert(info->jump_event == 0);
    info = fd_analysis_event(analysis, 4);
    assert(!info->open_after);
    fd_analysis_free(analysis);
    trace_model_free(&model);
}

static void test_fork_inherits(void) {
    TraceModel model;
    FdAnalysis *analysis;
    trace_model_init(&model);
    add(&model, "[pid 100] openat(AT_FDCWD, \"/tmp/a\", O_RDONLY) = 3");
    add(&model, "[pid 100] clone(child_stack=NULL, flags=SIGCHLD) = 101");
    add(&model, "[pid 100] close(3) = 0");
    add(&model, "[pid 101] read(3, \"a\", 1) = 1");
    analysis = fd_analysis_build(&model);
    assert(analysis);
    assert(object_at(analysis, 0) == object_at(analysis, 3));
    assert(fd_analysis_event(analysis, 3)->jump_event == 0);
    fd_analysis_free(analysis);
    trace_model_free(&model);
}

static void test_cloexec(void) {
    TraceModel model;
    FdAnalysis *analysis;
    const FdEventInfo *info;
    trace_model_init(&model);
    add(&model, "[pid 100] openat(AT_FDCWD, \"/tmp/a\", O_RDONLY|O_CLOEXEC) = 4");
    add(&model, "[pid 100] execve(\"/bin/true\", [\"true\"], 0x0) = 0");
    add(&model, "[pid 100] read(4, \"a\", 1) = -1 EBADF (Bad file descriptor)");
    analysis = fd_analysis_build(&model);
    assert(analysis);
    info = fd_analysis_event(analysis, 2);
    assert(info && info->ref_count == 1);
    assert(info->refs[0].object_id == -1);
    assert(info->jump_event == -1);
    fd_analysis_free(analysis);
    trace_model_free(&model);
}

static void test_pipe_fcntl_and_accept(void) {
    TraceModel model;
    FdAnalysis *analysis;
    trace_model_init(&model);
    add(&model, "[pid 100] pipe([3, 4]) = 0");
    add(&model, "[pid 100] fcntl(3, F_DUPFD, 10) = 7");
    add(&model, "[pid 100] read(7, \"a\", 1) = 1");
    add(&model, "[pid 100] fcntl(4, F_DUPFD_CLOEXEC, 10) = 8");
    add(&model, "[pid 100] execve(\"/bin/true\", [\"true\"], 0x0) = 0");
    add(&model, "[pid 100] read(8, \"a\", 1) = -1 EBADF (Bad file descriptor)");
    add(&model, "[pid 100] read(7, \"a\", 1) = 1");
    add(&model, "[pid 100] socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) = 9");
    add(&model, "[pid 100] accept4(9, NULL, NULL, SOCK_CLOEXEC) = 10");
    add(&model, "[pid 100] read(10, \"a\", 1) = 1");
    analysis = fd_analysis_build(&model);
    assert(analysis);
    assert(object_at(analysis, 0) == object_at(analysis, 1));
    assert(object_at(analysis, 0) == object_at(analysis, 2));
    assert(object_at(analysis, 0) == object_at(analysis, 3));
    assert(fd_analysis_event(analysis, 5)->refs[0].object_id == -1);
    assert(object_at(analysis, 0) == object_at(analysis, 6));
    assert(object_at(analysis, 7) != object_at(analysis, 8));
    assert(object_at(analysis, 8) == object_at(analysis, 9));
    assert(fd_analysis_event(analysis, 9)->jump_event == 8);
    fd_analysis_free(analysis);
    trace_model_free(&model);
}

int main(void) {
    test_dup_and_reuse();
    test_fork_inherits();
    test_cloexec();
    test_pipe_fcntl_and_accept();
    puts("fd tracker tests passed");
    return 0;
}
