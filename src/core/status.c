#include "fwu/meta.h"
#include "fwu/status.h"

const char *fwu_status_str(fwu_status_t st)
{
    switch (st) {
    case FWU_OK:                  return "ok";
    case FWU_ERR_ARG:             return "invalid argument";
    case FWU_ERR_STATE:           return "not allowed in current state";
    case FWU_ERR_NO_METADATA:     return "no valid metadata (run init)";
    case FWU_ERR_FLASH:           return "flash error";
    case FWU_ERR_POWER_LOSS:      return "power loss";
    case FWU_ERR_IMG_MAGIC:       return "image: bad magic";
    case FWU_ERR_IMG_FORMAT:      return "image: unsupported format";
    case FWU_ERR_IMG_HEADER_CRC:  return "image: header CRC mismatch";
    case FWU_ERR_IMG_TOO_LARGE:   return "image: too large";
    case FWU_ERR_IMG_TRUNCATED:   return "image: truncated";
    case FWU_ERR_IMG_PAYLOAD_CRC: return "image: payload CRC mismatch";
    case FWU_ERR_IMG_MISMATCH:    return "image: does not match metadata";
    case FWU_ERR_META_INVALID:    return "metadata record invalid";
    case FWU_ERR_NO_BOOTABLE:     return "no bootable image";
    }
    return "unknown";
}

const char *fwu_state_str(uint8_t state)
{
    switch (state) {
    case FWU_STATE_CONFIRMED: return "CONFIRMED";
    case FWU_STATE_PENDING:   return "PENDING";
    case FWU_STATE_TRIAL:     return "TRIAL";
    default:                  return "?";
    }
}

const char *fwu_event_str(uint8_t event)
{
    switch (event) {
    case FWU_EVENT_NONE:        return "none";
    case FWU_EVENT_UPDATED:     return "updated";
    case FWU_EVENT_ROLLED_BACK: return "rolled-back";
    case FWU_EVENT_REJECTED:    return "rejected";
    default:                    return "?";
    }
}
