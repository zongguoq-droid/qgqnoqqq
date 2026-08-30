# gps_daemon 模块完整学习文档

> 本文档从架构设计、使用逻辑、代码设计三个维度，循序渐进地讲解 `gps_daemon` —— 一个运行在嵌入式 Linux 上的 GPS 定位守护进程。

---

## 目录

1. [模块概述：它是什么，解决什么问题](#1-模块概述)
2. [快速上手：编译、运行、测试](#2-快速上手)
3. [整体架构设计：三层模型与数据流](#3-整体架构设计)
4. [第一层：串口通信层（uart_util）](#4-第一层串口通信层)
5. [第二层：NMEA 协议解析层（nmea_parser）](#5-第二层nmea-协议解析层)
6. [第三层：业务逻辑层（gps_daemon）](#6-第三层业务逻辑层)
7. [测试设计：如何验证每个环节](#7-测试设计)
8. [关键 Linux 知识点总结](#8-关键-linux-知识点总结)
9. [扩展与改进方向](#9-扩展与改进方向)

---

## 1. 模块概述

### 1.1 它是什么

`gps_daemon` 是一个**单线程、事件驱动**的 GPS 定位守护进程，运行在 i.MX6ULL 嵌入式 Linux 平台上。它通过串口连接 GPS 模块，持续读取 NMEA-0183 协议数据，解析出经纬度、速度、卫星数等定位信息，并通过 UNIX Domain Socket 实时推送给上层 UI 程序。

### 1.2 解决什么问题

在车载终端系统中，UI 程序（通常是 Qt 应用）需要实时显示车辆位置、速度、里程等信息。但：

- **串口是独占资源**：不能让 UI 直接打开串口，否则多个进程会冲突
- **GPS 数据是持续的流**：需要有人一直读、一直解析
- **UI 不应该被串口阻塞**：UI 需要保持响应

因此，把 GPS 数据采集和解析独立成一个守护进程（daemon），通过 IPC（UNIX Socket）向 UI 推送数据，是典型的解耦设计。

### 1.3 核心功能清单

| 功能      | 说明                                          |
| ------- | ------------------------------------------- |
| 串口通信    | termios 配置，9600bps / 8N1 / 非阻塞              |
| NMEA 解析 | 字符级状态机，支持 GGA / RMC / GSV / GLL / VTG / GSA |
| 里程累计    | Haversine 公式计算相邻定位点距离，过滤静止漂移                |
| 轨迹记录    | 环形缓冲保存最近 1 小时的定位点（1Hz × 3600）               |
| 实时推送    | UNIX Socket 每秒推送 `gps_data_t` 给 UI          |
| 定位状态    | 无定位 / 2D / 3D 三级指示                          |
| 优雅退出    | signalfd 捕获 SIGTERM / SIGINT，清理资源后退出        |

### 1.4 硬件连接

```
GPS 模块          i.MX6ULL 开发板
┌─────────┐      ┌──────────────┐
│  TX  ───┼──────┼─ RXD (UART5) │  GPS → 开发板（数据流）
│  RX  ───┼──────┼─ TXD (UART5) │  开发板 → GPS（配置用，通常不用）
│  VCC ───┼──────┼─ 3.3V / 5V   │
│  GND ───┼──────┼─ GND          │
└─────────┘      └──────────────┘

设备节点: /dev/ttymxc5
```

---

## 2. 快速上手

### 2.1 文件结构

```
gps_daemon/
├── gps_daemon.h      # 配置宏、数据结构（gps_context_t, gps_odometer_t）
├── gps_daemon.c      # 主程序（epoll + 里程 + 轨迹 + Socket + 信号）
├── nmea_parser.h     # NMEA 解析器头文件（状态机设计）
├── nmea_parser.c     # NMEA 解析器实现（字符级状态机 + 6 种语句提取）
├── uart_util.h       # 串口工具头文件
├── uart_util.c       # termios 串口配置实现
├── Makefile           # 编译脚本（支持 ARM 交叉编译 / x86 本地编译 / 测试）
├── test/
│   └── test_gps.c     # 10 组单元测试（无需硬件）
└── README.md
```

### 2.2 编译

```bash
# ARM 交叉编译（默认，部署到开发板）
make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-

# x86 本地编译（仅用于测试 NMEA 解析逻辑，不能实际读串口）
make HOST=1

# 编译并运行单元测试（无需 GPS 硬件）
make test

# 清理
make clean
```

编译产物在 `build/` 目录下，最终可执行文件会复制到 `../../build/bin/gps_daemon`。

### 2.3 运行

```bash
# 使用默认配置
./gps_daemon

# 指定配置文件
./gps_daemon /etc/car_terminal/config.ini
```

配置文件格式（INI）：

```ini
[gps]
uart_device = /dev/ttymxc5
baud_rate = 9600
report_interval = 1000   ; 毫秒，数据上报间隔
```

### 2.4 运行时日志

日志输出到 `/var/log/car_terminal/gps.log`，每秒一行：

```
Fix=2 Sats=8 Lat=39.205760 Lon=116.390945 Spd=28.7km/h Alt=50.5m Odo=0.123km
```

各字段含义：

- `Fix`：定位质量（0=无效，1=GPS，2=DGPS，3=3D）
- `Sats`：可见卫星数
- `Lat/Lon`：经纬度（十进制度）
- `Spd`：地面速度（km/h）
- `Alt`：海拔高度（米）
- `Odo`：累计里程（公里）

---

## 3. 整体架构设计

### 3.1 三层模型

`gps_daemon` 采用清晰的**三层架构**，自底向上依次是：

```
┌─────────────────────────────────────────────────────┐
│              第三层：业务逻辑层                       │
│  gps_daemon.c                                        │
│  ┌─────────────────────────────────────────────┐    │
│  │ epoll 事件循环                                │    │
│  │  ├─ 里程累计（Haversine）                    │    │
│  │  ├─ 轨迹记录（环形缓冲）                      │    │
│  │  ├─ UNIX Socket（推送 / 查询 / 状态）        │    │
│  │  ├─ 定时器（每秒上报）                        │    │
│  │  └─ 信号处理（优雅退出）                      │    │
│  └─────────────────────────────────────────────┘    │
├─────────────────────────────────────────────────────┤
│              第二层：协议解析层                       │
│  nmea_parser.c                                       │
│  ┌─────────────────────────────────────────────┐    │
│  │ 字符级状态机（IDLE→TALKER→TYPE→FIELD→...） │    │
│  │  ├─ XOR 校验和验证                           │    │
│  │  ├─ GGA / RMC / GSV / GLL / VTG / GSA 提取 │    │
│  │  └─ 度分格式 → 十进制度转换                  │    │
│  └─────────────────────────────────────────────┘    │
├─────────────────────────────────────────────────────┤
│              第一层：硬件抽象层                       │
│  uart_util.c                                         │
│  ┌─────────────────────────────────────────────┐    │
│  │ termios 串口配置                              │    │
│  │  ├─ 9600bps / 8N1 / 原始模式                │    │
│  │  ├─ 非阻塞（O_NONBLOCK + VMIN/VTIME）       │    │
│  │  └─ 打开 / 读取 / 关闭                       │    │
│  └─────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────┘
         ▲
         │ 串口字节流
         │
    GPS 模块（硬件）
```

**分层原则**：

- 每一层只依赖下一层，不反向依赖
- 第一层不知道什么是 NMEA，只负责收发字节
- 第二层不知道什么是里程、Socket，只负责把字节解析成结构体
- 第三层不知道串口怎么配、NMEA 怎么解析，只调用下层 API

### 3.2 完整数据流

```
GPS 模块
   │
   │  (串口, 9600bps, NMEA-0183 文本流)
   ▼
uart_read() ──→ 原始字节 (uint8_t buf[256])
   │
   │  (逐字节)
   ▼
nmea_parser_feed() ──→ 状态机逐字符处理
   │
   │  (完整语句 + 校验和通过)
   ▼
nmea_extract_data() ──→ gps_data_t (经纬度/速度/卫星数...)
   │
   ├──────────────────→ update_odometer() ──→ 累计里程
   │                      (Haversine, 过滤 <1m 漂移)
   │
   ├──────────────────→ record_track_point() ──→ 轨迹环形缓冲
   │                      (最多 3600 点, 覆盖最旧)
   │
   └──────────────────→ ctx->latest (最新数据快照)
                           │
                           │  (timerfd 每秒触发)
                           ▼
                    sock_send_frame() ──→ UNIX Socket ──→ UI 进程
                    (MSG_GPS_DATA, gps_data_t)
```

### 3.3 事件驱动模型（epoll）

整个进程是**单线程**的，通过 `epoll` 同时监听 5 种文件描述符：

```
┌──────────────────────────────────────────────────┐
│              epoll_wait() 阻塞等待                │
│                                                    │
│  ┌──────────┐  ┌──────────┐  ┌──────────────┐  │
│  │ uart_fd  │  │ timer_fd │  │ signal_fd    │  │
│  │ 串口数据 │  │ 1s 定时  │  │ SIGTERM/INT  │  │
│  └────┬─────┘  └────┬─────┘  └──────┬───────┘  │
│       │              │                 │          │
│       ▼              ▼                 ▼          │
│  handle_uart    handle_timer     handle_signal   │
│  _event()       _event()         _event()        │
│                                                    │
│  ┌──────────┐  ┌──────────┐                      │
│  │ server_  │  │ client_  │                      │
│  │ fd       │  │ fd       │                      │
│  │ 新连接   │  │ UI 消息  │                      │
│  └────┬─────┘  └────┬─────┘                      │
│       │              │                            │
│       ▼              ▼                            │
│  handle_server  handle_client                     │
│  _event()       _event()                          │
└──────────────────────────────────────────────────┘
```

**为什么用 epoll 而不是多线程？**

- 嵌入式平台资源有限，单线程避免了线程切换开销和锁竞争
- 所有操作都是非阻塞的，epoll 可以高效管理大量 fd
- 数据流向单一（串口→解析→推送），没有复杂的并发需求

---

## 4. 第一层：串口通信层

> 文件：`uart_util.h` / `uart_util.c`
> 
> 这一层的唯一职责：**打开串口、配置串口、从串口读字节、关闭串口**。它不知道 NMEA 是什么，也不知道数据要给谁。

### 4.1 什么是 termios

在 Linux 中，串口（UART）被抽象为一个终端设备文件（如 `/dev/ttymxc5`）。对它的配置通过 `termios` 结构体完成。

```c
struct termios {
    tcflag_t c_iflag;   // 输入模式标志
    tcflag_t c_oflag;   // 输出模式标志
    tcflag_t c_cflag;   // 控制模式标志（波特率、数据位、校验、停止位）
    tcflag_t c_lflag;   // 本地模式标志（回显、规范模式、信号）
    cc_t     c_cc[NCCS];// 特殊控制字符（VMIN、VTIME 等）
};
```

### 4.2 串口配置的 6 个步骤

`uart_open()` 函数完整展示了串口配置的标准流程：

#### 步骤 1：打开设备文件

```c
fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
```

三个标志的含义：

- `O_RDWR`：读写模式。GPS 通常只需读，但某些模块支持写配置命令
- `O_NOCTTY`：**关键！** 不将此设备设为进程的控制终端。否则设备收到 Ctrl+C 可能会向进程发送 SIGINT
- `O_NONBLOCK`：非阻塞。`open()` 立即返回，不等待 DCD（载波检测）信号

#### 步骤 2：保存原始配置

```c
tcgetattr(fd, &g_old_termios);  // 保存，退出时恢复
```

这是一个好习惯——程序退出时恢复串口的原始设置，避免影响其他可能使用该串口的程序。

#### 步骤 3：设置原始模式（Raw Mode）

```c
cfmakeraw(&tio);
```

`cfmakeraw()` 是一个便利函数，它等价于：

```c
tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
tio.c_oflag &= ~OPOST;
tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
tio.c_cflag &= ~(CSIZE | PARENB);
tio.c_cflag |= CS8;
```

**原始模式的含义**：内核不做任何加工，每个字节按原样传递。这正是 GPS NMEA 解析需要的——我们不希望内核把 `\r` 转成 `\n`，不希望回显，不希望行缓冲。

对比：**规范模式（Canonical Mode）** 是终端的默认模式，它会按行缓冲、处理退格键、回显字符等——适合人机交互，不适合设备通信。

#### 步骤 4：设置波特率和帧格式

```c
cfsetispeed(&tio, baud_rate);  // 输入波特率
cfsetospeed(&tio, baud_rate);  // 输出波特率

tio.c_cflag |= CREAD | CLOCAL;  // 启用接收器 + 忽略调制解调器控制线
tio.c_cflag &= ~PARENB;          // 无校验位
tio.c_cflag &= ~CSTOPB;          // 1 位停止位（不设 CSTOPB 就是 1 位）
tio.c_cflag &= ~CSIZE;           // 清除数据位掩码
tio.c_cflag |= CS8;              // 8 位数据位
tio.c_cflag &= ~CRTSCTS;         // 关闭硬件流控
```

组合起来就是 **8N1**：8 数据位、无校验、1 停止位——这是 GPS 模块的标准配置。

`CLOCAL` 很重要：GPS 模块通常只有 TX/RX/GND 三根线，没有 DCD/DTR/DSR 等调制解调器控制线。如果不设 `CLOCAL`，`open()` 可能会阻塞等待 DCD 信号。

#### 步骤 5：设置读取超时（VMIN / VTIME）

```c
tio.c_cc[VMIN]  = 0;   // 不要求最少字节数
tio.c_cc[VTIME] = 1;   // 超时 0.1 秒（1 × 0.1s）
```

VMIN 和 VTIME 只在**非规范模式**下生效，共有 4 种组合：

| VMIN | VTIME | 行为                             |
| ---- | ----- | ------------------------------ |
| 0    | 0     | 有数据立即返回，无数据返回 0                |
| 0    | >0    | 有数据返回，无数据等待 VTIME×0.1s 后超时返回 0 |
| >0   | 0     | 阻塞直到至少收到 VMIN 字节               |
| >0   | >0    | 收到首字节后等 VTIME×0.1s，或收满 VMIN 字节 |

GPS 串口使用 `VMIN=0, VTIME=1`：

- 有数据时立即返回（适合 epoll 事件驱动）
- 无数据时最多等 100ms（防止 epoll 误触发时 CPU 空转）

#### 步骤 6：写入配置 + 清空缓冲 + 确认非阻塞

```c
tcsetattr(fd, TCSANOW, &tio);   // TCSANOW = 立即生效
tcflush(fd, TCIOFLUSH);          // 清空输入输出缓冲区

// 再次通过 fcntl 确认非阻塞（某些驱动可能重置标志）
int flags = fcntl(fd, F_GETFL, 0);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);
```

`tcflush` 清空内核缓冲区中的残留数据，避免读到打开之前的垃圾数据。

### 4.3 非阻塞读取

```c
int uart_read(int fd, uint8_t *buf, uint32_t size)
{
    ssize_t n = read(fd, buf, size);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;  // 无数据，正常
        if (errno == EINTR) return 0;                              // 被信号中断
        return -1;  // 真正的错误
    }
    return (int)n;
}
```

非阻塞 `read()` 的返回值：

- `>0`：读到了数据，返回字节数
- `0`：在 VMIN=0 模式下表示无数据（或对端关闭，但串口不会）
- `-1, errno=EAGAIN`：无数据，这是非阻塞的正常情况，不是错误
- `-1, errno=EIO`：串口断开等硬件错误

### 4.4 小结

| 概念       | 关键点                                      |
| -------- | ---------------------------------------- |
| termios  | Linux 串口配置的标准接口，通过结构体设置                  |
| 原始模式     | `cfmakeraw()`，关闭所有内核加工，字节透传              |
| 8N1      | 8 数据位、无校验、1 停止位，GPS 标准                   |
| 非阻塞      | `O_NONBLOCK` + `VMIN=0/VTIME=1`，配合 epoll |
| O_NOCTTY | 防止串口成为控制终端，避免意外信号                        |

---

## 5. 第二层：NMEA 协议解析层

> 文件：`nmea_parser.h` / `nmea_parser.c`
> 
> 这一层的职责：**把串口读到的原始字节流，解析成结构化的 GPS 数据**。它不关心数据从哪来（串口/文件/网络），也不关心解析后的数据用来做什么。

### 5.1 NMEA-0183 协议格式

NMEA-0183 是 GPS 设备的标准通信协议，基于 ASCII 文本，每条语句以 `$` 开头，以 `\r\n` 结尾：

```
$GPGGA,123519.00,3912.3456,N,11623.4567,E,1,08,0.9,50.5,M,0.0,M,,*4A\r\n
││  │  │        │         │ │          │ │ │  │   │    │ │     │  │  │
││  │  │        │         │ │          │ │ │  │   │    │ │     │  │  └─ LF (0x0A)
││  │  │        │         │ │          │ │ │  │   │    │ │     │  └──── CR (0x0D)
││  │  │        │         │ │          │ │ │  │   │    │ │     └─────── 校验和 (2位HEX)
││  │  │        │         │ │          │ │ │  │   │    │ └────────────── 字段 N
││  │  │        │         │ │          │ │ │  │   │    └──────────────── 字段分隔符 ','
││  │  │        │         │ │          │ │ │  │   └───────────────────── 字段 3 (海拔)
││  │  │        │         │ │          │ │ │  └────────────────────────── 字段 2 (HDOP)
││  │  │        │         │ │          │ │ └───────────────────────────── 字段 1 (卫星数)
││  │  │        │         │ │          │ └──────────────────────────────── 字段 0 (定位质量)
││  │  │        │         │ │          └────────────────────────────────── 经度方向 E/W
││  │  │        │         │ └───────────────────────────────────────────── 经度 dddmm.mmmm
││  │  │        │         └─────────────────────────────────────────────── 纬度方向 N/S
││  │  │        └────────────────────────────────────────────────────────── 纬度 ddmm.mmmm
││  │  └─────────────────────────────────────────────────────────────────── UTC 时间 hhmmss.ss
││  └────────────────────────────────────────────────────────────────────── 语句类型 (3字符: GGA)
│└───────────────────────────────────────────────────────────────────────── Talker ID (2字符: GP)
└────────────────────────────────────────────────────────────────────────── 起始符 '$'
```

**校验和规则**：从 `$` 之后第一个字节开始，到 `*` 之前最后一个字节结束，所有字节逐位 XOR，结果用 2 位十六进制表示。

```
校验和 = G ^ P ^ G ^ G ^ A ^ , ^ 1 ^ 2 ^ 3 ^ 5 ^ 1 ^ 9 ^ . ^ 0 ^ 0 ^ , ^ ...
```

### 5.2 为什么用状态机而不是 sscanf

常见的简单做法是用 `sscanf`：

```c
sscanf(sentence, "$GPGGA,%f,%f,%c,%f,%c,%d,%d,%f,%f,M,...", ...);
```

但这种方式有明显缺陷：

- 格式错误时行为不可预测
- 字段为空时解析中断
- 没有校验和验证
- 不能处理跨多个 `read()` 的分片数据

**状态机方式**逐字符处理，每一步状态转换明确，对线路噪声和格式错误有天然的容错能力，且天然支持流式数据（语句可以跨任意多个 `read()` 调用）。

### 5.3 状态机设计

#### 状态定义

```c
typedef enum {
    NMEA_STATE_IDLE     = 0,  // 等待 '$'
    NMEA_STATE_TALKER   = 1,  // 读 Talker ID（2字符：GP/GL/GN）
    NMEA_STATE_TYPE     = 2,  // 读语句类型（3字符：GGA/RMC/GSV...）
    NMEA_STATE_FIELD    = 3,  // 读字段值（以 ',' 或 '*' 终止）
    NMEA_STATE_CHECKSUM = 4,  // 读 2 位十六进制校验和
    NMEA_STATE_CR       = 5,  // 已收到 CR，等待 LF
} nmea_parser_state_t;
```

#### 状态转换图

```
                    ┌──────────────────────────────────────────────┐
                    │                                              │
                    ▼                                              │
  ┌─────┐   '$'   ┌────────┐  2字符  ┌──────┐  3字符  ┌───────┐ │
  │IDLE │ ──────→ │ TALKER │ ──────→ │ TYPE │ ──────→ │ FIELD │ │
  └─────┘         └────────┘         └──────┘         └───┬───┘ │
    ▲                                                         │     │
    │                                            ',' 字段分隔 │     │
    │                                                         ▼     │
    │                                                    (继续FIELD)│
    │                                                         │     │
    │                                              '*' 校验和 │     │
    │                                                         ▼     │
    │                                                    ┌────────┐ │
    │                                                    │CHECKSUM│ │
    │                                                    └───┬────┘ │
    │                                                        │ 2字符 │
    │                                                        ▼      │
    │                                                    ┌──────┐   │
    └────────────────────── LF / 容错 ─────────────────│  CR  │   │
                                                         └──────┘   │
                                                            │ LF    │
                                                            └───────┘
                                                         (完成, 返回1)
```

#### 核心函数：`nmea_parser_feed()`

这是解析器的核心，**每收到一个字节就调用一次**：

```c
int nmea_parser_feed(nmea_parser_t *parser, char ch)
{
    switch (parser->state) {
    case NMEA_STATE_IDLE:
        if (ch == '$') {
            nmea_parser_reset(parser);       // 重置内部状态
            parser->state = NMEA_STATE_TALKER;
        }
        break;  // 非 '$' 字符直接跳过（噪声）

    case NMEA_STATE_TALKER:
        // 读 2 个 Talker ID 字符，第 3 个字符进入 TYPE
        parser->buf[parser->buf_pos++] = ch;
        if (parser->buf_pos >= 3) parser->state = NMEA_STATE_TYPE;
        parser->checksum ^= (uint8_t)ch;    // 参与校验和
        break;

    case NMEA_STATE_TYPE:
        // 读 3 个类型字符，读完后识别类型，进入 FIELD
        ...
        parser->result.type = identify_sentence_type(...);
        parser->state = NMEA_STATE_FIELD;
        break;

    case NMEA_STATE_FIELD:
        if (ch == ',') {
            // 字段分隔符：结束当前字段，开始下一个
            parser->buf[parser->buf_pos++] = '\0';
            parser->result.fields[parser->field_idx] = ...;
            parser->field_idx++;
        } else if (ch == '*') {
            // 校验和分隔符：结束最后一个字段，进入 CHECKSUM
            ...
            parser->state = NMEA_STATE_CHECKSUM;
        } else if (ch == '\r' || ch == '\n') {
            // 容错：部分廉价 GPS 模块不发送校验和
            // 直接视为语句结束
            ...
            return 1;
        } else {
            // 普通字段字符：写入缓冲区
            parser->buf[parser->buf_pos++] = ch;
        }
        parser->checksum ^= (uint8_t)ch;
        break;

    case NMEA_STATE_CHECKSUM:
        // 读 2 位十六进制，验证校验和
        ...
        parser->result.checksum_ok = (raw == calc) ? 1 : 0;
        parser->state = NMEA_STATE_CR;
        break;

    case NMEA_STATE_CR:
        if (ch == '\n') {
            parser->state = NMEA_STATE_IDLE;
            return parser->result.checksum_ok ? 1 : -1;
        }
        break;
    }
    return 0;  // 需要更多字符
}
```

**返回值约定**：

- `0`：语句仍在处理中，需要更多字符
- `1`：一条完整、校验和通过的语句已解码，`parser->result` 已填充
- `-1`：语句已解码但校验和失败（`result` 仍已填充，可供调试）

#### 解析器状态结构体

```c
typedef struct {
    nmea_parser_state_t state;           // 当前状态
    nmea_parsed_t       result;          // 解析结果（进行中或已完成）
    char    buf[NMEA_MAX_FIELDS * 32];  // 内部字符缓冲区（字段指针指向这里）
    int     buf_pos;                      // buf 写入游标
    int     field_start;                  // 当前字段起始偏移
    int     field_idx;                    // 当前字段索引
    uint8_t checksum;                     // 累计 XOR 校验和
    int     checksum_pos;                 // 已读校验和字符数（0-2）
    char    checksum_hex[3];             // 校验和字符
    // 诊断统计
    uint32_t total_sentences;
    uint32_t checksum_errors;
    uint32_t unknown_types;
} nmea_parser_t;
```

**关键设计**：`fields[]` 数组存储的是**指向内部 `buf` 的指针**，不是字符串拷贝。这意味着：

- 零拷贝，效率高
- 指针仅在下次调用 `feed()` 之前有效，调用者必须及时复制数据

### 5.4 校验和算法

```c
// 计算：从 '$' 后到 '*' 前，所有字节逐位 XOR
parser->checksum ^= (uint8_t)ch;  // 在 TALKER/TYPE/FIELD 状态中持续累加

// 验证：将收到的 2 位 HEX 转为整数，与计算值比较
parser->result.raw_checksum  = hex_to_byte(parser->checksum_hex);
parser->result.calc_checksum = parser->checksum;
parser->result.checksum_ok   = (raw == calc) ? 1 : 0;
```

`hex_to_byte()` 手动实现 2 位十六进制转整数（不使用 `sscanf`，保持轻量）：

```c
static int hex_to_byte(const char hex[2])
{
    int val = 0;
    for (int i = 0; i < 2; i++) {
        val <<= 4;
        if (hex[i] >= '0' && hex[i] <= '9')      val |= (hex[i] - '0');
        else if (hex[i] >= 'A' && hex[i] <= 'F') val |= (hex[i] - 'A' + 10);
        else if (hex[i] >= 'a' && hex[i] <= 'f') val |= (hex[i] - 'a' + 10);
        else return -1;
    }
    return val;
}
```

### 5.5 六种 NMEA 语句的字段提取

解析器识别语句类型后，通过 `nmea_extract_data()` 分发到对应的提取函数：

```c
int nmea_extract_data(const nmea_parsed_t *parsed, gps_data_t *gps_data)
{
    switch (parsed->type) {
    case NMEA_GPGGA: return nmea_extract_gga(parsed, gps_data);
    case NMEA_GPRMC: return nmea_extract_rmc(parsed, gps_data);
    case NMEA_GPGSV: return nmea_extract_gsv(parsed, gps_data);
    case NMEA_GPGLL: return nmea_extract_gll(parsed, gps_data);
    case NMEA_GPVTG: return nmea_extract_vtg(parsed, gps_data);
    case NMEA_GPGSA: return nmea_extract_gsa(parsed, gps_data);
    default:         return -1;
    }
}
```

**重要设计**：每个提取函数**只写入该语句类型中存在的字段**，其他字段保留之前的值。这样多条不同类型的语句可以累积成一个完整的定位结果。

#### GGA — 定位信息（最核心）

```
$GPGGA,时间,纬度,N,经度,E,质量,卫星数,HDOP,海拔,M,...*校验和
  [0]  [1]  [2] [3][4]  [5][6] [7]  [8]  [9]
```

| 字段  | 含义                      | 写入 gps_data_t    |
| --- | ----------------------- | ---------------- |
| [0] | UTC 时间 hhmmss.ss        | hour, min, sec   |
| [1] | 纬度 ddmm.mmmm            | latitude（需度分转换）  |
| [2] | N/S                     | latitude 符号      |
| [3] | 经度 dddmm.mmmm           | longitude（需度分转换） |
| [4] | E/W                     | longitude 符号     |
| [5] | 定位质量（0=无效,1=GPS,2=DGPS） | fix_quality      |
| [6] | 使用中的卫星数                 | satellites       |
| [7] | HDOP（水平精度因子）            | hdop             |
| [8] | 海拔高度（米）                 | altitude         |

#### RMC — 推荐最小导航信息

```
$GPRMC,时间,状态,纬度,N,经度,E,速度(节),航向,日期,...*校验和
  [0]  [1]  [2] [3][4]  [5][6]    [7]  [8]
```

| 字段  | 含义            | 写入 gps_data_t           |
| --- | ------------- | ----------------------- |
| [1] | 状态 A=有效, V=无效 | V 时 fix_quality=0，不更新位置 |
| [6] | 地面速度（节）       | speed（节 → km/h，×1.852）  |
| [8] | 日期 ddmmyy     | year, month, day        |

RMC 是唯一包含**日期**的语句，也是速度的主要来源。

#### GSV — 可见卫星信息

```
$GPGSV,总语句数,当前语句号,可见卫星总数,卫星1PRN,仰角,方位角,SNR,...*校验和
  [0]    [1]      [2]      [3..]
```

只提取 `[2]`（可见卫星总数），且取多条 GSV 中的最大值（不同星座可能分别报告）。

#### GLL — 地理定位信息

```
$GPGLL,纬度,N,经度,E,时间,状态,模式*校验和
  [0]  [1][2]  [3][4] [5] [6]
```

简化版的位置信息，不含高度和 DOP。状态为 V 时不更新位置。

#### VTG — 地面速度和航向

```
$GPVTG,真北航向,T,磁北航向,M,速度(节),N,速度(km/h),K,模式*校验和
  [0]      [1][2]      [3][4]    [5][6]      [7]
```

速度提取优先级：

1. 字段 [6]（km/h）—— 存在时直接使用
2. 字段 [4]（节）—— 回退转换为 km/h

#### GSA — DOP 和活跃卫星

```
$GPGSA,模式,定位类型,PRN1..PRN12,PDOP,HDOP,VDOP*校验和
  [0]  [1]    [2..13]     [14] [15] [16]
```

| 字段   | 含义                  | 写入 gps_data_t    |
| ---- | ------------------- | ---------------- |
| [1]  | 定位类型（1=无,2=2D,3=3D） | fix_quality      |
| [15] | HDOP                | hdop（作为 GGA 的补充） |

### 5.6 度分格式转换

NMEA 中的经纬度不是十进制度，而是**度分格式**：

```
纬度:  ddmm.mmmm   (2位度数 + 分数)
经度: dddmm.mmmm   (3位度数 + 分数)
```

转换公式：

```
十进制度 = 度数 + 分数 / 60
```

实现：

```c
double gps_nmea_to_degrees(double nmea_deg)
{
    int    deg = (int)(nmea_deg / 100.0);   // 拿掉后两位就是度数
    double min = nmea_deg - (double)(deg * 100);  // 剩下的就是分数
    return (double)deg + min / 60.0;
}
```

示例：

- 纬度 `3912.3456` → 39 + 12.3456/60 = **39.205760°**
- 经度 `11623.4567` → 116 + 23.4567/60 = **116.390945°**

符号（N/S, E/W）由调用者根据 NMEA 语句中的半球指示字段单独应用：S 和 W 为负值。

### 5.7 容错设计

解析器在多个层面做了容错：

1. **线路噪声**：IDLE 状态下非 `$` 字符直接跳过
2. **无校验和模块**：FIELD 状态下遇到 `\r`/`\n` 直接视为语句结束，`checksum_ok` 默认通过
3. **空字段**：提取函数中每个字段都检查 `fields[i] && fields[i][0]`，空字段不更新
4. **缓冲区溢出**：写入 `buf` 前检查 `buf_pos < sizeof(buf) - 2`
5. **CR 不严格**：CR 状态下遇到非 LF 字符也视为完成（容错）
6. **校验和失败**：返回 -1 但 `result` 仍填充，可供调试

### 5.8 小结

| 概念        | 关键点                                |
| --------- | ---------------------------------- |
| NMEA-0183 | ASCII 文本协议，`$` 开头，`\r\n` 结尾，逗号分隔字段 |
| 字符级状态机    | 逐字节处理，天然支持流式分片数据，容错性强              |
| XOR 校验和   | `$` 到 `*` 之间所有字节逐位 XOR，2 位 HEX 表示  |
| 度分转换      | `ddmm.mmmm` → `度 + 分/60`           |
| 语句累积      | 每种语句只写自己的字段，多条语句拼成完整结果             |
| 零拷贝       | `fields[]` 是指向内部 buf 的指针，调用者需及时复制  |

---

## 6. 第三层：业务逻辑层

> 文件：`gps_daemon.h` / `gps_daemon.c`
> 
> 这一层是整个模块的"大脑"，负责：初始化所有资源、运行 epoll 事件循环、处理串口数据、累计里程、记录轨迹、与 UI 通信、优雅退出。

### 6.1 核心数据结构

#### gps_context_t — 进程上下文

这是整个守护进程的"全局状态"，所有模块共享：

```c
typedef struct {
    /* 串口 */
    char        uart_device[64];   // 串口设备路径
    int         uart_fd;            // 串口文件描述符

    /* 最新定位数据 */
    gps_data_t  latest;             // 最新解析的 GPS 数据（共享结构体）
    gps_fix_type_t fix_type;        // 当前定位状态（NONE/2D/3D）

    /* 里程 */
    gps_odometer_t odo;             // 里程累计器

    /* 轨迹 */
    gps_track_point_t *track_buf;   // 轨迹缓冲区（动态分配）
    int         track_count;        // 当前轨迹点数
    int         track_max;          // 最大轨迹点数

    /* 配置 */
    int         report_interval_ms; // 上报间隔
    proc_config_t proc_cfg;         // 进程通用配置（socket路径、日志路径）

    /* 运行状态 */
    int         running;            // 运行标志（0=退出）
    int         epoll_fd;           // epoll 实例
    int         timer_fd;           // 数据上报定时器
    int         signal_fd;          // 信号 fd
    int         server_fd;          // Socket 服务端
    int         client_fd;          // UI 客户端连接
    protocol_parser_t proto_parser; // 协议解析器（UI 消息）
    ringbuffer_t uart_rb;           // 串口接收环形缓冲
    ringbuffer_t recv_rb;           // Socket 接收环形缓冲
} gps_context_t;
```

#### gps_odometer_t — 里程累计器

```c
typedef struct {
    double   total_km;     // 总里程（公里）
    double   last_lat;     // 上一个有效定位点的纬度
    double   last_lon;     // 上一个有效定位点的经度
    int      has_last;     // 是否有上一个有效点
    uint32_t point_count;  // 累计定位点数量
} gps_odometer_t;
```

#### gps_track_point_t — 轨迹点

```c
typedef struct {
    double   lat;          // 纬度
    double   lon;          // 经度
    float    speed;        // 速度（km/h）
    time_t   timestamp;    // 时间戳
} gps_track_point_t;
```

### 6.2 初始化流程（init_all）

`init_all()` 按顺序完成 11 个初始化步骤，任何一步失败都返回 -1：

```
1.  memset 上下文，fd 初始化为 -1，running=1
2.  初始化日志系统（log_init）
3.  加载配置（gps_config_load）
4.  分配轨迹缓冲区（calloc, 3600 个点）
5.  初始化里程累计器（has_last=0, total_km=0）
6.  打开串口（uart_open, 9600bps 8N1）
7.  初始化环形缓冲区（uart_rb 4KB, recv_rb 4KB）和协议解析器
8.  创建 UNIX Socket 服务端（sock_create_server）
9.  创建定时器（timerfd_create_ms, 上报间隔）
10. 创建 epoll 实例，注册 uart_fd / server_fd / timer_fd / signal_fd
11. 启动定时器（timer_start）
```

**epoll 注册**：

```c
ctx->epoll_fd = epoll_create1(0);

ev.events = EPOLLIN;
ev.data.fd = ctx->uart_fd;
epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->uart_fd, &ev);
// ... 同样注册 server_fd, timer_fd, signal_fd
```

**信号处理的特殊设计**：不使用传统的 signal handler，而是用 `signalfd`：

```c
sigset_t sigmask;
sigemptyset(&sigmask);
sigaddset(&sigmask, SIGTERM);
sigaddset(&sigmask, SIGINT);
sigprocmask(SIG_BLOCK, &sigmask, NULL);  // 先阻塞信号
ctx->signal_fd = signalfd(-1, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
// 注册到 epoll
```

这样信号就变成了一个可以被 epoll 监听的 fd，处理逻辑在事件循环中同步执行，避免了信号处理函数的重入问题。

### 6.3 主事件循环（run_loop）

```c
static void run_loop(gps_context_t *ctx)
{
    struct epoll_event events[MAX_EPOLL_EVENTS];

    while (ctx->running) {
        int nfds = epoll_wait(ctx->epoll_fd, events, MAX_EPOLL_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) continue;  // 被信号中断，重试
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            if      (fd == ctx->uart_fd)   handle_uart_event(ctx);
            else if (fd == ctx->timer_fd)  handle_timer_event(ctx);
            else if (fd == ctx->signal_fd) handle_signal_event(ctx);
            else if (fd == ctx->server_fd) handle_server_event(ctx);
            else if (fd == ctx->client_fd) handle_client_event(ctx);
        }
    }
}
```

`epoll_wait(..., -1)` 无限期阻塞，直到有事件发生。通过 `events[i].data.fd` 判断是哪个 fd 触发，分发到对应的处理函数。

### 6.4 串口事件处理（handle_uart_event）

这是数据流入的入口：

```c
static void handle_uart_event(gps_context_t *ctx)
{
    uint8_t buf[256];
    static nmea_parser_t nmea;       // static：状态在调用间保持
    static int nmea_inited = 0;

    if (!nmea_inited) {
        nmea_parser_init(&nmea);
        nmea_inited = 1;
    }

    // 循环读取，直到 EAGAIN（非阻塞，读空为止）
    while (1) {
        int n = uart_read(ctx->uart_fd, buf, sizeof(buf));
        if (n <= 0) break;

        // 逐字节喂入状态机
        for (int i = 0; i < n; i++) {
            int ret = nmea_parser_feed(&nmea, (char)buf[i]);

            if (ret == 1) {
                // 成功解析一条语句
                nmea_extract_data(&nmea.result, &ctx->latest);

                // 更新定位类型
                if (ctx->latest.fix_quality >= 2)
                    ctx->fix_type = GPS_FIX_3D;
                else if (ctx->latest.fix_quality == 1)
                    ctx->fix_type = GPS_FIX_2D;
                else
                    ctx->fix_type = GPS_FIX_NONE;

                // 位置类语句触发里程和轨迹更新
                if (nmea.result.type == NMEA_GPRMC
                    || nmea.result.type == NMEA_GPGGA
                    || nmea.result.type == NMEA_GPGLL) {
                    update_odometer(ctx);
                    record_track_point(ctx);
                }
            }
            else if (ret == -1) {
                // 校验和错误，已计数
                LOG_DEBUG("gps", "NMEA checksum error (total=%u)", nmea.checksum_errors);
            }
        }
    }
}
```

**关键点**：

- `nmea_parser_t` 声明为 `static`，因为解析器状态必须在多次 `handle_uart_event()` 调用之间保持（串口数据以任意大小的块到达）
- 内层 `while(1)` 循环读取，直到 `uart_read()` 返回 0（EAGAIN），确保把内核缓冲区中的数据全部读完
- 只有位置类语句（GGA/RMC/GLL）才触发里程和轨迹更新，GSV/VTG/GSA 不包含位置信息

### 6.5 里程累计（update_odometer）

```c
static void update_odometer(gps_context_t *ctx)
{
    if (ctx->latest.fix_quality == 0) return;  // 需要有效定位

    if (ctx->odo.has_last) {
        double dist_m = gps_haversine_distance(
            ctx->odo.last_lat, ctx->odo.last_lon,
            ctx->latest.latitude, ctx->latest.longitude);

        // 过滤微小位移（GPS 静止漂移通常 < 5m）
        // 阈值 1m：比实际漂移小，确保不遗漏真实低速移动
        if (dist_m > 1.0) {
            ctx->odo.total_km += dist_m / 1000.0;
            ctx->odo.point_count++;
        }
    }

    // 更新上一个有效点（无论是否累加了里程都要更新）
    ctx->odo.last_lat = ctx->latest.latitude;
    ctx->odo.last_lon = ctx->latest.longitude;
    ctx->odo.has_last = 1;
}
```

**为什么需要 1m 过滤阈值？**
GPS 在静止时会有"漂移"——由于多径效应、卫星几何变化等原因，即使车辆完全不动，GPS 输出的经纬度也会在小范围内跳动（通常 < 5m）。如果不过滤，静止时里程会缓慢增加。1m 阈值比典型漂移小，既能过滤噪声，又不会遗漏真实的低速移动。

### 6.6 Haversine 公式

计算地球表面两点之间的大圆距离：

```c
double gps_haversine_distance(double lat1, double lon1,
                              double lat2, double lon2)
{
    // 转为弧度
    double rlat1 = lat1 * M_PI / 180.0;
    double rlat2 = lat2 * M_PI / 180.0;
    double rlon1 = lon1 * M_PI / 180.0;
    double rlon2 = lon2 * M_PI / 180.0;

    double dlat = rlat2 - rlat1;
    double dlon = rlon2 - rlon1;

    // a = sin²(Δlat/2) + cos(lat1)·cos(lat2)·sin²(Δlon/2)
    double a = sin(dlat / 2.0) * sin(dlat / 2.0)
             + cos(rlat1) * cos(rlat2)
             * sin(dlon / 2.0) * sin(dlon / 2.0);

    // c = 2·atan2(√a, √(1-a))
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));

    // d = R · c
    return GPS_EARTH_RADIUS_M * c;  // R = 6371000 m
}
```

**为什么用 Haversine 而不是球面余弦定理？**
球面余弦定理公式：`d = R · arccos(sin(lat1)·sin(lat2) + cos(lat1)·cos(lat2)·cos(Δlon))`

当两点距离很近时，`arccos` 的参数接近 1，浮点计算会产生**灾难性消去**（catastrophic cancellation），导致精度严重下降。Haversine 公式通过 `sin²(Δ/2)` 的形式避免了这个问题，在短距离时精度更高——而车载 GPS 相邻两点的距离通常很短（几米到几十米），正好是 Haversine 的优势场景。

### 6.7 轨迹记录（record_track_point）

```c
static void record_track_point(gps_context_t *ctx)
{
    if (ctx->latest.fix_quality == 0) return;
    if (!ctx->track_buf) return;

    // 环形缓冲：count % max 得到索引，满了覆盖最旧的
    int idx = ctx->track_count % ctx->track_max;

    ctx->track_buf[idx].lat       = ctx->latest.latitude;
    ctx->track_buf[idx].lon       = ctx->latest.longitude;
    ctx->track_buf[idx].speed     = ctx->latest.speed;
    ctx->track_buf[idx].timestamp = time(NULL);

    ctx->track_count++;
}
```

**环形缓冲区的特点**：

- 固定大小（`GPS_MAX_TRACK_POINTS = 3600`，即 1Hz × 1 小时）
- 写满后覆盖最旧的数据（FIFO）
- `track_count` 持续递增（不回绕），通过 `count % max` 计算实际索引
- 这样可以区分"缓冲区还没满"和"缓冲区已满正在覆盖"两种情况

### 6.8 定时器事件（handle_timer_event）

每秒触发一次，做两件事：

```c
static void handle_timer_event(gps_context_t *ctx)
{
    uint64_t exp;
    timer_get_expirations(ctx->timer_fd, &exp);  // 消费过期计数

    // 1. 向 UI 推送最新 GPS 数据
    if (ctx->client_fd >= 0) {
        gps_data_t send_data;
        memcpy(&send_data, &ctx->latest, sizeof(gps_data_t));
        sock_send_frame(ctx->client_fd, MSG_GPS_DATA,
                        (uint8_t*)&send_data, sizeof(send_data));
    }

    // 2. 输出日志行
    LOG_INFO("gps", "Fix=%d Sats=%d Lat=%.6f Lon=%.6f Spd=%.1fkm/h ...",
             ctx->latest.fix_quality, ctx->latest.satellites, ...);
}
```

**为什么用 memcpy 拷贝一份再发送？**
`ctx->latest` 可能在串口事件中被随时更新（虽然单线程不会真正并发，但保持数据快照的一致性是好习惯）。拷贝一份确保发送的是某个时间点的完整快照。

### 6.9 UNIX Socket 通信

#### IPC 消息类型

| 方向       | 消息类型             | 值    | 说明                              |
| -------- | ---------------- | ---- | ------------------------------- |
| gps → UI | `MSG_GPS_DATA`   | 0x10 | 每秒推送完整 `gps_data_t`             |
| UI → gps | `MSG_GPS_QUERY`  | 0x11 | 请求最新数据（立即回复）                    |
| gps → UI | `MSG_GPS_STATUS` | 0x13 | 定位状态（fix_type/sats/fix_quality） |

#### 服务端事件（接受连接）

```c
static void handle_server_event(gps_context_t *ctx)
{
    // 单客户端策略：已有客户端则拒绝新连接
    if (ctx->client_fd >= 0) {
        int fd = sock_accept(ctx->server_fd);
        if (fd >= 0) sock_close(fd, NULL);
        return;
    }

    int fd = sock_accept(ctx->server_fd);
    if (fd < 0) return;

    // 注册到 epoll
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, fd, &ev);

    ctx->client_fd = fd;
    protocol_reset(&ctx->proto_parser, &ctx->recv_rb);
}
```

**单客户端策略**：同时只允许一个 UI 连接。新连接到来时直接 accept 然后 close，避免资源浪费。

#### 客户端事件（处理 UI 消息）

```c
static void handle_client_event(gps_context_t *ctx)
{
    uint8_t buf[512];
    int n = sock_recv(ctx->client_fd, buf, sizeof(buf));

    if (n <= 0) {
        // 客户端断开或错误：从 epoll 移除，关闭，重置
        epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, ctx->client_fd, NULL);
        sock_close(ctx->client_fd, NULL);
        ctx->client_fd = -1;
        return;
    }

    // 写入接收环形缓冲
    ringbuffer_write(&ctx->recv_rb, buf, (uint32_t)n);

    // 循环解包完整帧
    uint8_t type, data[64];
    uint32_t len;
    while (protocol_unpack(&ctx->proto_parser, &ctx->recv_rb,
                            &type, data, sizeof(data), &len) == 0) {
        switch (type) {
        case MSG_GPS_QUERY:
            // UI 请求最新数据：立即推送
            sock_send_frame(ctx->client_fd, MSG_GPS_DATA,
                           (uint8_t*)&ctx->latest, sizeof(ctx->latest));
            break;
        case MSG_GPS_STATUS:
            // UI 请求状态：回复 4 字节状态
            uint8_t status[4] = {
                (uint8_t)ctx->fix_type,
                ctx->latest.satellites,
                ctx->latest.fix_quality,
                0
            };
            sock_send_frame(ctx->client_fd, MSG_GPS_STATUS, status, 4);
            break;
        }
    }
}
```

### 6.10 信号处理与优雅退出

```c
static void handle_signal_event(gps_context_t *ctx)
{
    struct signalfd_siginfo si;
    if (read(ctx->signal_fd, &si, sizeof(si)) == sizeof(si)) {
        if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) {
            LOG_INFO("gps", "Signal %d, shutting down.", si.ssi_signo);
            ctx->running = 0;  // 设置退出标志，事件循环自然退出
        }
    }
}
```

收到信号后只是设置 `running = 0`，`run_loop()` 的 while 条件不满足后自然退出，然后调用 `cleanup_all()` 释放所有资源。这比在信号处理函数中直接做清理安全得多（信号处理函数中只能调用异步信号安全的函数）。

### 6.11 资源清理（cleanup_all）

```c
static void cleanup_all(gps_context_t *ctx)
{
    if (ctx->client_fd >= 0) sock_close(ctx->client_fd, NULL);
    ringbuffer_destroy(&ctx->uart_rb);
    ringbuffer_destroy(&ctx->recv_rb);
    if (ctx->timer_fd >= 0)  timer_close(ctx->timer_fd);
    if (ctx->server_fd >= 0) sock_close(ctx->server_fd, ctx->proc_cfg.socket_path);
    if (ctx->signal_fd >= 0) close(ctx->signal_fd);
    if (ctx->epoll_fd >= 0)  close(ctx->epoll_fd);
    if (ctx->uart_fd >= 0)   uart_close(ctx->uart_fd);
    free(ctx->track_buf);
    ctx->track_buf = NULL;
    log_close();
}
```

每个资源在释放前都检查 `>= 0` 或 `!= NULL`，确保即使初始化中途失败也能安全调用。

### 6.12 配置加载

```c
int gps_config_load(gps_context_t *ctx, const char *conf_file)
{
    config_t conf;
    int conf_loaded = (conf_file && config_load(&conf, conf_file) == 0);

    // 串口设备（默认 /dev/ttymxc5）
    if (conf_loaded) {
        config_get_str(&conf, "gps", "uart_device",
                       GPS_DEFAULT_UART_DEVICE, ctx->uart_device, sizeof(ctx->uart_device));
    } else {
        strncpy(ctx->uart_device, GPS_DEFAULT_UART_DEVICE, sizeof(ctx->uart_device) - 1);
    }

    // 上报间隔（默认 1000ms）
    ctx->report_interval_ms = conf_loaded ?
        config_get_int(&conf, "gps", "report_interval", 1) * 1000
        : GPS_DEFAULT_REPORT_INTERVAL_MS;

    // Socket 路径和日志路径（固定）
    snprintf(ctx->proc_cfg.socket_path, sizeof(ctx->proc_cfg.socket_path),
             "%s", GPS_SOCKET_PATH);
    snprintf(ctx->proc_cfg.log_path, sizeof(ctx->proc_cfg.log_path),
             "%s", GPS_LOG_PATH);

    if (conf_loaded) config_unload(&conf);
    return RET_OK;
}
```

**设计要点**：配置文件加载失败时使用硬编码默认值，确保进程总能启动。

### 6.13 小结

| 概念            | 关键点                                  |
| ------------- | ------------------------------------ |
| gps_context_t | 全局上下文，持有所有 fd、缓冲区、配置和状态              |
| epoll 事件循环    | 单线程同时管理 5 种 fd，事件驱动，无阻塞              |
| 里程累计          | Haversine 公式 + 1m 漂移过滤 + 上一点缓存       |
| 轨迹记录          | 环形缓冲，固定 3600 点，满了覆盖最旧                |
| UNIX Socket   | 单客户端策略，定时推送 + 查询响应 + 状态回复            |
| timerfd       | 毫秒级定时器，集成到 epoll                     |
| signalfd      | 信号变 fd，同步处理，避免重入问题                   |
| 优雅退出          | 设置 running=0，循环自然退出，cleanup_all 释放资源 |

---

## 7. 测试设计

> 文件：`test/test_gps.c`
> 
> 单元测试无需 GPS 硬件，直接在 x86 上编译运行，验证 NMEA 解析和地理计算的正确性。

### 7.1 测试框架

使用极简的自定义测试框架（不依赖外部库）：

```c
#define TEST_START(n)  printf("\n========== [TEST] %s ==========\n", n)
#define TEST_CASE(d)   do { g_total++; printf("  [%d] %s ... ", g_total, d); } while(0)
#define TEST_OK()      do { printf("OK\n"); g_passed++; } while(0)
#define TEST_FAIL(m)   do { printf("FAIL: %s\n", m); g_failed++; return; } while(0)
#define TEST_ASSERT(c,m) do { if(!(c)) TEST_FAIL(m); } while(0)
```

辅助函数：逐字符喂入 NMEA 语句

```c
static int feed_sentence(nmea_parser_t *p, const char *s)
{
    int ret = 0;
    for (; *s; s++) {
        int r = nmea_parser_feed(p, *s);
        if (r != 0) ret = r;
    }
    return ret;
}
```

### 7.2 十组测试用例

| #   | 测试组       | 验证内容                                                      |
| --- | --------- | --------------------------------------------------------- |
| 1   | 度分转换      | `3912.3456 → 39.205760`，`11623.4567 → 116.390945`，`0 → 0` |
| 2   | Haversine | 天安门→故宫约 1.3km，同一点距离≈0                                     |
| 3   | GGA 解析    | 完整语句解析、类型识别、校验和通过、时间/经纬度/质量/卫星数/HDOP/海拔提取                 |
| 4   | RMC 解析    | 速度 15.5节→28.7km/h，日期 `010826→2026-08-01`                  |
| 5   | 校验和错误     | 篡改校验和后返回 -1，`checksum_ok=0`，错误计数器+1                       |
| 6   | 空字段容错     | GPS 刚启动时字段全空，不崩溃，默认值为 0                                   |
| 7   | GSV 解析    | 可见卫星数 12 颗                                                |
| 8   | GLL 解析    | 经纬度/时间提取，状态 V 时 `fix_quality=0`                           |
| 9   | VTG 解析    | km/h 直接使用，只有节时回退转换                                        |
| 10  | GSA 解析    | 定位类型 3D，HDOP=0.9，无定位时 fix_type=1                          |

### 7.3 运行测试

```bash
make test
```

输出示例：

```
╔══════════════════════════════════════════════╗
║  gps_daemon — Module Unit Tests              ║
╚══════════════════════════════════════════════╝

========== [TEST] NMEA Degrees → Decimal ==========
  [1] lat 3912.3456 → 39.205760 ... OK
  [2] lon 11623.4567 → 116.390945 ... OK
  [3] equator 0.0 → 0.0 ... OK
...
╔══════════════════════════════════════════════╗
║  Results: total=20 passed=20 failed=0        ║
╚══════════════════════════════════════════════╝
```

### 7.4 测试设计的启示

1. **纯逻辑可测**：NMEA 解析器和地理计算不依赖硬件，可以在 x86 上完全测试
2. **边界情况**：空字段、校验和错误、同一点距离为 0 等边界情况都有覆盖
3. **已知值验证**：使用真实的 NMEA 语句和已知地理距离作为测试数据
4. **无需框架**：嵌入式环境中不引入外部测试框架，用宏实现最小测试能力

---

## 8. 关键 Linux 知识点总结

| 知识点                    | 应用位置                    | 核心要点                                                                               |
| ---------------------- | ----------------------- | ---------------------------------------------------------------------------------- |
| **termios**            | `uart_util.c`           | 串口配置的标准接口；原始模式 `cfmakeraw()`；8N1；非阻塞 `O_NONBLOCK`；`VMIN/VTIME` 控制读取行为              |
| **字符级状态机**             | `nmea_parser.c`         | 逐字节处理协议；状态明确转换；天然支持流式分片数据；容错性强                                                     |
| **XOR 校验和**            | `nmea_parser.c`         | 最简单的数据完整性校验；逐字节异或；2 位 HEX 表示                                                       |
| **epoll**              | `gps_daemon.c`          | Linux 特有的高效 IO 多路复用；`epoll_create1` / `epoll_ctl` / `epoll_wait`；水平触发模式；单线程管理大量 fd |
| **Haversine**          | `gps_daemon.c`          | 球面两点大圆距离；短距离精度优于余弦定理；`a = sin²(Δlat/2) + cos(lat1)cos(lat2)sin²(Δlon/2)`           |
| **环形缓冲区**              | `gps_daemon.c` / common | 固定大小，满了覆盖最旧；`count % max` 索引；用于轨迹记录和流式数据缓冲                                         |
| **UNIX Domain Socket** | `gps_daemon.c`          | 进程间通信；`socket(AF_UNIX, SOCK_STREAM, 0)`；文件系统路径作为地址；比 TCP 快，无需网络协议栈                 |
| **timerfd**            | `gps_daemon.c`          | 定时器变成 fd，可被 epoll 监听；毫秒级精度；`timerfd_create` / `timerfd_settime`                    |
| **signalfd**           | `gps_daemon.c`          | 信号变成 fd，可被 epoll 监听；同步处理信号，避免重入；先 `sigprocmask` 阻塞再创建                              |
| **度分转换**               | `nmea_parser.c`         | NMEA `ddmm.mmmm` → 十进制度 `度 + 分/60`；S/W 为负                                          |
| **非阻塞 IO**             | 全模块                     | `O_NONBLOCK`；`read` 返回 EAGAIN 表示无数据；配合 epoll 实现事件驱动                                |
| **守护进程设计**             | 整体                      | 单线程事件驱动；资源初始化/清理对称；优雅退出；日志持久化                                                      |

---

## 9. 扩展与改进方向

### 9.1 功能扩展

1. **多星座支持**：当前 Talker ID 只识别 GP（GPS），可扩展 GL（GLONASS）、GN（多星座混合）、BD（北斗）
2. **轨迹持久化**：当前轨迹只在内存中，进程重启后丢失。可定期写入文件或数据库
3. **里程持久化**：总里程应持久化存储（如写入文件或 Flash），断电不丢失
4. **AGPS 辅助**：支持辅助 GPS 快速定位（通过网络下载星历）
5. **PPS 秒脉冲**：支持 GPS 模块的 PPS 信号，提供高精度时间同步

### 9.2 代码改进

1. **NMEA 解析器多实例**：当前 `handle_uart_event` 中 `nmea_parser_t` 是 static 局部变量，不支持多串口。应移入 `gps_context_t`
2. **配置热重载**：当前配置只在启动时加载，可支持 SIGHUP 信号触发重新加载
3. **错误恢复**：串口断开后当前进程直接退出，可增加自动重连机制
4. **多客户端支持**：当前只允许一个 UI 连接，可扩展为客户端列表支持多个订阅者
5. **数据校验**：可增加经纬度范围校验、速度合理性检查等数据质量控制

### 9.3 性能优化

1. **批量读取**：当前每次 `uart_read` 最多 256 字节，可根据实际吞吐量调整
2. **NMEA 语句过滤**：可在状态机中提前跳过不关心的语句类型，减少字段提取开销
3. **日志分级**：高频日志（如每秒的定位日志）在生产环境可降级为 DEBUG

---

## 附录：完整调用链速查

### 启动流程

```
main(argc, argv)
  └─ init_all(&g_ctx, conf_file)
       ├─ log_init()
       ├─ gps_config_load()
       ├─ calloc(track_buf)
       ├─ uart_open()          → 第一层：串口配置
       ├─ ringbuffer_init() ×2
       ├─ protocol_parser_init()
       ├─ sock_create_server()
       ├─ timerfd_create_ms()
       ├─ epoll_create1() + epoll_ctl(ADD) ×4
       ├─ sigprocmask(BLOCK) + signalfd()
       └─ timer_start()
  └─ run_loop(&g_ctx)
       └─ epoll_wait() → 分发事件
  └─ cleanup_all(&g_ctx)
       └─ 释放所有资源
```

### 数据处理流程（串口事件）

```
handle_uart_event(ctx)
  └─ while(uart_read() > 0)
       └─ for each byte:
            └─ nmea_parser_feed(&nmea, byte)   → 第二层：状态机
                 ├─ ret==1 → nmea_extract_data()
                 │            ├─ nmea_extract_gga()  / rmc / gsv / gll / vtg / gsa
                 │            └─ 更新 ctx->latest
                 ├─ 更新 fix_type
                 └─ 位置类语句 → update_odometer() + record_track_point()
                                   ├─ gps_haversine_distance()
                                   └─ 环形缓冲写入
```

### UI 通信流程

```
定时推送:
  timer_fd → handle_timer_event()
    └─ sock_send_frame(MSG_GPS_DATA, gps_data_t)

查询响应:
  client_fd → handle_client_event()
    ├─ sock_recv() → ringbuffer_write()
    └─ protocol_unpack() → MSG_GPS_QUERY
         └─ sock_send_frame(MSG_GPS_DATA, gps_data_t)

状态查询:
  client_fd → handle_client_event()
    └─ protocol_unpack() → MSG_GPS_STATUS
         └─ sock_send_frame(MSG_GPS_STATUS, 4字节状态)
```

---

> 本文档基于 `gps_daemon` 模块源码逐行分析整理，覆盖了从硬件接口到业务逻辑的完整学习路径。建议按照"第一层→第二层→第三层"的顺序阅读代码，配合本文档理解设计思路。
