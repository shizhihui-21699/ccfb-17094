#include "wake_command.h"

#include <string.h>

const char *wake_command_type_str(wake_cmd_type_t type)
{
    switch (type) {
    case WAKE_CMD_NONE: return "NONE";
    case WAKE_CMD_CLIMATE_ON: return "CLIMATE_ON";
    case WAKE_CMD_CLIMATE_OFF: return "CLIMATE_OFF";
    case WAKE_CMD_DOOR_UNLOCK: return "DOOR_UNLOCK";
    case WAKE_CMD_DOOR_LOCK: return "DOOR_LOCK";
    case WAKE_CMD_TRUNK_UNLOCK: return "TRUNK_UNLOCK";
    case WAKE_CMD_LOCATE: return "LOCATE";
    case WAKE_CMD_MAX: break;
    }
    return "UNKNOWN";
}

static bool valid_id(const char *id)
{
    size_t len;

    if (id == NULL || id[0] == '\0') {
        return false;
    }

    len = strnlen(id, WAKE_COMMAND_ID_MAX);
    if (len == 0 || len >= WAKE_COMMAND_ID_MAX) {
        return false;
    }

    /* IDs are platform-generated printable ASCII/correlation tokens. */
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)id[i];
        if (c <= 0x20 || c >= 0x7f) {
            return false;
        }
    }
    return true;
}

wake_status_t wake_command_validate(const wake_command_t *cmd,
                                    uint64_t trusted_now_ms)
{
    if (cmd == NULL) {
        return WAKE_E_ARG;
    }
    if (cmd->seq == 0 || !valid_id(cmd->id)) {
        return WAKE_E_INVALID;
    }
    if (cmd->type <= WAKE_CMD_NONE || cmd->type >= WAKE_CMD_MAX) {
        return WAKE_E_UNSUPPORTED;
    }
    if (cmd->expires_at_ms == 0 || trusted_now_ms == 0) {
        return WAKE_E_INVALID;
    }
    if (cmd->expires_at_ms <= trusted_now_ms) {
        return WAKE_E_INVALID;
    }
    if (cmd->issued_at_ms > cmd->expires_at_ms) {
        return WAKE_E_INVALID;
    }

    /* Reject implausibly long-lived remote-control commands. */
    if (cmd->expires_at_ms - trusted_now_ms > 24ULL * 60ULL * 60ULL * 1000ULL) {
        return WAKE_E_INVALID;
    }

    if (cmd->auth_tag_len > WAKE_AUTH_TAG_MAX) {
        return WAKE_E_INVALID;
    }

    if (cmd->type == WAKE_CMD_CLIMATE_ON &&
        (cmd->value < 16 || cmd->value > 30)) {
        return WAKE_E_INVALID;
    }

    return WAKE_OK;
}
