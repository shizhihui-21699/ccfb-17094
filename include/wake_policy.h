#ifndef WAKE_POLICY_H
#define WAKE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Deadline-aware retry and herd-smoothing helpers.  They are pure functions so
 * tests can prove the terminal will not spend its whole wake window asleep.
 */

uint32_t wake_jitter_delay_ms(uint32_t random_value, uint32_t max_delay_ms);

/*
 * Returns a bounded full-jitter exponential backoff.  If the platform supplies
 * Retry-After, that value becomes a minimum and 0..50% jitter is added so a
 * fleet told to "retry in 30 s" does not reconnect synchronously.
 */
uint32_t wake_backoff_delay_ms(uint32_t attempt,
                               uint32_t initial_ms,
                               uint32_t max_ms,
                               uint32_t random_value,
                               uint32_t server_retry_after_ms);

uint32_t wake_remaining_ms(uint64_t start_ms,
                          uint32_t window_ms,
                          uint64_t now_ms);

bool wake_policy_can_start(uint32_t remaining_ms, uint32_t min_operation_ms);

/*
 * True when sleeping @delay_ms still leaves @min_operation_ms for network I/O.
 */
bool wake_policy_delay_fits(uint32_t delay_ms,
                            uint32_t remaining_ms,
                            uint32_t min_operation_ms);

#endif /* WAKE_POLICY_H */
