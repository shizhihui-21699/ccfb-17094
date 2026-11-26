#ifndef TT_COMMAND_H
#define TT_COMMAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tt/tt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TT_COMMAND_ID_MAX 40U
#define TT_REASON_MAX 64U
#define TT_COMMAND_PAYLOAD_MAX 512U
#define TT_COMMAND_SIGNATURE_MAX 96U

typedef enum tt_command_type {
    TT_CMD_NOOP = 0,
    TT_CMD_LOCK = 1,
    TT_CMD_UNLOCK = 2,
    TT_CMD_SET_CLIMATE = 3,
    TT_CMD_LOCATE = 4
} tt_command_type_t;

typedef struct tt_command {
    char id[TT_COMMAND_ID_MAX + 1U];
    uint64_t sequence;

    /* Signed platform timestamps, milliseconds since Unix epoch. */
    uint64_t issued_at_ms;
    uint64_t expires_at_ms;

    tt_command_type_t type;

    /* Valid only for TT_CMD_SET_CLIMATE. */
    int16_t target_temp_c;
} tt_command_t;

/*
 * The gateway adapter parses JSON/CBOR/nanopb into command while retaining
 * the exact signed wire bytes.  Verification must cover those bytes, not a
 * re-serialized structure.
 */
typedef struct tt_signed_command {
    tt_command_t command;
    uint8_t canonical_payload[TT_COMMAND_PAYLOAD_MAX];
    uint16_t payload_len;
    uint8_t signature[TT_COMMAND_SIGNATURE_MAX];
    uint8_t signature_len;
} tt_signed_command_t;

typedef enum tt_ack_code {
    TT_ACK_ACCEPTED = 1,
    TT_ACK_DUPLICATE = 2,
    TT_ACK_REJECTED = 3,
    TT_ACK_RETRYABLE = 4
} tt_ack_code_t;

typedef struct tt_ack {
    tt_ack_code_t code;
    int32_t vehicle_error;
    char reason[TT_REASON_MAX + 1U];
} tt_ack_t;

typedef tt_result_t (*tt_signature_verify_fn)(
    void *verify_ctx,
    const uint8_t *payload,
    size_t payload_len,
    const uint8_t *signature,
    size_t signature_len);

typedef struct tt_validation_config {
    tt_signature_verify_fn verify_signature;
    void *verify_ctx;

    uint64_t max_clock_skew_ms;
    uint32_t max_command_ttl_ms;
    int16_t min_temp_c;
    int16_t max_temp_c;
} tt_validation_config_t;

/* Validates signature, freshness, allow-list and parameter ranges. */
tt_result_t tt_command_validate(const tt_signed_command_t *envelope,
                                uint64_t trusted_now_ms,
                                const tt_validation_config_t *config);

/* Per-device monotonic checkpoint used to reject replay/conflicting IDs. */
typedef struct tt_checkpoint {
    uint64_t last_sequence;
    char last_command_id[TT_COMMAND_ID_MAX + 1U];
} tt_checkpoint_t;

/*
 * Returns TT_OK and sets duplicate=true for an already checkpointed exact
 * command.  A matching sequence with a different ID is TT_E_REPLAY.
 */
tt_result_t tt_checkpoint_check(const tt_checkpoint_t *checkpoint,
                                const tt_command_t *command,
                                bool *duplicate);

const char *tt_command_type_name(tt_command_type_t type);
const char *tt_ack_code_name(tt_ack_code_t code);

#ifdef __cplusplus
}
#endif

#endif /* TT_COMMAND_H */
