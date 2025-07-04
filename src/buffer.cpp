#include "buffer.h"
#include <string.h>
#include <assert.h>

void buf_init(Buffer *buf, size_t capacity) {
    buf->buffer_begin = (uint8_t *)malloc(capacity);
    assert(buf->buffer_begin);
    buf->buffer_end = buf->buffer_begin + capacity;
    buf->data_begin = buf->data_end = buf->buffer_begin;
}

void buf_free(Buffer *buf) {
    free(buf->buffer_begin);
    buf->buffer_begin = buf->buffer_end = buf->data_begin = buf->data_end = nullptr;
}

void buf_compact(Buffer *buf) {
    size_t size = buf->data_end - buf->data_begin;
    memmove(buf->buffer_begin, buf->data_begin, size);
    buf->data_begin = buf->buffer_begin;
    buf->data_end = buf->buffer_begin + size;
}

size_t buf_size(const Buffer *buf) {
    return buf->data_end - buf->data_begin;
}

void buf_append(Buffer *buf, const uint8_t *data, size_t len) {
    size_t space_left = buf->buffer_end - buf->data_end;
    if (space_left < len) {
        buf_compact(buf);
        space_left = buf->buffer_end - buf->data_end;
        if (space_left < len) {
            size_t current = buf_size(buf);
            size_t new_cap = (buf->buffer_end - buf->buffer_begin) * 2 + len;
            uint8_t *new_mem = (uint8_t *)malloc(new_cap);
            assert(new_mem);
            memcpy(new_mem, buf->data_begin, current);
            free(buf->buffer_begin);

            buf->buffer_begin = new_mem;
            buf->buffer_end = new_mem + new_cap;
            buf->data_begin = new_mem;
            buf->data_end = new_mem + current;
        }
    }

    memcpy(buf->data_end, data, len);
    buf->data_end += len;
}   


void buf_consume(Buffer *buf, size_t n) {
    assert(n <= buf_size(buf));
    buf->data_begin += n;
}