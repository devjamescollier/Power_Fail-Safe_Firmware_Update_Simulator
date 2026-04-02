#include "flash_sim.h"

#include <string.h>

#include "fwu/flash_layout.h"

#define SIM_CHUNK 256u

static flash_status_t check_range(uint32_t addr, uint32_t len)
{
    if (len > FLASH_TOTAL_SIZE || addr > FLASH_TOTAL_SIZE - len) {
        return FLASH_ERR_RANGE;
    }
    return FLASH_OK;
}

static flash_status_t file_read(FILE *fp, uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (fseek(fp, (long)addr, SEEK_SET) != 0 || fread(buf, 1, len, fp) != len) {
        return FLASH_ERR_IO;
    }
    return FLASH_OK;
}

static flash_status_t file_write(FILE *fp, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if (fseek(fp, (long)addr, SEEK_SET) != 0 || fwrite(buf, 1, len, fp) != len ||
        fflush(fp) != 0) {
        return FLASH_ERR_IO;
    }
    return FLASH_OK;
}

flash_status_t flashsim_create(const char *path)
{
    uint8_t erased[SIM_CHUNK];
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        return FLASH_ERR_IO;
    }
    memset(erased, FLASH_ERASED_BYTE, sizeof(erased));
    flash_status_t st = FLASH_OK;
    for (uint32_t off = 0; off < FLASH_TOTAL_SIZE && st == FLASH_OK; off += SIM_CHUNK) {
        if (fwrite(erased, 1, SIM_CHUNK, fp) != SIM_CHUNK) {
            st = FLASH_ERR_IO;
        }
    }
    if (fclose(fp) != 0) {
        st = FLASH_ERR_IO;
    }
    return st;
}

flash_status_t flashsim_open(flashsim_t *sim, const char *path)
{
    memset(sim, 0, sizeof(*sim));
    sim->fault_at = FLASHSIM_NO_FAULT;

    FILE *fp = fopen(path, "r+b");
    if (fp == NULL) {
        return FLASH_ERR_IO;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return FLASH_ERR_IO;
    }
    if (ftell(fp) != (long)FLASH_TOTAL_SIZE) {
        fclose(fp);
        return FLASH_ERR_RANGE;
    }
    sim->fp = fp;
    return FLASH_OK;
}

void flashsim_close(flashsim_t *sim)
{
    if (sim->fp != NULL) {
        fclose(sim->fp);
        sim->fp = NULL;
    }
}

void flashsim_set_fault(flashsim_t *sim, uint32_t fault_at)
{
    sim->fault_at = fault_at;
}

static bool fault_point(flashsim_t *sim)
{
    bool hit = sim->fault_points == sim->fault_at;
    sim->fault_points++;
    if (hit) {
        sim->power_lost = true;
    }
    return hit;
}

static flash_status_t write_erased(FILE *fp, uint32_t addr, uint32_t len)
{
    uint8_t erased[SIM_CHUNK];
    flash_status_t st = FLASH_OK;
    memset(erased, FLASH_ERASED_BYTE, sizeof(erased));
    for (uint32_t off = 0; off < len && st == FLASH_OK; off += SIM_CHUNK) {
        uint32_t n = (len - off < SIM_CHUNK) ? len - off : SIM_CHUNK;
        st = file_write(fp, addr + off, erased, n);
    }
    return st;
}

static flash_status_t sim_read(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len)
{
    flashsim_t *sim = ctx;
    if (sim->power_lost) {
        return FLASH_ERR_POWER_LOSS;
    }
    flash_status_t st = check_range(addr, len);
    if (st != FLASH_OK || len == 0u) {
        return st;
    }
    return file_read(sim->fp, addr, buf, len);
}

static flash_status_t sim_program(void *ctx, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    flashsim_t *sim = ctx;
    uint8_t cur[SIM_CHUNK];
    if (sim->power_lost) {
        return FLASH_ERR_POWER_LOSS;
    }
    flash_status_t st = check_range(addr, len);
    if (st != FLASH_OK || len == 0u) {
        return st;
    }

    for (uint32_t off = 0; off < len; off += SIM_CHUNK) {
        uint32_t n = (len - off < SIM_CHUNK) ? len - off : SIM_CHUNK;
        st = file_read(sim->fp, addr + off, cur, n);
        if (st != FLASH_OK) {
            return st;
        }
        for (uint32_t i = 0; i < n; i++) {
            if ((uint8_t)(cur[i] & buf[off + i]) != buf[off + i]) {
                return FLASH_ERR_PROGRAM_RULE;
            }
        }
    }

    if (fault_point(sim)) {
        st = file_write(sim->fp, addr, buf, len / 2u);
        return st == FLASH_OK ? FLASH_ERR_POWER_LOSS : st;
    }
    st = file_write(sim->fp, addr, buf, len);
    if (st == FLASH_OK && fault_point(sim)) {
        return FLASH_ERR_POWER_LOSS;
    }
    return st;
}

static flash_status_t sim_erase_sector(void *ctx, uint32_t sector_addr)
{
    flashsim_t *sim = ctx;
    if (sim->power_lost) {
        return FLASH_ERR_POWER_LOSS;
    }
    if (sector_addr % FLASH_SECTOR_SIZE != 0u) {
        return FLASH_ERR_ALIGN;
    }
    flash_status_t st = check_range(sector_addr, FLASH_SECTOR_SIZE);
    if (st != FLASH_OK) {
        return st;
    }
    if (fault_point(sim)) {
        st = write_erased(sim->fp, sector_addr, FLASH_SECTOR_SIZE / 2u);
        return st == FLASH_OK ? FLASH_ERR_POWER_LOSS : st;
    }
    st = write_erased(sim->fp, sector_addr, FLASH_SECTOR_SIZE);
    if (st == FLASH_OK && fault_point(sim)) {
        return FLASH_ERR_POWER_LOSS;
    }
    return st;
}

void flashsim_bind(flashsim_t *sim, flash_dev_t *dev)
{
    dev->ctx = sim;
    dev->read = sim_read;
    dev->program = sim_program;
    dev->erase_sector = sim_erase_sector;
}
