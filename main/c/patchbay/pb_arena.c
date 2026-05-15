#include "pb_arena.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool is_power_of_two(size_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

static bool align_ptr_offset(const unsigned char *base, size_t used, size_t align, size_t *out) {
    if (!is_power_of_two(align)) {
        return false;
    }

    const uintptr_t base_addr = (uintptr_t)base;
    if (used > UINTPTR_MAX - base_addr) {
        return false;
    }

    const uintptr_t addr = base_addr + used;
    const uintptr_t mask = (uintptr_t)align - 1;
    if (addr > UINTPTR_MAX - mask) {
        return false;
    }

    const uintptr_t aligned = (addr + mask) & ~mask;
    if (aligned < base_addr) {
        return false;
    }

    const uintptr_t delta = aligned - base_addr;
    if (delta > SIZE_MAX) {
        return false;
    }

    *out = (size_t)delta;
    return true;
}

void *pb_arena_alloc(pb_arena *arena, size_t size, size_t align) {
    if (arena == NULL) {
        return NULL;
    }
    if (align == 0) {
        align = sizeof(void *);
    }
    if (!is_power_of_two(align)) {
        return NULL;
    }

    for (pb_arena_block *b = arena->head; b != NULL; b = b->next) {
        size_t off = 0;
        if (!align_ptr_offset(b->data, b->used, align, &off)) {
            return NULL;
        }
        if (off <= b->cap && size <= b->cap - off) {
            b->used = off + size;
            return b->data + off;
        }
    }

    if (size > SIZE_MAX - align) {
        return NULL;
    }
    const size_t min_cap = size + align;

    size_t cap = 4096;
    while (cap < min_cap) {
        if (cap > SIZE_MAX / 2) {
            cap = min_cap;
            break;
        }
        cap *= 2;
    }
    if (cap > SIZE_MAX - sizeof(pb_arena_block)) {
        return NULL;
    }

    pb_arena_block *b = malloc(sizeof *b + cap);
    if (b == NULL) {
        return NULL;
    }
    b->next = arena->head;
    b->used = 0;
    b->cap = cap;
    arena->head = b;

    size_t off = 0;
    if (!align_ptr_offset(b->data, b->used, align, &off) || off > b->cap || size > b->cap - off) {
        arena->head = b->next;
        free(b);
        return NULL;
    }

    b->used = off + size;
    return b->data + off;
}

void *pb_arena_memdup(pb_arena *arena, const void *src, size_t len) {
    if (len != 0 && src == NULL) {
        return NULL;
    }

    void *dst = pb_arena_alloc(arena, len == 0 ? 1 : len, 1);
    if (dst == NULL) {
        return NULL;
    }
    if (len != 0) {
        memcpy(dst, src, len);
    }
    return dst;
}

void pb_arena_reset(pb_arena *arena) {
    if (arena == NULL) {
        return;
    }
    for (pb_arena_block *b = arena->head; b != NULL; b = b->next) {
        b->used = 0;
    }
}

void pb_arena_free(pb_arena *arena) {
    if (arena == NULL) {
        return;
    }

    pb_arena_block *b = arena->head;
    while (b != NULL) {
        pb_arena_block *next = b->next;
        free(b);
        b = next;
    }
    arena->head = NULL;
}
