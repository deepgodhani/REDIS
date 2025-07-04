#pragma once
#include <stdint.h>
#include <stdlib.h>

struct Buffer {
    uint8_t *buffer_begin = nullptr;
    uint8_t *buffer_end = nullptr;
    uint8_t *data_begin = nullptr;
    uint8_t *data_end = nullptr;
};

void buf_init(Buffer *buf, size_t capacity);
void buf_free(Buffer *buf);
void buf_compact(Buffer *buf);
void buf_append(Buffer *buf, const uint8_t *data, size_t len);
void buf_consume(Buffer *buf, size_t n);
size_t buf_size(const Buffer *buf);
