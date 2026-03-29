#include "fwu/image.h"

#include <stddef.h>

#include "byteorder.h"
#include "fwu/crc32.h"

#define OFF_MAGIC        0u
#define OFF_FORMAT       4u
#define OFF_HEADER_SIZE  6u
#define OFF_FW_VERSION   8u
#define OFF_PAYLOAD_LEN  12u
#define OFF_PAYLOAD_CRC  16u
#define OFF_HEADER_CRC   20u

#define VERIFY_CHUNK     256u

void fwimg_header_encode(const fwimg_header_t *hdr, uint8_t out[FWIMG_HEADER_SIZE])
{
    put_le32(&out[OFF_MAGIC], FWIMG_MAGIC);
    put_le16(&out[OFF_FORMAT], FWIMG_FORMAT_VERSION);
    put_le16(&out[OFF_HEADER_SIZE], FWIMG_HEADER_SIZE);
    put_le32(&out[OFF_FW_VERSION], hdr->fw_version);
    put_le32(&out[OFF_PAYLOAD_LEN], hdr->payload_len);
    put_le32(&out[OFF_PAYLOAD_CRC], hdr->payload_crc);
    put_le32(&out[OFF_HEADER_CRC], crc32_compute(out, OFF_HEADER_CRC));
}

fwu_status_t fwimg_header_decode(const uint8_t in[FWIMG_HEADER_SIZE], fwimg_header_t *out)
{
    if (get_le32(&in[OFF_MAGIC]) != FWIMG_MAGIC) {
        return FWU_ERR_IMG_MAGIC;
    }
    if (get_le16(&in[OFF_FORMAT]) != FWIMG_FORMAT_VERSION ||
        get_le16(&in[OFF_HEADER_SIZE]) != FWIMG_HEADER_SIZE) {
        return FWU_ERR_IMG_FORMAT;
    }
    if (get_le32(&in[OFF_HEADER_CRC]) != crc32_compute(in, OFF_HEADER_CRC)) {
        return FWU_ERR_IMG_HEADER_CRC;
    }

    uint32_t payload_len = get_le32(&in[OFF_PAYLOAD_LEN]);
    if (payload_len == 0u) {
        return FWU_ERR_IMG_FORMAT;
    }
    if (payload_len > FWIMG_MAX_PAYLOAD) {
        return FWU_ERR_IMG_TOO_LARGE;
    }

    out->fw_version = get_le32(&in[OFF_FW_VERSION]);
    out->payload_len = payload_len;
    out->payload_crc = get_le32(&in[OFF_PAYLOAD_CRC]);
    return FWU_OK;
}

fwu_status_t fwimg_check_buffer(const uint8_t *img, uint32_t len, fwimg_header_t *out)
{
    fwimg_header_t hdr;
    if (img == NULL && len != 0u) {
        return FWU_ERR_ARG;
    }
    if (len < FWIMG_HEADER_SIZE) {
        return FWU_ERR_IMG_TRUNCATED;
    }
    fwu_status_t st = fwimg_header_decode(img, &hdr);
    if (st != FWU_OK) {
        return st;
    }
    uint32_t total = FWIMG_HEADER_SIZE + hdr.payload_len;
    if (len < total) {
        return FWU_ERR_IMG_TRUNCATED;
    }
    if (len > total) {
        return FWU_ERR_IMG_TOO_LARGE;
    }
    if (crc32_compute(&img[FWIMG_HEADER_SIZE], hdr.payload_len) != hdr.payload_crc) {
        return FWU_ERR_IMG_PAYLOAD_CRC;
    }
    if (out != NULL) {
        *out = hdr;
    }
    return FWU_OK;
}

fwu_status_t fwimg_verify_slot(const flash_dev_t *flash, uint8_t slot, fwimg_header_t *out)
{
    uint8_t buf[VERIFY_CHUNK];
    fwimg_header_t hdr;

    if (flash == NULL || slot >= FLASH_SLOT_COUNT) {
        return FWU_ERR_ARG;
    }
    uint32_t base = FLASH_SLOT_ADDR(slot);

    flash_status_t fst = flash->read(flash->ctx, base, buf, FWIMG_HEADER_SIZE);
    if (fst != FLASH_OK) {
        return fst == FLASH_ERR_POWER_LOSS ? FWU_ERR_POWER_LOSS : FWU_ERR_FLASH;
    }
    fwu_status_t st = fwimg_header_decode(buf, &hdr);
    if (st != FWU_OK) {
        return st;
    }

    uint32_t crc = CRC32_INIT;
    for (uint32_t off = 0; off < hdr.payload_len; off += VERIFY_CHUNK) {
        uint32_t n = hdr.payload_len - off;
        if (n > VERIFY_CHUNK) {
            n = VERIFY_CHUNK;
        }
        fst = flash->read(flash->ctx, base + FWIMG_HEADER_SIZE + off, buf, n);
        if (fst != FLASH_OK) {
            return fst == FLASH_ERR_POWER_LOSS ? FWU_ERR_POWER_LOSS : FWU_ERR_FLASH;
        }
        crc = crc32_update(crc, buf, n);
    }
    if (crc32_final(crc) != hdr.payload_crc) {
        return FWU_ERR_IMG_PAYLOAD_CRC;
    }
    if (out != NULL) {
        *out = hdr;
    }
    return FWU_OK;
}
