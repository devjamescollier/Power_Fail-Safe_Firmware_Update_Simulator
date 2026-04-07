#include <stdint.h>
#include <string.h>

#include "flash_sim.h"
#include "fwu/crc32.h"
#include "fwu/update.h"
#include "test_util.h"

static const char *g_path;
static flashsim_t g_sim;
static flash_dev_t g_dev;
static fwu_ctx_t g_ctx;

static uint8_t g_img[FWIMG_HEADER_SIZE + FWIMG_MAX_PAYLOAD + 1u];

static uint32_t build_image(uint32_t fw_version, uint32_t payload_len, uint8_t seed)
{
    uint8_t *payload = &g_img[FWIMG_HEADER_SIZE];
    for (uint32_t i = 0; i < payload_len; i++) {
        payload[i] = (uint8_t)(i * 13u + seed);
    }
    fwimg_header_t hdr = {fw_version, payload_len, crc32_compute(payload, payload_len)};
    fwimg_header_encode(&hdr, g_img);
    return FWIMG_HEADER_SIZE + payload_len;
}

static void reboot(void)
{
    flashsim_close(&g_sim);
    CHECK(flashsim_open(&g_sim, g_path) == FLASH_OK);
    flashsim_bind(&g_sim, &g_dev);
    CHECK(fwu_open(&g_ctx, &g_dev) == FWU_OK);
}

static void fresh_device(void)
{
    flashsim_close(&g_sim);
    CHECK(flashsim_create(g_path) == FLASH_OK);
    reboot();
}

static fwu_status_t stream(uint32_t len, uint32_t chunk)
{
    fwu_status_t st = FWU_OK;
    for (uint32_t off = 0; off < len && st == FWU_OK; off += chunk) {
        uint32_t n = (len - off < chunk) ? len - off : chunk;
        st = fwu_install_write(&g_ctx, &g_img[off], n);
    }
    fwu_status_t fin = fwu_install_finish(&g_ctx);
    return st != FWU_OK ? st : fin;
}

static fwu_status_t factory(uint32_t version)
{
    uint32_t len = build_image(version, 3000u, 1u);
    CHECK(fwu_factory_begin(&g_ctx) == FWU_OK);
    return stream(len, 777u);
}

static fwu_status_t install(uint32_t version, uint32_t size, uint8_t seed, uint32_t chunk)
{
    uint32_t len = build_image(version, size, seed);
    fwu_status_t st = fwu_install_begin(&g_ctx);
    return st != FWU_OK ? st : stream(len, chunk);
}

static fwu_boot_result_t boot_ok(void)
{
    fwu_boot_result_t b;
    memset(&b, 0xEE, sizeof(b));
    CHECK(fwu_boot(&g_ctx, &b) == FWU_OK);
    return b;
}

static uint32_t slot_crc(uint8_t slot)
{
    uint8_t buf[256];
    uint32_t c = CRC32_INIT;
    int read_ok = 1;
    for (uint32_t off = 0; off < FLASH_SLOT_SIZE; off += sizeof(buf)) {
        read_ok &= g_dev.read(g_dev.ctx, FLASH_SLOT_ADDR(slot) + off, buf, sizeof(buf)) == FLASH_OK;
        c = crc32_update(c, buf, sizeof(buf));
    }
    CHECK(read_ok);
    return crc32_final(c);
}

static void test_unprovisioned(void)
{
    fwu_boot_result_t b;
    fresh_device();
    CHECK(!g_ctx.meta_valid);
    CHECK(fwu_boot(&g_ctx, &b) == FWU_ERR_NO_METADATA);
    CHECK(fwu_install_begin(&g_ctx) == FWU_ERR_NO_METADATA);
    CHECK(fwu_confirm(&g_ctx) == FWU_ERR_NO_METADATA);
}

static void test_factory_init(void)
{
    fresh_device();
    CHECK(factory(1u) == FWU_OK);
    reboot();
    CHECK(g_ctx.meta_valid);
    CHECK(g_ctx.meta.state == FWU_STATE_CONFIRMED);
    CHECK_EQ_U32(g_ctx.meta.seq, 1u);
    fwu_boot_result_t b = boot_ok();
    CHECK(b.slot == 0 && b.fw_version == 1u && !b.trial);
    reboot();
    CHECK_EQ_U32(g_ctx.meta.seq, 1u);
    CHECK(fwu_confirm(&g_ctx) == FWU_ERR_STATE);
}

static void test_update_confirm_and_rollback(void)
{
    fresh_device();
    CHECK(factory(1u) == FWU_OK);
    uint32_t a_before = slot_crc(0);

    CHECK(install(2u, 5000u, 2u, 1u) == FWU_OK);
    CHECK_EQ_U32(slot_crc(0), a_before);
    reboot();
    CHECK(g_ctx.meta.state == FWU_STATE_PENDING);
    CHECK(g_ctx.meta.candidate_slot == 1);
    CHECK(fwu_confirm(&g_ctx) == FWU_ERR_STATE);

    fwu_boot_result_t b = boot_ok();
    CHECK(b.slot == 1 && b.fw_version == 2u && b.trial);
    reboot();
    CHECK(g_ctx.meta.state == FWU_STATE_TRIAL);
    CHECK(fwu_install_begin(&g_ctx) == FWU_ERR_STATE);

    CHECK(fwu_confirm(&g_ctx) == FWU_OK);
    reboot();
    CHECK(g_ctx.meta.state == FWU_STATE_CONFIRMED);
    CHECK(g_ctx.meta.confirmed_slot == 1);
    CHECK(g_ctx.meta.last_event == FWU_EVENT_UPDATED);
    b = boot_ok();
    CHECK(b.slot == 1 && b.fw_version == 2u && !b.trial);

    uint32_t b_before = slot_crc(1);
    CHECK(install(3u, 700u, 3u, 256u) == FWU_OK);
    CHECK_EQ_U32(slot_crc(1), b_before);
    reboot();
    CHECK(g_ctx.meta.candidate_slot == 0);
    b = boot_ok();
    CHECK(b.slot == 0 && b.fw_version == 3u && b.trial);
    reboot();
    b = boot_ok();
    CHECK(b.slot == 1 && b.fw_version == 2u && !b.trial && b.event == FWU_EVENT_ROLLED_BACK);
    reboot();
    CHECK(g_ctx.meta.state == FWU_STATE_CONFIRMED);
    CHECK(g_ctx.meta.last_event == FWU_EVENT_ROLLED_BACK);
    CHECK(g_ctx.meta.candidate_slot == FWU_SLOT_NONE);
    CHECK(fwu_confirm(&g_ctx) == FWU_ERR_STATE);
    b = boot_ok();
    CHECK(b.slot == 1 && b.event == FWU_EVENT_NONE);
}

static void expect_rejected(fwu_status_t got, fwu_status_t want)
{
    CHECK(got == want);
    if (got != want) {
        fprintf(stderr, "  got %s, want %s\n", fwu_status_str(got), fwu_status_str(want));
    }
    uint32_t seq = g_ctx.meta.seq;
    reboot();
    CHECK(g_ctx.meta.state == FWU_STATE_CONFIRMED);
    CHECK_EQ_U32(g_ctx.meta.seq, seq);
    fwu_boot_result_t b = boot_ok();
    CHECK(b.slot == 0 && b.fw_version == 1u && !b.trial);
}

static void test_install_rejections(void)
{
    fresh_device();
    CHECK(factory(1u) == FWU_OK);
    uint32_t len;

    len = build_image(2u, 4000u, 2u);
    g_img[FWIMG_HEADER_SIZE + 1234u] ^= 0x40u;
    CHECK(fwu_install_begin(&g_ctx) == FWU_OK);
    expect_rejected(stream(len, 1000u), FWU_ERR_IMG_PAYLOAD_CRC);

    len = build_image(2u, 4000u, 2u);
    CHECK(fwu_install_begin(&g_ctx) == FWU_OK);
    expect_rejected(stream(len - 1u, 1000u), FWU_ERR_IMG_TRUNCATED);

    CHECK(fwu_install_begin(&g_ctx) == FWU_OK);
    expect_rejected(stream(10u, 1000u), FWU_ERR_IMG_TRUNCATED);

    CHECK(fwu_install_begin(&g_ctx) == FWU_OK);
    expect_rejected(fwu_install_finish(&g_ctx), FWU_ERR_IMG_TRUNCATED);

    len = build_image(2u, 4000u, 2u);
    CHECK(fwu_install_begin(&g_ctx) == FWU_OK);
    expect_rejected(stream(len + 1u, 1000u), FWU_ERR_IMG_TOO_LARGE);

    fwimg_header_t big = {2u, FWIMG_MAX_PAYLOAD + 1u, 0u};
    fwimg_header_encode(&big, g_img);
    CHECK(fwu_install_begin(&g_ctx) == FWU_OK);
    expect_rejected(stream(FWIMG_HEADER_SIZE, 1000u), FWU_ERR_IMG_TOO_LARGE);

    len = build_image(2u, 4000u, 2u);
    g_img[9] ^= 0x01u;
    CHECK(fwu_install_begin(&g_ctx) == FWU_OK);
    expect_rejected(stream(len, 1000u), FWU_ERR_IMG_HEADER_CRC);

    CHECK(install(2u, FWIMG_MAX_PAYLOAD, 9u, 4096u) == FWU_OK);
    fwu_boot_result_t b = boot_ok();
    CHECK(b.slot == 1 && b.fw_version == 2u && b.trial);
}

static void test_install_while_pending_replaces_candidate(void)
{
    fresh_device();
    CHECK(factory(1u) == FWU_OK);
    CHECK(install(2u, 4000u, 2u, 512u) == FWU_OK);
    CHECK(install(3u, 2000u, 3u, 512u) == FWU_OK);
    reboot();
    CHECK(g_ctx.meta.state == FWU_STATE_PENDING);
    CHECK_EQ_U32(g_ctx.meta.candidate_version, 3u);
    fwu_boot_result_t b = boot_ok();
    CHECK(b.slot == 1 && b.fw_version == 3u && b.trial);
}

static void test_candidate_verified_at_boot(void)
{
    fwu_boot_result_t b;

    fresh_device();
    CHECK(factory(1u) == FWU_OK);
    CHECK(install(2u, 4000u, 2u, 512u) == FWU_OK);
    uint8_t zero = 0x00;
    CHECK(g_dev.program(g_dev.ctx, FLASH_SLOT_ADDR(1) + FWIMG_HEADER_SIZE + 100u, &zero, 1) ==
          FLASH_OK);
    reboot();
    b = boot_ok();
    CHECK(b.slot == 0 && b.fw_version == 1u && !b.trial && b.event == FWU_EVENT_REJECTED);
    reboot();
    CHECK(g_ctx.meta.state == FWU_STATE_CONFIRMED);
    CHECK(g_ctx.meta.last_event == FWU_EVENT_REJECTED);

    fresh_device();
    CHECK(factory(1u) == FWU_OK);
    CHECK(install(2u, 4000u, 2u, 512u) == FWU_OK);
    uint32_t len = build_image(2u, 4000u, 77u);
    for (uint32_t s = 0; s < FLASH_SLOT_SECTORS; s++) {
        CHECK(g_dev.erase_sector(g_dev.ctx, FLASH_SLOT_ADDR(1) + s * FLASH_SECTOR_SIZE) == FLASH_OK);
    }
    CHECK(g_dev.program(g_dev.ctx, FLASH_SLOT_ADDR(1), g_img, len) == FLASH_OK);
    CHECK(fwimg_verify_slot(&g_dev, 1, NULL) == FWU_OK);
    reboot();
    b = boot_ok();
    CHECK(b.slot == 0 && b.event == FWU_EVENT_REJECTED);

    fresh_device();
    CHECK(factory(1u) == FWU_OK);
    CHECK(install(2u, 4000u, 2u, 512u) == FWU_OK);
    b = boot_ok();
    CHECK(b.trial);
    CHECK(g_dev.program(g_dev.ctx, FLASH_SLOT_ADDR(1) + FWIMG_HEADER_SIZE + 100u, &zero, 1) ==
          FLASH_OK);
    CHECK(fwu_confirm(&g_ctx) == FWU_ERR_IMG_PAYLOAD_CRC);
    reboot();
    b = boot_ok();
    CHECK(b.slot == 0 && b.event == FWU_EVENT_ROLLED_BACK);
}

static void test_confirmed_image_corrupt(void)
{
    fwu_boot_result_t b;
    fresh_device();
    CHECK(factory(1u) == FWU_OK);
    uint8_t zero = 0x00;
    CHECK(g_dev.program(g_dev.ctx, FLASH_SLOT_ADDR(0) + FWIMG_HEADER_SIZE + 5u, &zero, 1) ==
          FLASH_OK);
    reboot();
    CHECK(fwu_boot(&g_ctx, &b) == FWU_ERR_NO_BOOTABLE);
}

int main(int argc, char **argv)
{
    g_path = (argc > 1) ? argv[1] : "test_update.img";
    memset(&g_sim, 0, sizeof(g_sim));
    test_unprovisioned();
    test_factory_init();
    test_update_confirm_and_rollback();
    test_install_rejections();
    test_install_while_pending_replaces_candidate();
    test_candidate_verified_at_boot();
    test_confirmed_image_corrupt();
    flashsim_close(&g_sim);
    remove(g_path);
    return test_summary("test_update");
}
