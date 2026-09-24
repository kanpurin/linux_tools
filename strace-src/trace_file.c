#define _GNU_SOURCE
#include "strace_src.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TRACE_FORMAT_VERSION 3U
#define MAX_EVENTS 10000000ULL
#define MAX_FRAMES 4096U
#define MAX_STRING (16U * 1024U * 1024U)

static const unsigned char trace_magic[8] = {'S', 'S', 'R', 'C', 'B', 'I', 'N', '1'};

static int write_bytes(FILE *file, const void *data, size_t size) {
    return fwrite(data, 1, size, file) == size ? 0 : -1;
}

static int read_bytes(FILE *file, void *data, size_t size) {
    return fread(data, 1, size, file) == size ? 0 : -1;
}

static int write_u32(FILE *file, uint32_t value) {
    unsigned char bytes[4] = {
        (unsigned char)(value >> 24), (unsigned char)(value >> 16),
        (unsigned char)(value >> 8), (unsigned char)value
    };
    return write_bytes(file, bytes, sizeof(bytes));
}

static int write_u64(FILE *file, uint64_t value) {
    unsigned char bytes[8];
    int i;
    for (i = 7; i >= 0; --i) {
        bytes[i] = (unsigned char)value;
        value >>= 8;
    }
    return write_bytes(file, bytes, sizeof(bytes));
}

static int read_u32(FILE *file, uint32_t *value) {
    unsigned char bytes[4];
    if (read_bytes(file, bytes, sizeof(bytes)) < 0) return -1;
    *value = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
             ((uint32_t)bytes[2] << 8) | bytes[3];
    return 0;
}

static int read_u64(FILE *file, uint64_t *value) {
    unsigned char bytes[8];
    int i;
    uint64_t result = 0;
    if (read_bytes(file, bytes, sizeof(bytes)) < 0) return -1;
    for (i = 0; i < 8; ++i) result = (result << 8) | bytes[i];
    *value = result;
    return 0;
}

static int write_string(FILE *file, const char *text) {
    size_t length = text ? strlen(text) : 0;
    if (length > UINT32_MAX || write_u32(file, (uint32_t)length) < 0) return -1;
    return length ? write_bytes(file, text, length) : 0;
}

static int read_string(FILE *file, char **text) {
    uint32_t length;
    char *value;
    if (read_u32(file, &length) < 0 || length > MAX_STRING) return -1;
    value = malloc((size_t)length + 1);
    if (!value) return -1;
    if (length && read_bytes(file, value, length) < 0) {
        free(value);
        return -1;
    }
    value[length] = '\0';
    *text = value;
    return 0;
}

static int write_event(FILE *file, TraceEvent *event) {
    size_t i;
    trace_event_resolve(event);
    if (write_u32(file, (uint32_t)event->pid) < 0 ||
        write_u32(file, (uint32_t)event->tid) < 0 ||
        write_string(file, event->comm) < 0 ||
        write_string(file, event->text) < 0 ||
        event->frame_count > UINT32_MAX ||
        write_u32(file, (uint32_t)event->frame_count) < 0)
        return -1;
    for (i = 0; i < event->frame_count; ++i) {
        StackFrame *frame = &event->frames[i];
        if (write_string(file, frame->module) < 0 ||
            write_string(file, frame->function) < 0 ||
            write_string(file, frame->offset) < 0 ||
            write_string(file, frame->address) < 0)
            return -1;
    }
    if (write_u32(file, (uint32_t)event->resolution_state) < 0 ||
        write_u32(file, (uint32_t)event->source_frame) < 0 ||
        write_string(file, event->source_path) < 0 ||
        write_u32(file, (uint32_t)event->source_line) < 0 ||
        write_string(file, event->source_function) < 0)
        return -1;
    return 0;
}

int trace_model_save(TraceModel *model, const char *path) {
    char *temporary;
    size_t needed;
    int fd = -1;
    FILE *file = NULL;
    size_t i;
    int result = -1;
    int saved_errno = 0;

    needed = strlen(path) + sizeof(".tmp.XXXXXX");
    temporary = malloc(needed);
    if (!temporary) {
        errno = ENOMEM;
        return -1;
    }
    snprintf(temporary, needed, "%s.tmp.XXXXXX", path);
    fd = mkstemp(temporary);
    if (fd < 0 || !(file = fdopen(fd, "wb"))) goto done;
    fd = -1;
    if (write_bytes(file, trace_magic, sizeof(trace_magic)) < 0 ||
        write_u32(file, TRACE_FORMAT_VERSION) < 0 ||
        write_u64(file, model->count) < 0)
        goto done;
    for (i = 0; i < model->count; ++i) {
        if (write_event(file, &model->events[i]) < 0) goto done;
    }
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
        goto done;
    }
    if (fclose(file) != 0) {
        file = NULL;
        goto done;
    }
    file = NULL;
    if (rename(temporary, path) != 0) goto done;
    result = 0;
done:
    if (result < 0) saved_errno = errno ? errno : EIO;
    if (file) fclose(file);
    if (fd >= 0) close(fd);
    if (result < 0) unlink(temporary);
    free(temporary);
    if (result < 0) errno = saved_errno;
    return result;
}

static int read_event(FILE *file, TraceEvent *event, uint32_t version) {
    uint32_t value;
    uint32_t frame_count;
    uint32_t i;
    memset(event, 0, sizeof(*event));
    if (read_u32(file, &value) < 0) return -1;
    event->pid = (int)value;
    if (read_u32(file, &value) < 0) return -1;
    event->tid = (int)value;
    if (version >= 3) {
        if (read_string(file, &event->comm) < 0) return -1;
    } else {
        event->comm = strdup("");
        if (!event->comm) return -1;
    }
    if (read_string(file, &event->text) < 0 || read_u32(file, &frame_count) < 0 ||
        frame_count > MAX_FRAMES)
        return -1;
    if (frame_count) {
        event->frames = calloc(frame_count, sizeof(*event->frames));
        if (!event->frames) return -1;
        event->frame_cap = event->frame_count = frame_count;
    }
    for (i = 0; i < frame_count; ++i) {
        StackFrame *frame = &event->frames[i];
        if (read_string(file, &frame->module) < 0 ||
            read_string(file, &frame->function) < 0 ||
            read_string(file, &frame->offset) < 0 ||
            read_string(file, &frame->address) < 0)
            return -1;
    }
    if (read_u32(file, &value) < 0 || value > 2) return -1;
    event->resolution_state = (int)value;
    if (version >= 2) {
        if (read_u32(file, &value) < 0 || value > event->frame_count) return -1;
        event->source_frame = value;
    }
    if (read_string(file, &event->source_path) < 0 || read_u32(file, &value) < 0)
        return -1;
    event->source_line = (int)value;
    if (read_string(file, &event->source_function) < 0) return -1;
    if (!event->source_path[0]) {
        free(event->source_path);
        event->source_path = NULL;
    }
    if (!event->source_function[0]) {
        free(event->source_function);
        event->source_function = NULL;
    }
    return 0;
}

int trace_model_load(TraceModel *model, const char *path) {
    FILE *file;
    unsigned char magic[8];
    uint32_t version;
    uint64_t count;
    uint64_t i;
    int extra;

    file = fopen(path, "rb");
    if (!file) return -1;
    trace_model_init(model);
    if (read_bytes(file, magic, sizeof(magic)) < 0 ||
        memcmp(magic, trace_magic, sizeof(magic)) != 0 ||
        read_u32(file, &version) < 0 || version < 1 || version > TRACE_FORMAT_VERSION ||
        read_u64(file, &count) < 0 || count > MAX_EVENTS) {
        errno = EINVAL;
        goto fail;
    }
    if (count) {
        model->events = calloc((size_t)count, sizeof(*model->events));
        if (!model->events) goto fail;
        model->cap = (size_t)count;
    }
    for (i = 0; i < count; ++i) {
        model->count++;
        if (read_event(file, &model->events[i], version) < 0) {
            errno = EINVAL;
            goto fail;
        }
        if (model->events[i].resolution_state == 1 &&
            (!model->events[i].source_path || !model->events[i].source_function ||
             model->events[i].source_line <= 0)) {
            model->events[i].resolution_state = 2;
        }
    }
    extra = fgetc(file);
    if (extra != EOF) {
        errno = EINVAL;
        goto fail;
    }
    fclose(file);
    return 0;
fail:
    fclose(file);
    trace_model_free(model);
    return -1;
}
