#ifndef TINYBLOK_PATCHBAY_ROUTER_H
#define TINYBLOK_PATCHBAY_ROUTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct mb_slice
{
    const uint8_t *ptr;
    size_t len;
} mb_slice;

typedef struct mb_router
{
    bool (*publish)(void *ctx, mb_slice subject, mb_slice payload);
    void *ctx;
} mb_router;

static inline bool mb_router_publish(mb_router *router, mb_slice subject, mb_slice payload)
{
    return router != NULL && router->publish != NULL && router->publish(router->ctx, subject, payload);
}

#endif
