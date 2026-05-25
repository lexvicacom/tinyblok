#ifndef TINYBLOK_PATCHBAY_H
#define TINYBLOK_PATCHBAY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint32_t (*tinyblok_read_u32_fn)(void);
typedef int (*tinyblok_read_i32_fn)(void);
typedef float (*tinyblok_read_f32_fn)(void);
typedef uint64_t (*tinyblok_read_u64_fn)(void);
typedef float (*tinyblok_call_f32_f32_fn)(float);
typedef size_t (*tinyblok_call_bytes_fn)(const uint8_t *payload, size_t payload_len, uint8_t *out, size_t out_len);

typedef enum tinyblok_value_type
{
    TINYBLOK_TYPE_U32,
    TINYBLOK_TYPE_I32,
    TINYBLOK_TYPE_F32,
    TINYBLOK_TYPE_U64,
    TINYBLOK_TYPE_UPTIME_S,
    TINYBLOK_TYPE_BYTES,
} tinyblok_value_type;

// Compiled C symbol that a parsed `(pump ...)` or `(fn ...)` may reference.
typedef struct tinyblok_native_symbol
{
    const char *name;
    tinyblok_value_type type;
    tinyblok_value_type input;
    bool has_input;
    union
    {
        tinyblok_read_u32_fn read_u32;
        tinyblok_read_i32_fn read_i32;
        tinyblok_read_f32_fn read_f32;
        tinyblok_read_u64_fn read_u64;
        tinyblok_call_f32_f32_fn call_f32_f32;
        tinyblok_call_bytes_fn call_bytes;
    } fn;
} tinyblok_native_symbol;

// Optional app/component hook. Return a static array of extra pump/function
// symbols; Tinyblok keeps only pointers to these entries.
size_t tinyblok_app_native_symbols(const tinyblok_native_symbol **out);

// Timer source loaded from a top-level `(pump ...)` form. The subject slice is
// owned by the patchbay parse arena and remains valid after init.
typedef struct tinyblok_pump
{
    const char *subject;
    uint64_t period_us;
} tinyblok_pump;

int tinyblok_patchbay_init(void);
void tinyblok_patchbay_main(void);

size_t tinyblok_patchbay_pump_count(void);
const tinyblok_pump *tinyblok_patchbay_pump(size_t index);
void tinyblok_patchbay_fire_pump(size_t index);

size_t tinyblok_patchbay_request_count(void);
const char *tinyblok_patchbay_request_subject(size_t index);
void tinyblok_patchbay_handle_msg(const unsigned char *subject, size_t subject_len,
                                  const unsigned char *reply, size_t reply_len,
                                  const unsigned char *payload, size_t payload_len);

void tinyblok_patchbay_clock_fired(void);
void tinyblok_patchbay_arm_next_clock(void);

#endif
