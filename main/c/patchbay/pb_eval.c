#include "pb_eval_internal.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

pb_eval_result ok(pb_value v) {
    return (pb_eval_result){.err = PB_EVAL_OK, .value = v};
}

pb_eval_result fail(pb_eval_error err) {
    return (pb_eval_result){.err = err};
}

#ifdef TINYBLOK
__attribute__((weak)) bool pb_user_eval_symbol(pb_eval_ctx *ctx, pb_slice name, pb_eval_result *out) {
    (void)ctx;
    (void)name;
    (void)out;
    return false;
}

__attribute__((weak)) bool pb_user_eval_call(pb_eval_ctx *ctx, pb_slice name, pb_values args, pb_eval_result *out) {
    (void)ctx;
    (void)name;
    (void)args;
    (void)out;
    return false;
}
#endif

bool text_eq(pb_slice s, const char *lit) {
    const size_t n = strlen(lit);
    return s.len == n && memcmp(s.ptr, lit, n) == 0;
}

bool truthy(pb_value v) {
    return v.kind != PB_NIL && !(v.kind == PB_BOOL && !v.boolean);
}

pb_value bool_value(bool b) {
    return (pb_value){.kind = PB_BOOL, .boolean = b};
}

bool as_number(pb_value v, double *out) {
    if (v.kind == PB_NUMBER) {
        *out = v.number;
        return true;
    }
    if (v.kind != PB_STRING && v.kind != PB_SYMBOL) {
        return false;
    }
#ifdef TINYBLOK
    if (v.text.len == 0) {
        return false;
    }
    size_t i = 0;
    double sign = 1.0;
    if (v.text.ptr[i] == '-' || v.text.ptr[i] == '+') {
        sign = v.text.ptr[i] == '-' ? -1.0 : 1.0;
        i += 1;
        if (i == v.text.len) {
            return false;
        }
    }

    double n = 0.0;
    bool saw_digit = false;
    while (i < v.text.len && v.text.ptr[i] >= '0' && v.text.ptr[i] <= '9') {
        saw_digit = true;
        n = n * 10.0 + (double)(v.text.ptr[i] - '0');
        i += 1;
    }
    if (i < v.text.len && v.text.ptr[i] == '.') {
        i += 1;
        double scale = 0.1;
        while (i < v.text.len && v.text.ptr[i] >= '0' && v.text.ptr[i] <= '9') {
            saw_digit = true;
            n += (double)(v.text.ptr[i] - '0') * scale;
            scale *= 0.1;
            i += 1;
        }
    }
    if (!saw_digit || i != v.text.len) {
        return false;
    }
    *out = sign * n;
    return true;
#else
    if (v.text.len >= 128) {
        return false;
    }

    char tmp[128];
    memcpy(tmp, v.text.ptr, v.text.len);
    tmp[v.text.len] = '\0';

    errno = 0;
    char *end = NULL;
    const double n = strtod(tmp, &end);
    if (errno != 0 || end != tmp + v.text.len) {
        return false;
    }
    *out = n;
    return true;
#endif
}

bool as_string(pb_value v, pb_slice *out) {
    if (v.kind == PB_STRING || v.kind == PB_SYMBOL) {
        *out = v.text;
        return true;
    }
    if (v.kind == PB_BOOL) {
        *out = v.boolean ? (pb_slice){.ptr = "true", .len = 4} : (pb_slice){.ptr = "false", .len = 5};
        return true;
    }
    if (v.kind == PB_NIL) {
        *out = (pb_slice){.ptr = "", .len = 0};
        return true;
    }
    return false;
}

bool coerce_payload(pb_eval_ctx *ctx, pb_value v, pb_slice *out) {
    if (as_string(v, out)) {
        return true;
    }
    if (v.kind != PB_NUMBER) {
        return false;
    }

    char tmp[32];
#ifdef TINYBLOK
    int n = snprintf(tmp, sizeof tmp, "%.15g", v.number);
#else
    int n = -1;
    for (int precision = 15; precision <= 17; precision += 1) {
        n = snprintf(tmp, sizeof tmp, "%.*g", precision, v.number);
        if (n < 0 || (size_t)n >= sizeof tmp) {
            return false;
        }
        errno = 0;
        char *end = NULL;
        const double roundtrip = strtod(tmp, &end);
        if (errno == 0 && end == tmp + n && roundtrip == v.number) {
            break;
        }
    }
#endif
    if (n < 0 || (size_t)n >= sizeof tmp) {
        return false;
    }
    char *owned = pb_arena_memdup(ctx->arena, tmp, (size_t)n);
    if (owned == NULL) {
        return false;
    }
    *out = (pb_slice){.ptr = owned, .len = (size_t)n};
    return true;
}

bool value_eq(pb_value a, pb_value b) {
    if (a.kind != b.kind) {
        return false;
    }
    switch (a.kind) {
    case PB_NIL: return true;
    case PB_BOOL: return a.boolean == b.boolean;
    case PB_NUMBER: return a.number == b.number;
    case PB_SYMBOL:
    case PB_KEYWORD:
    case PB_STRING:
        return a.text.len == b.text.len && memcmp(a.text.ptr, b.text.ptr, a.text.len) == 0;
    case PB_LIST:
    case PB_VECTOR:
        return false;
    }
    return false;
}

static pb_eval_result eval_list(pb_eval_ctx *ctx, pb_values call);

typedef struct pb_builtin_entry {
    const char *name;
    pb_builtin builtin;
    bool special;
} pb_builtin_entry;

static const pb_builtin_entry BUILTINS[] = {
    {.name = "do", .builtin = PB_BUILTIN_DO, .special = true},
    {.name = "if", .builtin = PB_BUILTIN_IF, .special = true},
    {.name = "when", .builtin = PB_BUILTIN_WHEN, .special = true},
    {.name = "and", .builtin = PB_BUILTIN_AND, .special = true},
    {.name = "or", .builtin = PB_BUILTIN_OR, .special = true},
    {.name = "->", .builtin = PB_BUILTIN_THREAD, .special = true},
    {.name = "transition", .builtin = PB_BUILTIN_TRANSITION, .special = true},
    {.name = "dropout", .builtin = PB_BUILTIN_DROPOUT, .special = true},
    {.name = "now", .builtin = PB_BUILTIN_NOW},
    {.name = "not", .builtin = PB_BUILTIN_NOT},
    {.name = "=", .builtin = PB_BUILTIN_EQ},
    {.name = ">", .builtin = PB_BUILTIN_GT},
    {.name = "<", .builtin = PB_BUILTIN_LT},
    {.name = ">=", .builtin = PB_BUILTIN_GE},
    {.name = "<=", .builtin = PB_BUILTIN_LE},
    {.name = "+", .builtin = PB_BUILTIN_ADD},
    {.name = "-", .builtin = PB_BUILTIN_SUB},
    {.name = "*", .builtin = PB_BUILTIN_MUL},
    {.name = "/", .builtin = PB_BUILTIN_DIV},
    {.name = "str-concat", .builtin = PB_BUILTIN_STR_CONCAT},
    {.name = "contains?", .builtin = PB_BUILTIN_CONTAINS},
    {.name = "starts-with?", .builtin = PB_BUILTIN_STARTS_WITH},
    {.name = "ends-with?", .builtin = PB_BUILTIN_ENDS_WITH},
    {.name = "subject-append", .builtin = PB_BUILTIN_SUBJECT_APPEND},
    {.name = "subject-token", .builtin = PB_BUILTIN_SUBJECT_TOKEN},
    {.name = "subject-with", .builtin = PB_BUILTIN_SUBJECT_WITH},
    {.name = "publish!", .builtin = PB_BUILTIN_PUBLISH},
    {.name = "publish", .builtin = PB_BUILTIN_PUBLISH},
    {.name = "json-get", .builtin = PB_BUILTIN_JSON_GET},
    {.name = "json-demux!", .builtin = PB_BUILTIN_JSON_DEMUX},
    {.name = "round", .builtin = PB_BUILTIN_ROUND},
    {.name = "quantize", .builtin = PB_BUILTIN_QUANTIZE},
    {.name = "clamp", .builtin = PB_BUILTIN_CLAMP},
    {.name = "min", .builtin = PB_BUILTIN_MIN},
    {.name = "max", .builtin = PB_BUILTIN_MAX},
    {.name = "abs", .builtin = PB_BUILTIN_ABS},
    {.name = "sign", .builtin = PB_BUILTIN_SIGN},
    {.name = "squelch", .builtin = PB_BUILTIN_SQUELCH},
    {.name = "deadband", .builtin = PB_BUILTIN_DEADBAND},
    {.name = "changed?", .builtin = PB_BUILTIN_CHANGED},
    {.name = "hold-off", .builtin = PB_BUILTIN_HOLD_OFF},
    {.name = "rising-edge", .builtin = PB_BUILTIN_RISING_EDGE},
    {.name = "falling-edge", .builtin = PB_BUILTIN_FALLING_EDGE},
    {.name = "delta", .builtin = PB_BUILTIN_DELTA},
    {.name = "count!", .builtin = PB_BUILTIN_COUNT},
    {.name = "count", .builtin = PB_BUILTIN_COUNT},
    {.name = "moving-avg", .builtin = PB_BUILTIN_MOVING_AVG},
    {.name = "moving-sum", .builtin = PB_BUILTIN_MOVING_SUM},
    {.name = "moving-max", .builtin = PB_BUILTIN_MOVING_MAX},
    {.name = "moving-min", .builtin = PB_BUILTIN_MOVING_MIN},
    {.name = "median", .builtin = PB_BUILTIN_MEDIAN},
    {.name = "percentile", .builtin = PB_BUILTIN_PERCENTILE},
    {.name = "stddev", .builtin = PB_BUILTIN_STDDEV},
    {.name = "variance", .builtin = PB_BUILTIN_VARIANCE},
    {.name = "rate", .builtin = PB_BUILTIN_RATE},
    {.name = "throttle", .builtin = PB_BUILTIN_THROTTLE},
    {.name = "debounce!", .builtin = PB_BUILTIN_DEBOUNCE},
    {.name = "sample!", .builtin = PB_BUILTIN_SAMPLE},
    {.name = "aggregate!", .builtin = PB_BUILTIN_AGGREGATE},
    {.name = "bar!", .builtin = PB_BUILTIN_BAR},
    {.name = "bar", .builtin = PB_BUILTIN_BAR},
};

static const pb_builtin_entry *find_builtin(pb_slice name) {
    for (size_t i = 0; i < sizeof BUILTINS / sizeof BUILTINS[0]; i += 1) {
        if (text_eq(name, BUILTINS[i].name)) {
            return &BUILTINS[i];
        }
    }
    return NULL;
}

static void state_entry_free(pb_eval_state_entry *e) {
    free(e->op);
    free(e->subject);
    free(e->bytes);
    free(e->ring_values);
    free(e->ring_times_ms);
    free(e->emit_subject);
    *e = (pb_eval_state_entry){0};
}

void pb_eval_state_free(pb_eval_state *state) {
    for (size_t i = 0; i < state->len; i += 1) {
        state_entry_free(&state->items[i]);
    }
    free(state->items);
    *state = (pb_eval_state){0};
}

static bool state_key_eq(const pb_eval_state_entry *e, size_t rule_id, pb_slice op, pb_slice subject) {
    return e->rule_id == rule_id &&
           e->op_len == op.len &&
           e->subject_len == subject.len &&
           memcmp(e->op, op.ptr, op.len) == 0 &&
           memcmp(e->subject, subject.ptr, subject.len) == 0;
}

static bool heap_dup_slice(pb_slice s, char **out, size_t *out_len) {
    char *ptr = malloc(s.len == 0 ? 1 : s.len);
    if (ptr == NULL) {
        return false;
    }
    memcpy(ptr, s.ptr, s.len);
    *out = ptr;
    *out_len = s.len;
    return true;
}

pb_eval_state_entry *state_slot(pb_eval_ctx *ctx, const char *op_lit) {
    if (ctx->state == NULL) {
        return NULL;
    }

    const pb_slice op = {.ptr = op_lit, .len = strlen(op_lit)};
    for (size_t i = 0; i < ctx->state->len; i += 1) {
        if (state_key_eq(&ctx->state->items[i], ctx->rule_id, op, ctx->subject)) {
            return &ctx->state->items[i];
        }
    }

    if (ctx->state->len == ctx->state->cap) {
        const size_t next = ctx->state->cap == 0 ? 8 : ctx->state->cap * 2;
        pb_eval_state_entry *items = realloc(ctx->state->items, next * sizeof items[0]);
        if (items == NULL) {
            return NULL;
        }
        ctx->state->items = items;
        ctx->state->cap = next;
    }

    pb_eval_state_entry *e = &ctx->state->items[ctx->state->len];
    *e = (pb_eval_state_entry){.rule_id = ctx->rule_id};
    if (!heap_dup_slice(op, &e->op, &e->op_len) ||
        !heap_dup_slice(ctx->subject, &e->subject, &e->subject_len)) {
        state_entry_free(e);
        return NULL;
    }
    ctx->state->len += 1;
    return e;
}

bool state_set_bytes(pb_eval_state_entry *e, pb_slice bytes) {
    if (e->bytes_cap < bytes.len) {
        char *next = realloc(e->bytes, bytes.len == 0 ? 1 : bytes.len);
        if (next == NULL) {
            return false;
        }
        e->bytes = next;
        e->bytes_cap = bytes.len;
    }
    memcpy(e->bytes, bytes.ptr, bytes.len);
    e->bytes_len = bytes.len;
    e->kind = PB_EVAL_STATE_BYTES;
    return true;
}

bool state_set_emit_subject(pb_eval_state_entry *e, pb_slice bytes) {
    if (e->emit_subject_cap < bytes.len) {
        char *next = realloc(e->emit_subject, bytes.len == 0 ? 1 : bytes.len);
        if (next == NULL) {
            return false;
        }
        e->emit_subject = next;
        e->emit_subject_cap = bytes.len;
    }
    memcpy(e->emit_subject, bytes.ptr, bytes.len);
    e->emit_subject_len = bytes.len;
    return true;
}

pb_eval_result pb_eval(pb_eval_ctx *ctx, pb_value expr) {
    switch (expr.kind) {
    case PB_NIL:
    case PB_BOOL:
    case PB_NUMBER:
    case PB_STRING:
    case PB_KEYWORD:
        return ok(expr);
    case PB_SYMBOL:
        if (text_eq(expr.text, "subject")) {
            return ok((pb_value){.kind = PB_STRING, .text = ctx->subject});
        }
        if (text_eq(expr.text, "payload")) {
            return ok((pb_value){.kind = PB_STRING, .text = ctx->payload});
        }
        if (text_eq(expr.text, "payload-float") || text_eq(expr.text, "payload-int")) {
            pb_value payload = {.kind = PB_STRING, .text = ctx->payload};
            double n = 0;
            if (!as_number(payload, &n)) {
                return fail(PB_EVAL_TYPE);
            }
            if (text_eq(expr.text, "payload-int") && floor(n) != n) {
                return fail(PB_EVAL_TYPE);
            }
            return ok((pb_value){.kind = PB_NUMBER, .number = n});
        }
#ifdef TINYBLOK
        {
            pb_eval_result user = {0};
            if (pb_user_eval_symbol(ctx, expr.text, &user)) {
                return user;
            }
        }
#endif
        return fail(PB_EVAL_UNKNOWN_SYMBOL);
    case PB_VECTOR: {
        pb_value *items = pb_arena_alloc(ctx->arena, expr.seq.len * sizeof items[0], _Alignof(pb_value));
        if (items == NULL && expr.seq.len != 0) {
            return fail(PB_EVAL_OOM);
        }
        for (size_t i = 0; i < expr.seq.len; i += 1) {
            pb_eval_result r = pb_eval(ctx, expr.seq.items[i]);
            if (r.err != PB_EVAL_OK) {
                return r;
            }
            items[i] = r.value;
        }
        return ok((pb_value){.kind = PB_VECTOR, .seq = {.items = items, .len = expr.seq.len}});
    }
    case PB_LIST:
        return eval_list(ctx, expr.seq);
    }
    return fail(PB_EVAL_TYPE);
}

static pb_eval_result eval_args(pb_eval_ctx *ctx, pb_values raw, pb_values *out) {
    pb_value *items = pb_arena_alloc(ctx->arena, raw.len * sizeof items[0], _Alignof(pb_value));
    if (items == NULL && raw.len != 0) {
        return fail(PB_EVAL_OOM);
    }
    for (size_t i = 0; i < raw.len; i += 1) {
        pb_eval_result r = pb_eval(ctx, raw.items[i]);
        if (r.err != PB_EVAL_OK) {
            return r;
        }
        items[i] = r.value;
    }
    *out = (pb_values){.items = items, .len = raw.len};
    return ok((pb_value){.kind = PB_NIL});
}

static pb_eval_result eval_do(pb_eval_ctx *ctx, pb_values args) {
    pb_value last = {.kind = PB_NIL};
    for (size_t i = 0; i < args.len; i += 1) {
        pb_eval_result r = pb_eval(ctx, args.items[i]);
        if (r.err != PB_EVAL_OK) {
            return r;
        }
        last = r.value;
    }
    return ok(last);
}

static pb_eval_result eval_if(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 2 && args.len != 3) {
        return fail(PB_EVAL_ARITY);
    }
    pb_eval_result cond = pb_eval(ctx, args.items[0]);
    if (cond.err != PB_EVAL_OK) {
        return cond;
    }
    if (truthy(cond.value)) {
        return pb_eval(ctx, args.items[1]);
    }
    if (args.len == 3) {
        return pb_eval(ctx, args.items[2]);
    }
    return ok((pb_value){.kind = PB_NIL});
}

static pb_eval_result eval_when(pb_eval_ctx *ctx, pb_values args) {
    if (args.len < 1) {
        return fail(PB_EVAL_ARITY);
    }
    pb_eval_result cond = pb_eval(ctx, args.items[0]);
    if (cond.err != PB_EVAL_OK) {
        return cond;
    }
    if (!truthy(cond.value)) {
        return ok((pb_value){.kind = PB_NIL});
    }
    return eval_do(ctx, (pb_values){.items = args.items + 1, .len = args.len - 1});
}

static pb_eval_result eval_and(pb_eval_ctx *ctx, pb_values args) {
    pb_value last = {.kind = PB_BOOL, .boolean = true};
    for (size_t i = 0; i < args.len; i += 1) {
        pb_eval_result r = pb_eval(ctx, args.items[i]);
        if (r.err != PB_EVAL_OK) {
            return r;
        }
        if (!truthy(r.value)) {
            return ok(r.value);
        }
        last = r.value;
    }
    return ok(last);
}

static pb_eval_result eval_or(pb_eval_ctx *ctx, pb_values args) {
    for (size_t i = 0; i < args.len; i += 1) {
        pb_eval_result r = pb_eval(ctx, args.items[i]);
        if (r.err != PB_EVAL_OK) {
            return r;
        }
        if (truthy(r.value)) {
            return ok(r.value);
        }
    }
    return ok((pb_value){.kind = PB_NIL});
}

static pb_eval_result eval_call_with_threaded(pb_eval_ctx *ctx, pb_value form, pb_value threaded) {
    if (form.kind == PB_SYMBOL) {
        pb_value items[2] = {form, threaded};
        return eval_list(ctx, (pb_values){.items = items, .len = 2});
    }
    if (form.kind != PB_LIST || form.seq.len == 0 || form.seq.items[0].kind != PB_SYMBOL) {
        return fail(PB_EVAL_TYPE);
    }

    pb_value *items = pb_arena_alloc(ctx->arena, (form.seq.len + 1) * sizeof items[0], _Alignof(pb_value));
    if (items == NULL) {
        return fail(PB_EVAL_OOM);
    }
    memcpy(items, form.seq.items, form.seq.len * sizeof items[0]);
    items[form.seq.len] = threaded;
    return eval_list(ctx, (pb_values){.items = items, .len = form.seq.len + 1});
}

static pb_eval_result eval_thread(pb_eval_ctx *ctx, pb_values args) {
    if (args.len == 0) {
        return fail(PB_EVAL_ARITY);
    }
    pb_eval_result cur = pb_eval(ctx, args.items[0]);
    if (cur.err != PB_EVAL_OK) {
        return cur;
    }
    for (size_t i = 1; i < args.len; i += 1) {
        if (cur.value.kind == PB_NIL) {
            return cur;
        }
        cur = eval_call_with_threaded(ctx, args.items[i], cur.value);
        if (cur.err != PB_EVAL_OK) {
            return cur;
        }
    }
    return cur;
}

static pb_eval_result eval_transition(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 3) {
        return fail(PB_EVAL_ARITY);
    }

    pb_eval_result cond = pb_eval(ctx, args.items[0]);
    if (cond.err != PB_EVAL_OK) {
        return cond;
    }

    const bool cur = truthy(cond.value);
    pb_eval_state_entry *slot = state_slot(ctx, "transition");
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
    if (!prev && cur) {
        return pb_eval(ctx, args.items[1]);
    }
    if (prev && !cur) {
        return pb_eval(ctx, args.items[2]);
    }
    return ok((pb_value){.kind = PB_NIL});
}

static pb_eval_result eval_list(pb_eval_ctx *ctx, pb_values call) {
    const pb_slice head = call.items[0].text;
    const pb_values raw_args = {.items = call.items + 1, .len = call.len - 1};
    const pb_builtin_entry *entry = find_builtin(head);
    if (entry == NULL) {
#ifdef TINYBLOK
        pb_values args = {0};
        pb_eval_result er = eval_args(ctx, raw_args, &args);
        if (er.err != PB_EVAL_OK) {
            return er;
        }
        pb_eval_result user = {0};
        if (pb_user_eval_call(ctx, head, args, &user)) {
            return user;
        }
#endif
        return fail(PB_EVAL_UNKNOWN_SYMBOL);
    }

    if (entry->special) {
        switch (entry->builtin) {
        case PB_BUILTIN_DO: return eval_do(ctx, raw_args);
        case PB_BUILTIN_IF: return eval_if(ctx, raw_args);
        case PB_BUILTIN_WHEN: return eval_when(ctx, raw_args);
        case PB_BUILTIN_AND: return eval_and(ctx, raw_args);
        case PB_BUILTIN_OR: return eval_or(ctx, raw_args);
        case PB_BUILTIN_THREAD: return eval_thread(ctx, raw_args);
        case PB_BUILTIN_TRANSITION: return eval_transition(ctx, raw_args);
        case PB_BUILTIN_DROPOUT: return pb_eval_call_dropout(ctx, raw_args);
        default: return fail(PB_EVAL_UNKNOWN_SYMBOL);
        }
    }

    pb_values args = {0};
    pb_eval_result er = eval_args(ctx, raw_args, &args);
    if (er.err != PB_EVAL_OK) {
        return er;
    }
    return pb_eval_call_builtin(ctx, entry->builtin, args);
}

const char *pb_eval_error_name(pb_eval_error err) {
    switch (err) {
    case PB_EVAL_OK: return "ok";
    case PB_EVAL_OOM: return "out of memory";
    case PB_EVAL_UNKNOWN_SYMBOL: return "unknown symbol";
    case PB_EVAL_TYPE: return "type mismatch";
    case PB_EVAL_ARITY: return "arity mismatch";
    case PB_EVAL_INVALID_SUBJECT: return "invalid subject";
    case PB_EVAL_PUBLISH_FAILED: return "publish failed";
    }
    return "unknown eval error";
}
