#ifndef FWU_FLASH_IF_H
#define FWU_FLASH_IF_H

#include <stdint.h>

typedef enum {
    FLASH_OK = 0,
    FLASH_ERR_RANGE,
    FLASH_ERR_ALIGN,
    FLASH_ERR_PROGRAM_RULE,
    FLASH_ERR_IO,
    FLASH_ERR_POWER_LOSS
} flash_status_t;

typedef struct flash_dev {
    void *ctx;
    flash_status_t (*read)(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len);
    flash_status_t (*program)(void *ctx, uint32_t addr, const uint8_t *buf, uint32_t len);
    flash_status_t (*erase_sector)(void *ctx, uint32_t sector_addr);
} flash_dev_t;

#endif
