// User-owned C sources for patchbay functions.
//
// Add small ESP-IDF-facing pump reads or request helpers here, then register
// them from patchbay.edn with:
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

For request handlers, add the compiled symbol to tinyblok_patchbay.c's native
symbol table, add an (fn ...) declaration here, then call it from (on-req ...).
*/
