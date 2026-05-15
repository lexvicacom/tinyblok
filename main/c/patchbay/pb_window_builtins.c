#include "pb_eval_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum window_calc {
    WINDOW_SUM,
    WINDOW_AVG,
    WINDOW_MIN,
    WINDOW_MAX,
    WINDOW_VARIANCE,
    WINDOW_STDDEV,
} window_calc;

static bool window_op(char *buf, size_t buf_len, const char *name, bool time_window) {
    const int n = snprintf(buf, buf_len, "%s/%c", name, time_window ? 'm' : 't');
    return n > 0 && (size_t)n < buf_len;
}

static bool parse_count(double n, size_t *out) {
    if (n < 1 || floor(n) != n) {
        return false;
    }
    *out = (size_t)n;
    return true;
}

static bool parse_window(pb_values args, size_t *pos, bool *time_window, size_t *count, uint64_t *ms) {
    *pos = 0;
    *time_window = false;
    *count = 0;
    *ms = 0;
    if (args.len >= 2 && args.items[0].kind == PB_KEYWORD && text_eq(args.items[0].text, "ms")) {
        double n = 0;
        if (!as_number(args.items[1], &n) || n < 1 || floor(n) != n) {
            return false;
        }
        *time_window = true;
        *ms = (uint64_t)n;
        *pos = 2;
        return true;
    }
    double n = 0;
    if (args.len >= 1 && as_number(args.items[0], &n) && parse_count(n, count)) {
        *pos = 1;
        return true;
    }
    return false;
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

static pb_eval_state_entry *window_slot(pb_eval_ctx *ctx, const char *name, bool time_window, size_t count, uint64_t ms) {
    char op[64];
    if (!window_op(op, sizeof op, name, time_window)) {
        return NULL;
    }
    pb_eval_state_entry *slot = state_slot(ctx, op);
    if (slot == NULL) {
        return NULL;
    }
    if (slot->kind == PB_EVAL_STATE_EMPTY) {
        slot->kind = PB_EVAL_STATE_RING;
        slot->ring_time_window = time_window;
        slot->ring_window_ms = ms;
        if (!time_window && !ring_reserve(slot, count, false)) {
            return NULL;
        }
    }
    if (slot->kind != PB_EVAL_STATE_RING || slot->ring_time_window != time_window) {
        return NULL;
    }
    return slot;
}

static bool window_push(pb_eval_state_entry *slot, bool time_window, size_t count, uint64_t ms, uint64_t now_ms, double x) {
    if (time_window) {
        const uint64_t configured = slot->ring_window_ms == 0 ? ms : slot->ring_window_ms;
        const uint64_t cutoff = now_ms > configured ? now_ms - configured : 0;
        ring_evict_time(slot, cutoff);
        if (!ring_push_time(slot, x, now_ms)) {
            return false;
        }
    } else {
        if (slot->ring_cap == 0 && !ring_reserve(slot, count, false)) {
            return false;
        }
        ring_push_count(slot, x);
    }
    return true;
}

static double ring_value_at(const pb_eval_state_entry *slot, size_t i) {
    const size_t idx = slot->ring_cap == 0 ? 0 : (slot->ring_start + i) % slot->ring_cap;
    return slot->ring_values[idx];
}

static pb_eval_result calc_ring(const pb_eval_state_entry *slot, window_calc calc) {
    if (slot->ring_len == 0) {
        return ok((pb_value){.kind = PB_NIL});
    }
    if (calc == WINDOW_SUM) {
        return ok((pb_value){.kind = PB_NUMBER, .number = slot->ring_sum});
    }
    if (calc == WINDOW_AVG) {
        return ok((pb_value){.kind = PB_NUMBER, .number = slot->ring_sum / (double)slot->ring_len});
    }
    double acc = ring_value_at(slot, 0);
    if (calc == WINDOW_MIN || calc == WINDOW_MAX) {
        for (size_t i = 1; i < slot->ring_len; i += 1) {
            const double x = ring_value_at(slot, i);
            if (calc == WINDOW_MIN && x < acc) acc = x;
            if (calc == WINDOW_MAX && x > acc) acc = x;
        }
        return ok((pb_value){.kind = PB_NUMBER, .number = acc});
    }
    const double mean = slot->ring_sum / (double)slot->ring_len;
    double sumsq = 0;
    for (size_t i = 0; i < slot->ring_len; i += 1) {
        const double d = ring_value_at(slot, i) - mean;
        sumsq += d * d;
    }
    const double var = sumsq / (double)slot->ring_len;
    return ok((pb_value){.kind = PB_NUMBER, .number = calc == WINDOW_STDDEV ? sqrt(var) : var});
}

static pb_eval_result call_window_calc(pb_eval_ctx *ctx, pb_values args, const char *op, window_calc calc) {
    size_t pos = 0;
    size_t count = 0;
    uint64_t ms = 0;
    bool time_window = false;
    if (!parse_window(args, &pos, &time_window, &count, &ms) || pos + 1 != args.len) {
        return fail(PB_EVAL_ARITY);
    }
    double x = 0;
    if (!as_number(args.items[pos], &x)) {
        return fail(PB_EVAL_TYPE);
    }
    pb_eval_state_entry *slot = window_slot(ctx, op, time_window, count, ms);
    if (slot == NULL || !window_push(slot, time_window, count, ms, ctx->now_ms, x)) {
        return fail(PB_EVAL_OOM);
    }
    return calc_ring(slot, calc);
}

static int cmp_double(const void *a, const void *b) {
    const double da = *(const double *)a;
    const double db = *(const double *)b;
    return (da > db) - (da < db);
}

static pb_eval_result call_percentile(pb_eval_ctx *ctx, pb_values args, bool median) {
    size_t pos = 0;
    size_t count = 0;
    uint64_t ms = 0;
    bool time_window = false;
    if (!parse_window(args, &pos, &time_window, &count, &ms)) {
        return fail(PB_EVAL_ARITY);
    }
    double p = 0.5;
    if (!median) {
        if (pos >= args.len || !as_number(args.items[pos], &p) || p < 0 || p > 1) {
            return fail(PB_EVAL_TYPE);
        }
        pos += 1;
    }
    if (pos + 1 != args.len) {
        return fail(PB_EVAL_ARITY);
    }
    double x = 0;
    if (!as_number(args.items[pos], &x)) {
        return fail(PB_EVAL_TYPE);
    }
    pb_eval_state_entry *slot = window_slot(ctx, median ? "median" : "percentile", time_window, count, ms);
    if (slot == NULL || !window_push(slot, time_window, count, ms, ctx->now_ms, x)) {
        return fail(PB_EVAL_OOM);
    }
    double *copy = pb_arena_alloc(ctx->arena, slot->ring_len * sizeof copy[0], _Alignof(double));
    if (copy == NULL) {
        return fail(PB_EVAL_OOM);
    }
    for (size_t i = 0; i < slot->ring_len; i += 1) {
        copy[i] = ring_value_at(slot, i);
    }
    qsort(copy, slot->ring_len, sizeof copy[0], cmp_double);
    const size_t idx = slot->ring_len == 1 ? 0 : (size_t)floor(p * (double)(slot->ring_len - 1));
    return ok((pb_value){.kind = PB_NUMBER, .number = copy[idx]});
}

static pb_eval_result call_rate(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 3 || args.items[0].kind != PB_KEYWORD || !text_eq(args.items[0].text, "ms")) {
        return fail(PB_EVAL_ARITY);
    }
    double ms_d = 0;
    if (!as_number(args.items[1], &ms_d) || ms_d < 1 || floor(ms_d) != ms_d) {
        return fail(PB_EVAL_TYPE);
    }
    pb_eval_state_entry *slot = window_slot(ctx, "rate", true, 0, (uint64_t)ms_d);
    if (slot == NULL || !window_push(slot, true, 0, (uint64_t)ms_d, ctx->now_ms, 1.0)) {
        return fail(PB_EVAL_OOM);
    }
    const double hz = (double)slot->ring_len * 1000.0 / ms_d;
    return ok((pb_value){.kind = PB_NUMBER, .number = hz});
}

static pb_eval_result call_throttle(pb_eval_ctx *ctx, pb_values args) {
#ifdef TINYBLOK
    if (args.len == 3 && args.items[0].kind == PB_KEYWORD && text_eq(args.items[0].text, "ms")) {
        double ms_d = 0;
        if (!as_number(args.items[1], &ms_d) || ms_d < 1 || floor(ms_d) != ms_d) {
            return fail(PB_EVAL_TYPE);
        }
        pb_eval_state_entry *slot = state_slot(ctx, "throttle/hold");
        if (slot == NULL) {
            return fail(PB_EVAL_OOM);
        }
        if (slot->kind == PB_EVAL_STATE_EMPTY) {
            slot->kind = PB_EVAL_STATE_NUMBER;
            slot->number = (double)ctx->now_ms;
            return ok(args.items[2]);
        }
        if (slot->kind != PB_EVAL_STATE_NUMBER) {
            return fail(PB_EVAL_TYPE);
        }
        const double now_ms = (double)ctx->now_ms;
        if (now_ms < slot->number || now_ms - slot->number < ms_d) {
            return ok((pb_value){.kind = PB_NIL});
        }
        slot->number = now_ms;
        return ok(args.items[2]);
    }
#endif

    size_t pos = 0;
    size_t count = 0;
    uint64_t ms = 0;
    bool time_window = false;
    if (!parse_window(args, &pos, &time_window, &count, &ms) || pos + 2 != args.len) {
        return fail(PB_EVAL_ARITY);
    }
    double max_d = 0;
    if (!as_number(args.items[pos], &max_d) || max_d < 1 || floor(max_d) != max_d) {
        return fail(PB_EVAL_TYPE);
    }
    const size_t max = (size_t)max_d;
    pb_eval_state_entry *slot = window_slot(ctx, "throttle", time_window, count, ms);
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    if (time_window) {
        const uint64_t cutoff = ctx->now_ms > ms ? ctx->now_ms - ms : 0;
        ring_evict_time(slot, cutoff);
        if (slot->ring_len >= max) {
            return ok((pb_value){.kind = PB_NIL});
        }
        if (!ring_push_time(slot, 1.0, ctx->now_ms)) {
            return fail(PB_EVAL_OOM);
        }
        return ok(args.items[pos + 1]);
    }
    const bool pass = slot->ring_sum < (double)max;
    ring_push_count(slot, pass ? 1.0 : 0.0);
    return ok(pass ? args.items[pos + 1] : (pb_value){.kind = PB_NIL});
}

static pb_eval_result clock_publish(pb_eval_ctx *ctx, pb_eval_state_entry *slot) {
    if (slot->bytes == NULL && slot->bytes_len != 0) {
        return fail(PB_EVAL_TYPE);
    }
    if (ctx->publish == NULL ||
        !ctx->publish(ctx->publish_ctx,
                      (pb_slice){.ptr = slot->emit_subject, .len = slot->emit_subject_len},
                      (pb_slice){.ptr = slot->bytes, .len = slot->bytes_len})) {
        return fail(PB_EVAL_PUBLISH_FAILED);
    }
    return ok((pb_value){.kind = PB_NIL});
}

static pb_eval_result retain_publish(pb_eval_ctx *ctx, const char *op, pb_eval_clock_kind kind, uint64_t ms,
                                     pb_value subject_v, pb_value value_v, bool reset_deadline) {
    pb_slice subject = {0};
    pb_slice payload = {0};
    if (!as_string(subject_v, &subject) || !coerce_payload(ctx, value_v, &payload)) {
        return fail(PB_EVAL_TYPE);
    }
    pb_eval_state_entry *slot = state_slot(ctx, op);
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    slot->clock_kind = kind;
    slot->clock_interval_ms = ms;
    slot->clock_armed = true;
    if (reset_deadline || slot->clock_deadline_ms == 0) {
        slot->clock_deadline_ms = ctx->now_ms + ms;
    }
    if (!state_set_emit_subject(slot, subject) || !state_set_bytes(slot, payload)) {
        return fail(PB_EVAL_OOM);
    }
    slot->kind = PB_EVAL_STATE_CLOCK;
    return ok((pb_value){.kind = PB_NIL});
}

static pb_eval_result call_debounce(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 4 || args.items[0].kind != PB_KEYWORD || !text_eq(args.items[0].text, "ms")) {
        return fail(PB_EVAL_ARITY);
    }
    double ms = 0;
    if (!as_number(args.items[1], &ms) || ms < 1 || floor(ms) != ms) {
        return fail(PB_EVAL_TYPE);
    }
    return retain_publish(ctx, "debounce", PB_EVAL_CLOCK_DEBOUNCE, (uint64_t)ms, args.items[2], args.items[3], true);
}

static pb_eval_result call_sample(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 4 || args.items[0].kind != PB_KEYWORD || !text_eq(args.items[0].text, "ms")) {
        return fail(PB_EVAL_ARITY);
    }
    double ms = 0;
    if (!as_number(args.items[1], &ms) || ms < 1 || floor(ms) != ms) {
        return fail(PB_EVAL_TYPE);
    }
    return retain_publish(ctx, "sample", PB_EVAL_CLOCK_SAMPLE, (uint64_t)ms, args.items[2], args.items[3], false);
}

static pb_eval_result call_aggregate(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 5 || args.items[0].kind != PB_KEYWORD || !text_eq(args.items[0].text, "ms") ||
        args.items[3].kind != PB_KEYWORD) {
        return fail(PB_EVAL_ARITY);
    }
    double ms_d = 0;
    double x = 0;
    if (!as_number(args.items[1], &ms_d) || ms_d < 1 || floor(ms_d) != ms_d || !as_number(args.items[4], &x)) {
        return fail(PB_EVAL_TYPE);
    }
    pb_eval_metric_kind metric = PB_EVAL_METRIC_NONE;
    if (text_eq(args.items[3].text, "avg")) metric = PB_EVAL_METRIC_AVG;
    if (text_eq(args.items[3].text, "sum")) metric = PB_EVAL_METRIC_SUM;
    if (text_eq(args.items[3].text, "min")) metric = PB_EVAL_METRIC_MIN;
    if (text_eq(args.items[3].text, "max")) metric = PB_EVAL_METRIC_MAX;
    if (text_eq(args.items[3].text, "count")) metric = PB_EVAL_METRIC_COUNT;
    if (text_eq(args.items[3].text, "rate")) metric = PB_EVAL_METRIC_RATE;
    if (metric == PB_EVAL_METRIC_NONE) {
        return fail(PB_EVAL_UNKNOWN_SYMBOL);
    }
    pb_slice subject = {0};
    if (!as_string(args.items[2], &subject)) {
        return fail(PB_EVAL_TYPE);
    }
    pb_eval_state_entry *slot = state_slot(ctx, "aggregate");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    if (slot->kind == PB_EVAL_STATE_EMPTY) {
        slot->clock_kind = PB_EVAL_CLOCK_AGGREGATE;
        slot->clock_interval_ms = (uint64_t)ms_d;
        slot->clock_deadline_ms = ctx->now_ms + (uint64_t)ms_d;
        slot->clock_armed = true;
    }
    slot->metric_kind = metric;
    if (!state_set_emit_subject(slot, subject)) {
        return fail(PB_EVAL_OOM);
    }
    if (!ring_reserve(slot, slot->ring_len + 1, true) || !ring_push_time(slot, x, ctx->now_ms)) {
        return fail(PB_EVAL_OOM);
    }
    slot->kind = PB_EVAL_STATE_CLOCK;
    return ok((pb_value){.kind = PB_NIL});
}

pb_eval_result pb_eval_call_dropout(pb_eval_ctx *ctx, pb_values raw_args) {
    if (raw_args.len != 6 || raw_args.items[0].kind != PB_KEYWORD || !text_eq(raw_args.items[0].text, "ms") ||
        raw_args.items[2].kind != PB_KEYWORD || !text_eq(raw_args.items[2].text, "lost") ||
        raw_args.items[4].kind != PB_KEYWORD || !text_eq(raw_args.items[4].text, "found")) {
        return fail(PB_EVAL_ARITY);
    }
    pb_eval_result ms_v = pb_eval(ctx, raw_args.items[1]);
    double ms = 0;
    if (ms_v.err != PB_EVAL_OK || !as_number(ms_v.value, &ms) || ms < 1 || floor(ms) != ms) {
        return fail(PB_EVAL_TYPE);
    }
    pb_eval_state_entry *slot = state_slot(ctx, "dropout");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    if (slot->kind == PB_EVAL_STATE_EMPTY) {
        slot->kind = PB_EVAL_STATE_CLOCK;
        slot->clock_kind = PB_EVAL_CLOCK_DROPOUT;
        slot->clock_interval_ms = (uint64_t)ms;
        slot->lost_form = raw_args.items[3];
        slot->found_form = raw_args.items[5];
    } else if (slot->kind != PB_EVAL_STATE_CLOCK || slot->clock_kind != PB_EVAL_CLOCK_DROPOUT) {
        return fail(PB_EVAL_TYPE);
    }
    slot->clock_armed = true;
    slot->clock_deadline_ms = ctx->now_ms + (uint64_t)ms;
    if (slot->dropout_lost) {
        slot->dropout_lost = false;
        return pb_eval(ctx, slot->found_form);
    }
    return ok((pb_value){.kind = PB_NIL});
}

pb_eval_result pb_eval_call_window_builtin(pb_eval_ctx *ctx, pb_builtin builtin, pb_values args) {
    switch (builtin) {
    case PB_BUILTIN_MOVING_SUM: return call_window_calc(ctx, args, "moving-sum", WINDOW_SUM);
    case PB_BUILTIN_MOVING_MAX: return call_window_calc(ctx, args, "moving-max", WINDOW_MAX);
    case PB_BUILTIN_MOVING_MIN: return call_window_calc(ctx, args, "moving-min", WINDOW_MIN);
    case PB_BUILTIN_MEDIAN: return call_percentile(ctx, args, true);
    case PB_BUILTIN_PERCENTILE: return call_percentile(ctx, args, false);
    case PB_BUILTIN_STDDEV: return call_window_calc(ctx, args, "stddev", WINDOW_STDDEV);
    case PB_BUILTIN_VARIANCE: return call_window_calc(ctx, args, "variance", WINDOW_VARIANCE);
    case PB_BUILTIN_RATE: return call_rate(ctx, args);
    case PB_BUILTIN_THROTTLE: return call_throttle(ctx, args);
    case PB_BUILTIN_DEBOUNCE: return call_debounce(ctx, args);
    case PB_BUILTIN_SAMPLE: return call_sample(ctx, args);
    case PB_BUILTIN_AGGREGATE: return call_aggregate(ctx, args);
    default: return fail(PB_EVAL_UNKNOWN_SYMBOL);
    }
}

pb_eval_result pb_eval_tick_clock_state_entry(pb_eval_ctx *ctx, pb_eval_state_entry *entry) {
    if (!entry->clock_armed || ctx->now_ms < entry->clock_deadline_ms) {
        return ok((pb_value){.kind = PB_NIL});
    }
    switch (entry->clock_kind) {
    case PB_EVAL_CLOCK_DROPOUT:
        entry->dropout_lost = true;
        entry->clock_armed = false;
        return pb_eval(ctx, entry->lost_form);
    case PB_EVAL_CLOCK_DEBOUNCE:
        entry->clock_armed = false;
        return clock_publish(ctx, entry);
    case PB_EVAL_CLOCK_SAMPLE: {
        entry->clock_deadline_ms += entry->clock_interval_ms;
        return clock_publish(ctx, entry);
    }
    case PB_EVAL_CLOCK_AGGREGATE: {
        if (entry->ring_len == 0) {
            entry->clock_deadline_ms += entry->clock_interval_ms;
            return ok((pb_value){.kind = PB_NIL});
        }
        pb_eval_result r = ok((pb_value){.kind = PB_NIL});
        switch (entry->metric_kind) {
        case PB_EVAL_METRIC_AVG: r = calc_ring(entry, WINDOW_AVG); break;
        case PB_EVAL_METRIC_SUM: r = calc_ring(entry, WINDOW_SUM); break;
        case PB_EVAL_METRIC_MIN: r = calc_ring(entry, WINDOW_MIN); break;
        case PB_EVAL_METRIC_MAX: r = calc_ring(entry, WINDOW_MAX); break;
        case PB_EVAL_METRIC_COUNT:
            r = ok((pb_value){.kind = PB_NUMBER, .number = (double)entry->ring_len});
            break;
        case PB_EVAL_METRIC_RATE:
            r = ok((pb_value){.kind = PB_NUMBER, .number = (double)entry->ring_len * 1000.0 / (double)entry->clock_interval_ms});
            break;
        case PB_EVAL_METRIC_NONE:
            return fail(PB_EVAL_TYPE);
        }
        if (r.err != PB_EVAL_OK) {
            return r;
        }
        pb_slice payload = {0};
        if (!coerce_payload(ctx, r.value, &payload) || !state_set_bytes(entry, payload)) {
            return fail(PB_EVAL_OOM);
        }
        entry->kind = PB_EVAL_STATE_CLOCK;
        entry->ring_len = 0;
        entry->ring_start = 0;
        entry->ring_sum = 0;
        entry->clock_deadline_ms += entry->clock_interval_ms;
        return clock_publish(ctx, entry);
    }
    case PB_EVAL_CLOCK_NONE:
        break;
    }
    return ok((pb_value){.kind = PB_NIL});
}
