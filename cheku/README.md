# 智能车载终端 (cheku)

基于 **NXP i.MX6ULL** (ARM Cortex-A7) 的嵌入式车载终端系统。采用**多进程 + UNIX Domain Socket** 架构：6 个 C 语言守护进程各司其职，通过统一的二进制协议与 Qt 触控界面通信。

- **硬件平台**：i.MX6ULL，ARM Cortex-A7 528MHz / 512MB DDR3 / 4GB eMMC，LCD 1024x600 触摸屏
- **软件栈**：嵌入式 Linux 4.9.88 (Buildroot) + C (gnu99) + Qt 5 (Widgets)
- **代码规模**：约 40 个源文件，C 与 Qt/C++ 混合

---

## 系统架构

```
                    ┌──────────────────────────┐
                    │      Qt 触控界面 (car_ui) │
                    │  Qt5 Widgets, 1024x600   │
                    └────────────┬─────────────┘
                                 │  UNIX Domain Socket
                                 │  /tmp/car_*.sock
        ┌────────────┬───────────┼───────────┬────────────┐
        │            │           │           │            │
   ┌────▼───┐  ┌─────▼───┐ ┌─────▼───┐ ┌─────▼───┐  ┌─────▼───┐
   │  gps   │  │   dvr   │ │   av    │ │  input  │  │ canbus  │
   │ daemon │  │ daemon  │ │ daemon  │ │ daemon  │  │ daemon  │
   │ GPS定位│  │ 行车记录│ │ 音频播放│ │ 按键输入│  │ CAN 总线│
   └────────┘  └─────────┘ └─────────┘ └─────────┘  └─────────┘
        │            │           │           │            │
    /dev/ttymxc5  /dev/video1  ALSA hw   /dev/input    can0
                                                    (SocketCAN)

   ┌──────────────────────────────────────────────────────────┐
   │  guard_daemon — 进程守护与心跳监控 (编译产出, 部署脚本待接入) │
   └──────────────────────────────────────────────────────────┘
```

**IPC 协议**：自定义二进制帧 `0xAA 0x55 | TYPE | LEN(2B) | DATA | CRC8 | 0x55`，
使用 5 状态有限状态机解析，配合环形缓冲区处理粘包/断包。详见 `include/protocol/protocol.h`。

---

## 目录结构

```
cheku/
├── src/                # 源代码 (.c/.cpp) — 业务实现
│   ├── common/         #   公共库: log / crc / config / protocol / ringbuffer / socket / timer
│   ├── guard_daemon/   #   进程守护
│   ├── gps_daemon/     #   GPS 定位 (含 NMEA 解析、UART 工具)
│   ├── input_daemon/   #   按键输入 (含按键解析、LED 控制)
│   ├── canbus_daemon/  #   CAN 总线 (含 SocketCAN 驱动封装)
│   ├── av_daemon/      #   音频播放 (ALSA)
│   └── dvr_daemon/     #   行车记录 (V4L2 采集 + 存储管理)
│
├── include/            # 公共头文件 (.h) — 与 src 分离, 通过 -Iinclude 引用
│   ├── common.h        #   公共类型、消息类型、返回码
│   ├── protocol/       #   通信协议
│   ├── socket/         #   UNIX Socket 封装
│   ├── log/  config/  crc/  ringbuffer/  timer/
│   └── ...
│
├── tests/              # 单元测试 — 全项目测试统一入口
│   ├── Makefile        #   make -C tests 编译并运行全部测试
│   ├── test_common.c   #   公共库测试
│   ├── test_gps.c      #   NMEA 解析测试
│   ├── test_input.c    #   按键解析测试
│   ├── test_can.c      #   CAN 协议测试
│   ├── test_guard.c    #   守护逻辑测试
│   └── test_dvr.c      #   存储管理测试
│
├── config/             # 运行时配置 (config.ini)
├── docs/               # 文档
│   ├── architecture.md #   架构设计
│   ├── 学习文档.md     #   完整学习文档
│   ├── 调试问题记录.md #   编译调试踩坑记录
│   └── modules/        #   各模块学习文档 (gps / dvr / canbus / qt_ui ...)
│
├── scripts/            # 脚本: 编译部署、服务启停
├── qt_ui/              # Qt 触控界面工程 (qmake)
│
├── Makefile            # 顶层构建脚本
├── .gitignore
├── .gitattributes      # 强制 LF, 防止 .sh 在 Windows 被转为 CRLF
└── build/              # 编译输出 (不入库)
```

---

## 快速开始

### 1. 环境准备（ARM 交叉编译）

```bash
export ARCH=arm
export CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
export PATH=$PATH:/home/book/100ask_imx6ull-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/bin
```

### 2. 编译

```bash
make            # 编译全部模块 (ARM), 产物在 build/bin/
make HOST=1     # x86 本地编译
make common     # 只编译公共库
make gps        # 只编译 gps_daemon (别名: guard/input/can/av/dvr)
make help       # 查看全部目标
```

### 3. 运行单元测试

```bash
make test               # 编译并运行全部单元测试 (x86 本地)
make -C tests test_gps  # 只运行某一个测试
```

> 测试程序需要在本机直接执行，因此 `make test` 固定使用本地编译器。

### 4. 部署到开发板

```bash
bash scripts/build_and_deploy.sh   # 一键交叉编译 + adb 推送
```

在开发板上：

```bash
chmod +x /usr/bin/*.sh /usr/bin/*_daemon /usr/bin/car_ui
start_all.sh      # 启动全部服务
stop_all.sh       # 停止全部服务
```

---

## 模块一览

| 模块 | 职责 | 关键外设/技术 |
|---|---|---|
| `gps_daemon` | GPS 定位数据采集与上报 | UART5 (`/dev/ttymxc5`), NMEA-0183 解析 |
| `dvr_daemon` | 行车记录仪录像与存储管理 | V4L2 视频采集, 分段存储与循环覆盖 |
| `av_daemon` | 音频播放控制 | ALSA (`hw:0,0`) |
| `input_daemon` | 物理按键事件采集 | Linux input 子系统, sysfs LED |
| `canbus_daemon` | CAN 总线数据收发 | SocketCAN (`can0`, 500kbps) |
| `guard_daemon` | 子进程心跳监控与自动重启 | `/proc` 解析, 进程管理 |
| `qt_ui` | 触控人机界面 | Qt5 Widgets, 自绘仪表盘控件 |

---

## 公共库 (src/common)

编译产出 `libcarcommon.a`，供所有守护进程链接：

| 子模块 | 功能 |
|---|---|
| `log` | 分级日志 (DEBUG/INFO/WARN/ERROR)，文件大小轮转，线程安全 |
| `protocol` | 二进制帧组包/拆包，5 状态机，CRC8 校验 |
| `socket` | UNIX Domain Socket 封装，非阻塞 IO |
| `ringbuffer` | 环形缓冲区，解决流式数据粘包/断包 |
| `config` | INI 配置文件解析 |
| `crc` | CRC8 查表法校验 |
| `timer` | 基于 `timerfd` 的定时器封装 |

---

## 文档

| 文档 | 内容 |
|---|---|
| [docs/architecture.md](docs/architecture.md) | 架构设计（**注意：文档开头注明部分内容已过时**） |
| [docs/学习文档.md](docs/学习文档.md) | 完整学习文档，含各模块实现细节 |
| [docs/调试问题记录.md](docs/调试问题记录.md) | 从首次编译到运行成功的问题与解决方法 |
| [docs/modules/](docs/modules/) | 各模块专项学习文档 |
| [docs/项目简历.md](docs/项目简历.md) | 项目简介（面向求职） |

---

## 已知问题

- `guard_daemon` 已实现并纳入编译，但 `scripts/start_all.sh` 与 `stop_all.sh` 尚未接入该进程。
- `docs/architecture.md` 开头已声明：架构相比设计文档有所简化（`sensor_daemon` 被 Qt 的 `SensorThread` 替代，`net_daemon` 未运行）。
- `scripts/` 中部分路径（项目目录、工具链路径）为硬编码，换机器需手动调整。
- `dvr_daemon` 的 `v4l2_capture.h` 等模块暂无独立单元测试（测试通过 `tests/test_dvr.c` 覆盖存储管理部分）。

---

## 构建系统说明

项目使用**手写 Makefile**（非 CMake），分层结构：

- 顶层 `Makefile`：调度各模块，提供 `all / test / clean / deploy / help`
- `src/common/Makefile`：编译公共静态库（支持 `make shared` 生成动态库）
- `src/*/Makefile`：各守护进程独立编译
- `tests/Makefile`：统一编译并运行全部单元测试

**关于测试与 `main()` 冲突**：部分测试需链接守护进程源文件（如 `test_guard.c` 需要 `guard_daemon.c` 中的函数），但两者都定义了 `main()`。`tests/Makefile` 在编译这些源文件时通过 `-Dmain=<file>_main_disabled` 将其 `main` 重命名，从而零侵入地解决冲突，无需修改任何源码。
