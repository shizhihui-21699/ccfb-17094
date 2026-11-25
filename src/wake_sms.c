#include "wake_sms.h"

#include <string.h>

#define WAKE_SMS_PREFIX "WAKE1 "
#define WAKE_SMS_PREFIX_LEN 6U

static bool token_char(char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

wake_sms_signal_t wake_sms_classify(const char *sender, const char *body)
{
    wake_sms_signal_t result;
    const char *p;
    size_t token_len = 0;

    memset(&result, 0, sizeof(result));

    /* Explicitly untrusted and unused. */
    (void)sender;

    if (body == NULL || strncmp(body, WAKE_SMS_PREFIX, WAKE_SMS_PREFIX_LEN) != 0) {
        return result;
    }

    p = body + WAKE_SMS_PREFIX_LEN;
    while (*p != '\0' && *p != '\r' && *p != '\n') {
        if (!token_char(*p) || token_len + 1U >= sizeof(result.token)) {
            memset(&result, 0, sizeof(result));
            return result;
        }
        result.token[token_len++] = *p;
        ++p;
    }

    if (token_len == 0) {
        memset(&result, 0, sizeof(result));
        return result;
    }

    /* Permit only line ending characters after the token. */
    while (*p != '\0') {
        if (*p != '\r' && *p != '\n') {
            memset(&result, 0, sizeof(result));
            return result;
        }
        ++p;
    }

    result.token[token_len] = '\0';
    result.classification = WAKE_SMS_WAKE_SIGNAL;
    return result;
}
