# guard_daemon 模块学习文档

> 适用对象：具备 C 语言基础、了解 Linux 基本命令，想系统学习 Linux 进程编程的开发者
> 源码位置：`src/guard_daemon/`（guard_daemon.h / guard_daemon.c / test/test_guard.c / Makefile）
> 学习方式：本文档按「概念铺垫 → 架构设计 → 运行逻辑 → 代码逐段精讲 → 构建测试 → 自测练习」的顺序组织，建议按章节顺序阅读。

---

## 目录

1. [模块是什么：一句话定位](#1-模块是什么一句话定位)
2. [前置知识地图：读懂本模块需要的 Linux 概念](#2-前置知识地图)
3. [架构设计：从全局视角看模块](#3-架构设计)
4. [使用逻辑：程序跑起来后发生什么](#4-使用逻辑)
5. [代码设计逐段精讲](#5-代码设计逐段精讲)
6. [构建系统与单元测试](#6-构建系统与单元测试)
7. [关键知识点深挖](#7-关键知识点深挖)
8. [自测练习](#8-自测练习)
9. [可扩展方向](#9-可扩展方向)

---

## 1. 模块是什么：一句话定位

**guard_daemon 是车载终端系统的"总管"进程（守护进程）**：它自己在后台常驻，负责把其他 5 个业务进程（GPS、录像、音视频、按键输入、CAN 总线）拉起来，盯着它们别死掉；死了就自动重启；同时每 5 秒体检一次系统（CPU/内存/温度/磁盘），并通过 UNIX Socket 把这些健康数据提供给 Qt 界面（UI）查询。

用现实世界类比：

| 现实角色 | 对应模块职责 |
|---|---|
| 工厂值班总管 | guard_daemon 本身 |
| 招募并安排工人上岗 | fork+exec 启动 5 个子进程 |
| 工人晕倒了扶起来 | 崩溃自动重启（带速率限制） |
| 定时巡查水电仪表 | 每 5 秒读 /proc 采集系统状态 |
| 前台问询窗口 | UNIX Socket 响应 UI 的状态查询 |
| 下班时按顺序关灯锁门 | 优雅退出（反向停止子进程） |

**为什么需要这样的进程？** 嵌入式车载设备没有桌面环境，开机后必须有一个常驻进程负责：
- 把所有业务服务按正确顺序拉起；
- 任何一个服务崩溃时自动恢复（车辆行驶中不可能人工干预）；
- 监控系统资源，防止温度过高、磁盘写满等故障；
- 给上层界面提供统一的状态查询入口。

---

## 2. 前置知识地图

读代码前，先建立这些概念的最小认知。每个概念在第 7 章都有深挖。

### 2.1 进程相关

| 概念 | 最小认知 | 在模块中的用途 |
|---|---|---|
| `fork()` | 复制当前进程，产生父子两个进程；父进程拿到子进程 PID，子进程拿到 0 | 创建子进程 |
| `exec()` 族 | 用新程序替换当前进程的代码，替换成功则原代码不再执行 | 让子进程变成 gps_daemon 等目标程序 |
| 僵尸进程 (zombie) | 子进程退出后、父进程回收前，残留的进程表项 | 必须用 waitpid 回收，否则耗尽 PID |
| `waitpid()` | 父进程回收子进程的退出状态；`WNOHANG` 表示不阻塞 | 每轮主循环回收已退出子进程 |
| 守护进程 (daemon) | 脱离控制终端、在后台常驻的系统进程 | 模块自身的存在形态 |

### 2.2 信号相关

| 信号 | 特性 | 在模块中的用途 |
|---|---|---|
| `SIGCHLD` | 子进程退出时内核发给父进程 | 设为默认处理，靠 waitpid 回收 |
| `SIGTERM` | 可捕获的"请你退出"信号 | 优雅终止子进程 / 触发自身退出 |
| `SIGKILL` | 不可捕获，内核强杀 | 子进程不响应 SIGTERM 时的最后手段 |
| `SIGINT` | Ctrl-C | 前台调试模式（-f）下退出 |

### 2.3 IO 与系统接口

| 概念 | 最小认知 | 在模块中的用途 |
|---|---|---|
| `/proc` 伪文件系统 | 内核把运行时数据以文件形式导出，读取即动态生成 | 采集 CPU/内存/负载 |
| `/sys` (sysfs) | 设备和驱动信息的文件接口 | 读 CPU 温度 |
| `statfs()` | 查询文件系统统计信息的系统调用 | 查 eMMC / SD 卡剩余空间 |
| `poll()` | 同时监视多个 fd，等待任一事件或超时 | 主事件循环的核心 |
| UNIX Domain Socket | 同一台机器上进程间通信的套接字（不走网络协议栈） | guard 与 UI 的 IPC 通道 |

---

## 3. 架构设计

### 3.1 总体架构图

```
                        ┌─────────────────────────────────────┐
                        │          guard_daemon (总管)          │
                        │                                     │
  Qt UI 客户端           │   ┌───────────┐   ┌─────────────┐  │
  ┌──────────┐  UNIX    │   │ 子进程管理 │   │ 系统健康监控 │  │
  │          │  Socket  │   │ children[]│   │ sys_status  │  │
  │ 状态查询  ├─────────►│   └─────┬─────┘   └──────┬──────┘  │
  │          │◄─────────┤         │                │         │
  └──────────┘ /tmp/    │   ┌─────▼────────────────▼──────┐  │
             car_guard  │   │     主事件循环 (poll)         │  │
               .sock    │   │  超时→心跳  fd事件→收发消息    │  │
                        │   │  每轮 waitpid 回收僵尸子进程   │  │
                        │   └─────────────────────────────┘  │
                        └──────┬──────┬──────┬──────┬──────┬─┘
                          fork │      │      │      │      │ + exec
                               ▼      ▼      ▼      ▼      ▼
                              gps   dvr     av   input  canbus
                            daemon daemon daemon daemon daemon
                            0ms   500ms  1000ms 1500ms 2000ms
```

### 3.2 四大职责分解

模块把头文件注释里写明了四大职责，这是理解架构的钥匙：

1. **守护进程化 (daemonize)**：双 fork 脱离终端，成为系统级后台进程；
2. **子进程生命周期管理**：启动、监控、回收、崩溃重启、优雅退出；
3. **系统健康监控**：通过 /proc、/sys、statfs 采集 CPU/内存/负载/温度/磁盘；
4. **IPC 服务端**：UNIX Socket 监听，响应 UI 的状态查询。

### 3.3 核心数据结构（三件套）

模块的全部运行时状态集中在三个结构体里，位于 `guard_daemon.h`：

**① `child_process_t` —— 一个子进程的完整档案**

```c
typedef struct {
    /* 静态配置 */
    char    name[32];          /* 进程名，用于日志 */
    char    bin_path[256];     /* 可执行文件完整路径 */
    int     auto_restart;      /* 异常退出后是否自动重启 */
    int     startup_delay_ms;  /* 启动延迟，错开启动避免资源争抢 */

    /* 运行时状态 */
    pid_t   pid;               /* 子进程 PID，0 表示未启动 */
    int     state;             /* PROC_STOPPED/STARTING/RUNNING/ERROR */
    time_t  start_time;        /* 最近一次启动时间 */

    /* 重启速率限制（滑动窗口计数器） */
    int     restart_count;     /* 当前窗口内已重启次数 */
    time_t  first_restart;     /* 当前窗口的起始时间 */

    /* 退出诊断 */
    int     exit_code;         /* WIFEXITED 时的退出码 */
    int     exit_signal;       /* WIFSIGNALED 时的终止信号 */
} child_process_t;
```

设计要点：**配置、状态、限速、诊断四类信息聚合在同一个结构体里**。每个子进程的所有信息都在这一个"档案"中，主循环里用 `pid` 就能反查到对应档案。这是典型的"数据驱动"设计——新增一个子进程只需多填一条档案，不需要改管理逻辑。

**② `system_status_t` —— 一次系统体检的快照**

```c
typedef struct {
    float  cpu_percent;    /* CPU 使用率 (%)，/proc/stat 差值计算 */
    int    mem_total_kb;   /* 总内存，/proc/meminfo */
    int    mem_free_kb;    /* 空闲内存 */
    float  mem_percent;    /* 内存使用率 */
    float  load_1min;      /* 1 分钟平均负载，/proc/loadavg */
    float  cpu_temp;       /* CPU 温度 (℃)，/sys/class/thermal */
    long   emmc_free_mb;   /* eMMC 剩余 (MB)，statfs("/") */
    long   sd_free_mb;     /* SD 卡剩余 (MB)，statfs("/mnt/sdcard") */
    time_t update_time;    /* 最近采集时间戳 */
} system_status_t;
```

设计要点：这个结构体会被**整体序列化**通过 Socket 发给 UI（`(uint8_t*)&ctx->sys_status, sizeof(...)`），所以字段全是定长的基本类型，没有指针——这是"可直接传输的结构体"的典型写法。

**③ `guard_context_t` —— 全局上下文（所有状态的根）**

```c
typedef struct {
    child_process_t children[GUARD_MAX_CHILDREN]; /* 子进程表 */
    int             child_count;
    system_status_t sys_status;      /* 最近一次系统状态快照 */
    int             running;         /* 主循环控制标志 */
    int             server_fd;       /* 监听 socket */
    int             client_fd;       /* 客户端连接 (-1=无连接) */
    protocol_parser_t proto_parser;  /* 协议帧解析状态机 */
    ringbuffer_t    recv_rb;         /* 接收环形缓冲区 */
    proc_config_t   proc_cfg;        /* 日志/心跳等配置 */
} guard_context_t;
```

设计要点：用一个全局实例 `static guard_context_t g_ctx` 持有全部状态。对单实例守护进程来说，"一个大上下文结构体"比散落的全局变量好——状态集中、易于 memset 清零、易于传给各函数。

### 3.4 子进程状态机

每个子进程的状态迁移如下：

```
                 guard_child_start()
   (未启动) ──────────────────────────► PROC_RUNNING
      ▲                                    │
      │ 重启(限速内)                        │ 退出/被杀
      │                                    ▼
      │◄─── guard_child_restart() ── PROC_STOPPED
                                           │
                                           │ 60秒内重启超过5次
                                           ▼
                                      PROC_ERROR (放弃重启，需人工介入)
```

关键决策：**为什么要有 `PROC_ERROR` 这个终态？** 如果一个子进程因为硬件故障或配置错误而"启动即崩溃"，无条件自动重启会形成 fork→crash→fork→crash 的无限循环（重启风暴），烧 CPU、刷爆日志。滑动窗口限速 + ERROR 终态就是熔断机制。

### 3.5 为什么用 poll 而不是 epoll？

头文件注释里专门说明了：**guard 只监视 2 个 fd（server_fd + client_fd）**。poll 在 fd 数量少时开销与 epoll 相当，且代码更简单、不需要额外管理 epoll 实例。这是"按规模选型"的好例子——不是 epoll 不好，而是这个场景用不上。

---

## 4. 使用逻辑

这一章从"程序运行起来后时间线上发生了什么"的角度讲解。

### 4.1 启动流程（main 函数的前半段）

```
./guard_daemon            ← 正常运行：自动 daemonize，后台常驻
./guard_daemon -f         ← 调试模式：前台运行，不脱离终端，方便 gdb/看输出
```

启动时序（对应 `main()` 第 554-609 行）：

```
第1步  解析参数：是否带 -f（前台调试模式）
第2步  daemonize（双fork，见 5.1 节）      ← 此后脱离终端
第3步  memset 清零上下文 g_ctx，client_fd = -1
第4步  初始化日志（文件滚动：1MB × 3 个备份）
第5步  build_child_table()：构建 5 个子进程档案
第6步  依次启动子进程，每个之间按 0/500/1000/1500/2000ms 错开
第7步  创建 UNIX Socket 服务端（/tmp/car_guard.sock）
第8步  注册信号：SIGCHLD=默认，SIGTERM/SIGINT=优雅退出
第9步  首次采集系统状态（给 CPU 使用率差值计算一个基准）
第10步 进入主事件循环
```

**为什么子进程要错开启动？** 5 个进程同时启动会造成 CPU/IO 尖峰（thundering herd，惊群式资源争抢）。错开 500ms 让启动负载平摊。这是嵌入式系统的常见手法。

### 4.2 主循环：一个循环干三件事

主循环（第 621-699 行）是整个模块的心脏，一轮循环做三件事：

```
┌─────────────────────────────────────────────────────────┐
│  while (g_running) {                                    │
│                                                         │
│    ① poll(监听fd数组, timeout=5秒)                        │
│         ├─ 5秒内没有socket事件 → 超时返回 → 触发心跳       │
│         └─ 有事件 → 立即返回                              │
│                                                         │
│    ② waitpid(-1, WNOHANG) 循环回收僵尸子进程              │
│         └─ 发现是"异常退出+允许自动重启" → 触发重启(限速)   │
│                                                         │
│    ③ 距上次心跳 ≥5秒 → update_status() 采集系统状态       │
│                                                         │
│    ④ handle_sockets() 处理连接/收发消息                  │
│  }                                                      │
└─────────────────────────────────────────────────────────┘
```

这里有一个精妙的设计：**poll 的 timeout 同时充当了心跳定时器**。

- 如果 5 秒内 UI 没有任何请求 → poll 超时返回 → 顺便做一次系统体检；
- 如果 UI 频繁请求 → poll 被事件唤醒，但第 ③ 步还有 `now - last >= 5s` 的时间检查做双重保障。

这样就不需要额外的 timer fd 或定时器线程——**单线程同时处理"定时任务 + 事件驱动"**，是小型守护进程的经典架构。

### 4.3 崩溃重启的完整链路

假设 canbus_daemon 因为段错误崩溃：

```
1. 内核向 guard 发送 SIGCHLD（设为默认处理，不中断 poll）
2. canbus_daemon 变成僵尸进程，等待回收
3. 主循环本轮（或下一轮）执行 waitpid(-1, WNOHANG)
   └─ 回收僵尸，拿到 status，WIFSIGNALED 为真，WTERMSIG=11 (SIGSEGV)
4. 遍历子进程表，用 pid 匹配到 canbus_daemon 的档案
5. 记录日志：退出原因（退出码 / 信号 / 是否 core dump）
6. 检查 auto_restart==1 且系统仍在运行 → 调用 guard_child_restart()
7. 滑动窗口检查：
   ├─ 60秒窗口内重启未超5次 → stop旧进程 + 等500ms + start新进程
   └─ 超过5次 → 标记 PROC_ERROR，放弃（熔断）
```

### 4.4 优雅退出的完整链路

收到 `SIGTERM`（如 `kill <pid>` 或系统关机）：

```
1. sig_handler 只做一件事：g_running = 0
   （信号处理函数里只设标志位——这是信号安全的铁律，见 7.4 节）
2. 主循环条件 g_running 变假，退出循环
3. 反向顺序停止子进程（LIFO：后启动的先停）
   └─ 原因：后启动的进程可能依赖先启动的进程
      （例如 av 可能依赖 dvr），先停上层避免悬空依赖
4. 每个子进程：SIGTERM → 轮询等待3秒 → 仍不退 → SIGKILL → waitpid 回收
5. 关闭客户端连接 → 关闭服务端 socket（并删除 .sock 文件）
6. 关闭日志 → return 0
```

### 4.5 IPC 交互：UI 怎么查状态

```
UI 客户端                              guard_daemon
    │                                       │
    │ connect(/tmp/car_guard.sock)          │
    ├──────────────────────────────────────►│ accept
    │                                       │ （已有连接则拒绝新连接）
    │ 发送 MSG_SERVICE_STATUS 查询帧          │
    ├──────────────────────────────────────►│ recv → ringbuffer
    │                                       │ → protocol_unpack 解帧
    │ 回复 MSG_SERVICE_STATUS + 状态结构体    │
    │◄──────────────────────────────────────┤ sock_send_frame
    │                                       │
    │ 断开                                   │
    ├──────────────────────────────────────►│ client_fd = -1，等待重连
```

数据收发的分层设计值得注意：

```
原始字节流 (recv)
   → 环形缓冲区 ringbuffer（处理粘包/半包：一次 recv 可能包含半帧或多帧）
   → protocol_unpack 状态机逐帧解析（帧头/长度/校验/载荷）
   → 按消息类型分发
```

**为什么要环形缓冲区 + 状态机解帧？** 因为 TCP/UNIX 流式 socket 不保证"一次 send 对应一次 recv"——粘包和拆包是常态。把字节先攒进 ringbuffer，再由解析器按需取用，是处理流式协议的标准架构。

---

## 5. 代码设计逐段精讲

以下按"从简单到复杂"的顺序逐个函数讲解。

### 5.1 `guard_daemonize()` —— 双 fork 守护进程化

```c
void guard_daemonize(void)
{
    if (fork() > 0) _exit(0);   /* 第一次 fork：父进程退出 */
    setsid();                   /* 创建新会话，脱离控制终端 */
    if (fork() > 0) _exit(0);   /* 第二次 fork：父进程退出 */
    chdir("/");                 /* 切到根目录，避免占用挂载点 */
    umask(0);                   /* 重置文件创建掩码 */

    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, 0);            /* stdin  → /dev/null */
        dup2(fd, 1);            /* stdout → /dev/null */
        dup2(fd, 2);            /* stderr → /dev/null */
        if (fd > 2) close(fd);
    }
}
```

**逐步拆解"为什么要这样做"：**

| 步骤 | 做什么 | 为什么 |
|---|---|---|
| 第一次 fork | 父进程退出，子进程继续 | ① 让 shell 收回控制权（后台化）；② 子进程不再是进程组组长，这是 setsid 的前提（组长调用 setsid 会失败） |
| setsid | 创建新会话 | 脱离控制终端，从此终端关闭、SIGHUP 都不再影响它 |
| 第二次 fork | 父进程（会话组长）退出，孙进程继续 | 孙进程不是会话组长，而只有会话组长能 open 一个 tty 重新获取控制终端——这是一道"永久隔离防线" |
| chdir("/") | 切换到根目录 | 如果守护进程的工作目录在某个挂载点上，该挂载点永远无法 umount |
| umask(0) | 清零文件掩码 | 继承来的 umask 可能拒绝某些权限位，清零后文件权限完全由 open 参数决定 |
| 重定向到 /dev/null | 0/1/2 指向空设备 | 防止 printf 写已关闭的终端触发 SIGPIPE；日志应走 log 模块 |

**学习建议**：这段代码只有 15 行，但每一行都有历史原因。试着自己回答：如果去掉第二次 fork，会出什么问题？（答案见 7.1 节）

### 5.2 `guard_child_start()` —— fork + exec 启动子进程

```c
int guard_child_start(child_process_t *child)
{
    if (!child || !child->bin_path[0]) return -1;

    pid_t pid = fork();
    if (pid < 0) {                       /* fork 失败 */
        LOG_ERROR("guard","fork: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* ---- 子进程 ---- */
        execlp(child->bin_path, child->bin_path, NULL);
        _exit(1);   /* 只有 exec 失败才走到这里 */
    }

    /* ---- 父进程 ---- */
    child->pid = pid;
    child->state = PROC_RUNNING;
    child->start_time = time(NULL);
    return 0;
}
```

**三个必须掌握的设计细节：**

1. **`fork()` 的三分支判断**：`pid < 0` 失败、`pid == 0` 子进程、`pid > 0` 父进程。这是所有 fork 代码的固定骨架。

2. **为什么用 `_exit(1)` 而不是 `exit(1)`？**
   - `exit()` 会执行 atexit 回调、刷新 stdio 缓冲区——这些是"父进程的善后逻辑"，在 fork 之后的子进程里执行会产生副作用（比如把父进程缓冲的日志重复刷一份）；
   - `_exit()` 直接陷入内核终止进程，干净利落。
   - **规则：fork 之后、exec 之前的子进程代码里，只用 `_exit`。**

3. **fork 的写时复制 (COW)**：现代内核不会在 fork 时真的复制整个内存，父子共享物理页，谁写才复制谁。所以"fork 一个几十 MB 的进程后立即 exec"开销很小——这也是 fork+exec 模型至今高效的原因。

### 5.3 `guard_child_stop()` —— 两阶段终止

```c
void guard_child_stop(child_process_t *child)
{
    if (!child || child->pid <= 0) return;

    /* 阶段1：SIGTERM，轮询等待最多 3 秒 (30 × 100ms) */
    kill(child->pid, SIGTERM);
    int status, wait_cnt = 0;
    while (waitpid(child->pid, &status, WNOHANG) == 0 && wait_cnt < 30) {
        usleep(100000);
        wait_cnt++;
    }

    /* 阶段2：仍不退出 → SIGKILL 强杀 + 阻塞回收 */
    if (waitpid(child->pid, &status, WNOHANG) == 0) {
        kill(child->pid, SIGKILL);
        waitpid(child->pid, &status, 0);   /* 阻塞，确保回收 */
    }

    child->pid = 0;
    child->state = PROC_STOPPED;
}
```

**为什么是"先礼后兵"两阶段？**

- SIGTERM 可被捕获 → 子进程有机会做清理（关文件、存状态、释放锁）；
- 但子进程可能卡死（阻塞 IO、死循环）→ 3 秒后 SIGKILL 兜底；
- SIGKILL 由内核执行、不可拦截，所以"一定能死"。

这个 `SIGTERM → 超时 → SIGKILL` 模式是进程管理的通用最佳实践，systemd、Docker 都是同款。

**注意 waitpid 的三种返回值**：`>0` 已回收；`0` 还没退出（WNOHANG）；`-1` 出错（如进程已不存在）。

### 5.4 `guard_child_restart()` —— 滑动窗口限速

```c
void guard_child_restart(child_process_t *child)
{
    time_t now = time(NULL);

    /* 窗口过期（或首次）→ 重置计数器，开新窗口 */
    if (child->restart_count == 0 ||
        now - child->first_restart > GUARD_RESTART_WINDOW_SEC) {
        child->restart_count = 0;
        child->first_restart = now;
    }

    child->restart_count++;

    /* 超过 5 次 → 熔断，标记 PROC_ERROR */
    if (child->restart_count > GUARD_MAX_RESTART) {
        child->state = PROC_ERROR;
        return;
    }

    guard_child_stop(child);    /* 停旧 */
    usleep(500000);             /* 等 500ms 让 socket/资源释放干净 */
    guard_child_start(child);   /* 启新 */
}
```

**滑动窗口的工作方式（举例）：**

```
时刻   事件                        窗口状态
00:00  第1次重启                    窗口开启 [00:00, 01:00)，计数1
00:10  第2次重启                    计数2
00:30  第3次重启                    计数3
00:40  第4次重启                    计数4
00:50  第5次重启                    计数5（还允许）
00:55  第6次重启 → 超限！           标记 PROC_ERROR，放弃
—— 如果是另一种情况 ——
00:50  第5次重启                    计数5
01:30  再次崩溃 → 距窗口起点已>60秒  重置计数，重新允许重启
```

**为什么重启前要 `usleep(500ms)`？** 旧进程的 socket 文件可能还没被内核清理、SIGCHLD 可能还在路上。留一点时间给资源释放，避免新旧进程的竞态条件。

### 5.5 `update_status()` —— 解析 /proc 采集系统状态

这个函数是"用户空间读内核数据"的范例，四段采集各有代表性手法：

**① CPU 使用率：差值法**

```c
static long pt = 0, pi = 0;   /* static：跨调用保存上次采样值 */
/* /proc/stat: "cpu user nice system idle iowait irq softirq steal" */
long tot = u + n + s + i + io + ir + si + stl;
long td  = tot - pt;          /* 总时间差 */
long id  = i   - pi;          /* idle 时间差 */
cpu_percent = (td - id) * 100 / td;
```

关键点：`/proc/stat` 给的是**开机以来的累计值**，单次读没有意义，必须两次采样做差。`static` 变量在这里承担了"上一次采样"的记忆职责。

**② 内存：键值扫描法**

```c
while (fscanf(f, "%s %ld", l, &v) == 2) {
    if (!strcmp(l, "MemTotal:")) st->mem_total_kb = (int)v;
    if (!strcmp(l, "MemFree:"))  st->mem_free_kb  = (int)v;
}
```

`/proc/meminfo` 是 `Key: value kB` 格式的文本，逐行扫描、按键名取值即可。

**③ 负载 / 温度：单值直读**

```c
fscanf(f, "%f", &st->load_1min);        /* /proc/loadavg 第一个数 */
st->cpu_temp = (float)t / 1000.0f;      /* sysfs 单位是毫摄氏度 */
```

注意温度的单位陷阱：`/sys/class/thermal/thermal_zone0/temp` 是毫摄氏度，要除以 1000。

**④ 磁盘：statfs 系统调用**

```c
struct statfs fs;
if (statfs("/", &fs) == 0)
    st->emmc_free_mb = fs.f_bavail * fs.f_bsize / 1048576;
```

比 fork 一个 `df` 命令高效得多。`f_bavail` 是非 root 用户可用块数，`f_bsize` 是块大小，乘起来除以 1024² 得 MB。

### 5.6 `handle_sockets()` —— 事件分发

```c
static void handle_sockets(guard_context_t *ctx, int srv_ev, int cli_ev)
{
    /* 有新连接：已占用则拒绝，否则登记为当前客户端 */
    if (srv_ev & POLLIN) { ... }

    /* 客户端消息：recv → ringbuffer → 循环解帧 → 按类型分发 */
    int n = sock_recv(ctx->client_fd, buf, sizeof(buf));
    if (n <= 0) { /* 断开连接，client_fd = -1 */ return; }

    ringbuffer_write(&ctx->recv_rb, buf, n);
    while (protocol_unpack(...) == 0) {
        if (t == MSG_SERVICE_STATUS)
            sock_send_frame(fd, MSG_SERVICE_STATUS,
                            (uint8_t*)&ctx->sys_status, sizeof(...));
    }
}
```

设计要点：
- **单客户端策略**：同一时间只服务一个 UI 连接，新连接直接关闭。嵌入式场景连接方唯一（就是车机屏幕），简化了并发模型；
- **`n <= 0` 的三种含义都要处理**：EAGAIN（无数据）、0（对端正常关闭）、-1（错误），这里统一按断开处理；
- **`while` 循环解帧**：一次 recv 可能带来多个完整帧，必须解到"帧不完整"为止。

### 5.7 `main()` —— 把一切串起来

主函数的编排顺序本身就是知识点（见 4.1 节的 10 步时序），这里补充两个易忽略的细节：

**① 信号注册的位置**：在启动完子进程之后、进入主循环之前。如果注册太晚（比如先跑主循环再注册），中间窗口内子进程退出没人管。

**② `SIGCHLD` 为什么设为 `SIG_DFL` 而不是 `SIG_IGN`？**
- `SIG_DFL`（默认）：子进程退出变僵尸，等 waitpid 回收——行为确定；
- `SIG_IGN`（忽略）：某些 UNIX 系统上内核会自动回收子进程，导致 waitpid 拿不到退出状态，且行为跨平台不一致；
- 本项目需要精确的退出码/信号做日志和重启判断，所以选 `SIG_DFL` + 主动 waitpid。

---

## 6. 构建系统与单元测试

### 6.1 Makefile 解读

```makefile
HOST ?= 0
ifeq ($(HOST),1)
  CC = gcc                                    # 本机编译（开发调试）
else
  CROSS_COMPILE ?= arm-buildroot-linux-gnueabihf-
  CC = $(CROSS_COMPILE)gcc                    # 交叉编译（目标板）
endif
```

**双模式编译**是这个 Makefile 的核心设计：
- `make` → 交叉编译出 ARM 版本，部署到车载开发板；
- `make host` → 用本机 gcc 编译，在 PC 上跑逻辑验证；
- `make test` → 编译并运行单元测试（加 `-DTEST_MODE` 宏）。

其他值得学习的点：
- `CFLAGS` 里的 `-ffunction-sections -fdata-sections` 配合 `LDFLAGS` 的 `-Wl,--gc-sections`：**把没用到的函数/数据段在链接期裁剪掉**，嵌入式减小体积的标准手法；
- 编译产物自动拷贝到 `../../build/bin`，统一集成目录；
- 依赖 `../common/lib/libcarcommon.a` 公共库（log、protocol、socket 等模块的沉淀）。

### 6.2 单元测试怎么测一个守护进程？

守护进程涉及 fork、信号、/proc，看起来很"不可测"，`test/test_guard.c` 展示了拆解思路：

| 测试对象 | 测试手法 |
|---|---|
| 子进程表构建 | 调 `guard_config_load()`，断言数量、名字、路径、标志位 |
| fork+exec+waitpid | 真实 fork 一个 `/bin/true`，验证退出码为 0 |
| 重启计数逻辑 | **不真启进程**，只操纵 `restart_count`/`first_restart` 字段，验证窗口判断 |

关键思想：**把纯逻辑（计数、状态迁移）从系统调用（fork、poll）中剥离出来单独测**。测试里用 `TS/TC/OK/FL/AS` 宏搭了一个极简测试框架（计数 + 打印 + 断言），没有外部依赖，适合嵌入式环境。

### 6.3 运行与调试

```bash
# 目标板（交叉编译）
make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-

# 后台运行
./guard_daemon

# 前台调试（不 daemonize，日志直接可见，可 Ctrl-C / gdb）
./guard_daemon -f

# 优雅停止
kill -TERM <guard_pid>
```

日志位置：`/var/log/car_terminal/guard.log`（1MB 滚动，保留 3 份）。
Socket 路径：`/tmp/car_guard.sock`。

---

## 7. 关键知识点深挖

### 7.1 双 fork 的必要性（回答 5.1 的思考题）

如果只做一次 fork + setsid：子进程成为**新会话的组长**。而 POSIX 规定：会话组长可以通过 `open()` 一个终端设备重新获得控制终端。虽然我们的程序不会主动这么做，但万一将来代码里某个库 open 了 tty 设备，就可能意外绑定终端，破坏守护进程的"无终端"前提。

第二次 fork 产生的孙进程**不是会话组长**，从机制上永远无法获取控制终端。这是一种防御性设计——不为"现在会做什么"负责，而为"将来不可能做什么"负责。

### 7.2 僵尸进程与 waitpid

子进程退出时，内核不会立即销毁它，而是保留 PID、退出状态等信息，等父进程来"认领"——这个中间态就是僵尸进程。

- 少量僵尸无害，但**大量僵尸会耗尽 PID 表**，最终整个系统无法 fork 新进程；
- 父进程用 `waitpid` 认领后，内核才彻底释放；
- 如果父进程先死，子进程会被 init（PID 1）收养并由 init 自动回收——所以守护进程的父进程是 init 而非 shell。

本模块每轮主循环都执行 `while (waitpid(-1, ..., WNOHANG) > 0)`，`-1` 表示等待任意子进程，`WNOHANG` 表示不阻塞，循环直到没有可回收的为止——一次能回收多个同时退出的子进程。

### 7.3 退出状态的解读宏

```c
WIFEXITED(status)    → 正常退出（调用了 exit/_exit）
  WEXITSTATUS(status)→ 取低 8 位退出码
WIFSIGNALED(status)  → 被信号杀死
  WTERMSIG(status)   → 取信号编号（11=SIGSEGV, 9=SIGKILL）
  WCOREDUMP(status)  → 是否产生 core dump
```

主循环中用这组宏区分"正常退出"和"被信号杀死"，分别记不同级别的日志——这是排查现场问题的重要信息。

### 7.4 信号处理的安全铁律

```c
static volatile int g_running = 1;
static void sig_handler(int s) { (void)s; g_running = 0; }
```

信号处理函数里**只设置一个 `volatile` 标志位**，其他所有事情都留到主循环里做。原因：

- 信号处理函数运行在"异步中断"上下文，能安全调用的函数极少（称为 async-signal-safe 函数）；
- 在信号处理里调用 `malloc`、`printf`、`LOG_INFO` 都可能死锁或崩溃（比如信号恰好打断了一次 malloc 内部的锁）；
- `volatile` 防止编译器把 `g_running` 优化进寄存器缓存——信号在函数外部修改它，必须每次从内存读。

### 7.5 poll 事件模型

```c
struct pollfd pfds[3];
pfds[0] = { server_fd, POLLIN, 0 };   /* 监听新连接 */
pfds[1] = { client_fd, POLLIN, 0 };   /* 监听客户端数据 */
poll(pfds, nfds, 5000);               /* 最多等 5 秒 */
```

- `poll` 返回后检查每个 `revents`：`POLLIN`=有数据，`POLLHUP`=对端断开，`POLLERR`=错误；
- 与 epoll 的区别：poll 每次调用都要把整个 fd 数组传给内核，O(n) 扫描；epoll 用红黑树管理、回调就绪，O(1) 通知。**fd 少（<10）选 poll，fd 多、连接活跃选 epoll**；
- 本模块的 timeout 一石二鸟：既是 poll 的等待上限，也是心跳周期的来源。

---

## 8. 自测练习

按难度排列，建议先独立思考再看提示。

### 基础题

**Q1.** daemonize 中如果把 `chdir("/")` 删掉，最直接的危害是什么？
> 提示：想想守护进程的工作目录如果在 /mnt/sdcard 上，SD 卡还能卸载吗？

**Q2.** `guard_child_start` 里子进程分支为什么必须用 `_exit(1)` 而不能用 `exit(1)`？
> 提示：exit 会做哪两件 _exit 不做的事？在 fork 后的子进程里做它们有什么副作用？

**Q3.** 主循环中为什么用 `while ((pid = waitpid(-1, &status, WNOHANG)) > 0)` 的循环，而不是只调一次 waitpid？
> 提示：如果两个子进程在同一轮 poll 期间同时退出呢？

### 进阶题

**Q4.** 如果把 `SIGCHLD` 设为 `SIG_IGN`，子进程退出后还会变僵尸吗？这样做的代价是什么？
> 提示：查资料确认 SIG_IGN 下内核的行为，以及 waitpid 还能不能拿到退出码。本模块为什么需要退出码？

**Q5.** 滑动窗口限速用的是"固定窗口"（从 first_restart 起算 60 秒），而不是真正的"滑动"。思考一种边界情况：进程在窗口最后 1 秒内崩溃 5 次触发熔断，与真正的滑动窗口相比行为有何差异？如果要改成真滑动窗口，数据结构要怎么调整？
> 提示：可以用环形数组记录每次重启的时间戳。

**Q6.** `handle_sockets` 中，为什么解帧要用 `while (protocol_unpack(...) == 0)` 循环而不是只解一帧？如果 UI 一次发来 3 个完整帧 + 半个帧，ringbuffer 解帧后剩下什么？
> 提示：流式 socket 的粘包/半包特性。

### 挑战题

**Q7.** 目前 `update_status` 用 `static` 局部变量保存上次 CPU 采样。如果将来要支持同时采集两个 CPU 核心（或两台机器），这个设计要怎么重构？
> 提示：把 static 状态收进结构体，作为函数参数传入。

**Q8.** 设计实验验证"重启风暴熔断"：写一个必崩的子进程（启动即 `_exit(1)`），挂到 guard 下，观察日志验证 60 秒内最多重启 5 次后进入 PROC_ERROR。需要修改哪些地方？
> 提示：临时改 `build_child_table` 的 bin_path 指向测试程序；用 `-f` 前台模式跑方便观察。

**Q9.** 当前架构只支持一个 UI 客户端。如果要支持多个并发客户端，`client_fd` 单值、poll 数组、"拒绝新连接"逻辑分别要怎么改？
> 提示：fd 数组化 + 每客户端独立的 ringbuffer；评估一下消息广播的需求。

---

## 9. 可扩展方向

学完本模块后，以下方向可以动手实践（按推荐顺序）：

1. **配置文件化子进程列表**：头文件已预留 `guard_config_load()` 接口，当前子进程表是硬编码的。实现从 `/etc/car_terminal/guard.conf` 读取进程定义（ini 格式，README 里给了示例键）。
2. **告警动作落地**：常量 `GUARD_TEMP_ALERT_C (80℃)` 和 `GUARD_STORAGE_ALERT_MB (100MB)` 已定义，但代码里还没有对应的告警动作。可在 `update_status` 后加判断：超温→记录并主动上报；磁盘不足→清理旧日志。
3. **手动重启命令**：README 提到 `MSG_SERVICE_RESTART (0x02)` 消息（UI 指定重启某进程），但 `handle_sockets` 目前只处理了 `MSG_SERVICE_STATUS`。补上这个消息的分发逻辑是很好的练习。
4. **进程状态查询**：目前 IPC 只返回系统状态，可扩展返回子进程表（每个进程的 PID/状态/重启次数），让 UI 能显示服务健康面板。
5. **升级重启策略**：把固定窗口改为指数退避（第 1 次等 1 秒、第 2 次等 2 秒、第 3 次等 4 秒……），对"偶发故障"更友好。

---

## 附：阅读提醒（文档与代码的差异）

学习时注意对照源码而非只看 README，当前 `README.md` 与实际代码有几处不一致（这本身就是很好的教训——文档会过期，代码才是事实）：

| README 的说法 | 代码的实际实现 |
|---|---|
| 7 个子进程（含 sensor/net） | 实际 5 个（gps/dvr/av/input/canbus），net/sensor 是预留槽位（`GUARD_MAX_CHILDREN=8`） |
| epoll + signalfd | 实际用 poll；SIGCHLD 为默认处理 + 主循环 waitpid |
| `handle_sigchld()` / `read_cpu_percent()` 等函数名 | 实际为 `update_status()` / `handle_sockets()`，无独立 SIGCHLD 处理函数 |
| 配置文件加载 | 实际为 `build_child_table()` 硬编码，`guard_config_load()` 是预留接口 |
| 测试期望 7 个子进程 | 与代码的 5 个不匹配，`make test` 第一条断言会失败 |

以 `guard_daemon.h/.c` 源码和本文档为准。

---

*本文档基于源码 `src/guard_daemon/` 实际代码编写。建议配合源码阅读：先通读头文件注释建立全局观，再按本文档第 5 章顺序逐函数精读，最后完成第 8 章自测题。*
