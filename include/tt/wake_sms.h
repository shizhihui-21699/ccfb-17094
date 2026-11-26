#ifndef TT_WAKE_SMS_H
#define TT_WAKE_SMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tt/tt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The SMS is an unauthenticated doorbell, not a command channel.
 *
 * Its optional nonce exists only so logs and platform wake statistics can
 * correlate one wake event.  It is never used to decide whether a vehicle
 * command is valid.  Sender number is deliberately absent from this
 * structure because SMS sender addresses can be spoofed.
 */
#define TT_WAKE_SMS_MAGIC "TT-WAKE/1"
#define TT_WAKE_NONCE_LEN 16U

typedef struct tt_wake_sms {
    /* NUL-terminated lowercase hex; empty when the bare magic was received. */
    char nonce[TT_WAKE_NONCE_LEN + 1U];
} tt_wake_sms_t;

/*
 * Parses a decoded SMS text body of exactly body_len bytes.  The PDU/UCS-2
 * decoder and modem-specific CR/LF normalization belong below this layer.
 *
 * TT_OK       - body is the dedicated wake token
 * TT_E_IGNORED- ordinary/non-wake SMS; return to sleep normally
 * TT_E_FORMAT - wake prefix is present but malformed
 */
tt_result_t tt_wake_sms_parse(const uint8_t *body,
                              size_t body_len,
                              tt_wake_sms_t *out);

/*
 * Bounds how often a forged or repeated doorbell may turn on the cellular
 * radio.  It protects the battery; it is not command authentication.
 *
 * Production should keep these counters in always-on RAM/RTC registers or
 * another reset-surviving bounded store.  Do not wear out ordinary flash by
 * rewriting this counter on every SMS.
 */
typedef struct tt_wake_limiter {
    uint32_t max_wakes_per_window;
    uint64_t window_ms;
    uint64_t window_started_ms;
    uint32_t wakes_in_window;
} tt_wake_limiter_t;

void tt_wake_limiter_init(tt_wake_limiter_t *limiter,
                          uint32_t max_wakes_per_window,
                          uint64_t window_ms);

/* Returns true when this wake session may start. Monotonic milliseconds. */
bool tt_wake_limiter_allow(tt_wake_limiter_t *limiter, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* TT_WAKE_SMS_H */
