#include "pb_sexpr.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

// Temporary growable list before freezing items into the arena.
typedef struct value_vec {
    pb_value *items;
    size_t len;
    size_t cap;
} value_vec;

// Cursor over strict patchbay source.
typedef struct parser {
    pb_arena *arena;
    const char *src;
    size_t len;
    size_t pos;
} parser;

static void skip_ws(parser *p) {
    while (p->pos < p->len) {
        const char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            p->pos += 1;
        } else if (c == ';') {
            while (p->pos < p->len && p->src[p->pos] != '\n') {
                p->pos += 1;
            }
        } else {
            break;
        }
    }
}

static bool vec_push(value_vec *v, pb_value item) {
    if (v->len == v->cap) {
        const size_t next = v->cap == 0 ? 8 : v->cap * 2;
        pb_value *items = realloc(v->items, next * sizeof v->items[0]);
        if (items == NULL) {
            return false;
        }
        v->items = items;
        v->cap = next;
    }
    v->items[v->len] = item;
    v->len += 1;
    return true;
}

static bool slice_eq(pb_slice s, const char *lit) {
    const size_t n = strlen(lit);
    return s.len == n && memcmp(s.ptr, lit, n) == 0;
}

static bool parse_number(pb_slice tok, double *out) {
    if (tok.len == 0) {
        return false;
    }
    const char first = tok.ptr[0];
    const bool numeric =
        (first >= '0' && first <= '9') ||
        ((first == '-' || first == '+') && tok.len > 1 && tok.ptr[1] >= '0' && tok.ptr[1] <= '9') ||
        first == '.';
    if (!numeric) {
        return false;
    }

    char *tmp = malloc(tok.len + 1);
    if (tmp == NULL) {
        return false;
    }
    memcpy(tmp, tok.ptr, tok.len);
    tmp[tok.len] = '\0';

    errno = 0;
    char *end = NULL;
    const double n = strtod(tmp, &end);
    const bool ok = errno == 0 && end == tmp + tok.len;
    free(tmp);
    if (!ok) {
        return false;
    }
    *out = n;
    return true;
}

static pb_parse_error parse_one(parser *p, pb_value *out);

static pb_parse_error parse_seq(parser *p, char close, char wrong_close, pb_value_kind kind, pb_value *out) {
    const size_t open = p->pos;
    p->pos += 1;
    value_vec items = {0};

    for (;;) {
        skip_ws(p);
        if (p->pos >= p->len) {
            free(items.items);
            return PB_PARSE_UNEXPECTED_EOF;
        }
        const char c = p->src[p->pos];
        if (c == close) {
            p->pos += 1;
            const size_t count = items.len;
            pb_value *frozen = pb_arena_alloc(p->arena, count * sizeof frozen[0], _Alignof(pb_value));
            if (frozen == NULL && count != 0) {
                free(items.items);
                return PB_PARSE_OOM;
            }
            if (count != 0) {
                memcpy(frozen, items.items, count * sizeof frozen[0]);
            }
            free(items.items);
            if (kind == PB_LIST && (count == 0 || frozen[0].kind != PB_SYMBOL)) {
                p->pos = open;
                return PB_PARSE_INVALID_LIST_HEAD;
            }
            *out = (pb_value){.kind = kind, .seq = {.items = frozen, .len = count}};
            return PB_PARSE_OK;
        }
        if (c == wrong_close) {
            free(items.items);
            return PB_PARSE_MISMATCHED_BRACKET;
        }

        pb_value item = {0};
        const pb_parse_error err = parse_one(p, &item);
        if (err != PB_PARSE_OK) {
            free(items.items);
            return err;
        }
        if (!vec_push(&items, item)) {
            free(items.items);
            return PB_PARSE_OOM;
        }
    }
}

static pb_parse_error parse_string(parser *p, pb_value *out) {
    const size_t open = p->pos;
    p->pos += 1;
    char *tmp = NULL;
    size_t len = 0;
    size_t cap = 0;

    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == '"') {
            p->pos += 1;
            char *owned = pb_arena_memdup(p->arena, tmp, len);
            free(tmp);
            if (owned == NULL) {
                return PB_PARSE_OOM;
            }
            *out = (pb_value){.kind = PB_STRING, .text = {.ptr = owned, .len = len}};
            return PB_PARSE_OK;
        }
        if (c == '\\') {
            p->pos += 1;
            if (p->pos >= p->len) {
                free(tmp);
                p->pos = open;
                return PB_PARSE_UNTERMINATED_STRING;
            }
            switch (p->src[p->pos]) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case '\\': c = '\\'; break;
            case '"': c = '"'; break;
            default:
                free(tmp);
                return PB_PARSE_INVALID_ESCAPE;
            }
        }
        if (len == cap) {
            const size_t next = cap == 0 ? 16 : cap * 2;
            char *new_tmp = realloc(tmp, next);
            if (new_tmp == NULL) {
                free(tmp);
                return PB_PARSE_OOM;
            }
            tmp = new_tmp;
            cap = next;
        }
        tmp[len] = c;
        len += 1;
        p->pos += 1;
    }

    free(tmp);
    p->pos = open;
    return PB_PARSE_UNTERMINATED_STRING;
}

static pb_parse_error parse_atom(parser *p, pb_value *out) {
    const size_t start = p->pos;
    while (p->pos < p->len) {
        const char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
            c == '(' || c == ')' || c == '[' || c == ']' || c == ';') {
            break;
        }
        p->pos += 1;
    }
    if (p->pos == start) {
        return PB_PARSE_UNEXPECTED_EOF;
    }

    pb_slice tok = {.ptr = p->src + start, .len = p->pos - start};
    if (slice_eq(tok, "nil")) {
        *out = (pb_value){.kind = PB_NIL};
        return PB_PARSE_OK;
    }
    if (slice_eq(tok, "true")) {
        *out = (pb_value){.kind = PB_BOOL, .boolean = true};
        return PB_PARSE_OK;
    }
    if (slice_eq(tok, "false")) {
        *out = (pb_value){.kind = PB_BOOL, .boolean = false};
        return PB_PARSE_OK;
    }

    double n = 0;
    if (parse_number(tok, &n)) {
        *out = (pb_value){.kind = PB_NUMBER, .number = n};
        return PB_PARSE_OK;
    }
    if (tok.ptr[0] == ':' && tok.len > 1) {
        char *owned = pb_arena_memdup(p->arena, tok.ptr + 1, tok.len - 1);
        if (owned == NULL) {
            return PB_PARSE_OOM;
        }
        *out = (pb_value){.kind = PB_KEYWORD, .text = {.ptr = owned, .len = tok.len - 1}};
        return PB_PARSE_OK;
    }
    char *owned = pb_arena_memdup(p->arena, tok.ptr, tok.len);
    if (owned == NULL) {
        return PB_PARSE_OOM;
    }
    *out = (pb_value){.kind = PB_SYMBOL, .text = {.ptr = owned, .len = tok.len}};
    return PB_PARSE_OK;
}

static pb_parse_error parse_one(parser *p, pb_value *out) {
    skip_ws(p);
    if (p->pos >= p->len) {
        return PB_PARSE_UNEXPECTED_EOF;
    }

    switch (p->src[p->pos]) {
    case '(':
        return parse_seq(p, ')', ']', PB_LIST, out);
    case '[':
        return parse_seq(p, ']', ')', PB_VECTOR, out);
    case ')':
        return PB_PARSE_UNEXPECTED_RPAREN;
    case ']':
        return PB_PARSE_UNEXPECTED_RBRACKET;
    case '"':
        return parse_string(p, out);
    default:
        return parse_atom(p, out);
    }
}

pb_parse_result pb_parse_all(pb_arena *arena, const char *src, size_t len) {
    parser p = {.arena = arena, .src = src, .len = len};
    value_vec forms = {0};

    for (;;) {
        skip_ws(&p);
        if (p.pos >= p.len) {
            const size_t count = forms.len;
            pb_value *items = pb_arena_alloc(arena, count * sizeof items[0], _Alignof(pb_value));
            if (items == NULL && count != 0) {
                free(forms.items);
                return (pb_parse_result){.err = PB_PARSE_OOM, .err_offset = p.pos};
            }
            if (count != 0) {
                memcpy(items, forms.items, count * sizeof items[0]);
            }
            free(forms.items);
            return (pb_parse_result){
                .err = PB_PARSE_OK,
                .forms = {.items = items, .len = count},
            };
        }

        pb_value v = {0};
        const pb_parse_error err = parse_one(&p, &v);
        if (err != PB_PARSE_OK) {
            free(forms.items);
            return (pb_parse_result){.err = err, .err_offset = p.pos};
        }
        if (!vec_push(&forms, v)) {
            free(forms.items);
            return (pb_parse_result){.err = PB_PARSE_OOM, .err_offset = p.pos};
        }
    }
}

const char *pb_parse_error_name(pb_parse_error err) {
    switch (err) {
    case PB_PARSE_OK: return "ok";
    case PB_PARSE_OOM: return "out of memory";
    case PB_PARSE_UNEXPECTED_EOF: return "unexpected eof";
    case PB_PARSE_UNEXPECTED_RPAREN: return "unexpected ')'";
    case PB_PARSE_UNEXPECTED_RBRACKET: return "unexpected ']'";
    case PB_PARSE_MISMATCHED_BRACKET: return "mismatched bracket";
    case PB_PARSE_UNTERMINATED_STRING: return "unterminated string";
    case PB_PARSE_INVALID_ESCAPE: return "invalid escape";
    case PB_PARSE_INVALID_NUMBER: return "invalid number";
    case PB_PARSE_INVALID_LIST_HEAD: return "invalid list head";
    case PB_PARSE_INVALID_JSON: return "invalid json";
    }
    return "unknown parse error";
}
