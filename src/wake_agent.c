#include "wake_agent.h"
#include "wake_policy.h"

#include <string.h>

#define WAKE_AUTH_FATAL_RETRY_MS (5U * 60U * 1000U)
#define WAKE_MIN_RETRY_SLEEP_MS  10U

void wake_config_default(wake_config_t *config)
{
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->device_id = NULL;
    config->hard_window_ms = 45U * 1000U;
    config->pre_connect_jitter_ms = 4U * 1000U;
    config->radio_attach_timeout_ms = 12U * 1000U;
    config->pull_timeout_ms = 10U * 1000U;
    config->ack_timeout_ms = 4U * 1000U;
    config->min_operation_ms = 1000U;
    config->initial_backoff_ms = 500U;
    config->max_backoff_ms = 8U * 1000U;
    config->max_attempts = 8U;
    config->duplicate_window_ms = 20U * 1000U;
    config->wake_rate_window_ms = 10U * 60U * 1000U;
    config->max_wakes_per_window = 6U;
    config->routine_checkin_ms = 60U * 60U * 1000U;
    config->min_battery_pct = 5U;
    config->require_app_auth = true;
}

const char *wake_run_outcome_str(wake_run_outcome_t outcome)
{
    switch (outcome) {
    case WAKE_RUN_INVALID_ARGUMENT: return "invalid-argument";
    case WAKE_RUN_OK: return "ok";
    case WAKE_RUN_OK_NO_WORK: return "ok-no-work";
    case WAKE_RUN_COALESCED: return "coalesced";
    case WAKE_RUN_RATE_LIMITED: return "rate-limited";
    case WAKE_RUN_LOW_BATTERY: return "low-battery";
    case WAKE_RUN_DEADLINE: return "deadline";
    case WAKE_RUN_NETWORK_FATAL: return "network-fatal";
    case WAKE_RUN_AUTH_FATAL: return "auth-fatal";
    case WAKE_RUN_PROTOCOL_FATAL: return "protocol-fatal";
    case WAKE_RUN_STORE_FATAL: return "store-fatal";
    }
    return "unknown";
}

typedef struct agent_ctx {
    const wake_config_t *cfg;
    const wake_hal_t *hal;
    wake_secure_channel_t *channel;
    wake_store_state_t *store;
    wake_report_t *report;
    uint64_t start_ms;
} agent_ctx_t;

static uint64_t ctx_now(const agent_ctx_t *ctx)
{
    return ctx->hal->now_ms(ctx->hal->user);
}

static uint32_t ctx_remaining(const agent_ctx_t *ctx)
{
    return wake_remaining_ms(ctx->start_ms,
                             ctx->cfg->hard_window_ms,
                             ctx_now(ctx));
}

static void copy_fixed(char *dst, size_t dst_size, const char *src)
{
    size_t len;

    if (dst_size == 0U) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    len = strnlen(src, dst_size - 1U);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Full jitter may legitimately be zero; enforce a small local yield. */
static uint32_t agent_backoff_delay(agent_ctx_t *ctx,
                                    uint32_t attempt,
                                    uint32_t server_retry_after_ms)
{
    uint32_t delay = wake_backoff_delay_ms(
        attempt, ctx->cfg->initial_backoff_ms, ctx->cfg->max_backoff_ms,
        ctx->hal->random_u32(ctx->hal->user), server_retry_after_ms);

    if (delay < WAKE_MIN_RETRY_SLEEP_MS) {
        delay = WAKE_MIN_RETRY_SLEEP_MS;
    }
    return delay;
}

/*
 * A retry that will happen after a full low-power wake cycle must not be 0..10ms
 * just because full jitter returned zero.  Keep its floor around half of the
 * initial backoff while preserving jitter.
 */
static uint32_t agent_scheduled_retry_delay(agent_ctx_t *ctx,
                                            uint32_t attempt,
                                            uint32_t server_retry_after_ms)
{
    uint32_t delay = wake_backoff_delay_ms(
        attempt, ctx->cfg->initial_backoff_ms, ctx->cfg->max_backoff_ms,
        ctx->hal->random_u32(ctx->hal->user), server_retry_after_ms);

    if (server_retry_after_ms == 0U) {
        uint32_t half = ctx->cfg->initial_backoff_ms / 2U;
        uint32_t floor = half + wake_jitter_delay_ms(
            ctx->hal->random_u32(ctx->hal->user), half);
        if (delay < floor) {
            delay = floor;
        }
    }
    return delay;
}

static wake_run_outcome_t persist_completed(agent_ctx_t *ctx,
                                           const wake_ack_t *ack)
{
    ctx->store->cursor = ack->seq;
    ctx->store->pending_ack_seq = ack->seq;
    ctx->store->pending_ack_status = ack->status;
    ctx->store->pending_ack_vendor_code = ack->vendor_code;
    copy_fixed(ctx->store->pending_ack_id,
               sizeof(ctx->store->pending_ack_id), ack->id);
    ctx->report->cursor = ack->seq;

    if (ctx->hal->store_save(ctx->hal->user, ctx->store) != WAKE_OK) {
        return WAKE_RUN_STORE_FATAL;
    }
    return WAKE_RUN_OK;
}

static void clear_pending_ack(agent_ctx_t *ctx)
{
    ctx->store->pending_ack_seq = 0U;
    ctx->store->pending_ack_status = WAKE_OK;
    ctx->store->pending_ack_vendor_code = 0;
    ctx->store->pending_ack_id[0] = '\0';

    /* If this clear is lost, the next wake harmlessly sends the ACK again. */
    (void)ctx->hal->store_save(ctx->hal->user, ctx->store);
}

static wake_status_t send_ack(agent_ctx_t *ctx, const wake_ack_t *ack)
{
    uint32_t attempt = 0;

    /* An ACK callback is optional: the next pull cursor is cumulative ACK. */
    if (ctx->channel->ack == NULL) {
        return WAKE_OK;
    }

    for (;;) {
        uint32_t remaining = ctx_remaining(ctx);
        uint32_t timeout;
        wake_status_t status;
        uint32_t delay;

        if (!wake_policy_can_start(remaining, ctx->cfg->min_operation_ms)) {
            return WAKE_E_TIMEOUT;
        }

        timeout = ctx->cfg->ack_timeout_ms < remaining ?
                  ctx->cfg->ack_timeout_ms : remaining;
        status = ctx->channel->ack(ctx->channel->user, ack, timeout);
        if (status == WAKE_OK) {
            ++ctx->report->acks_succeeded;
            return WAKE_OK;
        }
        if (!wake_status_retryable(status)) {
            return status;
        }

        delay = agent_backoff_delay(ctx, attempt++, 0U);
        remaining = ctx_remaining(ctx);
        if (!wake_policy_delay_fits(delay, remaining,
                                    ctx->cfg->min_operation_ms)) {
            return status;
        }
        ctx->hal->sleep_ms(ctx->hal->user, delay);
    }
}

static wake_run_outcome_t process_response(agent_ctx_t *ctx,
                                           const wake_pull_response_t *response)
{
    uint64_t previous_seq = 0;

    ctx->report->commands_received += (uint32_t)response->command_count;

    for (size_t i = 0; i < response->command_count; ++i) {
        const wake_command_t *cmd = &response->commands[i];
        wake_ack_t ack;
        wake_status_t status;

        if (cmd->seq == 0U ||
            (i > 0U && cmd->seq <= previous_seq)) {
            return WAKE_RUN_PROTOCOL_FATAL;
        }
        previous_seq = cmd->seq;

        /* Already durable: platform redelivery after lost ACK. */
        if (cmd->seq <= ctx->store->cursor) {
            continue;
        }

        if (!wake_policy_can_start(ctx_remaining(ctx),
                                   ctx->cfg->min_operation_ms)) {
            ctx->report->next_wake_delay_ms = ctx->cfg->initial_backoff_ms;
            return WAKE_RUN_DEADLINE;
        }

        status = wake_command_validate(cmd, response->server_time_ms);

        if (status == WAKE_OK && ctx->cfg->require_app_auth) {
            status = ctx->hal->verify_command(ctx->hal->user, cmd);
        }

        if (status == WAKE_OK && ctx->hal->safety_allowed != NULL) {
            if (!ctx->hal->safety_allowed(ctx->hal->user, cmd)) {
                status = WAKE_E_STATE;
            }
        }

        if (status != WAKE_OK) {
            memset(&ack, 0, sizeof(ack));
            ack.seq = cmd->seq;
            copy_fixed(ack.id, sizeof(ack.id), cmd->id);
            ack.status = status;

            /* Reject durably so an invalid command cannot block the queue. */
            wake_run_outcome_t saved = persist_completed(ctx, &ack);
            if (saved != WAKE_RUN_OK) {
                return saved;
            }
            if (send_ack(ctx, &ack) == WAKE_OK) {
                clear_pending_ack(ctx);
            }
            continue;
        }

        memset(&ack, 0, sizeof(ack));
        ack.seq = cmd->seq;
        copy_fixed(ack.id, sizeof(ack.id), cmd->id);

        for (uint32_t exec_attempt = 0;; ++exec_attempt) {
            uint32_t remaining;
            uint32_t delay;

            if (!wake_policy_can_start(ctx_remaining(ctx),
                                       ctx->cfg->min_operation_ms)) {
                status = WAKE_E_TIMEOUT;
                break;
            }

            memset(&ack, 0, sizeof(ack));
            ack.seq = cmd->seq;
            copy_fixed(ack.id, sizeof(ack.id), cmd->id);

            status = ctx->hal->execute_command(ctx->hal->user, cmd, &ack);
            ack.seq = cmd->seq;
            copy_fixed(ack.id, sizeof(ack.id), cmd->id);
            ack.status = status;

            if (status == WAKE_OK || !wake_status_retryable(status)) {
                break;
            }

            remaining = ctx_remaining(ctx);
            delay = agent_backoff_delay(ctx, exec_attempt, 0U);
            if (!wake_policy_delay_fits(delay, remaining,
                                        ctx->cfg->min_operation_ms)) {
                break;
            }
            ctx->hal->sleep_ms(ctx->hal->user, delay);
        }

        if (wake_status_retryable(status)) {
            /*
             * Do not advance cursor: next pull after low-power retry receives
             * the same command.  Handlers must be idempotent.
             */
            ack.status = status;
            (void)send_ack(ctx, &ack);
            ctx->report->last_transport = status;
            ctx->report->next_wake_delay_ms = ctx->cfg->initial_backoff_ms;
            return WAKE_RUN_DEADLINE;
        }

        wake_run_outcome_t saved = persist_completed(ctx, &ack);
        if (saved != WAKE_RUN_OK) {
            return saved;
        }

        if (status == WAKE_OK) {
            ++ctx->report->commands_succeeded;
        }
        if (send_ack(ctx, &ack) == WAKE_OK) {
            clear_pending_ack(ctx);
        }
    }

    return WAKE_RUN_OK;
}

static bool duplicate_wake(const wake_config_t *cfg,
                           const wake_store_state_t *store,
                           const char *token,
                           uint64_t now_ms)
{
    if (token == NULL || token[0] == '\0' || store->last_wake_token[0] == '\0') {
        return false;
    }
    if (strncmp(store->last_wake_token, token,
                sizeof(store->last_wake_token)) != 0) {
        return false;
    }
    if (store->last_wake_mono_ms == 0U ||
        store->last_success_mono_ms < store->last_wake_mono_ms) {
        return false;
    }
    if (now_ms < store->last_wake_mono_ms) {
        return false;
    }
    return now_ms - store->last_wake_mono_ms <= cfg->duplicate_window_ms;
}

wake_report_t wake_run(const wake_config_t *config,
                       const wake_hal_t *hal,
                       wake_secure_channel_t *channel,
                       const char *wake_token)
{
    wake_report_t report;
    wake_config_t cfg;
    wake_store_state_t store;
    agent_ctx_t ctx;
    uint64_t start_ms;
    bool power_held = false;
    bool pending_flush_attempted = false;

    memset(&report, 0, sizeof(report));
    memset(&cfg, 0, sizeof(cfg));
    memset(&store, 0, sizeof(store));

    if (config == NULL || hal == NULL || channel == NULL ||
        config->device_id == NULL || config->device_id[0] == '\0' ||
        hal->now_ms == NULL || hal->sleep_ms == NULL ||
        hal->random_u32 == NULL || hal->store_load == NULL ||
        hal->store_save == NULL || hal->execute_command == NULL ||
        channel->pull == NULL) {
        report.outcome = WAKE_RUN_INVALID_ARGUMENT;
        return report;
    }

    cfg = *config;
    if (cfg.hard_window_ms == 0U) cfg.hard_window_ms = 45U * 1000U;
    if (cfg.radio_attach_timeout_ms == 0U) cfg.radio_attach_timeout_ms = 12000U;
    if (cfg.pull_timeout_ms == 0U) cfg.pull_timeout_ms = 10000U;
    if (cfg.ack_timeout_ms == 0U) cfg.ack_timeout_ms = 4000U;
    if (cfg.min_operation_ms == 0U) cfg.min_operation_ms = 1000U;
    if (cfg.initial_backoff_ms == 0U) cfg.initial_backoff_ms = 500U;
    if (cfg.max_backoff_ms == 0U) cfg.max_backoff_ms = 8000U;
    if (cfg.duplicate_window_ms == 0U) cfg.duplicate_window_ms = 20000U;
    if (cfg.wake_rate_window_ms == 0U) cfg.wake_rate_window_ms = 600000U;
    if (cfg.routine_checkin_ms == 0U) cfg.routine_checkin_ms = 3600000U;

    if (cfg.require_app_auth && hal->verify_command == NULL) {
        report.outcome = WAKE_RUN_INVALID_ARGUMENT;
        return report;
    }

    if (hal->power_hold != NULL) {
        hal->power_hold(hal->user, true);
        power_held = true;
    }

    start_ms = hal->now_ms(hal->user);
    ctx.cfg = &cfg;
    ctx.hal = hal;
    ctx.channel = channel;
    ctx.store = &store;
    ctx.report = &report;
    ctx.start_ms = start_ms;

    if (hal->store_load(hal->user, &store) != WAKE_OK) {
        report.outcome = WAKE_RUN_STORE_FATAL;
        report.next_wake_delay_ms = cfg.routine_checkin_ms;
        goto out;
    }
    report.cursor = store.cursor;

    if (duplicate_wake(&cfg, &store, wake_token, start_ms)) {
        report.outcome = WAKE_RUN_COALESCED;
        report.next_wake_delay_ms = cfg.routine_checkin_ms;
        goto out;
    }

    /* The persistent flood budget applies to SMS-triggered runs.  Locally
     * scheduled RTC retries/routine check-ins call wake_run() with NULL. */
    if (wake_token != NULL && wake_token[0] != '\0' &&
        cfg.max_wakes_per_window != UINT16_MAX) {
        uint32_t max_wakes = cfg.max_wakes_per_window;
        uint64_t window_age;

        if (max_wakes == 0U) {
            max_wakes = 6U;
        }

        if (store.rate_window_start_mono_ms == 0U ||
            start_ms < store.rate_window_start_mono_ms ||
            start_ms - store.rate_window_start_mono_ms >=
                cfg.wake_rate_window_ms) {
            store.rate_window_start_mono_ms = start_ms;
            store.rate_window_count = 0U;
        }

        window_age = start_ms - store.rate_window_start_mono_ms;
        if (store.rate_window_count >= max_wakes) {
            report.outcome = WAKE_RUN_RATE_LIMITED;
            if (window_age < cfg.wake_rate_window_ms) {
                report.next_wake_delay_ms =
                    cfg.wake_rate_window_ms - (uint32_t)window_age;
            } else {
                report.next_wake_delay_ms = cfg.wake_rate_window_ms;
            }
            goto out;
        }
        ++store.rate_window_count;
    }

    store.wake_count++;
    store.last_wake_mono_ms = start_ms;
    copy_fixed(store.last_wake_token, sizeof(store.last_wake_token), wake_token);
    if (hal->store_save(hal->user, &store) != WAKE_OK) {
        report.outcome = WAKE_RUN_STORE_FATAL;
        report.next_wake_delay_ms = cfg.routine_checkin_ms;
        goto out;
    }

    if (hal->battery_pct != NULL) {
        uint8_t battery = hal->battery_pct(hal->user);
        if (battery <= 100U && battery < cfg.min_battery_pct) {
            report.outcome = WAKE_RUN_LOW_BATTERY;
            report.next_wake_delay_ms = cfg.routine_checkin_ms;
            goto out;
        }
    }

    /* Spread mass-SMS reconnects before touching the cellular radio. */
    {
        uint32_t jitter = wake_jitter_delay_ms(
            hal->random_u32(hal->user), cfg.pre_connect_jitter_ms);
        uint32_t remaining = ctx_remaining(&ctx);

        if (jitter > 0U &&
            wake_policy_delay_fits(jitter, remaining, cfg.min_operation_ms)) {
            hal->sleep_ms(hal->user, jitter);
        }
    }

    for (;;) {
        uint32_t remaining;
        uint32_t timeout;
        wake_status_t status;

        remaining = ctx_remaining(&ctx);
        if (!wake_policy_can_start(remaining, cfg.min_operation_ms)) {
            report.outcome = WAKE_RUN_DEADLINE;
            if (report.next_wake_delay_ms == 0U) {
                report.next_wake_delay_ms =
                    agent_scheduled_retry_delay(&ctx, report.attempts, 0U);
            }
            goto out;
        }

        if (cfg.max_attempts != 0U && report.attempts >= cfg.max_attempts) {
            report.outcome = WAKE_RUN_DEADLINE;
            report.next_wake_delay_ms =
                agent_scheduled_retry_delay(&ctx, report.attempts, 0U);
            goto out;
        }

        timeout = cfg.radio_attach_timeout_ms < remaining ?
                  cfg.radio_attach_timeout_ms : remaining;
        if (hal->radio_attach != NULL) {
            status = hal->radio_attach(hal->user, timeout);
            report.last_transport = status;
            if (status != WAKE_OK) {
                ++report.attempts;
                if (wake_status_retryable(status)) {
                    uint32_t delay = agent_backoff_delay(
                        &ctx, report.attempts - 1U, 0U);
                    remaining = ctx_remaining(&ctx);
                    if (wake_policy_delay_fits(delay, remaining,
                                               cfg.min_operation_ms)) {
                        hal->sleep_ms(hal->user, delay);
                        continue;
                    }
                    report.outcome = WAKE_RUN_DEADLINE;
                    report.next_wake_delay_ms = agent_scheduled_retry_delay(
                        &ctx, report.attempts - 1U, 0U);
                    goto out;
                }

                report.outcome = WAKE_RUN_NETWORK_FATAL;
                report.next_wake_delay_ms = cfg.routine_checkin_ms;
                goto out;
            }
        }

        if (!pending_flush_attempted && store.pending_ack_seq != 0U) {
            wake_ack_t pending_ack;

            pending_flush_attempted = true;
            memset(&pending_ack, 0, sizeof(pending_ack));
            pending_ack.seq = store.pending_ack_seq;
            pending_ack.status = store.pending_ack_status;
            pending_ack.vendor_code = store.pending_ack_vendor_code;
            copy_fixed(pending_ack.id, sizeof(pending_ack.id),
                       store.pending_ack_id);

            if (send_ack(&ctx, &pending_ack) == WAKE_OK) {
                clear_pending_ack(&ctx);
            }

            remaining = ctx_remaining(&ctx);
            if (!wake_policy_can_start(remaining, cfg.min_operation_ms)) {
                report.outcome = WAKE_RUN_DEADLINE;
                report.next_wake_delay_ms = cfg.initial_backoff_ms;
                goto out;
            }
        }

        remaining = ctx_remaining(&ctx);
        if (!wake_policy_can_start(remaining, cfg.min_operation_ms)) {
            report.outcome = WAKE_RUN_DEADLINE;
            report.next_wake_delay_ms = cfg.initial_backoff_ms;
            goto out;
        }

        wake_pull_request_t request;
        wake_pull_response_t response;

        memset(&request, 0, sizeof(request));
        memset(&response, 0, sizeof(response));
        copy_fixed(request.device_id, sizeof(request.device_id),
                   cfg.device_id);
        copy_fixed(request.wake_token, sizeof(request.wake_token), wake_token);
        request.cursor = store.cursor;
        request.battery_pct = hal->battery_pct != NULL ?
                              hal->battery_pct(hal->user) : 255U;
        request.rssi_dbm = 0;

        remaining = ctx_remaining(&ctx);
        timeout = cfg.pull_timeout_ms < remaining ?
                  cfg.pull_timeout_ms : remaining;

        status = channel->pull(channel->user, &request, &response, timeout);
        ++report.attempts;
        report.last_transport = status;

        if (status != WAKE_OK) {
            uint32_t delay;

            if (status == WAKE_E_AUTH) {
                report.outcome = WAKE_RUN_AUTH_FATAL;
                report.next_wake_delay_ms = WAKE_AUTH_FATAL_RETRY_MS;
                goto out;
            }

            if (!wake_status_retryable(status)) {
                report.outcome = (status == WAKE_E_INVALID) ?
                                 WAKE_RUN_PROTOCOL_FATAL :
                                 WAKE_RUN_NETWORK_FATAL;
                report.next_wake_delay_ms = cfg.routine_checkin_ms;
                goto out;
            }

            delay = agent_backoff_delay(
                &ctx, report.attempts - 1U, response.retry_after_ms);
            remaining = ctx_remaining(&ctx);
            if (!wake_policy_delay_fits(delay, remaining,
                                        cfg.min_operation_ms)) {
                report.outcome = WAKE_RUN_DEADLINE;
                report.next_wake_delay_ms = agent_scheduled_retry_delay(
                    &ctx, report.attempts - 1U, response.retry_after_ms);
                goto out;
            }

            hal->sleep_ms(hal->user, delay);
            continue;
        }

        if (response.command_count > WAKE_MAX_COMMANDS) {
            report.outcome = WAKE_RUN_PROTOCOL_FATAL;
            report.next_wake_delay_ms = cfg.routine_checkin_ms;
            goto out;
        }

        wake_run_outcome_t processed = process_response(&ctx, &response);
        if (processed == WAKE_RUN_STORE_FATAL ||
            processed == WAKE_RUN_PROTOCOL_FATAL) {
            report.outcome = processed;
            report.next_wake_delay_ms = cfg.routine_checkin_ms;
            goto out;
        }
        if (processed == WAKE_RUN_DEADLINE) {
            report.outcome = WAKE_RUN_DEADLINE;
            if (report.next_wake_delay_ms == 0U) {
                report.next_wake_delay_ms = cfg.initial_backoff_ms;
            }
            goto out;
        }

        store.last_success_mono_ms = ctx_now(&ctx);
        if (hal->store_save(hal->user, &store) != WAKE_OK) {
            report.outcome = WAKE_RUN_STORE_FATAL;
            report.next_wake_delay_ms = cfg.routine_checkin_ms;
            goto out;
        }
        report.cursor = store.cursor;

        if (response.has_more && response.command_count > 0U &&
            wake_policy_can_start(ctx_remaining(&ctx), cfg.min_operation_ms)) {
            continue;
        }

        if (response.has_more && response.command_count > 0U) {
            report.outcome = WAKE_RUN_DEADLINE;
            report.next_wake_delay_ms = cfg.initial_backoff_ms;
            goto out;
        }

        report.outcome = response.command_count == 0U ?
                         WAKE_RUN_OK_NO_WORK : WAKE_RUN_OK;
        report.next_wake_delay_ms = response.next_pull_after_ms != 0U ?
                                    response.next_pull_after_ms :
                                    cfg.routine_checkin_ms;
        goto out;
    }

out:
    if (hal != NULL && hal->now_ms != NULL) {
        uint64_t elapsed = hal->now_ms(hal->user) - start_ms;
        report.elapsed_ms = elapsed > UINT32_MAX ? UINT32_MAX :
                            (uint32_t)elapsed;
    }
    report.cursor = store.cursor;
    if (report.next_wake_delay_ms == 0U) {
        report.next_wake_delay_ms = cfg.routine_checkin_ms;
    }
    if (hal != NULL && hal->schedule_next_wake != NULL &&
        report.outcome != WAKE_RUN_INVALID_ARGUMENT) {
        hal->schedule_next_wake(hal->user, report.next_wake_delay_ms);
    }
    if (power_held && hal != NULL && hal->power_hold != NULL) {
        hal->power_hold(hal->user, false);
    }
    return report;
}
