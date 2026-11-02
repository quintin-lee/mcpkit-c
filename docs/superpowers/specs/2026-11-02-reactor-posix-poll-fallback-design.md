# 事件反应器 Reactor 跨平台 POSIX `poll()` 降级实现设计

> 状态：提案 (Brainstorming 完成，待 Spec Reviewer 验证与用户批准)  
> 日期：2026-11-02  
> 责任模块：`src/runtime/reactor.c`, `include/mcpkit/runtime/reactor.h`, `cmake/MCPKitOptions.cmake`, `tests/unit/test_reactor.c`, `CMakeLists.txt`

---

## 1. 目标与背景 (Intent & Why)

### 1.1 背景
`mcpkit-c` 在运行时层引入了基于非阻塞 I/O 的单线程事件多路复用组件 `mcp_reactor_t`（位于 [`src/runtime/reactor.c`](file:///data/home/quintin/workspace/source/c/mcpkit-c/src/runtime/reactor.c)），为上层的 Stdio、Socket 以及高并发统一异步服务 `mcp_async_server_t` 提供统一的事件调度与跨线程唤醒支持。

在项目文档与架构设计中，明确承诺了“Linux `epoll` + POSIX `poll` 回退”。但在目前的具体实现中：
1. **系统头文件强耦合**：`src/runtime/reactor.c` 直接无条件包含了 Linux 专有的 `<sys/epoll.h>`，且仅实现了基于 `epoll_create1`、`epoll_ctl` 与 `epoll_wait` 的内核系统调用。
2. **缺乏非 Linux 平台的构建与运行能力**：在 macOS (Darwin)、FreeBSD、OpenBSD 等非 Linux POSIX 平台上，由于不存在 `<sys/epoll.h>`，代码会在编译预处理阶段直接报错失败；此外，唤醒管道创建中对 `pipe2` 的使用也会在不支持 `pipe2` 的系统（如 macOS）上报错。
3. **缺乏可验证的本地回退通道**：Linux 开发机和 CI 默认只能跑 `epoll` 分支，若不提供显式的降级构建配置，降级实现代码容易处于缺乏测试防护的灰色地带。

### 1.2 目标
为 `mcp_reactor_t` 补全原生 POSIX `poll()` 降级实现：
- 在未提供 `<sys/epoll.h>` 的系统（如 macOS / BSD）上自动切换为纯 POSIX `poll()` 后端；
- 提供 CMake 显式配置开关 `MCPKIT_REACTOR_FORCE_POLL`，允许在 Linux 环境下强制启用 `poll` 后端进行双引擎对照测试；
- 保持 `include/mcpkit/runtime/reactor.h` 公开 API 100% 稳定不变；
- 两种后端下 `mcp_reactor_*` 的语义、错误码、跨线程唤醒与调度行为保持完全一致；
- 在 `-Wall -Wextra` 门禁下保持零警告，全量单元测试与压力测试 100% 通过。

---

## 2. 目标、非目标与反目标 (Goals, Non-Goals, Anti-Goals)

### 2.1 目标 (Goals)
1. **自动平台检测与平滑降级**：
   - 当检测到 Linux 且具备 `<sys/epoll.h>` 且未强制开启 poll 时，使用高效的 `epoll` 后端；
   - 在其他平台（或 `MCPKIT_REACTOR_FORCE_POLL=ON`）时，编译为 POSIX `poll()` 后端。
2. **跨平台 self-pipe 唤醒机制**：
   - 在支持 `eventfd` 的 Linux 下使用 `eventfd`（无论选用 epoll 还是 poll 引擎）；若未提供或在非 Linux（如 macOS / BSD）环境下，使用标准 POSIX `pipe()` + `fcntl(FD_CLOEXEC, O_NONBLOCK)`，避免在 macOS 等缺少 `pipe2` 的系统上引用无效符号。
3. **完备的迭代器失效与全量事件防饥饿防护**：
   - 在 `poll()` 返回就绪事件后，将所有就绪事件提取到结构体内部维护的动态快照缓冲区（`snapshots`，容量与 `capacity` 对应），杜绝硬编码固定长度导致的就绪事件饥饿截断；并在每次回调触发前校验目标 fd 存在性，彻底消除回调重入调用 `mcp_reactor_remove` 时元素平移导致的事件丢失或跳过缺陷。
4. **零外部第三方依赖**：
   - 严格使用 POSIX.1-2008 标准系统调用（`<poll.h>`, `<unistd.h>`, `<fcntl.h>`），不引入 `libkqueue` 等外部第三方包装。

### 2.2 非目标 (Non-Goals)
1. **kqueue 专用引擎开发**：暂不引入 BSD/macOS 专有的 `kqueue` 系统调用，优先采用通用标准 POSIX `poll()` 保障跨平台普遍可用性（YAGNI，`poll()` 足以满足中小规模多路复用与全平台兼容）。
2. **Windows IOCP 支持**：暂不直接封装 Windows 原生重叠 I/O，Windows 环境下通过 MSYS2 / POSIX 兼容层使用 `poll()`。

### 2.3 反目标 (Anti-Goals)
1. **禁止破坏原有公开 API 签名**：不得向 `mcp_reactor_create` 或其他公共函数增加平台特有的入参。
2. **禁止改变已有事件掩码语义**：`MCP_EVENT_READ`、`MCP_EVENT_WRITE`、`MCP_EVENT_ERROR` 的位定义及其对 EOF / HUP 的处理语义必须与现存 epoll 实现完全对称等价。

---

## 3. 详细技术设计 (Detailed Design)

### 3.1 预编译条件与后端选择宏

在 `src/runtime/reactor.c` 头部实现明确的编译期后端选型，并将 `<sys/eventfd.h>` 解耦独立包含：
```c
#if defined(__linux__) && __has_include(<sys/epoll.h>) && !defined(MCP_REACTOR_FORCE_POLL)
#define MCP_REACTOR_BACKEND_EPOLL 1
#include <sys/epoll.h>
#else
#define MCP_REACTOR_BACKEND_POLL 1
#include <poll.h>
#endif

#if __has_include(<sys/eventfd.h>)
#include <sys/eventfd.h>
#endif
```

---

### 3.2 结构体布局与资源封装

```c
typedef struct {
    int fd;
    short revents;
} poll_event_snapshot_t;

struct mcp_reactor {
#if defined(MCP_REACTOR_BACKEND_EPOLL)
    int epoll_fd;
#else
    struct pollfd *poll_fds;              /* 动态分配的 pollfd 数组，容量为 capacity + 1 */
    poll_event_snapshot_t *snapshots;     /* 动态就绪事件快照数组，容量为 capacity */
#endif
    int wakeup_fd;                        /* 读端 fd (eventfd 或 self-pipe 读端) */
    int wakeup_write_fd;                  /* 写端 fd (-1 表示使用单一 eventfd) */
    atomic_bool stop_requested;

    mcp_reactor_entry_t *entries;         /* 按 fd 有序排列的回调信息表 */
    size_t count;
    size_t capacity;
};
```

---

### 3.3 跨平台唤醒机制 (Wakeup Channel)

1. **Linux 环境**：无论选择 epoll 还是 poll 引擎，只要定义了 `EFD_CLOEXEC` 与 `EFD_NONBLOCK`，优先尝试 `eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)`；
2. **非 Linux 或降级环境**：
   - 严禁在 macOS / BSD 上无条件调用 `pipe2`（macOS 不支持 `pipe2`）；
   - 规范为：
     ```c
     int pipe_fds[2];
     int w_read = -1;
     int w_write = -1;
#if defined(__linux__) && defined(_GNU_SOURCE)
     if (pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) == 0) {
         w_read = pipe_fds[0];
         w_write = pipe_fds[1];
     }
#endif
     if (w_read < 0) {
         if (pipe(pipe_fds) == 0) {
             (void)fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
             (void)fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);
             int flags0 = fcntl(pipe_fds[0], F_GETFL, 0);
             (void)fcntl(pipe_fds[0], F_SETFL, flags0 | O_NONBLOCK);
             int flags1 = fcntl(pipe_fds[1], F_GETFL, 0);
             (void)fcntl(pipe_fds[1], F_SETFL, flags1 | O_NONBLOCK);
             w_read = pipe_fds[0];
             w_write = pipe_fds[1];
         }
     }
     ```
3. **唤醒与排空 (`wakeup` / `drain_wakeup`)**：
   - 保持现有 `mcp_reactor_wakeup` 与 `drain_wakeup` 逻辑完全不变（已具备区分 `wakeup_write_fd >= 0` 的自适应能力）。

---

### 3.4 POSIX `poll()` 后端状态机实现

#### 3.4.1 事件双向转换映射（保持与 epoll 100% 对称）
```c
#if defined(MCP_REACTOR_BACKEND_POLL)
static short map_to_poll_events(mcp_event_mask_t events) {
    short ev = 0;
    if (events & MCP_EVENT_READ) {
        ev |= (short)POLLIN;
#if defined(POLLRDHUP)
        ev |= (short)POLLRDHUP;
#endif
    }
    if (events & MCP_EVENT_WRITE) {
        ev |= (short)POLLOUT;
    }
    return ev;
}

static mcp_event_mask_t map_from_poll_revents(short revents, mcp_event_mask_t registered) {
    mcp_event_mask_t mask = 0;
    /* 1. 读就绪：POLLIN 或对端半关闭 POLLRDHUP */
    if (revents & (POLLIN
#if defined(POLLRDHUP)
                   | POLLRDHUP
#endif
                  )) {
        if (registered & MCP_EVENT_READ) {
            mask |= MCP_EVENT_READ;
        }
    }
    /* 2. 写就绪：POLLOUT */
    if (revents & POLLOUT) {
        if (registered & MCP_EVENT_WRITE) {
            mask |= MCP_EVENT_WRITE;
        }
    }
    /* 3. 错误与对端挂断：POLLERR, POLLHUP, POLLNVAL 严格与 epoll (EPOLLERR | EPOLLHUP) 对称 */
    if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
        if (registered & MCP_EVENT_ERROR) {
            mask |= MCP_EVENT_ERROR;
        } else if (registered & MCP_EVENT_READ) {
            mask |= MCP_EVENT_READ;
        }
    }
    return mask;
}
#endif
```

#### 3.4.2 反应器创建与初始状态分配 (`mcp_reactor_create`)
在 `mcp_reactor_create()` 中：
1. 创建自唤醒通道（得到 `w_read`, `w_write`）；
2. 实例化 `mcp_reactor_t *r = rt_malloc(ctx, sizeof(*r))`；
3. 初始化通用字段：`r->wakeup_fd = w_read; r->wakeup_write_fd = w_write; r->count = 0; r->capacity = 0; r->entries = NULL; atomic_init(&r->stop_requested, false);`；
4. **poll 后端初始 Slot 0 分配**：
   - 在 poll 模式下，分配恰好容纳 Slot 0 唤醒描述符的初始数组：
     ```c
     r->poll_fds = rt_malloc(ctx, 1 * sizeof(struct pollfd));
     if (r->poll_fds == NULL) {
         close(w_read);
         if (w_write >= 0) close(w_write);
         rt_free(ctx, r);
         return NULL;
     }
     r->poll_fds[0].fd = w_read;
     r->poll_fds[0].events = POLLIN;
     r->poll_fds[0].revents = 0;
     r->snapshots = NULL;
     ```
   - 保持 `r->capacity = 0; r->entries = NULL;`，从而保证当尚未添加任何业务 fd 时，调用 `mcp_reactor_step` 会以 `poll(r->poll_fds, 1, timeout_ms)` 正常等待唤醒，且第一次调用 `mcp_reactor_add` 时 `r->count == r->capacity` (`0 == 0`) 会触发两者的首次同步扩容。

#### 3.4.3 数组排布与安全扩容 (`mcp_reactor_add / modify / remove`)
- **添加操作 (`mcp_reactor_add`)**：
  1. 通过二分查找确定插入位置 `ins`；若已存在返回 `MCP_ERR_ALREADY_EXISTS`；
  2. 容量扩容处理（事务性与内存安全原则）：
     - 若 `r->count == r->capacity`：
       - 计算 `new_cap = r->capacity == 0 ? 16 : r->capacity * 2`；
       - **先重分配 `poll_fds`**：
         ```c
         struct pollfd *new_poll_fds = rt_realloc(ctx, r->poll_fds, (new_cap + 1) * sizeof(*new_poll_fds));
         if (new_poll_fds == NULL) return MCP_ERR_NOMEM;
         r->poll_fds = new_poll_fds;
         ```
       - **再重分配 `snapshots`**：
         ```c
         poll_event_snapshot_t *new_snapshots = rt_realloc(ctx, r->snapshots, new_cap * sizeof(*new_snapshots));
         if (new_snapshots == NULL) return MCP_ERR_NOMEM;
         r->snapshots = new_snapshots;
         ```
       - **最后重分配 `entries`**：
         ```c
         mcp_reactor_entry_t *new_entries = rt_realloc(ctx, r->entries, new_cap * sizeof(*new_entries));
         if (new_entries == NULL) return MCP_ERR_NOMEM;
         r->entries = new_entries;
         ```
       - 三者全部成功后更新容量标记：`r->capacity = new_cap;`；
  3. 平移插入新项：
     - 将 `r->entries` 在 `ins` 后的元素向后平移 1 位；
     - 将 `r->poll_fds` 在 `ins + 1` 后的元素向后平移 1 位；
     - 初始化 `poll_fds[ins + 1].fd = fd; poll_fds[ins + 1].events = map_to_poll_events(events); poll_fds[ins + 1].revents = 0;`；
     - 初始化 `entries[ins]`，`r->count++`；
- **修改操作 (`mcp_reactor_modify`)**：
  1. 二分查找定位索引 `idx`；若不存在返回 `MCP_ERR_NOT_FOUND`；
  2. 更新 `r->entries[idx].events = events; r->poll_fds[idx + 1].events = map_to_poll_events(events);`；
- **删除操作 (`mcp_reactor_remove`)**：
  1. 二分查找定位索引 `idx`；若不存在返回 `MCP_ERR_NOT_FOUND`；
  2. 将 `entries[idx + 1]` 与 `poll_fds[idx + 2]` 之后的项前移覆盖；
  3. `r->count--`；
- **销毁操作 (`mcp_reactor_destroy`)**：
  - 释放 `r->poll_fds`、`r->snapshots` 与 `r->entries`，关闭 `wakeup_fd` 与 `wakeup_write_fd`。

#### 3.4.4 单步轮询调度与快照分发 (`mcp_reactor_step`)
1. 检查关机状态：`if (mcp_shutdown_requested()) return MCP_ERR_CANCELLED;`；
2. 执行轮询：`int n = poll(r->poll_fds, (nfds_t)(r->count + 1), timeout_ms);`；
3. 处理返回值：
   - `n < 0`：若 `errno == EINTR`，返回 `MCP_OK`；否则返回 `MCP_ERR_IO`；
   - `n == 0`：超时，**先检查关机**：`if (mcp_shutdown_requested()) return MCP_ERR_CANCELLED; return MCP_OK;`；
4. **处理唤醒事件（Slot 0）**：
   - 若 `r->poll_fds[0].revents != 0`，调用 `drain_wakeup(r)`；
5. **动态快照采集（解决回调重入平移失效与大并发饥饿截断）**：
   - 提取所有就绪的业务描述符到 `r->snapshots` 缓冲区：
     ```c
     size_t n_fired = 0;
     for (size_t i = 1; i <= r->count; i++) {
         if (r->poll_fds[i].revents != 0) {
             r->snapshots[n_fired].fd = r->poll_fds[i].fd;
             r->snapshots[n_fired].revents = r->poll_fds[i].revents;
             n_fired++;
         }
     }
     ```
6. **快照分发与存在性二次验证**：
   - 遍历快照数组 `for (size_t i = 0; i < n_fired; i++)`：
     - 若 `atomic_load(&r->stop_requested) || mcp_shutdown_requested()`，跳出分发；
     - **存在性二次验证**：调用 `find_entry_idx(r, r->snapshots[i].fd, &cur_idx)`；
     - 若返回 `< 0`，说明该 fd 已在当前批次前面执行的回调中被注销，安全跳过；
     - 否则获取 `entry = r->entries[cur_idx]`，计算 `mcp_event_mask_t mask = map_from_poll_revents(r->snapshots[i].revents, entry.events)`；
     - 若 `mask != 0`，执行回调 `entry.cb(ctx, r->snapshots[i].fd, mask, entry.user)`；
7. 尾部检查关机：`if (mcp_shutdown_requested()) return MCP_ERR_CANCELLED; return MCP_OK;`。

---

## 4. 构建配置与验证计划 (Build & Verification Plan)

### 4.1 CMake 选项支持
在 `cmake/MCPKitOptions.cmake` 中添加开发测试选项：
```cmake
option(MCPKIT_REACTOR_FORCE_POLL "Force POSIX poll backend in reactor for testing" OFF)
```
并在根目录 `CMakeLists.txt` 中将配置紧随 options 之后加入：
```cmake
if(MCPKIT_REACTOR_FORCE_POLL)
  add_compile_definitions(MCP_REACTOR_FORCE_POLL)
endif()
```

### 4.2 自动化测试设计
1. **单元测试回归与重入快照验证 (`test_reactor.c`)**：
   - 验证添加/修改/删除基本流程；
   - 验证自唤醒（`mcp_reactor_wakeup`）；
   - 验证空反应器超时（`test_timeout_step` 在未注册业务 fd 时正常工作，验证 Slot 0 唤醒通道）；
   - **新增专项用例 `test_reentrant_remove_sibling`**：
     - 创建两组 socketpair `sv1` 与 `sv2` 并全部注册入 reactor 监听读；
     - 同时向 `sv1[1]` 与 `sv2[1]` 写入数据使两者同时在同一次 `poll()` 批次中就绪；
     - 在 `sv1[0]` 的读回调中执行 `mcp_reactor_remove(ctx, r, sv2[0])`；
     - 断言 `sv2[0]` 的回调在当次迭代中**绝不会被非法触发**，且 `reactor` 状态完全正常。
2. **双引擎对照验证矩阵**：
   - 默认配置（Linux 下为 `epoll`）：`ctest --test-dir build` 69/69 100% 通过；
   - 强制 poll 配置：`cmake -S . -B build-poll -DMCPKIT_REACTOR_FORCE_POLL=ON && cmake --build build-poll -j && ctest --test-dir build-poll`，验证全量 69/69 单元与验收测试在 `poll` 引擎下完全通过！
3. **`-Wall -Wextra` 验证**：
   - 两个构建均保持零警告。

---

## 5. 验收标准 (Acceptance Criteria)

- `src/runtime/reactor.c` 实现 `epoll` 与 `poll` 双后端无缝切换；
- 移除对 `<sys/epoll.h>` 的无条件强包含；
- 唤醒通道在非 Linux 模式下使用标准 POSIX `pipe()` + `fcntl()`，消除 macOS 下的 `pipe2` 符号缺失风险；
- `-DMCPKIT_REACTOR_FORCE_POLL=ON` 构建配置下所有 69+ 项已有测试（包括 `async_server`、`async_concurrency` 压力测试）100% 全绿；
- 编译无警告，ASan/UBSan 无内存泄漏与越界。
