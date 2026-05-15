#ifndef PB_SEXPR_H
#define PB_SEXPR_H

#include "pb_arena.h"

#include <stdbool.h>
#include <stddef.h>

// Borrowed text slice; ownership is determined by the containing value.
typedef struct pb_slice {
    const char *ptr;
    size_t len;
} pb_slice;

typedef enum pb_value_kind {
    PB_NIL,
    PB_BOOL,
    PB_NUMBER,
    PB_SYMBOL,
    PB_KEYWORD,
    PB_STRING,
    PB_LIST,
    PB_VECTOR,
} pb_value_kind;

typedef struct pb_value pb_value;

// Arena-owned sequence backing list and vector values.
typedef struct pb_values {
    pb_value *items;
    size_t len;
} pb_values;

// Patchbay AST node. Lists are call forms; vectors are data.
struct pb_value {
    pb_value_kind kind;
    union {
        bool boolean;
        double number;
        pb_slice text;
        pb_values seq;
    };
};

typedef enum pb_parse_error {
    PB_PARSE_OK,
    PB_PARSE_OOM,
    PB_PARSE_UNEXPECTED_EOF,
    PB_PARSE_UNEXPECTED_RPAREN,
    PB_PARSE_UNEXPECTED_RBRACKET,
    PB_PARSE_MISMATCHED_BRACKET,
    PB_PARSE_UNTERMINATED_STRING,
    PB_PARSE_INVALID_ESCAPE,
    PB_PARSE_INVALID_NUMBER,
    PB_PARSE_INVALID_LIST_HEAD,
    PB_PARSE_INVALID_JSON,
} pb_parse_error;

// Parse result with top-level forms owned by the supplied arena.
typedef struct pb_parse_result {
    pb_parse_error err;
    size_t err_offset;
    pb_values forms;
} pb_parse_result;

pb_parse_result pb_parse_all(pb_arena *arena, const char *src, size_t len);
const char *pb_parse_error_name(pb_parse_error err);

#endif
