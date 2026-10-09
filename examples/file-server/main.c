#define _DEFAULT_SOURCE

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mcpkit/mcpkit.h"

#define FILE_SERVER_MAX_BYTES (1024u * 1024u)
#define FILE_SERVER_DEFAULT_LIMIT 200L
#define FILE_SERVER_MAX_LIMIT 2000L

static char g_root[PATH_MAX];

static mcp_status_t set_new_string(mcp_context_t *ctx, mcp_json_value_t *obj,
                                   const char *key, const char *val) {
    mcp_json_value_t *v = mcp_json_string_new(ctx, val);
    if (v == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t st = mcp_json_object_set(ctx, obj, key, v);
    if (st != MCP_OK) {
        mcp_json_destroy(ctx, v);
    }
    return st;
}

/* Resolve argv --root (or cwd) to a canonical absolute path. */
static int resolve_root(const char *arg) {
    char probe[PATH_MAX];
    if (arg == NULL) {
        if (getcwd(probe, sizeof(probe)) == NULL) {
            return -1;
        }
    } else {
        size_t n = strlen(arg);
        if (n == 0 || n >= sizeof(probe)) {
            return -1;
        }
        memcpy(probe, arg, n + 1);
    }
    if (realpath(probe, g_root) == NULL) {
        return -1;
    }
    struct stat st;
    if (stat(g_root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return -1;
    }
    return 0;
}

/* Join root + user path (or take absolute path as-is), canonicalize,
 * and require the result to stay inside g_root. */
static int resolve_inside_root(const char *path, char *out, size_t out_size) {
    char candidate[PATH_MAX];
    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    if (path[0] == '/') {
        if (strlen(path) >= sizeof(candidate)) {
            return -1;
        }
        strcpy(candidate, path);
    } else {
        size_t root_len = strlen(g_root);
        size_t path_len = strlen(path);
        if (root_len + 1 + path_len + 1 > sizeof(candidate)) {
            return -1;
        }
        memcpy(candidate, g_root, root_len);
        candidate[root_len] = '/';
        memcpy(candidate + root_len + 1, path, path_len + 1);
    }
    if (realpath(candidate, out) == NULL) {
        return -1;
    }
    if (out_size == 0) {
        return -1;
    }
    size_t root_len = strlen(g_root);
    if (strncmp(out, g_root, root_len) != 0) {
        return -1;
    }
    /* Prefix match must end on a boundary: g_root itself or "g_root/...". */
    if (out[root_len] != '\0' && out[root_len] != '/') {
        return -1;
    }
    return 0;
}

static mcp_status_t get_optional_long(mcp_context_t *ctx, const mcp_json_value_t *args,
                                      const char *key, long fallback, long *out) {
    const mcp_json_value_t *v = mcp_json_object_get(ctx, args, key);
    if (v == NULL) {
        *out = fallback;
        return MCP_OK;
    }
    double d = 0.0;
    if (mcp_json_number_value(ctx, v, &d) != MCP_OK || d < 0.0 ||
        d > (double)LONG_MAX) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *out = (long)d;
    return MCP_OK;
}

/* Build "NNN: line" output for lines [offset, offset+limit). The buffer is
 * read-only here; the caller frees the returned text with free(). */
static char *slice_lines(const char *buf, size_t len, long offset, long limit) {
    /* First pass: count lines. */
    long total = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            total++;
        }
    }
    if (len > 0 && buf[len - 1] != '\n') {
        total++;
    }

    long begin = offset < total ? offset : total;
    long end = begin + limit < total ? begin + limit : total;

    /* Second pass: measure output size. */
    size_t need = 1; /* NUL */
    long lineno = 0;
    size_t line_start = 0;
    for (size_t i = 0; i <= len; i++) {
        int eol = (i == len) || (buf[i] == '\n');
        if (!eol) {
            continue;
        }
        if (lineno >= begin && lineno < end) {
            /* "%ld: " prefix (20 chars is plenty) + line + '\n'. */
            need += 20 + (i - line_start) + 1;
        }
        lineno++;
        line_start = i + 1;
        if (lineno >= end) {
            break;
        }
    }

    char *text = malloc(need);
    if (text == NULL) {
        return NULL;
    }
    size_t pos = 0;
    lineno = 0;
    line_start = 0;
    for (size_t i = 0; i <= len; i++) {
        int eol = (i == len) || (buf[i] == '\n');
        if (!eol) {
            continue;
        }
        if (lineno >= begin && lineno < end) {
            int w = snprintf(text + pos, need - pos, "%ld: %.*s\n", lineno + 1,
                             (int)(i - line_start), buf + line_start);
            if (w < 0 || (size_t)w >= need - pos) {
                free(text);
                return NULL;
            }
            pos += (size_t)w;
        }
        lineno++;
        line_start = i + 1;
        if (lineno >= end) {
            break;
        }
    }
    text[pos] = '\0';
    return text;
}

static mcp_status_t read_file_handler(mcp_context_t *ctx, mcp_session_t *session,
                                      const mcp_json_value_t *args, void *user_data,
                                      mcp_json_value_t **result_out) {
    (void)session;
    (void)user_data;

    const mcp_json_value_t *pathv = mcp_json_object_get(ctx, args, "path");
    const char *path = NULL;
    if (pathv == NULL || mcp_json_string_value(ctx, pathv, &path) != MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    long offset = 0;
    long limit = FILE_SERVER_DEFAULT_LIMIT;
    if (get_optional_long(ctx, args, "offset", 0, &offset) != MCP_OK ||
        get_optional_long(ctx, args, "limit", FILE_SERVER_DEFAULT_LIMIT, &limit) !=
            MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (limit > FILE_SERVER_MAX_LIMIT) {
        limit = FILE_SERVER_MAX_LIMIT;
    }

    char resolved[PATH_MAX];
    if (resolve_inside_root(path, resolved, sizeof(resolved)) != 0) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode) ||
        (unsigned long)st.st_size > FILE_SERVER_MAX_BYTES) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    FILE *fp = fopen(resolved, "rb");
    if (fp == NULL) {
        return MCP_ERR_IO;
    }
    size_t size = (size_t)st.st_size;
    char *buf = malloc(size + 1);
    if (buf == NULL) {
        fclose(fp);
        return MCP_ERR_NOMEM;
    }
    size_t got = fread(buf, 1, size, fp);
    int read_err = ferror(fp);
    fclose(fp);
    if (read_err || got != size) {
        free(buf);
        return MCP_ERR_IO;
    }
    buf[size] = '\0';
    if (memchr(buf, '\0', size) != NULL) {
        /* Binary file: refuse rather than truncate at the first NUL. */
        free(buf);
        return MCP_ERR_INVALID_ARGUMENT;
    }

    char *text = slice_lines(buf, size, offset, limit);
    free(buf);
    if (text == NULL) {
        return MCP_ERR_NOMEM;
    }

    mcp_json_value_t *item = mcp_json_object_new(ctx);
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_status_t stt = MCP_ERR_NOMEM;
    int item_linked = 0;
    int content_linked = 0;
    if (item != NULL && content != NULL && result != NULL &&
        set_new_string(ctx, item, "type", "text") == MCP_OK &&
        set_new_string(ctx, item, "text", text) == MCP_OK &&
        mcp_json_array_append(ctx, content, item) == MCP_OK) {
        /* append took ownership of item on success. */
        item_linked = 1;
        if (mcp_json_object_set(ctx, result, "content", content) == MCP_OK) {
            content_linked = 1;
            stt = MCP_OK;
        }
    }
    free(text);
    if (stt != MCP_OK) {
        if (!item_linked) {
            mcp_json_destroy(ctx, item);
        }
        if (!content_linked) {
            /* content still owns item when linked. */
            mcp_json_destroy(ctx, content);
        }
        mcp_json_destroy(ctx, result);
        return stt;
    }
    *result_out = result;
    return MCP_OK;
}

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [--root <dir>]\n", prog);
}

int main(int argc, char **argv) {
    /* A closed peer must surface as EPIPE/MCP_ERR_IO, not a SIGPIPE kill. */
    signal(SIGPIPE, SIG_IGN);

    const char *root_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--root") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 2;
            }
            root_arg = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (resolve_root(root_arg) != 0) {
        fprintf(stderr, "invalid --root '%s': %s\n", root_arg != NULL ? root_arg : ".",
                strerror(errno));
        return 2;
    }

    int rc = 1;
    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        fprintf(stderr, "context create failed\n");
        return 1;
    }
    mcp_server_t *srv = mcp_server_create(ctx, "file-server", "0.1.0");
    if (srv == NULL) {
        fprintf(stderr, "server create failed\n");
        goto done;
    }
    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    if (schema == NULL ||
        mcp_schema_add_property(ctx, schema, "path", mcp_schema_string_new(ctx)) !=
            MCP_OK ||
        mcp_schema_add_property(ctx, schema, "offset",
                                mcp_schema_integer_new(ctx)) != MCP_OK ||
        mcp_schema_add_property(ctx, schema, "limit",
                                mcp_schema_integer_new(ctx)) != MCP_OK ||
        mcp_schema_add_required(ctx, schema, "path") != MCP_OK ||
        mcp_server_add_tool(
            ctx, srv,
            mcp_tool_new(ctx, "read_file", "Read a text file under the server root "
                                          "with line offset/limit pagination. Lines are "
                                          "1-based in the output.",
                         schema, read_file_handler, NULL)) != MCP_OK) {
        fprintf(stderr, "tool register failed\n");
        goto done;
    }
    mcp_transport_t *t = mcp_stdio_transport_create(ctx, NULL, NULL);
    if (t == NULL) {
        fprintf(stderr, "transport create failed\n");
        goto done;
    }
    if (mcp_transport_start(ctx, t) != MCP_OK) {
        fprintf(stderr, "transport start failed\n");
        goto done_transport;
    }
    if (mcp_stdio_serve(ctx, srv, t) != MCP_OK) {
        fprintf(stderr, "serve failed\n");
        goto done_transport;
    }
    rc = 0;

done_transport:
    mcp_transport_stop(ctx, t);
    mcp_transport_destroy(ctx, t);
done:
    if (srv != NULL) {
        mcp_server_destroy(ctx, srv);
    }
    mcp_context_destroy(ctx);
    return rc;
}
