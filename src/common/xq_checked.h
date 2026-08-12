/* SPDX-License-Identifier: Apache-2.0 */
#ifndef XQ_CHECKED_H
#define XQ_CHECKED_H

#include <stdint.h>
#include <stddef.h>

#if defined(__clang__) || defined(__GNUC__)
#  define XQ_WRAPS __attribute__((no_sanitize("unsigned-integer-overflow")))
#else
#  define XQ_WRAPS
#endif

#if defined(__has_builtin)
#  if __has_builtin(__builtin_add_overflow)
#    define XQ_HAVE_OVERFLOW_BUILTINS 1
#  endif
#elif defined(__GNUC__) && __GNUC__ >= 5
#  define XQ_HAVE_OVERFLOW_BUILTINS 1
#endif

static inline int xq_add_u64_ok(uint64_t a, uint64_t b, uint64_t *out)
{
#ifdef XQ_HAVE_OVERFLOW_BUILTINS
    return !__builtin_add_overflow(a, b, out);
#else
    if (a > UINT64_MAX - b) return 0;
    *out = a + b;
    return 1;
#endif
}

static inline int xq_mul_u64_ok(uint64_t a, uint64_t b, uint64_t *out)
{
#ifdef XQ_HAVE_OVERFLOW_BUILTINS
    return !__builtin_mul_overflow(a, b, out);
#else
    if (a != 0 && b > UINT64_MAX / a) return 0;
    *out = a * b;
    return 1;
#endif
}

static inline int xq_range_ok(uint64_t off, uint64_t len, uint64_t limit)
{
    uint64_t end;
    if (!xq_add_u64_ok(off, len, &end)) return 0;
    return end <= limit;
}

static inline int xq_range_ok_sz(size_t off, size_t len, size_t limit)
{
    if (off > limit) return 0;
    return len <= limit - off;
}

#endif
