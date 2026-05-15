#ifndef PB_EVAL_INTERNAL_H
#define PB_EVAL_INTERNAL_H

#include "pb_eval.h"

typedef enum pb_builtin {
    PB_BUILTIN_DO,
    PB_BUILTIN_IF,
    PB_BUILTIN_WHEN,
    PB_BUILTIN_AND,
    PB_BUILTIN_OR,
    PB_BUILTIN_THREAD,
    PB_BUILTIN_TRANSITION,
    PB_BUILTIN_DROPOUT,
    PB_BUILTIN_NOW,
    PB_BUILTIN_NOT,
    PB_BUILTIN_EQ,
    PB_BUILTIN_GT,
    PB_BUILTIN_LT,
    PB_BUILTIN_GE,
    PB_BUILTIN_LE,
    PB_BUILTIN_ADD,
    PB_BUILTIN_SUB,
    PB_BUILTIN_MUL,
    PB_BUILTIN_DIV,
    PB_BUILTIN_STR_CONCAT,
    PB_BUILTIN_CONTAINS,
    PB_BUILTIN_STARTS_WITH,
    PB_BUILTIN_ENDS_WITH,
    PB_BUILTIN_SUBJECT_APPEND,
    PB_BUILTIN_SUBJECT_TOKEN,
    PB_BUILTIN_SUBJECT_WITH,
    PB_BUILTIN_PUBLISH,
    PB_BUILTIN_JSON_GET,
    PB_BUILTIN_JSON_DEMUX,
    PB_BUILTIN_ROUND,
    PB_BUILTIN_QUANTIZE,
    PB_BUILTIN_CLAMP,
    PB_BUILTIN_MIN,
    PB_BUILTIN_MAX,
    PB_BUILTIN_ABS,
    PB_BUILTIN_SIGN,
    PB_BUILTIN_SQUELCH,
    PB_BUILTIN_DEADBAND,
    PB_BUILTIN_CHANGED,
    PB_BUILTIN_HOLD_OFF,
    PB_BUILTIN_RISING_EDGE,
    PB_BUILTIN_FALLING_EDGE,
    PB_BUILTIN_DELTA,
    PB_BUILTIN_COUNT,
    PB_BUILTIN_MOVING_AVG,
    PB_BUILTIN_MOVING_SUM,
    PB_BUILTIN_MOVING_MAX,
    PB_BUILTIN_MOVING_MIN,
    PB_BUILTIN_MEDIAN,
    PB_BUILTIN_PERCENTILE,
    PB_BUILTIN_STDDEV,
    PB_BUILTIN_VARIANCE,
    PB_BUILTIN_RATE,
    PB_BUILTIN_THROTTLE,
    PB_BUILTIN_DEBOUNCE,
    PB_BUILTIN_SAMPLE,
    PB_BUILTIN_AGGREGATE,
    PB_BUILTIN_BAR,
} pb_builtin;

pb_eval_result pb_eval_ok(pb_value v);
pb_eval_result pb_eval_fail(pb_eval_error err);
bool pb_eval_text_eq(pb_slice s, const char *lit);
bool pb_eval_truthy(pb_value v);
pb_value pb_eval_bool_value(bool b);
bool pb_eval_as_number(pb_value v, double *out);
bool pb_eval_as_string(pb_value v, pb_slice *out);
bool pb_eval_coerce_payload(pb_eval_ctx *ctx, pb_value v, pb_slice *out);
bool pb_eval_value_eq(pb_value a, pb_value b);
pb_eval_state_entry *pb_eval_state_slot(pb_eval_ctx *ctx, const char *op_lit);
bool pb_eval_state_set_bytes(pb_eval_state_entry *e, pb_slice bytes);
bool pb_eval_state_set_emit_subject(pb_eval_state_entry *e, pb_slice bytes);
pb_eval_result pb_eval_call_builtin(pb_eval_ctx *ctx, pb_builtin builtin, pb_values args);
pb_eval_result pb_eval_call_window_builtin(pb_eval_ctx *ctx, pb_builtin builtin, pb_values args);
pb_eval_result pb_eval_call_dropout(pb_eval_ctx *ctx, pb_values raw_args);
pb_eval_result pb_eval_tick_clock_state_entry(pb_eval_ctx *ctx, pb_eval_state_entry *entry);

// Short aliases used as local sugar in evaluator builtin implementations.
#define ok pb_eval_ok
#define fail pb_eval_fail
#define text_eq pb_eval_text_eq
#define truthy pb_eval_truthy
#define bool_value pb_eval_bool_value
#define as_number pb_eval_as_number
#define as_string pb_eval_as_string
#define coerce_payload pb_eval_coerce_payload
#define value_eq pb_eval_value_eq
#define state_slot pb_eval_state_slot
#define state_set_bytes pb_eval_state_set_bytes
#define state_set_emit_subject pb_eval_state_set_emit_subject

#endif
