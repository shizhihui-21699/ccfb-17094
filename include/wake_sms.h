#ifndef WAKE_SMS_H
#define WAKE_SMS_H

#include "wake_types.h"

typedef enum wake_sms_class {
    WAKE_SMS_IGNORED = 0,
    WAKE_SMS_WAKE_SIGNAL = 1
} wake_sms_class_t;

/*
 * This structure intentionally has no command, parameter, action, or sender
 * trust field.  An SMS can only request a pull.
 */
typedef struct wake_sms_signal {
    wake_sms_class_t classification;
    char token[WAKE_WAKE_TOKEN_MAX];
} wake_sms_signal_t;

/*
 * Classify an inbound SMS.  @sender is accepted only to make the untrusted
 * boundary explicit; it is never compared against an allow-list because the
 * source address is spoofable.
 *
 * Accepted body grammar, after trimming trailing CR/LF:
 *
 *     WAKE1 <1..39 chars [A-Za-z0-9_-]>
 *
 * Any extra field, semicolon, "CMD=", or whitespace makes the signal invalid.
 * Invalid messages are ignored rather than parsed for instructions.
 */
wake_sms_signal_t wake_sms_classify(const char *sender, const char *body);

#endif /* WAKE_SMS_H */
