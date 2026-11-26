#include "tt/retry.h"

#include <string.h>

uint64_t tt_deadline_remaining_ms(const tt_deadline_t *deadline,
                                  uint64_t now_ms)
{
    uint64_t elapsed;

    if (deadline == NULL || deadline->budget_ms == 0U) {
        return 0U;
    }
    if (now_ms <= deadline->start_ms) {
        return deadline->budget_ms;
    }

    elapsed = now_ms - deadline->start_ms;
    if (elapsed >= deadline->budget_ms) {
        return 0U;
    }
    return (uint64_t)deadline->budget_ms - elapsed;
}

bool tt_deadline_has_room(const tt_deadline_t *deadline,
                          uint64_t now_ms,
                          uint32_t operation_reserve_ms)
{
    return tt_deadline_remaining_ms(deadline, now_ms) >=
           (uint64_t)operation_reserve_ms;
}

uint32_t tt_retry_jitter_delay(uint32_t failed_attempts,
                               uint32_t initial_delay_ms,
                               uint32_t max_delay_ms,
                               uint32_t random_bits)
{
    uint64_t ceiling;
    uint32_t i;

    if (initial_delay_ms == 0U) {
        return 0U;
    }
    ceiling = initial_delay_ms;
    if (max_delay_ms < initial_delay_ms) {
        max_delay_ms = initial_delay_ms;
    }

    /* failed_attempts == 1 is the first retry and uses initial_delay_ms. */
    for (i = 1U; i < failed_attempts && ceiling < max_delay_ms; ++i) {
        if (ceiling > (uint64_t)max_delay_ms / 2U) {
            ceiling = max_delay_ms;
        } else {
            ceiling *= 2U;
        }
    }
    if (ceiling > max_delay_ms) {
        ceiling = max_delay_ms;
    }

    /*
     * Full jitter: uniform random delay over [0, ceiling].  This spreads a
     * fleet that received the same wake event much better than fixed
     * exponential back-off.  ceiling == UINT32_MAX means modulus 2^32.
     */
    if (ceiling == UINT32_MAX) {
        return random_bits;
    }
    return random_bits % (uint32_t)(ceiling + 1U);
}

tt_result_t tt_retry_plan(const tt_retry_policy_t *policy,
                          const tt_deadline_t *deadline,
                          uint32_t failed_attempts,
                          uint64_t now_ms,
                          uint32_t random_bits,
                          uint32_t server_retry_after_ms,
                          tt_retry_decision_t *decision)
{
    uint64_t remaining;
    uint64_t wait_budget;
    uint32_t jitter;

    if (policy == NULL || deadline == NULL || decision == NULL ||
        policy->max_attempts == 0U || policy->initial_delay_ms == 0U ||
        policy->max_delay_ms < policy->initial_delay_ms) {
        return TT_E_ARG;
    }

    memset(decision, 0, sizeof(*decision));

    if (failed_attempts >= policy->max_attempts) {
        return TT_OK;
    }

    remaining = tt_deadline_remaining_ms(deadline, now_ms);
    if (remaining < policy->attempt_reserve_ms) {
        return TT_OK;
    }
    wait_budget = remaining - policy->attempt_reserve_ms;

    if (server_retry_after_ms != 0U) {
        if ((uint64_t)server_retry_after_ms > wait_budget) {
            /* Honour the platform's fleet-wide spreading hint next wake. */
            decision->should_retry = false;
            decision->off_session_wake_ms = server_retry_after_ms;
            return TT_OK;
        }
        decision->should_retry = true;
        decision->delay_ms = server_retry_after_ms;
        return TT_OK;
    }

    jitter = tt_retry_jitter_delay(failed_attempts,
                                   policy->initial_delay_ms,
                                   policy->max_delay_ms,
                                   random_bits);
    if ((uint64_t)jitter > wait_budget) {
        jitter = (uint32_t)wait_budget;
    }

    decision->should_retry = true;
    decision->delay_ms = jitter;
    return TT_OK;
}
