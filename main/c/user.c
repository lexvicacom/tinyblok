// Example C sources for patchbay functions.
//
// Apps that consume Tinyblok as a component should put their own pump reads or
// request helpers in a separate component and expose them with
// tinyblok_app_native_symbols() from tinyblok_patchbay.h. This file remains a
// small in-tree example for the default patchbay.
//
// - (pump SUBJECT :from symbol ...)
// - (fn NAME :from symbol ...)
// - (on-req SUBJECT ...)
//
// Keep hot-path code bounded: no per-tick allocation, blocking I/O, or
// unbounded parsing.

#include <stddef.h>
#include <stdint.h>

uint32_t tinyblok_user_counter(void)
{
    static uint32_t n;
    return n++;
}

/*
patchbay.edn:

(pump "tinyblok.user.counter" :from tinyblok_user_counter :type u32 :hz 1)

For component users, expose the compiled symbol from tinyblok_app_native_symbols,
add an (fn ...) declaration, then call it from (on-req ...).
*/
