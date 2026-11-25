#include "wake_agent.h"
#include "wake_policy.h"
#include "wake_sms.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

typedef struct mock {
    uint64_t now;
    uint32_t rand_value;

    wake_store_state_t store;
    wake_status_t store_load_status;
    wake_status_t store_save_status;
    int save_calls;

    uint8_t battery;
    wake_status_t radio_status;
    int radio_calls;

    wake_status_t pull_status;
    wake_status_t pull_statuses[16];
    size_t pull_status_count;
    int pull_calls;
    uint32_t last_pull_timeout;
    wake_pull_request_t last_request;
    wake_pull_response_t response;

    wake_status_t ack_status;
    int ack_calls;
    wake_ack_t last_ack;

    wake_status_t execute_status;
    int execute_calls;
    wake_command_t last_command;

    wake_status_t verify_status;
    int verify_calls;
    bool safety_allowed;

    bool power_held;
    uint32_t scheduled_delay;
    int schedule_calls;

    uint32_t last_sleep;
    uint32_t sleep_durations[64];
    int sleep_calls;
} mock_t;

static uint64_t mock_now(void *user)
{
    mock_t *m = user;
    return m->now;
}

static void mock_sleep(void *user, uint32_t duration_ms)
{
    mock_t *m = user;
    m->last_sleep = duration_ms;
    if (m->sleep_calls < (int)(sizeof(m->sleep_durations) /
                               sizeof(m->sleep_durations[0]))) {
        m->sleep_durations[m->sleep_calls++] = duration_ms;
    }
    m->now += duration_ms;
}

static uint32_t mock_random(void *user)
{
    mock_t *m = user;
    return m->rand_value;
}

static uint8_t mock_battery(void *user)
{
    mock_t *m = user;
    return m->battery;
}

static wake_status_t mock_radio(void *user, uint32_t timeout_ms)
{
    mock_t *m = user;
    (void)timeout_ms;
    ++m->radio_calls;
    return m->radio_status;
}

static wake_status_t mock_store_load(void *user, wake_store_state_t *state)
{
    mock_t *m = user;
    if (m->store_load_status != WAKE_OK) {
        return m->store_load_status;
    }
    *state = m->store;
    return WAKE_OK;
}

static wake_status_t mock_store_save(void *user,
                                     const wake_store_state_t *state)
{
    mock_t *m = user;
    ++m->save_calls;
    if (m->store_save_status != WAKE_OK) {
        return m->store_save_status;
    }
    m->store = *state;
    return WAKE_OK;
}

static wake_status_t mock_verify(void *user, const wake_command_t *cmd)
{
    mock_t *m = user;
    (void)cmd;
    ++m->verify_calls;
    return m->verify_status;
}

static bool mock_safety(void *user, const wake_command_t *cmd)
{
    mock_t *m = user;
    (void)cmd;
    return m->safety_allowed;
}

static wake_status_t mock_execute(void *user,
                                  const wake_command_t *cmd,
                                  wake_ack_t *ack)
{
    mock_t *m = user;
    ++m->execute_calls;
    m->last_command = *cmd;
    if (ack != NULL) {
        ack->vendor_code = 0;
        ack->execution_ms = 12;
    }
    return m->execute_status;
}

static void mock_power_hold(void *user, bool hold)
{
    mock_t *m = user;
    m->power_held = hold;
}

static void mock_schedule(void *user, uint32_t delay_ms)
{
    mock_t *m = user;
    ++m->schedule_calls;
    m->scheduled_delay = delay_ms;
}

static wake_status_t mock_pull(void *user,
                               const wake_pull_request_t *request,
                               wake_pull_response_t *response,
                               uint32_t timeout_ms)
{
    mock_t *m = user;
    wake_status_t status = m->pull_status;

    ++m->pull_calls;
    m->last_request = *request;
    m->last_pull_timeout = timeout_ms;
    *response = m->response;

    if (m->pull_status_count > 0U) {
        size_t index = (size_t)m->pull_calls - 1U;
        if (index < m->pull_status_count) {
            status = m->pull_statuses[index];
        }
    }
    return status;
}

static wake_status_t mock_ack(void *user,
                              const wake_ack_t *ack,
                              uint32_t timeout_ms)
{
    mock_t *m = user;
    (void)timeout_ms;
    ++m->ack_calls;
    m->last_ack = *ack;
    return m->ack_status;
}

static void mock_init(mock_t *m)
{
    memset(m, 0, sizeof(*m));
    m->now = 1000U;
    m->rand_value = 0U;
    m->battery = 100U;
    m->store_load_status = WAKE_OK;
    m->store_save_status = WAKE_OK;
    m->radio_status = WAKE_OK;
    m->pull_status = WAKE_OK;
    m->ack_status = WAKE_OK;
    m->execute_status = WAKE_OK;
    m->verify_status = WAKE_OK;
    m->safety_allowed = true;
}

static wake_config_t test_config(void)
{
    wake_config_t cfg;
    wake_config_default(&cfg);
    cfg.device_id = "VIN-TEST-0001";
    cfg.hard_window_ms = 10000U;
    cfg.pre_connect_jitter_ms = 0U;
    cfg.min_operation_ms = 100U;
    cfg.initial_backoff_ms = 100U;
    cfg.max_backoff_ms = 1000U;
    cfg.max_attempts = 5U;
    cfg.routine_checkin_ms = 60000U;
    cfg.duplicate_window_ms = 20000U;
    return cfg;
}

static wake_hal_t make_hal(mock_t *m)
{
    wake_hal_t hal;
    memset(&hal, 0, sizeof(hal));
    hal.user = m;
    hal.now_ms = mock_now;
    hal.sleep_ms = mock_sleep;
    hal.random_u32 = mock_random;
    hal.battery_pct = mock_battery;
    hal.radio_attach = mock_radio;
    hal.store_load = mock_store_load;
    hal.store_save = mock_store_save;
    hal.verify_command = mock_verify;
    hal.safety_allowed = mock_safety;
    hal.execute_command = mock_execute;
    hal.power_hold = mock_power_hold;
    hal.schedule_next_wake = mock_schedule;
    return hal;
}

static wake_secure_channel_t make_channel(mock_t *m)
{
    wake_secure_channel_t channel;
    memset(&channel, 0, sizeof(channel));
    channel.user = m;
    channel.pull = mock_pull;
    channel.ack = mock_ack;
    return channel;
}

static wake_command_t make_command(uint64_t seq)
{
    wake_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.seq = seq;
    snprintf(cmd.id, sizeof(cmd.id), "cmd-%" PRIu64, seq);
    cmd.type = WAKE_CMD_CLIMATE_ON;
    cmd.value = 22;
    cmd.issued_at_ms = 90000U;
    cmd.expires_at_ms = 160000U;
    return cmd;
}

static void test_sms_is_only_wake_signal(void)
{
    wake_sms_signal_t ok = wake_sms_classify("+155500000000",
                                             "WAKE1 abc-123_X\r\n");
    CHECK(ok.classification == WAKE_SMS_WAKE_SIGNAL);
    CHECK(strcmp(ok.token, "abc-123_X") == 0);

    /* Sender is spoofed, but even a platform-looking sender grants nothing. */
    wake_sms_signal_t spoofed = wake_sms_classify("PLATFORM",
                                                  "WAKE1 wake-token");
    CHECK(spoofed.classification == WAKE_SMS_WAKE_SIGNAL);

    wake_sms_signal_t injected = wake_sms_classify(
        "+155500000000", "WAKE1 tok;CMD=DOOR_UNLOCK");
    CHECK(injected.classification == WAKE_SMS_IGNORED);

    wake_sms_signal_t old_exploit = wake_sms_classify(
        "+155500000000", "CMD=DOOR_UNLOCK SIGN=forged");
    CHECK(old_exploit.classification == WAKE_SMS_IGNORED);

    wake_sms_signal_t extra_field = wake_sms_classify(
        NULL, "WAKE1 tok CMD:UNLOCK");
    CHECK(extra_field.classification == WAKE_SMS_IGNORED);
}

static void test_policy_bounds(void)
{
    CHECK(wake_jitter_delay_ms(0U, 1000U) == 0U);
    CHECK(wake_jitter_delay_ms(UINT32_MAX, 1000U) <= 1000U);

    CHECK(wake_backoff_delay_ms(0U, 100U, 1000U, 0U, 0U) == 0U);
    CHECK(wake_backoff_delay_ms(0U, 100U, 1000U, UINT32_MAX, 0U) <= 100U);
    CHECK(wake_backoff_delay_ms(99U, 100U, 1000U, UINT32_MAX, 0U) <= 1000U);

    uint32_t retry_after = wake_backoff_delay_ms(
        0U, 100U, 1000U, 0U, 2000U);
    CHECK(retry_after >= 2000U);
    CHECK(retry_after <= 3001U);

    CHECK(wake_remaining_ms(1000U, 5000U, 3000U) == 3000U);
    CHECK(wake_remaining_ms(1000U, 5000U, 9000U) == 0U);
    CHECK(wake_policy_can_start(100U, 100U));
    CHECK(!wake_policy_can_start(99U, 100U));
    CHECK(wake_policy_delay_fits(400U, 500U, 100U));
    CHECK(!wake_policy_delay_fits(401U, 500U, 100U));
}

static void test_success_pulls_executes_and_acks(void)
{
    mock_t m;
    mock_init(&m);
    m.response.server_time_ms = 100000U;
    m.response.command_count = 1U;
    m.response.commands[0] = make_command(1U);

    wake_config_t cfg = test_config();
    wake_hal_t hal = make_hal(&m);
    wake_secure_channel_t channel = make_channel(&m);
    wake_report_t r = wake_run(&cfg, &hal, &channel, "wake-1");

    CHECK(r.outcome == WAKE_RUN_OK);
    CHECK(r.attempts == 1U);
    CHECK(r.commands_received == 1U);
    CHECK(r.commands_succeeded == 1U);
    CHECK(r.acks_succeeded == 1U);
    CHECK(m.pull_calls == 1);
    CHECK(m.execute_calls == 1);
    CHECK(m.ack_calls == 1);
    CHECK(m.verify_calls == 1);
    CHECK(m.store.cursor == 1U);
    CHECK(m.power_held == false);
    CHECK(m.scheduled_delay == 60000U);
    CHECK(strcmp(m.last_request.wake_token, "wake-1") == 0);
    CHECK(m.last_request.cursor == 0U);
    CHECK(strcmp(m.last_ack.id, "cmd-1") == 0);
    CHECK(m.last_ack.status == WAKE_OK);
}

static void test_duplicate_sms_is_coalesced_only_after_success(void)
{
    mock_t m;
    mock_init(&m);

    wake_config_t cfg = test_config();
    wake_hal_t hal = make_hal(&m);
    wake_secure_channel_t channel = make_channel(&m);

    wake_report_t first = wake_run(&cfg, &hal, &channel, "same-token");
    CHECK(first.outcome == WAKE_RUN_OK_NO_WORK);
    CHECK(m.pull_calls == 1);

    wake_report_t duplicate = wake_run(&cfg, &hal, &channel, "same-token");
    CHECK(duplicate.outcome == WAKE_RUN_COALESCED);
    CHECK(m.pull_calls == 1);

    wake_report_t next = wake_run(&cfg, &hal, &channel, "different-token");
    CHECK(next.outcome == WAKE_RUN_OK_NO_WORK);
    CHECK(m.pull_calls == 2);
}

static void test_wake_flood_rate_limit_is_persistent(void)
{
    mock_t m;
    mock_init(&m);

    wake_config_t cfg = test_config();
    cfg.max_wakes_per_window = 2U;
    cfg.wake_rate_window_ms = 60000U;
    wake_hal_t hal = make_hal(&m);
    wake_secure_channel_t channel = make_channel(&m);

    wake_report_t first = wake_run(&cfg, &hal, &channel, "flood-1");
    wake_report_t second = wake_run(&cfg, &hal, &channel, "flood-2");
    wake_report_t blocked = wake_run(&cfg, &hal, &channel, "flood-3");

    CHECK(first.outcome == WAKE_RUN_OK_NO_WORK);
    CHECK(second.outcome == WAKE_RUN_OK_NO_WORK);
    CHECK(blocked.outcome == WAKE_RUN_RATE_LIMITED);
    CHECK(m.pull_calls == 2);
    CHECK(m.store.rate_window_count == 2U);
    CHECK(blocked.next_wake_delay_ms == 60000U);
}

static void test_retry_then_success(void)
{
    mock_t m;
    mock_init(&m);
    m.rand_value = 0U;
    m.pull_statuses[0] = WAKE_E_TIMEOUT;
    m.pull_statuses[1] = WAKE_E_OVERLOADED;
    m.pull_statuses[2] = WAKE_OK;
    m.pull_status_count = 3U;
    m.response.retry_after_ms = 100U;

    wake_config_t cfg = test_config();
    wake_hal_t hal = make_hal(&m);
    wake_secure_channel_t channel = make_channel(&m);
    wake_report_t r = wake_run(&cfg, &hal, &channel, "retry");

    CHECK(r.outcome == WAKE_RUN_OK_NO_WORK);
    CHECK(r.attempts == 3U);
    CHECK(m.pull_calls == 3);
    CHECK(m.sleep_calls >= 2);
}

static void test_deadline_caps_retry_sleep(void)
{
    mock_t m;
    mock_init(&m);
    /* 500 mod (500 + 1) is the maximum full-jitter value. */
    m.rand_value = 500U;
    m.pull_status = WAKE_E_TIMEOUT;

    wake_config_t cfg = test_config();
    cfg.hard_window_ms = 1000U;
    cfg.min_operation_ms = 200U;
    cfg.initial_backoff_ms = 500U;
    cfg.max_backoff_ms = 500U;
    cfg.max_attempts = 0U;

    wake_hal_t hal = make_hal(&m);
    wake_secure_channel_t channel = make_channel(&m);
    wake_report_t r = wake_run(&cfg, &hal, &channel, "deadline");

    CHECK(r.outcome == WAKE_RUN_DEADLINE);
    CHECK(r.attempts == 2U);
    CHECK(m.pull_calls == 2);
    CHECK(r.next_wake_delay_ms == 500U);
    CHECK(m.power_held == false);
}

static void test_retry_after_is_honored(void)
{
    mock_t m;
    mock_init(&m);
    m.rand_value = 0U;
    m.pull_statuses[0] = WAKE_E_OVERLOADED;
    m.pull_statuses[1] = WAKE_OK;
    m.pull_status_count = 2U;
    m.response.retry_after_ms = 2000U;

    wake_config_t cfg = test_config();
    wake_hal_t hal = make_hal(&m);
    wake_secure_channel_t channel = make_channel(&m);
    wake_report_t r = wake_run(&cfg, &hal, &channel, "overload");

    CHECK(r.outcome == WAKE_RUN_OK_NO_WORK);
    CHECK(r.attempts == 2U);
    CHECK(m.sleep_calls >= 1);
    bool saw_retry_after_floor = false;
    for (int i = 0; i < m.sleep_calls; ++i) {
        if (m.sleep_durations[i] >= 2000U) {
            saw_retry_after_floor = true;
        }
    }
    CHECK(saw_retry_after_floor);
    CHECK(r.elapsed_ms >= 2000U);
}

static void test_old_cursor_command_is_not_executed(void)
{
    mock_t m;
    mock_init(&m);
    m.store.cursor = 5U;
    m.response.server_time_ms = 100000U;
    m.response.command_count = 1U;
    m.response.commands[0] = make_command(5U);

    wake_config_t cfg = test_config();
    wake_hal_t hal = make_hal(&m);
    wake_secure_channel_t channel = make_channel(&m);
    wake_report_t r = wake_run(&cfg, &hal, &channel, "cursor");

    CHECK(r.outcome == WAKE_RUN_OK);
    CHECK(m.execute_calls == 0);
    CHECK(m.ack_calls == 0);
    CHECK(m.store.cursor == 5U);
}

static void test_expired_command_is_rejected_and_advanced(void)
{
    mock_t m;
    mock_init(&m);
    m.response.server_time_ms = 200000U;
    m.response.command_count = 1U;
    m.response.commands[0] = make_command(1U);

    wake_config_t cfg = test_config();
    wake_hal_t hal = make_hal(&m);
    wake_secure_channel_t channel = make_channel(&m);
    wake_report_t r = wake_run(&cfg, &hal, &channel, "expired");

    CHECK(r.outcome == WAKE_RUN_OK);
    CHECK(m.execute_calls == 0);
    CHECK(m.ack_calls == 1);
    CHECK(m.last_ack.status == WAKE_E_INVALID);
    CHECK(m.store.cursor == 1U);
}

static void test_bad_signature_is_rejected(void)
{
    mock_t m;
    mock_init(&m);
    m.verify_status = WAKE_E_AUTH;
    m.response.server_time_ms = 100000U;
    m.response.command_count = 1U;
    m.response.commands[0] = make_command(1U);

    wake_config_t cfg = test_config();
    wake_hal_t hal = make_hal(&m);
    wake_secure_channel_t channel = make_channel(&m);
    wake_report_t r = wake_run(&cfg, &hal, &channel, "sig");

    CHECK(m.execute_calls == 0);
    CHECK(m.last_ack.status == WAKE_E_AUTH);
    CHECK(m.store.cursor == 1U);
    CHECK(r.outcome == WAKE_RUN_OK);
}

static void test_safety_interlock_blocks_command(void)
{
    mock_t m;
    mock_init(&m);
    m.safety_allowed = false;
    m.response.server_time_ms = 100000U;
    m.response.command_count = 1U;
    m.response.commands[0] = make_command(1U);

    wake_config_t cfg = test_config();
    wake_hal_t hal = make_hal(&m);
    wake_secure_channel_t channel = make_channel(&m);
    wake_report_t r = wake_run(&cfg, &hal, &channel, "unsafe");

    CHECK(m.execute_calls == 0);
    CHECK(m.last_ack.status == WAKE_E_STATE);
    CHECK(m.store.cursor == 1U);
    CHECK(r.outcome == WAKE_RUN_OK);
}

static void test_transient_execution_failure_keeps_cursor(void)
{
    mock_t m;
    mock_init(&m);
    m.rand_value = UINT32_MAX;
    m.execute_status = WAKE_E_TIMEOUT;
    m.response.server_time_ms = 100000U;
    m.response.command_count = 1U;
    m.response.commands[0] = make_command(1U);

    wake_config_t cfg = test_config();
    cfg.hard_window_ms = 1000U;
    cfg.min_operation_ms = 200U;
    cfg.initial_backoff_ms = 100U;
    cfg.max_backoff_ms = 1000U;

    wake_hal_t hal = make_hal(&m);
    wake_secure_channel_t channel = make_channel(&m);
    wake_report_t r = wake_run(&cfg, &hal, &channel, "busy-bus");

    CHECK(r.outcome == WAKE_RUN_DEADLINE);
    CHECK(m.execute_calls > 1);
    CHECK(m.store.cursor == 0U);
    CHECK(m.ack_calls == 1);
    CHECK(m.last_ack.status == WAKE_E_TIMEOUT);
}

static void test_permanent_execution_failure_is_acked(void)
{
    mock_t m;
    mock_init(&m);
    m.execute_status = WAKE_E_UNSUPPORTED;
    m.response.server_time_ms = 100000U;
    m.response.command_count = 1U;
    m.response.commands[0] = make_command(1U);

    wake_config_t cfg = test_config();
    wake_hal_t hal = make_hal(&m);
    wake_secure_channel_t channel = make_channel(&m);
    wake_report_t r = wake_run(&cfg, &hal, &channel, "unsupported");

    CHECK(r.outcome == WAKE_RUN_OK);
    CHECK(m.store.cursor == 1U);
    CHECK(m.ack_calls == 1);
    CHECK(m.last_ack.status == WAKE_E_UNSUPPORTED);
    CHECK(r.commands_succeeded == 0U);
}

static void test_failed_ack_is_replayed_after_restart(void)
{
    mock_t m;
    mock_init(&m);
    m.ack_status = WAKE_E_FATAL;
    m.response.server_time_ms = 100000U;
    m.response.command_count = 1U;
    m.response.commands[0] = make_command(1U);

    wake_config_t cfg = test_config();
    wake_hal_t hal = make_hal(&m);
    wake_secure_channel_t channel = make_channel(&m);

    wake_report_t first = wake_run(&cfg, &hal, &channel, "ack-loss");
    CHECK(first.outcome == WAKE_RUN_OK);
    CHECK(m.store.cursor == 1U);
    CHECK(m.store.pending_ack_seq == 1U);
    CHECK(m.store.pending_ack_status == WAKE_OK);

    /* Next wake flushes the durable pending ACK before pulling. */
    m.ack_status = WAKE_OK;
    wake_report_t second = wake_run(&cfg, &hal, &channel, "ack-retry");

    CHECK(second.outcome == WAKE_RUN_OK);
    CHECK(second.acks_succeeded == 1U);
    CHECK(m.last_ack.seq == 1U);
    CHECK(m.last_ack.status == WAKE_OK);
    CHECK(m.store.pending_ack_seq == 0U);
    CHECK(m.execute_calls == 1);
}

static void test_platform_auth_failure_backs_off(void)
{
    mock_t m;
    mock_init(&m);
    m.pull_status = WAKE_E_AUTH;

    wake_config_t cfg = test_config();
    wake_hal_t hal = make_hal(&m);
    wake_secure_channel_t channel = make_channel(&m);
    wake_report_t r = wake_run(&cfg, &hal, &channel, "auth");

    CHECK(r.outcome == WAKE_RUN_AUTH_FATAL);
    CHECK(r.attempts == 1U);
    CHECK(m.pull_calls == 1);
    CHECK(r.next_wake_delay_ms == 300000U);
    CHECK(m.scheduled_delay == 300000U);
}

int main(void)
{
    test_sms_is_only_wake_signal();
    test_policy_bounds();
    test_success_pulls_executes_and_acks();
    test_duplicate_sms_is_coalesced_only_after_success();
    test_wake_flood_rate_limit_is_persistent();
    test_retry_then_success();
    test_deadline_caps_retry_sleep();
    test_retry_after_is_honored();
    test_old_cursor_command_is_not_executed();
    test_expired_command_is_rejected_and_advanced();
    test_bad_signature_is_rejected();
    test_safety_interlock_blocks_command();
    test_transient_execution_failure_keeps_cursor();
    test_permanent_execution_failure_is_acked();
    test_failed_ack_is_replayed_after_restart();
    test_platform_auth_failure_backs_off();

    if (g_failures != 0) {
        printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all wake-agent tests passed\n");
    return 0;
}
