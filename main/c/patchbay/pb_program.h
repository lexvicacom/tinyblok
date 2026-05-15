#ifndef PB_PROGRAM_H
#define PB_PROGRAM_H

#include "pb_eval.h"
#include "router.h"

typedef struct pb_rule {
    pb_slice filter;
    pb_value body;
    bool reentrant;
    pb_eval_state state;
} pb_rule;

// Top-level `(lvc ...)` filters borrowed from the parse arena.
typedef struct pb_lvc_config {
    pb_slice *filters;
    size_t len;
    size_t cap;
} pb_lvc_config;

// Top-level `(bridge ...)` export-only remote NATS config.
typedef struct pb_bridge_config {
    pb_slice *servers;
    size_t servers_len;
    size_t servers_cap;
    pb_slice *exports;
    size_t exports_len;
    size_t exports_cap;
    pb_slice name;
    pb_slice creds;
    pb_slice user;
    pb_slice password;
    pb_slice token;
    pb_slice tls_ca;
    pb_slice tls_cert;
    pb_slice tls_key;
    int64_t connect_timeout_ms;
    int64_t ping_interval_ms;
    int64_t reconnect_wait_ms;
    int max_reconnect;
    bool present;
    bool tls;
    bool tls_skip_verify;
    bool has_name;
    bool has_creds;
    bool has_user;
    bool has_password;
    bool has_token;
    bool has_tls_ca;
    bool has_tls_cert;
    bool has_tls_key;
    bool has_connect_timeout_ms;
    bool has_ping_interval_ms;
    bool has_reconnect_wait_ms;
    bool has_max_reconnect;
} pb_bridge_config;

// The loaded and validated patchbay program: rules/config plus runtime state and eval scratch.
typedef struct pb_program {
    pb_arena parse_arena;
    pb_arena scratch;
    pb_rule *rules;
    size_t len;
    size_t cap;
    pb_lvc_config lvc;
    pb_bridge_config bridge;
    bool uses_wall_clock;
    bool uses_clock_timer;
    size_t eval_depth;
} pb_program;

bool pb_program_load_file(pb_program *program, const char *path);
bool pb_program_load_source(pb_program *program, const char *label, const char *source, size_t source_len);
void pb_program_free(pb_program *program);
bool pb_program_eval_publish(pb_program *program, mb_router *router, mb_slice subject, mb_slice payload,
                             uint64_t now_ms, int64_t wall_ms);
bool pb_program_tick(pb_program *program, mb_router *router, uint64_t now_ms, int64_t wall_ms);
bool pb_program_next_clock_deadline(const pb_program *program, uint64_t *out_ms);

#endif
