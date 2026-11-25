/*
 * Host demonstration with an in-process fake platform.
 *
 * This is NOT production TLS/flash code.  It shows the control flow and the
 * HAL/channel boundaries.  A real port replaces the POSIX timing functions and
 * fake channel with modem AT/socket, mTLS, secure storage, and vehicle bus
 * implementations.
 */
#include "wake_agent.h"
#include "wake_sms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t demo_now_ms(void *user)
{
    struct timespec ts;
    (void)user;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void demo_sleep_ms(void *user, uint32_t duration_ms)
{
    struct timespec ts;
    (void)user;
    ts.tv_sec = duration_ms / 1000U;
    ts.tv_nsec = (long)(duration_ms % 1000U) * 1000000L;
    nanosleep(&ts, NULL);
}

static uint32_t demo_random(void *user)
{
    (void)user;
    return (uint32_t)rand();
}

static uint8_t demo_battery(void *user)
{
    (void)user;
    return 87U;
}

static wake_status_t demo_radio_attach(void *user, uint32_t timeout_ms)
{
    (void)user;
    printf("[hal] attach LTE/Cat-M data path (timeout=%ums)\n", timeout_ms);
    return WAKE_OK;
}

static wake_status_t demo_store_load(void *user, wake_store_state_t *state)
{
    wake_store_state_t *persistent = user;
    *state = *persistent;
    return WAKE_OK;
}

static wake_status_t demo_store_save(void *user,
                                     const wake_store_state_t *state)
{
    wake_store_state_t *persistent = user;
    *persistent = *state;
    printf("[hal] durable cursor=%llu wake_count=%u\n",
           (unsigned long long)state->cursor, state->wake_count);
    return WAKE_OK;
}

/* Test double: production calls into Secure Element/HSM or verified Ed25519. */
static wake_status_t demo_verify_command(void *user,
                                         const wake_command_t *cmd)
{
    (void)user;
    printf("[hal] verify platform signature for %s seq=%llu\n",
           cmd->id, (unsigned long long)cmd->seq);
    return WAKE_OK;
}

static bool demo_safety_allowed(void *user, const wake_command_t *cmd)
{
    (void)user;
    printf("[hal] safety interlock for %s: parked/locked policy allows\n",
           wake_command_type_str(cmd->type));
    return true;
}

static wake_status_t demo_execute(void *user,
                                  const wake_command_t *cmd,
                                  wake_ack_t *ack)
{
    (void)user;
    printf("[vehicle] EXECUTE %s id=%s value=%d\n",
           wake_command_type_str(cmd->type), cmd->id, cmd->value);
    if (ack != NULL) {
        ack->vendor_code = 0;
        ack->execution_ms = 35U;
    }
    return WAKE_OK;
}

static void demo_power_hold(void *user, bool hold)
{
    (void)user;
    printf("[pm] application power %s\n", hold ? "HELD" : "RELEASED");
}

static void demo_schedule(void *user, uint32_t delay_ms)
{
    (void)user;
    printf("[pm] next low-power wake in %u ms\n", delay_ms);
}

typedef struct demo_platform {
    int pull_calls;
    int ack_calls;
} demo_platform_t;

static wake_status_t demo_pull(void *user,
                               const wake_pull_request_t *request,
                               wake_pull_response_t *response,
                               uint32_t timeout_ms)
{
    demo_platform_t *platform = user;
    uint64_t wall_now = (uint64_t)time(NULL) * 1000ULL;

    (void)timeout_ms;
    ++platform->pull_calls;
    printf("[tls] POST /v1/devices/%s/pull cursor=%llu token=%s batt=%u\n",
           request->device_id, (unsigned long long)request->cursor,
           request->wake_token, request->battery_pct);

    memset(response, 0, sizeof(*response));
    response->server_time_ms = wall_now;

    if (platform->pull_calls == 1) {
        printf("[platform] 503 capacity protection; Retry-After=200ms\n");
        response->retry_after_ms = 200U;
        return WAKE_E_OVERLOADED;
    }

    response->command_count = 1U;
    response->next_pull_after_ms = 3600000U;

    wake_command_t *cmd = &response->commands[0];
    cmd->seq = request->cursor + 1ULL;
    snprintf(cmd->id, sizeof(cmd->id), "remote-ac-0001");
    cmd->type = WAKE_CMD_CLIMATE_ON;
    cmd->value = 22;
    cmd->issued_at_ms = wall_now;
    cmd->expires_at_ms = wall_now + 60000ULL;
    cmd->auth_tag_len = 0;
    return WAKE_OK;
}

static wake_status_t demo_ack(void *user,
                              const wake_ack_t *ack,
                              uint32_t timeout_ms)
{
    demo_platform_t *platform = user;
    (void)timeout_ms;
    ++platform->ack_calls;
    printf("[tls] ACK %s seq=%llu result=%s\n",
           ack->id, (unsigned long long)ack->seq,
           wake_status_str(ack->status));
    return WAKE_OK;
}

int main(void)
{
    const char *malicious_sms = "WAKE1 abc;CMD=DOOR_UNLOCK";
    const char *wake_sms = "WAKE1 demo-wake-0001";

    srand((unsigned)time(NULL));

    wake_sms_signal_t bad = wake_sms_classify("+1555-FAKE-NUMBER",
                                              malicious_sms);
    if (bad.classification != WAKE_SMS_WAKE_SIGNAL) {
        printf("SMS rejected as a wake signal; its text is never executed.\n");
    }

    wake_sms_signal_t wake = wake_sms_classify("+1555-PLATFORM-ID", wake_sms);
    if (wake.classification != WAKE_SMS_WAKE_SIGNAL) {
        fprintf(stderr, "demo wake SMS was not accepted\n");
        return 1;
    }
    printf("SMS accepted solely as wake token: %s\n", wake.token);

    wake_config_t cfg;
    wake_config_default(&cfg);
    cfg.device_id = "VIN-DEMO-0001";
    cfg.hard_window_ms = 8000U;
    cfg.pre_connect_jitter_ms = 200U;
    cfg.min_operation_ms = 100U;
    cfg.duplicate_window_ms = 20000U;

    static wake_store_state_t persistent_state;
    demo_platform_t platform;
    memset(&platform, 0, sizeof(platform));

    wake_hal_t hal = {
        .user = &persistent_state,
        .now_ms = demo_now_ms,
        .sleep_ms = demo_sleep_ms,
        .random_u32 = demo_random,
        .battery_pct = demo_battery,
        .radio_attach = demo_radio_attach,
        .store_load = demo_store_load,
        .store_save = demo_store_save,
        .verify_command = demo_verify_command,
        .safety_allowed = demo_safety_allowed,
        .execute_command = demo_execute,
        .power_hold = demo_power_hold,
        .schedule_next_wake = demo_schedule
    };

    wake_secure_channel_t channel = {
        .user = &platform,
        .pull = demo_pull,
        .ack = demo_ack
    };

    wake_report_t report = wake_run(&cfg, &hal, &channel, wake.token);
    printf("\nresult=%s attempts=%u commands=%u succeeded=%u elapsed=%ums "
           "next=%ums\n",
           wake_run_outcome_str(report.outcome), report.attempts,
           report.commands_received, report.commands_succeeded,
           report.elapsed_ms, report.next_wake_delay_ms);

    return report.outcome == WAKE_RUN_OK ? 0 : 1;
}
