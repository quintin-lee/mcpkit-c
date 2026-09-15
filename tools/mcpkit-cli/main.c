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

#include "mcpkit/mcpkit.h"

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

static int cli_init(cli_t *cli) {
    cli->ctx = mcp_context_create(NULL);
    if (cli->ctx == NULL) return -1;
    cli->transport = mcp_stdio_transport_create(cli->ctx, cli->in, cli->out);
    if (cli->transport == NULL) return -1;
    cli->client = mcp_client_create(cli->ctx, cli->transport);
    if (cli->client == NULL) return -1;
    if (mcp_client_connect(cli->ctx, cli->client) != MCP_OK) return -1;
    if (mcp_client_initialize(cli->ctx, cli->client, "mcpkit-cli", "0.1.0", NULL) != MCP_OK)
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

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: mcpkit-cli inspect <server-bin>\n"
                "       mcpkit-cli call <server-bin> <tool> [args-json]\n"
                "       mcpkit-cli validate <file>\n"
                "       mcpkit-cli test <server-bin>\n");
        return 2;
    }
    if (argc < 3) {
        fprintf(stderr, "mcpkit-cli: missing argument\n");
        return 2;
    }
    if (strcmp(argv[1], "inspect") == 0) {
        return cmd_inspect(argv[2]);
    }
    fprintf(stderr, "mcpkit-cli: subcommand '%s' not implemented in T1\n", argv[1]);
    return 2;
}
