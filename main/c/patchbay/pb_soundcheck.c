#define _POSIX_C_SOURCE 200809L

#include "pb_soundcheck.h"

#include "pb_eval.h"
#include "pb_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct soundcheck_out {
    bool label;
} soundcheck_out;

static bool slice_eq(pb_slice s, const char *lit) {
    const size_t n = strlen(lit);
    return s.len == n && memcmp(s.ptr, lit, n) == 0;
}

static bool read_file(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(f);
        return false;
    }
    const long size = ftell(f);
    if (size < 0) {
        perror("ftell");
        fclose(f);
        return false;
    }
    rewind(f);

    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return false;
    }
    const size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (nread != (size_t)size) {
        free(buf);
        return false;
    }
    buf[nread] = '\0';
    *out = buf;
    *out_len = nread;
    return true;
}

static bool token_match(pb_slice filter, pb_slice subject) {
    size_t fp = 0;
    size_t sp = 0;

    for (;;) {
        const size_t fs = fp;
        while (fp < filter.len && filter.ptr[fp] != '.') fp += 1;
        const pb_slice ftok = {.ptr = filter.ptr + fs, .len = fp - fs};

        if (ftok.len == 1 && ftok.ptr[0] == '>') {
            return true;
        }

        const size_t ss = sp;
        while (sp < subject.len && subject.ptr[sp] != '.') sp += 1;
        if (ss == subject.len && ftok.len != 0) {
            return false;
        }
        const pb_slice stok = {.ptr = subject.ptr + ss, .len = sp - ss};

        if (!(ftok.len == 1 && ftok.ptr[0] == '*') &&
            !(ftok.len == stok.len && memcmp(ftok.ptr, stok.ptr, ftok.len) == 0)) {
            return false;
        }

        const bool fend = fp == filter.len;
        const bool send = sp == subject.len;
        if (fend || send) {
            return fend && send;
        }
        fp += 1;
        sp += 1;
    }
}

static bool publish_cb(void *ctx, pb_slice subject, pb_slice payload) {
    soundcheck_out *out = ctx;
    if (out->label) {
        printf("out|%.*s|%.*s\n", (int)subject.len, subject.ptr, (int)payload.len, payload.ptr);
    } else {
        printf("%.*s|%.*s\n", (int)subject.len, subject.ptr, (int)payload.len, payload.ptr);
    }
    return true;
}

static bool eval_on_form(pb_value form, size_t rule_id, pb_eval_state *state,
                         pb_slice subject, pb_slice payload, soundcheck_out *out) {
    if (form.kind != PB_LIST || form.seq.len < 3) {
        return true;
    }
    pb_values items = form.seq;
    if (items.items[0].kind != PB_SYMBOL || !slice_eq(items.items[0].text, "on")) {
        return true;
    }
    if (items.items[1].kind != PB_STRING) {
        fprintf(stderr, "soundcheck: invalid on form\n");
        return false;
    }
    if (!token_match(items.items[1].text, subject)) {
        return true;
    }

    pb_arena scratch = {0};
    pb_eval_ctx ctx = {
        .arena = &scratch,
        .state = state,
        .rule_id = rule_id,
        .subject = subject,
        .payload = payload,
        .publish = publish_cb,
        .publish_ctx = out,
    };
    const pb_eval_result r = pb_eval(&ctx, items.items[items.len - 1]);
    pb_arena_free(&scratch);
    if (r.err != PB_EVAL_OK) {
        fprintf(stderr, "soundcheck: eval error: %s\n", pb_eval_error_name(r.err));
        return false;
    }
    return true;
}

int pb_soundcheck_run(const char *path, pb_soundcheck_options opts) {
    char *source = NULL;
    size_t source_len = 0;
    if (!read_file(path, &source, &source_len)) {
        return 1;
    }

    pb_arena parse_arena = {0};
    const pb_parse_result parsed = pb_parse_patchbay_source(&parse_arena, path, source, source_len);
    if (parsed.err != PB_PARSE_OK) {
        fprintf(stderr, "soundcheck: parse error: %s at byte %zu\n",
                pb_parse_error_name(parsed.err), parsed.err_offset);
        pb_arena_free(&parse_arena);
        free(source);
        return 1;
    }

    soundcheck_out out = {.label = opts.label};
    pb_eval_state state = {0};
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = 0;
    int rc = 0;
    while ((n = getline(&line, &cap, stdin)) >= 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            n -= 1;
        }
        if (n == 0) {
            continue;
        }
        char *sep = memchr(line, '|', (size_t)n);
        if (sep == NULL) {
            fprintf(stderr, "soundcheck: expected SUBJECT|payload, got: %.*s\n", (int)n, line);
            rc = 1;
            continue;
        }

        pb_slice subject = {.ptr = line, .len = (size_t)(sep - line)};
        pb_slice payload = {.ptr = sep + 1, .len = (size_t)(line + n - (sep + 1))};
        if (opts.label) {
            printf("in|%.*s|%.*s\n", (int)subject.len, subject.ptr, (int)payload.len, payload.ptr);
        } else {
            printf("%.*s|%.*s\n", (int)subject.len, subject.ptr, (int)payload.len, payload.ptr);
        }

        for (size_t i = 0; i < parsed.forms.len; i += 1) {
            if (!eval_on_form(parsed.forms.items[i], i, &state, subject, payload, &out)) {
                rc = 1;
            }
        }
    }

    free(line);
    pb_eval_state_free(&state);
    pb_arena_free(&parse_arena);
    free(source);
    return rc;
}
