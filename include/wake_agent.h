#ifndef WAKE_AGENT_H
#define WAKE_AGENT_H

#include "wake_channel.h"
#include "wake_hal.h"
#include "wake_types.h"

typedef enum wake_run_outcome {
    WAKE_RUN_INVALID_ARGUMENT = 0,
    WAKE_RUN_OK = 1,
    WAKE_RUN_OK_NO_WORK = 2,
    WAKE_RUN_COALESCED = 3,
    WAKE_RUN_RATE_LIMITED = 4,
    WAKE_RUN_LOW_BATTERY = 5,
    WAKE_RUN_DEADLINE = 6,
    WAKE_RUN_NETWORK_FATAL = 7,
    WAKE_RUN_AUTH_FATAL = 8,
    WAKE_RUN_PROTOCOL_FATAL = 9,
    WAKE_RUN_STORE_FATAL = 10
} wake_run_outcome_t;

typedef struct wake_config {
    const char *device_id;

    /* Hard upper bound for this wake window, including jitter and retries. */
    uint32_t hard_window_ms;

    /* Initial fleet-spreading delay after a mass SMS wake. */
    uint32_t pre_connect_jitter_ms;

    uint32_t radio_attach_timeout_ms;
    uint32_t pull_timeout_ms;
    uint32_t ack_timeout_ms;

    /* Leave at least this much time to do useful work. */
    uint32_t min_operation_ms;

    uint32_t initial_backoff_ms;
    uint32_t max_backoff_ms;

    /* Zero means retry until hard_window_ms is exhausted. */
    uint32_t max_attempts;

    /* Duplicate wake token within this interval is coalesced after success. */
    uint32_t duplicate_window_ms;

    /* Persistent SMS-wake flood budget. 0 selects 6; UINT16_MAX disables. */
    uint32_t wake_rate_window_ms;
    uint16_t max_wakes_per_window;

    /* Ordinary low-power check-in interval. */
    uint32_t routine_checkin_ms;

    uint8_t min_battery_pct;

    /* Require HAL command signature/MAC verification. */
    bool require_app_auth;
} wake_config_t;

typedef struct wake_report {
    wake_run_outcome_t outcome;
    wake_status_t last_transport;

    uint32_t attempts;
    uint32_t commands_received;
    uint32_t commands_succeeded;
    uint32_t acks_succeeded;

    uint32_t elapsed_ms;
    uint32_t next_wake_delay_ms;
    uint64_t cursor;
} wake_report_t;

void wake_config_default(wake_config_t *config);
const char *wake_run_outcome_str(wake_run_outcome_t outcome);

/*
 * Run one deadline-bounded wake/pull/execute/ack cycle.  This function never
 * inspects an SMS PDU or sender; @wake_token is only a correlation/dedup token.
 * Pass NULL for a locally scheduled RTC retry or routine check-in; those runs
 * do not consume the SMS flood budget.
 *
 * The function always releases the power wakelock through the HAL after it was
 * acquired.  It is safe to call from the wake task after the modem driver has
 * validated/stored the incoming SMS event.
 */
wake_report_t wake_run(const wake_config_t *config,
                       const wake_hal_t *hal,
                       wake_secure_channel_t *channel,
                       const char *wake_token);

#endif /* WAKE_AGENT_H */
