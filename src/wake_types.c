#include "wake_types.h"

const char *wake_status_str(wake_status_t status)
{
    switch (status) {
    case WAKE_OK: return "OK";
    case WAKE_E_ARG: return "invalid-argument";
    case WAKE_E_STATE: return "invalid-state";
    case WAKE_E_INVALID: return "invalid-message";
    case WAKE_E_UNSUPPORTED: return "unsupported-command";
    case WAKE_E_TIMEOUT: return "timeout";
    case WAKE_E_RETRY: return "retryable";
    case WAKE_E_OVERLOADED: return "overloaded";
    case WAKE_E_AUTH: return "auth-failed";
    case WAKE_E_FATAL: return "fatal";
    case WAKE_E_STORE: return "store-failed";
    }
    return "unknown";
}

bool wake_status_retryable(wake_status_t status)
{
    return status == WAKE_E_TIMEOUT ||
           status == WAKE_E_RETRY ||
           status == WAKE_E_OVERLOADED;
}
