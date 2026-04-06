#include <stdint.h>
#include <string.h>

#include "flash_sim.h"
#include "fwu/flash_layout.h"
#include "test_util.h"

static const char *g_path;

static int all_bytes_are(const flash_dev_t *dev, uint32_t addr, uint32_t len, uint8_t v)
{
    uint8_t buf[256];
    for (uint32_t off = 0; off < len; off += sizeof(buf)) {
        uint32_t n = (len - off < sizeof(buf)) ? len - off : (uint32_t)sizeof(buf);
        if (dev->read(dev->ctx, addr + off, buf, n) != FLASH_OK) {
            return 0;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (buf[i] != v) {
                return 0;
            }
        }
    }
    return 1;
}

static void open_dev(flashsim_t *sim, flash_dev_t *dev)
{
    CHECK(flashsim_open(sim, g_path) == FLASH_OK);
    flashsim_bind(sim, dev);
}

static void test_fresh_device_is_erased(void)
{
    flashsim_t sim;
    flash_dev_t dev;
    CHECK(flashsim_create(g_path) == FLASH_OK);
    open_dev(&sim, &dev);
    CHECK(all_bytes_are(&dev, 0, FLASH_TOTAL_SIZE, 0xFF));
    flashsim_close(&sim);
}

static void test_program_clears_bits(void)
{
    flashsim_t sim;
    flash_dev_t dev;
    uint8_t buf[4];
    CHECK(flashsim_create(g_path) == FLASH_OK);
    open_dev(&sim, &dev);

    const uint8_t a[4] = {0xF0, 0x0F, 0xA5, 0x00};
    CHECK(dev.program(dev.ctx, 100, a, 4) == FLASH_OK);
    CHECK(dev.read(dev.ctx, 100, buf, 4) == FLASH_OK);
    CHECK(memcmp(buf, a, 4) == 0);

    const uint8_t b[4] = {0x30, 0x0F, 0x21, 0x00};
    CHECK(dev.program(dev.ctx, 100, b, 4) == FLASH_OK);
    CHECK(dev.read(dev.ctx, 100, buf, 4) == FLASH_OK);
    CHECK(memcmp(buf, b, 4) == 0);

    CHECK(all_bytes_are(&dev, 99, 1, 0xFF));
    CHECK(all_bytes_are(&dev, 104, 1, 0xFF));
    flashsim_close(&sim);
}

static void test_illegal_program_rejected_atomically(void)
{
    flashsim_t sim;
    flash_dev_t dev;
    uint8_t buf[8];
    CHECK(flashsim_create(g_path) == FLASH_OK);
    open_dev(&sim, &dev);

    const uint8_t zeros[8] = {0};
    CHECK(dev.program(dev.ctx, 0x2000, zeros, 8) == FLASH_OK);

    const uint8_t one_bit[1] = {0x01};
    CHECK(dev.program(dev.ctx, 0x2000, one_bit, 1) == FLASH_ERR_PROGRAM_RULE);

    const uint8_t mixed[4] = {0x12, 0x34, 0x56, 0x78};
    CHECK(dev.program(dev.ctx, 0x2006, mixed, 4) == FLASH_ERR_PROGRAM_RULE);
    CHECK(dev.read(dev.ctx, 0x2006, buf, 4) == FLASH_OK);
    CHECK(buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0xFF && buf[3] == 0xFF);

    const uint8_t ff[1] = {0xFF};
    CHECK(dev.program(dev.ctx, 0x2000, ff, 1) == FLASH_ERR_PROGRAM_RULE);

    CHECK(dev.program(dev.ctx, 0x2000, zeros, 8) == FLASH_OK);
    flashsim_close(&sim);
}

static void test_sector_erase(void)
{
    flashsim_t sim;
    flash_dev_t dev;
    uint8_t zeros[FLASH_SECTOR_SIZE];
    memset(zeros, 0, sizeof(zeros));
    CHECK(flashsim_create(g_path) == FLASH_OK);
    open_dev(&sim, &dev);

    for (uint32_t s = 2; s <= 4; s++) {
        CHECK(dev.program(dev.ctx, s * FLASH_SECTOR_SIZE, zeros, FLASH_SECTOR_SIZE) == FLASH_OK);
    }
    CHECK(dev.erase_sector(dev.ctx, 3 * FLASH_SECTOR_SIZE) == FLASH_OK);
    CHECK(all_bytes_are(&dev, 2 * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE, 0x00));
    CHECK(all_bytes_are(&dev, 3 * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE, 0xFF));
    CHECK(all_bytes_are(&dev, 4 * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE, 0x00));

    const uint8_t v[1] = {0x5A};
    CHECK(dev.program(dev.ctx, 3 * FLASH_SECTOR_SIZE, v, 1) == FLASH_OK);

    CHECK(dev.erase_sector(dev.ctx, 1) == FLASH_ERR_ALIGN);
    CHECK(dev.erase_sector(dev.ctx, FLASH_SECTOR_SIZE + 512u) == FLASH_ERR_ALIGN);
    CHECK(dev.erase_sector(dev.ctx, FLASH_TOTAL_SIZE) == FLASH_ERR_RANGE);
    CHECK(dev.erase_sector(dev.ctx, (FLASH_SECTOR_COUNT - 1u) * FLASH_SECTOR_SIZE) == FLASH_OK);
    flashsim_close(&sim);
}

static void test_range_checks(void)
{
    flashsim_t sim;
    flash_dev_t dev;
    uint8_t buf[16];
    memset(buf, 0, sizeof(buf));
    CHECK(flashsim_create(g_path) == FLASH_OK);
    open_dev(&sim, &dev);

    CHECK(dev.read(dev.ctx, FLASH_TOTAL_SIZE - 16u, buf, 16) == FLASH_OK);
    CHECK(dev.read(dev.ctx, FLASH_TOTAL_SIZE - 15u, buf, 16) == FLASH_ERR_RANGE);
    CHECK(dev.read(dev.ctx, FLASH_TOTAL_SIZE, buf, 1) == FLASH_ERR_RANGE);
    CHECK(dev.read(dev.ctx, FLASH_TOTAL_SIZE, buf, 0) == FLASH_OK);
    CHECK(dev.read(dev.ctx, UINT32_MAX - 3u, buf, 16) == FLASH_ERR_RANGE);
    CHECK(dev.program(dev.ctx, UINT32_MAX - 3u, buf, 16) == FLASH_ERR_RANGE);
    CHECK(dev.program(dev.ctx, FLASH_TOTAL_SIZE - 15u, buf, 16) == FLASH_ERR_RANGE);
    CHECK(all_bytes_are(&dev, FLASH_TOTAL_SIZE - 16u, 16, 0xFF));

    memset(buf, 0, sizeof(buf));
    CHECK(dev.program(dev.ctx, FLASH_SECTOR_SIZE - 8u, buf, 16) == FLASH_OK);
    CHECK(all_bytes_are(&dev, FLASH_SECTOR_SIZE - 8u, 16, 0x00));
    flashsim_close(&sim);
}

static void test_persistence_across_reopen(void)
{
    flashsim_t sim;
    flash_dev_t dev;
    uint8_t buf[3];
    const uint8_t data[3] = {0xDE, 0xAD, 0x00};
    CHECK(flashsim_create(g_path) == FLASH_OK);

    open_dev(&sim, &dev);
    CHECK(dev.program(dev.ctx, FLASH_SLOT_B_ADDR, data, 3) == FLASH_OK);
    CHECK(dev.erase_sector(dev.ctx, FLASH_META1_ADDR) == FLASH_OK);
    flashsim_close(&sim);

    open_dev(&sim, &dev);
    CHECK(dev.read(dev.ctx, FLASH_SLOT_B_ADDR, buf, 3) == FLASH_OK);
    CHECK(memcmp(buf, data, 3) == 0);
    const uint8_t up[1] = {0xFF};
    CHECK(dev.program(dev.ctx, FLASH_SLOT_B_ADDR + 2u, up, 1) == FLASH_ERR_PROGRAM_RULE);
    flashsim_close(&sim);
}

static void test_open_rejects_bad_files(void)
{
    flashsim_t sim;
    char path[512];
    snprintf(path, sizeof(path), "%s.short", g_path);
    FILE *fp = fopen(path, "wb");
    CHECK(fp != NULL);
    if (fp != NULL) {
        fputc(0xFF, fp);
        fclose(fp);
    }
    CHECK(flashsim_open(&sim, path) == FLASH_ERR_RANGE);
    remove(path);

    snprintf(path, sizeof(path), "%s.missing", g_path);
    remove(path);
    CHECK(flashsim_open(&sim, path) == FLASH_ERR_IO);
}

static void test_fault_erase_points(void)
{
    flashsim_t sim;
    flash_dev_t dev;
    uint8_t zeros[FLASH_SECTOR_SIZE];
    memset(zeros, 0, sizeof(zeros));
    const uint32_t sec = 5u * FLASH_SECTOR_SIZE;

    CHECK(flashsim_create(g_path) == FLASH_OK);
    open_dev(&sim, &dev);
    CHECK(dev.program(dev.ctx, sec, zeros, FLASH_SECTOR_SIZE) == FLASH_OK);
    flashsim_close(&sim);
    open_dev(&sim, &dev);
    flashsim_set_fault(&sim, 0);
    CHECK(dev.erase_sector(dev.ctx, sec) == FLASH_ERR_POWER_LOSS);
    flashsim_close(&sim);
    open_dev(&sim, &dev);
    CHECK(all_bytes_are(&dev, sec, FLASH_SECTOR_SIZE / 2u, 0xFF));
    CHECK(all_bytes_are(&dev, sec + FLASH_SECTOR_SIZE / 2u, FLASH_SECTOR_SIZE / 2u, 0x00));
    flashsim_close(&sim);

    open_dev(&sim, &dev);
    flashsim_set_fault(&sim, 1);
    CHECK(dev.erase_sector(dev.ctx, sec) == FLASH_ERR_POWER_LOSS);
    flashsim_close(&sim);
    open_dev(&sim, &dev);
    CHECK(all_bytes_are(&dev, sec, FLASH_SECTOR_SIZE, 0xFF));
    flashsim_close(&sim);
}

static void test_fault_program_points(void)
{
    flashsim_t sim;
    flash_dev_t dev;
    const uint8_t data[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    uint8_t buf[10];

    CHECK(flashsim_create(g_path) == FLASH_OK);
    open_dev(&sim, &dev);
    flashsim_set_fault(&sim, 0);
    CHECK(dev.program(dev.ctx, 0x100, data, 10) == FLASH_ERR_POWER_LOSS);
    flashsim_close(&sim);
    open_dev(&sim, &dev);
    CHECK(dev.read(dev.ctx, 0x100, buf, 10) == FLASH_OK);
    CHECK(memcmp(buf, data, 5) == 0);
    CHECK(all_bytes_are(&dev, 0x105, 5, 0xFF));
    flashsim_close(&sim);

    CHECK(flashsim_create(g_path) == FLASH_OK);
    open_dev(&sim, &dev);
    flashsim_set_fault(&sim, 1);
    CHECK(dev.program(dev.ctx, 0x100, data, 10) == FLASH_ERR_POWER_LOSS);
    flashsim_close(&sim);
    open_dev(&sim, &dev);
    CHECK(dev.read(dev.ctx, 0x100, buf, 10) == FLASH_OK);
    CHECK(memcmp(buf, data, 10) == 0);
    flashsim_close(&sim);
}

static void test_fault_counting_and_dead_device(void)
{
    flashsim_t sim;
    flash_dev_t dev;
    uint8_t b = 0x00, buf[1];

    CHECK(flashsim_create(g_path) == FLASH_OK);
    open_dev(&sim, &dev);
    flashsim_set_fault(&sim, FLASHSIM_NO_FAULT);
    CHECK(dev.erase_sector(dev.ctx, 1) == FLASH_ERR_ALIGN);
    CHECK(dev.program(dev.ctx, FLASH_TOTAL_SIZE, &b, 1) == FLASH_ERR_RANGE);
    CHECK_EQ_U32(sim.fault_points, 0u);
    CHECK(dev.program(dev.ctx, 0x10, &b, 1) == FLASH_OK);
    CHECK(dev.erase_sector(dev.ctx, FLASH_SECTOR_SIZE) == FLASH_OK);
    CHECK(dev.read(dev.ctx, 0x10, buf, 1) == FLASH_OK);
    CHECK_EQ_U32(sim.fault_points, 4u);
    flashsim_close(&sim);

    open_dev(&sim, &dev);
    flashsim_set_fault(&sim, 0);
    CHECK(dev.erase_sector(dev.ctx, 2u * FLASH_SECTOR_SIZE) == FLASH_ERR_POWER_LOSS);
    CHECK(dev.program(dev.ctx, 0x20, &b, 1) == FLASH_ERR_POWER_LOSS);
    CHECK(dev.erase_sector(dev.ctx, 0) == FLASH_ERR_POWER_LOSS);
    CHECK(dev.read(dev.ctx, 0x20, buf, 1) == FLASH_ERR_POWER_LOSS);
    flashsim_close(&sim);
    open_dev(&sim, &dev);
    CHECK(all_bytes_are(&dev, 0x20, 1, 0xFF));
    CHECK(all_bytes_are(&dev, 0x10, 1, 0x00));
    flashsim_close(&sim);
}

int main(int argc, char **argv)
{
    g_path = (argc > 1) ? argv[1] : "test_flash_sim.img";
    test_fresh_device_is_erased();
    test_program_clears_bits();
    test_illegal_program_rejected_atomically();
    test_sector_erase();
    test_range_checks();
    test_persistence_across_reopen();
    test_open_rejects_bad_files();
    test_fault_erase_points();
    test_fault_program_points();
    test_fault_counting_and_dead_device();
    remove(g_path);
    return test_summary("test_flash_sim");
}
