/**
 * @file schema.c
 *
 * JSON-Schema builder and validator.
 *
 * Builders (mcp_schema_*_new, mcp_schema_add_*, mcp_schema_set_*)
 * produce standard JSON-Schema document trees on the JSON DOM.
 * Validators (mcp_schema_validate, mcp_schema_validate_verbose)
 * perform recursive top-down type-checking; verbose mode fills a
 * caller-owned buffer with a dotted-path failure description.
 *
 * "integer" type check: a JSON number is an integer only when it is
 * finite and exactly representable as int64 (|d| <= 2^53-1 and d ==
 * (double)(int64_t)d). This is a libm-free approximation of the
 * JSON-Schema spec's "value is an integer" rule.
 */
#include "mcpkit/json/schema.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mcpkit/json/array.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/object.h"

#define SCHEMA_MAX_PATH 256

// Helper: create a schema object whose "type" key is set to the given
// JSON-Schema type name. On any allocation or set failure both the
// schema object and the type string are destroyed and NULL is returned.
static mcp_json_value_t *typed_new(mcp_context_t *ctx, const char *type) {
    mcp_json_value_t *s = mcp_json_object_new(ctx);
    mcp_json_value_t *t = s != NULL ? mcp_json_string_new(ctx, type) : NULL;
    if (s == NULL || t == NULL) {
        if (s != NULL) {
            mcp_json_destroy(ctx, s);
        }
        if (t != NULL) {
            mcp_json_destroy(ctx, t);
        }
        return NULL;
    }
    if (mcp_json_object_set(ctx, s, "type", t) != MCP_OK) {
        mcp_json_destroy(ctx, s);
        return NULL;
    }
    return s;
}

mcp_json_value_t *mcp_schema_object_new(mcp_context_t *ctx) {
    return typed_new(ctx, "object");
}

mcp_json_value_t *mcp_schema_string_new(mcp_context_t *ctx) {
    return typed_new(ctx, "string");
}

mcp_json_value_t *mcp_schema_integer_new(mcp_context_t *ctx) {
    return typed_new(ctx, "integer");
}

mcp_json_value_t *mcp_schema_number_new(mcp_context_t *ctx) {
    return typed_new(ctx, "number");
}

mcp_json_value_t *mcp_schema_boolean_new(mcp_context_t *ctx) {
    return typed_new(ctx, "boolean");
}

mcp_json_value_t *mcp_schema_array_new(mcp_context_t *ctx, mcp_json_value_t *items) {
    mcp_json_value_t *s = typed_new(ctx, "array");
    if (s == NULL) {
        if (items != NULL) {
            mcp_json_destroy(ctx, items);
        }
        return NULL;
    }
    if (items != NULL) {
        if (mcp_json_object_set(ctx, s, "items", items) != MCP_OK) {
            mcp_json_destroy(ctx, items);
            mcp_json_destroy(ctx, s);
            return NULL;
        }
    }
    return s;
}

// Returns a mutable pointer to the schema's "key" child, creating it
// (object or array per want_array) if absent. Returns NULL if the
// child exists with the wrong type or if creation fails; in neither
// case is any allocation leaked.
static mcp_json_value_t *ensure_child(mcp_context_t *ctx, mcp_json_value_t *schema,
                                      const char *key, bool want_array) {
    const mcp_json_value_t *old = mcp_json_object_get(ctx, schema, key);
    if (old != NULL) {
        bool ok = want_array ? mcp_json_type(ctx, old) == MCP_JSON_ARRAY
                             : mcp_json_type(ctx, old) == MCP_JSON_OBJECT;
        return ok ? (mcp_json_value_t *)old : NULL;
    }
    mcp_json_value_t *fresh = want_array ? mcp_json_array_new(ctx) : mcp_json_object_new(ctx);
    if (fresh == NULL || mcp_json_object_set(ctx, schema, key, fresh) != MCP_OK) {
        if (fresh != NULL) {
            mcp_json_destroy(ctx, fresh);
        }
        return NULL;
    }
    return fresh;
}

mcp_status_t mcp_schema_add_property(mcp_context_t *ctx, mcp_json_value_t *schema,
                                     const char *name, mcp_json_value_t *prop) {
    if (schema == NULL || mcp_json_type(ctx, schema) != MCP_JSON_OBJECT || name == NULL ||
        prop == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *props = ensure_child(ctx, schema, "properties", false);
    if (props == NULL || mcp_json_object_set(ctx, props, name, prop) != MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    return MCP_OK;
}

mcp_status_t mcp_schema_add_required(mcp_context_t *ctx, mcp_json_value_t *schema,
                                     const char *name) {
    if (schema == NULL || mcp_json_type(ctx, schema) != MCP_JSON_OBJECT || name == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *req = ensure_child(ctx, schema, "required", true);
    mcp_json_value_t *s = req != NULL ? mcp_json_string_new(ctx, name) : NULL;
    if (s == NULL || mcp_json_array_append(ctx, req, s) != MCP_OK) {
        if (s != NULL) {
            mcp_json_destroy(ctx, s);
        }
        return MCP_ERR_INVALID_ARGUMENT;
    }
    return MCP_OK;
}

mcp_status_t mcp_schema_add_enum(mcp_context_t *ctx, mcp_json_value_t *schema,
                                 mcp_json_value_t *values) {
    if (schema == NULL || mcp_json_type(ctx, schema) != MCP_JSON_OBJECT || values == NULL ||
        mcp_json_type(ctx, values) != MCP_JSON_ARRAY) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (mcp_json_object_set(ctx, schema, "enum", values) != MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    return MCP_OK;
}

static mcp_status_t set_bound(mcp_context_t *ctx, mcp_json_value_t *schema, const char *key,
                              double b) {
    if (schema == NULL || mcp_json_type(ctx, schema) != MCP_JSON_OBJECT || !isfinite(b)) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *v = mcp_json_number_new(ctx, b);
    if (v == NULL || mcp_json_object_set(ctx, schema, key, v) != MCP_OK) {
        if (v != NULL) {
            mcp_json_destroy(ctx, v);
        }
        return MCP_ERR_NOMEM;
    }
    return MCP_OK;
}

mcp_status_t mcp_schema_set_minimum(mcp_context_t *ctx, mcp_json_value_t *schema, double min) {
    return set_bound(ctx, schema, "minimum", min);
}

mcp_status_t mcp_schema_set_maximum(mcp_context_t *ctx, mcp_json_value_t *schema, double max) {
    return set_bound(ctx, schema, "maximum", max);
}

mcp_status_t mcp_schema_set_description(mcp_context_t *ctx, mcp_json_value_t *schema,
                                        const char *desc) {
    if (schema == NULL || mcp_json_type(ctx, schema) != MCP_JSON_OBJECT || desc == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *v = mcp_json_string_new(ctx, desc);
    if (v == NULL || mcp_json_object_set(ctx, schema, "description", v) != MCP_OK) {
        if (v != NULL) {
            mcp_json_destroy(ctx, v);
        }
        return MCP_ERR_NOMEM;
    }
    return MCP_OK;
}

// Checks whether a JSON number is an integer per JSON-Schema rules.
// A finite double is an integer when |d| <= 2^53-1 and d casts
// losslessly to int64_t. Values beyond 2^53-1 (e.g. 1e300) must
// be rejected even though they are finite.
static bool is_integer_value(double d) {
    // JSON Schema "integer": value must be finite and exactly representable as int64.
    if (!isfinite(d) || d < -9007199254740991.0 || d > 9007199254740991.0) {
        return false;
    }
    return d == (double)(int64_t)d;
}

static bool value_equal(mcp_context_t *ctx, const mcp_json_value_t *a,
                        const mcp_json_value_t *b) {
    mcp_json_type_t ta = mcp_json_type(ctx, a);
    if (ta != mcp_json_type(ctx, b)) {
        return false;
    }
    switch (ta) {
        case MCP_JSON_NULL:
            return true;
        case MCP_JSON_BOOL: {
            bool x = false, y = true;
            return mcp_json_bool_value(ctx, a, &x) == MCP_OK &&
                   mcp_json_bool_value(ctx, b, &y) == MCP_OK && x == y;
        }
        case MCP_JSON_NUMBER: {
            double x = 0, y = 0;
            return mcp_json_number_value(ctx, a, &x) == MCP_OK &&
                   mcp_json_number_value(ctx, b, &y) == MCP_OK && x == y;
        }
        case MCP_JSON_STRING: {
            const char *x = NULL, *y = NULL;
            return mcp_json_string_value(ctx, a, &x) == MCP_OK &&
                   mcp_json_string_value(ctx, b, &y) == MCP_OK && strcmp(x, y) == 0;
        }
        case MCP_JSON_ARRAY: {
            size_t n = mcp_json_array_size(ctx, a);
            if (n != mcp_json_array_size(ctx, b)) {
                return false;
            }
            for (size_t i = 0; i < n; i++) {
                if (!value_equal(ctx, mcp_json_array_get(ctx, a, i),
                                 mcp_json_array_get(ctx, b, i))) {
                    return false;
                }
            }
            return true;
        }
        case MCP_JSON_OBJECT: {
            size_t n = mcp_json_object_size(ctx, a);
            if (n != mcp_json_object_size(ctx, b)) {
                return false;
            }
            for (size_t i = 0; i < n; i++) {
                const char *k = mcp_json_object_key_at(ctx, a, i);
                const mcp_json_value_t *vb = mcp_json_object_get(ctx, b, k);
                if (vb == NULL ||
                    !value_equal(ctx, mcp_json_object_get(ctx, a, k), vb)) {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

static void fail_at(char *err, size_t errcap, const char *path, const char *reason) {
    if (err == NULL || errcap == 0) {
        return;
    }
    snprintf(err, errcap, "%s: %s", path[0] != '\0' ? path : "<root>", reason);
    err[errcap - 1] = '\0';
}

static mcp_status_t check(mcp_context_t *ctx, const mcp_json_value_t *schema,
                          const mcp_json_value_t *inst, const char *path, char *err,
                          size_t errcap);

static mcp_status_t check_object(mcp_context_t *ctx, const mcp_json_value_t *schema,
                                 const mcp_json_value_t *inst, const char *path, char *err,
                                 size_t errcap) {
    const mcp_json_value_t *req = mcp_json_object_get(ctx, schema, "required");
    if (req != NULL) {
        if (mcp_json_type(ctx, req) != MCP_JSON_ARRAY) {
            fail_at(err, errcap, path, "required is not an array");
            return MCP_ERR_INVALID_ARGUMENT;
        }
        size_t n = mcp_json_array_size(ctx, req);
        for (size_t i = 0; i < n; i++) {
            const mcp_json_value_t *e = mcp_json_array_get(ctx, req, i);
            const char *name = NULL;
            if (e == NULL || mcp_json_string_value(ctx, e, &name) != MCP_OK ||
                !mcp_json_object_has(ctx, inst, name)) {
                fail_at(err, errcap, path, "missing required property");
                return MCP_ERR_INVALID_ARGUMENT;
            }
        }
    }
    const mcp_json_value_t *props = mcp_json_object_get(ctx, schema, "properties");
    if (props != NULL) {
        if (mcp_json_type(ctx, props) != MCP_JSON_OBJECT) {
            fail_at(err, errcap, path, "properties is not an object");
            return MCP_ERR_INVALID_ARGUMENT;
        }
        size_t n = mcp_json_object_size(ctx, props);
        for (size_t i = 0; i < n; i++) {
            const char *name = mcp_json_object_key_at(ctx, props, i);
            const mcp_json_value_t *sub = mcp_json_object_get(ctx, props, name);
            const mcp_json_value_t *val = mcp_json_object_get(ctx, inst, name);
            if (val == NULL) {
                continue;
            }
            char sub_path[SCHEMA_MAX_PATH];
            snprintf(sub_path, sizeof(sub_path), "%s%s%s", path,
                     path[0] != '\0' ? "." : "", name);
            sub_path[sizeof(sub_path) - 1] = '\0';
            mcp_status_t st = check(ctx, sub, val, sub_path, err, errcap);
            if (st != MCP_OK) {
                return st;
            }
        }
    }
    return MCP_OK;
}

static mcp_status_t check(mcp_context_t *ctx, const mcp_json_value_t *schema,
                          const mcp_json_value_t *inst, const char *path, char *err,
                          size_t errcap) {
    if (schema == NULL || mcp_json_type(ctx, schema) != MCP_JSON_OBJECT || inst == NULL) {
        fail_at(err, errcap, path, "bad schema or instance");
        return MCP_ERR_INVALID_ARGUMENT;
    }
    const mcp_json_value_t *typev = mcp_json_object_get(ctx, schema, "type");
    const char *type = NULL;
    if (typev != NULL) {
        if (mcp_json_string_value(ctx, typev, &type) != MCP_OK || type == NULL) {
            fail_at(err, errcap, path, "type is not a string");
            return MCP_ERR_INVALID_ARGUMENT;
        }
        bool match = false;
        if (strcmp(type, "string") == 0) {
            match = mcp_json_type(ctx, inst) == MCP_JSON_STRING;
        } else if (strcmp(type, "integer") == 0) {
            double d = 0;
            match = mcp_json_number_value(ctx, inst, &d) == MCP_OK && is_integer_value(d);
        } else if (strcmp(type, "number") == 0) {
            double d = 0;
            match = mcp_json_number_value(ctx, inst, &d) == MCP_OK && isfinite(d);
        } else if (strcmp(type, "boolean") == 0) {
            match = mcp_json_type(ctx, inst) == MCP_JSON_BOOL;
        } else if (strcmp(type, "array") == 0) {
            match = mcp_json_type(ctx, inst) == MCP_JSON_ARRAY;
        } else if (strcmp(type, "object") == 0) {
            match = mcp_json_type(ctx, inst) == MCP_JSON_OBJECT;
        } else {
            fail_at(err, errcap, path, "unknown type");
            return MCP_ERR_INVALID_ARGUMENT;
        }
        if (!match) {
            fail_at(err, errcap, path, "type mismatch");
            return MCP_ERR_INVALID_ARGUMENT;
        }
    }
    const mcp_json_value_t *en = mcp_json_object_get(ctx, schema, "enum");
    if (en != NULL) {
        if (mcp_json_type(ctx, en) != MCP_JSON_ARRAY) {
            fail_at(err, errcap, path, "enum is not an array");
            return MCP_ERR_INVALID_ARGUMENT;
        }
        bool found = false;
        size_t n = mcp_json_array_size(ctx, en);
        for (size_t i = 0; i < n; i++) {
            if (value_equal(ctx, inst, mcp_json_array_get(ctx, en, i))) {
                found = true;
                break;
            }
        }
        if (!found) {
            fail_at(err, errcap, path, "not in enum");
            return MCP_ERR_INVALID_ARGUMENT;
        }
    }
    mcp_json_type_t it = mcp_json_type(ctx, inst);
    if (it == MCP_JSON_OBJECT) {
        return check_object(ctx, schema, inst, path, err, errcap);
    }
    if (it == MCP_JSON_ARRAY) {
        const mcp_json_value_t *items = mcp_json_object_get(ctx, schema, "items");
        if (items != NULL) {
            size_t n = mcp_json_array_size(ctx, inst);
            for (size_t i = 0; i < n; i++) {
                char sub_path[SCHEMA_MAX_PATH];
                snprintf(sub_path, sizeof(sub_path), "%s[%zu]", path, i);
                sub_path[sizeof(sub_path) - 1] = '\0';
                mcp_status_t st = check(ctx, items, mcp_json_array_get(ctx, inst, i),
                                        sub_path, err, errcap);
                if (st != MCP_OK) {
                    return st;
                }
            }
        }
        return MCP_OK;
    }
    if (it == MCP_JSON_NUMBER) {
        double d = 0;
        mcp_json_number_value(ctx, inst, &d);
        const mcp_json_value_t *lo = mcp_json_object_get(ctx, schema, "minimum");
        const mcp_json_value_t *hi = mcp_json_object_get(ctx, schema, "maximum");
        double bound = 0;
        if (lo != NULL && mcp_json_number_value(ctx, lo, &bound) == MCP_OK && d < bound) {
            fail_at(err, errcap, path, "below minimum");
            return MCP_ERR_INVALID_ARGUMENT;
        }
        if (hi != NULL && mcp_json_number_value(ctx, hi, &bound) == MCP_OK && d > bound) {
            fail_at(err, errcap, path, "above maximum");
            return MCP_ERR_INVALID_ARGUMENT;
        }
    }
    return MCP_OK;
}

mcp_status_t mcp_schema_validate_verbose(mcp_context_t *ctx, const mcp_json_value_t *schema,
                                         const mcp_json_value_t *instance,
                                         char *buf, size_t cap) {
    if (buf != NULL && cap > 0) {
        buf[0] = '\0';
    }
    return check(ctx, schema, instance, "", buf, buf != NULL ? cap : 0);
}

mcp_status_t mcp_schema_validate(mcp_context_t *ctx, const mcp_json_value_t *schema,
                                 const mcp_json_value_t *instance) {
    return mcp_schema_validate_verbose(ctx, schema, instance, NULL, 0);
}
