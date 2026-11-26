#include "tt/command.h"

#include <string.h>

static size_t bounded_len(const char *s, size_t capacity)
{
    const void *nul = memchr(s, '\0', capacity);
    if (nul == NULL) {
        return capacity + 1U;
    }
    return (size_t)((const char *)nul - s);
}

static bool valid_id(const char *id)
{
    size_t len = bounded_len(id, TT_COMMAND_ID_MAX + 1U);
    size_t i;

    if (len == 0U || len > TT_COMMAND_ID_MAX) {
        return false;
    }
    for (i = 0; i < len; ++i) {
        char c = id[i];
        bool ok = (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') ||
                  c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

tt_result_t tt_command_validate(const tt_signed_command_t *envelope,
                                uint64_t trusted_now_ms,
                                const tt_validation_config_t *config)
{
    const tt_command_t *cmd;
    tt_result_t rc;

    if (envelope == NULL || config == NULL ||
        config->verify_signature == NULL || config->max_command_ttl_ms == 0U ||
        config->min_temp_c > config->max_temp_c) {
        return TT_E_ARG;
    }

    if (envelope->payload_len == 0U ||
        envelope->payload_len > TT_COMMAND_PAYLOAD_MAX ||
        envelope->signature_len == 0U ||
        envelope->signature_len > TT_COMMAND_SIGNATURE_MAX) {
        return TT_E_FORMAT;
    }

    /* Verify untrusted signed bytes before acting on parsed fields. */
    rc = config->verify_signature(config->verify_ctx,
                                  envelope->canonical_payload,
                                  envelope->payload_len,
                                  envelope->signature,
                                  envelope->signature_len);
    if (rc != TT_OK) {
        return TT_E_AUTH;
    }

    cmd = &envelope->command;
    if (!valid_id(cmd->id) || cmd->sequence == 0U) {
        return TT_E_FORMAT;
    }

    if (cmd->issued_at_ms == 0U || cmd->expires_at_ms <= cmd->issued_at_ms) {
        return TT_E_FORMAT;
    }
    if (cmd->expires_at_ms - cmd->issued_at_ms > config->max_command_ttl_ms) {
        return TT_E_FORMAT;
    }

    /* Reject commands issued too far in the future for a skewed RTC. */
    if (trusted_now_ms < cmd->issued_at_ms &&
        cmd->issued_at_ms - trusted_now_ms > config->max_clock_skew_ms) {
        return TT_E_EXPIRED;
    }
    if (trusted_now_ms > cmd->expires_at_ms) {
        return TT_E_EXPIRED;
    }

    switch (cmd->type) {
    case TT_CMD_LOCK:
    case TT_CMD_UNLOCK:
    case TT_CMD_LOCATE:
    case TT_CMD_NOOP:
        return TT_OK;
    case TT_CMD_SET_CLIMATE:
        if (cmd->target_temp_c < config->min_temp_c ||
            cmd->target_temp_c > config->max_temp_c) {
            return TT_E_FORMAT;
        }
        return TT_OK;
    default:
        return TT_E_UNSUPPORTED;
    }
}

tt_result_t tt_checkpoint_check(const tt_checkpoint_t *checkpoint,
                                const tt_command_t *command,
                                bool *duplicate)
{
    if (checkpoint == NULL || command == NULL || duplicate == NULL) {
        return TT_E_ARG;
    }
    *duplicate = false;

    if (checkpoint->last_sequence == 0U) {
        return TT_OK;
    }

    if (command->sequence > checkpoint->last_sequence) {
        return TT_OK;
    }

    if (command->sequence == checkpoint->last_sequence) {
        if (strncmp(checkpoint->last_command_id,
                    command->id,
                    TT_COMMAND_ID_MAX + 1U) == 0) {
            *duplicate = true;
            return TT_OK;
        }
        return TT_E_REPLAY;
    }

    /*
     * Older sequence.  The orchestrator first checks the bounded idempotency
     * store by command ID; reaching here means it is not a known redelivery.
     */
    return TT_E_REPLAY;
}

const char *tt_command_type_name(tt_command_type_t type)
{
    switch (type) {
    case TT_CMD_NOOP: return "noop";
    case TT_CMD_LOCK: return "lock";
    case TT_CMD_UNLOCK: return "unlock";
    case TT_CMD_SET_CLIMATE: return "set_climate";
    case TT_CMD_LOCATE: return "locate";
    }
    return "unknown";
}

const char *tt_ack_code_name(tt_ack_code_t code)
{
    switch (code) {
    case TT_ACK_ACCEPTED: return "accepted";
    case TT_ACK_DUPLICATE: return "duplicate";
    case TT_ACK_REJECTED: return "rejected";
    case TT_ACK_RETRYABLE: return "retryable";
    }
    return "unknown";
}
