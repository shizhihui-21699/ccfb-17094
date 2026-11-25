#include "wake_policy.h"

#include <stdint.h>

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static uint32_t max_u32(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

uint32_t wake_jitter_delay_ms(uint32_t random_value, uint32_t max_delay_ms)
{
    if (max_delay_ms == 0U) {
        return 0U;
    }
    return (uint32_t)(((uint64_t)random_value) % ((uint64_t)max_delay_ms + 1U));
}

uint32_t wake_backoff_delay_ms(uint32_t attempt,
                               uint32_t initial_ms,
                               uint32_t max_ms,
                               uint32_t random_value,
                               uint32_t server_retry_after_ms)
{
    uint64_t ceiling = initial_ms;
    uint64_t jittered;

    if (max_ms == 0U) {
        return 0U;
    }
    if (ceiling == 0U) {
        ceiling = 1U;
    }

    for (uint32_t i = 0; i < attempt; ++i) {
        if (ceiling >= max_ms / 2U) {
            ceiling = max_ms;
            break;
        }
        ceiling *= 2U;
    }
    ceiling = min_u32((uint32_t)ceiling, max_ms);

    if (server_retry_after_ms == 0U) {
        /* Full jitter: 0..ceiling. */
        jittered = ((uint64_t)random_value % (ceiling + 1U));
        return (uint32_t)jittered;
    }

    /* Honor Retry-After as a floor, then spread reconnects by up to 50%. */
    ceiling = max_u32((uint32_t)ceiling, server_retry_after_ms);
    jittered = ceiling +
               ((uint64_t)random_value % (ceiling / 2U + 1U));
    if (jittered > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)jittered;
}

uint32_t wake_remaining_ms(uint64_t start_ms,
                           uint32_t window_ms,
                           uint64_t now_ms)
{
    uint64_t elapsed;

    if (now_ms <= start_ms) {
        return window_ms;
    }

    elapsed = now_ms - start_ms;
    if (elapsed >= window_ms) {
        return 0U;
    }
    return window_ms - (uint32_t)elapsed;
}

bool wake_policy_can_start(uint32_t remaining_ms, uint32_t min_operation_ms)
{
    return remaining_ms >= min_operation_ms;
}

bool wake_policy_delay_fits(uint32_t delay_ms,
                            uint32_t remaining_ms,
                            uint32_t min_operation_ms)
{
    uint64_t required = (uint64_t)delay_ms + min_operation_ms;
    return required <= remaining_ms;
}
