# P0-2 TLS Transport Adapter Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `mcp_tls_transport_create` — an OpenSSL-backed TLS transport adapter behind `MCPKIT_BUILD_TLS` (default OFF) that keeps the zero-dep default build intact.

**Architecture:** `src/transport/tls.c` mirrors the structure of `socket.c`: `tls_backend_t{fd, ssl, sctx, server_mode}` implements the 4-op vtable (start is a no-op since handshake happens at create; send/recv use SSL_write/SSL_read with the same 4 KB chunk + 4 MB cap + deadline logic; stop does SSL_shutdown + close + free). The adapter links OpenSSL PRIVATE to `mcpkit_core` only when `MCPKIT_BUILD_TLS=ON`.

**Tech Stack:** C23, OpenSSL 3.x, POSIX sockets, mcpkit_core static lib.

---

## File Structure

| File | Responsibility |
|---|---|
| `cmake/MCPKitOptions.cmake` | Add `MCPKIT_BUILD_TLS` option (default OFF) |
| `CMakeLists.txt` | TLS gate: find OpenSSL, append `tls.c`, link libs |
| `include/mcpkit/transport/tls.h` | `mcp_tls_transport_create` decl + Doxygen |
| `src/transport/tls.c` | ~220 lines: TLS backend + 4-op vtable + create |
| `tests/unit/test_tls_transport.c` | Self-signed cert roundtrip (SOCKET+TLS gated) |
| `tests/CMakeLists.txt` | Register `test_tls_transport` under TLS+SOCKET |
| `docs/module-reference.md` | Add `tls.h` section |
| `CHANGELOG.md` | Add bullet to [Unreleased] Added |

---

## Task 1: CMake option + TLS backend implementation

**Files:**
- Modify: `cmake/MCPKitOptions.cmake`
- Modify: `CMakeLists.txt`
- Create: `include/mcpkit/transport/tls.h`
- Create: `src/transport/tls.c`

- [ ] **Step 1: Add MCPKIT_BUILD_TLS option**

In `cmake/MCPKitOptions.cmake`, add after the existing options:
```cmake
option(MCPKIT_BUILD_TLS "Build the OpenSSL TLS transport adapter" OFF)
```

- [ ] **Step 2: Add TLS gate to CMakeLists.txt**

After the `if(MCPKIT_BUILD_SOCKET)` block:
```cmake
if(MCPKIT_BUILD_TLS)
  list(APPEND MCPKIT_CORE_SOURCES src/transport/tls.c)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(OPENSSL REQUIRED openssl)
  target_link_libraries(mcpkit_core PRIVATE ${OPENSSL_LIBRARIES})
  target_include_directories(mcpkit_core PRIVATE ${OPENSSL_INCLUDE_DIRS})
endif()
```

Note: `mcpkit.h` umbrella does NOT include `tls.h` (conditional include in a public umbrella is problematic; host includes `mcpkit/transport/tls.h` directly when TLS is enabled).

- [ ] **Step 3: Write tls.h**

```c
/**
 * @file tls.h
 * @brief OpenSSL-backed TLS line transport.
 * @ingroup mcpkit-transport
 *
 * Wraps a TCP socket in TLS. The handshake completes inside
 * mcp_tls_transport_create; mcp_transport_start is a no-op.
 * Line framing, 4 MB cap, and deadline semantics match socket.c.
 *
 * Server: supply cert + key file paths; port 0 = ephemeral.
 * Client: host = "127.0.0.1" or IP; cert/key = NULL (no verification
 * by default — suitable for loopback tests, NOT for production).
 */

#ifndef MCPKIT_TRANSPORT_TLS_H
#define MCPKIT_TRANSPORT_TLS_H

#include <stdint.h>
#include <stdbool.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_transport mcp_transport_t;

mcp_transport_t *mcp_tls_transport_create(mcp_context_t *ctx,
                                         const char *host_or_null,
                                         uint16_t port,
                                         bool server_mode,
                                         const char *cert_or_null,
                                         const char *key_or_null);

#endif
```

- [ ] **Step 4: Write tls.c**

Structure (mirrors socket.c):
```c
#include <errno.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "mcpkit/transport/tls.h"
// ... existing mcpkit includes ...

typedef struct {
    int fd;
    SSL *ssl;
    SSL_CTX *sctx;
    bool server_mode;
} tls_backend_t;

// alloc_of, now_ms, wait_until copied from socket.c pattern
// (or factored into a shared internals helper if warranted)

static mcp_status_t tls_start(mcp_context_t *ctx, mcp_transport_t *t); // no-op, returns OK
static mcp_status_t tls_send(mcp_context_t *ctx, mcp_transport_t *t, const char *data, size_t len);
static mcp_status_t tls_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out);
static mcp_status_t tls_stop(mcp_context_t *ctx, mcp_transport_t *t);

static const mcp_transport_ops_t kTlsOps = { tls_start, tls_send, tls_recv, tls_stop };

// open_tls_socket: same as open_socket but without the handshake
// mcp_tls_transport_create: open_socket + SSL_CTX_new + SSL_new + handshake
```

Key implementation notes:
- `SSL_CTX_new(TLS_method())` for both client and server
- Server: `SSL_CTX_use_certificate_file` + `SSL_CTX_use_PrivateKey_file`
- `BIO_new_fd(fd)` + `SSL_set_bio`
- `SSL_set_connect_state(ssl)` for client, `SSL_set_accept_state(ssl)` for server
- `SSL_do_handshake` loop: retry on `SSL_ERROR_WANT_READ`/`SSL_ERROR_WANT_WRITE` with poll
- `tls_send`: `SSL_write` with `\n` appended, retry on `SSL_ERROR_WANT_WRITE`, same 4 MB cap as socket.c
- `tls_recv`: `SSL_read` into 4 KB buffer, accumulate line buffer, 4 MB cap, same semantics as socket.c
- `tls_stop`: `SSL_shutdown` (best-effort, ignore errors), `SSL_free`, `SSL_CTX_free`, `close(fd)`, free backend, null via `mcp_transport_set_backend`

- [ ] **Step 5: Build with TLS=ON + verify no regression with TLS=OFF**

```bash
# TLS=OFF default build (should stay 46/46)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON
cmake --build build -j4 && ctest --test-dir build   # expect 46/46

# TLS=ON build (adds test_tls_transport = 47th test once registered; but Task 1 has no test yet)
cmake -S . -B build-tls -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON -DMCPKIT_BUILD_TLS=ON
cmake --build build-tls -j4   # expect clean compile, no new tests yet
rm -rf build build-tls
```

Commit: `git add cmake/MCPKitOptions.cmake CMakeLists.txt include/mcpkit/transport/tls.h src/transport/tls.c && git commit -m "feat(transport): add OpenSSL TLS transport adapter behind MCPKIT_BUILD_TLS"`

---

## Task 2: test_tls_transport.c

**Files:**
- Create: `tests/unit/test_tls_transport.c`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the test**

The test needs a self-signed cert. Generate it at CMake configure time via a custom command, OR generate it at test runtime via `system()`. **Decision: CMake-time** (deterministic, no system calls in test).

Add to `tests/CMakeLists.txt` under `MCPKIT_BUILD_TLS AND MCPKIT_BUILD_SOCKET`:
```cmake
# Generate self-signed cert for TLS test
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/tls_test.crt ${CMAKE_CURRENT_BINARY_DIR}/tls_test.key
  COMMAND openssl req -x509 -newkey rsa:2048 -nodes
          -keyout ${CMAKE_CURRENT_BINARY_DIR}/tls_test.key
          -out ${CMAKE_CURRENT_BINARY_DIR}/tls_test.crt
          -days 1 -subj "/CN=localhost" 2>/dev/null
  COMMENT "Generating self-signed cert for TLS test"
)
add_executable(test_tls_transport unit/test_tls_transport.c)
target_link_libraries(test_tls_transport PRIVATE mcpkit_core)
add_dependencies(test_tls_transport tls_test_cert)
add_test(NAME test_tls_transport COMMAND test_tls_transport
         ${CMAKE_CURRENT_BINARY_DIR}/tls_test.crt ${CMAKE_CURRENT_BINARY_DIR}/tls_test.key)
```

Wait — the cert generation needs to be a target the test depends on. Use a custom target:
```cmake
add_custom_target(tls_test_cert ALL
  COMMAND openssl req -x509 -newkey rsa:2048 -nodes
          -keyout ${CMAKE_CURRENT_BINARY_DIR}/tls_test.key
          -out ${CMAKE_CURRENT_BINARY_DIR}/tls_test.crt
          -days 1 -subj "/CN=localhost" 2>/dev/null
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
```

Then `add_dependencies(test_tls_transport tls_test_cert)`.

The test passes cert/key paths as argv[1]/argv[2].

Test structure (port 45700, fixed):
1. Server: `mcp_tls_transport_create(ctx, NULL, 45700, true, cert, key)` → non-NULL
2. Client: `mcp_tls_transport_create(ctx, "127.0.0.1", 45700, false, NULL, NULL)` → non-NULL
3. Send a JSON-RPC `initialize` line from client → `mcp_transport_send`
4. Recv on server → `mcp_transport_recv` → parse → verify it's the init request
5. Send a response line from server → `mcp_transport_send`
6. Recv on client → `mcp_transport_recv` → parse → verify it's the init response
7. Stop + destroy both transports

- [ ] **Step 2: Register in tests/CMakeLists.txt** (see above)

- [ ] **Step 3: Build + verify TLS=ON 47/47 + TLS=OFF 46/46 + commit**

```bash
cmake -S . -B build-tls -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON -DMCPKIT_BUILD_TLS=ON
cmake --build build-tls -j4 && ctest --test-dir build-tls   # expect 47/47
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON
cmake --build build -j4 && ctest --test-dir build           # expect 46/46 (TLS test not built)
rm -rf build build-tls
```

Commit: `git add tests/unit/test_tls_transport.c tests/CMakeLists.txt && git commit -m "test(tls): prove TLS transport adapter loopback roundtrip"`

---

## Task 3: Docs + CHANGELOG + triple gates + cleanup

**Files:**
- Modify: `docs/module-reference.md`
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Update module-reference.md**

Add a `### tls.h` section after `socket.h`:
```
### `tls.h`
(OpenSSL; built only when `MCPKIT_BUILD_TLS=ON`)
```c
mcp_transport_t *mcp_tls_transport_create(ctx, host_or_null, uint16_t port,
                                          bool server_mode,
                                          const char *cert_or_null,
                                          const char *key_or_null);
```
Server: `port` = listen port, `cert`/`key` = PEM file paths.
Client: `host_or_null` = "127.0.0.1" or IP, `port` = server port,
`cert`/`key` = NULL (no cert verification by default).
Handshake completes inside create; `mcp_transport_start` is a no-op.
Line framing, 4 MB cap, deadline semantics match `socket.h`.

- [ ] **Step 2: Update CHANGELOG.md**

Add bullet to [Unreleased] > Added:
```
- `mcp_tls_transport_create`: OpenSSL-backed TLS transport adapter
  (behind `MCPKIT_BUILD_TLS`, default OFF). The default zero-dep build
  is unchanged; when enabled, links OpenSSL PRIVATE to `mcpkit_core`.
```

- [ ] **Step 3: Doxygen verify**

`doxygen Doxyfile 2>&1 | grep -v 'Parsing\|Preprocessing\|Generating' | grep -iE 'warning|error'`
Expected: 0 real warnings.

- [ ] **Step 4: Triple gates (TLS=OFF default)**

```
rm -rf build build-clang build-asan
cmake -S . -B build      -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON && cmake --build build -j4 && ctest --test-dir build
cmake -S . -B build-clang -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON && cmake --build build-clang -j4 && ctest --test-dir build-clang
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_ENABLE_ASAN=ON -DMCPKIT_ENABLE_UBSAN=ON -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON && cmake --build build-asan -j4 && ctest --test-dir build-asan
ldd build-asan/tests/test_socket_serve | grep -E 'asan|ubsan'
rm -rf build build-clang build-asan
```
Expected: 46/46 all three; libasan.so.8 + libubsan.so.1 confirmed.

Also run one TLS=ON gcc gate to confirm 47/47:
```
cmake -S . -B build-tls -DCMAKE_BUILD_TYPE=Debug -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON -DMCPKIT_BUILD_TLS=ON && cmake --build build-tls -j4 && ctest --test-dir build-tls
rm -rf build-tls
```

- [ ] **Step 5: Commit docs**

`git add docs/module-reference.md CHANGELOG.md && git commit -m "docs: cover mcp_tls_transport_create in CHANGELOG and module reference"`

---

## Comment-justifications (P3)

| Comment | Justification |
|---|---|
| tls.c `SSL_set_connect_state` / `SSL_set_accept_state` before handshake | Documents why the state is set explicitly (BIO_fd doesn't set it automatically) — non-obvious OpenSSL API detail |
| tls.c `SSL_shutdown` best-effort comment | Explains why the return value is ignored (peer may already have closed; forced close is acceptable for a line transport) |
| tls.h @file header no-cert-verification note | Documents the security limitation (insecure for production; suitable for loopback tests only) |
| CMakeLists.txt TLS gate comment | Documents why OpenSSL is linked PRIVATE and only under the flag (zero-dep default build invariant) |
