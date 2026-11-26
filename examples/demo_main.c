/*
 * Host demo of the firmware-independent wake state machine.
 *
 * The gateway/storage implementations here are in-memory fakes.  Production
 * replacements call the modem SDK, mbedTLS/WolfSSL and flash KV storage.
 */
#include <stdio.h>
#include <string.h>

#include "tt/wake_flow.h"
#include "tt/wake_sms.h"

typedef struct demo_platform {
    uint64_t mono_ms;
    uint64_t wall_ms;
    bool command_returned;
} demo_platform_t;

static uint64_t demo_mono(void *ctx)
{
    demo_platform_t *p = ctx;
    return p->mono_ms++;
}

static tt_result_t demo_time(void *ctx, uint64_t *unix_ms)
{
    demo_platform_t *p = ctx;
    *unix_ms = p->wall_ms;
    return TT_OK;
}

static uint32_t demo_rand(void *ctx)
{
    (void)ctx;
    /* A real port must use the modem/MCU hardware RNG or a seeded CSPRNG. */
    return 0x12345678U;
}

static void demo_delay(void *ctx, uint32_t ms)
{
    demo_platform_t *p = ctx;
    p->mono_ms += ms;
    printf("  hal: radio/RTOS wait %u ms\n", ms);
}

static tt_result_t demo_connect(void *ctx, uint32_t timeout_ms,
                                uint32_t *retry_after_ms)
{
    (void)ctx;
    (void)timeout_ms;
    if (retry_after_ms != NULL) *retry_after_ms = 0U;
    printf("  tls: mTLS session established (timeout=%u ms)\n", timeout_ms);
    return TT_OK;
}

static void demo_disconnect(void *ctx)
{
    (void)ctx;
    printf("  tls: session closed, radio can wind down\n");
}

/* DEMO ONLY: this checks presence, not a real cryptographic signature. */
static tt_result_t demo_insecure_verify_only(void *ctx,
                                             const uint8_t *payload,
                                             size_t len,
                                             const uint8_t *sig,
                                             size_t sig_len)
{
    (void)ctx;
    if (payload == NULL || len == 0U || sig == NULL || sig_len == 0U) {
        return TT_E_AUTH;
    }
    printf("  sign: verified platform signature over %zu canonical bytes\n",
           len);
    return TT_OK;
}

static tt_result_t demo_pull(void *ctx, uint32_t timeout_ms,
                             tt_signed_command_t *out,
                             bool *has_command, uint32_t *retry_after_ms)
{
    demo_platform_t *p = ctx;
    (void)timeout_ms;
    *retry_after_ms = 0U;

    if (!p->command_returned) {
        p->command_returned = true;
        memset(out, 0, sizeof(*out));
        snprintf(out->command.id, sizeof(out->command.id), "cmd-demo-0001");
        out->command.sequence = 1U;
        out->command.issued_at_ms = p->wall_ms - 1000U;
        out->command.expires_at_ms = p->wall_ms + 120000U;
        out->command.type = TT_CMD_SET_CLIMATE;
        out->command.target_temp_c = 22;
        memcpy(out->canonical_payload, "canonical climate command", 25U);
        out->payload_len = 25U;
        out->signature[0] = 0x41U;
        out->signature_len = 1U;
        *has_command = true;
        printf("  api: pulled %s seq=%llu temp=%d\n",
               out->command.id,
               (unsigned long long)out->command.sequence,
               out->command.target_temp_c);
        return TT_OK;
    }

    *has_command = false;
    printf("  api: command queue empty\n");
    return TT_OK;
}

static tt_result_t demo_execute(void *ctx, const tt_command_t *command,
                                uint32_t timeout_ms, tt_ack_t *result)
{
    (void)ctx;
    (void)timeout_ms;
    memset(result, 0, sizeof(*result));
    printf("  vehicle: idempotently executing %s on %s\n",
           command->id, tt_command_type_name(command->type));
    result->code = TT_ACK_ACCEPTED;
    tt_ack_set_reason(result, "climate set");
    return TT_OK;
}

static tt_result_t demo_ack(void *ctx, const char *id, const tt_ack_t *ack,
                            uint32_t timeout_ms, uint32_t *retry_after_ms)
{
    (void)ctx;
    (void)timeout_ms;
    *retry_after_ms = 0U;
    printf("  api: ack %s -> %s (%s)\n", id,
           tt_ack_code_name(ack->code), ack->reason);
    return TT_OK;
}

typedef struct demo_store {
    tt_checkpoint_t checkpoint;
} demo_store_t;

static tt_result_t demo_load_cp(void *ctx, tt_checkpoint_t *cp)
{
    demo_store_t *s = ctx;
    *cp = s->checkpoint;
    return TT_OK;
}

static tt_result_t demo_find(void *ctx, const char *id, tt_ack_t *result,
                             bool *found)
{
    (void)ctx;
    (void)id;
    memset(result, 0, sizeof(*result));
    *found = false;
    return TT_OK;
}

static tt_result_t demo_commit(void *ctx, const char *id, uint64_t sequence,
                               const tt_ack_t *ack)
{
    demo_store_t *s = ctx;
    s->checkpoint.last_sequence = sequence;
    snprintf(s->checkpoint.last_command_id,
             sizeof(s->checkpoint.last_command_id), "%s", id);
    printf("  flash: committed seq=%llu id=%s outcome=%s\n",
           (unsigned long long)sequence, id, tt_ack_code_name(ack->code));
    return TT_OK;
}

static tt_result_t demo_schedule(void *ctx, uint32_t delay_ms)
{
    (void)ctx;
    printf("  power: next RTC wake in %u ms; terminal sleeps now\n", delay_ms);
    return TT_OK;
}

int main(void)
{
    const uint8_t wake_sms[] = TT_WAKE_SMS_MAGIC " 0123456789abcdef";
    tt_wake_sms_t parsed_wake;
    tt_wake_limiter_t limiter;
    tt_wake_config_t config;
    tt_wake_report_t report;

    demo_platform_t platform = {1000U, 1790000000000ULL, false};
    demo_store_t store_impl;
    tt_gateway_t gateway;
    tt_storage_t storage;
    tt_result_t rc;

    memset(&store_impl, 0, sizeof(store_impl));

    printf("1. SMS is parsed only as a doorbell\n");
    rc = tt_wake_sms_parse(wake_sms, sizeof(wake_sms) - 1U, &parsed_wake);
    if (rc != TT_OK) {
        /* "demo-correlation" is not 16 hex chars; bare token still works. */
        const uint8_t bare[] = TT_WAKE_SMS_MAGIC;
        printf("   nonce example was not canonical; accepting bare token\n");
        rc = tt_wake_sms_parse(bare, sizeof(bare) - 1U, &parsed_wake);
    }
    if (rc != TT_OK) {
        printf("wake token rejected: %s\n", tt_strerror(rc));
        return 1;
    }
    printf("   accepted wake-only SMS; sender number is not trusted\n");

    tt_wake_limiter_init(&limiter, 3U, 60U * 60U * 1000U);
    if (!tt_wake_limiter_allow(&limiter, platform.mono_ms)) {
        printf("wake rate limit reached; sleeping without radio\n");
        return 0;
    }

    tt_wake_config_default(&config);
    config.validation.verify_signature = demo_insecure_verify_only;

    memset(&gateway, 0, sizeof(gateway));
    gateway.ctx = &platform;
    gateway.monotonic_ms = demo_mono;
    gateway.trusted_unix_ms = demo_time;
    gateway.random_u32 = demo_rand;
    gateway.delay_ms = demo_delay;
    gateway.connect = demo_connect;
    gateway.disconnect = demo_disconnect;
    gateway.pull_next = demo_pull;
    gateway.ack = demo_ack;
    gateway.execute = demo_execute;
    gateway.schedule_next_wake = demo_schedule;

    memset(&storage, 0, sizeof(storage));
    storage.ctx = &store_impl;
    storage.load_checkpoint = demo_load_cp;
    storage.find_result = demo_find;
    storage.commit_result = demo_commit;

    printf("2. terminal runs a bounded authenticated pull window\n");
    rc = tt_wake_run(&config, &gateway, &storage, &report);

    printf("3. report: rc=%s secure=%d seen=%u accepted=%u pending=%d "
           "wake_after=%u\n",
           tt_strerror(rc), report.secure_session_established,
           report.commands_seen, report.accepted, report.pending_remains,
           report.wake_after_ms);
    return rc == TT_OK ? 0 : 1;
}
