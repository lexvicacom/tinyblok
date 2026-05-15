#ifndef PB_JSON_H
#define PB_JSON_H

#include "pb_sexpr.h"

#ifndef PB_ENABLE_JSON
#define PB_ENABLE_JSON 0
#endif

#if PB_ENABLE_JSON
pb_parse_result pb_parse_patchbay_source(pb_arena *arena, const char *path, const char *src, size_t len);
#else
static inline pb_parse_result pb_parse_patchbay_source(pb_arena *arena, const char *path, const char *src, size_t len) {
    (void)path;
    return pb_parse_all(arena, src, len);
}
#endif

#endif
