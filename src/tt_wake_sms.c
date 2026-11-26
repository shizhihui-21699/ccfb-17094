#include "tt/wake_sms.h"

#include <string.h>

#define WAKE_MAGIC_LEN (sizeof(TT_WAKE_SMS_MAGIC) - 1U)

static int hex_value(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

tt_result_t tt_wake_sms_parse(const uint8_t *body,
                              size_t body_len,
                              tt_wake_sms_t *out)
{
    size_t i;

    if (body == NULL || out == NULL || body_len == 0U) {
        return TT_E_ARG;
    }

    memset(out, 0, sizeof(*out));

    if (body_len < WAKE_MAGIC_LEN ||
        memcmp(body, TT_WAKE_SMS_MAGIC, WAKE_MAGIC_LEN) != 0) {
        return TT_E_IGNORED;
    }

    if (body_len == WAKE_MAGIC_LEN) {
        return TT_OK;
    }

    /* A wake body is either the bare magic or magic + one space + 16 hex. */
    if (body[WAKE_MAGIC_LEN] != ' ') {
        return TT_E_FORMAT;
    }

    if (body_len != WAKE_MAGIC_LEN + 1U + TT_WAKE_NONCE_LEN) {
        return TT_E_FORMAT;
    }

    for (i = 0; i < TT_WAKE_NONCE_LEN; ++i) {
        uint8_t c = body[WAKE_MAGIC_LEN + 1U + i];
        int v = hex_value(c);
        if (v < 0) {
            return TT_E_FORMAT;
        }
        out->nonce[i] = (char)(c >= 'A' && c <= 'F' ? (uint8_t)c + 32U : c);
    }
    out->nonce[TT_WAKE_NONCE_LEN] = '\0';
    return TT_OK;
}

void tt_wake_limiter_init(tt_wake_limiter_t *limiter,
                          uint32_t max_wakes_per_window,
                          uint64_t window_ms)
{
    if (limiter == NULL) {
        return;
    }
    limiter->max_wakes_per_window = max_wakes_per_window;
    limiter->window_ms = window_ms;
    limiter->window_started_ms = 0U;
    limiter->wakes_in_window = 0U;
}

bool tt_wake_limiter_allow(tt_wake_limiter_t *limiter, uint64_t now_ms)
{
    if (limiter == NULL || limiter->max_wakes_per_window == 0U ||
        limiter->window_ms == 0U) {
        return false;
    }

    if (limiter->wakes_in_window == 0U) {
        limiter->window_started_ms = now_ms;
    } else if (now_ms < limiter->window_started_ms ||
               now_ms - limiter->window_started_ms >= limiter->window_ms) {
        limiter->window_started_ms = now_ms;
        limiter->wakes_in_window = 0U;
    }

    if (limiter->wakes_in_window >= limiter->max_wakes_per_window) {
        return false;
    }

    limiter->wakes_in_window++;
    return true;
}
