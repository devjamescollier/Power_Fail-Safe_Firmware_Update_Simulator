#include <string.h>

#include "fwu/crc32.h"
#include "test_util.h"

int main(void)
{
    const uint8_t check[] = "123456789";

    CHECK_EQ_U32(crc32_compute(check, 9), 0xCBF43926u);
    CHECK_EQ_U32(crc32_compute(NULL, 0), 0x00000000u);

    uint32_t c = CRC32_INIT;
    c = crc32_update(c, check, 4);
    c = crc32_update(c, check + 4, 5);
    CHECK_EQ_U32(crc32_final(c), 0xCBF43926u);

    uint8_t buf[9];
    memcpy(buf, check, sizeof(buf));
    buf[3] ^= 0x01u;
    CHECK(crc32_compute(buf, sizeof(buf)) != 0xCBF43926u);

    return test_summary("test_crc32");
}
