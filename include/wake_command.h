#ifndef WAKE_COMMAND_H
#define WAKE_COMMAND_H

#include "wake_types.h"

typedef enum wake_cmd_type {
    WAKE_CMD_NONE = 0,
    WAKE_CMD_CLIMATE_ON = 1,
    WAKE_CMD_CLIMATE_OFF = 2,
    WAKE_CMD_DOOR_UNLOCK = 3,
    WAKE_CMD_DOOR_LOCK = 4,
    WAKE_CMD_TRUNK_UNLOCK = 5,
    WAKE_CMD_LOCATE = 6,
    WAKE_CMD_MAX = 7
} wake_cmd_type_t;

/*
 * A fully decoded and authenticated command.  The TLS/codec layer is expected
 * to decode CBOR/JSON into this fixed-size structure; firmware never parses an
 * SMS body into it.
 */
typedef struct wake_command {
    uint64_t seq;
    char id[WAKE_COMMAND_ID_MAX];
    wake_cmd_type_t type;

    /* Typed command argument. For CLIMATE_ON this is target temperature in C. */
    int32_t value;

    /* Trusted platform-clock validity window, supplied in pull response. */
    uint64_t issued_at_ms;
    uint64_t expires_at_ms;

    /* Optional detached command signature/MAC, verified through the HAL. */
    uint8_t auth_tag[WAKE_AUTH_TAG_MAX];
    uint8_t auth_tag_len;
} wake_command_t;

typedef struct wake_ack {
    uint64_t seq;
    char id[WAKE_COMMAND_ID_MAX];
    wake_status_t status;
    int32_t vendor_code;
    uint32_t execution_ms;
} wake_ack_t;

const char *wake_command_type_str(wake_cmd_type_t type);

/*
 * Structural and freshness validation only.  Signature verification and
 * vehicle-safety checks are separate HAL calls so they cannot be bypassed by
 * accidentally calling this helper.
 */
wake_status_t wake_command_validate(const wake_command_t *cmd,
                                    uint64_t trusted_now_ms);

#endif /* WAKE_COMMAND_H */
