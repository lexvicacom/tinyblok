#include "pb_json.h"

#include "yyjson.h"

#include <stdlib.h>
#include <string.h>

// Temporary growable list before freezing items into the arena.
typedef struct value_vec {
    pb_value *items;
    size_t len;
    size_t cap;
} value_vec;

// JSON-to-AST conversion state; err carries the first failure.
typedef struct json_conv {
    pb_arena *arena;
    pb_parse_error err;
} json_conv;

static bool slice_eq(pb_slice s, const char *lit) {
    const size_t n = strlen(lit);
    return s.len == n && memcmp(s.ptr, lit, n) == 0;
}

static bool vec_push(value_vec *v, pb_value item) {
    if (v->len == v->cap) {
        const size_t next = v->cap == 0 ? 8 : v->cap * 2;
        pb_value *items = realloc(v->items, next * sizeof items[0]);
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

static pb_value text_value(json_conv *c, pb_value_kind kind, const char *ptr, size_t len) {
    char *owned = pb_arena_memdup(c->arena, ptr, len);
    if (owned == NULL) {
        c->err = PB_PARSE_OOM;
        return (pb_value){0};
    }
    return (pb_value){.kind = kind, .text = {.ptr = owned, .len = len}};
}

static pb_value string_value(json_conv *c, yyjson_val *v) {
    const char *s = yyjson_get_str(v);
    const size_t len = yyjson_get_len(v);
    pb_slice raw = {.ptr = s, .len = len};
    if (slice_eq(raw, "subject") || slice_eq(raw, "payload") ||
        slice_eq(raw, "payload-float") || slice_eq(raw, "payload-int")) {
        return text_value(c, PB_SYMBOL, s, len);
    }
    return text_value(c, PB_STRING, s, len);
}

static bool freeze_vec(json_conv *c, value_vec *v, pb_value_kind kind, pb_value *out) {
    pb_value *items = pb_arena_alloc(c->arena, v->len * sizeof items[0], _Alignof(pb_value));
    if (items == NULL && v->len != 0) {
        c->err = PB_PARSE_OOM;
        return false;
    }
    if (v->len != 0) {
        memcpy(items, v->items, v->len * sizeof items[0]);
    }
    *out = (pb_value){.kind = kind, .seq = {.items = items, .len = v->len}};
    return true;
}

static pb_value convert_expr(json_conv *c, yyjson_val *v);

static bool convert_vec_array(json_conv *c, yyjson_val *array, pb_value *out) {
    value_vec items = {0};
    size_t idx = 0;
    size_t max = 0;
    yyjson_val *item = NULL;
    yyjson_arr_foreach(array, idx, max, item) {
        if (!vec_push(&items, convert_expr(c, item))) {
            c->err = PB_PARSE_OOM;
        }
        if (c->err != PB_PARSE_OK) {
            free(items.items);
            return false;
        }
    }
    const bool ok = freeze_vec(c, &items, PB_VECTOR, out);
    free(items.items);
    return ok;
}

static bool convert_config_value(json_conv *c, yyjson_val *v, pb_value *out) {
    if (yyjson_is_arr(v)) {
        return convert_vec_array(c, v, out);
    }
    *out = convert_expr(c, v);
    return c->err == PB_PARSE_OK;
}

static bool convert_special_object(json_conv *c, yyjson_val *obj, pb_value *out) {
    if (!yyjson_is_obj(obj) || yyjson_obj_size(obj) != 1) {
        return false;
    }
    yyjson_val *key = NULL;
    yyjson_val *val = NULL;
    size_t idx = 0;
    size_t max = 0;
    yyjson_obj_foreach(obj, idx, max, key, val) {
        break;
    }
    if (key == NULL || val == NULL) {
        return false;
    }
    const char *name = yyjson_get_str(key);
    const size_t name_len = yyjson_get_len(key);
    if (name_len == 3 && memcmp(name, "sym", 3) == 0 && yyjson_is_str(val)) {
        *out = text_value(c, PB_SYMBOL, yyjson_get_str(val), yyjson_get_len(val));
        return c->err == PB_PARSE_OK;
    }
    if (name_len == 3 && memcmp(name, "str", 3) == 0 && yyjson_is_str(val)) {
        *out = text_value(c, PB_STRING, yyjson_get_str(val), yyjson_get_len(val));
        return c->err == PB_PARSE_OK;
    }
    if (name_len == 2 && memcmp(name, "kw", 2) == 0 && yyjson_is_str(val)) {
        *out = text_value(c, PB_KEYWORD, yyjson_get_str(val), yyjson_get_len(val));
        return c->err == PB_PARSE_OK;
    }
    if (name_len == 3 && memcmp(name, "vec", 3) == 0 && yyjson_is_arr(val)) {
        return convert_vec_array(c, val, out);
    }
    return false;
}

static bool convert_form(json_conv *c, yyjson_val *array, pb_value *out) {
    if (!yyjson_is_arr(array) || yyjson_arr_size(array) == 0) {
        c->err = PB_PARSE_INVALID_LIST_HEAD;
        return false;
    }

    yyjson_val *head = yyjson_arr_get_first(array);
    if (!yyjson_is_str(head)) {
        c->err = PB_PARSE_INVALID_LIST_HEAD;
        return false;
    }

    value_vec items = {0};
    if (!vec_push(&items, text_value(c, PB_SYMBOL, yyjson_get_str(head), yyjson_get_len(head)))) {
        c->err = PB_PARSE_OOM;
    }
    if (c->err != PB_PARSE_OK) {
        free(items.items);
        return false;
    }

    size_t idx = 0;
    size_t max = 0;
    yyjson_val *item = NULL;
    yyjson_arr_foreach(array, idx, max, item) {
        if (idx == 0) {
            continue;
        }
        pb_value special = {0};
        if (convert_special_object(c, item, &special)) {
            if (!vec_push(&items, special)) {
                c->err = PB_PARSE_OOM;
            }
        } else if (yyjson_is_obj(item)) {
            yyjson_val *key = NULL;
            yyjson_val *val = NULL;
            size_t map_idx = 0;
            size_t map_max = 0;
            yyjson_obj_foreach(item, map_idx, map_max, key, val) {
                if (!vec_push(&items, text_value(c, PB_KEYWORD, yyjson_get_str(key), yyjson_get_len(key)))) {
                    c->err = PB_PARSE_OOM;
                    break;
                }
                pb_value ast = {0};
                if (!convert_config_value(c, val, &ast) || !vec_push(&items, ast)) {
                    c->err = c->err == PB_PARSE_OK ? PB_PARSE_OOM : c->err;
                    break;
                }
            }
        } else {
            if (!vec_push(&items, convert_expr(c, item))) {
                c->err = PB_PARSE_OOM;
            }
        }
        if (c->err != PB_PARSE_OK) {
            free(items.items);
            return false;
        }
    }

    const bool ok = freeze_vec(c, &items, PB_LIST, out);
    free(items.items);
    return ok;
}

static pb_value convert_expr(json_conv *c, yyjson_val *v) {
    if (yyjson_is_null(v)) {
        return (pb_value){.kind = PB_NIL};
    }
    if (yyjson_is_bool(v)) {
        return (pb_value){.kind = PB_BOOL, .boolean = yyjson_get_bool(v)};
    }
    if (yyjson_is_num(v)) {
        return (pb_value){.kind = PB_NUMBER, .number = yyjson_get_num(v)};
    }
    if (yyjson_is_str(v)) {
        return string_value(c, v);
    }
    if (yyjson_is_arr(v)) {
        pb_value out = {0};
        (void)convert_form(c, v, &out);
        return out;
    }
    if (yyjson_is_obj(v)) {
        pb_value out = {0};
        if (convert_special_object(c, v, &out)) {
            return out;
        }
    }
    c->err = PB_PARSE_INVALID_JSON;
    return (pb_value){0};
}

static bool looks_json(const char *path, const char *src, size_t len) {
    const size_t path_len = strlen(path);
    if (path_len >= 5 && memcmp(path + path_len - 5, ".json", 5) == 0) {
        return true;
    }
    for (size_t i = 0; i < len; i += 1) {
        const char c = src[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        return c == '[' || c == '{';
    }
    return false;
}

static pb_parse_result parse_json_patchbay(pb_arena *arena, const char *src, size_t len) {
    char *json_buf = malloc(len == 0 ? 1 : len);
    if (json_buf == NULL) {
        return (pb_parse_result){.err = PB_PARSE_OOM};
    }
    memcpy(json_buf, src, len);

    yyjson_read_err err = {0};
    yyjson_doc *doc = yyjson_read_opts(json_buf, len, YYJSON_READ_NOFLAG, NULL, &err);
    if (doc == NULL) {
        free(json_buf);
        return (pb_parse_result){.err = PB_PARSE_INVALID_JSON, .err_offset = err.pos};
    }

    json_conv c = {.arena = arena, .err = PB_PARSE_OK};
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_arr(root)) {
        yyjson_doc_free(doc);
        free(json_buf);
        return (pb_parse_result){.err = PB_PARSE_INVALID_JSON, .err_offset = 0};
    }

    value_vec forms = {0};
    yyjson_val *first = yyjson_arr_get_first(root);
    if (yyjson_is_str(first)) {
        pb_value form = {0};
        if (convert_form(&c, root, &form) && !vec_push(&forms, form)) {
            c.err = PB_PARSE_OOM;
        }
    } else {
        size_t idx = 0;
        size_t max = 0;
        yyjson_val *item = NULL;
        yyjson_arr_foreach(root, idx, max, item) {
            pb_value form = {0};
            if (!convert_form(&c, item, &form) || !vec_push(&forms, form)) {
                c.err = c.err == PB_PARSE_OK ? PB_PARSE_OOM : c.err;
                break;
            }
        }
    }
    yyjson_doc_free(doc);
    free(json_buf);

    if (c.err != PB_PARSE_OK) {
        free(forms.items);
        return (pb_parse_result){.err = c.err, .err_offset = 0};
    }
    pb_value *items = pb_arena_alloc(arena, forms.len * sizeof items[0], _Alignof(pb_value));
    if (items == NULL && forms.len != 0) {
        free(forms.items);
        return (pb_parse_result){.err = PB_PARSE_OOM};
    }
    if (forms.len != 0) {
        memcpy(items, forms.items, forms.len * sizeof items[0]);
    }
    const size_t count = forms.len;
    free(forms.items);
    return (pb_parse_result){.err = PB_PARSE_OK, .forms = {.items = items, .len = count}};
}

pb_parse_result pb_parse_patchbay_source(pb_arena *arena, const char *path, const char *src, size_t len) {
    if (looks_json(path, src, len)) {
        return parse_json_patchbay(arena, src, len);
    }
    return pb_parse_all(arena, src, len);
}
