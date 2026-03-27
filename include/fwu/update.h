#ifndef FWU_UPDATE_H
#define FWU_UPDATE_H

#include <stdbool.h>
#include <stdint.h>

#include "fwu/flash_if.h"
#include "fwu/image.h"
#include "fwu/meta.h"
#include "fwu/status.h"

typedef struct {
    const flash_dev_t *flash;

    bool       meta_valid;
    uint8_t    meta_sector;
    fwu_meta_t meta;

    struct {
        bool     active;
        bool     factory;
        uint8_t  slot;
        uint32_t received;
        bool     header_ok;
        uint32_t total;
        uint8_t  header[FWIMG_HEADER_SIZE];
        fwu_status_t error;
    } install;
} fwu_ctx_t;

typedef struct {
    uint8_t  slot;
    uint32_t fw_version;
    bool     trial;
    uint8_t  event;
} fwu_boot_result_t;

typedef struct {
    bool         meta_valid;
    uint8_t      meta_sector;
    fwu_meta_t   meta;
    fwu_status_t slot_status[FLASH_SLOT_COUNT];
    fwimg_header_t slot_header[FLASH_SLOT_COUNT];
} fwu_report_t;

fwu_status_t fwu_open(fwu_ctx_t *ctx, const flash_dev_t *flash);

fwu_status_t fwu_factory_begin(fwu_ctx_t *ctx);

fwu_status_t fwu_install_begin(fwu_ctx_t *ctx);

fwu_status_t fwu_install_write(fwu_ctx_t *ctx, const uint8_t *data, uint32_t len);

fwu_status_t fwu_install_finish(fwu_ctx_t *ctx);

fwu_status_t fwu_boot(fwu_ctx_t *ctx, fwu_boot_result_t *out);

fwu_status_t fwu_confirm(fwu_ctx_t *ctx);

fwu_status_t fwu_report(fwu_ctx_t *ctx, fwu_report_t *out);

#endif
