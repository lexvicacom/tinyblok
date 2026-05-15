#define _POSIX_C_SOURCE 200809L

#include "pb_eval_internal.h"

#if PB_ENABLE_JSON
#include "yyjson.h"
#endif

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pb_eval_result call_now(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 1 || args.items[0].kind != PB_KEYWORD) {
        return fail(PB_EVAL_ARITY);
    }

    time_t sec = (time_t)(ctx->wall_ms / 1000);
    struct tm tm_utc = {0};
    if (gmtime_r(&sec, &tm_utc) == NULL) {
        return fail(PB_EVAL_TYPE);
    }

    char tmp[18];
    int n = 0;
    if (text_eq(args.items[0].text, "date")) {
        n = snprintf(tmp, sizeof tmp, "%04d-%02d-%02d",
                     tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday);
    } else if (text_eq(args.items[0].text, "hour")) {
        n = snprintf(tmp, sizeof tmp, "%04d-%02d-%02dT%02d",
                     tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday, tm_utc.tm_hour);
    } else if (text_eq(args.items[0].text, "minute")) {
        n = snprintf(tmp, sizeof tmp, "%04d-%02d-%02dT%02d%02d",
                     tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                     tm_utc.tm_hour, tm_utc.tm_min);
    } else {
        return fail(PB_EVAL_UNKNOWN_SYMBOL);
    }
    if (n < 0 || (size_t)n >= sizeof tmp) {
        return fail(PB_EVAL_TYPE);
    }

    char *owned = pb_arena_memdup(ctx->arena, tmp, (size_t)n);
    if (owned == NULL) {
        return fail(PB_EVAL_OOM);
    }
    return ok((pb_value){.kind = PB_STRING, .text = {.ptr = owned, .len = (size_t)n}});
}

static pb_eval_result call_not(pb_values args) {
    if (args.len != 1) {
        return fail(PB_EVAL_ARITY);
    }
    return ok(bool_value(!truthy(args.items[0])));
}

static pb_eval_result call_eq(pb_values args) {
    if (args.len < 2) {
        return fail(PB_EVAL_ARITY);
    }
    for (size_t i = 1; i < args.len; i += 1) {
        if (!value_eq(args.items[0], args.items[i])) {
            return ok(bool_value(false));
        }
    }
    return ok(bool_value(true));
}

typedef enum cmp_op { CMP_GT, CMP_LT, CMP_GE, CMP_LE } cmp_op;

static pb_eval_result call_cmp(pb_values args, cmp_op op) {
    if (args.len < 2) {
        return fail(PB_EVAL_ARITY);
    }
    for (size_t i = 0; i + 1 < args.len; i += 1) {
        double a = 0;
        double b = 0;
        if (!as_number(args.items[i], &a) || !as_number(args.items[i + 1], &b)) {
            return fail(PB_EVAL_TYPE);
        }
        const bool pass =
            (op == CMP_GT && a > b) ||
            (op == CMP_LT && a < b) ||
            (op == CMP_GE && a >= b) ||
            (op == CMP_LE && a <= b);
        if (!pass) {
            return ok(bool_value(false));
        }
    }
    return ok(bool_value(true));
}

typedef enum arith_op { ARITH_ADD, ARITH_SUB, ARITH_MUL, ARITH_DIV } arith_op;

static pb_eval_result call_arith(pb_values args, arith_op op) {
    if (args.len == 0) {
        return fail(PB_EVAL_ARITY);
    }
    double acc = 0;
    if (!as_number(args.items[0], &acc)) {
        return fail(PB_EVAL_TYPE);
    }
    if (args.len == 1) {
        if (op == ARITH_SUB) acc = -acc;
        if (op == ARITH_DIV) acc = 1.0 / acc;
        return ok((pb_value){.kind = PB_NUMBER, .number = acc});
    }
    for (size_t i = 1; i < args.len; i += 1) {
        double x = 0;
        if (!as_number(args.items[i], &x)) {
            return fail(PB_EVAL_TYPE);
        }
        if (op == ARITH_ADD) acc += x;
        if (op == ARITH_SUB) acc -= x;
        if (op == ARITH_MUL) acc *= x;
        if (op == ARITH_DIV) acc /= x;
    }
    return ok((pb_value){.kind = PB_NUMBER, .number = acc});
}

static pb_eval_result call_round(pb_values args) {
    if (args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    double places = 0;
    double x = 0;
    if (!as_number(args.items[0], &places) ||
        !as_number(args.items[1], &x) ||
        places < 0 ||
        floor(places) != places) {
        return fail(PB_EVAL_TYPE);
    }
    const double factor = pow(10.0, places);
    return ok((pb_value){.kind = PB_NUMBER, .number = round(x * factor) / factor});
}

static pb_eval_result call_quantize(pb_values args) {
    if (args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    double step = 0;
    double x = 0;
    if (!as_number(args.items[0], &step) || !as_number(args.items[1], &x) || step <= 0) {
        return fail(PB_EVAL_TYPE);
    }
    return ok((pb_value){.kind = PB_NUMBER, .number = round(x / step) * step});
}

static pb_eval_result call_clamp(pb_values args) {
    if (args.len != 3) {
        return fail(PB_EVAL_ARITY);
    }
    double lo = 0;
    double hi = 0;
    double x = 0;
    if (!as_number(args.items[0], &lo) ||
        !as_number(args.items[1], &hi) ||
        !as_number(args.items[2], &x)) {
        return fail(PB_EVAL_TYPE);
    }
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    return ok((pb_value){.kind = PB_NUMBER, .number = x});
}

typedef enum minmax_kind { MINMAX_MIN, MINMAX_MAX } minmax_kind;

static pb_eval_result call_minmax(pb_values args, minmax_kind kind) {
    if (args.len == 0) {
        return fail(PB_EVAL_ARITY);
    }
    double acc = 0;
    if (!as_number(args.items[0], &acc)) {
        return fail(PB_EVAL_TYPE);
    }
    for (size_t i = 1; i < args.len; i += 1) {
        double x = 0;
        if (!as_number(args.items[i], &x)) {
            return fail(PB_EVAL_TYPE);
        }
        if (kind == MINMAX_MIN && x < acc) acc = x;
        if (kind == MINMAX_MAX && x > acc) acc = x;
    }
    return ok((pb_value){.kind = PB_NUMBER, .number = acc});
}

static pb_eval_result call_abs(pb_values args) {
    if (args.len != 1) {
        return fail(PB_EVAL_ARITY);
    }
    double x = 0;
    if (!as_number(args.items[0], &x)) {
        return fail(PB_EVAL_TYPE);
    }
    return ok((pb_value){.kind = PB_NUMBER, .number = fabs(x)});
}

static pb_eval_result call_sign(pb_values args) {
    if (args.len != 1) {
        return fail(PB_EVAL_ARITY);
    }
    double x = 0;
    if (!as_number(args.items[0], &x)) {
        return fail(PB_EVAL_TYPE);
    }
    return ok((pb_value){.kind = PB_NUMBER, .number = (x > 0) - (x < 0)});
}

static pb_eval_result call_str_concat(pb_eval_ctx *ctx, pb_values args) {
    size_t total = 0;
    for (size_t i = 0; i < args.len; i += 1) {
        pb_slice s = {0};
        if (!as_string(args.items[i], &s)) {
            return fail(PB_EVAL_TYPE);
        }
        total += s.len;
    }

    char *out = pb_arena_alloc(ctx->arena, total == 0 ? 1 : total, 1);
    if (out == NULL) {
        return fail(PB_EVAL_OOM);
    }
    size_t off = 0;
    for (size_t i = 0; i < args.len; i += 1) {
        pb_slice s = {0};
        (void)as_string(args.items[i], &s);
        memcpy(out + off, s.ptr, s.len);
        off += s.len;
    }
    return ok((pb_value){.kind = PB_STRING, .text = {.ptr = out, .len = total}});
}

static pb_eval_result call_contains(pb_values args) {
    if (args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    if (args.items[0].kind == PB_VECTOR) {
        for (size_t i = 0; i < args.items[0].seq.len; i += 1) {
            if (value_eq(args.items[0].seq.items[i], args.items[1])) {
                return ok(bool_value(true));
            }
        }
        return ok(bool_value(false));
    }

    pb_slice hay = {0};
    pb_slice needle = {0};
    if (!as_string(args.items[0], &hay) || !as_string(args.items[1], &needle)) {
        return fail(PB_EVAL_TYPE);
    }
    if (needle.len == 0) {
        return ok(bool_value(true));
    }
    for (size_t i = 0; i + needle.len <= hay.len; i += 1) {
        if (memcmp(hay.ptr + i, needle.ptr, needle.len) == 0) {
            return ok(bool_value(true));
        }
    }
    return ok(bool_value(false));
}

typedef enum affix_kind { AFFIX_STARTS, AFFIX_ENDS } affix_kind;

static pb_eval_result call_affix(pb_values args, affix_kind kind) {
    if (args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    pb_slice s = {0};
    pb_slice affix = {0};
    if (!as_string(args.items[0], &s) || !as_string(args.items[1], &affix)) {
        return fail(PB_EVAL_TYPE);
    }
    if (affix.len > s.len) {
        return ok(bool_value(false));
    }
    const char *at = kind == AFFIX_STARTS ? s.ptr : s.ptr + s.len - affix.len;
    return ok(bool_value(memcmp(at, affix.ptr, affix.len) == 0));
}

static pb_eval_result call_subject_append(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 1) {
        return fail(PB_EVAL_ARITY);
    }
    pb_slice suffix = {0};
    if (!as_string(args.items[0], &suffix)) {
        return fail(PB_EVAL_TYPE);
    }

    const size_t len = ctx->subject.len + 1 + suffix.len;
    char *out = pb_arena_alloc(ctx->arena, len, 1);
    if (out == NULL) {
        return fail(PB_EVAL_OOM);
    }
    memcpy(out, ctx->subject.ptr, ctx->subject.len);
    out[ctx->subject.len] = '.';
    memcpy(out + ctx->subject.len + 1, suffix.ptr, suffix.len);
    return ok((pb_value){.kind = PB_STRING, .text = {.ptr = out, .len = len}});
}

static pb_eval_result call_subject_token(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 1 ||
        args.items[0].kind != PB_NUMBER ||
        args.items[0].number < 0 ||
        floor(args.items[0].number) != args.items[0].number) {
        return fail(PB_EVAL_ARITY);
    }

    const size_t want = (size_t)args.items[0].number;
    size_t tok = 0;
    size_t start = 0;
    for (size_t i = 0; i <= ctx->subject.len; i += 1) {
        if (i == ctx->subject.len || ctx->subject.ptr[i] == '.') {
            if (tok == want) {
                return ok((pb_value){.kind = PB_STRING, .text = {.ptr = ctx->subject.ptr + start, .len = i - start}});
            }
            tok += 1;
            start = i + 1;
        }
    }
    return ok((pb_value){.kind = PB_NIL});
}

static pb_eval_result call_subject_with(pb_eval_ctx *ctx, pb_values args) {
    pb_values toks = args;
    if (args.len == 1 && args.items[0].kind == PB_VECTOR) {
        toks = args.items[0].seq;
    }
    if (toks.len == 0) {
        return fail(PB_EVAL_ARITY);
    }

    pb_slice *parts = pb_arena_alloc(ctx->arena, toks.len * sizeof parts[0], _Alignof(pb_slice));
    if (parts == NULL) {
        return fail(PB_EVAL_OOM);
    }
    size_t total = toks.len - 1;
    for (size_t i = 0; i < toks.len; i += 1) {
        if (!as_string(toks.items[i], &parts[i]) || parts[i].len == 0) {
            return fail(PB_EVAL_TYPE);
        }
        total += parts[i].len;
    }

    char *out = pb_arena_alloc(ctx->arena, total, 1);
    if (out == NULL) {
        return fail(PB_EVAL_OOM);
    }
    size_t off = 0;
    for (size_t i = 0; i < toks.len; i += 1) {
        if (i != 0) {
            out[off] = '.';
            off += 1;
        }
        memcpy(out + off, parts[i].ptr, parts[i].len);
        off += parts[i].len;
    }
    return ok((pb_value){.kind = PB_STRING, .text = {.ptr = out, .len = total}});
}

static pb_eval_result call_publish(pb_eval_ctx *ctx, pb_values args) {
#ifdef TINYBLOK
    if (args.len != 1 && args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    pb_value value = args.len == 1 ? (pb_value){.kind = PB_STRING, .text = ctx->payload} : args.items[1];
#else
    if (args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    pb_value value = args.items[1];
#endif
    if (value.kind == PB_NIL) {
        return ok((pb_value){.kind = PB_NIL});
    }

    pb_slice subject = {0};
    pb_slice payload = {0};
    if (!as_string(args.items[0], &subject) || !coerce_payload(ctx, value, &payload)) {
        return fail(PB_EVAL_TYPE);
    }
    if (ctx->publish == NULL || !ctx->publish(ctx->publish_ctx, subject, payload)) {
        return fail(PB_EVAL_PUBLISH_FAILED);
    }
    return ok((pb_value){.kind = PB_NIL});
}

static bool encode_state_value(pb_eval_ctx *ctx, pb_value v, pb_slice *out) {
    if (v.kind == PB_NUMBER) {
        return coerce_payload(ctx, v, out);
    }
    if (v.kind == PB_STRING || v.kind == PB_SYMBOL || v.kind == PB_BOOL || v.kind == PB_NIL) {
        return as_string(v, out);
    }
    return false;
}

static pb_eval_result stateful_changed(pb_eval_ctx *ctx, const char *op, pb_value v, bool bool_mode) {
    pb_eval_state_entry *slot = state_slot(ctx, op);
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }

    bool changed = true;
    if (v.kind == PB_NUMBER) {
        changed = slot->kind != PB_EVAL_STATE_NUMBER || slot->number != v.number;
        slot->kind = PB_EVAL_STATE_NUMBER;
        slot->number = v.number;
    } else {
        pb_slice bytes = {0};
        if (!encode_state_value(ctx, v, &bytes)) {
            return fail(PB_EVAL_TYPE);
        }
        changed = slot->kind != PB_EVAL_STATE_BYTES ||
                  slot->bytes_len != bytes.len ||
                  memcmp(slot->bytes, bytes.ptr, bytes.len) != 0;
        if (changed && !state_set_bytes(slot, bytes)) {
            return fail(PB_EVAL_OOM);
        }
    }
    return ok(bool_mode ? bool_value(changed) : (changed ? v : (pb_value){.kind = PB_NIL}));
}

static pb_eval_result call_squelch(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 1) {
        return fail(PB_EVAL_ARITY);
    }
    return stateful_changed(ctx, "squelch", args.items[0], false);
}

static pb_eval_result call_changed(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 1) {
        return fail(PB_EVAL_ARITY);
    }
    return stateful_changed(ctx, "changed?", args.items[0], true);
}

static pb_eval_result call_hold_off(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    double window_ms = 0;
    if (!as_number(args.items[0], &window_ms) || window_ms < 0 || floor(window_ms) != window_ms) {
        return fail(PB_EVAL_TYPE);
    }

    pb_eval_state_entry *slot = state_slot(ctx, "hold-off");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    if (slot->kind == PB_EVAL_STATE_EMPTY) {
        slot->kind = PB_EVAL_STATE_NUMBER;
        slot->number = (double)ctx->now_ms;
        return ok(args.items[1]);
    }
    if (slot->kind != PB_EVAL_STATE_NUMBER) {
        return fail(PB_EVAL_TYPE);
    }

    const double now_ms = (double)ctx->now_ms;
    if (now_ms < slot->number || now_ms - slot->number < window_ms) {
        return ok((pb_value){.kind = PB_NIL});
    }
    slot->number = now_ms;
    return ok(args.items[1]);
}

static pb_eval_result call_edge(pb_eval_ctx *ctx, pb_values args, bool rising) {
    if (args.len != 1) {
        return fail(PB_EVAL_ARITY);
    }

    const bool cur = truthy(args.items[0]);
    pb_eval_state_entry *slot = state_slot(ctx, rising ? "rising-edge" : "falling-edge");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    if (slot->kind == PB_EVAL_STATE_EMPTY) {
        slot->kind = PB_EVAL_STATE_NUMBER;
        slot->number = cur ? 1.0 : 0.0;
        return ok((pb_value){.kind = PB_NIL});
    }
    if (slot->kind != PB_EVAL_STATE_NUMBER) {
        return fail(PB_EVAL_TYPE);
    }

    const bool prev = slot->number != 0.0;
    slot->number = cur ? 1.0 : 0.0;
    const bool fire = rising ? (!prev && cur) : (prev && !cur);
    return ok(fire ? bool_value(true) : (pb_value){.kind = PB_NIL});
}

static pb_eval_result call_deadband(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    double threshold = 0;
    double x = 0;
    if (!as_number(args.items[0], &threshold) || !as_number(args.items[1], &x) || threshold < 0) {
        return fail(PB_EVAL_TYPE);
    }

    pb_eval_state_entry *slot = state_slot(ctx, "deadband");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    const bool pass = slot->kind != PB_EVAL_STATE_NUMBER || fabs(x - slot->number) >= threshold;
    if (!pass) {
        return ok((pb_value){.kind = PB_NIL});
    }
    slot->kind = PB_EVAL_STATE_NUMBER;
    slot->number = x;
    return ok((pb_value){.kind = PB_NUMBER, .number = x});
}

static pb_eval_result call_delta(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 1) {
        return fail(PB_EVAL_ARITY);
    }
    double x = 0;
    if (!as_number(args.items[0], &x)) {
        return fail(PB_EVAL_TYPE);
    }

    pb_eval_state_entry *slot = state_slot(ctx, "delta");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    const double prev = slot->kind == PB_EVAL_STATE_NUMBER ? slot->number : x;
    slot->kind = PB_EVAL_STATE_NUMBER;
    slot->number = x;
    return ok((pb_value){.kind = PB_NUMBER, .number = x - prev});
}

static pb_eval_result call_count(pb_eval_ctx *ctx, pb_values args) {
    if (args.len > 1) {
        return fail(PB_EVAL_ARITY);
    }
    const bool inc = args.len == 0 || truthy(args.items[0]);
    pb_eval_state_entry *slot = state_slot(ctx, "count");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    if (!inc) {
        return ok((pb_value){.kind = PB_NIL});
    }

    const double next = (slot->kind == PB_EVAL_STATE_NUMBER ? slot->number : 0) + 1;
    slot->kind = PB_EVAL_STATE_NUMBER;
    slot->number = next;

    const size_t subject_len = ctx->subject.len + 6;
    char *subject_ptr = pb_arena_alloc(ctx->arena, subject_len, 1);
    if (subject_ptr == NULL) {
        return fail(PB_EVAL_OOM);
    }
    memcpy(subject_ptr, ctx->subject.ptr, ctx->subject.len);
    memcpy(subject_ptr + ctx->subject.len, ".count", 6);

    char tmp[32];
    const int n = snprintf(tmp, sizeof tmp, "%.0f", next);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        return fail(PB_EVAL_TYPE);
    }
    char *payload_ptr = pb_arena_memdup(ctx->arena, tmp, (size_t)n);
    if (payload_ptr == NULL) {
        return fail(PB_EVAL_OOM);
    }
    if (ctx->publish == NULL ||
        !ctx->publish(ctx->publish_ctx, (pb_slice){.ptr = subject_ptr, .len = subject_len},
                      (pb_slice){.ptr = payload_ptr, .len = (size_t)n})) {
        return fail(PB_EVAL_PUBLISH_FAILED);
    }
    return ok((pb_value){.kind = PB_NIL});
}

typedef struct bar_close {
    double open;
    double high;
    double low;
    double close;
} bar_close;

static bool emit_one_bar_field(pb_eval_ctx *ctx, const char *name, double value) {
    const size_t name_len = strlen(name);
    const size_t subject_len = ctx->subject.len + 5 + name_len;
    char *subject_ptr = pb_arena_alloc(ctx->arena, subject_len, 1);
    if (subject_ptr == NULL) {
        return false;
    }
    memcpy(subject_ptr, ctx->subject.ptr, ctx->subject.len);
    memcpy(subject_ptr + ctx->subject.len, ".bar.", 5);
    memcpy(subject_ptr + ctx->subject.len + 5, name, name_len);

    char tmp[32];
    const int n = snprintf(tmp, sizeof tmp, "%.17g", value);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        return false;
    }
    char *payload_ptr = pb_arena_memdup(ctx->arena, tmp, (size_t)n);
    if (payload_ptr == NULL) {
        return false;
    }
    return ctx->publish != NULL &&
           ctx->publish(ctx->publish_ctx,
                        (pb_slice){.ptr = subject_ptr, .len = subject_len},
                        (pb_slice){.ptr = payload_ptr, .len = (size_t)n});
}

static pb_eval_result emit_bar_fields(pb_eval_ctx *ctx, bar_close c) {
    if (!emit_one_bar_field(ctx, "open", c.open) ||
        !emit_one_bar_field(ctx, "high", c.high) ||
        !emit_one_bar_field(ctx, "low", c.low) ||
        !emit_one_bar_field(ctx, "close", c.close)) {
        return fail(PB_EVAL_PUBLISH_FAILED);
    }
    return ok((pb_value){.kind = PB_NIL});
}

static bool bar_tick_update(pb_eval_state_entry *slot, double x, bar_close *out) {
    if (slot->bar_count == 0) {
        slot->bar_open = x;
        slot->bar_high = x;
        slot->bar_low = x;
        slot->bar_count = 1;
    } else {
        if (x > slot->bar_high) slot->bar_high = x;
        if (x < slot->bar_low) slot->bar_low = x;
        slot->bar_count += 1;
    }
    if (slot->bar_count < slot->bar_cap) {
        return false;
    }
    *out = (bar_close){.open = slot->bar_open, .high = slot->bar_high, .low = slot->bar_low, .close = x};
    slot->bar_count = 0;
    return true;
}

static bool bar_time_update(pb_eval_state_entry *slot, uint64_t now, double x, bar_close *out) {
    const uint64_t aligned = (now / slot->bar_window_ms) * slot->bar_window_ms;
    if (slot->bar_count == 0) {
        slot->bar_open = x;
        slot->bar_high = x;
        slot->bar_low = x;
        slot->bar_count = 1;
        slot->bar_window_start_ms = aligned;
        slot->bar_last_close = x;
        return false;
    }
    if (slot->bar_window_start_ms != aligned) {
        *out = (bar_close){
            .open = slot->bar_open,
            .high = slot->bar_high,
            .low = slot->bar_low,
            .close = slot->bar_last_close,
        };
        slot->bar_open = x;
        slot->bar_high = x;
        slot->bar_low = x;
        slot->bar_count = 1;
        slot->bar_window_start_ms = aligned;
        slot->bar_last_close = x;
        return true;
    }
    if (x > slot->bar_high) slot->bar_high = x;
    if (x < slot->bar_low) slot->bar_low = x;
    slot->bar_count += 1;
    slot->bar_last_close = x;
    return false;
}

static bool bar_time_tick(pb_eval_state_entry *slot, uint64_t now, bar_close *out) {
    if (slot->kind != PB_EVAL_STATE_BAR || !slot->bar_time_window || slot->bar_count == 0) {
        return false;
    }
    if (now - slot->bar_window_start_ms < slot->bar_window_ms) {
        return false;
    }
    *out = (bar_close){
        .open = slot->bar_open,
        .high = slot->bar_high,
        .low = slot->bar_low,
        .close = slot->bar_last_close,
    };
    slot->bar_count = 0;
    slot->bar_window_start_ms = 0;
    return true;
}

static pb_eval_result call_bar(pb_eval_ctx *ctx, pb_values args) {
    bool time_window = false;
    uint32_t count = 0;
    uint64_t window_ms = 0;
    pb_value input = {0};

    if (args.len == 2) {
        double n = 0;
        if (!as_number(args.items[0], &n) || n < 1 || floor(n) != n || n > UINT32_MAX) {
            return fail(PB_EVAL_TYPE);
        }
        count = (uint32_t)n;
        input = args.items[1];
    } else if (args.len == 3 &&
               args.items[0].kind == PB_KEYWORD &&
               text_eq(args.items[0].text, "ms")) {
        double ms = 0;
        if (!as_number(args.items[1], &ms) || ms < 1 || floor(ms) != ms) {
            return fail(PB_EVAL_TYPE);
        }
        time_window = true;
        window_ms = (uint64_t)ms;
        input = args.items[2];
    } else {
        return fail(PB_EVAL_ARITY);
    }

    double x = 0;
    if (!as_number(input, &x)) {
        return fail(PB_EVAL_TYPE);
    }

    pb_eval_state_entry *slot = state_slot(ctx, time_window ? "bar/m" : "bar/t");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    if (slot->kind == PB_EVAL_STATE_EMPTY) {
        slot->kind = PB_EVAL_STATE_BAR;
        slot->bar_time_window = time_window;
        slot->bar_cap = count;
        slot->bar_window_ms = window_ms;
    }
    if (slot->kind != PB_EVAL_STATE_BAR || slot->bar_time_window != time_window) {
        return fail(PB_EVAL_TYPE);
    }

    bar_close close = {0};
    const bool closed = time_window ? bar_time_update(slot, ctx->now_ms, x, &close) : bar_tick_update(slot, x, &close);
    if (!closed) {
        return ok((pb_value){.kind = PB_NIL});
    }
    return emit_bar_fields(ctx, close);
}

static bool ring_reserve(pb_eval_state_entry *slot, size_t cap, bool with_times) {
    if (slot->ring_cap >= cap) {
        return true;
    }

    double *values = malloc(cap * sizeof values[0]);
    if (values == NULL) {
        return false;
    }
    uint64_t *times = NULL;
    if (with_times) {
        times = malloc(cap * sizeof times[0]);
        if (times == NULL) {
            free(values);
            return false;
        }
    }

    for (size_t i = 0; i < slot->ring_len; i += 1) {
        const size_t old_idx = slot->ring_cap == 0 ? 0 : (slot->ring_start + i) % slot->ring_cap;
        values[i] = slot->ring_values[old_idx];
        if (with_times && slot->ring_times_ms != NULL) {
            times[i] = slot->ring_times_ms[old_idx];
        }
    }

    free(slot->ring_values);
    free(slot->ring_times_ms);
    slot->ring_values = values;
    slot->ring_times_ms = times;
    slot->ring_cap = cap;
    slot->ring_start = 0;
    return true;
}

static void ring_push_count(pb_eval_state_entry *slot, double x) {
    if (slot->ring_len < slot->ring_cap) {
        const size_t idx = (slot->ring_start + slot->ring_len) % slot->ring_cap;
        slot->ring_values[idx] = x;
        slot->ring_len += 1;
        slot->ring_sum += x;
        return;
    }
    const size_t idx = slot->ring_start;
    slot->ring_sum -= slot->ring_values[idx];
    slot->ring_values[idx] = x;
    slot->ring_sum += x;
    slot->ring_start = (slot->ring_start + 1) % slot->ring_cap;
}

static void ring_evict_time(pb_eval_state_entry *slot, uint64_t cutoff_ms) {
    while (slot->ring_len > 0) {
        const size_t idx = slot->ring_start;
        if (slot->ring_times_ms[idx] >= cutoff_ms) {
            break;
        }
        slot->ring_sum -= slot->ring_values[idx];
        slot->ring_start = (slot->ring_start + 1) % slot->ring_cap;
        slot->ring_len -= 1;
    }
}

static bool ring_push_time(pb_eval_state_entry *slot, double x, uint64_t t_ms) {
    if (slot->ring_len == slot->ring_cap) {
        const size_t next = slot->ring_cap == 0 ? 16 : slot->ring_cap * 2;
        if (!ring_reserve(slot, next, true)) {
            return false;
        }
    }
    const size_t idx = (slot->ring_start + slot->ring_len) % slot->ring_cap;
    slot->ring_values[idx] = x;
    slot->ring_times_ms[idx] = t_ms;
    slot->ring_len += 1;
    slot->ring_sum += x;
    return true;
}

static pb_eval_result call_moving_avg(pb_eval_ctx *ctx, pb_values args) {
    bool time_window = false;
    size_t window_count = 0;
    uint64_t window_ms = 0;
    pb_value input = {0};

    if (args.len == 2) {
        double window = 0;
        if (!as_number(args.items[0], &window) || window < 1 || floor(window) != window) {
            return fail(PB_EVAL_TYPE);
        }
        window_count = (size_t)window;
        input = args.items[1];
    } else if (args.len == 3 &&
               args.items[0].kind == PB_KEYWORD &&
               text_eq(args.items[0].text, "ms")) {
        double ms = 0;
        if (!as_number(args.items[1], &ms) || ms < 1 || floor(ms) != ms) {
            return fail(PB_EVAL_TYPE);
        }
        time_window = true;
        window_ms = (uint64_t)ms;
        input = args.items[2];
    } else {
        return fail(PB_EVAL_ARITY);
    }

    double x = 0;
    if (!as_number(input, &x)) {
        return fail(PB_EVAL_TYPE);
    }

    pb_eval_state_entry *slot = state_slot(ctx, time_window ? "moving-avg/m" : "moving-avg/t");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    if (slot->kind == PB_EVAL_STATE_EMPTY) {
        slot->kind = PB_EVAL_STATE_RING;
        slot->ring_time_window = time_window;
        slot->ring_window_ms = window_ms;
        if (!time_window && !ring_reserve(slot, window_count, false)) {
            return fail(PB_EVAL_OOM);
        }
    }
    if (slot->kind != PB_EVAL_STATE_RING || slot->ring_time_window != time_window) {
        return fail(PB_EVAL_TYPE);
    }

    if (time_window) {
        const uint64_t configured = slot->ring_window_ms == 0 ? window_ms : slot->ring_window_ms;
        const uint64_t t = ctx->now_ms;
        const uint64_t cutoff = t > configured ? t - configured : 0;
        ring_evict_time(slot, cutoff);
        if (!ring_push_time(slot, x, t)) {
            return fail(PB_EVAL_OOM);
        }
        ring_evict_time(slot, cutoff);
    } else {
        ring_push_count(slot, x);
    }

    if (slot->ring_len == 0) {
        return ok((pb_value){.kind = PB_NIL});
    }
    return ok((pb_value){.kind = PB_NUMBER, .number = slot->ring_sum / (double)slot->ring_len});
}

#if PB_ENABLE_JSON
static bool path_token(pb_slice path, size_t *pos, pb_slice *out) {
    if (*pos >= path.len) {
        return false;
    }
    const size_t start = *pos;
    while (*pos < path.len && path.ptr[*pos] != '.') {
        *pos += 1;
    }
    if (*pos == start) {
        return false;
    }
    *out = (pb_slice){.ptr = path.ptr + start, .len = *pos - start};
    if (*pos < path.len && path.ptr[*pos] == '.') {
        *pos += 1;
    }
    return true;
}

static bool token_index(pb_slice s, size_t *out) {
    if (s.len == 0) {
        return false;
    }
    size_t n = 0;
    for (size_t i = 0; i < s.len; i += 1) {
        if (s.ptr[i] < '0' || s.ptr[i] > '9') {
            return false;
        }
        n = n * 10 + (size_t)(s.ptr[i] - '0');
    }
    *out = n;
    return true;
}

static yyjson_val *json_lookup(yyjson_val *root, pb_slice path) {
    yyjson_val *cur = root;
    size_t pos = 0;
    pb_slice tok = {0};
    while (path_token(path, &pos, &tok)) {
        if (yyjson_is_obj(cur)) {
            cur = yyjson_obj_getn(cur, tok.ptr, tok.len);
        } else if (yyjson_is_arr(cur)) {
            size_t idx = 0;
            cur = token_index(tok, &idx) ? yyjson_arr_get(cur, idx) : NULL;
        } else {
            return NULL;
        }
        if (cur == NULL) {
            return NULL;
        }
    }
    return pos == path.len ? cur : NULL;
}

static void *json_arena_malloc(void *ctx, size_t size) {
    return pb_arena_alloc(ctx, size == 0 ? 1 : size, _Alignof(max_align_t));
}

static void *json_arena_realloc(void *ctx, void *ptr, size_t old_size, size_t size) {
    void *next = pb_arena_alloc(ctx, size == 0 ? 1 : size, _Alignof(max_align_t));
    if (next == NULL) {
        return NULL;
    }
    if (ptr != NULL) {
        memcpy(next, ptr, old_size < size ? old_size : size);
    }
    return next;
}

static void json_arena_free(void *ctx, void *ptr) {
    (void)ctx;
    (void)ptr;
}

static yyjson_doc *read_json_slice(pb_eval_ctx *ctx, pb_slice source) {
    char *owned = pb_arena_memdup(ctx->arena, source.ptr, source.len);
    if (owned == NULL) {
        return NULL;
    }
    yyjson_alc alc = {
        .malloc = json_arena_malloc,
        .realloc = json_arena_realloc,
        .free = json_arena_free,
        .ctx = ctx->arena,
    };
    return yyjson_read_opts(owned, source.len, YYJSON_READ_NOFLAG, &alc, NULL);
}

static pb_eval_result json_scalar_value(pb_eval_ctx *ctx, yyjson_val *v) {
    if (v == NULL || yyjson_is_null(v) || yyjson_is_obj(v) || yyjson_is_arr(v)) {
        return ok((pb_value){.kind = PB_NIL});
    }
    if (yyjson_is_bool(v)) {
        return ok((pb_value){.kind = PB_BOOL, .boolean = yyjson_get_bool(v)});
    }
    if (yyjson_is_num(v)) {
        return ok((pb_value){.kind = PB_NUMBER, .number = yyjson_get_num(v)});
    }
    if (yyjson_is_str(v)) {
        const size_t len = yyjson_get_len(v);
        char *owned = pb_arena_memdup(ctx->arena, yyjson_get_str(v), len);
        if (owned == NULL) {
            return fail(PB_EVAL_OOM);
        }
        return ok((pb_value){.kind = PB_STRING, .text = {.ptr = owned, .len = len}});
    }
    return ok((pb_value){.kind = PB_NIL});
}

static pb_eval_result call_json_get(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    pb_slice path = {0};
    pb_slice payload = {0};
    if (!as_string(args.items[0], &path) || !as_string(args.items[1], &payload) || path.len == 0) {
        return fail(PB_EVAL_TYPE);
    }

    yyjson_doc *doc = read_json_slice(ctx, payload);
    if (doc == NULL) {
        return ok((pb_value){.kind = PB_NIL});
    }
    yyjson_val *found = json_lookup(yyjson_doc_get_root(doc), path);
    const pb_eval_result r = json_scalar_value(ctx, found);
    yyjson_doc_free(doc);
    return r;
}

static pb_slice path_leaf(pb_slice path) {
    size_t start = 0;
    for (size_t i = 0; i < path.len; i += 1) {
        if (path.ptr[i] == '.') {
            start = i + 1;
        }
    }
    return (pb_slice){.ptr = path.ptr + start, .len = path.len - start};
}

static bool demux_spec(pb_value spec, pb_slice *path, pb_slice *suffix) {
    if (spec.kind == PB_STRING || spec.kind == PB_SYMBOL) {
        *path = spec.text;
        *suffix = path_leaf(spec.text);
        return path->len != 0 && suffix->len != 0;
    }
    if (spec.kind == PB_VECTOR &&
        spec.seq.len == 2 &&
        (spec.seq.items[0].kind == PB_STRING || spec.seq.items[0].kind == PB_SYMBOL) &&
        (spec.seq.items[1].kind == PB_STRING || spec.seq.items[1].kind == PB_SYMBOL)) {
        *path = spec.seq.items[0].text;
        *suffix = spec.seq.items[1].text;
        return path->len != 0 && suffix->len != 0;
    }
    return false;
}

static bool publish_json_demux_one(pb_eval_ctx *ctx, yyjson_val *root, pb_value spec) {
    pb_slice path = {0};
    pb_slice suffix = {0};
    if (!demux_spec(spec, &path, &suffix)) {
        return false;
    }

    pb_eval_result scalar = json_scalar_value(ctx, json_lookup(root, path));
    if (scalar.err != PB_EVAL_OK) {
        return false;
    }
    if (scalar.value.kind == PB_NIL) {
        return true;
    }

    const size_t subject_len = ctx->subject.len + 1 + suffix.len;
    char *subject_ptr = pb_arena_alloc(ctx->arena, subject_len, 1);
    if (subject_ptr == NULL) {
        return false;
    }
    memcpy(subject_ptr, ctx->subject.ptr, ctx->subject.len);
    subject_ptr[ctx->subject.len] = '.';
    memcpy(subject_ptr + ctx->subject.len + 1, suffix.ptr, suffix.len);

    pb_slice payload = {0};
    if (!coerce_payload(ctx, scalar.value, &payload)) {
        return false;
    }
    return ctx->publish != NULL &&
           ctx->publish(ctx->publish_ctx, (pb_slice){.ptr = subject_ptr, .len = subject_len}, payload);
}

static pb_eval_result call_json_demux(pb_eval_ctx *ctx, pb_values args) {
    if (args.len < 2) {
        return fail(PB_EVAL_ARITY);
    }
    pb_slice payload = {0};
    if (!as_string(args.items[args.len - 1], &payload)) {
        return fail(PB_EVAL_TYPE);
    }

    yyjson_doc *doc = read_json_slice(ctx, payload);
    if (doc == NULL) {
        return ok((pb_value){.kind = PB_NIL});
    }

    bool ok_publish = true;
    if (args.len == 2 && args.items[0].kind == PB_VECTOR) {
        for (size_t i = 0; i < args.items[0].seq.len; i += 1) {
            if (!publish_json_demux_one(ctx, yyjson_doc_get_root(doc), args.items[0].seq.items[i])) {
                ok_publish = false;
                break;
            }
        }
    } else {
        for (size_t i = 0; i + 1 < args.len; i += 1) {
            if (!publish_json_demux_one(ctx, yyjson_doc_get_root(doc), args.items[i])) {
                ok_publish = false;
                break;
            }
        }
    }
    yyjson_doc_free(doc);
    if (!ok_publish) {
        return fail(PB_EVAL_PUBLISH_FAILED);
    }
    return ok((pb_value){.kind = PB_NIL});
}
#endif

pb_eval_result pb_eval_call_builtin(pb_eval_ctx *ctx, pb_builtin builtin, pb_values args) {
    switch (builtin) {
    case PB_BUILTIN_NOW: return call_now(ctx, args);
    case PB_BUILTIN_NOT: return call_not(args);
    case PB_BUILTIN_EQ: return call_eq(args);
    case PB_BUILTIN_GT: return call_cmp(args, CMP_GT);
    case PB_BUILTIN_LT: return call_cmp(args, CMP_LT);
    case PB_BUILTIN_GE: return call_cmp(args, CMP_GE);
    case PB_BUILTIN_LE: return call_cmp(args, CMP_LE);
    case PB_BUILTIN_ADD: return call_arith(args, ARITH_ADD);
    case PB_BUILTIN_SUB: return call_arith(args, ARITH_SUB);
    case PB_BUILTIN_MUL: return call_arith(args, ARITH_MUL);
    case PB_BUILTIN_DIV: return call_arith(args, ARITH_DIV);
    case PB_BUILTIN_STR_CONCAT: return call_str_concat(ctx, args);
    case PB_BUILTIN_CONTAINS: return call_contains(args);
    case PB_BUILTIN_STARTS_WITH: return call_affix(args, AFFIX_STARTS);
    case PB_BUILTIN_ENDS_WITH: return call_affix(args, AFFIX_ENDS);
    case PB_BUILTIN_SUBJECT_APPEND: return call_subject_append(ctx, args);
    case PB_BUILTIN_SUBJECT_TOKEN: return call_subject_token(ctx, args);
    case PB_BUILTIN_SUBJECT_WITH: return call_subject_with(ctx, args);
    case PB_BUILTIN_PUBLISH: return call_publish(ctx, args);
#if PB_ENABLE_JSON
    case PB_BUILTIN_JSON_GET: return call_json_get(ctx, args);
    case PB_BUILTIN_JSON_DEMUX: return call_json_demux(ctx, args);
#else
    case PB_BUILTIN_JSON_GET:
    case PB_BUILTIN_JSON_DEMUX:
        return fail(PB_EVAL_UNKNOWN_SYMBOL);
#endif
    case PB_BUILTIN_ROUND: return call_round(args);
    case PB_BUILTIN_QUANTIZE: return call_quantize(args);
    case PB_BUILTIN_CLAMP: return call_clamp(args);
    case PB_BUILTIN_MIN: return call_minmax(args, MINMAX_MIN);
    case PB_BUILTIN_MAX: return call_minmax(args, MINMAX_MAX);
    case PB_BUILTIN_ABS: return call_abs(args);
    case PB_BUILTIN_SIGN: return call_sign(args);
    case PB_BUILTIN_SQUELCH: return call_squelch(ctx, args);
    case PB_BUILTIN_DEADBAND: return call_deadband(ctx, args);
    case PB_BUILTIN_CHANGED: return call_changed(ctx, args);
    case PB_BUILTIN_HOLD_OFF: return call_hold_off(ctx, args);
    case PB_BUILTIN_RISING_EDGE: return call_edge(ctx, args, true);
    case PB_BUILTIN_FALLING_EDGE: return call_edge(ctx, args, false);
    case PB_BUILTIN_DELTA: return call_delta(ctx, args);
    case PB_BUILTIN_COUNT: return call_count(ctx, args);
    case PB_BUILTIN_MOVING_SUM:
    case PB_BUILTIN_MOVING_MAX:
    case PB_BUILTIN_MOVING_MIN:
    case PB_BUILTIN_MEDIAN:
    case PB_BUILTIN_PERCENTILE:
    case PB_BUILTIN_STDDEV:
    case PB_BUILTIN_VARIANCE:
    case PB_BUILTIN_RATE:
    case PB_BUILTIN_THROTTLE:
    case PB_BUILTIN_DEBOUNCE:
    case PB_BUILTIN_SAMPLE:
    case PB_BUILTIN_AGGREGATE:
        return pb_eval_call_window_builtin(ctx, builtin, args);
    case PB_BUILTIN_MOVING_AVG: return call_moving_avg(ctx, args);
    case PB_BUILTIN_BAR: return call_bar(ctx, args);
    case PB_BUILTIN_DO:
    case PB_BUILTIN_IF:
    case PB_BUILTIN_WHEN:
    case PB_BUILTIN_AND:
    case PB_BUILTIN_OR:
    case PB_BUILTIN_THREAD:
    case PB_BUILTIN_TRANSITION:
    case PB_BUILTIN_DROPOUT:
        break;
    }
    return fail(PB_EVAL_UNKNOWN_SYMBOL);
}

pb_eval_result pb_eval_tick_state_entry(pb_eval_ctx *ctx, pb_eval_state_entry *entry) {
    if (entry->kind == PB_EVAL_STATE_CLOCK) {
        return pb_eval_tick_clock_state_entry(ctx, entry);
    }
    if (entry->kind == PB_EVAL_STATE_RING && entry->ring_time_window && entry->ring_len > 0) {
        const uint64_t cutoff = ctx->now_ms > entry->ring_window_ms ? ctx->now_ms - entry->ring_window_ms : 0;
        ring_evict_time(entry, cutoff);
    }
    bar_close close = {0};
    if (bar_time_tick(entry, ctx->now_ms, &close)) {
        return emit_bar_fields(ctx, close);
    }
    return ok((pb_value){.kind = PB_NIL});
}
