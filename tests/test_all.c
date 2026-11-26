#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "tt/command.h"
#include "tt/retry.h"
#include "tt/wake_flow.h"
#include "tt/wake_sms.h"

static int checks_run = 0;
static int checks_failed = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        checks_run++;                                                    \
        if (!(cond)) {                                                   \
            checks_failed++;                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                \
    } while (0)

#define CHECK_RC(actual, expected)                                       \
    do {                                                                 \
        tt_result_t actual_rc = (actual);                                \
        tt_result_t expected_rc = (expected);                            \
        checks_run++;                                                    \
        if (actual_rc != expected_rc) {                                   \
            checks_failed++;                                             \
            printf("FAIL %s:%d: %s == %d, expected %d (%s vs %s)\n",     \
                   __FILE__, __LINE__, #actual, (int)actual_rc,          \
                   (int)expected_rc, tt_strerror(actual_rc),             \
                   tt_strerror(expected_rc));                            \
        }                                                                \
    } while (0)

/* ----------------------------- SMS tests ----------------------------- */

static void test_wake_sms(void)
{
    tt_wake_sms_t wake;
    tt_wake_limiter_t limiter;
    const uint8_t bare[] = TT_WAKE_SMS_MAGIC;
    const uint8_t nonce_lower[] = TT_WAKE_SMS_MAGIC " 0123456789abcdef";
    const uint8_t nonce_upper[] = TT_WAKE_SMS_MAGIC " 0123456789ABCDEF";
    const uint8_t command_sms[] = "TT-WAKE/1;UNLOCK NOW";
    const uint8_t bad_nonce[] = TT_WAKE_SMS_MAGIC " xyz";

    CHECK_RC(tt_wake_sms_parse(bare, sizeof(TT_WAKE_SMS_MAGIC) - 1U, &wake),
             TT_OK);
    CHECK(wake.nonce[0] == '\0');

    CHECK_RC(tt_wake_sms_parse(nonce_lower, sizeof(nonce_lower) - 1U, &wake),
             TT_OK);
    CHECK(strcmp(wake.nonce, "0123456789abcdef") == 0);

    CHECK_RC(tt_wake_sms_parse(nonce_upper, sizeof(nonce_upper) - 1U, &wake),
             TT_OK);
    CHECK(strcmp(wake.nonce, "0123456789abcdef") == 0);

    CHECK_RC(tt_wake_sms_parse(command_sms, sizeof(command_sms) - 1U, &wake),
             TT_E_FORMAT);
    CHECK_RC(tt_wake_sms_parse(bad_nonce, sizeof(bad_nonce) - 1U, &wake),
             TT_E_FORMAT);

    {
        const uint8_t ordinary[] = "meeting at 10";
        CHECK_RC(tt_wake_sms_parse(ordinary, sizeof(ordinary) - 1U, &wake),
                 TT_E_IGNORED);
    }
    CHECK_RC(tt_wake_sms_parse(NULL, 0U, &wake), TT_E_ARG);

    tt_wake_limiter_init(&limiter, 2U, 1000U);
    CHECK(tt_wake_limiter_allow(&limiter, 0U));
    CHECK(tt_wake_limiter_allow(&limiter, 100U));
    CHECK(!tt_wake_limiter_allow(&limiter, 200U));
    CHECK(tt_wake_limiter_allow(&limiter, 1000U));
}

/* ---------------------------- retry tests ---------------------------- */

static void test_retry(void)
{
    tt_deadline_t deadline = {0U, 10000U};
    tt_retry_policy_t policy = {5U, 100U, 1000U, 1000U};
    tt_retry_decision_t decision;
    uint32_t r;

    CHECK(tt_deadline_remaining_ms(&deadline, 2500U) == 7500U);
    CHECK(tt_deadline_remaining_ms(&deadline, 20000U) == 0U);
    CHECK(!tt_deadline_has_room(&deadline, 9500U, 1000U));
    CHECK(tt_deadline_has_room(&deadline, 9000U, 1000U));

    for (r = 0U; r < 1000U; ++r) {
        uint32_t d1 = tt_retry_jitter_delay(1U, 100U, 1000U, r);
        uint32_t d2 = tt_retry_jitter_delay(2U, 100U, 1000U, r);
        uint32_t d10 = tt_retry_jitter_delay(10U, 100U, 1000U, r);
        CHECK(d1 <= 100U);
        CHECK(d2 <= 200U);
        CHECK(d10 <= 1000U);
    }

    CHECK_RC(tt_retry_plan(&policy, &deadline, 5U, 0U, 7U, 0U, &decision),
             TT_OK);
    CHECK(!decision.should_retry);

    CHECK_RC(tt_retry_plan(&policy, &deadline, 1U, 9500U, 7U, 0U, &decision),
             TT_OK);
    CHECK(!decision.should_retry);

    CHECK_RC(tt_retry_plan(&policy, &deadline, 1U, 0U, 3U, 5000U, &decision),
             TT_OK);
    CHECK(decision.should_retry);
    CHECK(decision.delay_ms == 5000U);

    CHECK_RC(tt_retry_plan(&policy, &deadline, 1U, 0U, 3U, 20000U, &decision),
             TT_OK);
    CHECK(!decision.should_retry);
    CHECK(decision.off_session_wake_ms == 20000U);

    CHECK_RC(tt_retry_plan(&policy, &deadline, 1U, 0U, 0U, 0U, &decision),
             TT_OK);
    CHECK(decision.should_retry);
    CHECK(decision.delay_ms == 0U);
}

/* --------------------------- command tests --------------------------- */

static tt_result_t verify_ok(void *ctx, const uint8_t *payload, size_t len,
                             const uint8_t *sig, size_t sig_len)
{
    (void)ctx;
    if (payload == NULL || len == 0U || sig == NULL || sig_len == 0U) {
        return TT_E_ARG;
    }
    return TT_OK;
}

static tt_result_t verify_bad(void *ctx, const uint8_t *payload, size_t len,
                              const uint8_t *sig, size_t sig_len)
{
    (void)ctx; (void)payload; (void)len; (void)sig; (void)sig_len;
    return TT_E_AUTH;
}

static tt_signed_command_t make_test_envelope(void)
{
    tt_signed_command_t env;
    memset(&env, 0, sizeof(env));
    snprintf(env.command.id, sizeof(env.command.id), "cmd-1234");
    env.command.sequence = 7U;
    env.command.issued_at_ms = 1000U;
    env.command.expires_at_ms = 101000U;
    env.command.type = TT_CMD_LOCATE;
    memcpy(env.canonical_payload, "{\"id\":\"cmd-1234\"}", 17U);
    env.payload_len = 17U;
    env.signature[0] = 0x30U;
    env.signature_len = 1U;
    return env;
}

static void test_commands(void)
{
    tt_validation_config_t valid_cfg = {
        verify_ok, NULL, 300000U, 86400000U, 16, 30
    };
    tt_validation_config_t bad_sig_cfg = valid_cfg;
    tt_signed_command_t env = make_test_envelope();
    tt_checkpoint_t cp = {0, ""};
    bool duplicate = false;

    CHECK_RC(tt_command_validate(&env, 50000U, &valid_cfg), TT_OK);

    env.command.type = TT_CMD_SET_CLIMATE;
    env.command.target_temp_c = 42;
    CHECK_RC(tt_command_validate(&env, 50000U, &valid_cfg), TT_E_FORMAT);
    env.command.target_temp_c = 22;
    CHECK_RC(tt_command_validate(&env, 50000U, &valid_cfg), TT_OK);

    CHECK_RC(tt_command_validate(&env, 200000U, &valid_cfg), TT_E_EXPIRED);

    env = make_test_envelope();
    env.command.issued_at_ms = 2000000U;
    env.command.expires_at_ms = 2001000U;
    CHECK_RC(tt_command_validate(&env, 0U, &valid_cfg), TT_E_EXPIRED);

    bad_sig_cfg.verify_signature = verify_bad;
    env = make_test_envelope();
    CHECK_RC(tt_command_validate(&env, 50000U, &bad_sig_cfg), TT_E_AUTH);

    cp.last_sequence = 5U;
    snprintf(cp.last_command_id, sizeof(cp.last_command_id), "cmd-5");
    env.command.sequence = 6U;
    CHECK_RC(tt_checkpoint_check(&cp, &env.command, &duplicate), TT_OK);
    CHECK(!duplicate);

    env.command.sequence = 5U;
    snprintf(env.command.id, sizeof(env.command.id), "cmd-5");
    CHECK_RC(tt_checkpoint_check(&cp, &env.command, &duplicate), TT_OK);
    CHECK(duplicate);

    snprintf(env.command.id, sizeof(env.command.id), "cmd-evil");
    CHECK_RC(tt_checkpoint_check(&cp, &env.command, &duplicate), TT_E_REPLAY);

    env.command.sequence = 4U;
    CHECK_RC(tt_checkpoint_check(&cp, &env.command, &duplicate), TT_E_REPLAY);
}

/* ------------------------- orchestrator fakes ------------------------ */

#define FAKE_MAX_ITEMS 8

typedef struct fake_gateway {
    uint64_t mono_ms;
    uint64_t wall_ms;
    uint32_t rng;

    uint32_t delays[32];
    size_t delay_count;

    uint32_t connect_calls;
    tt_result_t connect_rc;
    uint32_t connect_hint;
    tt_result_t connect_script[FAKE_MAX_ITEMS];
    uint32_t connect_hints[FAKE_MAX_ITEMS];
    size_t connect_script_len;

    uint32_t disconnect_calls;
    uint32_t pull_calls;
    tt_signed_command_t commands[FAKE_MAX_ITEMS];
    size_t command_count;
    size_t command_index;
    tt_result_t pull_script[FAKE_MAX_ITEMS];
    uint32_t pull_hints[FAKE_MAX_ITEMS];
    size_t pull_script_len;

    uint32_t execute_calls;
    char executed_ids[FAKE_MAX_ITEMS][TT_COMMAND_ID_MAX + 1U];
    tt_command_type_t executed_types[FAKE_MAX_ITEMS];
    tt_result_t execute_rc;
    tt_ack_code_t execute_code;

    size_t ack_count;
    char ack_ids[FAKE_MAX_ITEMS][TT_COMMAND_ID_MAX + 1U];
    tt_ack_code_t ack_codes[FAKE_MAX_ITEMS];
    tt_result_t ack_rc;

    uint32_t scheduled[FAKE_MAX_ITEMS];
    size_t schedule_count;
} fake_gateway_t;

typedef struct fake_record {
    bool used;
    char id[TT_COMMAND_ID_MAX + 1U];
    uint64_t sequence;
    tt_ack_t ack;
} fake_record_t;

typedef struct fake_storage {
    tt_checkpoint_t checkpoint;
    fake_record_t records[FAKE_MAX_ITEMS * 2];
    size_t record_count;
    uint32_t commits;
} fake_storage_t;

static uint64_t fake_monotonic(void *ctx)
{
    fake_gateway_t *f = ctx;
    return f->mono_ms;
}

static tt_result_t fake_trusted_time(void *ctx, uint64_t *unix_ms)
{
    fake_gateway_t *f = ctx;
    if (unix_ms == NULL) return TT_E_ARG;
    *unix_ms = f->wall_ms;
    return TT_OK;
}

static uint32_t fake_random(void *ctx)
{
    fake_gateway_t *f = ctx;
    return f->rng;
}

static void fake_delay(void *ctx, uint32_t ms)
{
    fake_gateway_t *f = ctx;
    if (f->delay_count < sizeof(f->delays) / sizeof(f->delays[0])) {
        f->delays[f->delay_count++] = ms;
    }
    f->mono_ms += ms;
    f->wall_ms += ms;
}

static tt_result_t fake_connect(void *ctx, uint32_t timeout_ms,
                                uint32_t *retry_after_ms)
{
    fake_gateway_t *f = ctx;
    tt_result_t rc = f->connect_rc;
    uint32_t hint = f->connect_hint;
    size_t call = f->connect_calls;
    (void)timeout_ms;

    f->connect_calls++;
    if (call < f->connect_script_len) {
        rc = f->connect_script[call];
        hint = f->connect_hints[call];
    }
    if (retry_after_ms != NULL) *retry_after_ms = hint;
    return rc;
}

static void fake_disconnect(void *ctx)
{
    fake_gateway_t *f = ctx;
    f->disconnect_calls++;
}

static tt_result_t fake_pull(void *ctx, uint32_t timeout_ms,
                             tt_signed_command_t *out,
                             bool *has_command, uint32_t *retry_after_ms)
{
    fake_gateway_t *f = ctx;
    size_t call = f->pull_calls;

    (void)timeout_ms;
    f->pull_calls++;
    if (has_command != NULL) *has_command = false;
    if (retry_after_ms != NULL) *retry_after_ms = 0U;

    if (call < f->pull_script_len) {
        if (retry_after_ms != NULL) *retry_after_ms = f->pull_hints[call];
        return f->pull_script[call];
    }

    if (f->command_index < f->command_count) {
        if (out == NULL || has_command == NULL) return TT_E_ARG;
        *out = f->commands[f->command_index++];
        *has_command = true;
        return TT_OK;
    }

    return TT_OK;
}

static tt_result_t fake_ack(void *ctx, const char *id, const tt_ack_t *ack,
                            uint32_t timeout_ms, uint32_t *retry_after_ms)
{
    fake_gateway_t *f = ctx;
    (void)timeout_ms;
    if (retry_after_ms != NULL) *retry_after_ms = 0U;
    if (id == NULL || ack == NULL) return TT_E_ARG;

    if (f->ack_count < FAKE_MAX_ITEMS) {
        snprintf(f->ack_ids[f->ack_count], TT_COMMAND_ID_MAX + 1U, "%s", id);
        f->ack_codes[f->ack_count] = ack->code;
        f->ack_count++;
    }
    return f->ack_rc;
}

static tt_result_t fake_execute(void *ctx, const tt_command_t *command,
                                uint32_t timeout_ms, tt_ack_t *result)
{
    fake_gateway_t *f = ctx;
    size_t i = f->execute_calls;

    (void)timeout_ms;
    if (command == NULL || result == NULL) return TT_E_ARG;
    f->execute_calls++;
    if (i < FAKE_MAX_ITEMS) {
        snprintf(f->executed_ids[i], TT_COMMAND_ID_MAX + 1U, "%s",
                 command->id);
        f->executed_types[i] = command->type;
    }

    memset(result, 0, sizeof(*result));
    result->code = f->execute_code == 0 ? TT_ACK_ACCEPTED : f->execute_code;
    if (result->code == TT_ACK_ACCEPTED) {
        tt_ack_set_reason(result, "done");
    }
    return f->execute_rc;
}

static tt_result_t fake_schedule(void *ctx, uint32_t delay_ms)
{
    fake_gateway_t *f = ctx;
    if (f->schedule_count < FAKE_MAX_ITEMS) {
        f->scheduled[f->schedule_count++] = delay_ms;
    }
    return TT_OK;
}

static tt_result_t fake_load_checkpoint(void *ctx, tt_checkpoint_t *cp)
{
    fake_storage_t *s = ctx;
    if (cp == NULL) return TT_E_ARG;
    *cp = s->checkpoint;
    return TT_OK;
}

static tt_result_t fake_find_result(void *ctx, const char *id,
                                    tt_ack_t *result, bool *found)
{
    fake_storage_t *s = ctx;
    size_t i;
    if (result == NULL || found == NULL) return TT_E_ARG;
    *found = false;
    memset(result, 0, sizeof(*result));

    for (i = 0; i < s->record_count; ++i) {
        if (s->records[i].used &&
            strncmp(s->records[i].id, id, TT_COMMAND_ID_MAX + 1U) == 0) {
            *result = s->records[i].ack;
            *found = true;
            return TT_OK;
        }
    }
    return TT_OK;
}

static tt_result_t fake_commit_result(void *ctx, const char *id,
                                      uint64_t sequence,
                                      const tt_ack_t *terminal_result)
{
    fake_storage_t *s = ctx;
    fake_record_t *rec;

    if (id == NULL || terminal_result == NULL) return TT_E_ARG;
    if (s->record_count >= sizeof(s->records) / sizeof(s->records[0])) {
        return TT_E_CAPACITY;
    }

    rec = &s->records[s->record_count++];
    rec->used = true;
    snprintf(rec->id, sizeof(rec->id), "%s", id);
    rec->sequence = sequence;
    rec->ack = *terminal_result;

    s->commits++;
    s->checkpoint.last_sequence = sequence;
    memset(s->checkpoint.last_command_id, 0,
           sizeof(s->checkpoint.last_command_id));
    snprintf(s->checkpoint.last_command_id,
             sizeof(s->checkpoint.last_command_id), "%s", id);
    return TT_OK;
}

static tt_gateway_t make_gateway(fake_gateway_t *gw)
{
    tt_gateway_t table;
    memset(gw, 0, sizeof(*gw));
    gw->mono_ms = 1000U;
    gw->wall_ms = 1790000000000ULL;
    gw->rng = 0U;
    gw->connect_rc = TT_OK;
    gw->execute_rc = TT_OK;
    gw->ack_rc = TT_OK;

    memset(&table, 0, sizeof(table));
    table.ctx = gw;
    table.monotonic_ms = fake_monotonic;
    table.trusted_unix_ms = fake_trusted_time;
    table.random_u32 = fake_random;
    table.delay_ms = fake_delay;
    table.connect = fake_connect;
    table.disconnect = fake_disconnect;
    table.pull_next = fake_pull;
    table.ack = fake_ack;
    table.execute = fake_execute;
    table.schedule_next_wake = fake_schedule;
    return table;
}

static tt_storage_t make_storage(fake_storage_t *store)
{
    tt_storage_t table;
    memset(store, 0, sizeof(*store));
    memset(&table, 0, sizeof(table));
    table.ctx = store;
    table.load_checkpoint = fake_load_checkpoint;
    table.find_result = fake_find_result;
    table.commit_result = fake_commit_result;
    return table;
}

static void add_command(fake_gateway_t *gw, size_t index, const char *id,
                        uint64_t sequence, tt_command_type_t type)
{
    tt_signed_command_t *env = &gw->commands[index];
    memset(env, 0, sizeof(*env));
    snprintf(env->command.id, sizeof(env->command.id), "%s", id);
    env->command.sequence = sequence;
    env->command.issued_at_ms = gw->wall_ms - 1000U;
    env->command.expires_at_ms = gw->wall_ms + 60000U;
    env->command.type = type;
    env->command.target_temp_c = 22;
    memcpy(env->canonical_payload, "signed-payload", 14U);
    env->payload_len = 14U;
    env->signature[0] = 1U;
    env->signature_len = 1U;
    gw->command_count++;
}

static tt_wake_config_t test_config(void)
{
    tt_wake_config_t cfg;
    tt_wake_config_default(&cfg);
    cfg.validation.verify_signature = verify_ok;
    return cfg;
}

static void test_orchestrator_happy(void)
{
    fake_gateway_t fake;
    fake_storage_t fake_store;
    tt_gateway_t gw = make_gateway(&fake);
    tt_storage_t st = make_storage(&fake_store);
    tt_wake_config_t cfg = test_config();
    tt_wake_report_t report;

    add_command(&fake, 0, "cmd-unlock-1", 1U, TT_CMD_UNLOCK);
    CHECK_RC(tt_wake_run(&cfg, &gw, &st, &report), TT_OK);

    CHECK(report.secure_session_established);
    CHECK(report.commands_seen == 1U);
    CHECK(report.accepted == 1U);
    CHECK(report.rejected == 0U);
    CHECK(report.retryable == 0U);
    CHECK(report.duplicates == 0U);
    CHECK(!report.pending_remains);
    CHECK(report.wake_after_ms == 0U);
    CHECK(fake.connect_calls == 1U);
    CHECK(fake.execute_calls == 1U);
    CHECK(fake.ack_count == 1U);
    CHECK(fake.ack_codes[0] == TT_ACK_ACCEPTED);
    CHECK(fake_store.commits == 1U);
    CHECK(fake_store.checkpoint.last_sequence == 1U);
    CHECK(strcmp(fake_store.checkpoint.last_command_id, "cmd-unlock-1") == 0);
    CHECK(fake.schedule_count == 0U);
}

static void test_orchestrator_connect_retry(void)
{
    fake_gateway_t fake;
    fake_storage_t fake_store;
    tt_gateway_t gw = make_gateway(&fake);
    tt_storage_t st = make_storage(&fake_store);
    tt_wake_config_t cfg = test_config();
    tt_wake_report_t report;

    fake.connect_script[0] = TT_E_IO;
    fake.connect_hints[0] = 0U;
    fake.connect_script_len = 1U;
    add_command(&fake, 0, "cmd-ac-1", 1U, TT_CMD_SET_CLIMATE);

    CHECK_RC(tt_wake_run(&cfg, &gw, &st, &report), TT_OK);
    CHECK(fake.connect_calls == 2U);
    CHECK(report.accepted == 1U);
    CHECK(fake.execute_calls == 1U);
}

static void test_orchestrator_overload_hint(void)
{
    fake_gateway_t fake;
    fake_storage_t fake_store;
    tt_gateway_t gw = make_gateway(&fake);
    tt_storage_t st = make_storage(&fake_store);
    tt_wake_config_t cfg = test_config();
    tt_wake_report_t report;

    fake.connect_script[0] = TT_E_CAPACITY;
    fake.connect_hints[0] = 200000U;
    fake.connect_script_len = 1U;

    CHECK_RC(tt_wake_run(&cfg, &gw, &st, &report), TT_E_CAPACITY);
    CHECK(!report.secure_session_established);
    CHECK(report.wake_after_ms == 200000U);
    CHECK(fake.schedule_count == 1U);
    CHECK(fake.scheduled[0] == 200000U);
    CHECK(fake.connect_calls == 1U);
}

static void test_orchestrator_bounded_attempts(void)
{
    fake_gateway_t fake;
    fake_storage_t fake_store;
    tt_gateway_t gw = make_gateway(&fake);
    tt_storage_t st = make_storage(&fake_store);
    tt_wake_config_t cfg = test_config();
    tt_wake_report_t report;

    fake.connect_rc = TT_E_IO;
    fake.rng = 0U;
    CHECK_RC(tt_wake_run(&cfg, &gw, &st, &report), TT_E_IO);
    CHECK(fake.connect_calls == cfg.retry.max_attempts);
    CHECK(report.wake_after_ms >= cfg.pending_wake_delay_ms);
    CHECK(fake.schedule_count == 1U);
}

static void test_orchestrator_retryable_execution(void)
{
    fake_gateway_t fake;
    fake_storage_t fake_store;
    tt_gateway_t gw = make_gateway(&fake);
    tt_storage_t st = make_storage(&fake_store);
    tt_wake_config_t cfg = test_config();
    tt_wake_report_t report;

    fake.execute_code = TT_ACK_RETRYABLE;
    add_command(&fake, 0, "cmd-unlock-2", 2U, TT_CMD_UNLOCK);

    CHECK_RC(tt_wake_run(&cfg, &gw, &st, &report), TT_OK);
    CHECK(report.retryable == 1U);
    CHECK(report.pending_remains);
    CHECK(fake.ack_count == 1U);
    CHECK(fake.ack_codes[0] == TT_ACK_RETRYABLE);
    CHECK(fake_store.commits == 0U);
    CHECK(fake_store.checkpoint.last_sequence == 0U);
    CHECK(report.wake_after_ms >= cfg.pending_wake_delay_ms);
}

static void test_orchestrator_retryable_ack_lost(void)
{
    fake_gateway_t fake;
    fake_storage_t fake_store;
    tt_gateway_t gw = make_gateway(&fake);
    tt_storage_t st = make_storage(&fake_store);
    tt_wake_config_t cfg = test_config();
    tt_wake_report_t report;

    fake.execute_code = TT_ACK_RETRYABLE;
    fake.ack_rc = TT_E_IO;
    add_command(&fake, 0, "cmd-unlock-ack-lost", 2U, TT_CMD_UNLOCK);

    CHECK_RC(tt_wake_run(&cfg, &gw, &st, &report), TT_E_IO);
    CHECK(report.retryable == 1U);
    CHECK(report.pending_remains);
    CHECK(fake.execute_calls == 1U);
    CHECK(fake.ack_count == 1U);
    /* Do not pull later commands after an uncertain retryable NACK. */
    CHECK(fake.pull_calls == 1U);
    CHECK(fake_store.commits == 0U);
    CHECK(fake.schedule_count == 1U);
}

static void test_orchestrator_stored_duplicate(void)
{
    fake_gateway_t fake;
    fake_storage_t fake_store;
    tt_gateway_t gw = make_gateway(&fake);
    tt_storage_t st = make_storage(&fake_store);
    tt_wake_config_t cfg = test_config();
    tt_wake_report_t report;
    fake_record_t *rec;

    add_command(&fake, 0, "cmd-dup", 3U, TT_CMD_LOCK);
    rec = &fake_store.records[fake_store.record_count++];
    rec->used = true;
    snprintf(rec->id, sizeof(rec->id), "cmd-dup");
    rec->sequence = 3U;
    rec->ack.code = TT_ACK_ACCEPTED;
    tt_ack_set_reason(&rec->ack, "stored");
    fake_store.checkpoint.last_sequence = 3U;
    snprintf(fake_store.checkpoint.last_command_id,
             sizeof(fake_store.checkpoint.last_command_id), "cmd-dup");

    CHECK_RC(tt_wake_run(&cfg, &gw, &st, &report), TT_OK);
    CHECK(report.duplicates == 1U);
    CHECK(report.accepted == 0U);
    CHECK(fake.execute_calls == 0U);
    CHECK(fake.ack_count == 1U);
    CHECK(fake.ack_codes[0] == TT_ACK_ACCEPTED);
}

static void test_orchestrator_replay_conflict(void)
{
    fake_gateway_t fake;
    fake_storage_t fake_store;
    tt_gateway_t gw = make_gateway(&fake);
    tt_storage_t st = make_storage(&fake_store);
    tt_wake_config_t cfg = test_config();
    tt_wake_report_t report;

    add_command(&fake, 0, "cmd-new-id", 1U, TT_CMD_UNLOCK);
    fake_store.checkpoint.last_sequence = 9U;
    snprintf(fake_store.checkpoint.last_command_id,
             sizeof(fake_store.checkpoint.last_command_id), "cmd-old");

    CHECK_RC(tt_wake_run(&cfg, &gw, &st, &report), TT_OK);
    CHECK(report.rejected == 1U);
    CHECK(fake.execute_calls == 0U);
    CHECK(fake.ack_count == 1U);
    CHECK(fake.ack_codes[0] == TT_ACK_REJECTED);
}

static void test_orchestrator_bad_signature(void)
{
    fake_gateway_t fake;
    fake_storage_t fake_store;
    tt_gateway_t gw = make_gateway(&fake);
    tt_storage_t st = make_storage(&fake_store);
    tt_wake_config_t cfg = test_config();
    tt_wake_report_t report;

    cfg.validation.verify_signature = verify_bad;
    add_command(&fake, 0, "cmd-fake", 1U, TT_CMD_UNLOCK);

    CHECK_RC(tt_wake_run(&cfg, &gw, &st, &report), TT_OK);
    CHECK(report.rejected == 1U);
    CHECK(fake.execute_calls == 0U);
    CHECK(fake.ack_count == 1U);
    CHECK(fake.ack_codes[0] == TT_ACK_REJECTED);
}

int main(void)
{
    test_wake_sms();
    test_retry();
    test_commands();
    test_orchestrator_happy();
    test_orchestrator_connect_retry();
    test_orchestrator_overload_hint();
    test_orchestrator_bounded_attempts();
    test_orchestrator_retryable_execution();
    test_orchestrator_retryable_ack_lost();
    test_orchestrator_stored_duplicate();
    test_orchestrator_replay_conflict();
    test_orchestrator_bad_signature();

    if (checks_failed != 0) {
        printf("\n%d/%d checks failed\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d checks passed\n", checks_run);
    return 0;
}
