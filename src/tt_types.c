#include "tt/tt_types.h"

const char *tt_strerror(tt_result_t rc)
{
    switch (rc) {
    case TT_OK: return "ok";
    case TT_E_ARG: return "invalid argument";
    case TT_E_IGNORED: return "ignored";
    case TT_E_FORMAT: return "format error";
    case TT_E_TIMEOUT: return "deadline or timeout";
    case TT_E_IO: return "i/o error";
    case TT_E_AUTH: return "authentication error";
    case TT_E_EXPIRED: return "expired";
    case TT_E_REPLAY: return "replay or sequence error";
    case TT_E_UNSUPPORTED: return "unsupported";
    case TT_E_CAPACITY: return "platform busy";
    case TT_E_CORRUPT: return "corrupt state";
    case TT_E_STATE: return "invalid state";
    }
    return "unknown";
}

uint64_t tt_min_u64(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

uint32_t tt_min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}
