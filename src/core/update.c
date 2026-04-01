#include "fwu/update.h"

#include <stddef.h>
#include <string.h>

static fwu_status_t map_flash(flash_status_t st)
{
    if (st == FLASH_OK) {
        return FWU_OK;
    }
    return st == FLASH_ERR_POWER_LOSS ? FWU_ERR_POWER_LOSS : FWU_ERR_FLASH;
}

static fwu_status_t erase_region(const flash_dev_t *flash, uint32_t addr, uint32_t sectors)
{
    for (uint32_t i = 0; i < sectors; i++) {
        fwu_status_t st = map_flash(flash->erase_sector(flash->ctx, addr + i * FLASH_SECTOR_SIZE));
        if (st != FWU_OK) {
            return st;
        }
    }
    return FWU_OK;
}

static fwu_status_t commit(fwu_ctx_t *ctx, fwu_meta_t next)
{
    uint8_t sector = ctx->meta_sector;
    if (ctx->meta_valid) {
        next.seq = ctx->meta.seq;
    }
    fwu_status_t st = fwu_meta_commit(ctx->flash, &next, ctx->meta_valid, &sector);
    if (st == FWU_OK) {
        ctx->meta = next;
        ctx->meta_sector = sector;
        ctx->meta_valid = true;
    }
    return st;
}

static fwu_meta_t without_candidate(const fwu_meta_t *m, uint8_t event)
{
    fwu_meta_t next = *m;
    next.state = FWU_STATE_CONFIRMED;
    next.last_event = event;
    next.candidate_slot = FWU_SLOT_NONE;
    next.candidate_version = 0u;
    next.candidate_crc = 0u;
    return next;
}

static fwu_status_t verify_named(const fwu_ctx_t *ctx, uint8_t slot, uint32_t version,
                                 uint32_t crc)
{
    fwimg_header_t hdr;
    fwu_status_t st = fwimg_verify_slot(ctx->flash, slot, &hdr);
    if (st != FWU_OK) {
        return st;
    }
    if (hdr.fw_version != version || hdr.payload_crc != crc) {
        return FWU_ERR_IMG_MISMATCH;
    }
    return FWU_OK;
}

fwu_status_t fwu_open(fwu_ctx_t *ctx, const flash_dev_t *flash)
{
    if (ctx == NULL || flash == NULL) {
        return FWU_ERR_ARG;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->flash = flash;

    fwu_status_t st = fwu_meta_load(flash, &ctx->meta, &ctx->meta_sector);
    if (st == FWU_OK) {
        ctx->meta_valid = true;
    } else if (st == FWU_ERR_NO_METADATA) {
        st = FWU_OK;
    }
    return st;
}

static void install_reset(fwu_ctx_t *ctx, bool factory, uint8_t slot)
{
    memset(&ctx->install, 0, sizeof(ctx->install));
    ctx->install.active = true;
    ctx->install.factory = factory;
    ctx->install.slot = slot;
    ctx->install.error = FWU_OK;
}

fwu_status_t fwu_factory_begin(fwu_ctx_t *ctx)
{
    if (ctx == NULL || ctx->flash == NULL) {
        return FWU_ERR_ARG;
    }
    fwu_status_t st = erase_region(ctx->flash, FLASH_META0_ADDR, FLASH_META_SECTOR_COUNT);
    if (st != FWU_OK) {
        return st;
    }
    ctx->meta_valid = false;
    ctx->meta_sector = 0u;
    memset(&ctx->meta, 0, sizeof(ctx->meta));

    st = erase_region(ctx->flash, FLASH_SLOT_ADDR(0), FLASH_SLOT_SECTORS);
    if (st != FWU_OK) {
        return st;
    }
    install_reset(ctx, true, 0u);
    return FWU_OK;
}

fwu_status_t fwu_install_begin(fwu_ctx_t *ctx)
{
    if (ctx == NULL || ctx->flash == NULL) {
        return FWU_ERR_ARG;
    }
    if (!ctx->meta_valid) {
        return FWU_ERR_NO_METADATA;
    }
    if (ctx->meta.state == FWU_STATE_TRIAL) {
        return FWU_ERR_STATE;
    }
    if (ctx->meta.state == FWU_STATE_PENDING) {
        fwu_status_t st = commit(ctx, without_candidate(&ctx->meta, FWU_EVENT_NONE));
        if (st != FWU_OK) {
            return st;
        }
    }

    uint8_t slot = (uint8_t)(1u - ctx->meta.confirmed_slot);
    fwu_status_t st = erase_region(ctx->flash, FLASH_SLOT_ADDR(slot), FLASH_SLOT_SECTORS);
    if (st != FWU_OK) {
        return st;
    }
    install_reset(ctx, false, slot);
    return FWU_OK;
}

fwu_status_t fwu_install_write(fwu_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
    if (ctx == NULL || !ctx->install.active || (data == NULL && len != 0u)) {
        return FWU_ERR_ARG;
    }
    if (ctx->install.error != FWU_OK) {
        return ctx->install.error;
    }

    uint32_t base = FLASH_SLOT_ADDR(ctx->install.slot);
    while (len > 0u) {
        uint32_t limit = ctx->install.header_ok ? ctx->install.total : FWIMG_HEADER_SIZE;
        if (ctx->install.received >= limit) {
            ctx->install.error = FWU_ERR_IMG_TOO_LARGE;
            return ctx->install.error;
        }
        uint32_t n = limit - ctx->install.received;
        if (n > len) {
            n = len;
        }
        if (n > FLASH_PROGRAM_CHUNK) {
            n = FLASH_PROGRAM_CHUNK;
        }

        if (ctx->install.received < FWIMG_HEADER_SIZE) {
            memcpy(&ctx->install.header[ctx->install.received], data, n);
        }
        fwu_status_t st = map_flash(
            ctx->flash->program(ctx->flash->ctx, base + ctx->install.received, data, n));
        if (st != FWU_OK) {
            ctx->install.error = st;
            return st;
        }
        ctx->install.received += n;
        data += n;
        len -= n;

        if (!ctx->install.header_ok && ctx->install.received == FWIMG_HEADER_SIZE) {
            fwimg_header_t hdr;
            st = fwimg_header_decode(ctx->install.header, &hdr);
            if (st != FWU_OK) {
                ctx->install.error = st;
                return st;
            }
            ctx->install.header_ok = true;
            ctx->install.total = FWIMG_HEADER_SIZE + hdr.payload_len;
        }
    }
    return FWU_OK;
}

fwu_status_t fwu_install_finish(fwu_ctx_t *ctx)
{
    if (ctx == NULL || !ctx->install.active) {
        return FWU_ERR_ARG;
    }
    ctx->install.active = false;
    if (ctx->install.error != FWU_OK) {
        return ctx->install.error;
    }
    if (!ctx->install.header_ok || ctx->install.received < ctx->install.total) {
        return FWU_ERR_IMG_TRUNCATED;
    }

    fwimg_header_t hdr;
    fwu_status_t st = fwimg_verify_slot(ctx->flash, ctx->install.slot, &hdr);
    if (st != FWU_OK) {
        return st;
    }

    fwu_meta_t next;
    if (ctx->install.factory) {
        memset(&next, 0, sizeof(next));
        next.state = FWU_STATE_CONFIRMED;
        next.last_event = FWU_EVENT_NONE;
        next.confirmed_slot = ctx->install.slot;
        next.candidate_slot = FWU_SLOT_NONE;
        next.confirmed_version = hdr.fw_version;
        next.confirmed_crc = hdr.payload_crc;
    } else {
        next = ctx->meta;
        next.state = FWU_STATE_PENDING;
        next.last_event = FWU_EVENT_NONE;
        next.candidate_slot = ctx->install.slot;
        next.candidate_version = hdr.fw_version;
        next.candidate_crc = hdr.payload_crc;
    }
    return commit(ctx, next);
}

static fwu_status_t boot_confirmed(fwu_ctx_t *ctx, uint8_t event, fwu_boot_result_t *out)
{
    const fwu_meta_t *m = &ctx->meta;
    fwu_status_t st = verify_named(ctx, m->confirmed_slot, m->confirmed_version, m->confirmed_crc);
    if (st == FWU_ERR_POWER_LOSS || st == FWU_ERR_FLASH) {
        return st;
    }
    if (st != FWU_OK) {
        return FWU_ERR_NO_BOOTABLE;
    }
    out->slot = m->confirmed_slot;
    out->fw_version = m->confirmed_version;
    out->trial = false;
    out->event = event;
    return FWU_OK;
}

fwu_status_t fwu_boot(fwu_ctx_t *ctx, fwu_boot_result_t *out)
{
    if (ctx == NULL || out == NULL) {
        return FWU_ERR_ARG;
    }
    if (!ctx->meta_valid) {
        return FWU_ERR_NO_METADATA;
    }

    fwu_status_t st;
    switch (ctx->meta.state) {
    case FWU_STATE_CONFIRMED:
        return boot_confirmed(ctx, FWU_EVENT_NONE, out);

    case FWU_STATE_PENDING: {
        const fwu_meta_t *m = &ctx->meta;
        st = verify_named(ctx, m->candidate_slot, m->candidate_version, m->candidate_crc);
        if (st == FWU_ERR_POWER_LOSS || st == FWU_ERR_FLASH) {
            return st;
        }
        if (st != FWU_OK) {
            st = commit(ctx, without_candidate(m, FWU_EVENT_REJECTED));
            return st == FWU_OK ? boot_confirmed(ctx, FWU_EVENT_REJECTED, out) : st;
        }
        fwu_meta_t next = *m;
        next.state = FWU_STATE_TRIAL;
        next.last_event = FWU_EVENT_NONE;
        st = commit(ctx, next);
        if (st != FWU_OK) {
            return st;
        }
        out->slot = ctx->meta.candidate_slot;
        out->fw_version = ctx->meta.candidate_version;
        out->trial = true;
        out->event = FWU_EVENT_NONE;
        return FWU_OK;
    }

    case FWU_STATE_TRIAL:
        st = commit(ctx, without_candidate(&ctx->meta, FWU_EVENT_ROLLED_BACK));
        return st == FWU_OK ? boot_confirmed(ctx, FWU_EVENT_ROLLED_BACK, out) : st;

    default:
        return FWU_ERR_META_INVALID;
    }
}

fwu_status_t fwu_confirm(fwu_ctx_t *ctx)
{
    if (ctx == NULL) {
        return FWU_ERR_ARG;
    }
    if (!ctx->meta_valid) {
        return FWU_ERR_NO_METADATA;
    }
    if (ctx->meta.state != FWU_STATE_TRIAL) {
        return FWU_ERR_STATE;
    }
    const fwu_meta_t *m = &ctx->meta;
    fwu_status_t st = verify_named(ctx, m->candidate_slot, m->candidate_version, m->candidate_crc);
    if (st != FWU_OK) {
        return st;
    }

    fwu_meta_t next = without_candidate(m, FWU_EVENT_UPDATED);
    next.confirmed_slot = m->candidate_slot;
    next.confirmed_version = m->candidate_version;
    next.confirmed_crc = m->candidate_crc;
    return commit(ctx, next);
}

fwu_status_t fwu_report(fwu_ctx_t *ctx, fwu_report_t *out)
{
    if (ctx == NULL || out == NULL) {
        return FWU_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->meta_valid = ctx->meta_valid;
    out->meta_sector = ctx->meta_sector;
    out->meta = ctx->meta;
    for (uint8_t s = 0; s < FLASH_SLOT_COUNT; s++) {
        out->slot_status[s] = fwimg_verify_slot(ctx->flash, s, &out->slot_header[s]);
        if (out->slot_status[s] == FWU_ERR_POWER_LOSS || out->slot_status[s] == FWU_ERR_FLASH) {
            return out->slot_status[s];
        }
    }
    return FWU_OK;
}
