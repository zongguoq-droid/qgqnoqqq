# input_daemon 模块学习文档

> 按键输入管理守护进程 —— 从架构到代码的完整解析

---

## 目录

1. [模块定位与概述](#1-模块定位与概述)
2. [整体架构设计](#2-整体架构设计)
3. [文件结构与职责划分](#3-文件结构与职责划分)
4. [核心数据结构](#4-核心数据结构)
5. [初始化流程](#5-初始化流程)
6. [主事件循环（poll 模式）](#6-主事件循环poll-模式)
7. [按键手势状态机详解](#7-按键手势状态机详解)
8. [软件消抖机制](#8-软件消抖机制)
9. [LED 闪烁反馈](#9-led-闪烁反馈)
10. [UNIX Domain Socket IPC 通信](#10-unix-domain-socket-ipc-通信)
11. [配置管理](#11-配置管理)
12. [编译与测试](#12-编译与测试)
13. [涉及的 Linux 知识点汇总](#13-涉及的-linux-知识点汇总)
14. [代码设计亮点与改进建议](#14-代码设计亮点与改进建议)

---

## 1. 模块定位与概述

### 1.1 在系统中的位置

本项目是一个**智能车载终端**系统，采用**微服务风格的多进程架构**。整个 `src/` 目录下有多个独立的守护进程（daemon）：

```
src/
├── guard_daemon      # 父进程，统一管理所有子进程生命周期
├── gps_daemon        # GPS 定位
├── dvr_daemon        # 行车录像
├── av_daemon         # 音视频播放
├── canbus_daemon     # CAN 总线通信
├── input_daemon      # ← 本模块：按键输入管理
└── common/           # 公共库（日志、配置、Socket、协议、环形缓冲区）
```

各进程之间通过 **UNIX Domain Socket** 进行 IPC 通信，消息格式由 `protocol` 模块统一定义。

### 1.2 本模块的核心职责

`input_daemon` 是车载终端的**输入子系统核心**，负责以下 5 件事：

| 序号 | 职责 | 说明 |
|------|------|------|
| 1 | 监听物理按键 | 读取 `/dev/input/eventX` 的原始按键事件 |
| 2 | 手势识别 | 通过状态机识别**短按 / 长按 / 双击**三种手势 |
| 3 | LED 反馈 | 短按时驱动用户 LED 闪烁 150ms（非 toggle） |
| 4 | 事件上报 | 通过 Socket 将按键事件推送给 Qt UI 进程 |
| 5 | 接收控制指令 | 接收 Qt 发来的 LED 远程控制指令 |

### 1.3 按键功能映射

| 物理按键 | Linux 键码 | 逻辑 ID | 短按功能 | 长按功能 |
|----------|-----------|---------|---------|---------|
| KEY1 | `KEY_1` (2) | `KEY_ID_KEY1` (1) | 菜单 | 语音助手 |
| KEY2 | `KEY_2` (3) | `KEY_ID_KEY2` (2) | 返回 | 回主页 |

> 注意：键码由设备树（device tree）的 `gpio-keys` 节点决定，代码中通过编译期硬编码的映射表关联。

---

## 2. 整体架构设计

### 2.1 架构总览图

```
┌─────────────────────────────────────────────────────────────────┐
│                        input_daemon 进程                          │
│                                                                   │
│  ┌──────────────┐    ┌──────────────────┐    ┌──────────────┐  │
│  │ /dev/input/  │───▶│  key_parser      │───▶│  event_queue │  │
│  │ eventX       │    │  (状态机+消抖)    │    │  (环形队列)  │  │
│  └──────────────┘    └──────────────────┘    └──────┬───────┘  │
│                                                        │          │
│  ┌──────────────┐                              ┌──────▼───────┐  │
│  │  LED 驱动     │◀───────────────────────────│  主循环        │  │
│  │ /dev/100ask_ │    短按 → led_on + 计时     │  (poll 50ms)  │  │
│  │ led          │◀─── Qt指令 → led_on/off     │               │  │
│  └──────────────┘                              └──────┬───────┘  │
│                                                        │          │
│  ┌──────────────┐    ┌──────────────────┐    ┌──────▼───────┐  │
│  │  Qt UI 进程   │◀───│  MSG_KEY_EVENT   │◀───│  Socket 发送  │  │
│  │              │───▶│  MSG_LED_CONTROL │───▶│  Socket 接收  │  │
│  └──────────────┘    └──────────────────┘    └──────────────┘  │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 设计要点

1. **单线程 + poll() 多路复用**：同时监听 input 设备 fd、Socket 服务端 fd、Socket 客户端 fd，无需多线程，避免竞态条件。
2. **50ms 定时驱动**：`poll()` 超时设为 50ms，保证即使没有事件也能周期性检查 LED 闪烁超时。
3. **秒级 tick**：通过 `time(NULL)` 的秒级变化触发状态机 tick（每秒一次），用于长按判定和双击窗口过期检测。
4. **单客户端模式**：只允许一个 Qt UI 连接，新连接会被拒绝（典型的一对一 IPC）。
5. **生产者-消费者模型**：`key_parser_feed()` 是事件生产者（喂入原始事件），`key_parser_get_event()` 是消费者（取出已识别手势），中间通过环形队列解耦。

### 2.3 为什么用 poll() 而不是 epoll()？

| 对比项 | poll() | epoll() |
|--------|--------|---------|
| 监听 fd 数量 | 本场景 ≤ 3 个 | 适合大量 fd（>100） |
| API 复杂度 | 简单，一个函数调用 | 需要 create/ctl/wait 三步 |
| 性能 | fd 少时无差异 | fd 多时 O(1) 优势明显 |

**结论**：本模块最多监听 3 个 fd，`poll()` 更简单直接，完全够用。

---

## 3. 文件结构与职责划分

```
input_daemon/
├── input_daemon.h     # 配置宏、数据结构、状态机枚举、函数声明
├── input_daemon.c     # 主程序：main()、poll 循环、Socket 处理、LED 反馈
├── key_parser.h        # 按键解析器头文件：API 声明、parser 上下文结构
├── key_parser.c        # 手势状态机实现：消抖、短按/长按/双击识别
├── led_control.h       # LED 控制头文件
├── led_control.c       # LED 控制实现：通过 /dev/100ask_led 字符设备
├── Makefile            # x86/ARM 双编译支持、单元测试
├── README.md           # 简要说明
└── test/
    └── test_input.c    # 6 组单元测试（纯软件模拟，无需硬件）
```

### 各文件职责一句话总结

| 文件 | 一句话职责 |
|------|-----------|
| `input_daemon.h` | 定义"是什么"——配置参数、数据结构、状态枚举 |
| `input_daemon.c` | 定义"怎么跑"——进程生命周期、事件循环、外设协调 |
| `key_parser.h` | 定义"解析器对外接口"——init/feed/tick/get_event |
| `key_parser.c` | 定义"手势怎么识别"——状态机核心算法 |
| `led_control.c` | 定义"LED 怎么开关"——封装字符设备 write 操作 |
| `test_input.c` | 验证"手势识别对不对"——模拟事件序列注入测试 |

---

## 4. 核心数据结构

### 4.1 全局上下文 `input_ctx_t`

这是整个守护进程的"大脑"，所有运行时状态集中管理：

```c
typedef struct {
    input_config_t      cfg;            /* 配置：设备路径、时间阈值等 */
    key_parser_t        parser;         /* 按键状态机（手势识别核心） */
    int                 input_fd;       /* input 设备 fd */
    int                 server_fd;      /* Socket 监听 fd */
    int                 client_fd;      /* Qt UI 连接 fd（-1=无连接） */
    protocol_parser_t   proto_parser;   /* 协议解析器（从字节流提取帧） */
    ringbuffer_t        recv_rb;        /* 接收环形缓冲区 */
    time_t              last_tick;      /* 上次 tick 时刻（秒级） */
    int                 led_blinking;   /* LED 闪烁状态：1=亮等待熄灭 */
    struct timeval      led_on_time;    /* LED 亮起时刻（μs 精度） */
} input_ctx_t;
```

**设计意图**：
- 所有状态集中在一个全局变量 `g_ctx` 中，避免散落的全局变量难以追踪。
- `client_fd = -1` 表示无连接，这是 Unix 编程的惯例（有效 fd 从 0 开始）。

### 4.2 按键状态槽 `key_slot_t`

每个物理按键独立维护一个状态槽，支持最多 `INPUT_MAX_KEYS`（8）个按键：

```c
typedef struct {
    int             linux_code;     /* Linux 键码（如 KEY_1=2） */
    key_id_t        key_id;         /* 逻辑按键 ID（1=KEY1, 2=KEY2） */
    key_state_t     state;          /* 当前状态机状态 */
    struct timeval  press_time;     /* 按下时刻（计算持续时间） */
    struct timeval  release_time;   /* 释放时刻（双击窗口判断） */
    uint32_t        duration_ms;    /* 最近一次按下的持续时间 */
    int             event_pending;  /* 有待发送事件（0/1） */
    uint8_t         pending_type;   /* 待发送事件类型 */
} key_slot_t;
```

**关键设计**：
- 使用**内核时间戳** `ev->time`（而非 `gettimeofday`）记录按下/释放时刻，因为内核时间戳更精确，且不受应用层调度延迟影响。
- 每个按键独立状态，互不干扰——KEY1 长按的同时 KEY2 可以短按。

### 4.3 按键解析器 `key_parser_t`

```c
typedef struct {
    key_slot_t      slots[INPUT_MAX_KEYS];  /* 按键状态槽数组 */
    int             slot_count;              /* 已激活的槽数 */
    key_mapping_t  *key_map;                 /* 按键映射表（外部内存） */
    int             map_count;               /* 映射表条目数 */
    int             long_press_ms;           /* 长按阈值（ms） */
    int             double_click_ms;         /* 双击窗口（ms） */
    int             debounce_ms;             /* 消抖时间（ms） */

    /* 事件环形队列（生产者=feed，消费者=get_event） */
    key_event_t     event_queue[16];
    int             event_head;    /* 写入位置 */
    int             event_tail;    /* 读取位置 */
    int             event_count;   /* 队列中事件数 */
} key_parser_t;
```

### 4.4 上报事件 `key_event_t`（定义在 common.h）

这是通过 Socket 发送给 Qt UI 的数据结构，使用 `#pragma pack(1)` 保证 1 字节对齐：

```c
#pragma pack(push, 1)
typedef struct {
    uint8_t     key_id;         /* 按键标识：1=KEY1, 2=KEY2 */
    uint8_t     event_type;     /* 事件类型：1=短按, 2=长按, 3=双击, 4=释放 */
    uint32_t    duration_ms;    /* 按下持续时间（ms） */
    time_t      timestamp;      /* 事件时间戳 */
} key_event_t;
#pragma pack(pop)
```

> **为什么要 `#pragma pack(1)`？** 因为这个结构体会通过原始字节流在不同进程（甚至不同语言：C 发送、C++/Qt 接收）之间传输。如果不强制 1 字节对齐，不同编译器可能在字段间插入填充字节，导致接收端字段错位。

---

## 5. 初始化流程

`main()` 函数的初始化阶段按以下顺序执行，每一步都有明确的依赖关系：

```
main() 启动
  │
  ├─ 1. 初始化全局上下文 g_ctx（memset 清零，client_fd = -1）
  │
  ├─ 2. 初始化日志系统 log_init()
  │     └─ 日志路径: /var/log/car_terminal/input.log
  │
  ├─ 3. 加载配置 input_config_load()
  │     ├─ 尝试读取 config.ini [input] 节
  │     ├─ 失败则使用硬编码默认值
  │     └─ 按键映射表使用编译期 g_key_map（不从配置读取）
  │
  ├─ 4. 初始化 LED 控制 led_init()
  │     └─ 打开 /dev/100ask_led 字符设备
  │
  ├─ 5. 打开 input 设备 open(O_RDONLY | O_NONBLOCK)
  │     └─ 非阻塞模式：read() 不会阻塞等待事件
  │
  ├─ 6. 初始化按键状态机 key_parser_init()
  │     └─ 绑定按键映射表 + 时间阈值（长按/双击/消抖）
  │
  ├─ 7. 创建 UNIX Socket 服务端 sock_create_server()
  │     └─ 监听 /tmp/car_input.sock
  │
  ├─ 8. 初始化协议解析器 + 接收环形缓冲区
  │
  ├─ 9. 注册信号处理 signal(SIGTERM/SIGINT, sig_handler)
  │     └─ 收到信号时设置 g_running = 0，优雅退出
  │
  └─ 10. 记录 last_tick = time(NULL)，进入主循环
```

### 关键细节

**为什么 input 设备要用 `O_NONBLOCK`？**

因为在 `poll()` 返回 `POLLIN` 后，我们用 `while(read() == sizeof(ev))` 循环读取所有可用事件。如果是非阻塞模式，读完后 `read()` 返回 `-1`（`errno = EAGAIN`），循环自然结束。如果是阻塞模式，读完后 `read()` 会挂起等待下一个事件，导致主循环卡死。

---

## 6. 主事件循环（poll 模式）

主循环是整个模块的核心，每次迭代约 50ms，分为 4 个步骤：

### 6.1 循环伪代码

```c
while (g_running) {
    /* 构建 poll fd 数组 */
    pfds[0] = { input_fd,  POLLIN };   /* 物理按键 */
    pfds[1] = { server_fd, POLLIN };   /* Qt 连接请求 */
    if (client_fd >= 0)
        pfds[2] = { client_fd, POLLIN };  /* Qt 数据 */

    poll(pfds, nfds, 50);  /* 等待事件，最多 50ms */

    /* Step 1: 读取 input 事件，喂入状态机 */
    if (pfds[0].revents & POLLIN) {
        while (read(input_fd, &ev, sizeof(ev)) == sizeof(ev))
            key_parser_feed(&parser, &ev);
    }

    /* Step 2: 每秒驱动一次状态机 tick，取出已识别手势 */
    if (time(NULL) != last_tick) {
        key_parser_tick(&parser);
        last_tick = time(NULL);
        while (key_parser_get_event(&parser, &kev) == 0) {
            if (kev.event_type == 1) {  /* 短按 */
                led_on(0);
                gettimeofday(&led_on_time, NULL);
                led_blinking = 1;
            }
            if (client_fd >= 0)
                sock_send_frame(client_fd, MSG_KEY_EVENT, &kev, sizeof(kev));
        }
    }

    /* Step 3: 处理 Socket 事件（accept / recv 指令） */
    handle_sockets(&g_ctx, pfds[1].revents, ...);

    /* Step 4: LED 闪烁超时检查 */
    if (led_blinking) {
        elapsed_ms = (now - led_on_time) 换算为 ms;
        if (elapsed_ms >= 150) {
            led_off(0);
            led_blinking = 0;
        }
    }
}
```

### 6.2 为什么 tick 是每秒一次而不是每次循环？

状态机的 tick 用于检查**长按超时**（默认 2000ms）和**双击窗口过期**（默认 500ms）。这些时间阈值都是秒级或百毫秒级，不需要每 50ms 就检查一次。

通过 `time(NULL)` 的秒级变化来触发 tick，可以：
- 减少不必要的计算（每秒只遍历一次所有按键槽）
- 简化逻辑（不需要额外的定时器 fd）

> **注意**：这意味着双击窗口的实际判定精度是**秒级**的。如果用户在第 0.9 秒释放按键，双击窗口 500ms 会在第 1.0 秒的 tick 中被判定为过期（实际只过了 100ms）。这是一个精度 trade-off，对于车载按键场景可以接受。

### 6.3 一次 poll 可能对应多个 input_event

内核的 input 子系统会将多个事件缓冲在设备的读取队列中。一次 `poll()` 返回 `POLLIN` 后，可能有多个 `input_event` 可读。因此必须用 `while` 循环全部读完，而不是只读一个：

```c
while (read(input_fd, &ev, sizeof(ev)) == sizeof(ev)) {
    key_parser_feed(&parser, &ev);
}
```

非阻塞模式下，读完所有事件后 `read()` 返回 `-1`（`EAGAIN`），循环结束。

---

## 7. 按键手势状态机详解

这是整个模块**最核心、最有学习价值**的部分。

### 7.1 状态定义

```c
typedef enum {
    KEY_STATE_IDLE        = 0,  /* 空闲，等待按下 */
    KEY_STATE_PRESSED     = 1,  /* 已按下，等待释放或长按超时 */
    KEY_STATE_LONG_PRESS  = 2,  /* 长按已触发，等待释放 */
    KEY_STATE_WAIT_DOUBLE = 3,  /* 已释放，等待双击窗口内二次按下 */
    KEY_STATE_PRESSED_2ND = 4,  /* 第二次按下，等待释放完成双击 */
} key_state_t;
```

### 7.2 完整状态转换图

```
                         ┌──────────────────────────────────┐
                         │            IDLE                   │
                         └──────┬──────────────┬────────────┘
                          press │              │ tick: 双击窗口过期
                        (value=1)│              │ (release_time + 500ms)
                                ▼              │
                         ┌──────────┐          │
                         │ PRESSED  │          │
                         └─┬────┬──┘          │
              tick: 按住  │    │ release       │
              >2000ms     │    ▼ (value=0)    │
                          │  ┌─────────────┐  │
                          │  │ WAIT_DOUBLE │──┘
                          │  └──┬──────────┘
                          │     │ press 在 500ms 窗口内
                          │     ▼
                          │  ┌─────────────┐
                          │  │ PRESSED_2ND │
                          │  └──┬──────────┘
                          │     │ release
                          │     ▼
                          │  ┌─────────────┐
                          │  │DOUBLE_CLICK │ → 上报事件，回到 IDLE
                          │  └─────────────┘
                          ▼
                       ┌─────────────┐
                       │ LONG_PRESS  │ → 上报事件，等待释放后回 IDLE
                       └─────────────┘
```

### 7.3 三种手势的完整识别过程

#### 7.3.1 短按（Short Press）

**定义**：按下后在 2000ms 内释放，且释放后 500ms 内没有二次按下。

**时间线**：

```
t=0ms      t=100ms                  t=600ms
  │          │                          │
  ▼          ▼                          ▼
按下       释放                    tick 检测到
IDLE→PRESSED  PRESSED→WAIT_DOUBLE   双击窗口过期
                                   WAIT_DOUBLE→IDLE
                                   上报 SHORT_PRESS
```

**代码路径**：

1. `key_parser_feed()` 收到 `value=1`（按下）：`IDLE → PRESSED`，记录 `press_time`
2. `key_parser_feed()` 收到 `value=0`（释放）：计算 `duration_ms`，大于消抖阈值且小于长按阈值 → `PRESSED → WAIT_DOUBLE`，记录 `release_time`
3. `key_parser_tick()` 每秒检查：`WAIT_DOUBLE` 状态下，如果 `now - release_time >= 500ms` → 上报 `SHORT_PRESS`，回到 `IDLE`

> **关键理解**：短按不是在释放时立即上报的，而是要等双击窗口（500ms）过期后才能确认。因为如果在窗口内有第二次按下，那就是双击而不是短按。

#### 7.3.2 长按（Long Press）

**定义**：按下后持续超过 2000ms 未释放。

**时间线**：

```
t=0ms                 t=2000ms               t=2500ms
  │                     │                       │
  ▼                     ▼                       ▼
按下                 tick 检测到              释放
IDLE→PRESSED        按住>2000ms            LONG_PRESS→IDLE
                    PRESSED→LONG_PRESS
                    上报 LONG_PRESS
```

**代码路径**：

1. `key_parser_feed()` 收到 `value=1`：`IDLE → PRESSED`，记录 `press_time`
2. `key_parser_tick()` 每秒检查：`PRESSED` 状态下，如果 `now - press_time >= 2000ms` → 上报 `LONG_PRESS`，进入 `LONG_PRESS` 状态
3. `key_parser_feed()` 收到 `value=0`（释放）：`LONG_PRESS → IDLE`

> **关键理解**：长按在**按住达到阈值时就立即上报**，不需要等释放。这是因为长按的语义是"按住不放触发"，比如语音助手通常是按住说话。上报后进入 `LONG_PRESS` 状态只是为了等待释放后回到 IDLE，期间忽略重复事件。

#### 7.3.3 双击（Double Click）

**定义**：第一次短按释放后，在 500ms 内再次按下并释放。

**时间线**：

```
t=0ms    t=100ms   t=300ms   t=400ms
  │        │         │         │
  ▼        ▼         ▼         ▼
按下1    释放1      按下2     释放2
IDLE→   PRESSED→   WAIT_     PRESSED_
PRESSED  WAIT_     DOUBLE→   2ND→IDLE
         DOUBLE    PRESSED_  上报
                   2ND       DOUBLE_CLICK
```

**代码路径**：

1. 第一次按下+释放：同短按的前两步，进入 `WAIT_DOUBLE`
2. `key_parser_feed()` 收到第二次 `value=1`：在 `WAIT_DOUBLE` 状态下，检查 `ev->time - release_time <= 500ms` → `WAIT_DOUBLE → PRESSED_2ND`
3. `key_parser_feed()` 收到第二次 `value=0`：`PRESSED_2ND → IDLE`，上报 `DOUBLE_CLICK`

> **关键理解**：双击是在**第二次释放时**立即上报的，不需要等任何窗口。因为第二次释放本身就标志着双击手势的完成。

### 7.4 状态机驱动的两个入口

状态机由两个函数驱动，分别处理**事件驱动**和**时间驱动**：

| 函数 | 触发方式 | 处理内容 |
|------|---------|---------|
| `key_parser_feed()` | 事件驱动（有 input_event 时） | 按下/释放事件导致的状态转换 |
| `key_parser_tick()` | 时间驱动（每秒一次） | 超时导致的状态转换（长按、双击窗口过期） |

这种"事件 + 时间"双驱动模式是状态机的经典设计，几乎所有嵌入式输入处理都采用类似模式。

### 7.5 自动重复事件的处理

Linux 内核在按键持续按住时会发送 `value=2` 的自动重复事件（类似键盘按住不放时连续输入字符）。本模块**直接忽略** `value=2`：

```c
if (ev->value == 2) return 0;  /* 忽略自动重复 */
```

因为手势识别只需要**首次按下**（`value=1`）和**释放**（`value=0`）两个时刻，中间的重复事件没有意义。

---

## 8. 软件消抖机制

### 8.1 为什么需要消抖？

机械按键在按下和释放的瞬间，金属触点会发生**弹跳（bounce）**，导致电平在短时间内多次跳变。如果不处理，一次按下可能被识别为多次按下。

虽然 Linux input 子系统的驱动层通常已经做了硬件消抖，但本模块额外做了一层**软件消抖**作为双重保护。

### 8.2 消抖策略

**在释放时检查持续时间**：如果按键按下的持续时间小于 `debounce_ms`（默认 30ms），视为抖动，直接丢弃，回到 IDLE：

```c
/* key_parser.c, PRESSED 状态下收到释放 */
slot->duration_ms = timeval_diff_ms(&ev->time, &slot->press_time);

if (slot->duration_ms < parser->debounce_ms) {
    /* 持续时间太短，视为抖动，直接回到 IDLE，不上报任何事件 */
    slot->state = KEY_STATE_IDLE;
    break;
}
```

### 8.3 消抖阈值的选择

| 阈值 | 效果 |
|------|------|
| 太小（<10ms） | 可能无法过滤掉真实的机械抖动 |
| 太大（>100ms） | 可能误杀快速的真实按键操作 |
| 30ms（本模块默认） | 经验值，既能过滤抖动，又不影响正常操作 |

---

## 9. LED 闪烁反馈

### 9.1 设计目标

短按按键时，用户 LED 亮起 150ms 后自动熄灭，提供**触觉之外的视觉反馈**。

### 9.2 为什么是"闪烁一次"而不是"toggle"？

最初的设计是每次短按 toggle 一次 LED（亮→灭→亮→灭），但存在语义不明确的问题：用户不知道当前 LED 状态，也不知道这次按键是亮还是灭。

改为"闪烁一次"后：**每次短按都对应一次明确的亮→灭过程**，用户总能看到反馈，语义清晰。

### 9.3 实现机制

```
短按事件产生
  │
  ├─ led_on(0)           → 立即点亮 LED
  ├─ gettimeofday()      → 记录亮起时刻 led_on_time
  └─ led_blinking = 1    → 标记"正在闪烁"

每次 poll 循环末尾（最多 50ms 后）：
  │
  └─ if (led_blinking):
       elapsed_ms = now - led_on_time
       if (elapsed_ms >= 150):
         led_off(0)
         led_blinking = 0
```

**关键点**：
- 使用 `gettimeofday()` 微秒级精度计时（而非 `time()` 秒级），因为 150ms 小于 1 秒。
- 超时检查放在 poll 循环末尾，利用 50ms 的 poll 超时保证检查频率足够（误差 ≤ 50ms）。
- LED 亮起和熄灭是**异步**的：亮起在事件处理中立即执行，熄灭在后续循环中由超时检查触发。

### 9.4 LED 硬件控制

`led_control.c` 通过 `/dev/100ask_led` 字符设备控制 LED：

```c
int led_on(int which) {
    char buf[2];
    buf[0] = (char)which;   /* LED 编号 */
    buf[1] = 0;              /* 0 = 亮 */
    write(g_fd, buf, 2);
}
```

写入 2 字节：第 1 字节是 LED 编号，第 2 字节是状态（0=亮，1=灭）。

---

## 10. UNIX Domain Socket IPC 通信

### 10.1 通信模型

```
┌─────────────────┐                    ┌─────────────────┐
│  input_daemon   │                    │     Qt UI       │
│                 │                    │                 │
│  server_fd ◀────┼── accept() ───────┼── connect()     │
│                 │                    │                 │
│  client_fd ◀────┼── 双向通信 ────────┼── socket_fd     │
│                 │                    │                 │
└─────────────────┘                    └─────────────────┘
       /tmp/car_input.sock
```

- `input_daemon` 是**服务端**，创建监听 Socket，等待 Qt 连接。
- Qt UI 是**客户端**，主动连接。
- 连接建立后是**全双工**的，双方都可以发送和接收。

### 10.2 消息类型

| 方向 | 消息类型 | 值 | 说明 |
|------|---------|-----|------|
| input → UI | `MSG_KEY_EVENT` | 0x70 | 按键手势通知，载体 `key_event_t` |
| UI → input | `MSG_LED_CONTROL` | 0x71 | 远程 LED 控制，d[0]=1 亮，d[0]=0 灭 |

消息类型采用**高 4 位模块 ID + 低 4 位消息 ID** 的编码方案：
- `0x70`：模块 ID = 0x7（INPUT），消息 ID = 0x0（KEY_EVENT）
- `0x71`：模块 ID = 0x7（INPUT），消息 ID = 0x1（LED_CONTROL）

### 10.3 接收处理流程

Qt 发来的数据是**原始字节流**，需要经过以下步骤才能提取出完整的协议帧：

```
recv() 读取原始字节
  │
  ▼
写入环形缓冲区 recv_rb
  │
  ▼
protocol_unpack() 循环解包
  │
  ├─ 帧不完整 → 等待更多数据
  │
  └─ 帧完整 → 提取 msg_type + data
       │
       └─ msg_type == MSG_LED_CONTROL:
            d[0] == 1 → led_on(0)
            d[0] == 0 → led_off(0)
```

**为什么需要环形缓冲区？** 因为 TCP/Unix Socket 是**流协议**，一次 `recv()` 可能收到半帧、一帧或多帧数据。环形缓冲区暂存原始字节，协议解析器从中逐帧提取，解决粘包和半包问题。

### 10.4 单客户端策略

```c
if (srv_ev & POLLIN) {
    int fd = sock_accept(ctx->server_fd);
    if (fd >= 0) {
        if (ctx->client_fd >= 0)
            sock_close(fd, NULL);  /* 已有连接，拒绝新连接 */
        else {
            ctx->client_fd = fd;
            protocol_reset(&ctx->proto_parser, &ctx->recv_rb);
        }
    }
}
```

如果已有 Qt 连接（`client_fd >= 0`），新的连接请求会被**立即关闭**。这保证了一对一的通信模型，避免多个 UI 实例同时控制 LED 造成混乱。

### 10.5 断开检测

```c
int n = sock_recv(ctx->client_fd, buf, sizeof(buf));
if (n <= 0) {
    /* n=0: 对端正常关闭；n=-1: 错误 */
    sock_close(ctx->client_fd, NULL);
    ctx->client_fd = -1;
}
```

当 Qt 进程退出或网络异常时，`recv()` 返回 0（正常关闭）或 -1（错误），此时关闭连接并重置 `client_fd = -1`，后续可以接受新的连接。

---

## 11. 配置管理

### 11.1 配置来源优先级

```
config.ini [input] 节
  │ 存在且可读
  ▼
使用配置文件中的值
  │
  │ 不存在 / 读取失败
  ▼
使用硬编码默认值
```

### 11.2 可配置项

| 配置项 | config.ini 键 | 默认值 | 说明 |
|--------|---------------|--------|------|
| 输入设备路径 | `device` | `/dev/input/event2` | gpio-keys 设备节点 |
| 长按判定时间 | `long_press_ms` | 2000 | 按住超过此时间判定为长按 |
| 双击窗口时间 | `double_click_ms` | 500 | 两次按下间隔不超过此时间判定为双击 |
| 消抖时间 | — | 30 | **硬编码，不开放配置** |
| Socket 路径 | — | `/tmp/car_input.sock` | **硬编码，不开放配置** |

### 11.3 按键映射为什么不配置化？

按键映射表（Linux 键码 → 逻辑 ID）使用**编译期硬编码**：

```c
static key_mapping_t g_key_map[] = {
    { KEY_1, KEY_ID_KEY1, "KEY1" },
    { KEY_2, KEY_ID_KEY2, "KEY2" },
};
```

**原因**：Linux 键码由**设备树（device tree）**的 `gpio-keys` 节点决定，是硬件层面的固定配置。修改键码需要修改设备树并重新编译内核，因此在应用层配置化没有实际意义。

### 11.4 config.ini 示例

```ini
[input]
device = /dev/input/event2
long_press_ms = 2000
double_click_ms = 500
```

---

## 12. 编译与测试

### 12.1 Makefile 目标

| 命令 | 说明 |
|------|------|
| `make` | ARM 交叉编译（默认） |
| `make HOST=1` | x86 本地编译（测试用，无真实 input 设备） |
| `make test` | 编译并运行单元测试 |
| `make clean` | 清理编译产物 |

### 12.2 交叉编译

```bash
# 使用默认交叉编译器
make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-

# 产物: build/input_daemon
# 复制到开发板: ../../build/bin/input_daemon
```

### 12.3 单元测试

测试文件 `test/test_input.c` 包含 **6 组测试**，全部是**纯软件模拟**，不需要真实按键硬件：

| 测试 | 内容 | 验证点 |
|------|------|--------|
| test_mapping | 按键映射查找 | KEY_F1→KEY1, KEY_F2→KEY2, 未映射→NONE |
| test_short_press | 短按手势 | 按下100ms释放→等600ms tick→取出SHORT事件 |
| test_long_press | 长按手势 | 按下→等2100ms tick→取出LONG事件 |
| test_debounce | 消抖过滤 | 按下10ms释放→被过滤，无事件 |
| test_event_queue | 事件队列FIFO | 两个短按→按顺序取出，队列空 |
| test_unmapped_key | 未映射按键忽略 | KEY_ENTER按下释放→无事件 |

**测试原理**：通过构造 `struct input_event` 并手动设置时间戳，模拟真实的按键事件序列，注入 `key_parser_feed()`，然后用 `usleep()` 等待真实时间流逝后调用 `key_parser_tick()`，最后用 `key_parser_get_event()` 验证结果。

运行测试：

```bash
make test
```

### 12.4 运行守护进程

```bash
# 需要 root 权限（读取 /dev/input/eventX 和写入 LED 设备）
sudo ./input_daemon /etc/car_terminal/config.ini

# 不带参数时默认使用 ../config/config.ini
sudo ./input_daemon
```

---

## 13. 涉及的 Linux 知识点汇总

| 知识点 | 说明 | 对应代码位置 |
|--------|------|-------------|
| **input 子系统** | `/dev/input/eventX`，`struct input_event`，统一的输入设备抽象 | `input_daemon.c` open/read |
| **poll() 多路复用** | 同时监听多个 fd，超时返回，替代多线程 | `input_daemon.c` 主循环 |
| **非阻塞 IO** | `O_NONBLOCK`，read() 不阻塞 | `input_daemon.c` open() |
| **有限状态机（FSM）** | 手势识别的经典实现，事件+时间双驱动 | `key_parser.c` |
| **软件消抖** | 过滤机械按键抖动，嵌入式输入必备 | `key_parser.c` release 处理 |
| **struct timeval** | 微秒级时间戳，`timersub()` 宏计算时间差 | `key_parser.c` timeval_diff_ms() |
| **gettimeofday()** | 获取当前时间（VDSO 加速，不陷入内核） | `key_parser.c` / `input_daemon.c` |
| **UNIX Domain Socket** | 进程间通信，`SOCK_STREAM`，文件系统路径 | `input_daemon.c` handle_sockets() |
| **环形缓冲区** | 生产者-消费者解耦，解决粘包/半包 | `key_parser.c` event_queue / `input_daemon.c` recv_rb |
| **信号处理** | `SIGTERM`/`SIGINT` 优雅退出，`volatile` 变量 | `input_daemon.c` sig_handler() |
| **字符设备驱动** | `/dev/100ask_led`，write() 控制硬件 | `led_control.c` |
| **#pragma pack(1)** | 强制 1 字节对齐，跨进程/跨语言二进制传输 | `common.h` key_event_t |
| **Makefile 交叉编译** | `CROSS_COMPILE` 前缀，x86/ARM 双目标 | `Makefile` |

---

## 14. 代码设计亮点与改进建议

### 14.1 设计亮点

1. **清晰的分层架构**：`input_daemon.c`（进程协调）→ `key_parser.c`（手势识别）→ `led_control.c`（硬件控制），每层职责单一。

2. **状态机与主循环解耦**：`key_parser` 是纯算法模块，不依赖任何 IO，可以独立单元测试。主循环负责 IO 和外设协调。

3. **生产者-消费者模型**：`feed()` 生产事件，`get_event()` 消费事件，中间环形队列缓冲，解耦了事件产生和事件处理的时机。

4. **非阻塞 + poll**：单线程即可处理多个 IO 源，无需锁，无竞态条件。

5. **详细的注释**：头文件中有完整的架构说明、状态转换图、学习要点，非常适合学习。

### 14.2 可改进点

1. **tick 精度问题**：当前通过 `time(NULL)` 秒级变化触发 tick，导致双击窗口（500ms）的实际判定精度是秒级。建议改用 `gettimeofday()` 比较，或使用 `timerfd` 精确定时。

2. **LED 与短按事件耦合**：当前短按事件在主循环中处理时直接触发 LED。如果未来需要支持"长按也闪 LED"或"不同按键闪不同 LED"，建议将 LED 反馈逻辑抽象为独立的事件处理器。

3. **单客户端策略可优化**：当前新连接直接被拒绝。可以考虑"新连接踢旧连接"策略（关闭旧的，接受新的），这样 UI 重启后能自动重连，不需要等旧连接超时。

4. **错误处理不够完善**：`sock_send_frame()` 发送失败时没有处理（如连接断开），可能导致事件丢失。建议在发送失败时关闭连接并重置 `client_fd`。

5. **`input_find_device()` 未使用**：头文件中声明了自动探测 input 设备的函数，但 `main()` 中直接使用默认路径。可以在设备打开失败时 fallback 到自动探测。

6. **日志初始化在配置加载之前**：`log_init()` 使用硬编码的日志路径，而配置文件中可能有自定义路径。建议先加载配置，再用配置中的路径初始化日志。

---

## 附录：快速参考卡

### 手势判定速查

| 手势 | 判定条件 | 上报时机 | event_type |
|------|---------|---------|-----------|
| 短按 | 按下<2s释放，释放后500ms无二次按下 | 双击窗口过期时 | 1 |
| 长按 | 按下持续≥2s | 达到阈值时（不等释放） | 2 |
| 双击 | 两次按下间隔≤500ms | 第二次释放时 | 3 |

### 关键时间参数

| 参数 | 默认值 | 用途 |
|------|--------|------|
| `long_press_ms` | 2000 | 长按判定阈值 |
| `double_click_ms` | 500 | 双击窗口 |
| `debounce_ms` | 30 | 软件消抖 |
| `LED_BLINK_MS` | 150 | LED 闪烁持续时间 |
| poll timeout | 50 | 主循环周期 |
| tick 周期 | 1000 | 状态机超时检查周期 |

### 消息帧格式

```
┌──────┬──────┬────────┬────────┬──────┬──────┐
│ 0xAA │ 0x55 │ type   │ length │ data │ 0x55 │
│ 头0  │ 头1  │ 1字节  │ 2字节  │ N字节│ 尾   │
└──────┴──────┴────────┴────────┴──────┴──────┘
```

- `MSG_KEY_EVENT` (0x70): data = `key_event_t` (10 字节)
- `MSG_LED_CONTROL` (0x71): data[0] = 1(亮) / 0(灭)

---

> 本文档基于 `input_daemon` 模块源码逐行分析生成，覆盖架构设计、使用逻辑、代码实现三个层面。建议按照"概述 → 架构 → 数据结构 → 初始化 → 主循环 → 状态机"的顺序阅读，配合源码对照效果最佳。
