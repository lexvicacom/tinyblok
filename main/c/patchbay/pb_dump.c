#include "pb_json.h"

#include <stdio.h>
#include <stdlib.h>

static void dump_value(const pb_value *v, int depth);

static void indent(int depth) {
    for (int i = 0; i < depth; i += 1) {
        fputs("  ", stdout);
    }
}

static void dump_text(const char *tag, pb_slice s) {
    printf("%s %.*s\n", tag, (int)s.len, s.ptr);
}

static void dump_seq(const char *tag, pb_values seq, int depth) {
    printf("%s len=%zu\n", tag, seq.len);
    for (size_t i = 0; i < seq.len; i += 1) {
        dump_value(&seq.items[i], depth + 1);
    }
}

static void dump_value(const pb_value *v, int depth) {
    indent(depth);
    switch (v->kind) {
    case PB_NIL: puts("nil"); break;
    case PB_BOOL: printf("bool %s\n", v->boolean ? "true" : "false"); break;
    case PB_NUMBER: printf("number %.17g\n", v->number); break;
    case PB_SYMBOL: dump_text("symbol", v->text); break;
    case PB_KEYWORD: dump_text("keyword", v->text); break;
    case PB_STRING: dump_text("string", v->text); break;
    case PB_LIST: dump_seq("list", v->seq, depth); break;
    case PB_VECTOR: dump_seq("vector", v->seq, depth); break;
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s patchbay.{edn,json}\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(f);
        return 1;
    }
    const long size = ftell(f);
    if (size < 0) {
        perror("ftell");
        fclose(f);
        return 1;
    }
    rewind(f);

    char *src = malloc((size_t)size);
    if (src == NULL) {
        fclose(f);
        return 1;
    }
    const size_t nread = fread(src, 1, (size_t)size, f);
    fclose(f);
    if (nread != (size_t)size) {
        free(src);
        return 1;
    }

    pb_arena arena = {0};
    const pb_parse_result r = pb_parse_patchbay_source(&arena, argv[1], src, (size_t)size);
    if (r.err != PB_PARSE_OK) {
        fprintf(stderr, "%s at byte %zu\n", pb_parse_error_name(r.err), r.err_offset);
        pb_arena_free(&arena);
        free(src);
        return 1;
    }

    printf("forms len=%zu\n", r.forms.len);
    for (size_t i = 0; i < r.forms.len; i += 1) {
        dump_value(&r.forms.items[i], 0);
    }

    pb_arena_free(&arena);
    free(src);
    return 0;
}
