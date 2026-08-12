/* SPDX-License-Identifier: Apache-2.0 */
#ifndef XQ_FILE_H
#define XQ_FILE_H

#include <stddef.h>
#include <stdint.h>
#include "xq.h"

typedef struct {
    int      fd;
    int      is_stdio;
    int      seekable;
} xq_file;

xq_status xq_file_open_read(xq_file *f, const char *path);

xq_status xq_file_open_write(xq_file *f, const char *path);

void xq_file_close(xq_file *f);

xq_status xq_file_read(xq_file *f, void *buf, size_t len, size_t *got);

xq_status xq_file_write(xq_file *f, const void *buf, size_t len);

xq_status xq_file_pread(xq_file *f, void *buf, size_t len, uint64_t off, size_t *got);

xq_status xq_file_size(xq_file *f, uint64_t *size);

typedef struct {
    void  *addr;
    size_t maplen;
    const uint8_t *base;
    size_t len;
    int    is_mapped;
} xq_map;

xq_status xq_file_map(xq_file *f, uint64_t off, size_t len, xq_map *m);
void      xq_file_unmap(xq_map *m);

#endif
