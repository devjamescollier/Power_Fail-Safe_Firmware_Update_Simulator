#ifndef FWU_META_H
#define FWU_META_H

#include <stdbool.h>
#include <stdint.h>

#include "fwu/flash_if.h"
#include "fwu/status.h"

#define FWU_META_MAGIC          0x444D5746u
#define FWU_META_LAYOUT_VERSION 1u
#define FWU_META_COMMIT         0x54494D43u
#define FWU_META_BODY_SIZE      36u
#define FWU_META_RECORD_SIZE    40u
#define FWU_SLOT_NONE           0xFFu

typedef enum {
    FWU_STATE_CONFIRMED = 0x01,
    FWU_STATE_PENDING   = 0x02,
    FWU_STATE_TRIAL     = 0x03
} fwu_state_t;

typedef enum {
    FWU_EVENT_NONE        = 0x00,
    FWU_EVENT_UPDATED     = 0x01,
    FWU_EVENT_ROLLED_BACK = 0x02,
    FWU_EVENT_REJECTED    = 0x03
} fwu_event_t;

typedef struct {
    uint32_t seq;
    uint8_t  state;
    uint8_t  last_event;
    uint8_t  confirmed_slot;
    uint8_t  candidate_slot;
    uint32_t confirmed_version;
    uint32_t confirmed_crc;
    uint32_t candidate_version;
    uint32_t candidate_crc;
} fwu_meta_t;

void fwu_meta_encode(const fwu_meta_t *m, uint8_t out[FWU_META_RECORD_SIZE]);

fwu_status_t fwu_meta_decode(const uint8_t in[FWU_META_RECORD_SIZE], fwu_meta_t *out);

int fwu_meta_select(bool valid0, const fwu_meta_t *m0, bool valid1, const fwu_meta_t *m1);

fwu_status_t fwu_meta_load(const flash_dev_t *flash, fwu_meta_t *out, uint8_t *out_sector);

fwu_status_t fwu_meta_commit(const flash_dev_t *flash, fwu_meta_t *m,
                             bool have_current, uint8_t *io_sector);

const char *fwu_state_str(uint8_t state);
const char *fwu_event_str(uint8_t event);

#endif
