#ifndef FWU_IMAGE_H
#define FWU_IMAGE_H

#include <stdint.h>

#include "fwu/flash_if.h"
#include "fwu/flash_layout.h"
#include "fwu/status.h"

#define FWIMG_MAGIC           0x4D495746u
#define FWIMG_FORMAT_VERSION  1u
#define FWIMG_HEADER_SIZE     24u
#define FWIMG_MAX_PAYLOAD     (FLASH_SLOT_SIZE - FWIMG_HEADER_SIZE)

typedef struct {
    uint32_t fw_version;
    uint32_t payload_len;
    uint32_t payload_crc;
} fwimg_header_t;

void fwimg_header_encode(const fwimg_header_t *hdr, uint8_t out[FWIMG_HEADER_SIZE]);

fwu_status_t fwimg_header_decode(const uint8_t in[FWIMG_HEADER_SIZE], fwimg_header_t *out);

fwu_status_t fwimg_check_buffer(const uint8_t *img, uint32_t len, fwimg_header_t *out);

fwu_status_t fwimg_verify_slot(const flash_dev_t *flash, uint8_t slot, fwimg_header_t *out);

#endif
