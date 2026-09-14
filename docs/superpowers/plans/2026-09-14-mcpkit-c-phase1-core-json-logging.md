# Phase 1: Core (allocator/context) + Logging + JSON Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 建成 Core 地基（可插拔 allocator、context、capability）+ Logging + 零依赖 JSON（DOM/解析/序列化/可插拔 backend），8 个单测全绿。

**Architecture:** 所有 JSON 公共函数首参均为 `mcp_context_t *ctx`（NULL=defaults），经 ctx 的 backend 路由；NULL backend = 内置后端。值类型对用户 opaque，定义只出现在 `src/json/builtin.c`。容器成功时接管子值所有权，失败时调用方保留（§6 推广）。`double` 非有限值在构造期拒绝。logger 与 context 之间无循环依赖（logger 只依赖 allocator）。

**Tech Stack:** C23，CMake/CTest，断言式单测，`-Wall -Wextra -Werror` 双编译器，ASan+UBSan 冒烟。

---

## Chunk 1: API 契约（头文件全文，照抄落盘）

### `include/mcpkit/core/types.h` —— 追加 allocator（文件其余部分保持 Phase 0 原样）

```c
typedef struct mcp_allocator {
    void *(*malloc_fn)(size_t size, void *userdata);
    void (*free_fn)(void *ptr, void *userdata);
    void *(*calloc_fn)(size_t nmemb, size_t size, void *userdata);
    void *(*realloc_fn)(void *ptr, size_t size, void *userdata);
    void *userdata;
} mcp_allocator_t;

const mcp_allocator_t *mcp_default_allocator(void);
```

### `include/mcpkit/core/capability.h`（新建，纯数据 struct，不 opaque）

```c
#ifndef MCPKIT_CORE_CAPABILITY_H
#define MCPKIT_CORE_CAPABILITY_H

typedef struct {
    bool tools;
    bool resources;
    bool prompts;
} mcp_capabilities_t;

#define MCP_CAPABILITIES_INIT {false, false, false}

#endif
```

### `include/mcpkit/logging/log.h`（新建）

```c
#ifndef MCPKIT_LOGGING_LOG_H
#define MCPKIT_LOGGING_LOG_H

typedef enum {
    MCP_LOG_DEBUG = 0,
    MCP_LOG_INFO,
    MCP_LOG_WARN,
    MCP_LOG_ERROR,
} mcp_log_level_t;

const char *mcp_log_level_string(mcp_log_level_t level);

#endif
```

### `include/mcpkit/logging/logger.h`（新建）

```c
#ifndef MCPKIT_LOGGING_LOGGER_H
#define MCPKIT_LOGGING_LOGGER_H

#include <stddef.h>

#include "mcpkit/core/types.h"

typedef struct mcp_logger mcp_logger_t;
typedef void (*mcp_log_sink_fn)(mcp_log_level_t level, const char *message, void *userdata);

// alloc NULL -> libc. sink NULL -> stderr sink.
mcp_logger_t *mcp_logger_create(const mcp_allocator_t *alloc, mcp_log_sink_fn sink, void *userdata);
void mcp_logger_destroy(mcp_logger_t *logger); // NULL-safe
void mcp_logger_set_level(mcp_logger_t *logger, mcp_log_level_t level); // NULL-safe no-op
mcp_log_level_t mcp_logger_get_level(mcp_logger_t *logger); // NULL -> MCP_LOG_ERROR
// NULL logger/message -> no-op. Drops records below level.
void mcp_logger_log(mcp_logger_t *logger, mcp_log_level_t level, const char *message);
void mcp_log_sink_stderr(mcp_log_level_t level, const char *message, void *userdata);
mcp_logger_t *mcp_logger_default_stderr(const mcp_allocator_t *alloc);

#endif
```

### `include/mcpkit/core/context.h`（新建）

```c
#ifndef MCPKIT_CORE_CONTEXT_H
#define MCPKIT_CORE_CONTEXT_H

#include "mcpkit/core/types.h"
#include "mcpkit/logging/logger.h"

typedef struct mcp_json_backend_ops mcp_json_backend_ops_t; // defined in mcpkit/json/json.h

typedef struct {
    const mcp_allocator_t *allocator; // NULL -> libc defaults
    mcp_logger_t *logger; // NULL -> context creates+owns stderr logger; non-NULL -> borrowed
    const mcp_json_backend_ops_t *json_backend; // NULL -> builtin; borrowed
} mcp_context_config_t;

mcp_context_t *mcp_context_create(const mcp_context_config_t *config); // NULL config -> all defaults; NULL return = OOM only
void mcp_context_destroy(mcp_context_t *ctx); // NULL-safe
const mcp_allocator_t *mcp_context_allocator(mcp_context_t *ctx); // NULL ctx -> defaults
mcp_logger_t *mcp_context_logger(mcp_context_t *ctx); // borrowed; NULL ctx -> NULL
const mcp_json_backend_ops_t *mcp_context_json_backend(mcp_context_t *ctx); // NULL ctx -> NULL (=builtin)

#endif
```

### `include/mcpkit/json/value.h`（新建）

```c
#ifndef MCPKIT_JSON_VALUE_H
#define MCPKIT_JSON_VALUE_H

#include <stdbool.h>
#include <stddef.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;

typedef enum {
    MCP_JSON_NULL = 0,
    MCP_JSON_BOOL,
    MCP_JSON_NUMBER,
    MCP_JSON_STRING,
    MCP_JSON_ARRAY,
    MCP_JSON_OBJECT,
} mcp_json_type_t;

typedef struct mcp_json_value mcp_json_value_t;

// NULL value -> MCP_JSON_NULL.
mcp_json_type_t mcp_json_type(mcp_context_t *ctx, const mcp_json_value_t *v);
void mcp_json_destroy(mcp_context_t *ctx, mcp_json_value_t *v); // NULL-safe, frees subtree
mcp_status_t mcp_json_bool_value(mcp_context_t *ctx, const mcp_json_value_t *v, bool *out);
mcp_status_t mcp_json_number_value(mcp_context_t *ctx, const mcp_json_value_t *v, double *out);
mcp_status_t mcp_json_string_value(mcp_context_t *ctx, const mcp_json_value_t *v, const char **out); // borrowed

// Constructors: caller owns. NULL return = invalid argument or OOM.
mcp_json_value_t *mcp_json_null_new(mcp_context_t *ctx);
mcp_json_value_t *mcp_json_bool_new(mcp_context_t *ctx, bool b);
mcp_json_value_t *mcp_json_number_new(mcp_context_t *ctx, double d); // NaN/Inf -> NULL
mcp_json_value_t *mcp_json_string_new(mcp_context_t *ctx, const char *s); // copies; non-UTF8 -> NULL
mcp_json_value_t *mcp_json_string_new_n(mcp_context_t *ctx, const char *s, size_t n);
mcp_json_value_t *mcp_json_array_new(mcp_context_t *ctx);
mcp_json_value_t *mcp_json_object_new(mcp_context_t *ctx);

#endif
```

### `include/mcpkit/json/object.h` / `array.h`（新建）

```c
// object.h
mcp_status_t mcp_json_object_set(mcp_context_t *ctx, mcp_json_value_t *obj, const char *key, mcp_json_value_t *val);
// OK -> container owns val (duplicate key replaces, old freed). ERR -> caller retains val.
const mcp_json_value_t *mcp_json_object_get(mcp_context_t *ctx, const mcp_json_value_t *obj, const char *key); // borrowed, NULL if missing
bool mcp_json_object_has(mcp_context_t *ctx, const mcp_json_value_t *obj, const char *key);
size_t mcp_json_object_size(mcp_context_t *ctx, const mcp_json_value_t *obj);

// array.h
mcp_status_t mcp_json_array_append(mcp_context_t *ctx, mcp_json_value_t *arr, mcp_json_value_t *val); // OK -> container owns
const mcp_json_value_t *mcp_json_array_get(mcp_context_t *ctx, const mcp_json_value_t *arr, size_t i); // borrowed
size_t mcp_json_array_size(mcp_context_t *ctx, const mcp_json_value_t *arr);
```

类型/NULL 入参一律返回 `MCP_ERR_INVALID_ARGUMENT`（查询类返回 NULL/0/false），`key` 须为有效 UTF-8。

### `include/mcpkit/json/json.h`（新建，umbrella + backend + 编解码）

```c
#ifndef MCPKIT_JSON_JSON_H
#define MCPKIT_JSON_JSON_H

#include "mcpkit/json/value.h"
#include "mcpkit/json/object.h"
#include "mcpkit/json/array.h"

#define MCP_JSON_MAX_DEPTH 128

struct mcp_json_backend_ops {
    const char *name;
    mcp_json_value_t *(*parse)(mcp_context_t *ctx, const char *text, size_t len);
    char *(*serialize)(mcp_context_t *ctx, const mcp_json_value_t *value);
    void (*free_string)(mcp_context_t *ctx, char *s);
    void (*destroy)(mcp_context_t *ctx, mcp_json_value_t *value);
    mcp_json_type_t (*type_of)(mcp_context_t *ctx, const mcp_json_value_t *value);
    mcp_status_t (*get_bool)(mcp_context_t *ctx, const mcp_json_value_t *v, bool *out);
    mcp_status_t (*get_number)(mcp_context_t *ctx, const mcp_json_value_t *v, double *out);
    mcp_status_t (*get_string)(mcp_context_t *ctx, const mcp_json_value_t *v, const char **out);
    mcp_json_value_t *(*new_null)(mcp_context_t *ctx);
    mcp_json_value_t *(*new_bool)(mcp_context_t *ctx, bool b);
    mcp_json_value_t *(*new_number)(mcp_context_t *ctx, double d);
    mcp_json_value_t *(*new_string_n)(mcp_context_t *ctx, const char *s, size_t n);
    mcp_json_value_t *(*new_array)(mcp_context_t *ctx);
    mcp_json_value_t *(*new_object)(mcp_context_t *ctx);
    mcp_status_t (*object_set)(mcp_context_t *ctx, mcp_json_value_t *obj, const char *key, mcp_json_value_t *val);
    const mcp_json_value_t *(*object_get)(mcp_context_t *ctx, const mcp_json_value_t *obj, const char *key);
    bool (*object_has)(mcp_context_t *ctx, const mcp_json_value_t *obj, const char *key);
    size_t (*object_size)(mcp_context_t *ctx, const mcp_json_value_t *obj);
    mcp_status_t (*array_append)(mcp_context_t *ctx, mcp_json_value_t *arr, mcp_json_value_t *val);
    const mcp_json_value_t *(*array_get)(mcp_context_t *ctx, const mcp_json_value_t *arr, size_t i);
    size_t (*array_size)(mcp_context_t *ctx, const mcp_json_value_t *arr);
};

const mcp_json_backend_ops_t *mcp_json_builtin_backend(void);
// ctx NULL -> no-op. ops NULL -> restore builtin. Borrowed, must outlive ctx.
void mcp_json_set_backend(mcp_context_t *ctx, const mcp_json_backend_ops_t *ops);

// NULL return = invalid argument / depth / encoding / OOM. Trailing garbage -> NULL.
mcp_json_value_t *mcp_json_parse(mcp_context_t *ctx, const char *text, size_t len);
// Canonical minimal form. Caller owns; free with mcp_json_free_string (same ctx).
char *mcp_json_serialize(mcp_context_t *ctx, const mcp_json_value_t *v);
void mcp_json_free_string(mcp_context_t *ctx, char *s); // NULL-safe

#endif
```

`mcpkit.h` 追加 `#include "mcpkit/core/capability.h"`、`context.h`、`logging/*`、`json/json.h`。

---

## Chunk 2: 实现任务（TDD，每任务独立提交）

### Task 1: allocator

**Files:** Modify `include/mcpkit/core/types.h`, `CMakeLists.txt`; Create `src/core/allocator.c`.

`src/core/allocator.c`（全文）:

```c
#include "mcpkit/core/types.h"

#include <stdlib.h>

static void *libc_malloc(size_t size, void *userdata) { (void)userdata; return malloc(size); }
static void libc_free(void *ptr, void *userdata) { (void)userdata; free(ptr); }
static void *libc_calloc(size_t n, size_t size, void *userdata) { (void)userdata; return calloc(n, size); }
static void *libc_realloc(void *ptr, size_t size, void *userdata) { (void)userdata; return realloc(ptr, size); }

static const mcp_allocator_t k_default = {
    .malloc_fn = libc_malloc, .free_fn = libc_free,
    .calloc_fn = libc_calloc, .realloc_fn = libc_realloc, .userdata = NULL,
};

const mcp_allocator_t *mcp_default_allocator(void) { return &k_default; }
```

CMake 在 `add_library` 追加 `src/core/allocator.c`。

- [ ] Step 1: 落盘后 `cmake -S . -B build && cmake --build build -j` 通过（ Phase 0 测试仍 2/2）。
- [ ] Step 2: Commit `git add include/mcpkit/core/types.h src/core/allocator.c CMakeLists.txt && git commit -m "feat(core): add pluggable allocator"`。

### Task 2: logger

**Files:** Create `include/mcpkit/logging/log.h`, `logger.h`, `src/logging/logger.c`, `tests/unit/test_logger.c`; Modify `tests/CMakeLists.txt`, `CMakeLists.txt`（lib 加 `src/logging/logger.c`）。

`src/logging/logger.c` 行为：`level_string` 越界返回 `"UNKNOWN"`；`create` 任一 NULL 容忍（alloc NULL→默认，sink NULL→stderr）；`log` 按 `level < logger->level` 丢弃；`destroy` 释放自身（sink/userdata 不归 logger 管）。

`tests/unit/test_logger.c`（全文）:

```c
#include <assert.h>
#include <string.h>

#include "mcpkit/logging/log.h"
#include "mcpkit/logging/logger.h"

static char g_buf[256];
static size_t g_len;
static void mem_sink(mcp_log_level_t level, const char *msg, void *ud) {
    (void)level; (void)ud;
    size_t n = strlen(msg);
    if (g_len + n < sizeof(g_buf)) { memcpy(g_buf + g_len, msg, n); g_len += n; }
}

int main(void) {
    assert(strcmp(mcp_log_level_string(MCP_LOG_WARN), "WARN") == 0);
    assert(strcmp(mcp_log_level_string((mcp_log_level_t)99), "UNKNOWN") == 0);
    mcp_logger_t *lg = mcp_logger_create(NULL, mem_sink, NULL);
    assert(lg != NULL);
    mcp_logger_set_level(lg, MCP_LOG_WARN);
    mcp_logger_log(lg, MCP_LOG_DEBUG, "dropped");
    assert(g_len == 0);
    mcp_logger_log(lg, MCP_LOG_ERROR, "kept");
    assert(g_len == 4);
    mcp_logger_log(NULL, MCP_LOG_ERROR, "noop");
    mcp_logger_log(lg, MCP_LOG_ERROR, NULL);
    assert(mcp_logger_get_level(NULL) == MCP_LOG_ERROR);
    mcp_logger_destroy(lg);
    mcp_logger_destroy(NULL);
    mcp_log_sink_stderr(MCP_LOG_INFO, "smoke\n", NULL);
    mcp_logger_t *d = mcp_logger_default_stderr(NULL);
    assert(d != NULL);
    mcp_logger_destroy(d);
    return 0;
}
```

- [ ] Step 1: 先只加测试跑 `ctest -R test_logger`，预期 FAIL（无实现）。
- [ ] Step 2: 实现后全量 `cmake --build build -j && ctest --test-dir build` 3/3 通过。
- [ ] Step 3: Commit `feat(logging): add level-filtered logger with sinks`。

### Task 3: context + capability

**Files:** Create `include/mcpkit/core/context.h`, `capability.h`, `src/core/context.c`, `tests/unit/test_context.c`; Modify `tests/CMakeLists.txt`, `CMakeLists.txt`, `include/mcpkit/mcpkit.h`。

`src/core/context.c` 行为：内部 `struct mcp_context { mcp_allocator_t alloc; mcp_logger_t *logger; bool owns_logger; const void *json_backend; }`（backend 存 `const void *` 避免 include json 头，`mcp_context_json_backend` 转型返回——零循环依赖；`mcp_json_set_backend` 由 json/api.c 经内部 setter 写入？不——setter 必须写 `context.c` 的 struct。方案：`context.h` 声明 `void mcp_context_set_json_backend(mcp_context_t *, const void *)` 内部函数（下划线文档标 INTERNAL），`json.h` 的 `mcp_json_set_backend` 是包在 api.c 里的公开 wrapper 做 NULL 检查后调用它。`mcp_context_json_backend` 同理返回 `const void *` 再由 api.c 转型？公开头返回类型已是 `const mcp_json_backend_ops_t *`（前向声明），context.c 只需前向声明即可持有指针，无需完整定义——直接存该类型，无需 `void *` hack。）

`tests/unit/test_context.c`（全文）:

```c
#include <assert.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/capability.h"
#include "mcpkit/json/json.h"

static int g_allocs;
static void *counting_malloc(size_t n, void *ud) { (void)ud; g_allocs++; return malloc(n); }
static void counting_free(void *p, void *ud) { (void)ud; free(p); }

int main(void) {
    mcp_capabilities_t caps = MCP_CAPABILITIES_INIT;
    assert(!caps.tools && !caps.resources && !caps.prompts);
    mcp_context_t *c = mcp_context_create(NULL);
    assert(c != NULL);
    assert(mcp_context_allocator(c) != NULL);
    assert(mcp_context_logger(c) != NULL);
    assert(mcp_context_json_backend(c) == NULL);
    mcp_logger_t *borrowed = mcp_logger_default_stderr(NULL);
    mcp_context_config_t cfg = {.allocator = NULL, .logger = borrowed, .json_backend = NULL};
    mcp_context_t *c2 = mcp_context_create(&cfg);
    assert(mcp_context_logger(c2) == borrowed); // borrowed identity
    mcp_context_destroy(c2);
    mcp_logger_destroy(borrowed); // still ours
    mcp_context_destroy(c);
    static const mcp_allocator_t counting = {
        .malloc_fn = counting_malloc, .free_fn = counting_free,
        .calloc_fn = NULL, .realloc_fn = NULL, .userdata = NULL,
    };
    // NOTE: calloc/realloc NULL -> context must reject config? 契约: 表格函数指针全非NULL才接受,否则 INVALID? create 无 status 通道 -> 契约改为: 缺失则回退 libc 对应项。此处传全表,见实现注释。
    assert(mcp_context_allocator(NULL) == mcp_default_allocator());
    assert(mcp_context_logger(NULL) == NULL);
    assert(mcp_context_json_backend(NULL) == NULL);
    mcp_context_destroy(NULL);
    return 0;
}
```

注意：`counting` 表缺 calloc/realloc —— 契约定为“缺失项回退 libc”（create 只有 NULL/OOM 两种出口，无法报 INVALID）。实现照此执行，测试仅断言默认回退路径（不实际使用该表创建，避免歧义）。

- [ ] Step 1–3: 同 Task 2 节奏（先 FAIL 后 PASS 再提交 `feat(core): add context with allocator/logger/backend slots`）。

### Task 4: JSON（DOM + 解析 + 序列化）

**Files:** Create `include/mcpkit/json/*.h`（4 个，照 Chunk 1），`src/json/builtin.c`，`src/json/api.c`，`tests/unit/test_json_value.c`，`test_json_roundtrip.c`，`test_json_negative.c`；Modify `tests/CMakeLists.txt`，`CMakeLists.txt`，`mcpkit.h`。

内部 DOM（仅 `builtin.c` 可见）:

```c
struct mcp_json_value {
    mcp_json_type_t type;
    union {
        bool boolean;
        double number;
        char *string;
        struct { mcp_json_value_t **items; size_t len, cap; } array;
        struct { char **keys; mcp_json_value_t **vals; size_t len, cap; } object;
    } u;
};
```

关键规则（全文实现时遵守，review 对照）:
1. UTF-8 用 Hoehrmann DFA 验证（`string_new_n`、object key、parse 输入与 string 结果）。
2. `\uXXXX` 含代理对组合；孤立代理半 → parse 失败；`\` 转义全集 `"\/bfnrtu`，其它 → 失败；字符串内裸 `<0x20` → 失败。
3. 数字经 `strtod` 全消耗 + `isfinite`，`1e999` → 失败；`number_new(NaN/Inf)` → NULL。
4. 深度计数进 `{`/`[` 即 +1，`> MCP_JSON_MAX_DEPTH` → 失败并释放局部。
5. 顶层多值/尾随垃圾 → 释放并 NULL；输入必须整体有效 UTF-8（先整段 DFA 一遍，O(n) 可接受）。
6. 序列化 canonical：无空格；`"` `\` `\n \r \t \b \f` 短转义，其余 C0 用 `\u00XX`；`%.17g`。
7. `api.c` 全为 thin dispatcher：`ops = ctx ? mcp_context_json_backend(ctx) : NULL; ops ? ops : builtin`；ctx NULL → builtin。
8. 容器增长 ×2；`object_set` 重复 key 替换并释放旧值。

`tests/unit/test_json_negative.c`（全文，核心门禁）:

```c
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "mcpkit/json/json.h"

static void expect_null(const char *text) {
    mcp_json_value_t *v = mcp_json_parse(NULL, text, strlen(text));
    assert(v == NULL);
}

int main(void) {
    expect_null("");
    expect_null("   ");
    expect_null("{");
    expect_null("{\"a\":");
    expect_null("{\"a\":1,}");
    expect_null("[1,]");
    expect_null("{,}");
    expect_null("{} trailing");
    expect_null("[1 2]");
    expect_null("{'a':1}");
    expect_null("NaN");
    expect_null("Infinity");
    expect_null("[1e999]");
    expect_null("\"bad \\q escape\"");
    expect_null("\"lone \\ud800 surrogate\"");
    expect_null("\"\\ud83d\""); // high half only
    expect_null("\"raw \x01 control\"");
    expect_null("\"\\x41\""); // no \x in JSON
    // invalid UTF-8 raw bytes
    expect_null("\"\xff\"");
    expect_null("\"abc\x80\"");
    expect_null("\"\\ud800\""); // encoded surrogate -> invalid scalar
    // depth boundary: 128 ok, 129 rejected
    char deep_ok[512], deep_bad[520];
    size_t p = 0;
    for (int i = 0; i < 128; i++) { deep_ok[p++] = '['; }
    for (int i = 0; i < 128; i++) { deep_ok[p++] = ']'; }
    deep_ok[p] = '\0';
    mcp_json_value_t *v = mcp_json_parse(NULL, deep_ok, p);
    assert(v != NULL);
    mcp_json_destroy(NULL, v);
    p = 0;
    for (int i = 0; i < 129; i++) { deep_bad[p++] = '['; }
    for (int i = 0; i < 129; i++) { deep_bad[p++] = ']'; }
    deep_bad[p] = '\0';
    expect_null(deep_bad);
    return 0;
}
```

`test_json_value.c` 精确断言清单：7 构造器类型正确；错类型 accessor → `INVALID_ARGUMENT`；NULL 入参 → NULL/INVALID；`number_new(INFINITY)` → NULL；`string_new("\xff")` → NULL；`object_set` 成功转移（父 destroy 即全释，ASan 下跑）；`object_set` 对非 OBJECT → ERR 且子值仍归调用方（随后手动 destroy 无泄漏，ASan 验证）；重复 key 替换后 size 不变、值为新值；`array_get` 越界 → NULL。

`test_json_roundtrip.c` 精确断言清单：API 建嵌套文档 `{name:"caf\u00e9 \U0001F600", n:-1.5e10, t:true, z:null, arr:[1,"a\\b",{}]}` → serialize → parse → serialize 两次输出 `strcmp` 相等；emoji 以 `\uD83D\uDE00` 输入解析后序列化为 UTF-8 原字节（含 `"\xf0\x9f\x98\x80"` 子串）；`mcp_json_free_string` 释放无 crash。

- [ ] Step 1: 测试先行，四文件 commit 前必须 `ctest` 见 FAIL（实现缺失链接失败亦可）。
- [ ] Step 2: 实现后 `ctest` 7/7（2 老 + logger + context + 3 json）全绿。
- [ ] Step 3: Commit `feat(json): add zero-dependency DOM, parser and serializer`。

### Task 5: backend 可插拔证明 + 全门禁

**Files:** Create `tests/unit/test_json_backend.c`；Modify `tests/CMakeLists.txt`，`CHANGELOG.md`。

`tests/unit/test_json_backend.c`（全文，计数 wrapper 委托 builtin）:

```c
#include <assert.h>
#include <string.h>

#include "mcpkit/json/json.h"
#include "mcpkit/core/context.h"

static const mcp_json_backend_ops_t *g_real;
static int g_parse_calls, g_serialize_calls, g_new_calls;

static mcp_json_value_t *wrap_parse(mcp_context_t *c, const char *t, size_t n) {
    g_parse_calls++;
    return g_real->parse(c, t, n);
}
static char *wrap_serialize(mcp_context_t *c, const mcp_json_value_t *v) {
    g_serialize_calls++;
    return g_real->serialize(c, v);
}
static mcp_json_value_t *wrap_new_object(mcp_context_t *c) {
    g_new_calls++;
    return g_real->new_object(c);
}

int main(void) {
    g_real = mcp_json_builtin_backend();
    static mcp_json_backend_ops_t wrap;
    memcpy(&wrap, g_real, sizeof(wrap));
    wrap.name = "counting-wrapper";
    wrap.parse = wrap_parse;
    wrap.serialize = wrap_serialize;
    wrap.new_object = wrap_new_object;
    mcp_context_t *ctx = mcp_context_create(NULL);
    mcp_json_set_backend(ctx, &wrap);
    mcp_json_value_t *o = mcp_json_object_new(ctx);
    assert(o != NULL && g_new_calls == 1);
    mcp_json_value_t *v = mcp_json_parse(ctx, "{\"a\":1}", 7);
    assert(v != NULL && g_parse_calls == 1);
    char *s = mcp_json_serialize(ctx, v);
    assert(s != NULL && g_serialize_calls == 1);
    assert(strcmp(s, "{\"a\":1}") == 0);
    mcp_json_free_string(ctx, s);
    mcp_json_destroy(ctx, v);
    mcp_json_destroy(ctx, o);
    mcp_json_set_backend(ctx, NULL); // restore builtin
    mcp_json_value_t *v2 = mcp_json_parse(ctx, "[true]", 6);
    assert(v2 != NULL && g_parse_calls == 1); // wrapper bypassed
    mcp_json_destroy(ctx, v2);
    mcp_json_set_backend(NULL, &wrap); // NULL-safe no-op
    mcp_context_destroy(ctx);
    return 0;
}
```

`CHANGELOG.md` 在 `[Unreleased] / Added` 追加：`- Phase 1: pluggable allocator, logger, context, zero-dependency JSON.`。

门禁命令（依次执行，全部 exit 0）：

```bash
cmake -S . -B build -DMCPKIT_BUILD_TESTS=ON && cmake --build build -j && ctest --test-dir build --output-on-failure
cmake -S . -B build-clang -DCMAKE_C_COMPILER=clang -DMCPKIT_BUILD_TESTS=ON && cmake --build build-clang -j && ctest --test-dir build-clang
cmake -S . -B build-asan -DCMAKE_C_FLAGS="-fsanitize=address,undefined" && cmake --build build-asan -j && ctest --test-dir build-asan
gcc -std=c2x -Wall -Wextra -Werror -I include -c src/json/builtin.c -o /tmp/j.o
clang -std=c2x -Wall -Wextra -Werror -I include -c src/json/builtin.c -o /tmp/j2.o
rm -rf build build-clang build-asan /tmp/j.o /tmp/j2.o
```

- [ ] Step 1: backend 测试先 FAIL（`mcp_json_set_backend` 未实现）。
- [ ] Step 2: 实现（`context.c` 加 backend 槽 + `api.c` wrapper）后 8/8 全绿。
- [ ] Step 3: Commit `feat(json): prove pluggable backend seam` + `docs: changelog phase 1`（可合一）。
