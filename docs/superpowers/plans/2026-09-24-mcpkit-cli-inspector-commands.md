# mcpkit-cli Inspector Subcommands (discover & listen) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `discover` and `listen` subcommands in `mcpkit-cli` alongside `mcp_client_discover` in the C client SDK to align developer debugging tooling with the official MCP Inspector.

**Architecture:** Add `mcp_client_discover` to `client.h`/`client.c` as a high-level capability inspection wrapper. In `tools/mcpkit-cli/main.c`, add `cmd_discover` to probe server capabilities statelessly without an `initialize` handshake, and add `cmd_listen` to subscribe to server notification streams (`subscriptions/listen`), poll incoming notifications with a periodic transport timeout and signal handling (`SIGINT`/`SIGTERM`), format events to stdout, and gracefully cancel subscriptions (`notifications/cancelled`) on shutdown or timeout. Extend acceptance tests in `cli_accept.sh` and update documentation.

**Tech Stack:** C99 / POSIX (`poll`, `pipe`, `fork`, `sigaction`), CMake, CTest, ASan/UBSan.

---

### Task 1: Add `mcp_client_discover` to C Client Library & Unit Test

**Files:**
- Modify: `include/mcpkit/client/client.h`
- Modify: `src/client/client.c`
- Test: `tests/unit/test_client.c`

- [ ] **Step 1: Write failing unit test in `tests/unit/test_client.c`**

Add tests for `mcp_client_discover` checking argument validation and discovering capability response:
```c
    /* Test mcp_client_discover */
    CHECK(mcp_client_discover(ctx, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_client_discover(ctx, c2, NULL) == MCP_ERR_INVALID_ARGUMENT);
    mcp_json_value_t *disc = NULL;
    CHECK(mcp_client_discover(ctx, c2, &disc) == MCP_OK && disc != NULL);
    const char *disc_name = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, disc, "serverName"), &disc_name) == MCP_OK);
    CHECK(strcmp(disc_name, "s") == 0);
    mcp_json_destroy(ctx, disc);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target test_client && ./build/tests/test_client`
Expected: Compilation failure due to undeclared `mcp_client_discover`.

- [ ] **Step 3: Implement `mcp_client_discover` in `include/mcpkit/client/client.h` and `src/client/client.c`**

In `include/mcpkit/client/client.h`:
```c
/**
 * @brief Sends server/discover request to inspect server capabilities statelessly.
 *
 * Can be sent before or after initialize. Result contains serverName,
 * serverVersion, protocolVersion, capabilities, etc.
 *
 * @param ctx        Context; may be NULL.
 * @param client     Target client.
 * @param result_out Receives the caller-owned cloned result JSON object on MCP_OK.
 * @return MCP_OK on success; MCP_ERR_* on transport or protocol error.
 */
mcp_status_t mcp_client_discover(mcp_context_t *ctx, mcp_client_t *client,
                                 mcp_json_value_t **result_out);
```

In `src/client/client.c`:
```c
mcp_status_t mcp_client_discover(mcp_context_t *ctx, mcp_client_t *client,
                                 mcp_json_value_t **result_out) {
    if (client == NULL || result_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    return mcp_client_request(ctx, client, "server/discover", NULL, result_out);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target test_client && ./build/tests/test_client`
Expected: PASS with 0 errors.

- [ ] **Step 5: Commit**

```bash
git add include/mcpkit/client/client.h src/client/client.c tests/unit/test_client.c
git commit -m "feat(client): add mcp_client_discover API"
```

---

### Task 2: Implement `cmd_discover` and Update CLI Help in `tools/mcpkit-cli/main.c`

**Files:**
- Modify: `tools/mcpkit-cli/main.c`

- [ ] **Step 1: Implement `cmd_discover`**

In `tools/mcpkit-cli/main.c`, add `cmd_discover(const char *server_bin)`:
- Spawns child process via `spawn(server_bin, &cli)`.
- Allocates context, transport, and client.
- Connects client without `initialize` handshake.
- Calls `mcp_client_discover(cli.ctx, cli.client, &result)`.
- Prints serialized JSON result.
- Destroys result, disconnects, cleans up child process, returns 0 on success, 1 on failure.

- [ ] **Step 2: Update CLI Usage & Subcommand Dispatch**

Update `main(int argc, char **argv)`:
- Update usage message:
  `mcpkit-cli discover <server-bin>`
  `mcpkit-cli listen <server-bin> [filter] [timeout_sec]`
- Wire `argv[1]` equals `"discover"` to `cmd_discover(argv[2])`.

- [ ] **Step 3: Build and test manually against `examples/stdio-server`**

Run: `cmake --build build --target mcpkit-cli stdio-server`
Run: `./build/tools/mcpkit-cli discover ./build/examples/stdio-server`
Expected: Prints JSON containing `"serverName":"stdio-server"` and exits with 0.

- [ ] **Step 4: Commit**

```bash
git add tools/mcpkit-cli/main.c
git commit -m "feat(cli): add discover subcommand to mcpkit-cli"
```

---

### Task 3: Implement `cmd_listen` in `tools/mcpkit-cli/main.c`

**Files:**
- Modify: `tools/mcpkit-cli/main.c`

- [ ] **Step 1: Add time and signal helpers for `listen`**

Add monotonic timer helper `now_ms` and SIGINT/SIGTERM signal handler triggering `mcp_request_shutdown()`.

- [ ] **Step 2: Implement filter construction helper `build_filter`**

Parse filter string into `mcp_json_value_t *`:
- NULL or empty or `"all"` or `"*"`: `{"toolsListChanged": true, "promptsListChanged": true, "resourcesListChanged": true}`.
- Starts with `{`: `mcp_json_parse`.
- `"toolsListChanged"` or `"tools"`: `{"toolsListChanged": true}`.
- `"promptsListChanged"` or `"prompts"`: `{"promptsListChanged": true}`.
- `"resourcesListChanged"` or `"resources"`: `{"resourcesListChanged": true}`.
- Contains `"://"`: `{"resourceSubscriptions": [filter]}`.

- [ ] **Step 3: Implement `cmd_listen`**

In `tools/mcpkit-cli/main.c`:
- `spawn(server_bin, &cli)` and `cli_init(&cli)`.
- Set transport read timeout: `mcp_transport_set_timeout(cli.ctx, cli.transport, 200, 0)`.
- Send `mcp_client_subscriptions_listen(cli.ctx, cli.client, filter_val, &ack)`.
- Parse subscription ID from `ack` if present.
- Install signal handlers for `SIGINT` and `SIGTERM`.
- Run streaming loop: check `mcp_shutdown_requested()` and `timeout_sec`, call `mcp_client_recv_message`, print notifications.
- When exiting loop: if subscription ID present, call `mcp_client_cancel_subscription(cli.ctx, cli.client, sub_id)`.
- Restore signal handlers, call `mcp_shutdown_clear()`, cleanup and exit 0.

- [ ] **Step 4: Update argument parsing in `main()`**

In `main(int argc, char **argv)`:
- Handle `"listen"`:
  - If argument 3 is an integer, treat as `timeout_sec` with default filter.
  - If argument 3 is a string, treat as `filter`, and argument 4 as `timeout_sec` (if present).
  - Call `cmd_listen(server_bin, filter, timeout_sec)`.

- [ ] **Step 5: Test manually with `stdio-server` and timeout**

Run: `./build/tools/mcpkit-cli listen ./build/examples/stdio-server all 1`
Expected: Subscribes, waits 1 second, cleanly cancels and exits 0.

- [ ] **Step 6: Commit**

```bash
git add tools/mcpkit-cli/main.c
git commit -m "feat(cli): add listen subcommand to mcpkit-cli"
```

---

### Task 4: Add CLI Acceptance Tests & Complete Suite Verification

**Files:**
- Modify: `tests/acceptance/cli_accept.sh`

- [ ] **Step 1: Add `discover` and `listen` checks to `tests/acceptance/cli_accept.sh`**

Add checks:
```bash
# Test discover on stdio-server
out="$("$CLI" discover "$STDIO_SRV")"
case "$out" in *stdio-server*) ;; *) echo "FAIL: discover stdio-server missing stdio-server"; exit 1 ;; esac

# Test listen on stdio-server with 1s timeout
"$CLI" listen "$STDIO_SRV" all 1
"$CLI" listen "$STDIO_SRV" tools 1
```

- [ ] **Step 2: Run CTest test suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: 56/56 (100%) tests PASS.

- [ ] **Step 3: Run under ASan to verify zero leaks**

Run: `LD_PRELOAD=/usr/lib/libasan.so.8.0.0 ctest --test-dir build --output-on-failure`
Expected: PASS with 0 memory errors or leaks.

- [ ] **Step 4: Commit**

```bash
git add tests/acceptance/cli_accept.sh
git commit -m "test(cli): add acceptance tests for discover and listen subcommands"
```

---

### Task 5: Documentation & Changelog Update

**Files:**
- Modify: `docs/module-reference.md`
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Update `docs/module-reference.md`**

Document `mcp_client_discover` and the `mcpkit-cli` `discover` and `listen` commands with usage examples.

- [ ] **Step 2: Update `CHANGELOG.md`**

Add changelog entries for `discover` and `listen` subcommands and `mcp_client_discover`.

- [ ] **Step 3: Commit**

```bash
git add docs/module-reference.md CHANGELOG.md
git commit -m "docs: update module reference and changelog for cli discover and listen"
```
