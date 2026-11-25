#ifndef WAKE_TYPES_H
#define WAKE_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WAKE_DEVICE_ID_MAX       40
#define WAKE_WAKE_TOKEN_MAX      40
#define WAKE_COMMAND_ID_MAX      40
#define WAKE_MAX_COMMANDS        4
#define WAKE_AUTH_TAG_MAX        32

typedef enum wake_status {
    WAKE_OK = 0,
    WAKE_E_ARG = 1,
    WAKE_E_STATE = 2,
    WAKE_E_INVALID = 3,
    WAKE_E_UNSUPPORTED = 4,
    WAKE_E_TIMEOUT = 5,
    WAKE_E_RETRY = 6,
    WAKE_E_OVERLOADED = 7,
    WAKE_E_AUTH = 8,
    WAKE_E_FATAL = 9,
    WAKE_E_STORE = 10
} wake_status_t;

const char *wake_status_str(wake_status_t status);
bool wake_status_retryable(wake_status_t status);

#endif /* WAKE_TYPES_H */
