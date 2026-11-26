#ifndef TT_TYPES_H
#define TT_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum tt_result {
    TT_OK = 0,
    TT_E_ARG = -1,
    TT_E_IGNORED = -2,
    TT_E_FORMAT = -3,
    TT_E_TIMEOUT = -4,
    TT_E_IO = -5,
    TT_E_AUTH = -6,
    TT_E_EXPIRED = -7,
    TT_E_REPLAY = -8,
    TT_E_UNSUPPORTED = -9,
    TT_E_CAPACITY = -10,
    TT_E_CORRUPT = -11,
    TT_E_STATE = -12
} tt_result_t;

const char *tt_strerror(tt_result_t rc);

/* Saturating helpers used by several bounded state machines. */
uint64_t tt_min_u64(uint64_t a, uint64_t b);
uint32_t tt_min_u32(uint32_t a, uint32_t b);

#ifdef __cplusplus
}
#endif

#endif /* TT_TYPES_H */
