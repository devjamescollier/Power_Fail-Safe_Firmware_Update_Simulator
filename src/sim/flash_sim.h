#ifndef FWU_FLASH_SIM_H
#define FWU_FLASH_SIM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "fwu/flash_if.h"

#define FLASHSIM_NO_FAULT UINT32_MAX

typedef struct {
    FILE    *fp;
    uint32_t fault_at;
    uint32_t fault_points;
    bool     power_lost;
} flashsim_t;

flash_status_t flashsim_create(const char *path);

flash_status_t flashsim_open(flashsim_t *sim, const char *path);
void flashsim_close(flashsim_t *sim);

void flashsim_set_fault(flashsim_t *sim, uint32_t fault_at);

void flashsim_bind(flashsim_t *sim, flash_dev_t *dev);

#endif
