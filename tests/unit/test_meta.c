#include <stdint.h>
#include <string.h>

#include "flash_sim.h"
#include "fwu/flash_layout.h"
#include "fwu/meta.h"
#include "test_util.h"

static const char *g_path;

static fwu_meta_t confirmed_rec(uint32_t seq, uint8_t slot)
{
    fwu_meta_t m = {
        .seq = seq,
        .state = FWU_STATE_CONFIRMED,
        .last_event = FWU_EVENT_NONE,
        .confirmed_slot = slot,
        .candidate_slot = FWU_SLOT_NONE,
        .confirmed_version = 0x11u,
        .confirmed_crc = 0xAABBCCDDu,
    };
    return m;
}

static fwu_meta_t pending_rec(uint32_t seq)
{
    fwu_meta_t m = confirmed_rec(seq, 0);
    m.state = FWU_STATE_PENDING;
    m.candidate_slot = 1;
    m.candidate_version = 0x22u;
    m.candidate_crc = 0x12345678u;
    return m;
}

static void test_roundtrip_and_layout(void)
{
    uint8_t raw[FWU_META_RECORD_SIZE];
    fwu_meta_t in = pending_rec(0x01020304u), out;
    in.last_event = FWU_EVENT_ROLLED_BACK;
    fwu_meta_encode(&in, raw);
    CHECK(fwu_meta_decode(raw, &out) == FWU_OK);
    CHECK(memcmp(&in, &out, sizeof(in)) == 0);

    CHECK(memcmp(&raw[0], "FWMD", 4) == 0);
    CHECK(raw[4] == 1 && raw[5] == 0);
    CHECK(raw[6] == FWU_STATE_PENDING && raw[7] == FWU_EVENT_ROLLED_BACK);
    CHECK(raw[8] == 0x04 && raw[11] == 0x01);
    CHECK(raw[12] == 0 && raw[13] == 1);
    CHECK(memcmp(&raw[36], "CMIT", 4) == 0);
}

static void test_field_validation(void)
{
    uint8_t raw[FWU_META_RECORD_SIZE];
    fwu_meta_t out, m;
    const uint8_t bad_states[] = {0x00, 0x04, 0xFF};
    for (size_t i = 0; i < sizeof(bad_states); i++) {
        m = confirmed_rec(1, 0);
        m.state = bad_states[i];
        fwu_meta_encode(&m, raw);
        CHECK(fwu_meta_decode(raw, &out) == FWU_ERR_META_INVALID);
    }
    m = confirmed_rec(1, 0);
    m.candidate_slot = 1;
    fwu_meta_encode(&m, raw);
    CHECK(fwu_meta_decode(raw, &out) == FWU_ERR_META_INVALID);

    m = pending_rec(1);
    m.candidate_slot = m.confirmed_slot;
    fwu_meta_encode(&m, raw);
    CHECK(fwu_meta_decode(raw, &out) == FWU_ERR_META_INVALID);

    m = pending_rec(1);
    m.candidate_slot = FWU_SLOT_NONE;
    fwu_meta_encode(&m, raw);
    CHECK(fwu_meta_decode(raw, &out) == FWU_ERR_META_INVALID);

    m = confirmed_rec(1, 2);
    fwu_meta_encode(&m, raw);
    CHECK(fwu_meta_decode(raw, &out) == FWU_ERR_META_INVALID);

    m = confirmed_rec(1, 0);
    m.last_event = 4;
    fwu_meta_encode(&m, raw);
    CHECK(fwu_meta_decode(raw, &out) == FWU_ERR_META_INVALID);
}

static void test_framing_validation(void)
{
    uint8_t raw[FWU_META_RECORD_SIZE], good[FWU_META_RECORD_SIZE];
    fwu_meta_t m = pending_rec(5), out;
    fwu_meta_encode(&m, good);

    memset(raw, 0xFF, sizeof(raw));
    CHECK(fwu_meta_decode(raw, &out) == FWU_ERR_META_INVALID);
    memset(raw, 0x00, sizeof(raw));
    CHECK(fwu_meta_decode(raw, &out) == FWU_ERR_META_INVALID);

    for (uint32_t k = 0; k < 4; k++) {
        memcpy(raw, good, sizeof(raw));
        memset(&raw[FWU_META_BODY_SIZE + k], 0xFF, 4u - k);
        CHECK(fwu_meta_decode(raw, &out) == FWU_ERR_META_INVALID);
    }

    int rejected = 0;
    for (uint32_t i = 0; i < FWU_META_BODY_SIZE; i++) {
        for (int bit = 0; bit < 8; bit++) {
            memcpy(raw, good, sizeof(raw));
            raw[i] ^= (uint8_t)(1u << bit);
            rejected += fwu_meta_decode(raw, &out) == FWU_ERR_META_INVALID;
        }
    }
    CHECK_EQ_U32(rejected, FWU_META_BODY_SIZE * 8u);
}

static void test_select(void)
{
    fwu_meta_t a = confirmed_rec(5, 0), b = confirmed_rec(6, 0);
    CHECK(fwu_meta_select(false, &a, false, &b) == -1);
    CHECK(fwu_meta_select(true, &a, false, &b) == 0);
    CHECK(fwu_meta_select(false, &a, true, &b) == 1);
    CHECK(fwu_meta_select(true, &a, true, &b) == 1);
    CHECK(fwu_meta_select(true, &b, true, &a) == 0);
    CHECK(fwu_meta_select(true, &a, true, &a) == 0);

    a.seq = 0xFFFFFFFFu;
    b.seq = 0u;
    CHECK(fwu_meta_select(true, &a, true, &b) == 1);
    CHECK(fwu_meta_select(true, &b, true, &a) == 0);
}

static void open_dev(flashsim_t *sim, flash_dev_t *dev)
{
    CHECK(flashsim_open(sim, g_path) == FLASH_OK);
    flashsim_bind(sim, dev);
}

static void test_commit_alternates_and_persists(void)
{
    flashsim_t sim;
    flash_dev_t dev;
    fwu_meta_t m, loaded;
    uint8_t sector = 0, ls = 0;
    CHECK(flashsim_create(g_path) == FLASH_OK);
    open_dev(&sim, &dev);

    CHECK(fwu_meta_load(&dev, &loaded, &ls) == FWU_ERR_NO_METADATA);

    m = confirmed_rec(999, 0);
    CHECK(fwu_meta_commit(&dev, &m, false, &sector) == FWU_OK);
    CHECK_EQ_U32(m.seq, 1u);
    CHECK_EQ_U32(sector, 0u);

    for (uint32_t i = 2; i <= 5; i++) {
        m.confirmed_version = i;
        CHECK(fwu_meta_commit(&dev, &m, true, &sector) == FWU_OK);
        CHECK_EQ_U32(m.seq, i);
        CHECK_EQ_U32(sector, (i - 1u) % 2u);
        CHECK(fwu_meta_load(&dev, &loaded, &ls) == FWU_OK);
        CHECK_EQ_U32(loaded.seq, i);
        CHECK_EQ_U32(loaded.confirmed_version, i);
        CHECK_EQ_U32(ls, sector);
    }

    fwu_meta_t bad = m;
    bad.state = 0;
    CHECK(fwu_meta_commit(&dev, &bad, true, &sector) == FWU_ERR_ARG);
    flashsim_close(&sim);

    open_dev(&sim, &dev);
    CHECK(fwu_meta_load(&dev, &loaded, &ls) == FWU_OK);
    CHECK_EQ_U32(loaded.seq, 5u);
    flashsim_close(&sim);
}

static void test_interrupted_commit_states(void)
{
    flashsim_t sim;
    flash_dev_t dev;
    fwu_meta_t m, loaded;
    uint8_t sector = 0, ls = 0;
    uint8_t next_raw[FWU_META_RECORD_SIZE];

    CHECK(flashsim_create(g_path) == FLASH_OK);
    open_dev(&sim, &dev);
    m = confirmed_rec(0, 0);
    CHECK(fwu_meta_commit(&dev, &m, false, &sector) == FWU_OK);
    CHECK(fwu_meta_commit(&dev, &m, true, &sector) == FWU_OK);
    CHECK(fwu_meta_commit(&dev, &m, true, &sector) == FWU_OK);
    uint32_t target = FLASH_META1_ADDR;

    fwu_meta_t next = pending_rec(4);
    fwu_meta_encode(&next, next_raw);
    int ok = 0, cases = 0;

    cases++;
    ok += fwu_meta_load(&dev, &loaded, &ls) == FWU_OK && loaded.seq == 3u && ls == 0;

    CHECK(dev.erase_sector(dev.ctx, target) == FLASH_OK);
    cases++;
    ok += fwu_meta_load(&dev, &loaded, &ls) == FWU_OK && loaded.seq == 3u && ls == 0;

    for (uint32_t k = 0; k <= FWU_META_BODY_SIZE; k++) {
        CHECK(dev.erase_sector(dev.ctx, target) == FLASH_OK);
        CHECK(dev.program(dev.ctx, target, next_raw, k) == FLASH_OK);
        cases++;
        ok += fwu_meta_load(&dev, &loaded, &ls) == FWU_OK && loaded.seq == 3u && ls == 0;
    }

    for (uint32_t k = 0; k < 4; k++) {
        CHECK(dev.erase_sector(dev.ctx, target) == FLASH_OK);
        CHECK(dev.program(dev.ctx, target, next_raw, FWU_META_BODY_SIZE + k) == FLASH_OK);
        cases++;
        ok += fwu_meta_load(&dev, &loaded, &ls) == FWU_OK && loaded.seq == 3u && ls == 0;
    }
    CHECK_EQ_U32(ok, cases);

    CHECK(dev.program(dev.ctx, target + FWU_META_BODY_SIZE, &next_raw[FWU_META_BODY_SIZE], 4) ==
          FLASH_OK);
    CHECK(fwu_meta_load(&dev, &loaded, &ls) == FWU_OK);
    CHECK_EQ_U32(loaded.seq, 4u);
    CHECK_EQ_U32(ls, 1u);
    CHECK(loaded.state == FWU_STATE_PENDING);

    CHECK(dev.erase_sector(dev.ctx, FLASH_META0_ADDR) == FLASH_OK);
    m = confirmed_rec(2, 0);
    uint8_t old_raw[FWU_META_RECORD_SIZE];
    fwu_meta_encode(&m, old_raw);
    CHECK(dev.program(dev.ctx, FLASH_META0_ADDR, old_raw, sizeof(old_raw)) == FLASH_OK);
    CHECK(fwu_meta_load(&dev, &loaded, &ls) == FWU_OK);
    CHECK_EQ_U32(loaded.seq, 4u);
    flashsim_close(&sim);
}

int main(int argc, char **argv)
{
    g_path = (argc > 1) ? argv[1] : "test_meta.img";
    test_roundtrip_and_layout();
    test_field_validation();
    test_framing_validation();
    test_select();
    test_commit_alternates_and_persists();
    test_interrupted_commit_states();
    remove(g_path);
    return test_summary("test_meta");
}
