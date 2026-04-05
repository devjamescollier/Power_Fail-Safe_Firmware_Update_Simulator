#include <stdint.h>
#include <string.h>

#include "flash_sim.h"
#include "fwu/crc32.h"
#include "fwu/image.h"
#include "test_util.h"

static const uint8_t k_golden[40] = {
    0x46, 0x57, 0x49, 0x4d, 0x01, 0x00, 0x18, 0x00, 0x03, 0x00, 0x02, 0x01,
    0x10, 0x00, 0x00, 0x00, 0x24, 0x13, 0x36, 0xb7, 0x38, 0x52, 0x0d, 0xa9,
    0xbc, 0x82, 0x87, 0xea, 0x9b, 0xd3, 0xf0, 0x04, 0x5a, 0xb8, 0x5f, 0x3d,
    0x01, 0x5c, 0xcf, 0x4b,
};

static uint8_t g_img[FWIMG_HEADER_SIZE + FWIMG_MAX_PAYLOAD + 1u];
static const char *g_path;

static uint32_t build_image(uint32_t fw_version, uint32_t payload_len)
{
    uint8_t *payload = &g_img[FWIMG_HEADER_SIZE];
    for (uint32_t i = 0; i < payload_len; i++) {
        payload[i] = (uint8_t)(i * 31u + 7u);
    }
    fwimg_header_t hdr = {
        .fw_version = fw_version,
        .payload_len = payload_len,
        .payload_crc = crc32_compute(payload, payload_len),
    };
    fwimg_header_encode(&hdr, g_img);
    return FWIMG_HEADER_SIZE + payload_len;
}

static void reseal_header(uint8_t *h)
{
    uint32_t c = crc32_compute(h, 20);
    h[20] = (uint8_t)c;
    h[21] = (uint8_t)(c >> 8);
    h[22] = (uint8_t)(c >> 16);
    h[23] = (uint8_t)(c >> 24);
}

static void test_golden_matches_python(void)
{
    fwimg_header_t hdr;
    CHECK(fwimg_check_buffer(k_golden, sizeof(k_golden), &hdr) == FWU_OK);
    CHECK_EQ_U32(hdr.fw_version, 0x01020003u);
    CHECK_EQ_U32(hdr.payload_len, 16u);
    CHECK_EQ_U32(hdr.payload_crc, 0xB7361324u);

    uint8_t enc[FWIMG_HEADER_SIZE];
    fwimg_header_encode(&hdr, enc);
    CHECK(memcmp(enc, k_golden, FWIMG_HEADER_SIZE) == 0);
}

static void test_valid_image_roundtrip(void)
{
    fwimg_header_t hdr;
    uint32_t len = build_image(7u, 1000u);
    CHECK(fwimg_check_buffer(g_img, len, &hdr) == FWU_OK);
    CHECK_EQ_U32(hdr.fw_version, 7u);
    CHECK_EQ_U32(hdr.payload_len, 1000u);

    CHECK(memcmp(g_img, "FWIM", 4) == 0);
    CHECK(g_img[4] == 1 && g_img[5] == 0);
    CHECK(g_img[6] == 24 && g_img[7] == 0);
    CHECK(g_img[12] == 0xE8 && g_img[13] == 0x03 && g_img[14] == 0 && g_img[15] == 0);

    CHECK(fwimg_check_buffer(g_img, build_image(1u, 1u), NULL) == FWU_OK);
    CHECK(fwimg_check_buffer(g_img, build_image(1u, FWIMG_MAX_PAYLOAD), NULL) == FWU_OK);
}

static void test_changed_payload_bytes(void)
{
    uint32_t len = build_image(3u, 64u);
    int detected = 0;
    for (uint32_t i = FWIMG_HEADER_SIZE; i < len; i++) {
        for (int bit = 0; bit < 8; bit++) {
            g_img[i] ^= (uint8_t)(1u << bit);
            detected += fwimg_check_buffer(g_img, len, NULL) == FWU_ERR_IMG_PAYLOAD_CRC;
            g_img[i] ^= (uint8_t)(1u << bit);
        }
    }
    CHECK_EQ_U32(detected, 64u * 8u);
    CHECK(fwimg_check_buffer(g_img, len, NULL) == FWU_OK);
}

static void test_changed_header_bytes(void)
{
    uint32_t len = build_image(3u, 64u);
    int rejected = 0;
    for (uint32_t i = 0; i < FWIMG_HEADER_SIZE; i++) {
        for (int bit = 0; bit < 8; bit++) {
            g_img[i] ^= (uint8_t)(1u << bit);
            rejected += fwimg_check_buffer(g_img, len, NULL) != FWU_OK;
            g_img[i] ^= (uint8_t)(1u << bit);
        }
    }
    CHECK_EQ_U32(rejected, FWIMG_HEADER_SIZE * 8u);

    g_img[0] ^= 0x01;
    CHECK(fwimg_check_buffer(g_img, len, NULL) == FWU_ERR_IMG_MAGIC);
    g_img[0] ^= 0x01;
    g_img[8] ^= 0x01;
    CHECK(fwimg_check_buffer(g_img, len, NULL) == FWU_ERR_IMG_HEADER_CRC);
    g_img[8] ^= 0x01;

    g_img[4] = 2;
    reseal_header(g_img);
    CHECK(fwimg_check_buffer(g_img, len, NULL) == FWU_ERR_IMG_FORMAT);
    build_image(3u, 64u);
    g_img[6] = 32;
    reseal_header(g_img);
    CHECK(fwimg_check_buffer(g_img, len, NULL) == FWU_ERR_IMG_FORMAT);
}

static void test_truncated_images(void)
{
    uint32_t len = build_image(5u, 100u);
    int truncated = 0;
    for (uint32_t n = 0; n < len; n++) {
        truncated += fwimg_check_buffer(g_img, n, NULL) == FWU_ERR_IMG_TRUNCATED;
    }
    CHECK_EQ_U32(truncated, len);
}

static void test_oversized_images(void)
{
    uint32_t len = build_image(5u, 100u);
    CHECK(fwimg_check_buffer(g_img, len + 1u, NULL) == FWU_ERR_IMG_TOO_LARGE);

    fwimg_header_t hdr = {.fw_version = 1u, .payload_len = FWIMG_MAX_PAYLOAD + 1u, .payload_crc = 0u};
    fwimg_header_t out;
    fwimg_header_encode(&hdr, g_img);
    CHECK(fwimg_header_decode(g_img, &out) == FWU_ERR_IMG_TOO_LARGE);
    CHECK(fwimg_check_buffer(g_img, sizeof(g_img), NULL) == FWU_ERR_IMG_TOO_LARGE);
    hdr.payload_len = UINT32_MAX;
    fwimg_header_encode(&hdr, g_img);
    CHECK(fwimg_header_decode(g_img, &out) == FWU_ERR_IMG_TOO_LARGE);

    hdr.payload_len = 0u;
    fwimg_header_encode(&hdr, g_img);
    CHECK(fwimg_header_decode(g_img, &out) == FWU_ERR_IMG_FORMAT);
}

static flash_status_t write_slot(const flash_dev_t *dev, uint8_t slot, const uint8_t *data, uint32_t len)
{
    for (uint32_t s = 0; s < FLASH_SLOT_SECTORS; s++) {
        flash_status_t st = dev->erase_sector(dev->ctx, FLASH_SLOT_ADDR(slot) + s * FLASH_SECTOR_SIZE);
        if (st != FLASH_OK) {
            return st;
        }
    }
    return dev->program(dev->ctx, FLASH_SLOT_ADDR(slot), data, len);
}

static void test_verify_slot(void)
{
    flashsim_t sim;
    flash_dev_t dev;
    fwimg_header_t hdr;
    CHECK(flashsim_create(g_path) == FLASH_OK);
    CHECK(flashsim_open(&sim, g_path) == FLASH_OK);
    flashsim_bind(&sim, &dev);

    CHECK(fwimg_verify_slot(&dev, 1, NULL) == FWU_ERR_IMG_MAGIC);
    CHECK(fwimg_verify_slot(&dev, 2, NULL) == FWU_ERR_ARG);

    uint32_t len = build_image(9u, 5001u);
    CHECK(write_slot(&dev, 1, g_img, len) == FLASH_OK);
    CHECK(fwimg_verify_slot(&dev, 1, &hdr) == FWU_OK);
    CHECK_EQ_U32(hdr.fw_version, 9u);

    uint8_t b = (uint8_t)(g_img[FWIMG_HEADER_SIZE + 4000u] & 0xFEu);
    if (b == g_img[FWIMG_HEADER_SIZE + 4000u]) {
        b = (uint8_t)(b & 0xFDu);
    }
    CHECK(dev.program(dev.ctx, FLASH_SLOT_ADDR(1) + FWIMG_HEADER_SIZE + 4000u, &b, 1) == FLASH_OK);
    CHECK(fwimg_verify_slot(&dev, 1, NULL) == FWU_ERR_IMG_PAYLOAD_CRC);

    CHECK(write_slot(&dev, 1, g_img, FWIMG_HEADER_SIZE + 2500u) == FLASH_OK);
    CHECK(fwimg_verify_slot(&dev, 1, NULL) == FWU_ERR_IMG_PAYLOAD_CRC);

    len = build_image(10u, FWIMG_MAX_PAYLOAD);
    CHECK_EQ_U32(len, FLASH_SLOT_SIZE);
    CHECK(write_slot(&dev, 0, g_img, len) == FLASH_OK);
    CHECK(fwimg_verify_slot(&dev, 0, &hdr) == FWU_OK);
    CHECK_EQ_U32(hdr.payload_len, FWIMG_MAX_PAYLOAD);

    flashsim_close(&sim);
    remove(g_path);
}

int main(int argc, char **argv)
{
    g_path = (argc > 1) ? argv[1] : "test_image.img";
    test_golden_matches_python();
    test_valid_image_roundtrip();
    test_changed_payload_bytes();
    test_changed_header_bytes();
    test_truncated_images();
    test_oversized_images();
    test_verify_slot();
    return test_summary("test_image");
}
