/**
 * @file ui.c
 *
 * ui:// resource registration and Apps-mount lifecycle. The CSP
 * snapshot is taken at resource creation time, not at mount time,
 * so later CSP mutations do not affect an already-mounted UI.
 */
#include "mcpkit/apps/ui.h"

#include <string.h>

#include "mcpkit/apps/csp.h"
#include "mcpkit/core/context.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/value.h"
#include "mcpkit/server/resource.h"

struct ui_data {
    char *doc;
};

struct mcp_apps_mount {
    mcp_session_t *session;
    mcp_apps_lifecycle_fn on_unmount;
    void *user_data;
};

static const mcp_allocator_t *alloc_of(mcp_context_t *ctx) {
    return ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
}

static void free_of(mcp_context_t *ctx, void *p) {
    if (p == NULL) {
        return;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    a->free_fn(p, a->userdata);
}

static void ui_cleanup(mcp_context_t *ctx, void *user_data) {
    struct ui_data *d = user_data;
    if (d == NULL) {
        return;
    }
    free_of(ctx, d->doc);
    free_of(ctx, d);
}

static mcp_status_t ui_read(mcp_context_t *ctx, mcp_session_t *session, const char *uri, void *u,
                            mcp_json_value_t **contents_out) {
    (void)session;
    const struct ui_data *d = u;
    if (d == NULL || d->doc == NULL || contents_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    mcp_json_value_t *item = mcp_json_object_new(ctx);
    mcp_json_value_t *uval = mcp_json_string_new(ctx, uri);
    mcp_json_value_t *mval = mcp_json_string_new(ctx, MCP_APPS_UI_MIME);
    mcp_json_value_t *tval = mcp_json_string_new(ctx, d->doc);
    if (arr == NULL || item == NULL || uval == NULL || mval == NULL || tval == NULL) {
        mcp_json_destroy(ctx, uval);
        mcp_json_destroy(ctx, mval);
        mcp_json_destroy(ctx, tval);
        mcp_json_destroy(ctx, item);
        mcp_json_destroy(ctx, arr);
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set_take(ctx, item, "uri", uval) != MCP_OK) {
        mcp_json_destroy(ctx, mval); /* unattached siblings */
        mcp_json_destroy(ctx, tval);
        mcp_json_destroy(ctx, item);
        mcp_json_destroy(ctx, arr);
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set_take(ctx, item, "mimeType", mval) != MCP_OK) {
        mcp_json_destroy(ctx, tval); /* unattached; item owns uval */
        mcp_json_destroy(ctx, item);
        mcp_json_destroy(ctx, arr);
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set_take(ctx, item, "text", tval) != MCP_OK ||
        mcp_json_array_append(ctx, arr, item) != MCP_OK) {
        mcp_json_destroy(ctx, item); /* owns uval/mval/tval */
        mcp_json_destroy(ctx, arr);
        return MCP_ERR_NOMEM;
    }
    *contents_out = arr;
    return MCP_OK;
}

mcp_resource_t *mcp_apps_ui_resource_new(mcp_context_t *ctx, const char *uri, const char *name,
                                         const char *html, const mcp_csp_t *csp_or_null) {
    if (uri == NULL || name == NULL || html == NULL ||
        strncmp(uri, MCP_APPS_UI_SCHEME, strlen(MCP_APPS_UI_SCHEME)) != 0) {
        return NULL;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    char *policy = NULL;
    mcp_csp_t *tmp = NULL;
    if (csp_or_null != NULL) {
        if (mcp_csp_serialize(ctx, csp_or_null, &policy) != MCP_OK) {
            return NULL;
        }
    } else {
        tmp = mcp_csp_default_deny_new(ctx);
        if (tmp == NULL || mcp_csp_serialize(ctx, tmp, &policy) != MCP_OK) {
            mcp_csp_destroy(ctx, tmp);
            return NULL;
        }
        mcp_csp_destroy(ctx, tmp);
    }
    struct ui_data *d = a->malloc_fn(sizeof(*d), a->userdata);
    if (d == NULL) {
        mcp_json_free_string(ctx, policy);
        return NULL;
    }
    d->doc = NULL;
    static const char kMetaPrefix[] = "<meta http-equiv=\"Content-Security-Policy\" content=\"";
    static const char kMetaSuffix[] = "\">\n";
    if (strstr(html, "Content-Security-Policy") != NULL) {
        size_t n = strlen(html) + 1;
        d->doc = a->malloc_fn(n, a->userdata);
        if (d->doc != NULL) {
            memcpy(d->doc, html, n);
        }
    } else {
        size_t n = sizeof(kMetaPrefix) - 1 + strlen(policy) + sizeof(kMetaSuffix) - 1 +
                   strlen(html) + 1;
        d->doc = a->malloc_fn(n, a->userdata);
        if (d->doc != NULL) {
            size_t pos = 0;
            memcpy(d->doc + pos, kMetaPrefix, sizeof(kMetaPrefix) - 1);
            pos += sizeof(kMetaPrefix) - 1;
            memcpy(d->doc + pos, policy, strlen(policy));
            pos += strlen(policy);
            memcpy(d->doc + pos, kMetaSuffix, sizeof(kMetaSuffix) - 1);
            pos += sizeof(kMetaSuffix) - 1;
            memcpy(d->doc + pos, html, strlen(html) + 1);
        }
    }
    mcp_json_free_string(ctx, policy);
    if (d->doc == NULL) {
        free_of(ctx, d);
        return NULL;
    }
    mcp_resource_t *res = mcp_resource_new(ctx, uri, name, MCP_APPS_UI_MIME, ui_read, d);
    if (res == NULL) {
        ui_cleanup(ctx, d);
        return NULL;
    }
    if (mcp_resource_set_cleanup(ctx, res, ui_cleanup) != MCP_OK) {
        mcp_resource_destroy(ctx, res);
        ui_cleanup(ctx, d);
        return NULL;
    }
    return res;
}

mcp_status_t mcp_apps_result_with_ui(mcp_context_t *ctx, mcp_json_value_t *result,
                                     const char *resource_uri) {
    if (result == NULL || resource_uri == NULL ||
        mcp_json_type(ctx, result) != MCP_JSON_OBJECT) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *meta = mcp_json_object_new(ctx);
    mcp_json_value_t *mui = mcp_json_object_new(ctx);
    mcp_json_value_t *muri = mcp_json_string_new(ctx, resource_uri);
    if (meta == NULL || mui == NULL || muri == NULL) {
        mcp_json_destroy(ctx, muri);
        mcp_json_destroy(ctx, mui);
        mcp_json_destroy(ctx, meta);
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set_take(ctx, mui, "resourceUri", muri) != MCP_OK) {
        mcp_json_destroy(ctx, mui);
        mcp_json_destroy(ctx, meta);
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set_take(ctx, meta, "ui", mui) != MCP_OK) {
        mcp_json_destroy(ctx, meta);
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set_take(ctx, result, "_meta", meta) != MCP_OK) {
        return MCP_ERR_NOMEM;
    }
    return MCP_OK;
}

mcp_status_t mcp_apps_mount(mcp_context_t *ctx, mcp_session_t *session,
                            mcp_apps_lifecycle_fn on_mount_or_null,
                            mcp_apps_lifecycle_fn on_unmount_or_null, void *user_data,
                            mcp_apps_mount_t **handle_out) {
    if (session == NULL || handle_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    mcp_apps_mount_t *h = a->malloc_fn(sizeof(*h), a->userdata);
    if (h == NULL) {
        return MCP_ERR_NOMEM;
    }
    h->session = session;
    h->on_unmount = on_unmount_or_null;
    h->user_data = user_data;
    if (on_mount_or_null != NULL) {
        on_mount_or_null(ctx, session, user_data);
    }
    *handle_out = h;
    return MCP_OK;
}

mcp_status_t mcp_apps_unmount(mcp_context_t *ctx, mcp_apps_mount_t *handle) {
    if (handle == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (handle->on_unmount != NULL) {
        handle->on_unmount(ctx, handle->session, handle->user_data);
    }
    free_of(ctx, handle);
    return MCP_OK;
}
