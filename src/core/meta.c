#include "fwu/meta.h"

#include <stddef.h>
#include <string.h>

#include "byteorder.h"
#include "fwu/crc32.h"
#include "fwu/flash_layout.h"

#define OFF_MAGIC          0u
#define OFF_LAYOUT         4u
#define OFF_STATE          6u
#define OFF_EVENT          7u
#define OFF_SEQ            8u
#define OFF_CONF_SLOT      12u
#define OFF_CAND_SLOT      13u
#define OFF_RESERVED       14u
#define OFF_CONF_VERSION   16u
#define OFF_CONF_CRC       20u
#define OFF_CAND_VERSION   24u
#define OFF_CAND_CRC       28u
#define OFF_RECORD_CRC     32u
#define OFF_COMMIT         36u

_Static_assert(OFF_COMMIT == FWU_META_BODY_SIZE, "commit marker follows body");
_Static_assert(OFF_COMMIT + 4u == FWU_META_RECORD_SIZE, "record size");

static fwu_status_t map_flash(flash_status_t st)
{
    if (st == FLASH_OK) {
        return FWU_OK;
    }
    return st == FLASH_ERR_POWER_LOSS ? FWU_ERR_POWER_LOSS : FWU_ERR_FLASH;
}

void fwu_meta_encode(const fwu_meta_t *m, uint8_t out[FWU_META_RECORD_SIZE])
{
    put_le32(&out[OFF_MAGIC], FWU_META_MAGIC);
    put_le16(&out[OFF_LAYOUT], FWU_META_LAYOUT_VERSION);
    out[OFF_STATE] = m->state;
    out[OFF_EVENT] = m->last_event;
    put_le32(&out[OFF_SEQ], m->seq);
    out[OFF_CONF_SLOT] = m->confirmed_slot;
    out[OFF_CAND_SLOT] = m->candidate_slot;
    put_le16(&out[OFF_RESERVED], 0u);
    put_le32(&out[OFF_CONF_VERSION], m->confirmed_version);
    put_le32(&out[OFF_CONF_CRC], m->confirmed_crc);
    put_le32(&out[OFF_CAND_VERSION], m->candidate_version);
    put_le32(&out[OFF_CAND_CRC], m->candidate_crc);
    put_le32(&out[OFF_RECORD_CRC], crc32_compute(out, OFF_RECORD_CRC));
    put_le32(&out[OFF_COMMIT], FWU_META_COMMIT);
}

static bool fields_valid(const fwu_meta_t *m)
{
    if (m->confirmed_slot >= FLASH_SLOT_COUNT || m->last_event > FWU_EVENT_REJECTED) {
        return false;
    }
    switch (m->state) {
    case FWU_STATE_CONFIRMED:
        return m->candidate_slot == FWU_SLOT_NONE;
    case FWU_STATE_PENDING:
    case FWU_STATE_TRIAL:
        return m->candidate_slot < FLASH_SLOT_COUNT && m->candidate_slot != m->confirmed_slot;
    default:
        return false;
    }
}

fwu_status_t fwu_meta_decode(const uint8_t in[FWU_META_RECORD_SIZE], fwu_meta_t *out)
{
    fwu_meta_t m;

    if (get_le32(&in[OFF_MAGIC]) != FWU_META_MAGIC ||
        get_le16(&in[OFF_LAYOUT]) != FWU_META_LAYOUT_VERSION ||
        get_le32(&in[OFF_COMMIT]) != FWU_META_COMMIT ||
        get_le32(&in[OFF_RECORD_CRC]) != crc32_compute(in, OFF_RECORD_CRC) ||
        get_le16(&in[OFF_RESERVED]) != 0u) {
        return FWU_ERR_META_INVALID;
    }

    m.state = in[OFF_STATE];
    m.last_event = in[OFF_EVENT];
    m.seq = get_le32(&in[OFF_SEQ]);
    m.confirmed_slot = in[OFF_CONF_SLOT];
    m.candidate_slot = in[OFF_CAND_SLOT];
    m.confirmed_version = get_le32(&in[OFF_CONF_VERSION]);
    m.confirmed_crc = get_le32(&in[OFF_CONF_CRC]);
    m.candidate_version = get_le32(&in[OFF_CAND_VERSION]);
    m.candidate_crc = get_le32(&in[OFF_CAND_CRC]);
    if (!fields_valid(&m)) {
        return FWU_ERR_META_INVALID;
    }
    *out = m;
    return FWU_OK;
}

static bool seq_newer(uint32_t a, uint32_t b)
{
    uint32_t diff = a - b;
    return diff != 0u && diff < 0x80000000u;
}

int fwu_meta_select(bool valid0, const fwu_meta_t *m0, bool valid1, const fwu_meta_t *m1)
{
    if (valid0 && valid1) {
        return seq_newer(m1->seq, m0->seq) ? 1 : 0;
    }
    if (valid0) {
        return 0;
    }
    return valid1 ? 1 : -1;
}

fwu_status_t fwu_meta_load(const flash_dev_t *flash, fwu_meta_t *out, uint8_t *out_sector)
{
    uint8_t raw[FWU_META_RECORD_SIZE];
    fwu_meta_t m[FLASH_META_SECTOR_COUNT];
    bool valid[FLASH_META_SECTOR_COUNT];

    for (uint8_t i = 0; i < FLASH_META_SECTOR_COUNT; i++) {
        fwu_status_t st = map_flash(flash->read(flash->ctx, FLASH_META_ADDR(i), raw, sizeof(raw)));
        if (st != FWU_OK) {
            return st;
        }
        valid[i] = fwu_meta_decode(raw, &m[i]) == FWU_OK;
    }

    int sel = fwu_meta_select(valid[0], &m[0], valid[1], &m[1]);
    if (sel < 0) {
        return FWU_ERR_NO_METADATA;
    }
    *out = m[sel];
    *out_sector = (uint8_t)sel;
    return FWU_OK;
}

fwu_status_t fwu_meta_commit(const flash_dev_t *flash, fwu_meta_t *m,
                             bool have_current, uint8_t *io_sector)
{
    uint8_t rec[FWU_META_RECORD_SIZE];
    uint8_t check[FWU_META_RECORD_SIZE];
    fwu_meta_t readback;

    if (!fields_valid(m)) {
        return FWU_ERR_ARG;
    }
    uint8_t target = have_current ? (uint8_t)(1u - *io_sector) : 0u;
    fwu_meta_t next = *m;
    next.seq = have_current ? m->seq + 1u : 1u;
    fwu_meta_encode(&next, rec);

    uint32_t addr = FLASH_META_ADDR(target);
    fwu_status_t st = map_flash(flash->erase_sector(flash->ctx, addr));
    if (st == FWU_OK) {
        st = map_flash(flash->program(flash->ctx, addr, rec, FWU_META_BODY_SIZE));
    }
    if (st == FWU_OK) {
        st = map_flash(flash->program(flash->ctx, addr + FWU_META_BODY_SIZE,
                                      &rec[FWU_META_BODY_SIZE], 4u));
    }
    if (st == FWU_OK) {
        st = map_flash(flash->read(flash->ctx, addr, check, sizeof(check)));
    }
    if (st == FWU_OK && (memcmp(check, rec, sizeof(rec)) != 0 ||
                         fwu_meta_decode(check, &readback) != FWU_OK)) {
        st = FWU_ERR_FLASH;
    }
    if (st != FWU_OK) {
        return st;
    }

    *m = next;
    *io_sector = target;
    return FWU_OK;
}
