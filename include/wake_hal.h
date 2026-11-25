#ifndef WAKE_HAL_H
#define WAKE_HAL_H

#include "wake_command.h"
#include "wake_types.h"

/*
 * Persistent counter/state.  The port must provide atomic writes (for example
 * swap two flash sectors or use KV store with CRC).  Losing @cursor can cause
 * a redelivery; command handlers must therefore be idempotent.
 */
typedef struct wake_store_state {
    uint64_t cursor;

    /*
     * Last final result waiting for explicit ACK.  The cursor in pull requests
     * already cumulatively acknowledges completion; these fields allow the
     * platform to learn the detailed result after an ACK failure/reboot.
     */
    uint64_t pending_ack_seq;
    wake_status_t pending_ack_status;
    int32_t pending_ack_vendor_code;
    char pending_ack_id[WAKE_COMMAND_ID_MAX];

    char last_wake_token[WAKE_WAKE_TOKEN_MAX];
    uint64_t last_wake_mono_ms;
    uint64_t last_success_mono_ms;

    uint64_t rate_window_start_mono_ms;
    uint32_t rate_window_count;

    uint32_t wake_count;
} wake_store_state_t;

/*
 * All platform/OS dependencies are behind this table.  The wake state machine
 * itself contains no socket, AT-command, flash, or vehicle-bus specifics.
 */
typedef struct wake_hal {
    void *user;

    uint64_t (*now_ms)(void *user);

    /*
     * Wait for at least duration_ms.  The port should use the deepest safe
     * low-power idle/timer state; for a high-current application-processor
     * wakelock it may release/reacquire that lock around this wait while
     * preserving RAM and the wake task.
     */
    void (*sleep_ms)(void *user, uint32_t duration_ms);
    uint32_t (*random_u32)(void *user);

    /* Returns 0..100, or 255 when battery level is unknown. */
    uint8_t (*battery_pct)(void *user);

    /* Attach packet service and bring up PDP/TLS-capable data path. */
    wake_status_t (*radio_attach)(void *user, uint32_t timeout_ms);

    wake_status_t (*store_load)(void *user, wake_store_state_t *state);
    wake_status_t (*store_save)(void *user, const wake_store_state_t *state);

    /* Optional application-layer command signature/MAC verification. */
    wake_status_t (*verify_command)(void *user, const wake_command_t *cmd);

    /* Optional safety interlock: speed, gear, ignition, door state, etc. */
    bool (*safety_allowed)(void *user, const wake_command_t *cmd);

    /* Must be idempotent because at-least-once delivery is used. */
    wake_status_t (*execute_command)(void *user,
                                     const wake_command_t *cmd,
                                     wake_ack_t *ack);

    /* Hold/release the applications processor power rail/wakelock. */
    void (*power_hold)(void *user, bool hold);

    /* Program the low-power wake timer before releasing the wakelock. */
    void (*schedule_next_wake)(void *user, uint32_t delay_ms);
} wake_hal_t;

#endif /* WAKE_HAL_H */
