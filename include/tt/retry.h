#ifndef TT_RETRY_H
#define TT_RETRY_H

#include <stdbool.h>
#include <stdint.h>

#include "tt/tt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic deadline for one radio wake window. */
typedef struct tt_deadline {
    uint64_t start_ms;
    uint32_t budget_ms;
} tt_deadline_t;

uint64_t tt_deadline_remaining_ms(const tt_deadline_t *deadline,
                                  uint64_t now_ms);

/*
 * True if at least operation_reserve_ms remains.  The reserve covers a
 * connect/pull/ack attempt plus graceful TLS/modem shutdown.
 */
bool tt_deadline_has_room(const tt_deadline_t *deadline,
                          uint64_t now_ms,
                          uint32_t operation_reserve_ms);

typedef struct tt_retry_policy {
    uint32_t max_attempts;
    uint32_t initial_delay_ms;
    uint32_t max_delay_ms;
    uint32_t attempt_reserve_ms;
} tt_retry_policy_t;

typedef struct tt_retry_decision {
    bool should_retry;
    uint32_t delay_ms;

    /*
     * Non-zero when the authenticated platform asked the device to retry
     * later than this wake window.  Firmware should program RTC/wake source
     * for this value and sleep immediately.
     */
    uint32_t off_session_wake_ms;
} tt_retry_decision_t;

/*
 * failed_attempts is the number of network attempts already made.
 * random_bits should come from a hardware RNG or per-device seeded CSPRNG.
 * server_retry_after_ms may be zero; otherwise it is an authenticated
 * Retry-After/backoff hint from the platform.
 */
tt_result_t tt_retry_plan(const tt_retry_policy_t *policy,
                          const tt_deadline_t *deadline,
                          uint32_t failed_attempts,
                          uint64_t now_ms,
                          uint32_t random_bits,
                          uint32_t server_retry_after_ms,
                          tt_retry_decision_t *decision);

/* Full-jitter delay, exposed for platform/unit tests. */
uint32_t tt_retry_jitter_delay(uint32_t failed_attempts,
                               uint32_t initial_delay_ms,
                               uint32_t max_delay_ms,
                               uint32_t random_bits);

#ifdef __cplusplus
}
#endif

#endif /* TT_RETRY_H */
