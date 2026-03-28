#ifndef FWU_CRC32_H
#define FWU_CRC32_H

#include <stddef.h>
#include <stdint.h>

#define CRC32_INIT 0xFFFFFFFFu

uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len);
uint32_t crc32_final(uint32_t crc);

uint32_t crc32_compute(const uint8_t *data, size_t len);

#endif
