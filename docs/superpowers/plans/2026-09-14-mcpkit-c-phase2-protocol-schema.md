# phase2-protocol-schema Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 JSON-RPC 2.0 全系消息（request/notification/response）+ initialize 握手 + Schema Builder 与校验器 + 三级验证管线，通过 §26 负面矩阵验收。

**Architecture:** Protocol 层只向内依赖 Core（JSON DOM + context + status），禁止触碰 transport/runtime/server。消息以 JSON DOM 为存储，`mcp_message_t` 为 opaque 包装；parse 宽松只验 JSON 合法性，三级管线（envelope→method→params）逐级收紧并产出精确 RPC 错误码；Schema Builder 直接产出标准 JSON Schema DOM，校验器递归比对。

**Tech Stack:** C23, CMake + CTest, assert 式单测，gcc/clang + ASan/UBSan 门禁（沿用 Phase 1 工具链）。

---

## Chunk 1: 头文件契约（锁死，不可自行加 API）

全局规则：所有函数首参 `mcp_context_t *ctx`（NULL=builtin，最简可用）；只返回 `mcp_status_t` 表意错误；borrowed 指针随父对象失效；builder 接管子对象所有权仅在成功时，失败时调用方仍需释放传入值。

### `include/mcpkit/protocol/message.h`

```c
#ifndef MCPKIT_PROTOCOL_MESSAGE_H
#define MCPKIT_PROTOCOL_MESSAGE_H

#include <stdbool.h>
#include <stddef.h>

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;

typedef enum {
    MCP_RPC_PARSE_ERROR = -32700,
    MCP_RPC_INVALID_REQUEST = -32600,
    MCP_RPC_METHOD_NOT_FOUND = -32601,
    MCP_RPC_INVALID_PARAMS = -32602,
    MCP_RPC_INTERNAL_ERROR = -32603,
} mcp_rpc_code_t;

typedef enum {
    MCP_MSG_INVALID = 0,
    MCP_MSG_REQUEST,
    MCP_MSG_NOTIFICATION,
    MCP_MSG_RESPONSE,
} mcp_msg_kind_t;

typedef enum {
    MCP_ID_NONE = 0,
    MCP_ID_STRING,
    MCP_ID_NUMBER,
} mcp_id_type_t;

#define MCP_PROTOCOL_MAX_MESSAGE_BYTES (4u * 1024u * 1024u)

typedef struct mcp_message mcp_message_t;

// len > MCP_PROTOCOL_MAX_MESSAGE_BYTES、JSON 非法、根非 object → NULL
mcp_message_t *mcp_message_parse(mcp_context_t *ctx, const char *text, size_t len);
void mcp_message_destroy(mcp_context_t *ctx, mcp_message_t *msg);

mcp_msg_kind_t mcp_message_kind(mcp_context_t *ctx, const mcp_message_t *msg);
const char *mcp_message_method(mcp_context_t *ctx, const mcp_message_t *msg);
const mcp_json_value_t *mcp_message_params(mcp_context_t *ctx, const mcp_message_t *msg);
mcp_id_type_t mcp_message_id_type(mcp_context_t *ctx, const mcp_message_t *msg);
const char *mcp_message_id_string(mcp_context_t *ctx, const mcp_message_t *msg);
mcp_status_t mcp_message_id_number(mcp_context_t *ctx, const mcp_message_t *msg, double *out);
const mcp_json_value_t *mcp_message_result(mcp_context_t *ctx, const mcp_message_t *msg);
mcp_status_t mcp_message_error_code(mcp_context_t *ctx, const mcp_message_t *msg, int *out);
const char *mcp_message_error_text(mcp_context_t *ctx, const mcp_message_t *msg);

// builders：params/result/data 传 NULL 表示省略该键；成功即接管所有权
mcp_message_t *mcp_request_new_string_id(mcp_context_t *ctx, const char *id,
                                         const char *method, mcp_json_value_t *params);
mcp_message_t *mcp_request_new_number_id(mcp_context_t *ctx, double id,
                                         const char *method, mcp_json_value_t *params);
mcp_message_t *mcp_notification_new(mcp_context_t *ctx, const char *method,
                                    mcp_json_value_t *params);
mcp_message_t *mcp_response_ok_new(mcp_context_t *ctx, const mcp_message_t *req,
                                   mcp_json_value_t *result);
mcp_message_t *mcp_response_err_new(mcp_context_t *ctx, const mcp_message_t *req_or_null,
                                    int code, const char *message, mcp_json_value_t *data);

char *mcp_message_serialize(mcp_context_t *ctx, const mcp_message_t *msg);

int mcp_status_to_rpc_code(mcp_status_t status);
mcp_status_t mcp_rpc_code_to_status(int code);

#endif
```

status→code 映射：INVALID_ARGUMENT→-32602，NOT_FOUND→-32601，PROTOCOL→-32600，NOMEM/TIMEOUT/IO→-32603，OK→0；code→status 反向对称（未知码→PROTOCOL）。

### `include/mcpkit/protocol/initialize.h`

```c
#ifndef MCPKIT_PROTOCOL_INITIALIZE_H
#define MCPKIT_PROTOCOL_INITIALIZE_H

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_message mcp_message_t;

#define MCP_PROTOCOL_VERSION_LATEST "2025-06-18"

mcp_json_value_t *mcp_initialize_params_new(mcp_context_t *ctx, const char *client_name,
                                            const char *client_version);
mcp_json_value_t *mcp_initialize_params_new_v(mcp_context_t *ctx, const char *protocol_version,
                                              const char *client_name, const char *client_version);
mcp_status_t mcp_initialize_params_validate(mcp_context_t *ctx, const mcp_json_value_t *params);
// 返回静态支持版本字符串（borrowed，static 生命周期），不支持则 NULL
const char *mcp_protocol_negotiate(mcp_context_t *ctx, const char *client_version);
mcp_json_value_t *mcp_initialize_result_new(mcp_context_t *ctx, const char *server_name,
                                            const char *server_version);
mcp_message_t *mcp_initialized_notification_new(mcp_context_t *ctx);

#endif
```

支持版本表：`"2025-06-18"`，`"2024-11-05"`。params 形如 `{"protocolVersion":"…","capabilities":{},"clientInfo":{"name":"…","version":"…"}}`；validate 要求三键齐且类型对。

### `include/mcpkit/json/schema.h`

```c
#ifndef MCPKIT_JSON_SCHEMA_H
#define MCPKIT_JSON_SCHEMA_H

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;

mcp_json_value_t *mcp_schema_object_new(mcp_context_t *ctx);
mcp_json_value_t *mcp_schema_string_new(mcp_context_t *ctx);
mcp_json_value_t *mcp_schema_integer_new(mcp_context_t *ctx);
mcp_json_value_t *mcp_schema_number_new(mcp_context_t *ctx);
mcp_json_value_t *mcp_schema_boolean_new(mcp_context_t *ctx);
mcp_json_value_t *mcp_schema_array_new(mcp_context_t *ctx, mcp_json_value_t *items);
mcp_status_t mcp_schema_add_property(mcp_context_t *ctx, mcp_json_value_t *schema,
                                     const char *name, mcp_json_value_t *prop);
mcp_status_t mcp_schema_add_required(mcp_context_t *ctx, mcp_json_value_t *schema,
                                     const char *name);
mcp_status_t mcp_schema_add_enum(mcp_context_t *ctx, mcp_json_value_t *schema,
                                 mcp_json_value_t *values);
mcp_status_t mcp_schema_set_minimum(mcp_context_t *ctx, mcp_json_value_t *schema, double min);
mcp_status_t mcp_schema_set_maximum(mcp_context_t *ctx, mcp_json_value_t *schema, double max);
mcp_status_t mcp_schema_set_description(mcp_context_t *ctx, mcp_json_value_t *schema,
                                        const char *desc);
mcp_status_t mcp_schema_validate(mcp_context_t *ctx, const mcp_json_value_t *schema,
                                 const mcp_json_value_t *instance);
mcp_status_t mcp_schema_validate_verbose(mcp_context_t *ctx, const mcp_json_value_t *schema,
                                         const mcp_json_value_t *instance,
                                         char *buf, size_t cap);

#endif
```

语义：schema 即标准 JSON Schema DOM（`{"type":"object","properties":{…},"required":[…]}`），用 `mcp_json_destroy` 释放。校验规则：type（含 integer=整数值 number）、required、properties 递归、items 递归、enum（string/number/bool 值比对）、minimum/maximum；description 忽略。verbose 首条失败写入 `"path: reason"`，buf 必以 NUL 结尾。

### `include/mcpkit/protocol/validate.h`

```c
#ifndef MCPKIT_PROTOCOL_VALIDATE_H
#define MCPKIT_PROTOCOL_VALIDATE_H

#include <stdbool.h>

#include "mcpkit/core/error.h"
#include "mcpkit/protocol/message.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_message mcp_message_t;

typedef struct mcp_idset mcp_idset_t;

mcp_idset_t *mcp_idset_create(mcp_context_t *ctx);
void mcp_idset_destroy(mcp_context_t *ctx, mcp_idset_t *set);
mcp_status_t mcp_idset_add(mcp_context_t *ctx, mcp_idset_t *set,
                           mcp_id_type_t type, const char *s, double n);
void mcp_idset_remove(mcp_context_t *ctx, mcp_idset_t *set,
                      mcp_id_type_t type, const char *s, double n);
bool mcp_idset_contains(mcp_context_t *ctx, const mcp_idset_t *set,
                        mcp_id_type_t type, const char *s, double n);

bool mcp_method_known(const char *method);
mcp_status_t mcp_validate_envelope(mcp_context_t *ctx, const mcp_message_t *msg,
                                   int *rpc_code_out);
mcp_status_t mcp_validate_method(mcp_context_t *ctx, const mcp_message_t *msg,
                                 int *rpc_code_out);
mcp_status_t mcp_validate_params(mcp_context_t *ctx, const mcp_message_t *msg,
                                 int *rpc_code_out);
mcp_status_t mcp_message_validate(mcp_context_t *ctx, const mcp_message_t *msg,
                                  int *rpc_code_out);

#endif
```

管线语义：L1 envelope（jsonrpc=="2.0"；request 要求 id(string|number)+method(string)；notification 要求 method 且无 id；response 要求 id + result/error 恰其一，error 须为 {code:int,message:string}）→ 失败 `rpc=-32600`；L2 method 查已知表 → 失败 `-32601`（response 跳过 L2/L3，直接 OK）；L3 params：出现即须为 object，且按方法强制键（initialize→protocolVersion 等全校验；tools/call→name:string；resources/read→uri:string；prompts/get→name:string；completion/complete→ref,argument）→ 失败 `-32602`。`rpc_code_out` 在 OK 时置 0，NULL 允许。

已知方法表：initialize, ping, tools/list, tools/call, resources/list, resources/templates/list, resources/read, resources/subscribe, resources/unsubscribe, prompts/list, prompts/get, completion/complete, logging/setLevel, notifications/initialized, notifications/cancelled, notifications/progress, notifications/tools/list_changed, notifications/resources/list_changed, notifications/resources/updated, notifications/prompts/list_changed, notifications/message, roots/list, roots/list_changed, sampling/createMessage, elicitation/create。

---

## Chunk 2: Tasks T1–T5

### Task T1: message 信封（头+实现+单测）

**Files:**
- Create: `include/mcpkit/protocol/message.h`, `src/protocol/message.c`, `tests/unit/test_rpc_envelope.c`
- Modify: `CMakeLists.txt`（lib 加 `src/protocol/message.c`）, `tests/CMakeLists.txt`（加项）, `include/mcpkit/mcpkit.h`（加 protocol 头）

- [ ] **Step 1: 写头文件**（照 Chunk 1 全文落盘）
- [ ] **Step 2: 写测试先行**

```c
// tests/unit/test_rpc_envelope.c（assert 式 main，多断言）
#include <assert.h>
#include <string.h>
#include "mcpkit/protocol/message.h"
// 1. string-id request builder → serialize 含 "id":"r1" 且 "jsonrpc":"2.0" → parse 回来 kind==REQUEST, method, id 一致
// 2. number-id request：id_number 取回 7
// 3. notification 无 id：id_type==NONE，parse 后 kind==NOTIFICATION
// 4. response_ok_new(req, result) 拷贝 id；result 取回
// 5. response_err_new(req, -32601, "…") → error_code==-32601, error_text 一致
// 6. response_err_new(NULL, -32700, …) → id_type==NONE（parse 错误场景）
// 7. builder 传入 params NULL → 序列化无 "params" 键
// 8. status↔code 映射往返：INVALID_ARGUMENT↔-32602, NOT_FOUND↔-32601, PROTOCOL↔-32600
```

- [ ] **Step 3: 运行验证失败** `cmake -S . -B build && cmake --build build -j` 预期链接失败（message.c 缺失）
- [ ] **Step 4: 写 `src/protocol/message.c`**（内部 `struct mcp_message { mcp_json_value_t *dom; }`；parse 经 `mcp_json_parse`；分类看 method/id/result/error 键；builder 拼 DOM；accessor 经 DOM 只读；serialize 经 `mcp_json_serialize`）
- [ ] **Step 5: 全绿** `ctest --test-dir build` 预期 9/9（8 旧 + 1 新）
- [ ] **Step 6: Commit** `git add … && git commit -m "feat(protocol): add JSON-RPC message envelope"`

### Task T2: initialize 握手

**Files:** Create `include/mcpkit/protocol/initialize.h`, `src/protocol/initialize.c`, `tests/unit/test_initialize.c`; Modify CMake/tests/mcpkit.h 同 T1 模式

- [ ] **Step 1–2: 头 + 测试先行**（params_new→validate OK；缺 protocolVersion→INVALID_ARGUMENT；negotiate("2025-06-18") 回同串，negotiate("1999-01-01")→NULL；result_new 含 serverInfo；initialized_notification method 为 "notifications/initialized" 且无 id）
- [ ] **Step 3: 实现**（经 JSON DOM 拼/查；negotiate 查两元素静态表）
- [ ] **Step 4: 全绿** 预期 10/10
- [ ] **Step 5: Commit** `git commit -m "feat(protocol): add initialize handshake and version negotiation"`

### Task T3: Schema Builder + 校验器

**Files:** Create `include/mcpkit/json/schema.h`, `src/json/schema.c`, `tests/unit/test_schema.c`; Modify 同上

注：校验 `properties` 需枚举对象键，Phase 1 `object.h` 无此能力，故本任务附带最小扩展 `mcp_json_object_key_at`（object.h 声明 + backend ops 表 + api 分发 + builtin 实现；wrapper 测试用 memcpy 复制 ops 表，不受影响）。

- [ ] **Step 1–2: 头 + 测试先行**（建 tools/call 形 schema：object{properties:{name:string, priority?:integer, tags?:array<string>, mode?:enum}, required:[name]}；好实例 OK；缺 name/错 type/enum 外值/items 错类型/min-max 越界 → INVALID_ARGUMENT；verbose buf 非空且 NUL 结尾）
- [ ] **Step 3: 实现**（递归 validate_frame 带 path；integer 判 `floor(d)==d && isfinite`；enum 用类型+值比对）
- [ ] **Step 4: 全绿** 预期 11/11 + ASan 干净（递归+深 DOM 易漏 free，asan 必跑）
- [ ] **Step 5: Commit** `git commit -m "feat(json): add schema builder and validator"`

### Task T4: 三级验证管线 + §26 负面矩阵

**Files:** Create `include/mcpkit/protocol/validate.h`, `src/protocol/validate.c`, `tests/unit/test_validate_negative.c`; Modify 同上

- [ ] **Step 1–2: 头 + 测试先行**（7 项各断言：1 malformed `{"jsonrpc":` →parse NULL；2 缺 jsonrpc/错版本/根 array/request 缺 method/response 双无 →validate 得 -32600；3 未知 method →-32601；4 tools/call 缺 name/params 为 array →-32602；5 len=cap+1 →parse NULL；6 非法 UTF-8 字节 →parse NULL；7 同 id 加两次 →ALREADY_EXISTS，remove 后可再加；另 response 跳过 L2/L3 得 OK）
- [ ] **Step 3: 实现**（idset 为动态 tagged 数组；L3 各方法 params 检查函数 static；response 在 validate_method/params 直接返回 OK）
- [ ] **Step 4: 全绿** 预期 12/12
- [ ] **Step 5: Commit** `git commit -m "feat(protocol): add three-level validation pipeline"`

### Task T5: 错误映射证明 + 全门禁 + 清理

**Files:** Create `tests/unit/test_rpc_errors.c`; Modify `CHANGELOG.md`, tests/CMakeLists.txt

- [ ] **Step 1: 测试先行**（映射表全覆盖：11 码→code、5 码→status、未知码→PROTOCOL；端到端：validate 得 -32601 的消息 →response_err_new→serialize→parse→error_code 一致）
- [ ] **Step 2: 实现**（若 message.c 映射已在 T1 实现，此步仅测试；缺口则补）
- [ ] **Step 3: CHANGELOG 加 Phase 2 行**
- [ ] **Step 4: 三门禁全绿**：gcc `ctest` 13/13；clang 重配构建 exit 0 + ctest 13/13；ASan+UBSan 13/13；`builtin.c schema.c` 级别单文件 `-Wall -Wextra -Werror` 抽查
- [ ] **Step 5: 清理** `rm -rf build build-clang build-asan`，`git status` 干净
- [ ] **Step 6: Commit** `git commit -m "test(protocol): prove error mapping and close phase 2"`
