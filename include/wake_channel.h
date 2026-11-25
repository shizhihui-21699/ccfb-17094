#ifndef WAKE_CHANNEL_H
#define WAKE_CHANNEL_H

#include "wake_command.h"
#include "wake_types.h"

typedef struct wake_pull_request {
    char device_id[WAKE_DEVICE_ID_MAX];
    char wake_token[WAKE_WAKE_TOKEN_MAX];

    /* Cumulative high-water mark of durably completed commands. */
    uint64_t cursor;

    uint8_t battery_pct;
    int8_t rssi_dbm;
} wake_pull_request_t;

typedef struct wake_pull_response {
    /* Trusted platform time, epoch milliseconds; used for expiry checks. */
    uint64_t server_time_ms;

    /* On WAKE_E_OVERLOADED/WAKE_E_RETRY, platform's minimum retry delay. */
    uint32_t retry_after_ms;

    /* Platform-directed ordinary polling interval. 0 means use local default. */
    uint32_t next_pull_after_ms;

    bool has_more;
    size_t command_count;
    wake_command_t commands[WAKE_MAX_COMMANDS];
} wake_pull_response_t;

/*
 * Mutual-TLS / certificate-pinned encrypted channel.  A production port should
 * use TLS 1.2+ with device certificate or secure-element key, verify the full
 * certificate chain/hostname, and preferably pin the platform key/CA.
 *
 * pull() performs an authenticated request such as:
 *
 *     POST /v1/devices/{id}/pull
 *     { "cursor": 123, "wake_token": "...", "battery_pct": 87 }
 *
 * The HTTP layer maps 429/503 to WAKE_E_OVERLOADED and fills retry_after_ms.
 */
typedef struct wake_secure_channel {
    void *user;

    wake_status_t (*pull)(void *user,
                          const wake_pull_request_t *request,
                          wake_pull_response_t *response,
                          uint32_t timeout_ms);

    wake_status_t (*ack)(void *user,
                         const wake_ack_t *ack,
                         uint32_t timeout_ms);
} wake_secure_channel_t;

#endif /* WAKE_CHANNEL_H */
