#ifndef PB_EVAL_H
#define PB_EVAL_H

#include "pb_sexpr.h"

#include <stdint.h>

typedef enum pb_eval_error {
    PB_EVAL_OK,
    PB_EVAL_OOM,
    PB_EVAL_UNKNOWN_SYMBOL,
    PB_EVAL_TYPE,
    PB_EVAL_ARITY,
    PB_EVAL_INVALID_SUBJECT,
    PB_EVAL_PUBLISH_FAILED,
} pb_eval_error;

typedef bool (*pb_publish_fn)(void *ctx, pb_slice subject, pb_slice payload);

typedef enum pb_eval_state_kind {
    PB_EVAL_STATE_EMPTY,
    PB_EVAL_STATE_NUMBER,
    PB_EVAL_STATE_BYTES,
    PB_EVAL_STATE_RING,
    PB_EVAL_STATE_BAR,
    PB_EVAL_STATE_CLOCK,
} pb_eval_state_kind;

typedef enum pb_eval_clock_kind {
    PB_EVAL_CLOCK_NONE,
    PB_EVAL_CLOCK_DROPOUT,
    PB_EVAL_CLOCK_DEBOUNCE,
    PB_EVAL_CLOCK_SAMPLE,
    PB_EVAL_CLOCK_AGGREGATE,
} pb_eval_clock_kind;

typedef enum pb_eval_metric_kind {
    PB_EVAL_METRIC_NONE,
    PB_EVAL_METRIC_AVG,
    PB_EVAL_METRIC_SUM,
    PB_EVAL_METRIC_MIN,
    PB_EVAL_METRIC_MAX,
    PB_EVAL_METRIC_COUNT,
    PB_EVAL_METRIC_RATE,
} pb_eval_metric_kind;

// Long-lived per-(rule, op, subject) slot for simple stateful forms.
typedef struct pb_eval_state_entry {
    size_t rule_id;
    char *op;
    size_t op_len;
    char *subject;
    size_t subject_len;
    pb_eval_state_kind kind;
    double number;
    char *bytes;
    size_t bytes_len;
    size_t bytes_cap;
    double *ring_values;
    uint64_t *ring_times_ms;
    size_t ring_cap;
    size_t ring_len;
    size_t ring_start;
    double ring_sum;
    uint64_t ring_window_ms;
    bool ring_time_window;
    double bar_open;
    double bar_high;
    double bar_low;
    double bar_last_close;
    uint32_t bar_count;
    uint32_t bar_cap;
    uint64_t bar_window_ms;
    uint64_t bar_window_start_ms;
    bool bar_time_window;
    pb_eval_clock_kind clock_kind;
    pb_eval_metric_kind metric_kind;
    uint64_t clock_interval_ms;
    uint64_t clock_deadline_ms;
    bool clock_armed;
    bool dropout_lost;
    pb_value lost_form;
    pb_value found_form;
    char *emit_subject;
    size_t emit_subject_len;
    size_t emit_subject_cap;
} pb_eval_state_entry;

// Stateful patchbay storage owned by the caller and reused across publishes.
typedef struct pb_eval_state {
    pb_eval_state_entry *items;
    size_t len;
    size_t cap;
} pb_eval_state;

// Per-evaluation bindings and effect hooks; scratch allocations use arena.
typedef struct pb_eval_ctx {
    pb_arena *arena;
    pb_eval_state *state;
    size_t rule_id;
    uint64_t now_ms;
    int64_t wall_ms;
    pb_slice subject;
    pb_slice payload;
    pb_publish_fn publish;
    void *publish_ctx;
} pb_eval_ctx;

// Evaluator return value; value is meaningful only when err is PB_EVAL_OK.
typedef struct pb_eval_result {
    pb_eval_error err;
    pb_value value;
} pb_eval_result;

pb_eval_result pb_eval(pb_eval_ctx *ctx, pb_value expr);
pb_eval_result pb_eval_tick_state_entry(pb_eval_ctx *ctx, pb_eval_state_entry *entry);
void pb_eval_state_free(pb_eval_state *state);
const char *pb_eval_error_name(pb_eval_error err);

#ifdef TINYBLOK
// Optional embedder hooks for Tinyblok-specific symbols and calls. The default
// weak definitions return false, so the core evaluator remains standalone.
bool pb_user_eval_symbol(pb_eval_ctx *ctx, pb_slice name, pb_eval_result *out);
bool pb_user_eval_call(pb_eval_ctx *ctx, pb_slice name, pb_values args, pb_eval_result *out);
#endif

#endif
