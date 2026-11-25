/*
 * Discrete-event fleet reconnect simulation.
 *
 * It compares a synchronized SMS blast with platform-paced SMS plus device
 * jitter and deadline-aware Retry-After/backoff.  This is a capacity illustration,
 * not a network emulator.
 */
#include "wake_policy.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define DEVICE_COUNT       20000U
#define SERVER_CAPACITY    1000U   /* accepted pull requests per second */
#define SMS_WINDOW_S       60U
#define PRE_JITTER_S       4U
#define WAKE_WINDOW_S      45U
#define NETWORK_FAIL_PCT   8U
#define HORIZON_S          240U

typedef struct node {
    uint32_t due_s;
    uint32_t deadline_s;
    uint32_t attempts;
    bool done;
    bool expired;
} node_t;

typedef struct stats {
    size_t peak_due;
    uint64_t total_attempts;
    uint64_t network_failures;
    uint64_t overload_responses;
    uint64_t expired;
    uint64_t succeeded;
} stats_t;

static uint32_t next_random(uint64_t *state)
{
    *state = (*state * 6364136223846793005ULL) +
             1442695040888963407ULL;
    return (uint32_t)(*state >> 32U);
}

static uint32_t random_range(uint64_t *state, uint32_t low, uint32_t high)
{
    if (high <= low) {
        return low;
    }
    return low + (next_random(state) % (high - low + 1U));
}

static bool schedule_retry(node_t *node,
                           uint32_t now_s,
                           uint32_t retry_after_ms,
                           uint64_t *rng)
{
    uint32_t delay_ms = wake_backoff_delay_ms(
        node->attempts - 1U, 500U, 8000U, next_random(rng),
        retry_after_ms);
    uint32_t delay_s = delay_ms == 0U ? 1U : (delay_ms + 999U) / 1000U;
    uint32_t remaining_s = node->deadline_s > now_s ?
                            node->deadline_s - now_s : 0U;

    /* Keep at least one second in which the retry can actually execute. */
    if ((uint64_t)delay_s + 1ULL > remaining_s) {
        node->expired = true;
        return false;
    }

    node->due_s = now_s + delay_s;
    return true;
}

static stats_t run_scenario(const char *name,
                            bool paced_sms,
                            uint64_t rng_seed)
{
    node_t *nodes = calloc(DEVICE_COUNT, sizeof(*nodes));
    size_t *due = calloc(DEVICE_COUNT, sizeof(*due));
    stats_t stats = {0};
    uint64_t rng = rng_seed;

    if (nodes == NULL || due == NULL) {
        fprintf(stderr, "out of memory\n");
        free(nodes);
        free(due);
        exit(1);
    }
    for (size_t i = 0; i < DEVICE_COUNT; ++i) {
        uint32_t sms_s = paced_sms ?
                         random_range(&rng, 0U, SMS_WINDOW_S - 1U) : 0U;
        uint32_t jitter_s = paced_sms ?
                            random_range(&rng, 0U, PRE_JITTER_S) : 0U;
        nodes[i].due_s = sms_s + jitter_s;
        nodes[i].deadline_s = sms_s + WAKE_WINDOW_S;
    }

    for (uint32_t second = 0; second < HORIZON_S; ++second) {
        size_t due_count = 0;
        size_t accepted_budget = SERVER_CAPACITY;

        for (size_t i = 0; i < DEVICE_COUNT; ++i) {
            node_t *node = &nodes[i];
            if (node->done || node->expired) {
                continue;
            }
            if (second >= node->deadline_s) {
                node->expired = true;
                ++stats.expired;
                continue;
            }
            if (node->due_s <= second) {
                due[due_count++] = i;
            }
        }

        if (due_count > stats.peak_due) {
            stats.peak_due = due_count;
        }

        for (size_t j = 0; j < due_count; ++j) {
            node_t *node = &nodes[due[j]];
            ++node->attempts;
            ++stats.total_attempts;

            if (next_random(&rng) % 100U < NETWORK_FAIL_PCT) {
                ++stats.network_failures;
                if (!schedule_retry(node, second, 0U, &rng)) {
                    ++stats.expired;
                }
                continue;
            }

            if (accepted_budget > 0U) {
                --accepted_budget;
                node->done = true;
                ++stats.succeeded;
            } else {
                ++stats.overload_responses;
                if (!schedule_retry(node, second, 2000U, &rng)) {
                    ++stats.expired;
                }
            }
        }

        if (stats.succeeded + stats.expired == DEVICE_COUNT) {
            break;
        }
    }

    printf("%-28s peak_due=%5zu/s attempts=%7llu overload=%6llu "
           "net_fail=%5llu expired=%4llu success=%5llu/%u\n",
           name, stats.peak_due,
           (unsigned long long)stats.total_attempts,
           (unsigned long long)stats.overload_responses,
           (unsigned long long)stats.network_failures,
           (unsigned long long)stats.expired,
           (unsigned long long)stats.succeeded, DEVICE_COUNT);

    free(due);
    free(nodes);
    return stats;
}

int main(void)
{
    printf("Simulated fleet: %u devices, gateway capacity %u pulls/s, "
           "wake window %us, network failure %u%%\n\n",
           DEVICE_COUNT, SERVER_CAPACITY, WAKE_WINDOW_S, NETWORK_FAIL_PCT);

    run_scenario("synchronized SMS blast", false, 0x123456789abcdef0ULL);
    run_scenario("paced SMS + jitter", true,  0x123456789abcdef0ULL);

    puts("\nPlatform side must additionally shard connections, autoscale, "
         "shed load with 503/Retry-After, and pace SMS by region/cell/VIN.");
    return 0;
}
