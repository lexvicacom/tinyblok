#include <stdint.h>
#include <stddef.h>

#include "tinyblok_patchbay.h"

static float board_temp_c(void)
{
    return 21.5f;
}

static uint32_t board_counter(void)
{
    static uint32_t n;
    return n++;
}

static size_t board_echo(const uint8_t *payload, size_t payload_len, uint8_t *out, size_t out_len)
{
    size_t n = 0;
    static const char prefix[] = "board: ";
    for (size_t i = 0; i < sizeof(prefix) - 1 && n < out_len; i += 1)
        out[n++] = (uint8_t)prefix[i];
    for (size_t i = 0; i < payload_len && n < out_len; i += 1)
        out[n++] = payload[i];
    return n;
}

size_t tinyblok_app_native_symbols(const tinyblok_native_symbol **out)
{
    static const tinyblok_native_symbol symbols[] = {
        {.name = "board_temp_c", .type = TINYBLOK_TYPE_F32, .fn.read_f32 = board_temp_c},
        {.name = "board_counter", .type = TINYBLOK_TYPE_U32, .fn.read_u32 = board_counter},
        {.name = "board_echo", .type = TINYBLOK_TYPE_BYTES, .input = TINYBLOK_TYPE_BYTES, .has_input = true, .fn.call_bytes = board_echo},
    };
    *out = symbols;
    return sizeof(symbols) / sizeof(symbols[0]);
}

