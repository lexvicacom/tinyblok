#ifndef PB_ARENA_H
#define PB_ARENA_H

#include <stddef.h>

// Arena-owned allocation block; allocations bump `used` and are freed as a group.
typedef struct pb_arena_block {
    struct pb_arena_block *next;
    size_t used;
    size_t cap;
    unsigned char data[];
} pb_arena_block;

// Small bump allocator for patchbay parse trees and temporary eval objects.
typedef struct pb_arena {
    pb_arena_block *head;
} pb_arena;

// Returned pointers remain valid until reset/free of the same arena.
void *pb_arena_alloc(pb_arena *arena, size_t size, size_t align);
void *pb_arena_memdup(pb_arena *arena, const void *src, size_t len);
void pb_arena_reset(pb_arena *arena);
void pb_arena_free(pb_arena *arena);

#endif
