/* SPDX-License-Identifier: Apache-2.0 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <string.h>

#if defined(_WIN32)
#  include <io.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  define XQ_OPEN  _open
#  define XQ_READ  _read
#  define XQ_WRITE _write
#  define XQ_CLOSE _close
#  define XQ_LSEEK _lseeki64
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  define XQ_OPEN  open
#  define XQ_READ  read
#  define XQ_WRITE write
#  define XQ_CLOSE close
#  define XQ_LSEEK lseek
#endif

#include "xq_file.h"

#define XQ_IO_CHUNK (32u * 1024u * 1024u)

static int detect_seekable(int fd)
{

    return XQ_LSEEK(fd, 0, SEEK_CUR) != (off_t)-1;
}

xq_status xq_file_open_read(xq_file *f, const char *path)
{
    if (!f || !path) return XQ_ERR_PARAM;
    memset(f, 0, sizeof *f);

    if (strcmp(path, "-") == 0) {
        f->fd = 0;
        f->is_stdio = 1;
        f->seekable = detect_seekable(0);
        return XQ_OK;
    }

    int flags = O_RDONLY;
#ifdef O_BINARY
    flags |= O_BINARY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif

    int fd;
    do { fd = XQ_OPEN(path, flags); } while (fd < 0 && errno == EINTR);
    if (fd < 0) return XQ_ERR_IO;

    f->fd = fd;
    f->seekable = detect_seekable(fd);
    return XQ_OK;
}

xq_status xq_file_open_write(xq_file *f, const char *path)
{
    if (!f || !path) return XQ_ERR_PARAM;
    memset(f, 0, sizeof *f);

    if (strcmp(path, "-") == 0) {
        f->fd = 1;
        f->is_stdio = 1;
        f->seekable = detect_seekable(1);
        return XQ_OK;
    }

    int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_BINARY
    flags |= O_BINARY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif

    int fd;
    do { fd = XQ_OPEN(path, flags, 0666); } while (fd < 0 && errno == EINTR);
    if (fd < 0) return XQ_ERR_IO;

    f->fd = fd;
    f->seekable = detect_seekable(fd);
    return XQ_OK;
}

void xq_file_close(xq_file *f)
{
    if (!f) return;
    if (!f->is_stdio && f->fd >= 0) XQ_CLOSE(f->fd);
    f->fd = -1;
}

xq_status xq_file_read(xq_file *f, void *buf, size_t len, size_t *got)
{
    if (!f || (!buf && len) || !got) return XQ_ERR_PARAM;
    uint8_t *p = (uint8_t *)buf;
    size_t total = 0;

    while (total < len) {
        size_t want = len - total;
        if (want > XQ_IO_CHUNK) want = XQ_IO_CHUNK;

#if defined(_WIN32)
        int n = XQ_READ(f->fd, p + total, (unsigned)want);
#else
        ssize_t n = XQ_READ(f->fd, p + total, want);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            return XQ_ERR_IO;
        }
        if (n == 0) break;
        total += (size_t)n;
    }

    *got = total;
    return XQ_OK;
}

xq_status xq_file_write(xq_file *f, const void *buf, size_t len)
{
    if (!f || (!buf && len)) return XQ_ERR_PARAM;
    const uint8_t *p = (const uint8_t *)buf;
    size_t total = 0;

    while (total < len) {
        size_t want = len - total;
        if (want > XQ_IO_CHUNK) want = XQ_IO_CHUNK;

#if defined(_WIN32)
        int n = XQ_WRITE(f->fd, p + total, (unsigned)want);
#else
        ssize_t n = XQ_WRITE(f->fd, p + total, want);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            return XQ_ERR_IO;
        }
        if (n == 0) return XQ_ERR_IO;
        total += (size_t)n;
    }

    return XQ_OK;
}

xq_status xq_file_pread(xq_file *f, void *buf, size_t len, uint64_t off, size_t *got)
{
    if (!f || (!buf && len) || !got) return XQ_ERR_PARAM;
    if (!f->seekable) return XQ_ERR_IO;

    uint8_t *p = (uint8_t *)buf;
    size_t total = 0;

    while (total < len) {
        size_t want = len - total;
        if (want > XQ_IO_CHUNK) want = XQ_IO_CHUNK;

#if defined(_WIN32)

        if (XQ_LSEEK(f->fd, (off_t)(off + total), SEEK_SET) < 0) return XQ_ERR_IO;
        int n = XQ_READ(f->fd, p + total, (unsigned)want);
#else
        ssize_t n = pread(f->fd, p + total, want, (off_t)(off + total));
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            return XQ_ERR_IO;
        }
        if (n == 0) break;
        total += (size_t)n;
    }

    *got = total;
    return XQ_OK;
}

xq_status xq_file_size(xq_file *f, uint64_t *size)
{
    if (!f || !size) return XQ_ERR_PARAM;
    if (!f->seekable) return XQ_ERR_IO;

#if defined(_WIN32)
    struct _stat64 st;
    if (_fstat64(f->fd, &st) != 0) return XQ_ERR_IO;
#else
    struct stat st;
    if (fstat(f->fd, &st) != 0) return XQ_ERR_IO;
#endif
    if (st.st_size < 0) return XQ_ERR_IO;
    *size = (uint64_t)st.st_size;
    return XQ_OK;
}

#if !defined(_WIN32)
#  include <sys/mman.h>
#endif

#include <stdlib.h>

xq_status xq_file_map(xq_file *f, uint64_t off, size_t len, xq_map *m)
{
    if (!f || !m || len == 0) return XQ_ERR_PARAM;
    memset(m, 0, sizeof *m);

#if !defined(_WIN32)
    if (f->seekable) {
        long ps = sysconf(_SC_PAGESIZE);
        if (ps > 0) {
            uint64_t page = (uint64_t)ps;
            uint64_t aligned = off - (off % page);
            size_t   delta   = (size_t)(off - aligned);

            if (delta <= SIZE_MAX - len) {
                size_t maplen = delta + len;
                void *addr = mmap(NULL, maplen, PROT_READ, MAP_PRIVATE,
                                  f->fd, (off_t)aligned);
                if (addr != MAP_FAILED) {
                    m->addr = addr;
                    m->maplen = maplen;
                    m->base = (const uint8_t *)addr + delta;
                    m->len = len;
                    m->is_mapped = 1;
                    return XQ_OK;
                }
            }
        }
    }
#endif

    uint8_t *buf = malloc(len);
    if (!buf) return XQ_ERR_OOM;
    size_t got = 0;
    xq_status st = xq_file_pread(f, buf, len, off, &got);
    if (st != XQ_OK || got != len) { free(buf); return st == XQ_OK ? XQ_ERR_TRUNCATED : st; }

    m->addr = buf;
    m->maplen = len;
    m->base = buf;
    m->len = len;
    m->is_mapped = 0;
    return XQ_OK;
}

void xq_file_unmap(xq_map *m)
{
    if (!m || !m->addr) return;
#if !defined(_WIN32)
    if (m->is_mapped) { munmap(m->addr, m->maplen); m->addr = NULL; m->base = NULL; return; }
#endif
    free(m->addr);
    m->addr = NULL;
    m->base = NULL;
}
