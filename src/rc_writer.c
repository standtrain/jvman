#include "rc_writer.h"

#include "common.h"
#include "platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 一次性把整个 rc 文件读进内存，识别 jvman marker 块并进行替换/追加/删除。
 * 文件大小上限 1 MiB，超出即拒绝，避免误处理超大 dotfile。
 */
#define JVMAN_RC_FILE_MAX ((size_t)1024u * 1024u)

/* 简易 stringbuilder：足够 rc 文件的规模，失败时释放并返回 NULL。 */
typedef struct RcBuffer {
    char *data;
    size_t length;
    size_t capacity;
} RcBuffer;

static int rc_buffer_reserve(RcBuffer *buffer, size_t needed) {
    size_t capacity;
    char *resized;
    if (needed <= buffer->capacity) return 0;
    capacity = buffer->capacity ? buffer->capacity : 512u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) return -1;
        capacity *= 2u;
    }
    resized = (char *)realloc(buffer->data, capacity);
    if (!resized) return -1;
    buffer->data = resized;
    buffer->capacity = capacity;
    return 0;
}

static int rc_buffer_append(RcBuffer *buffer, const char *bytes, size_t count) {
    if (count == 0) return 0;
    if (rc_buffer_reserve(buffer, buffer->length + count + 1u) != 0) return -1;
    memcpy(buffer->data + buffer->length, bytes, count);
    buffer->length += count;
    buffer->data[buffer->length] = '\0';
    return 0;
}

static void rc_buffer_free(RcBuffer *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

/* 读整文件；不存在时 out_data=NULL,out_length=0 且返回 0（视作空文件）。 */
static int rc_read_file(const char *path, char **out_data, size_t *out_length) {
    FILE *file;
    long size;
    char *data;
    size_t read_bytes;
    *out_data = NULL;
    *out_length = 0;
    file = fopen(path, "rb");
    if (!file) {
        if (errno == ENOENT) return 0;
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return -1; }
    size = ftell(file);
    if (size < 0 || (size_t)size > JVMAN_RC_FILE_MAX) { fclose(file); return -1; }
    if (fseek(file, 0, SEEK_SET) != 0) { fclose(file); return -1; }
    data = (char *)malloc((size_t)size + 1u);
    if (!data) { fclose(file); return -1; }
    read_bytes = fread(data, 1, (size_t)size, file);
    fclose(file);
    if (read_bytes != (size_t)size) { free(data); return -1; }
    data[read_bytes] = '\0';
    *out_data = data;
    *out_length = read_bytes;
    return 0;
}

/*
 * 定位第一处 marker 块。返回 0 表示找到，*block_start 指向 BEGIN 行首（含行首其他空白视为不匹配），
 * *block_end 指向 END 行后的换行之后（一直吞到下一行）。找不到时返回 -1。
 * 只把"BEGIN/END 独占一行且行首无其它非空白字符"识别为 jvman 块，避免误伤用户注释。
 */
static int rc_locate_block(const char *data, size_t length,
                           size_t *block_start_out, size_t *block_end_out) {
    const char *begin_marker = JVMAN_RC_MARKER_BEGIN;
    const char *end_marker = JVMAN_RC_MARKER_END;
    size_t begin_length = strlen(begin_marker);
    size_t end_length = strlen(end_marker);
    size_t i;
    for (i = 0; i + begin_length <= length; ++i) {
        int is_line_start = (i == 0) || data[i - 1] == '\n';
        if (!is_line_start) continue;
        if (memcmp(data + i, begin_marker, begin_length) != 0) continue;
        /* BEGIN 行末必须是换行或文件结束。 */
        {
            size_t after = i + begin_length;
            if (after != length && data[after] != '\n' && data[after] != '\r') {
                continue;
            }
        }
        /* 从 BEGIN 之后找 END。 */
        {
            size_t j;
            for (j = i + begin_length; j + end_length <= length; ++j) {
                int end_at_line_start = data[j - 1] == '\n';
                if (!end_at_line_start) continue;
                if (memcmp(data + j, end_marker, end_length) != 0) continue;
                {
                    size_t after_end = j + end_length;
                    if (after_end != length && data[after_end] != '\n' &&
                        data[after_end] != '\r') {
                        continue;
                    }
                    /* 吞掉 END 行末的 \r\n 或 \n。 */
                    if (after_end < length && data[after_end] == '\r') ++after_end;
                    if (after_end < length && data[after_end] == '\n') ++after_end;
                    *block_start_out = i;
                    *block_end_out = after_end;
                    return 0;
                }
            }
        }
        /* 找到 BEGIN 但没找到 END：视为损坏块，仍返回定位以便替换/删除。 */
        *block_start_out = i;
        *block_end_out = length;
        return 0;
    }
    return -1;
}

/*
 * 用 payload 组装新的 jvman 块（含 BEGIN/END + 换行），写到 buffer。
 * 输出形式：
 *   # >>> jvman initialize >>>\n
 *   <payload>\n            (payload 已含末尾换行时不再追加)
 *   # <<< jvman initialize <<<\n
 */
static int rc_render_block(RcBuffer *buffer, const char *payload) {
    size_t payload_length = payload ? strlen(payload) : 0;
    if (rc_buffer_append(buffer, JVMAN_RC_MARKER_BEGIN,
                         strlen(JVMAN_RC_MARKER_BEGIN)) != 0 ||
        rc_buffer_append(buffer, "\n", 1u) != 0) {
        return -1;
    }
    if (payload_length > 0) {
        if (rc_buffer_append(buffer, payload, payload_length) != 0) return -1;
        if (payload[payload_length - 1u] != '\n') {
            if (rc_buffer_append(buffer, "\n", 1u) != 0) return -1;
        }
    }
    if (rc_buffer_append(buffer, JVMAN_RC_MARKER_END,
                         strlen(JVMAN_RC_MARKER_END)) != 0 ||
        rc_buffer_append(buffer, "\n", 1u) != 0) {
        return -1;
    }
    return 0;
}

/* 用受限权限原子写：先写临时文件，再 rename。已存在文件的模式不改。 */
static int rc_write_file_atomic(const char *path, const char *data, size_t length) {
    char temp_path[JVMAN_PATH_MAX];
    /* 复用 platform_write_text_atomic：它做临时文件 + rename，模式为 0644 或系统默认。 */
    if ((size_t)snprintf(temp_path, sizeof(temp_path), "%s", path) >= sizeof(temp_path)) {
        return -1;
    }
    if (length == 0) {
        /* 空 payload：直接写空字符串。 */
        return platform_write_text_atomic(path, "");
    }
    {
        /* platform_write_text_atomic 期望 NUL 结尾字符串；rc_read_file 已保证末尾 '\0'。 */
        return platform_write_text_atomic(path, data);
    }
}

int jvman_rc_writer_apply(const char *rc_path, const char *payload) {
    char *existing = NULL;
    size_t existing_length = 0;
    RcBuffer output = {0};
    size_t block_start = 0;
    size_t block_end = 0;
    int has_block;
    int result = -1;
    if (!rc_path || !payload) return -1;
    if (rc_read_file(rc_path, &existing, &existing_length) != 0) goto done;

    has_block = existing_length > 0 &&
                rc_locate_block(existing, existing_length, &block_start,
                                &block_end) == 0;

    if (has_block) {
        /* 拼接：文件头 + 新块 + 文件尾。若替换前后内容相同则跳过磁盘写。 */
        if (rc_buffer_append(&output, existing, block_start) != 0) goto done;
        if (rc_render_block(&output, payload) != 0) goto done;
        if (rc_buffer_append(&output, existing + block_end,
                             existing_length - block_end) != 0) {
            goto done;
        }
    } else {
        /* 追加到文件尾；若原文件不以换行结尾则补一个换行。 */
        if (existing_length > 0) {
            if (rc_buffer_append(&output, existing, existing_length) != 0) goto done;
            if (existing[existing_length - 1u] != '\n') {
                if (rc_buffer_append(&output, "\n", 1u) != 0) goto done;
            }
        }
        if (rc_render_block(&output, payload) != 0) goto done;
    }

    /* 幂等：内容一致则跳过写。 */
    if (existing_length == output.length && output.data &&
        memcmp(existing, output.data, existing_length) == 0) {
        result = 0;
        goto done;
    }
    if (rc_write_file_atomic(rc_path, output.data ? output.data : "",
                             output.length) != 0) {
        goto done;
    }
    result = 0;
done:
    free(existing);
    rc_buffer_free(&output);
    return result;
}

int jvman_rc_writer_remove(const char *rc_path) {
    char *existing = NULL;
    size_t existing_length = 0;
    RcBuffer output = {0};
    size_t block_start = 0;
    size_t block_end = 0;
    int result = -1;
    if (!rc_path) return -1;
    if (rc_read_file(rc_path, &existing, &existing_length) != 0) goto done;
    if (existing_length == 0) { result = 0; goto done; }
    if (rc_locate_block(existing, existing_length, &block_start,
                        &block_end) != 0) {
        /* 无块：视为已移除。 */
        result = 0;
        goto done;
    }
    if (rc_buffer_append(&output, existing, block_start) != 0) goto done;
    if (rc_buffer_append(&output, existing + block_end,
                         existing_length - block_end) != 0) {
        goto done;
    }
    if (rc_write_file_atomic(rc_path, output.data ? output.data : "",
                             output.length) != 0) {
        goto done;
    }
    result = 0;
done:
    free(existing);
    rc_buffer_free(&output);
    return result;
}

int jvman_rc_writer_probe(const char *rc_path, int *out_present) {
    char *existing = NULL;
    size_t existing_length = 0;
    size_t block_start = 0;
    size_t block_end = 0;
    if (!rc_path || !out_present) return -1;
    *out_present = 0;
    if (rc_read_file(rc_path, &existing, &existing_length) != 0) return -1;
    if (existing_length > 0 &&
        rc_locate_block(existing, existing_length, &block_start,
                        &block_end) == 0) {
        *out_present = 1;
    }
    free(existing);
    return 0;
}
