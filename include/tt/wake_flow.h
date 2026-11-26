#ifndef TT_WAKE_FLOW_H
#define TT_WAKE_FLOW_H

#include <stdbool.h>
#include <stdint.h>

#include "tt/command.h"
#include "tt/retry.h"
#include "tt/tt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Platform/radio abstraction.  A production implementation provides these
 * with the modem SDK plus mbedTLS/WolfSSL/SE driver.  Tests provide a fake.
 *
 * Functions returning tt_result_t separate transport failures from the
 * vehicle-domain result carried by tt_ack_t.
 */
typedef struct tt_gateway {
    void *ctx;

    /* Always-increasing monotonic time for deadlines and back-off. */
    uint64_t (*monotonic_ms)(void *ctx);

    /*
     * Authenticated wall time, preferably from the TLS server timestamp or a
     * secure RTC.  Used only for command freshness.
     */
    tt_result_t (*trusted_unix_ms)(void *ctx, uint64_t *unix_ms);

    /* CSPRNG/HWRNG value. */
    uint32_t (*random_u32)(void *ctx);

    /* Must advance both wall/monotonic fake clocks in tests. */
    void (*delay_ms)(void *ctx, uint32_t ms);

    tt_result_t (*connect)(void *ctx,
                           uint32_t timeout_ms,
                           uint32_t *retry_after_ms);
    void (*disconnect)(void *ctx);

    tt_result_t (*pull_next)(void *ctx,
                             uint32_t timeout_ms,
                             tt_signed_command_t *out,
                             bool *has_command,
                             uint32_t *retry_after_ms);

    tt_result_t (*ack)(void *ctx,
                       const char *command_id,
                       const tt_ack_t *ack,
                       uint32_t timeout_ms,
                       uint32_t *retry_after_ms);

    /*
     * Must be idempotent with command->id and return within timeout_ms.
     * The vehicle body controller should keep a bounded command-ID cache or
     * accept an idempotency key.
     */
    tt_result_t (*execute)(void *ctx,
                           const tt_command_t *command,
                           uint32_t timeout_ms,
                           tt_ack_t *result);

    /* Optional.  Programs the next RTC/network wake and immediately sleeps. */
    tt_result_t (*schedule_next_wake)(void *ctx, uint32_t delay_ms);
} tt_gateway_t;

/*
 * Persistent boundary.  commit_result must atomically store the terminal
 * idempotency record and advance the per-device sequence checkpoint.  On
 * flash, implement this as an A/B log/key-value sector with CRC/ECC and
 * wear leveling rather than writing a raw struct to a fixed address.
 */
typedef struct tt_storage {
    void *ctx;

    tt_result_t (*load_checkpoint)(void *ctx, tt_checkpoint_t *checkpoint);

    tt_result_t (*find_result)(void *ctx,
                               const char *command_id,
                               tt_ack_t *result,
                               bool *found);

    tt_result_t (*commit_result)(void *ctx,
                                 const char *command_id,
                                 uint64_t sequence,
                                 const tt_ack_t *terminal_result);
} tt_storage_t;

typedef struct tt_wake_config {
    tt_retry_policy_t retry;
    tt_validation_config_t validation;

    uint32_t session_budget_ms;
    uint32_t first_connect_jitter_ms;
    uint32_t connect_timeout_ms;
    uint32_t pull_timeout_ms;
    uint32_t ack_timeout_ms;
    uint32_t execute_timeout_ms;
    uint32_t max_commands_per_wake;
    uint32_t pending_wake_delay_ms;
} tt_wake_config_t;

typedef struct tt_wake_report {
    tt_result_t result;
    bool secure_session_established;
    uint32_t commands_seen;
    uint32_t accepted;
    uint32_t duplicates;
    uint32_t rejected;
    uint32_t retryable;
    bool pending_remains;
    uint32_t wake_after_ms;
} tt_wake_report_t;

/* Sets safe resource bounds; caller must install verify_signature. */
tt_result_t tt_wake_config_default(tt_wake_config_t *config);

/*
 * Runs one bounded wake window after the SMS has already been reduced to a
 * doorbell.  It never parses or executes instructions from the SMS.
 */
tt_result_t tt_wake_run(const tt_wake_config_t *config,
                        tt_gateway_t *gateway,
                        tt_storage_t *storage,
                        tt_wake_report_t *report);

/* Small helper for adapters that need to populate tt_ack_t reason text. */
void tt_ack_set_reason(tt_ack_t *ack, const char *reason);

#ifdef __cplusplus
}
#endif

#endif /* TT_WAKE_FLOW_H */
