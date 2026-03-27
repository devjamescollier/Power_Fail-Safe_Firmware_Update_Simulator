#ifndef FWU_FLASH_LAYOUT_H
#define FWU_FLASH_LAYOUT_H

#define FLASH_SECTOR_SIZE        4096u
#define FLASH_ERASED_BYTE        0xFFu

#define FLASH_PROGRAM_CHUNK      256u

#define FLASH_META_SECTOR_COUNT  2u
#define FLASH_META0_ADDR         0x00000u
#define FLASH_META1_ADDR         (FLASH_META0_ADDR + FLASH_SECTOR_SIZE)
#define FLASH_META_ADDR(i)       (FLASH_META0_ADDR + (unsigned)(i) * FLASH_SECTOR_SIZE)

#define FLASH_SLOT_COUNT         2u
#define FLASH_SLOT_SECTORS       16u
#define FLASH_SLOT_SIZE          (FLASH_SLOT_SECTORS * FLASH_SECTOR_SIZE)
#define FLASH_SLOT_A_ADDR        (FLASH_META0_ADDR + FLASH_META_SECTOR_COUNT * FLASH_SECTOR_SIZE)
#define FLASH_SLOT_B_ADDR        (FLASH_SLOT_A_ADDR + FLASH_SLOT_SIZE)
#define FLASH_SLOT_ADDR(i)       (FLASH_SLOT_A_ADDR + (unsigned)(i) * FLASH_SLOT_SIZE)

#define FLASH_TOTAL_SIZE         (FLASH_SLOT_B_ADDR + FLASH_SLOT_SIZE)
#define FLASH_SECTOR_COUNT       (FLASH_TOTAL_SIZE / FLASH_SECTOR_SIZE)

_Static_assert(FLASH_SLOT_A_ADDR == 0x02000u, "flash map changed: update docs");
_Static_assert(FLASH_SLOT_B_ADDR == 0x12000u, "flash map changed: update docs");
_Static_assert(FLASH_TOTAL_SIZE == 0x22000u, "flash map changed: update docs");
_Static_assert(FLASH_SLOT_A_ADDR % FLASH_SECTOR_SIZE == 0u, "slot A not sector aligned");
_Static_assert(FLASH_SLOT_B_ADDR % FLASH_SECTOR_SIZE == 0u, "slot B not sector aligned");
_Static_assert(FLASH_SECTOR_SIZE % FLASH_PROGRAM_CHUNK == 0u, "chunk must divide sector");

#endif
