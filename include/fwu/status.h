#ifndef FWU_STATUS_H
#define FWU_STATUS_H

typedef enum {
    FWU_OK = 0,
    FWU_ERR_ARG,
    FWU_ERR_STATE,
    FWU_ERR_NO_METADATA,
    FWU_ERR_FLASH,
    FWU_ERR_POWER_LOSS,
    FWU_ERR_IMG_MAGIC,
    FWU_ERR_IMG_FORMAT,
    FWU_ERR_IMG_HEADER_CRC,
    FWU_ERR_IMG_TOO_LARGE,
    FWU_ERR_IMG_TRUNCATED,
    FWU_ERR_IMG_PAYLOAD_CRC,
    FWU_ERR_IMG_MISMATCH,
    FWU_ERR_META_INVALID,
    FWU_ERR_NO_BOOTABLE
} fwu_status_t;

const char *fwu_status_str(fwu_status_t st);

#endif
