/* crc32.h - CRC32 checksum for state file hardening
 *
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>
#include <stddef.h>

/**
 * Calculate CRC32 checksum
 * 
 * @param data   Pointer to data buffer
 * @param length Size of data in bytes
 * @return       CRC32 checksum value
 */
uint32_t kp_crc32(const void *data, size_t length);

/**
 * Update running CRC32 checksum with additional data
 * 
 * @param crc    Previous CRC value (use 0 for initial call)
 * @param data   Pointer to data buffer
 * @param length Size of data in bytes
 * @return       Updated CRC32 checksum
 */
uint32_t kp_crc32_update(uint32_t crc, const void *data, size_t length);

#endif /* CRC32_H */
