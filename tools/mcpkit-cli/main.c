// glibc 2.44 requires _POSIX_C_SOURCE 200809L before any system header for
// pipe/fdopen/kill/waitpid to be declared (not just visible via -Wno-implicit).
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <time.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/protocol/validate.h"

typedef struct {
    mcp_context_t *ctx;
    mcp_transport_t *transport;
    mcp_client_t *client;
    FILE *in;
    FILE *out;
    pid_t pid;
} cli_t;

static void cli_cleanup(cli_t *cli) {
    if (cli->client != NULL) mcp_client_destroy(cli->ctx, cli->client);
    if (cli->transport != NULL) mcp_transport_destroy(cli->ctx, cli->transport);
    if (cli->in != NULL) fclose(cli->in);
    if (cli->out != NULL) fclose(cli->out);
    if (cli->pid > 0) {
        kill(cli->pid, SIGTERM);
        int st;
        (void)waitpid(cli->pid, &st, 0);
    }
    if (cli->ctx != NULL) mcp_context_destroy(cli->ctx);
}

static int spawn(const char *server_bin, cli_t *cli) {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        if (in_pipe[0] != -1) close(in_pipe[0]);
        if (in_pipe[1] != -1) close(in_pipe[1]);
        if (out_pipe[0] != -1) close(out_pipe[0]);
        if (out_pipe[1] != -1) close(out_pipe[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        // child: close parent-side ends; child stdin = read end of in_pipe,
        // child stdout = write end of out_pipe
        close(in_pipe[1]);
        close(out_pipe[0]);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]);
        close(out_pipe[1]);
        execl(server_bin, server_bin, (char *)NULL);
        _exit(127);
    }

    // parent: close child-side ends; keep in_pipe[1] (write to child stdin)
    // and out_pipe[0] (read child stdout) for fdopen.
    close(in_pipe[0]);
    close(out_pipe[1]);
    cli->in = fdopen(out_pipe[0], "r");
    cli->out = fdopen(in_pipe[1], "w");
    if (cli->in == NULL || cli->out == NULL) {
        if (cli->in != NULL) fclose(cli->in);
        if (cli->out != NULL) fclose(cli->out);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return -1;
    }
    cli->pid = pid;
    return 0;
}

static int cli_connect_raw(cli_t *cli) {
    cli->ctx = mcp_context_create(NULL);
    if (cli->ctx == NULL) return -1;
    cli->transport = mcp_stdio_transport_create(cli->ctx, cli->in, cli->out);
    if (cli->transport == NULL) return -1;
    cli->client = mcp_client_create(cli->ctx, cli->transport);
    if (cli->client == NULL) return -1;
    if (mcp_client_connect(cli->ctx, cli->client) != MCP_OK) return -1;
    return 0;
}

static int cli_init(cli_t *cli) {
    if (cli_connect_raw(cli) != 0) return -1;
    if (mcp_client_initialize(cli->ctx, cli->client, "mcpkit-cli", mcpkit_version_string(), NULL) != MCP_OK)
        return -1;
    return 0;
}

static void print_result(cli_t *cli, const mcp_json_value_t *v) {
    char *s = mcp_json_serialize(cli->ctx, v);
    if (s != NULL) {
        printf("%s\n", s);
        mcp_json_free_string(cli->ctx, s);
    }
}

static int cmd_discover(const char *server_bin) {
    cli_t cli = {NULL};
    if (spawn(server_bin, &cli) != 0) {
        cli_cleanup(&cli);
        return 1;
    }
    if (cli_connect_raw(&cli) != 0) {
        cli_cleanup(&cli);
        return 1;
    }
    mcp_json_value_t *result = NULL;
    if (mcp_client_discover(cli.ctx, cli.client, &result) != MCP_OK) {
        mcp_client_disconnect(cli.ctx, cli.client);
        cli_cleanup(&cli);
        return 1;
    }
    print_result(&cli, result);
    mcp_json_destroy(cli.ctx, result);
    mcp_client_disconnect(cli.ctx, cli.client);
    cli_cleanup(&cli);
    return 0;
}

static int cmd_inspect(const char *server_bin) {
    cli_t cli = {NULL};
    if (spawn(server_bin, &cli) != 0) {
        cli_cleanup(&cli);
        return 1;
    }
    if (cli_init(&cli) != 0) {
        cli_cleanup(&cli);
        return 1;
    }
    mcp_json_value_t *tools = NULL;
    if (mcp_client_list_tools(cli.ctx, cli.client, &tools) != MCP_OK) {
        cli_cleanup(&cli);
        return 1;
    }
    print_result(&cli, tools);
    mcp_json_destroy(cli.ctx, tools);
    mcp_json_value_t *resources = NULL;
    if (mcp_client_request(cli.ctx, cli.client, "resources/list", NULL, &resources) == MCP_OK) {
        print_result(&cli, resources);
        mcp_json_destroy(cli.ctx, resources);
    }
    mcp_client_disconnect(cli.ctx, cli.client);
    cli_cleanup(&cli);
    return 0;
}

static int cmd_call(const char *server_bin, const char *tool_name, const char *args_json) {
    cli_t cli = {NULL};
    if (spawn(server_bin, &cli) != 0 || cli_init(&cli) != 0) {
        cli_cleanup(&cli);
        return 1;
    }
    mcp_json_value_t *args = NULL;
    if (args_json != NULL) {
        args = mcp_json_parse(cli.ctx, args_json, strlen(args_json));
        if (args == NULL) {
            fprintf(stderr, "mcpkit-cli: invalid args JSON\n");
            mcp_client_disconnect(cli.ctx, cli.client);
            cli_cleanup(&cli);
            return 1;
        }
    }
    mcp_json_value_t *result = NULL;
    if (mcp_client_call_tool(cli.ctx, cli.client, tool_name, args, &result) != MCP_OK) {
        mcp_client_disconnect(cli.ctx, cli.client);
        cli_cleanup(&cli);
        return 1;
    }
    print_result(&cli, result);
    mcp_json_destroy(cli.ctx, result);
    mcp_client_disconnect(cli.ctx, cli.client);
    cli_cleanup(&cli);
    return 0;
}

static void on_signal(int sig) {
    (void)sig;
    mcp_request_shutdown();
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static mcp_json_value_t *build_filter(mcp_context_t *ctx, const char *filter_str) {
    if (filter_str == NULL || filter_str[0] == '\0' ||
        strcmp(filter_str, "all") == 0 || strcmp(filter_str, "*") == 0) {
        mcp_json_value_t *f = mcp_json_object_new(ctx);
        if (f == NULL) return NULL;
        (void)mcp_json_object_set_take(ctx, f, "toolsListChanged", mcp_json_bool_new(ctx, true));
        (void)mcp_json_object_set_take(ctx, f, "promptsListChanged", mcp_json_bool_new(ctx, true));
        (void)mcp_json_object_set_take(ctx, f, "resourcesListChanged", mcp_json_bool_new(ctx, true));
        return f;
    }
    if (filter_str[0] == '{') {
        return mcp_json_parse(ctx, filter_str, strlen(filter_str));
    }
    if (strcmp(filter_str, "toolsListChanged") == 0 || strcmp(filter_str, "tools") == 0) {
        mcp_json_value_t *f = mcp_json_object_new(ctx);
        if (f == NULL) return NULL;
        (void)mcp_json_object_set_take(ctx, f, "toolsListChanged", mcp_json_bool_new(ctx, true));
        return f;
    }
    if (strcmp(filter_str, "promptsListChanged") == 0 || strcmp(filter_str, "prompts") == 0) {
        mcp_json_value_t *f = mcp_json_object_new(ctx);
        if (f == NULL) return NULL;
        (void)mcp_json_object_set_take(ctx, f, "promptsListChanged", mcp_json_bool_new(ctx, true));
        return f;
    }
    if (strcmp(filter_str, "resourcesListChanged") == 0 || strcmp(filter_str, "resources") == 0) {
        mcp_json_value_t *f = mcp_json_object_new(ctx);
        if (f == NULL) return NULL;
        (void)mcp_json_object_set_take(ctx, f, "resourcesListChanged", mcp_json_bool_new(ctx, true));
        return f;
    }
    if (strstr(filter_str, "://") != NULL) {
        mcp_json_value_t *f = mcp_json_object_new(ctx);
        mcp_json_value_t *arr = mcp_json_array_new(ctx);
        mcp_json_value_t *val = mcp_json_string_new(ctx, filter_str);
        if (f == NULL || arr == NULL || val == NULL) {
            mcp_json_destroy(ctx, f);
            mcp_json_destroy(ctx, arr);
            mcp_json_destroy(ctx, val);
            return NULL;
        }
        if (mcp_json_array_append(ctx, arr, val) != MCP_OK) {
            mcp_json_destroy(ctx, val);
            mcp_json_destroy(ctx, arr);
            mcp_json_destroy(ctx, f);
            return NULL;
        }
        (void)mcp_json_object_set_take(ctx, f, "resourceSubscriptions", arr);
        return f;
    }
    mcp_json_value_t *f = mcp_json_object_new(ctx);
    if (f != NULL) {
        (void)mcp_json_object_set_take(ctx, f, filter_str, mcp_json_bool_new(ctx, true));
    }
    return f;
}

static int cmd_listen(const char *server_bin, const char *filter_str, int timeout_sec) {
    cli_t cli = {NULL};
    if (spawn(server_bin, &cli) != 0) {
        cli_cleanup(&cli);
        return 1;
    }
    if (cli_init(&cli) != 0) {
        cli_cleanup(&cli);
        return 1;
    }

    mcp_transport_set_timeout(cli.ctx, cli.transport, 200, 0);

    mcp_json_value_t *filter = build_filter(cli.ctx, filter_str);
    if (filter == NULL && filter_str != NULL && filter_str[0] == '{') {
        fprintf(stderr, "mcpkit-cli: invalid filter JSON\n");
        mcp_client_disconnect(cli.ctx, cli.client);
        cli_cleanup(&cli);
        return 1;
    }

    mcp_message_t *ack = NULL;
    if (mcp_client_subscriptions_listen(cli.ctx, cli.client, filter, &ack) != MCP_OK) {
        fprintf(stderr, "mcpkit-cli: subscriptions/listen failed\n");
        mcp_client_disconnect(cli.ctx, cli.client);
        cli_cleanup(&cli);
        return 1;
    }

    char sub_id_buf[64] = {0};
    const char *sub_id_str = NULL;
    if (ack != NULL) {
        const mcp_json_value_t *ack_p = mcp_message_params(cli.ctx, ack);
        if (ack_p != NULL) {
            const mcp_json_value_t *meta = mcp_json_object_get(cli.ctx, ack_p, "_meta");
            if (meta != NULL) {
                const mcp_json_value_t *sub_v =
                    mcp_json_object_get(cli.ctx, meta, "io.modelcontextprotocol/subscriptionId");
                if (sub_v != NULL) {
                    if (mcp_json_string_value(cli.ctx, sub_v, &sub_id_str) != MCP_OK) {
                        double d = 0;
                        if (mcp_json_number_value(cli.ctx, sub_v, &d) == MCP_OK) {
                            snprintf(sub_id_buf, sizeof(sub_id_buf), "%.0f", d);
                            sub_id_str = sub_id_buf;
                        }
                    }
                }
            }
        }
    }

    struct sigaction sa, old_sa_int, old_sa_term;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old_sa_int);
    sigaction(SIGTERM, &sa, &old_sa_term);

    uint64_t start_ms = now_ms();
    while (!mcp_shutdown_requested()) {
        if (timeout_sec > 0) {
            uint64_t elapsed_sec = (now_ms() - start_ms) / 1000u;
            if (elapsed_sec >= (uint64_t)timeout_sec) {
                break;
            }
        }
        mcp_message_t *msg = NULL;
        mcp_status_t st = mcp_client_recv_message(cli.ctx, cli.client, &msg);
        if (st == MCP_ERR_TIMEOUT) {
            continue;
        }
        if (st != MCP_OK) {
            break;
        }
        char *s = mcp_message_serialize(cli.ctx, msg);
        if (s != NULL) {
            printf("%s\n", s);
            fflush(stdout);
            mcp_json_free_string(cli.ctx, s);
        }
        mcp_message_destroy(cli.ctx, msg);
    }

    if (sub_id_str != NULL && sub_id_str[0] != '\0') {
        (void)mcp_client_cancel_subscription(cli.ctx, cli.client, sub_id_str);
    }

    sigaction(SIGINT, &old_sa_int, NULL);
    sigaction(SIGTERM, &old_sa_term, NULL);
    mcp_shutdown_clear();

    if (ack != NULL) {
        mcp_message_destroy(cli.ctx, ack);
    }
    mcp_client_disconnect(cli.ctx, cli.client);
    cli_cleanup(&cli);
    return 0;
}

static int cmd_validate(const char *file_path) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    mcp_idset_t *ids = mcp_idset_create(ctx);
    if (ctx == NULL || ids == NULL) {
        fprintf(stderr, "mcpkit-cli: init failed\n");
        if (ctx != NULL) mcp_context_destroy(ctx);
        return 1;
    }
    FILE *f = fopen(file_path, "r");
    if (f == NULL) {
        fprintf(stderr, "mcpkit-cli: cannot open %s\n", file_path);
        mcp_idset_destroy(ctx, ids);
        mcp_context_destroy(ctx);
        return 1;
    }
    int rc = 0;
    long line_no = 0;
    char line[8192];
    while (fgets(line, sizeof(line), f) != NULL) {
        line_no++;
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (len == 0) continue;
        mcp_message_t *msg = mcp_message_parse(ctx, line, len);
        if (msg == NULL) {
            printf("%ld: -32700\n", line_no);
            rc = 1;
            continue;
        }
        int code = 0;
        mcp_status_t st = mcp_message_validate(ctx, msg, &code);
        mcp_message_destroy(ctx, msg);
        if (st == MCP_OK) {
            printf("%ld: OK\n", line_no);
        } else {
            printf("%ld: %d\n", line_no, code);
            rc = 1;
        }
    }
    fclose(f);
    mcp_idset_destroy(ctx, ids);
    mcp_context_destroy(ctx);
    return rc;
}

static int cmd_test(const char *server_bin) {
    cli_t cli = {NULL};
    if (spawn(server_bin, &cli) != 0) {
        fprintf(stderr, "FAIL: spawn failed\n");
        cli_cleanup(&cli);
        return 1;
    }
    if (cli_init(&cli) != 0) {
        fprintf(stderr, "FAIL: initialize\n");
        cli_cleanup(&cli);
        return 1;
    }
    if (mcp_client_ping(cli.ctx, cli.client) != MCP_OK) {
        fprintf(stderr, "FAIL: ping\n");
        mcp_client_disconnect(cli.ctx, cli.client);
        cli_cleanup(&cli);
        return 1;
    }
    mcp_json_value_t *tools = NULL;
    if (mcp_client_list_tools(cli.ctx, cli.client, &tools) != MCP_OK) {
        fprintf(stderr, "FAIL: tools/list\n");
        mcp_client_disconnect(cli.ctx, cli.client);
        cli_cleanup(&cli);
        return 1;
    }
    const mcp_json_value_t *tools_arr = mcp_json_object_get(cli.ctx, tools, "tools");
    bool has_tools = tools_arr != NULL && mcp_json_array_size(cli.ctx, tools_arr) > 0;
    mcp_json_destroy(cli.ctx, tools);
    if (!has_tools) {
        fprintf(stderr, "FAIL: no tools\n");
        mcp_client_disconnect(cli.ctx, cli.client);
        cli_cleanup(&cli);
        return 1;
    }
    mcp_client_disconnect(cli.ctx, cli.client);
    cli_cleanup(&cli);
    return 0;
}

int main(int argc, char **argv) {
    /* A closed peer must surface as EPIPE/MCP_ERR_IO, not a SIGPIPE kill. */
    signal(SIGPIPE, SIG_IGN);
    if (argc < 2) {
        fprintf(stderr,
                "usage: mcpkit-cli discover <server-bin>\n"
                "       mcpkit-cli inspect <server-bin>\n"
                "       mcpkit-cli call <server-bin> <tool> [args-json]\n"
                "       mcpkit-cli listen <server-bin> [filter] [timeout_sec]\n"
                "       mcpkit-cli validate <file>\n"
                "       mcpkit-cli test <server-bin>\n");
        return 2;
    }
    if (argc < 3) {
        fprintf(stderr, "mcpkit-cli: missing argument\n");
        return 2;
    }
    if (strcmp(argv[1], "discover") == 0) {
        return cmd_discover(argv[2]);
    }
    if (strcmp(argv[1], "inspect") == 0) {
        return cmd_inspect(argv[2]);
    }
    if (strcmp(argv[1], "call") == 0) {
        if (argc < 4) {
            fprintf(stderr, "usage: mcpkit-cli call <server-bin> <tool> [args-json]\n");
            return 2;
        }
        const char *args_json = argc >= 5 ? argv[4] : NULL;
        return cmd_call(argv[2], argv[3], args_json);
    }
    if (strcmp(argv[1], "listen") == 0) {
        const char *filter = NULL;
        int timeout_sec = 0;
        if (argc >= 4) {
            char *endptr = NULL;
            long val = strtol(argv[3], &endptr, 10);
            if (*endptr == '\0' && val >= 0) {
                timeout_sec = (int)val;
            } else {
                filter = argv[3];
                if (argc >= 5) {
                    timeout_sec = atoi(argv[4]);
                }
            }
        }
        return cmd_listen(argv[2], filter, timeout_sec);
    }
    if (strcmp(argv[1], "validate") == 0) {
        return cmd_validate(argv[2]);
    }
    if (strcmp(argv[1], "test") == 0) {
        return cmd_test(argv[2]);
    }
    fprintf(stderr, "mcpkit-cli: unknown subcommand '%s'\n", argv[1]);
    return 2;
}
