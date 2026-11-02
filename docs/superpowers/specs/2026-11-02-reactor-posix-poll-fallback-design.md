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
2. **缺乏非 Linux 平台的构建与运行能力**：在 macOS (Darwin)、FreeBSD、OpenBSD 等非 Linux POSIX 平台上，由于不存在 `<sys/epoll.h>`，代码会在编译预处理阶段直接报错失败。
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
2. **自包含 self-pipe 跨平台唤醒**：
   - 在非 Linux 环境下，自动使用 `pipe()` + `O_NONBLOCK` + `FD_CLOEXEC` 实现可靠的 self-pipe 唤醒机制，替代 Linux 独有的 `eventfd`。
3. **完备的迭代器失效防护**：
   - 在 `poll()` 批次返回后的事件循环派发过程中，若回调函数内部调用了 `mcp_reactor_remove` 或 `mcp_reactor_add`，不能发生非法内存访问或未定义行为。
4. **零外部第三方依赖**：
   - 严格使用 POSIX.1-2008 标准系统调用（`<poll.h>`, `<unistd.h>`, `<fcntl.h>`），不引入 `libkqueue` 等外部第三方包装。

### 2.2 非目标 (Non-Goals)
1. **kqueue 专用引擎开发**：暂不引入 BSD/macOS 专有的 `kqueue` 系统调用，优先采用通用标准 POSIX `poll()` 保障跨平台普遍可用性（YAGNI，`poll()` 足以满足中小规模多路复用与全平台兼容）。
2. **Windows IOCP 支持**：暂不直接封装 Windows 原生重叠 I/O，Windows 环境下通过 MSYS2 / POSIX 兼容层使用 `poll()`。

### 2.3 反目标 (Anti-Goals)
1. **禁止破坏原有公开 API 签名**：不得向 `mcp_reactor_create` 或其他公共函数增加平台特有的入参。
2. **禁止改变已有事件掩码语义**：`MCP_EVENT_READ`、`MCP_EVENT_WRITE`、`MCP_EVENT_ERROR` 的位定义及其对 EOF / HUP 的处理语义必须与现存实现完全等价。

---

## 3. 详细技术设计 (Detailed Design)

### 3.1 预编译条件与后端选择宏

在 `src/runtime/reactor.c` 头部实现明确的编译期后端选型：
```c
#if defined(__linux__) && __has_include(<sys/epoll.h>) && !defined(MCP_REACTOR_FORCE_POLL)
#define MCP_REACTOR_BACKEND_EPOLL 1
#include <sys/epoll.h>
#if __has_include(<sys/eventfd.h>)
#include <sys/eventfd.h>
#endif
#else
#define MCP_REACTOR_BACKEND_POLL 1
#include <poll.h>
#endif
```

---

### 3.2 结构体布局与资源封装

```c
struct mcp_reactor {
#if defined(MCP_REACTOR_BACKEND_EPOLL)
    int epoll_fd;
#else
    struct pollfd *poll_fds;      /* 动态分配的 pollfd 数组，容量为 capacity + 1 */
#endif
    int wakeup_fd;                /* 读端 fd (eventfd 或 self-pipe 读端) */
    int wakeup_write_fd;          /* 写端 fd (-1 表示使用单一 eventfd) */
    atomic_bool stop_requested;

    mcp_reactor_entry_t *entries; /* 按 fd 有序排列的回调信息表 */
    size_t count;
    size_t capacity;
};
```

---

### 3.3 唤醒机制 (Wakeup Pipe / Eventfd)

1. **Linux 环境**：优先使用 `eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)`，若失败则回退至 self-pipe；
2. **POSIX / 降级环境**：直接使用标准非阻塞 self-pipe：
   - 调用 `pipe(pipe_fds)`；
   - 对两端分别设置 `fcntl(pipe_fds[i], F_SETFD, FD_CLOEXEC)` 与 `fcntl(pipe_fds[i], F_SETFL, O_NONBLOCK)`；
   - `w_read = pipe_fds[0]`, `w_write = pipe_fds[1]`；
3. **唤醒与排空 (`wakeup` / `drain_wakeup`)**：
   - 保持现有 `mcp_reactor_wakeup` 与 `drain_wakeup` 逻辑完全不变（已具备区分 `wakeup_write_fd >= 0` 的自适应能力）。

---

### 3.4 POSIX `poll()` 后端状态机实现

#### 3.4.1 事件双向转换映射
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
    if (revents & (POLLIN | POLLHUP
#if defined(POLLRDHUP)
                   | POLLRDHUP
#endif
                  )) {
        if (registered & MCP_EVENT_READ) {
            mask |= MCP_EVENT_READ;
        }
    }
    if (revents & POLLOUT) {
        if (registered & MCP_EVENT_WRITE) {
            mask |= MCP_EVENT_WRITE;
        }
    }
    if (revents & (POLLERR | POLLNVAL)) {
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

#### 3.4.2 数组排布与增删改 (Array Management)
- **数组结构**：
  - `poll_fds[0]` 固定绑定给 `r->wakeup_fd`，监听 `events = POLLIN`；
  - `poll_fds[1 ~ count]` 与按 fd 升序排列的 `entries[0 ~ count-1]` **一一对应**；
- **添加操作 (`mcp_reactor_add`)**：
  1. 通过二分查找确定插入位置 `ins`；若已存在返回 `MCP_ERR_ALREADY_EXISTS`；
  2. 容量扩容时，同步对 `entries` 和 `poll_fds` 重新分配内存（容量扩充至 `new_cap` 与 `new_cap + 1`）；
  3. 通过 `memmove` 将 `ins` 之后的项向后平移 1 个槽位（`poll_fds` 对应平移 `ins + 1` 处的项）；
  4. 初始化 `poll_fds[ins + 1].fd = fd`, `poll_fds[ins + 1].events = map_to_poll_events(events)`, `poll_fds[ins + 1].revents = 0`；
  5. 初始化 `entries[ins]`；
- **修改操作 (`mcp_reactor_modify`)**：
  1. 二分查找定位索引 `idx`；若不存在返回 `MCP_ERR_NOT_FOUND`；
  2. 更新 `r->entries[idx].events = events`；
  3. 更新 `r->poll_fds[idx + 1].events = map_to_poll_events(events)`；
- **删除操作 (`mcp_reactor_remove`)**：
  1. 二分查找定位索引 `idx`；若不存在返回 `MCP_ERR_NOT_FOUND`；
  2. 将 `entries[idx + 1]` 与 `poll_fds[idx + 2]` 之后的项前移平移覆盖；
  3. `r->count--`；
- **销毁操作 (`mcp_reactor_destroy`)**：
  - 释放 `r->poll_fds` 与 `r->entries`，关闭 `wakeup_fd` 与 `wakeup_write_fd`。

#### 3.4.3 单步轮询调度 (`mcp_reactor_step`)
1. 检查关机状态：`if (mcp_shutdown_requested()) return MCP_ERR_CANCELLED;`；
2. 执行轮询：`int n = poll(r->poll_fds, (nfds_t)(r->count + 1), timeout_ms);`；
3. 处理返回值：
   - `n < 0`：若 `errno == EINTR`，返回 `MCP_OK`；否则返回 `MCP_ERR_IO`；
   - `n == 0`：超时，返回 `MCP_OK`；
4. **处理唤醒事件（Slot 0）**：
   - 若 `r->poll_fds[0].revents & POLLIN`，调用 `drain_wakeup(r)`；
5. **分发注册事件与防失效快照**：
   - 提取待处理事件数量；
   - 遍历 `i = 1 ~ count`（在遍历期间如果回调调用了 `mcp_reactor_remove` 或 `mcp_reactor_add`，通过在分发前做目标 fd 存在性二次校验）：
     ```c
     for (size_t i = 1; i <= r->count; i++) {
         if (atomic_load(&r->stop_requested) || mcp_shutdown_requested()) {
             break;
         }
         short rev = r->poll_fds[i].revents;
         if (rev == 0) continue;
         int target_fd = r->poll_fds[i].fd;
         size_t cur_idx = 0;
         if (find_entry_idx(r, target_fd, &cur_idx) < 0) {
             /* 目标 fd 已在当前批次前面的回调中被注销，跳过 */
             continue;
         }
         mcp_reactor_entry_t entry = r->entries[cur_idx];
         mcp_event_mask_t fired = map_from_poll_revents(rev, entry.events);
         if (fired != 0) {
             entry.cb(ctx, target_fd, fired, entry.user);
         }
     }
     ```

---

## 4. 构建配置与验证计划 (Build & Verification Plan)

### 4.1 CMake 选项支持
在 `cmake/MCPKitOptions.cmake` 中添加开发测试选项：
```cmake
option(MCPKIT_REACTOR_FORCE_POLL "Force POSIX poll backend in reactor for testing" OFF)
```
并在根目录 `CMakeLists.txt` 中：
```cmake
if(MCPKIT_REACTOR_FORCE_POLL)
  add_compile_definitions(MCP_REACTOR_FORCE_POLL)
endif()
```

### 4.2 自动化测试设计
1. **单元测试回归 (`test_reactor.c`)**：
   - 验证添加/修改/删除基本流程；
   - 验证自唤醒（`mcp_reactor_wakeup`）；
   - 验证超时时间触发；
   - 验证回调内部注销当前 fd（重入安全性）；
   - 验证回调内部注销后继触发的 fd（防止处理无效事件）；
2. **双引擎对照验证矩阵**：
   - 默认配置（Linux 下为 `epoll`）：`ctest --test-dir build` 69/69 100% 通过；
   - 强制 poll 配置：`cmake -S . -B build-poll -DMCPKIT_REACTOR_FORCE_POLL=ON && cmake --build build-poll -j && ctest --test-dir build-poll`，验证全量 69/69 单元与验收测试在 `poll` 引擎下完全通过！
3. **`-Wall -Wextra` 验证**：
   - 两个构建均保持零警告。

---

## 5. 验收标准 (Acceptance Criteria)

- `src/runtime/reactor.c` 实现 `epoll` 与 `poll` 双后端无缝切换；
- 移除对 `<sys/epoll.h>` 的无条件强包含；
- `-DMCPKIT_REACTOR_FORCE_POLL=ON` 构建配置下所有 69+ 项已有测试（包括 `async_server`、`async_concurrency` 压力测试）100% 全绿；
- 编译无警告，ASan/UBSan 无内存泄漏与越界。
