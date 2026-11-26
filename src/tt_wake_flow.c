#include "tt/wake_flow.h"

#include <stdio.h>
#include <string.h>

void tt_ack_set_reason(tt_ack_t *ack, const char *reason)
{
    if (ack == NULL) {
        return;
    }
    memset(ack->reason, 0, sizeof(ack->reason));
    if (reason != NULL) {
        strncpy(ack->reason, reason, TT_REASON_MAX);
        ack->reason[TT_REASON_MAX] = '\0';
    }
}

tt_result_t tt_wake_config_default(tt_wake_config_t *config)
{
    if (config == NULL) {
        return TT_E_ARG;
    }
    memset(config, 0, sizeof(*config));

    config->retry.max_attempts = 5U;
    config->retry.initial_delay_ms = 500U;
    config->retry.max_delay_ms = 15000U;
    config->retry.attempt_reserve_ms = 30000U;

    /* verify_signature intentionally has no unsafe default. */
    config->validation.verify_signature = NULL;
    config->validation.verify_ctx = NULL;
    config->validation.max_clock_skew_ms = 5U * 60U * 1000U;
    config->validation.max_command_ttl_ms = 24U * 60U * 60U * 1000U;
    config->validation.min_temp_c = 16;
    config->validation.max_temp_c = 30;

    config->session_budget_ms = 120U * 1000U;
    config->first_connect_jitter_ms = 5000U;
    config->connect_timeout_ms = 12000U;
    config->pull_timeout_ms = 10000U;
    config->ack_timeout_ms = 5000U;
    config->execute_timeout_ms = 5000U;
    config->max_commands_per_wake = 8U;
    config->pending_wake_delay_ms = 10U * 1000U;
    return TT_OK;
}

static bool retryable_transport(tt_result_t rc)
{
    return rc == TT_E_IO || rc == TT_E_TIMEOUT || rc == TT_E_CAPACITY;
}

typedef struct net_recovery {
    uint32_t failures;
    uint32_t wake_after_ms;
    bool connected;
    tt_result_t last_error;
} net_recovery_t;

static bool net_backoff(const tt_wake_config_t *config,
                        tt_gateway_t *gateway,
                        const tt_deadline_t *deadline,
                        net_recovery_t *net,
                        uint32_t platform_hint_ms)
{
    tt_retry_decision_t decision;
    uint64_t now = gateway->monotonic_ms(gateway->ctx);
    tt_result_t rc;

    net->failures++;
    rc = tt_retry_plan(&config->retry,
                       deadline,
                       net->failures,
                       now,
                       gateway->random_u32(gateway->ctx),
                       platform_hint_ms,
                       &decision);
    if (rc != TT_OK) {
        return false;
    }

    if (decision.should_retry) {
        gateway->delay_ms(gateway->ctx, decision.delay_ms);
        return true;
    }

    if (decision.off_session_wake_ms != 0U) {
        net->wake_after_ms = decision.off_session_wake_ms;
    } else if (net->wake_after_ms == 0U) {
        /* Next RTC wake should also be spread across the fleet. */
        net->wake_after_ms = tt_retry_jitter_delay(
            net->failures,
            config->retry.initial_delay_ms,
            config->retry.max_delay_ms,
            gateway->random_u32(gateway->ctx));
    }
    return false;
}

typedef enum ack_delivery {
    ACK_DELIVERED,
    ACK_PLATFORM_BACKOFF,
    ACK_RECOVERABLE,
    ACK_FATAL
} ack_delivery_t;

static ack_delivery_t send_ack_with_recovery(
    const tt_wake_config_t *config,
    tt_gateway_t *gateway,
    const tt_deadline_t *deadline,
    net_recovery_t *net,
    const char *command_id,
    const tt_ack_t *ack,
    tt_result_t *fatal_rc)
{
    uint32_t hint = 0U;
    uint32_t timeout_ms = tt_min_u32(config->ack_timeout_ms,
                                    config->retry.attempt_reserve_ms);
    tt_result_t rc = gateway->ack(gateway->ctx, command_id, ack,
                                 timeout_ms, &hint);

    if (rc == TT_OK) {
        if (hint != 0U) {
            net->wake_after_ms = hint;
            return ACK_PLATFORM_BACKOFF;
        }
        return ACK_DELIVERED;
    }

    if (!retryable_transport(rc)) {
        *fatal_rc = rc;
        return ACK_FATAL;
    }
    net->last_error = rc;

    if (gateway->disconnect != NULL && net->connected) {
        gateway->disconnect(gateway->ctx);
    }
    net->connected = false;

    if (net_backoff(config, gateway, deadline, net, hint)) {
        return ACK_RECOVERABLE;
    }

    *fatal_rc = rc;
    return ACK_FATAL;
}

static bool config_valid(const tt_wake_config_t *config)
{
    const tt_retry_policy_t *r;

    if (config == NULL || config->max_commands_per_wake == 0U ||
        config->session_budget_ms == 0U || config->connect_timeout_ms == 0U ||
        config->pull_timeout_ms == 0U || config->ack_timeout_ms == 0U ||
        config->execute_timeout_ms == 0U ||
        config->pending_wake_delay_ms == 0U ||
        config->pending_wake_delay_ms > 3600U * 1000U ||
        config->first_connect_jitter_ms > 60U * 1000U) {
        return false;
    }

    r = &config->retry;
    if (r->max_attempts == 0U || r->initial_delay_ms == 0U ||
        r->max_delay_ms < r->initial_delay_ms ||
        r->attempt_reserve_ms == 0U ||
        config->session_budget_ms < r->attempt_reserve_ms ||
        config->connect_timeout_ms > r->attempt_reserve_ms ||
        config->pull_timeout_ms > r->attempt_reserve_ms ||
        config->ack_timeout_ms > r->attempt_reserve_ms ||
        config->execute_timeout_ms > r->attempt_reserve_ms) {
        return false;
    }

    return config->validation.verify_signature != NULL &&
           config->validation.max_command_ttl_ms != 0U &&
           config->validation.min_temp_c <= config->validation.max_temp_c;
}

static bool gateway_valid(tt_gateway_t *gateway, tt_storage_t *storage)
{
    if (gateway == NULL || storage == NULL) {
        return false;
    }
    return gateway->ctx != NULL &&
           gateway->monotonic_ms != NULL &&
           gateway->trusted_unix_ms != NULL &&
           gateway->random_u32 != NULL &&
           gateway->delay_ms != NULL &&
           gateway->connect != NULL &&
           gateway->pull_next != NULL &&
           gateway->ack != NULL &&
           gateway->execute != NULL &&
           storage->ctx != NULL &&
           storage->load_checkpoint != NULL &&
           storage->find_result != NULL &&
           storage->commit_result != NULL;
}

tt_result_t tt_wake_run(const tt_wake_config_t *config,
                        tt_gateway_t *gateway,
                        tt_storage_t *storage,
                        tt_wake_report_t *report)
{
    tt_deadline_t deadline;
    tt_checkpoint_t checkpoint;
    net_recovery_t net = {0};
    uint64_t start_ms;
    uint32_t first_jitter;
    uint32_t processed = 0U;
    tt_result_t final_rc = TT_OK;
    bool ever_connected = false;

    if (report != NULL) {
        memset(report, 0, sizeof(*report));
    }

    if (!config_valid(config) || !gateway_valid(gateway, storage) ||
        report == NULL) {
        return TT_E_ARG;
    }

    memset(&checkpoint, 0, sizeof(checkpoint));
    final_rc = storage->load_checkpoint(storage->ctx, &checkpoint);
    if (final_rc != TT_OK) {
        report->result = final_rc;
        return final_rc;
    }

    start_ms = gateway->monotonic_ms(gateway->ctx);
    deadline.start_ms = start_ms;
    deadline.budget_ms = config->session_budget_ms;

    /*
     * Small device-specific desynchronization before the first TLS handshake.
     * It turns a synchronized fleet pulse into a ramp.  The platform's own
     * staggered SMS dispatch and Retry-After are the larger-scale controls.
     */
    if (config->first_connect_jitter_ms != 0U) {
        uint64_t remaining = tt_deadline_remaining_ms(&deadline, start_ms);
        uint64_t available = 0U;

        first_jitter = gateway->random_u32(gateway->ctx) %
                       (config->first_connect_jitter_ms + 1U);
        if (remaining >= config->retry.attempt_reserve_ms) {
            available = remaining - config->retry.attempt_reserve_ms;
        }
        if ((uint64_t)first_jitter > available) {
            first_jitter = (uint32_t)available;
        }
        gateway->delay_ms(gateway->ctx, first_jitter);
    }

    while (processed < config->max_commands_per_wake) {
        uint64_t now = gateway->monotonic_ms(gateway->ctx);

        if (!net.connected) {
            tt_result_t connect_rc;
            uint32_t hint = 0U;
            uint32_t timeout_ms;

            if (!tt_deadline_has_room(&deadline, now,
                                      config->retry.attempt_reserve_ms)) {
                final_rc = TT_E_TIMEOUT;
                report->pending_remains = true;
                break;
            }

            timeout_ms = tt_min_u32(config->connect_timeout_ms,
                                    config->retry.attempt_reserve_ms);
            connect_rc = gateway->connect(gateway->ctx, timeout_ms, &hint);
            if (connect_rc == TT_OK) {
                net.connected = true;
                ever_connected = true;
                report->secure_session_established = true;
                continue;
            }

            if (connect_rc == TT_E_AUTH || !retryable_transport(connect_rc)) {
                final_rc = connect_rc;
                break;
            }

            if (net_backoff(config, gateway, &deadline, &net, hint)) {
                continue;
            }
            final_rc = connect_rc;
            break;
        }

        if (!tt_deadline_has_room(&deadline, now,
                                  config->retry.attempt_reserve_ms)) {
            final_rc = TT_E_TIMEOUT;
            report->pending_remains = true;
            break;
        }

        tt_signed_command_t envelope;
        bool has_command = false;
        uint32_t pull_hint = 0U;
        tt_result_t pull_rc;

        memset(&envelope, 0, sizeof(envelope));
        uint32_t pull_timeout = tt_min_u32(config->pull_timeout_ms,
                                           config->retry.attempt_reserve_ms);
        pull_rc = gateway->pull_next(gateway->ctx, pull_timeout, &envelope,
                                     &has_command, &pull_hint);
        if (pull_rc != TT_OK) {
            tt_result_t fatal_rc = TT_OK;

            if (!retryable_transport(pull_rc)) {
                final_rc = pull_rc;
                break;
            }

            if (gateway->disconnect != NULL) {
                gateway->disconnect(gateway->ctx);
            }
            net.connected = false;
            if (net_backoff(config, gateway, &deadline, &net, pull_hint)) {
                continue;
            }
            final_rc = pull_rc;
            (void)fatal_rc;
            break;
        }

        if (!has_command) {
            if (pull_hint != 0U) {
                net.wake_after_ms = pull_hint;
            }
            final_rc = TT_OK;
            break;
        }

        processed++;
        report->commands_seen++;

        uint64_t trusted_now = 0U;
        tt_result_t time_rc = gateway->trusted_unix_ms(gateway->ctx,
                                                       &trusted_now);
        if (time_rc != TT_OK) {
            final_rc = time_rc;
            break;
        }

        tt_result_t valid_rc = tt_command_validate(&envelope, trusted_now,
                                                   &config->validation);
        if (valid_rc != TT_OK) {
            tt_ack_t rejection;
            ack_delivery_t delivered;
            tt_result_t fatal_rc = TT_OK;

            memset(&rejection, 0, sizeof(rejection));
            rejection.code = TT_ACK_REJECTED;
            rejection.vehicle_error = (int32_t)valid_rc;
            tt_ack_set_reason(&rejection, tt_strerror(valid_rc));
            report->rejected++;

            if (envelope.command.id[0] == '\0') {
                /* Nothing safe to acknowledge; platform timeout redelivers. */
                final_rc = valid_rc;
                break;
            }

            delivered = send_ack_with_recovery(
                config, gateway, &deadline, &net,
                envelope.command.id, &rejection, &fatal_rc);
            if (delivered == ACK_FATAL) {
                final_rc = fatal_rc;
                break;
            }
            if (delivered == ACK_RECOVERABLE) {
                continue;
            }
            if (delivered == ACK_PLATFORM_BACKOFF) {
                final_rc = TT_OK;
                goto finish;
            }
            continue;
        }

        tt_ack_t stored;
        bool already_stored = false;
        tt_result_t storage_rc;

        memset(&stored, 0, sizeof(stored));
        storage_rc = storage->find_result(storage->ctx,
                                          envelope.command.id,
                                          &stored,
                                          &already_stored);
        if (storage_rc != TT_OK) {
            final_rc = storage_rc;
            break;
        }

        if (already_stored) {
            ack_delivery_t delivered;
            tt_result_t fatal_rc = TT_OK;

            report->duplicates++;
            delivered = send_ack_with_recovery(
                config, gateway, &deadline, &net,
                envelope.command.id, &stored, &fatal_rc);
            if (delivered == ACK_FATAL) {
                final_rc = fatal_rc;
                break;
            }
            if (delivered == ACK_RECOVERABLE) {
                continue;
            }
            if (delivered == ACK_PLATFORM_BACKOFF) {
                final_rc = TT_OK;
                goto finish;
            }
            continue;
        }

        bool duplicate_checkpoint = false;
        tt_result_t replay_rc = tt_checkpoint_check(&checkpoint,
                                                     &envelope.command,
                                                     &duplicate_checkpoint);
        if (replay_rc == TT_E_REPLAY) {
            tt_ack_t rejection;
            ack_delivery_t delivered;
            tt_result_t fatal_rc = TT_OK;

            memset(&rejection, 0, sizeof(rejection));
            rejection.code = TT_ACK_REJECTED;
            tt_ack_set_reason(&rejection, "replay or sequence conflict");
            report->rejected++;

            delivered = send_ack_with_recovery(
                config, gateway, &deadline, &net,
                envelope.command.id, &rejection, &fatal_rc);
            if (delivered == ACK_FATAL) {
                final_rc = fatal_rc;
                break;
            }
            if (delivered == ACK_RECOVERABLE) {
                continue;
            }
            if (delivered == ACK_PLATFORM_BACKOFF) {
                final_rc = TT_OK;
                goto finish;
            }
            continue;
        }
        if (replay_rc != TT_OK) {
            final_rc = replay_rc;
            break;
        }

        if (duplicate_checkpoint) {
            tt_ack_t duplicate_ack;
            ack_delivery_t delivered;
            tt_result_t fatal_rc = TT_OK;

            memset(&duplicate_ack, 0, sizeof(duplicate_ack));
            duplicate_ack.code = TT_ACK_DUPLICATE;
            tt_ack_set_reason(&duplicate_ack, "checkpoint match");
            report->duplicates++;

            delivered = send_ack_with_recovery(
                config, gateway, &deadline, &net,
                envelope.command.id, &duplicate_ack, &fatal_rc);
            if (delivered == ACK_FATAL) {
                final_rc = fatal_rc;
                break;
            }
            if (delivered == ACK_RECOVERABLE) {
                continue;
            }
            if (delivered == ACK_PLATFORM_BACKOFF) {
                final_rc = TT_OK;
                goto finish;
            }
            continue;
        }

        tt_ack_t execution;
        tt_result_t execute_rc;

        memset(&execution, 0, sizeof(execution));
        uint32_t execute_timeout = tt_min_u32(
            config->execute_timeout_ms, config->retry.attempt_reserve_ms);
        execute_rc = gateway->execute(gateway->ctx,
                                      &envelope.command,
                                      execute_timeout,
                                      &execution);
        if (execute_rc != TT_OK) {
            execution.code = TT_ACK_RETRYABLE;
            execution.vehicle_error = (int32_t)execute_rc;
            if (execution.reason[0] == '\0') {
                tt_ack_set_reason(&execution, tt_strerror(execute_rc));
            }
        } else if (execution.code != TT_ACK_ACCEPTED &&
                   execution.code != TT_ACK_REJECTED &&
                   execution.code != TT_ACK_RETRYABLE) {
            execution.code = TT_ACK_RETRYABLE;
            tt_ack_set_reason(&execution, "adapter returned invalid ack");
        }

        if (execution.code == TT_ACK_ACCEPTED) {
            report->accepted++;
        } else if (execution.code == TT_ACK_REJECTED) {
            report->rejected++;
        } else {
            report->retryable++;
        }

        if (execution.code == TT_ACK_ACCEPTED ||
            execution.code == TT_ACK_REJECTED) {
            tt_result_t commit_rc = storage->commit_result(
                storage->ctx, envelope.command.id,
                envelope.command.sequence, &execution);
            if (commit_rc != TT_OK) {
                final_rc = commit_rc;
                break;
            }
            checkpoint.last_sequence = envelope.command.sequence;
            memset(checkpoint.last_command_id, 0,
                   sizeof(checkpoint.last_command_id));
            strncpy(checkpoint.last_command_id, envelope.command.id,
                    TT_COMMAND_ID_MAX);
            checkpoint.last_command_id[TT_COMMAND_ID_MAX] = '\0';
        } else {
            report->pending_remains = true;
        }

        ack_delivery_t delivered;
        tt_result_t fatal_rc = TT_OK;
        delivered = send_ack_with_recovery(config, gateway, &deadline, &net,
                                           envelope.command.id, &execution,
                                           &fatal_rc);
        if (delivered == ACK_FATAL) {
            final_rc = fatal_rc;
            break;
        }

        /*
         * A retryable vehicle-domain result blocks later commands so order is
         * preserved (for example lock/unlock).  If the NACK itself may have
         * been lost, do not pull later commands in this window: end here and
         * let the platform lease/redelivery plus the next RTC wake recover.
         */
        if (execution.code == TT_ACK_RETRYABLE) {
            report->pending_remains = true;
            if (delivered == ACK_RECOVERABLE) {
                final_rc = net.last_error != TT_OK ? net.last_error
                                                    : TT_E_IO;
            } else {
                final_rc = TT_OK;
            }
            goto finish;
        }

        if (delivered == ACK_RECOVERABLE) {
            continue;
        }

        /* An authenticated platform back-off hint ends this radio window. */
        if (delivered == ACK_PLATFORM_BACKOFF) {
            final_rc = TT_OK;
            goto finish;
        }
    }

    if (final_rc == TT_OK && processed >= config->max_commands_per_wake) {
        report->pending_remains = true;
    }

finish:
    if (net.connected && gateway->disconnect != NULL) {
        gateway->disconnect(gateway->ctx);
        net.connected = false;
    }

    report->secure_session_established = ever_connected;
    report->result = final_rc;
    report->wake_after_ms = net.wake_after_ms;

    if (report->wake_after_ms == 0U &&
        (final_rc != TT_OK || report->pending_remains)) {
        uint32_t span = config->pending_wake_delay_ms / 4U;
        uint32_t extra = span == 0U ? 0U :
            gateway->random_u32(gateway->ctx) % (span + 1U);
        report->wake_after_ms = config->pending_wake_delay_ms + extra;
    }

    if (gateway->schedule_next_wake != NULL && report->wake_after_ms != 0U) {
        (void)gateway->schedule_next_wake(gateway->ctx,
                                          report->wake_after_ms);
    }

    return final_rc;
}
