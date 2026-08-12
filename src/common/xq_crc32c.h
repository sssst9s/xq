/* SPDX-License-Identifier: Apache-2.0 */
#ifndef XQ_CRC32C_H
#define XQ_CRC32C_H

#include <stddef.h>
#include <stdint.h>

uint32_t xq_crc32c(const void *data, size_t len);

uint32_t xq_crc32c_update(uint32_t crc, const void *data, size_t len);

int xq_crc32c_is_accelerated(void);

uint32_t xq_crc32c_portable(uint32_t crc, const void *data, size_t len);

#endif
