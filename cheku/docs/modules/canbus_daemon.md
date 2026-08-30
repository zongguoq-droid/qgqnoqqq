# canbus_daemon 模块学习文档

> 本文档从架构设计、使用逻辑、代码设计三个维度，循序渐进地讲解车载终端 CAN 总线通信守护进程模块。
> 
> 适用读者：具备 C 语言基础、了解 Linux 基本概念，希望系统学习嵌入式 CAN 通信开发的工程师。

---

## 目录

1. [模块概述](#1-模块概述)
2. [系统架构设计](#2-系统架构设计)
3. [硬件平台与设备树](#3-硬件平台与设备树)
4. [SocketCAN 基础](#4-socketcan-基础)
5. [文件结构与职责划分](#5-文件结构与职责划分)
6. [核心数据结构](#6-核心数据结构)
7. [驱动层详解 — can_driver](#7-驱动层详解--can_driver)
8. [守护进程层详解 — canbus_daemon](#8-守护进程层详解--canbus_daemon)
9. [环形缓冲区设计](#9-环形缓冲区设计)
10. [IPC 通信协议](#10-ipc-通信协议)
11. [主事件循环（poll 模式）](#11-主事件循环poll-模式)
12. [编译与构建系统](#12-编译与构建系统)
13. [单元测试](#13-单元测试)
14. [已知 Bug 与修复记录](#14-已知-bug-与修复记录)
15. [学习路径建议](#15-学习路径建议)

---

## 1. 模块概述

### 1.1 模块定位

`canbus_daemon` 是智能车载终端系统中负责 **CAN 总线通信** 的后台守护进程。它在整个系统中扮演"CAN 总线网关"的角色：

- **向下**：通过 Linux SocketCAN 接口与 CAN 硬件控制器交互，收发 CAN 报文
- **向上**：通过 UNIX Domain Socket 与 Qt UI 进程通信，实时推送 CAN 数据、接收 UI 的发送指令

### 1.2 核心功能

| 功能          | 说明                                   |
| ----------- | ------------------------------------ |
| CAN 接口管理    | 绑定 can0（FlexCAN1），500kbps 波特率        |
| 报文收发        | 支持标准帧（11-bit）、扩展帧（29-bit）、远程帧        |
| 环形缓冲        | 缓存最近 200 条 CAN 报文（RX + TX），用于历史查询和调试 |
| 错误监控        | 检测 TEC/REC 错误计数器，监控总线状态（主动/被动/关闭）    |
| UNIX Socket | 向 UI 推送报文，接收 UI 的 CAN 发送指令           |
| 日志记录        | 每秒输出 RX/TX 统计，记录总线异常                 |

### 1.3 设计特点

- **单线程 poll 模式**：用 `poll()` 统一监听 CAN socket、UNIX socket，无需多线程同步
- **非阻塞 IO**：所有 socket 设为非阻塞，避免主循环卡死
- **分层架构**：驱动层（can_driver）与业务层（canbus_daemon）分离，驱动层可独立复用
- **零依赖外部库**：仅使用 Linux 系统调用和项目公共库，适合嵌入式交叉编译

---

## 2. 系统架构设计

### 2.1 在整车系统中的位置

本项目采用**微服务风格的多进程架构**，各功能模块独立为守护进程，通过 UNIX Domain Socket 进行 IPC 通信：

```
┌─────────────────────────────────────────────────────────────┐
│                        Qt UI 进程                             │
│         (统一人机界面，连接所有后台 daemon)                    │
└──────┬──────────┬──────────┬──────────┬──────────┬─────────┘
       │          │          │          │          │
  /tmp/car_can  /tmp/car_gps /tmp/car_dvr ...  /tmp/car_input
       │          │          │          │          │
┌──────▼──┐ ┌─────▼───┐ ┌───▼────┐            ┌──▼────────┐
│ canbus  │ │  gps    │ │  dvr   │    ...     │   input   │
│ daemon  │ │ daemon  │ │ daemon │            │  daemon   │
└────┬────┘ └────┬────┘ └───┬────┘            └────┬──────┘
     │           │           │                      │
┌────▼───────────▼───────────▼──────────────────────▼───────┐
│                    guard_daemon (父进程)                     │
│           (统一管理所有子进程生命周期、心跳、重启)              │
└─────────────────────────────────────────────────────────────┘
```

`canbus_daemon` 是其中一个子进程，由 `guard_daemon` 启动和监控。

### 2.2 模块内部架构

```
┌──────────────────────────────────────────────────────────┐
│                    canbus_daemon 进程                      │
│                                                            │
│  ┌────────────────────────────────────────────────────┐  │
│  │              主事件循环 (poll, 1000ms)              │  │
│  │                                                    │  │
│  │   ┌─────────┐   ┌──────────┐   ┌──────────────┐ │  │
│  │   │ CAN 接收 │   │ UI 连接  │   │ UI 发送指令  │ │  │
│  │   │ (can_fd) │   │(server_fd)│   │ (client_fd) │ │  │
│  │   └────┬────┘   └────┬─────┘   └──────┬───────┘ │  │
│  │        │              │                  │         │  │
│  └────────┼──────────────┼──────────────────┼─────────┘  │
│           │              │                  │            │
│  ┌────────▼────────┐    │         ┌────────▼────────┐   │
│  │  can_ring_t     │    │         │  protocol_parser│   │
│  │  (200条环形缓冲) │    │         │  + recv_rb      │   │
│  └────────┬────────┘    │         └────────┬────────┘   │
│           │              │                  │            │
│  ┌────────▼─────────────────────────────────▼────────┐   │
│  │              can_driver (SocketCAN 封装)            │   │
│  │   can_socket_open / can_send / can_recv / ...     │   │
│  └────────────────────────┬───────────────────────────┘   │
│                           │                                │
└───────────────────────────┼────────────────────────────────┘
                            │
                ┌───────────▼───────────┐
                │  Linux SocketCAN 栈    │
                │  (can.ko / can_raw.ko)│
                └───────────┬───────────┘
                            │
                ┌───────────▼───────────┐
                │  FlexCAN 控制器驱动    │
                │  (flexcan.ko)          │
                └───────────┬───────────┘
                            │
                ┌───────────▼───────────┐
                │  i.MX6ULL FlexCAN1    │
                │  → CAN 收发器 → CAN总线│
                └───────────────────────┘
```

### 2.3 数据流

#### 数据流 A：CAN 总线 → UI（接收路径）

```
CAN 总线 → FlexCAN 控制器 → flexcan.ko → SocketCAN 协议栈
    → can_recv() 读取 struct can_frame
    → 转换为 can_msg_t (项目统一格式)
    → rx_count++
    → can_ring_push() 存入环形缓冲 (dir=0, RX)
    → sock_send_frame(MSG_CAN_DATA) 打包协议帧
    → UNIX Socket → Qt UI
```

#### 数据流 B：UI → CAN 总线（发送路径）

```
Qt UI → UNIX Socket → sock_recv() 接收原始字节
    → ringbuffer_write() 写入接收环形缓冲
    → protocol_unpack() 状态机解析协议帧
    → 识别 MSG_CAN_SEND 消息
    → memset + memcpy 构造 can_msg_t (防栈垃圾)
    → can_send() 转换为 struct can_frame 写入 CAN
    → tx_count++
    → can_ring_push() 存入环形缓冲 (dir=1, TX)
```

---

## 3. 硬件平台与设备树

### 3.1 硬件平台

本模块运行在 **100ask i.MX6ULL** 开发板上，使用 SoC 内置的 **FlexCAN1** 控制器。

#### 引脚映射

| CAN 信号  | SoC 引脚      | 设备树配置                                |
| ------- | ----------- | ------------------------------------ |
| CAN1_TX | UART3_CTS_B | `MX6UL_PAD_UART3_CTS_B__FLEXCAN1_TX` |
| CAN1_RX | UART3_RTS_B | `MX6UL_PAD_UART3_RTS_B__FLEXCAN1_RX` |

#### CAN 收发器

- 供电：3.3V（由设备树 `reg_can_3v3` 调节器提供）
- 典型型号：TJA1050 或兼容 3.3V CAN 收发器
- 总线终端：CAN_H / CAN_L 两端各需 120Ω 终端电阻

### 3.2 设备树配置

从 `100ask_imx6ull-14x14.dts` 中提取的关键配置：

```dts
&flexcan1 {
    pinctrl-0 = <&pinctrl_flexcan1>;
    xceiver-supply = <&reg_can_3v3>;  /* 3.3V CAN 收发器供电 */
    status = "okay";                     /* 启用 FlexCAN1 */
};

pinctrl_flexcan1: flexcan1grp {
    fsl,pins = <
        MX6UL_PAD_UART3_CTS_B__FLEXCAN1_TX  0x000010B0
        MX6UL_PAD_UART3_RTS_B__FLEXCAN1_RX  0x000010B0
    >;
};
```

**学习要点**：

- `pinctrl-0` 指定引脚复用配置，将 UART3 的 CTS/RTS 引脚复用为 CAN 的 TX/RX
- `xceiver-supply` 关联电源调节器，内核在启用 CAN 时会自动开启收发器电源
- `status = "okay"` 覆盖默认的 disabled 状态

### 3.3 运行前的接口配置

CAN 接口需要在应用程序启动前由系统脚本配置好：

```bash
# 设置波特率 500kbps，启用 100ms 自动总线关闭恢复
ip link set can0 type can bitrate 500000 restart-ms 100

# 启用接口
ip link set can0 up

# 查看接口状态
ip -details link show can0
```

**为什么不在代码中配置？**

- 应用层通过 `socket()` + `bind()` 只能绑定到已存在且已 up 的接口
- 接口的波特率、模式等配置需要 `CAP_NET_ADMIN` 权限，通常由启动脚本以 root 执行
- `restart-ms 100` 让内核在检测到 BUS_OFF 后 100ms 自动恢复，无需应用层干预

---

## 4. SocketCAN 基础

### 4.1 什么是 SocketCAN

SocketCAN 是 Linux 内核（2.6.25+）原生的 CAN 总线协议栈。它的核心思想是：

> **将 CAN 总线抽象为网络接口**，应用层通过标准 Socket API 操作 CAN 设备。

这与传统的串口式 CAN 适配器（如 `/dev/ttyUSB0` + 厂商私有协议）有本质区别：

| 对比项   | 串口式 CAN              | SocketCAN                    |
| ----- | -------------------- | ---------------------------- |
| 设备节点  | `/dev/ttyUSB0`       | 网络接口 `can0`                  |
| API   | `open()` + `termios` | `socket()` + `bind()`        |
| 多路复用  | 需自定义线程               | 原生支持 `select`/`poll`/`epoll` |
| 多应用共享 | 通常独占                 | 多个应用可同时监听同一 CAN 接口           |
| 错误处理  | 厂商私有                 | 统一的 error frame 机制           |

### 4.2 核心 API 调用链

```c
// 1. 创建 CAN 原始套接字
int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);

// 2. 获取接口索引（"can0" → 整数索引）
struct ifreq ifr;
strncpy(ifr.ifr_name, "can0", IFNAMSIZ-1);
ioctl(fd, SIOCGIFINDEX, &ifr);

// 3. 绑定到指定接口
struct sockaddr_can addr;
addr.can_family  = AF_CAN;
addr.can_ifindex = ifr.ifr_ifindex;
bind(fd, (struct sockaddr*)&addr, sizeof(addr));

// 4. 收发 CAN 帧
struct can_frame frame;
read(fd, &frame, sizeof(frame));   // 接收
write(fd, &frame, sizeof(frame));  // 发送
```

### 4.3 struct can_frame — CAN 帧的内核表示

```c
struct can_frame {
    canid_t can_id;   // 32位 CAN ID + 标志位
    __u8    can_dlc;  // 数据长度码 (0~8)
    __u8    data[8];  // 数据字节
};
```

#### can_id 的位布局

这是 SocketCAN 中最容易出错的地方。`can_id` 是一个 32 位整数，**低 29 位存 ID，高 3 位存标志**：

```
  31  30  29  28 ──────────────────────────────── 0
┌───┬───┬───┬───────────────────────────────────────┐
│EFF│RTR│ERR│              CAN ID (29 bit)            │
│   │   │   │  (标准帧只用低 11 位，扩展帧用低 29 位)  │
└───┴───┴───┴───────────────────────────────────────┘
```

| 标志位    | 宏定义            | 值            | 含义                 |
| ------ | -------------- | ------------ | ------------------ |
| bit 31 | `CAN_EFF_FLAG` | `0x80000000` | 扩展帧标志（1=扩展帧，0=标准帧） |
| bit 30 | `CAN_RTR_FLAG` | `0x40000000` | 远程帧标志（1=远程帧，0=数据帧） |
| bit 29 | `CAN_ERR_FLAG` | `0x20000000` | 错误帧标志（1=错误帧，0=正常帧） |

#### 掩码宏

| 宏              | 值            | 用途              |
| -------------- | ------------ | --------------- |
| `CAN_SFF_MASK` | `0x000007FF` | 标准帧 ID 掩码（11 位） |
| `CAN_EFF_MASK` | `0x1FFFFFFF` | 扩展帧 ID 掩码（29 位） |
| `CAN_ERR_MASK` | `0x1FFFFFFF` | 错误码掩码           |

#### 编码示例

```c
// 标准帧 ID=0x123
frame.can_id = 0x123;  // 直接使用，EFF=0

// 扩展帧 ID=0x12345
frame.can_id = 0x12345 | CAN_EFF_FLAG;  // 必须或上 EFF 标志

// 远程帧（标准帧 ID=0x100）
frame.can_id = 0x100 | CAN_RTR_FLAG;

// 接收时判断类型
if (frame.can_id & CAN_EFF_FLAG) {
    uint32_t id = frame.can_id & CAN_EFF_MASK;  // 提取 29 位 ID
} else {
    uint32_t id = frame.can_id & CAN_SFF_MASK;  // 提取 11 位 ID
}
```

### 4.4 CAN 错误帧

当总线发生错误时，内核自动生成 **error frame** 通过 socket 推送给应用层。这不是正常的 CAN 报文，而是内核的"带内通知"。

#### 错误帧识别

```c
if (frame.can_id & CAN_ERR_FLAG) {
    // 这是错误帧，不是正常报文
}
```

#### 错误类型（can_id & CAN_ERR_MASK）

| 错误类型宏                | 含义      |
| -------------------- | ------- |
| `CAN_ERR_TX_TIMEOUT` | 发送超时    |
| `CAN_ERR_BUSOFF`     | 总线关闭    |
| `CAN_ERR_BUSERROR`   | 总线错误    |
| `CAN_ERR_CRTL`       | 控制器状态变化 |
| `CAN_ERR_CNT`        | 错误计数器更新 |

#### 错误帧数据字段

```
data[0]  — 协议错误细节 (CAN_ERR_PROT_*)
data[1]  — 协议错误位置 (CAN_ERR_PROT_LOC_*)
data[2-5] — 其他错误信息
data[6]  — TEC (发送错误计数器)
data[7]  — REC (接收错误计数器)
```

### 4.5 CAN 总线状态机

CAN 2.0 规范定义了三种错误状态，由 TEC（发送错误计数）和 REC（接收错误计数）决定：

```
                    TEC/REC ≤ 96              TEC/REC > 127
         ┌──────────────────────┐    ┌──────────────────────┐
         │   Error Active        │    │   Error Passive       │
         │   (主动错误)           │    │   (被动错误)           │
         │   正常收发，发主动错误标志│    │   可收发，发被动错误标志  │
         └──────────┬───────────┘    └──────────┬───────────┘
                    │  TEC > 96                   │  TEC > 255
                    ▼                              ▼
         ┌──────────────────────┐    ┌──────────────────────┐
         │   Error Warning       │    │     Bus Off           │
         │   (错误警告)           │    │     (总线关闭)         │
         │   仅警告，不影响收发    │    │   完全脱离总线，需恢复   │
         └──────────────────────┘    └──────────────────────┘
```

| 状态            | 条件                    | 行为                       |
| ------------- | --------------------- | ------------------------ |
| Error Active  | TEC ≤ 96 且 REC ≤ 96   | 正常工作，发送主动错误标志            |
| Error Warning | TEC > 96 或 REC > 96   | 警告状态，不影响正常收发             |
| Error Passive | TEC > 127 或 REC > 127 | 可收发，但发送被动错误标志，接收后需等待额外时间 |
| Bus Off       | TEC > 255             | 完全脱离总线，不能收发，需手动或自动恢复     |

---

## 5. 文件结构与职责划分

### 5.1 目录结构

```
canbus_daemon/
├── canbus_daemon.h      # 配置宏、数据结构定义 (can_ring_t, can_error_info_t, canbus_context_t)
├── canbus_daemon.c      # 主程序入口、主循环、环形缓冲实现、配置加载
├── can_driver.h         # SocketCAN 驱动封装头文件 (API 声明)
├── can_driver.c         # SocketCAN 驱动实现 (socket/bind/send/recv/error)
├── Makefile              # 构建脚本 (ARM交叉编译 / x86本地 / 测试)
├── README.md             # 模块说明
└── test/
    └── test_can.c        # 单元测试 (5组测试，纯软件无需硬件)
```

### 5.2 分层设计

本模块采用清晰的**两层架构**：

```
┌─────────────────────────────────────────────┐
│           canbus_daemon (业务层)              │
│  - 主事件循环 (poll)                          │
│  - 环形缓冲区管理                              │
│  - IPC 协议解析                                │
│  - 统计与日志                                  │
│  - 配置加载                                    │
├─────────────────────────────────────────────┤
│            can_driver (驱动层)                 │
│  - SocketCAN socket 创建/绑定/关闭             │
│  - can_msg_t ↔ struct can_frame 转换          │
│  - CAN 帧发送/接收                             │
│  - 错误帧解析                                   │
└─────────────────────────────────────────────┘
```

#### 分层原则

1. **驱动层不依赖业务层**：`can_driver` 只依赖 Linux 系统头文件和 `canbus_daemon.h` 中的数据类型定义，不引用任何 IPC、环形缓冲等业务逻辑
2. **业务层通过驱动层 API 操作硬件**：`canbus_daemon` 不直接调用 `socket()`/`read()`/`write()`，而是通过 `can_socket_open()`/`can_recv()`/`can_send()` 等封装函数
3. **数据格式转换集中在驱动层**：项目统一使用 `can_msg_t`，内核使用 `struct can_frame`，两者的转换只在 `can_driver.c` 中进行

### 5.3 依赖关系

```
canbus_daemon.c
    ├── canbus_daemon.h      (本模块头文件)
    ├── can_driver.h         (驱动层 API)
    ├── log.h                 (公共日志库)
    ├── config.h              (公共配置库)
    ├── socket/unix_socket.h  (UNIX Socket 封装)
    └── protocol/protocol.h   (IPC 协议帧解析)

can_driver.c
    ├── can_driver.h
    ├── canbus_daemon.h      (数据类型: can_msg_t, can_error_info_t)
    └── log.h                 (公共日志库)

canbus_daemon.h
    ├── common.h              (项目公共头: can_msg_t, ret_code_t, proc_config_t)
    ├── protocol/protocol.h   (ringbuffer_t, protocol_parser_t)
    ├── linux/can.h           (struct can_frame, CAN_RAW, 标志位宏)
    └── linux/can/error.h     (CAN_ERR_*, can_err_cntr_t)
```

---

## 6. 核心数据结构

### 6.1 can_msg_t — 项目统一 CAN 报文格式

定义在 `common.h` 中，使用 `#pragma pack(1)` 确保 1 字节对齐：

```c
#pragma pack(push, 1)
typedef struct {
    uint32_t    can_id;         /* CAN ID: 标准帧 11 位 或 扩展帧 29 位 */
    uint8_t     can_dlc;        /* 数据长度码 (DLC), 0~8 */
    uint8_t     data[8];        /* 数据字节 (最多8字节) */
    uint8_t     is_extended;    /* 是否扩展帧: 0=标准帧, 1=扩展帧 */
    uint8_t     is_remote;      /* 是否远程帧: 0=数据帧, 1=远程帧 */
} can_msg_t;
#pragma pack(pop)
```

**设计意图**：

- 与内核的 `struct can_frame` 不同，`can_msg_t` 将标志位（EFF/RTR）拆分为独立字段，更直观
- `can_id` 只存纯 ID 值，不含标志位，避免使用者混淆
- `#pragma pack(1)` 确保在 C 守护进程和 C++/Qt UI 之间二进制布局一致
- 总大小：4 + 1 + 8 + 1 + 1 = **15 字节**

### 6.2 can_ring_entry_t — 环形缓冲区条目

```c
typedef struct {
    can_msg_t   msg;            /* CAN 报文 */
    time_t      timestamp;      /* 接收/发送时间 */
    uint8_t     direction;      /* 方向: 0=接收(RX), 1=发送(TX) */
} can_ring_entry_t;
```

### 6.3 can_ring_t — CAN 报文环形缓冲区

```c
typedef struct {
    can_ring_entry_t *buffer;   /* 数据缓冲区 (动态分配) */
    int             capacity;   /* 最大容量 (CAN_RING_SIZE=200) */
    int             head;       /* 下一个写入位置 */
    int             count;      /* 当前条目数 (≤capacity) */
} can_ring_t;
```

**内存占用**：200 条 × (15 + 8 + 1) 字节 ≈ **4.8KB**，对嵌入式系统非常友好。

### 6.4 can_error_info_t — CAN 错误信息

```c
typedef struct {
    uint8_t     tec;            /* 发送错误计数 (0~255) */
    uint8_t     rec;            /* 接收错误计数 (0~255) */
    can_bus_state_t state;      /* 当前总线状态 */
} can_error_info_t;
```

### 6.5 can_bus_state_t — 总线状态枚举

```c
typedef enum {
    CAN_BUS_OK          = 0,    /* 总线正常 (Error Active) */
    CAN_BUS_WARNING     = 1,    /* 错误警告 (TEC/REC > 96) */
    CAN_BUS_PASSIVE     = 2,    /* 错误被动 (TEC/REC > 127) */
    CAN_BUS_OFF         = 3,    /* 总线关闭 (TEC > 255) */
} can_bus_state_t;
```

### 6.6 canbus_context_t — 进程上下文

这是整个守护进程的"全局状态"，所有运行时数据都集中在这个结构体中：

```c
typedef struct {
    /* CAN 设备 */
    char        can_ifname[32];     /* CAN 接口名 ("can0") */
    int         can_fd;             /* CAN socket 文件描述符 */
    int         can_ifindex;        /* CAN 接口内核索引 */

    /* 环形缓冲 */
    can_ring_t  ring;               /* CAN 报文环形缓冲 (200条) */

    /* 错误状态 */
    can_error_info_t error_info;    /* 当前错误计数和总线状态 */

    /* 统计 */
    uint32_t    rx_count;           /* 累计接收报文数 */
    uint32_t    tx_count;           /* 累计发送报文数 */
    uint32_t    err_count;          /* 累计错误帧数 */

    /* 配置 */
    proc_config_t proc_cfg;         /* 进程配置 (socket_path 等) */

    /* 运行时 */
    int         running;             /* 运行标志 */
    int         epoll_fd;            /* (预留) epoll fd */
    int         timer_fd;            /* (预留) 状态上报定时器 */
    int         signal_fd;           /* (预留) 信号 fd */
    int         server_fd;           /* UNIX Socket 服务端 fd */
    int         client_fd;           /* UNIX Socket 客户端 fd (-1=无连接) */

    /* IPC 协议 */
    protocol_parser_t proto_parser;  /* 协议帧解析器 (状态机) */
    ringbuffer_t recv_rb;            /* 接收字节流环形缓冲 (4096字节) */
} canbus_context_t;
```

**设计要点**：

- 使用全局变量 `static canbus_context_t g_ctx;` 而非动态分配，避免嵌入式环境的内存碎片
- `client_fd` 初始化为 -1，表示当前无 UI 连接
- 预留了 `epoll_fd`、`timer_fd`、`signal_fd` 字段，为未来从 poll 迁移到 epoll + timerfd + signalfd 做准备

---

## 7. 驱动层详解 — can_driver

`can_driver` 模块封装了 SocketCAN 的所有底层操作，是业务层与内核之间的桥梁。

### 7.1 can_socket_open — 创建并绑定 CAN Socket

**函数签名**：

```c
int can_socket_open(const char *ifname, int *ifindex);
```

**执行步骤**：

#### Step 1：创建 CAN 原始套接字

```c
fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
```

参数解析：

- `PF_CAN`：CAN 协议族（与 `AF_CAN` 等价）
- `SOCK_RAW`：原始套接字，直接操作 CAN 帧，不经过额外协议头
- `CAN_RAW`：原始 CAN 协议（另可选 `CAN_BCM` 广播管理器，支持周期发送和内容过滤）

#### Step 2：设置非阻塞模式

```c
int flags = fcntl(fd, F_GETFL, 0);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);
```

非阻塞模式下，`read()` 在无数据时立即返回 `-1` 且 `errno = EAGAIN`，不会阻塞主循环。

#### Step 3：获取接口索引

```c
struct ifreq ifr;
strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
ioctl(fd, SIOCGIFINDEX, &ifr);
*ifindex = ifr.ifr_ifindex;
```

`SIOCGIFINDEX` ioctl 将接口名（如 `"can0"`）转换为内核中的整数索引。也可以用更简单的 `if_nametoindex(ifname)` 函数，但代码中使用 ioctl 是为了与传统网络编程风格保持一致。

#### Step 4：绑定到指定接口

```c
struct sockaddr_can addr;
memset(&addr, 0, sizeof(addr));
addr.can_family  = AF_CAN;
addr.can_ifindex = *ifindex;
bind(fd, (struct sockaddr*)&addr, sizeof(addr));
```

绑定后，此 socket **只收发来自 can0 的帧**。如果设置 `can_ifindex = 0`，则可以接收所有 CAN 接口的帧。

#### Step 5：启用错误帧接收

```c
can_err_mask_t err_mask = CAN_ERR_MASK;
setsockopt(fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER,
           &err_mask, sizeof(err_mask));
```

**默认情况下 SocketCAN 会过滤掉错误帧**。通过设置 `CAN_RAW_ERR_FILTER` 为 `CAN_ERR_MASK`（不过滤），所有类型的错误帧都会推送到应用层。

> **注意**：代码中手动定义了 `SOL_CAN_RAW`（101）和 `CAN_RAW_ERR_FILTER`（5），因为目标平台的内核 4.9 头文件未导出这些常量。这是嵌入式开发中常见的兼容性处理。

### 7.2 can_send — 发送 CAN 报文

**函数签名**：

```c
int can_send(int fd, const can_msg_t *msg);
```

**核心逻辑 — can_msg_t → struct can_frame 转换**：

```c
struct can_frame frame;
memset(&frame, 0, sizeof(frame));

// 1. 构造 can_id (含标志位)
if (msg->is_extended) {
    frame.can_id = (msg->can_id & CAN_EFF_MASK) | CAN_EFF_FLAG;
} else {
    frame.can_id = msg->can_id & CAN_SFF_MASK;
}

// 2. 远程帧标志
if (msg->is_remote) {
    frame.can_id |= CAN_RTR_FLAG;
}

// 3. 数据长度和数据
frame.can_dlc = msg->can_dlc;
memcpy(frame.data, msg->data, msg->can_dlc);

// 4. 发送
n = write(fd, &frame, sizeof(frame));
```

**错误处理**：

| errno                    | 处理方式                           |
| ------------------------ | ------------------------------ |
| `EAGAIN` / `EWOULDBLOCK` | 发送缓冲区满，返回 0（调用者可稍后重试）          |
| `ENETDOWN`               | CAN 接口 down（可能 BUS_OFF），记录错误日志 |
| 其他                       | 记录错误日志，返回 -1                   |

### 7.3 can_recv — 接收 CAN 报文

**函数签名**：

```c
int can_recv(int fd, can_msg_t *msg);
```

**返回值约定**：

| 返回值 | 含义                     |
| --- | ---------------------- |
| > 0 | 成功接收一帧，返回读取字节数         |
| 0   | 无数据（非阻塞模式下 EAGAIN）     |
| -1  | 错误                     |
| -2  | 收到错误帧（特殊返回值，调用者应进一步解析） |

**核心逻辑**：

```c
n = read(fd, &frame, sizeof(frame));

// 1. 错误帧检测
if (frame.can_id & CAN_ERR_FLAG) {
    return -2;  // 通知调用者: 这是错误帧
}

// 2. struct can_frame → can_msg_t 转换
if (frame.can_id & CAN_EFF_FLAG) {
    msg->can_id      = frame.can_id & CAN_EFF_MASK;  // 取低29位
    msg->is_extended = 1;
} else {
    msg->can_id      = frame.can_id & CAN_SFF_MASK;  // 取低11位
    msg->is_extended = 0;
}
msg->is_remote = (frame.can_id & CAN_RTR_FLAG) ? 1 : 0;
msg->can_dlc   = frame.can_dlc;
memcpy(msg->data, frame.data, msg->can_dlc);
```

**设计亮点**：

- 使用特殊返回值 `-2` 区分错误帧和正常错误，避免额外的输出参数
- 对 `can_dlc > 8` 做了保护（`if (msg->can_dlc > 8) msg->can_dlc = 8;`），防止异常帧导致缓冲区溢出

### 7.4 can_parse_error_frame — 解析错误帧

**函数签名**：

```c
void can_parse_error_frame(const struct can_frame *frame,
                           can_error_info_t *error_info);
```

**解析逻辑**：

```c
// 1. 提取错误计数器
error_info->tec = frame->data[6];  // 发送错误计数
error_info->rec = frame->data[7];  // 接收错误计数

// 2. 根据 TEC/REC 判断总线状态
if (frame->can_id & CAN_ERR_CRTL) {
    if (tec > 127 || rec > 127)
        error_info->state = CAN_BUS_PASSIVE;
    else if (tec > 96 || rec > 96)
        error_info->state = CAN_BUS_WARNING;
    else
        error_info->state = CAN_BUS_OK;
}

// 3. 特定错误类型
if (frame->can_id & CAN_ERR_BUSOFF) {
    error_info->state = CAN_BUS_OFF;
    // 记录 BUS_OFF 严重错误
}
```

### 7.5 can_socket_close — 关闭 CAN Socket

```c
void can_socket_close(int fd) {
    if (fd >= 0) {
        close(fd);
        LOG_INFO("can", "CAN socket closed (fd=%d)", fd);
    }
}
```

---

## 8. 守护进程层详解 — canbus_daemon

### 8.1 全局变量与信号处理

```c
static canbus_context_t g_ctx;       // 进程全局上下文
static volatile int g_running = 1;   // 运行标志 (volatile 确保信号处理中可见)

static void sig_handler(int s) {
    (void)s;
    g_running = 0;  // 收到 SIGTERM/SIGINT 时设置退出标志
}
```

**设计要点**：

- `g_running` 声明为 `volatile`，因为它在信号处理函数中被修改，编译器不能优化掉对它的读取
- 信号处理函数只做最简单的事情（设置标志），不调用任何不可重入函数（如 `printf`、`malloc`）
- 主循环通过检查 `g_running` 实现优雅退出

### 8.2 初始化流程

`main()` 函数的初始化阶段按以下顺序执行：

```
Step 1: 清零全局上下文，初始化 client_fd = -1
Step 2: 初始化日志系统 (log_init)
Step 3: 加载配置 (canbus_config_load)
Step 4: 初始化 CAN 环形缓冲区 (can_ring_init, 容量200)
Step 5: 打开 SocketCAN 接口 (can_socket_open)
Step 6: 创建 UNIX Socket 服务端 (sock_create_server)
Step 7: 初始化接收环形缓冲区 (ringbuffer_init, 4096字节)
Step 8: 初始化协议解析器 (protocol_parser_init)
Step 9: 注册信号处理 (signal SIGTERM/SIGINT)
```

每一步失败都会记录日志并返回错误码，确保初始化失败时不会进入主循环。

### 8.3 canbus_config_load — 配置加载

```c
int canbus_config_load(canbus_context_t *ctx, const char *conf_file)
```

**配置项**：

| 配置项       | 配置文件节/键                | 默认值                 | 说明             |
| --------- | ---------------------- | ------------------- | -------------- |
| CAN 接口名   | `[canbus] interface`   | `can0`              | 可配置为 can0/can1 |
| Socket 路径 | 编译期宏 `CAN_SOCKET_PATH` | `/tmp/car_can.sock` | 不可通过配置文件修改     |
| 环形缓冲大小    | 编译期宏 `CAN_RING_SIZE`   | `200`               | 不可通过配置文件修改     |

**设计决策**：

- 当前 `main()` 传入 `conf_file = NULL`，全部使用默认值
- Socket 路径和缓冲大小使用编译期宏，因为它们影响内存布局和 IPC 端点，运行时修改可能导致不兼容
- 配置加载失败时静默使用默认值，不中断启动

### 8.4 清理流程

收到退出信号后，主循环结束，执行资源清理：

```
Step 1: 关闭客户端连接 (sock_close client_fd)
Step 2: 销毁接收环形缓冲区 (ringbuffer_destroy)
Step 3: 销毁 CAN 报文环形缓冲区 (can_ring_destroy)
Step 4: 关闭 UNIX Socket 服务端并删除 socket 文件 (sock_close server_fd, path)
Step 5: 关闭 CAN socket (can_socket_close)
Step 6: 关闭日志系统 (log_close)
```

**注意**：`sock_close(server_fd, CAN_SOCKET_PATH)` 会删除 `/tmp/car_can.sock` 文件，避免下次启动时 bind 失败（"Address already in use"）。

---

## 9. 环形缓冲区设计

### 9.1 设计目标

CAN 总线报文频率可能很高（500kbps 下理论每秒数千帧），UI 不可能实时处理每一条。环形缓冲区用于：

1. **缓存最近报文**：UI 连接后可以查询历史数据
2. **调试支持**：开发人员可以查看最近的 CAN 通信记录
3. **零分配**：固定大小，运行时不动态分配内存

### 9.2 数据结构

```c
typedef struct {
    can_ring_entry_t *buffer;   // 数据数组
    int capacity;                // 最大容量
    int head;                    // 下一个写入位置 (0 ~ capacity-1)
    int count;                   // 当前有效条目数 (0 ~ capacity)
} can_ring_t;
```

### 9.3 核心操作

#### can_ring_init — 初始化

```c
int can_ring_init(can_ring_t *ring, int capacity) {
    ring->buffer = (can_ring_entry_t*)calloc(capacity, sizeof(can_ring_entry_t));
    if (!ring->buffer) return RET_ERR_MEM;
    ring->capacity = capacity;
    ring->head = 0;
    ring->count = 0;
    return RET_OK;
}
```

使用 `calloc` 而非 `malloc`，自动清零内存，避免未初始化数据。

#### can_ring_push — 写入（覆盖模式）

```c
void can_ring_push(can_ring_t *ring, const can_msg_t *msg, int dir) {
    int idx = ring->head;
    memcpy(&ring->buffer[idx].msg, msg, sizeof(can_msg_t));
    ring->buffer[idx].timestamp = time(NULL);
    ring->buffer[idx].direction = (uint8_t)dir;
    ring->head = (ring->head + 1) % ring->capacity;  // 循环推进
    if (ring->count < ring->capacity) ring->count++;
}
```

**覆盖逻辑**：

- `head` 始终指向下一个写入位置
- 写入后 `head = (head + 1) % capacity`，到达末尾后回到 0
- `count` 在未满时递增，满后保持不变（最旧数据被覆盖）

#### can_ring_get — 读取（0=最新）

> 注意：头文件中声明了 `can_ring_get`，但在当前 `canbus_daemon.c` 中未找到实现。这是一个预留接口，用于 UI 查询历史报文。

预期实现逻辑：

```c
const can_ring_entry_t* can_ring_get(const can_ring_t *ring, int index) {
    if (index < 0 || index >= ring->count) return NULL;
    // index=0 → 最新 = head-1 的位置
    // index=count-1 → 最旧
    int pos = (ring->head - 1 - index + ring->capacity) % ring->capacity;
    return &ring->buffer[pos];
}
```

#### can_ring_destroy — 销毁

```c
void can_ring_destroy(can_ring_t *ring) {
    if (ring) {
        free(ring->buffer);
        ring->buffer = NULL;
    }
}
```

可安全重复调用（检查 NULL 指针）。

### 9.4 环形缓冲示意

以容量 5 为例，依次写入 ID=1,2,3,4,5,6,7：

```
写入 1,2,3,4,5 后 (count=5, 满):
┌───┬───┬───┬───┬───┐
│ 1 │ 2 │ 3 │ 4 │ 5 │   head=0 (下一个写入位置)
└───┴───┴───┴───┴───┘
 最旧              最新

写入 6 后 (覆盖位置0, count=5):
┌───┬───┬───┬───┬───┐
│ 6 │ 2 │ 3 │ 4 │ 5 │   head=1
└───┴───┴───┴───┴───┘
     最旧          最新(6在位置0, 逻辑上最新)

写入 7 后:
┌───┬───┬───┬───┬───┐
│ 6 │ 7 │ 3 │ 4 │ 5 │   head=2
└───┴───┴───┴───┴───┘
         最旧      最新
```

逻辑顺序（get 索引）：get(0)=7(最新), get(1)=6, get(2)=5, get(3)=4, get(4)=3(最旧)

---

## 10. IPC 通信协议

### 10.1 通信架构

```
┌──────────────┐    UNIX Domain Socket     ┌──────────────┐
│              │ ◄──────────────────────►  │              │
│ canbus_daemon│     /tmp/car_can.sock     │    Qt UI     │
│  (服务端)     │       SOCK_STREAM          │   (客户端)    │
│              │                            │              │
└──────────────┘                            └──────────────┘
```

- **canbus_daemon 是服务端**：创建 socket、bind、listen、accept
- **Qt UI 是客户端**：connect 到服务端
- **单客户端模式**：同时只允许一个 UI 连接，新连接会被拒绝

### 10.2 消息类型

CAN 模块相关的 IPC 消息定义在 `common.h` 中，模块 ID 为 `0x50`：

| 消息类型             | 值      | 方向       | 说明          | 数据载体               |
| ---------------- | ------ | -------- | ----------- | ------------------ |
| `MSG_CAN_DATA`   | `0x50` | can → UI | CAN 报文实时推送  | `can_msg_t` (15字节) |
| `MSG_CAN_SEND`   | `0x51` | UI → can | 发送 CAN 报文   | `can_msg_t` (15字节) |
| `MSG_CAN_FILTER` | `0x52` | UI → can | 设置过滤规则（预留）  | 自定义                |
| `MSG_CAN_STATUS` | `0x53` | can → UI | 总线状态/统计（预留） | `can_error_info_t` |

**消息类型编码方案**：1 字节，高 4 位 = 模块 ID，低 4 位 = 消息 ID。CAN 模块 ID = `0x5`，所以消息范围 `0x50 ~ 0x5F`。

### 10.3 协议帧格式

所有 IPC 消息都封装在统一的协议帧中（定义在 `protocol.h`）：

```
┌────────┬────────┬────────┬──────────┬───────────┬────────┬────────┐
│ HEAD0  │ HEAD1  │  TYPE  │   LEN    │   DATA    │  CRC8  │  TAIL  │
│  0xAA  │  0x55  │ 1 Byte │ 2 Bytes  │ 0~65535 B │ 1 Byte │  0x55  │
└────────┴────────┴────────┴──────────┴───────────┴────────┴────────┘
  偏移0    偏移1    偏移2     偏移3-4    偏移5..     偏移-2   偏移-1
```

**帧开销**：7 字节（HEAD2 + TYPE1 + LEN2 + CRC1 + TAIL1）

**LEN 字段**：使用大端序（高位字节在前），直接通过位运算构造/还原，不使用 `htons`/`ntohs`。

**为什么不用 htons？**
在 ARM 小端平台上，`htons` 返回的 `uint16_t` 内部表示仍受主机字节序影响。直接位运算 `(high << 8) | low` 完全不受主机字节序影响，C 和 Qt 两端行为一致。

### 10.4 协议解析状态机

接收方使用 5 状态有限状态机（FSM）从连续字节流中找出帧边界：

```
IDLE ──0xAA──► HEAD1 ──0x55──► HEADER ──3字节──► DATA ──LEN+CRC──► TAIL ──0x55──► (输出帧, 回IDLE)
  ↑                                                                          │
  └──────────────────── CRC错/帧尾错/长度非法 ─────────────────────────────┘
```

| 状态                  | 等待内容                      |
| ------------------- | ------------------------- |
| `PROTO_SYNC_IDLE`   | 帧头字节 0xAA                 |
| `PROTO_SYNC_HEAD1`  | 帧头字节 0x55                 |
| `PROTO_SYNC_HEADER` | TYPE(1B) + LEN(2B) = 3 字节 |
| `PROTO_SYNC_DATA`   | DATA(LEN字节) + CRC(1字节)    |
| `PROTO_SYNC_TAIL`   | 帧尾字节 0x55                 |

**断包处理**：如果数据不足，`protocol_unpack()` 返回 `-2` 并保留当前状态，下次有数据时继续解析。

**粘包处理**：`ringbuffer` 缓存所有接收数据，`while` 循环逐帧解析，直到缓冲区中没有完整帧。

### 10.5 发送路径 — MSG_CAN_DATA

当 CAN 总线收到报文时，守护进程主动推送给 UI：

```c
// canbus_daemon.c 主循环中
if (g_ctx.client_fd >= 0)
    sock_send_frame(g_ctx.client_fd, MSG_CAN_DATA,
                     (uint8_t*)&m, sizeof(m));
```

`sock_send_frame()` 内部调用 `protocol_pack()` 组帧 + `sock_send()` 发送。

### 10.6 接收路径 — MSG_CAN_SEND

UI 发送 CAN 报文的指令经过以下处理：

```c
// 1. 接收原始字节到临时缓冲区
uint8_t buf[512];
int n = sock_recv(g_ctx.client_fd, buf, sizeof(buf));

// 2. 写入接收环形缓冲
ringbuffer_write(&g_ctx.recv_rb, buf, n);

// 3. 循环解析协议帧
uint8_t type, d[256];
uint32_t l;
while (protocol_unpack(&g_ctx.proto_parser, &g_ctx.recv_rb,
                        &type, d, sizeof(d), &l) == 0) {
    if (type == MSG_CAN_SEND && l >= 13) {
        // 4. 构造 can_msg_t (关键: 防栈垃圾)
        can_msg_t m;
        memset(&m, 0, sizeof(m));
        memcpy(&m, d, l < sizeof(m) ? l : sizeof(m));

        // 5. 发送到 CAN 总线
        if (can_send(g_ctx.can_fd, &m) > 0) {
            g_ctx.tx_count++;
            can_ring_push(&g_ctx.ring, &m, 1);  // dir=1, TX
        }
    }
}
```

**为什么 `l >= 13`？**
`can_msg_t` 大小为 15 字节，但最小有效 CAN 报文（ID + DLC + 0 数据 + 标志）至少需要 13 字节（4+1+0+1+1 不对，实际是结构体的最小有效部分）。这个检查确保数据长度足够构造一个合法的 CAN 报文。

---

## 11. 主事件循环（poll 模式）

### 11.1 为什么用 poll 而不是多线程

| 方案       | 优点                | 缺点                             |
| -------- | ----------------- | ------------------------------ |
| 多线程      | 各路径独立，逻辑直观        | 线程同步复杂（互斥锁、条件变量），嵌入式资源开销大，调试困难 |
| select   | 简单通用              | 最大 fd 数限制（1024），每次调用需重置 fd_set |
| **poll** | **无 fd 数限制，接口简洁** | **大量 fd 时性能不如 epoll**          |
| epoll    | 大量 fd 时性能最优       | Linux 专属，接口稍复杂                 |

本模块只有 3~4 个 fd，poll 和 epoll 性能差异可忽略。poll 代码更简洁，且为未来迁移到 epoll 预留了字段（`epoll_fd`）。

### 11.2 poll fd 数组构建

每轮循环动态构建 pollfd 数组：

```c
struct pollfd pfds[4];
int nfds = 0;

// [0] CAN socket — 始终监听
pfds[nfds].fd = g_ctx.can_fd;
pfds[nfds].events = POLLIN;
nfds++;

// [1] UNIX Socket 服务端 — 始终监听（接受新连接）
pfds[nfds].fd = g_ctx.server_fd;
pfds[nfds].events = POLLIN;
nfds++;

// [2] UNIX Socket 客户端 — 仅当有连接时监听
if (g_ctx.client_fd >= 0) {
    pfds[nfds].fd = g_ctx.client_fd;
    pfds[nfds].events = POLLIN;
    nfds++;
}

poll(pfds, nfds, 1000);  // 超时 1000ms
```

**超时 1000ms 的意义**：即使没有任何 IO 事件，每秒也会唤醒一次，用于输出统计日志。

### 11.3 事件处理分支

#### 分支 1：CAN 报文接收（pfds[0]）

```c
if (pfds[0].revents & POLLIN) {
    can_msg_t m;
    while (can_recv(g_ctx.can_fd, &m) > 0) {  // 循环读取所有待处理帧
        g_ctx.rx_count++;
        can_ring_push(&g_ctx.ring, &m, 0);      // dir=0, RX
        if (g_ctx.client_fd >= 0)
            sock_send_frame(g_ctx.client_fd, MSG_CAN_DATA,
                             (uint8_t*)&m, sizeof(m));
    }
}
```

**为什么用 while 循环？**
poll 只通知"有数据可读"，但可能有多帧数据已到达。`can_recv()` 是非阻塞的，循环读取直到返回 0（无数据），确保一次 poll 事件处理完所有待处理帧。

#### 分支 2：接受 UI 连接（pfds[1]）

```c
if (pfds[1].revents & POLLIN) {
    int fd = sock_accept(g_ctx.server_fd);
    if (fd >= 0) {
        if (g_ctx.client_fd >= 0)
            sock_close(fd, NULL);  // 已有连接，拒绝新连接
        else {
            g_ctx.client_fd = fd;
            LOG_INFO("canbus", "UI connected");
        }
    }
}
```

**单客户端策略**：已有 UI 连接时，新连接被立即关闭。这简化了状态管理，因为车载终端通常只有一个 UI 进程。

#### 分支 3：接收 UI 发送指令（pfds[2]）

```c
if (nfds >= 3 && (pfds[2].revents & (POLLIN | POLLHUP | POLLERR))) {
    uint8_t buf[512];
    int n = sock_recv(g_ctx.client_fd, buf, sizeof(buf));
    if (n <= 0) {
        // 对端关闭或错误
        sock_close(g_ctx.client_fd, NULL);
        g_ctx.client_fd = -1;
    } else {
        // 协议解析 + MSG_CAN_SEND 处理 (详见第10.6节)
    }
}
```

监听 `POLLHUP` 和 `POLLERR` 是为了及时检测 UI 断开连接，避免资源泄漏。

#### 分支 4：每秒统计日志

```c
time_t now = time(NULL);
if (now != last) {
    last = now;
    LOG_INFO("canbus", "RX=%u TX=%u", g_ctx.rx_count, g_ctx.tx_count);
}
```

利用 `time(NULL)` 的秒级变化检测"新的一秒"，无需额外定时器。输出的是**累计计数**（不是每秒增量）。

### 11.4 主循环完整流程图

```
┌─────────────────────────────────────────────────────────┐
│                    初始化完成                              │
└────────────────────────┬────────────────────────────────┘
                         ▼
┌─────────────────────────────────────────────────────────┐
│  while (g_running)                                       │
│  ┌─────────────────────────────────────────────────────┐│
│  │ 1. 构建 pollfd 数组 (can_fd, server_fd, client_fd)  ││
│  │ 2. poll(..., 1000)  ← 阻塞等待 IO 或 1秒超时        ││
│  │                                                      ││
│  │ 3. if can_fd POLLIN:                                 ││
│  │      while can_recv() > 0:                           ││
│  │        rx_count++ → ring_push(RX) → send_to_UI      ││
│  │                                                      ││
│  │ 4. if server_fd POLLIN:                              ││
│  │      accept() → 单客户端模式(新连接踢旧连接)           ││
│  │                                                      ││
│  │ 5. if client_fd POLLIN/HUP/ERR:                     ││
│  │      recv() → ringbuffer_write → protocol_unpack    ││
│  │      if MSG_CAN_SEND: memset+memcpy → can_send      ││
│  │                  → tx_count++ → ring_push(TX)        ││
│  │      if n<=0: close client, client_fd=-1             ││
│  │                                                      ││
│  │ 6. if 秒级时间变化: LOG_INFO RX/TX 统计              ││
│  └─────────────────────────────────────────────────────┘│
└────────────────────────┬────────────────────────────────┘
                         ▼ (g_running = 0)
┌─────────────────────────────────────────────────────────┐
│                    资源清理 + 退出                         │
└─────────────────────────────────────────────────────────┘
```

---

## 12. 编译与构建系统

### 12.1 Makefile 结构

```makefile
# 交叉编译工具链
CROSS_COMPILE ?= arm-buildroot-linux-gnueabihf-
CC = $(CROSS_COMPILE)gcc

# 源文件
SRCS  = canbus_daemon.c
SRCS += can_driver.c

# 编译选项
CFLAGS  = -Wall -Wextra -std=gnu99 -Os -g
CFLAGS += -ffunction-sections -fdata-sections   # 函数/数据独立段，配合链接时垃圾回收
CFLAGS += -I$(COMMON_INC) ...                    # 头文件搜索路径

# 链接选项
LDFLAGS = -Wl,--gc-sections -lpthread -lrt -lm  # --gc-sections 回收未使用段
```

### 12.2 编译目标

| 命令            | 说明                                                            |
| ------------- | ------------------------------------------------------------- |
| `make`        | ARM 交叉编译（默认），输出到 `build/canbus_daemon`，复制到 `../../build/bin/` |
| `make HOST=1` | x86 本地编译（需 Linux + can0 存在，用于开发调试）                            |
| `make test`   | 编译并运行单元测试（`-DTEST_MODE`）                                      |
| `make clean`  | 清理 build 目录                                                   |

### 12.3 编译优化策略

- `-Os`：优化代码大小（嵌入式环境 Flash 有限）
- `-g`：保留调试信息（便于 GDB 调试，发布时可 strip）
- `-ffunction-sections -fdata-sections` + `-Wl,--gc-sections`：链接时移除未使用的函数和数据，减小二进制体积
- `-std=gnu99`：使用 C99 标准 + GNU 扩展（如 `//` 注释、内联函数）

### 12.4 依赖库

- `libcarcommon.a`：项目公共库（日志、配置、协议、Socket 封装、环形缓冲）
- `-lpthread`：POSIX 线程库（公共库可能依赖）
- `-lrt`：实时扩展库（定时器、共享内存）
- `-lm`：数学库

---

## 13. 单元测试

### 13.1 测试框架

`test/test_can.c` 实现了一个轻量级的单元测试框架，不依赖任何外部测试库：

```c
#define TEST_START(n)   printf("\n========== [TEST] %s ==========\n", n)
#define TEST_CASE(d)    do { g_total++; printf("  [%d] %s ... ", g_total, d); } while(0)
#define TEST_OK()       do { printf("OK\n"); g_passed++; } while(0)
#define TEST_FAIL(m)    do { printf("FAIL: %s\n", m); g_failed++; return; } while(0)
#define TEST_ASSERT(c,m) do { if(!(c)) TEST_FAIL(m); } while(0)
```

### 13.2 测试用例

#### 测试 1：环形缓冲区基础

- `can_ring_init(100)` 初始化成功，容量正确，初始 count=0
- push 1 条后 count=1，get(0) 返回正确数据，direction=RX
- push 3 条后 count=4，FIFO 顺序正确（get(0)=最新，get(3)=最旧）

#### 测试 2：环形缓冲区覆盖

- 容量 5，写入 7 条，count 封顶为 5
- 最旧的两条（ID=1,2）被覆盖，当前最旧为 ID=3
- 最新为 ID=7

#### 测试 3：CAN ID 位标志

- 标准帧 ID=0x7FF：无 EFF 标志，SFF_MASK 保留全部 11 位
- 扩展帧 ID=0x1FFFFFFF | CAN_EFF_FLAG：有 EFF 标志，EFF_MASK 保留 29 位
- 错误帧：CAN_ERR_FLAG = 0x20000000，可正确检测

#### 测试 4：方向标志

- direction=0 (RX) 存储为 0
- direction=1 (TX) 存储为 1

#### 测试 5：配置加载

- 创建临时配置文件 `[canbus] interface=can1`
- 加载后 `ctx.can_ifname == "can1"`

### 13.3 运行测试

```bash
make test
```

测试不需要 CAN 硬件，纯软件逻辑验证，可在 x86 开发机上运行。

---

## 14. 已知 Bug 与修复记录

### 14.1 [已修复] 栈垃圾污染 can_msg_t 标志位

**严重程度**：严重

**问题描述**：

在处理 UI 的 `MSG_CAN_SEND` 指令时，原先使用直接类型转换将协议数据缓冲区转换为 `can_msg_t*`：

```c
// 错误写法
can_msg_t *m = (can_msg_t*)d;
can_send(g_ctx.can_fd, m);
```

但 `can_msg_t` 结构体因编译器对齐/填充可能包含未初始化的栈字节。虽然 `can_msg_t` 使用了 `#pragma pack(1)`，但在某些情况下（如数据长度小于结构体大小、或缓冲区末尾有未初始化字节），`is_extended` / `is_remote` 等标志位可能被栈垃圾随机置位。

**症状**：

- Qt 发送标准帧，CAN 总线上却出现扩展帧
- 随机出现远程帧
- 问题间歇性出现，难以复现

**修复方案**：

```c
// 正确写法
can_msg_t m;
memset(&m, 0, sizeof(m));                    // 先清零整个结构体
memcpy(&m, d, l < sizeof(m) ? l : sizeof(m)); // 逐字节拷贝，不超过结构体大小
can_send(g_ctx.can_fd, &m);
```

**修复原理**：

1. `memset` 确保所有字段（包括未显式设置的）初始化为 0
2. `memcpy` 只拷贝协议数据中实际存在的字节，限制拷贝量不超过 `sizeof(can_msg_t)` 防止溢出
3. 未被数据覆盖的字段保持为 0（即标准帧、数据帧）

**位置**：`canbus_daemon.c` 的 `main()` 函数中，处理 `MSG_CAN_SEND` 的分支。

**经验教训**：

- 永远不要假设结构体的所有字段都会被显式初始化
- 从外部数据源（网络、文件、IPC）构造结构体时，先清零再拷贝是安全的做法
- `#pragma pack(1)` 只解决对齐问题，不解决未初始化数据问题

---

## 15. 学习路径建议

### 15.1 前置知识

学习本模块前，建议掌握以下知识：

| 知识领域       | 关键概念                              | 推荐资源             |
| ---------- | --------------------------------- | ---------------- |
| C 语言       | 指针、结构体、位运算、内存管理                   | 《C Primer Plus》  |
| Linux 系统编程 | socket、bind、read/write、poll、ioctl | 《UNIX 环境高级编程》    |
| Linux 网络   | 网络接口、ifreq、sockaddr               | 《TCP/IP 详解》      |
| CAN 总线     | CAN 2.0 协议、帧格式、错误处理               | 《CAN 总线原理与应用》    |
| 嵌入式 Linux  | 设备树、交叉编译、内核模块                     | 《嵌入式 Linux 应用开发》 |

### 15.2 分阶段学习路线

#### 阶段 1：理解 CAN 总线基础（1~2 天）

1. 学习 CAN 2.0 协议：标准帧/扩展帧格式、仲裁机制、CRC、ACK
2. 理解 CAN 错误处理：TEC/REC、错误状态机、错误帧
3. 在 PC 上用虚拟 CAN 接口（vcan）做实验：
   
   ```bash
   sudo modprobe vcan
   sudo ip link add dev vcan0 type vcan
   sudo ip link set vcan0 up
   candump vcan0          # 终端1: 监听
   cansend vcan0 123#1122334455667788  # 终端2: 发送
   ```

#### 阶段 2：学习 SocketCAN（1~2 天）

1. 阅读 Linux 内核文档 `Documentation/networking/can.rst`
2. 理解 `struct can_frame` 的 can_id 位布局
3. 编写最简单的 CAN 收发程序（参考 `can_driver.c`）
4. 学习错误帧的接收和解析

#### 阶段 3：阅读驱动层代码（1 天）

1. 精读 `can_driver.h`：理解 API 设计
2. 精读 `can_driver.c`：
   - `can_socket_open()`：socket 创建、非阻塞、接口索引、bind、错误帧过滤
   - `can_send()`：can_msg_t → can_frame 转换
   - `can_recv()`：can_frame → can_msg_t 转换，错误帧识别
   - `can_parse_error_frame()`：错误帧解析

#### 阶段 4：阅读守护进程层代码（1~2 天）

1. 精读 `canbus_daemon.h`：理解数据结构设计
2. 精读 `canbus_daemon.c`：
   - 初始化流程
   - 环形缓冲区实现
   - 主循环 poll 事件处理
   - IPC 协议解析
3. 理解单客户端模式、优雅退出、资源清理

#### 阶段 5：学习公共依赖（1 天）

1. `common.h`：项目统一数据结构、消息类型编码
2. `protocol.h`：二进制协议帧格式、状态机解析
3. `unix_socket.h`：UNIX Domain Socket 封装
4. `ringbuffer`：字节流环形缓冲

#### 阶段 6：动手实践（2~3 天）

1. 在 x86 上用 vcan 编译运行 `make HOST=1`
2. 运行单元测试 `make test`
3. 用 `candump`/`cansend` 模拟 CAN 报文，观察守护进程行为
4. 编写一个简单的 UI 客户端，连接 `/tmp/car_can.sock`，发送 `MSG_CAN_SEND` 指令
5. 尝试添加 `MSG_CAN_STATUS` 消息的实现（预留接口）

### 15.3 扩展学习方向

掌握本模块后，可以进一步探索：

1. **CAN FD**：支持更长数据载荷（64 字节）和更高波特率，`struct canfd_frame`
2. **CAN 网关**：`cangw` 内核模块，实现 can0 ↔ can1 路由
3. **BCM 协议**：`CAN_BCM` 广播管理器，支持周期发送和内容变化检测
4. **DBC 文件解析**：CAN 数据库文件，定义信号的物理值转换
5. **UDS 诊断**：ISO 14229 统一诊断服务，汽车 ECU 诊断协议
6. **从 poll 迁移到 epoll**：利用预留的 `epoll_fd` 字段，提升大规模连接性能
7. **多客户端支持**：改造单客户端模式，支持多个 UI 同时连接

---

## 附录 A：关键宏定义速查

| 宏                       | 值                                    | 定义位置            | 说明              |
| ----------------------- | ------------------------------------ | --------------- | --------------- |
| `CAN_DEFAULT_INTERFACE` | `"can0"`                             | canbus_daemon.h | 默认 CAN 接口名      |
| `CAN_DEFAULT_BITRATE`   | `500000`                             | canbus_daemon.h | 默认波特率 (bps)     |
| `CAN_RING_SIZE`         | `200`                                | canbus_daemon.h | 环形缓冲容量 (条)      |
| `CAN_SOCKET_PATH`       | `"/tmp/car_can.sock"`                | canbus_daemon.h | UNIX Socket 路径  |
| `CAN_LOG_PATH`          | `"/var/log/car_terminal/canbus.log"` | canbus_daemon.h | 日志文件路径          |
| `CAN_EFF_FLAG`          | `0x80000000`                         | linux/can.h     | 扩展帧标志位          |
| `CAN_RTR_FLAG`          | `0x40000000`                         | linux/can.h     | 远程帧标志位          |
| `CAN_ERR_FLAG`          | `0x20000000`                         | linux/can.h     | 错误帧标志位          |
| `CAN_SFF_MASK`          | `0x000007FF`                         | linux/can.h     | 标准帧 ID 掩码 (11位) |
| `CAN_EFF_MASK`          | `0x1FFFFFFF`                         | linux/can.h     | 扩展帧 ID 掩码 (29位) |
| `MSG_CAN_DATA`          | `0x50`                               | common.h        | CAN 报文推送消息      |
| `MSG_CAN_SEND`          | `0x51`                               | common.h        | CAN 发送指令消息      |
| `MSG_CAN_FILTER`        | `0x52`                               | common.h        | CAN 过滤规则消息 (预留) |
| `MSG_CAN_STATUS`        | `0x53`                               | common.h        | CAN 状态消息 (预留)   |

## 附录 B：函数索引

### can_driver (驱动层)

| 函数                        | 文件           | 功能                                                           |
| ------------------------- | ------------ | ------------------------------------------------------------ |
| `can_socket_open()`       | can_driver.c | 创建并绑定 CAN Socket（非阻塞）                                        |
| `can_send()`              | can_driver.c | 发送 CAN 报文（can_msg_t → can_frame） |
| `can_recv()`              | can_driver.c | 接收 CAN 报文（can_frame → can_msg_t）                             |
| `can_parse_error_frame()` | can_driver.c | 解析错误帧，提取 TEC/REC/总线状态                                        |
| `can_socket_close()`      | can_driver.c | 关闭 CAN Socket                                                |

### canbus_daemon (业务层)

| 函数                     | 文件              | 功能                  |
| ---------------------- | --------------- | ------------------- |
| `main()`               | canbus_daemon.c | 进程入口，初始化 + 主循环 + 清理 |
| `can_ring_init()`      | canbus_daemon.c | 初始化环形缓冲区            |
| `can_ring_push()`      | canbus_daemon.c | 向环形缓冲区写入一条报文        |
| `can_ring_count()`     | canbus_daemon.c | 获取当前条目数             |
| `can_ring_destroy()`   | canbus_daemon.c | 释放环形缓冲区             |
| `canbus_config_load()` | canbus_daemon.c | 加载配置（接口名等）          |

---

> **文档版本**：v1.0
> **基于代码**：canbus_daemon 模块（canbus_daemon.c/h, can_driver.c/h, Makefile, test/test_can.c）
> **最后更新**：2026-08-22
