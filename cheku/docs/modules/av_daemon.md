# av_daemon 模块学习文档

> 音频播放守护进程 — 基于 ALSA 的嵌入式 WAV 播放器完整解析

---

## 目录

1. [模块概述与定位](#1-模块概述与定位)
2. [整体架构设计](#2-整体架构设计)
3. [文件结构与职责划分](#3-文件结构与职责划分)
4. [核心数据结构与全局状态](#4-核心数据结构与全局状态)
5. [初始化流程详解](#5-初始化流程详解)
6. [主事件循环](#6-主事件循环)
7. [播放状态机与控制逻辑](#7-播放状态机与控制逻辑)
8. [WAV 文件解析与播放列表](#8-wav-文件解析与播放列表)
9. [ALSA 音频核心层](#9-alsa-音频核心层)
10. [IPC 通信机制](#10-ipc-通信机制)
11. [音量控制](#11-音量控制)
12. [构建系统](#12-构建系统)
13. [与系统其他模块的关系](#13-与系统其他模块的关系)
14. [设计权衡与改进思考](#14-设计权衡与改进思考)

---

## 1. 模块概述与定位

### 1.1 它是什么

`av_daemon` 是智能车载终端系统中的**音频播放后台守护进程**。它以独立进程的形式运行在后台，负责：

- 扫描音乐目录中的 WAV 文件，构建内存播放列表
- 将 WAV 中的 PCM 数据写入 ALSA 硬件设备进行播放
- 通过 UNIX Domain Socket 接收 Qt UI 发来的播放控制指令（播放/暂停/停止）
- 通过共享文件与 UI 同步音量和当前曲目信息

### 1.2 在系统中的位置

整个车载终端采用**微服务风格的多进程架构**，各进程通过 UNIX Domain Socket 进行 IPC 通信：

```
┌─────────────────────────────────────────────────────────┐
│                    guard_daemon (父进程)                  │
│          统一管理所有子进程的生命周期与重启策略              │
└──────┬──────────┬──────────┬──────────┬────────────────┘
       │          │          │          │
  ┌────▼───┐ ┌────▼───┐ ┌────▼───┐ ┌───▼────┐
  │gps_    │ │dvr_    │ │av_     │ │input_  │ ...
  │daemon  │ │daemon  │ │daemon  │ │daemon  │
  └────┬───┘ └────┬───┘ └────┬───┘ └───┬────┘
       │           │           │           │
       └───────────┴─────┬─────┴───────────┘
                         │
                  ┌──────▼──────┐
                  │   Qt UI     │  (用户交互界面)
                  └─────────────┘
```

`av_daemon` 是其中负责**音频播放**的子系统，消息类型编码为 `0x60 ~ 0x6F`（高 4 位模块 ID = `0x6`）。

### 1.3 硬件目标

- **编解码器**：WM8960（I2C2 接口，地址 0x1a）
- **CPU DAI**：SAI2
- **主时钟**：12.288 MHz
- **支持采样率**：8k / 11.025k / 12k / 16k / 22.05k / 24k / 32k / 44.1k / 48k
- **输出通道**：喇叭（SPK_L/R）+ 耳机（HP_L/R）
- **ALSA 设备名**：`hw:0,0`

---

## 2. 整体架构设计

### 2.1 架构总览

```
┌──────────────────────────────────────────────────────────────┐
│                        av_daemon 进程                          │
│                                                                │
│  ┌──────────────┐    ┌────────────────────────────────────┐  │
│  │  初始化层     │    │           主事件循环 (poll)         │  │
│  │              │    │                                    │  │
│  │ · 日志系统   │    │  ┌─────────┐  ┌────────────────┐ │  │
│  │ · 混音器     │    │  │Socket   │  │ 音量轮询       │ │  │
│  │ · UNIX Socket│    │  │指令处理 │  │ (每2秒)        │ │  │
│  │ · 协议解析器 │    │  └────┬────┘  └────────────────┘ │  │
│  │ · 信号处理   │    │       │                            │  │
│  │ · 音乐扫描   │    │  ┌────▼─────────────────────────┐ │  │
│  └──────────────┘    │  │     play_chunk()             │ │  │
│                       │  │  (每次喂 1024 帧 PCM 数据)   │ │  │
│                       │  └──────────────────────────────┘ │  │
│                       └────────────────────────────────────┘  │
│                                                                │
│  ┌──────────────────────────────────────────────────────────┐ │
│  │                    audio_core (ALSA 封装层)                │ │
│  │  · audio_playback_open/write/pause/close                 │ │
│  │  · audio_mixer_open/set_volume/set_switch/close          │ │
│  └──────────────────────────────────────────────────────────┘ │
│                                                                │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐   │
│  │  /music/     │  │  ALSA 硬件   │  │  /tmp/ 共享文件  │   │
│  │  WAV 文件    │  │  (hw:0,0)    │  │  av_volume       │   │
│  │              │  │              │  │  av_track         │   │
│  └──────────────┘  └──────────────┘  └──────────────────┘   │
└──────────────────────────────────────────────────────────────┘
```

### 2.2 核心设计决策

| 决策 | 选择 | 原因 | 代价 |
|------|------|------|------|
| 线程模型 | **单线程 + poll 事件循环** | 避免多线程同步复杂度，嵌入式资源有限 | 播放阻塞可能影响指令响应延迟 |
| 数据加载 | **整文件读入内存** | 实现简单，播放期间无磁盘 I/O | 长曲目占用较多内存（一首 4 分钟 44.1kHz 16bit 立体声 ≈ 40MB） |
| 写入方式 | **snd_pcm_writei 阻塞模式** | 与 `aplay` 逻辑一致，稳定可靠 | 写入期间无法处理其他事件 |
| IPC 方式 | **UNIX Domain Socket + 共享文件** | Socket 传控制指令，文件传状态，职责分离 | 两种机制需要分别维护 |
| 音量同步 | **轮询 /tmp/av_volume（每 2 秒）** | 实现极简，无需双向 Socket | 音量调节有最多 2 秒延迟 |

### 2.3 数据流

```
WAV 文件 → open_track() 解析头部 → play_chunk() 首次调用加载全部 PCM 到堆
    → 每次循环 snd_pcm_writei() 写入 1024 帧 → ALSA 硬件缓冲区 → DAC → 喇叭/耳机

Qt UI → UNIX Socket (/tmp/car_av.sock) → protocol_unpack() 解码 → handle_sockets()
    → 修改 g_state → 主循环根据状态决定是否调用 play_chunk()

Qt UI → 写 /tmp/av_volume → 主循环每 2 秒读取 → audio_mixer_set_volume() → WM8960 寄存器
av_daemon → 写 /tmp/av_track → Qt UI 读取显示当前曲目名
```

---

## 3. 文件结构与职责划分

### 3.1 文件清单

```
av_daemon/
├── av_daemon.h          # 模块配置常量 & 数据结构声明
├── av_daemon.c          # 主程序：事件循环、播放控制、Socket IPC、播放列表
├── audio_core.h         # ALSA 音频核心接口声明
├── audio_core.c         # ALSA PCM 播放 & 混音器控制实现
├── Makefile             # 构建脚本（支持 ARM 交叉编译 & x86 本地编译）
├── README.md            # 简要说明
└── test/                # 测试目录（当前为空）
```

### 3.2 各文件职责

#### av_daemon.h — 配置与常量

定义模块级别的编译期常量，包括：
- 默认 PCM 设备名、采样率、声道数、音量
- 音乐目录路径、Socket 路径、日志路径

#### av_daemon.c — 主程序

模块的核心，包含：
- 所有全局状态变量
- 音乐目录扫描 `scan_music()`
- 曲目打开与 WAV 头部解析 `open_track()`
- 播放停止 `play_stop()`
- PCM 数据块播放 `play_chunk()`
- Socket 指令处理 `handle_sockets()`
- 主函数 `main()`（初始化 + 事件循环）

#### audio_core.h / audio_core.c — ALSA 封装层

将 alsa-lib 的复杂 API 封装为简洁的接口：
- **PCM 播放**：`audio_playback_open/write/pause/close`
- **混音器控制**：`audio_mixer_open/set_volume/set_switch/list_controls/close`

这一层的设计目标是：让 `av_daemon.c` 不需要直接接触 ALSA 的底层细节。

---

## 4. 核心数据结构与全局状态

### 4.1 为什么用全局变量

本模块采用**全局变量 + 单线程**的设计，而非将状态封装到 context 结构体中。原因：

1. 单线程环境下不存在竞态条件
2. 代码更简洁，函数间无需传递 context 指针
3. 嵌入式场景下模块功能单一，不需要多实例

### 4.2 全局变量分类

#### ALSA 硬件句柄

```c
static snd_pcm_t   *g_pcm   = NULL;   // PCM 播放句柄 (hw:0,0)
static snd_mixer_t *g_mixer = NULL;   // 混音器句柄 (音量控制)
```

- `g_pcm`：打开后在整个进程生命周期内复用，不会每首歌重新打开
- `g_mixer`：用于设置硬件音量，启动时打开一次

#### 进程生命周期

```c
static volatile int g_running = 1;     // 主循环运行标志
```

- `volatile` 关键字确保编译器不会优化掉对该变量的读取
- 由 SIGTERM/SIGINT 信号处理函数置 0

#### 播放列表

```c
#define MAX_TRACKS 64
static char  g_tracks[MAX_TRACKS][256]; // .wav 文件路径数组
static int   g_track_count = 0;          // 有效曲目数
static int   g_current     = -1;         // 当前曲目索引 (-1 = 无)
```

- 固定大小数组，最多 64 首，每首路径最长 255 字节
- 启动时扫描 `/music/` 一次性填充，运行期间不动态更新

#### 当前曲目状态

```c
static FILE *g_fp     = NULL;   // 当前打开的 .wav 文件句柄
static int   g_state  = 0;      // 播放状态: 0=停止, 1=播放中, 2=暂停
static int   g_volume = 100;    // 当前音量 (0-100)
```

`g_state` 是整个播放控制的核心变量，状态机的转换都围绕它进行。

#### PCM 格式参数

```c
static int g_sr;     // 采样率 (从 WAV 头部解析)
static int g_ch;     // 声道数
static int g_bits;   // 每采样位数
```

这些参数在 `open_track()` 中从 WAV 文件头部读取，用于配置 ALSA 设备和计算帧数。

#### IPC 相关

```c
static int g_server_fd = -1;   // 监听 socket fd
static int g_client_fd = -1;   // 已连接的 UI 客户端 fd (-1 = 无客户端)
```

- 单客户端策略：同一时间只允许一个 UI 连接
- 新客户端到来时如果已有连接，直接拒绝（关闭新 fd）

#### PCM 数据缓冲区

```c
static void *g_pcm_data = NULL;   // 堆缓冲区，存储全部 PCM 采样数据
static int   g_pcm_total = 0;     // 总帧数
static int   g_pcm_pos   = 0;     // 下一个要写入 ALSA 的帧索引
```

这是"整文件读入内存"设计的核心：
- `g_pcm_data`：指向 malloc 分配的堆内存，存放整首歌的原始 PCM 数据
- `g_pcm_total`：总帧数 = 数据字节数 / (声道数 × 每采样字节数)
- `g_pcm_pos`：播放进度指针，每次写入后递增

#### 协议解析

```c
static protocol_parser_t g_pp;   // Socket 消息帧解析器
static ringbuffer_t      g_rb;   // 环形缓冲区 (接收原始字节)
```

来自 common 库的协议模块，用于从 Socket 字节流中解析出完整的消息帧。

---

## 5. 初始化流程详解

### 5.1 初始化顺序

`main()` 函数的初始化按照严格的依赖顺序执行：

```
main()
  │
  ├─ 1. log_init()              ← 日志系统（最先初始化，后续所有步骤都能写日志）
  │
  ├─ 2. audio_mixer_open()      ← 打开 ALSA 混音器
  │     ├─ audio_mixer_set_volume("Speaker", 100)
  │     ├─ audio_mixer_set_volume("Headphone", 100)
  │     ├─ audio_mixer_set_switch("Left Output Mixer PCM", 1)
  │     └─ audio_mixer_set_switch("Right Output Mixer PCM", 1)
  │
  ├─ 3. sock_create_server()    ← 创建 UNIX Domain 监听 Socket
  │
  ├─ 4. protocol_parser_init()  ← 初始化协议解析器
  │     ringbuffer_init()        ← 初始化环形缓冲区 (4096 字节)
  │
  ├─ 5. signal(SIGTERM, ...)    ← 注册信号处理函数
  │     signal(SIGINT, ...)
  │
  ├─ 6. scan_music("/music/")   ← 扫描音乐目录，构建播放列表
  │
  └─ 7. if (有曲目) {             ← 自动开始播放第一首
         g_current = 0;
         open_track(0);
         g_state = 1;  // 播放中
       }
```

### 5.2 各步骤详解

#### 步骤 1：日志系统初始化

```c
log_init(AV_LOG_PATH, LOG_INFO, 1024*1024, 3);
```

- 日志文件：`/var/log/car_terminal/av.log`
- 日志级别：INFO（DEBUG 及以下不输出）
- 单文件最大：1 MB
- 轮转文件数：3 个（av.log, av.log.1, av.log.2）

日志是第一个初始化的子系统，因为后续所有步骤的诊断信息都需要通过日志输出。

#### 步骤 2：混音器初始化

```c
if (audio_mixer_open("default", &g_mixer) == 0) {
    audio_mixer_set_volume(g_mixer, "Speaker", 100);
    audio_mixer_set_volume(g_mixer, "Headphone", 100);
    audio_mixer_set_switch(g_mixer, "Left Output Mixer PCM", 1);
    audio_mixer_set_switch(g_mixer, "Right Output Mixer PCM", 1);
}
```

启动时将喇叭和耳机音量都设为 100%，并启用左右声道的输出混音开关。

注意：这里使用的是**原始值 100**（不是百分比映射后的值），因为 `audio_mixer_set_volume` 内部会做百分比到硬件原始值的线性映射。

#### 步骤 3：创建 UNIX Socket

```c
g_server_fd = sock_create_server(AV_SOCKET_PATH);
if (g_server_fd < 0) return 1;
```

Socket 路径：`/tmp/car_av.sock`

如果创建失败（比如路径已存在且无法删除、权限不足），进程直接退出返回 1。

#### 步骤 4：协议解析器初始化

```c
protocol_parser_init(&g_pp);
ringbuffer_init(&g_rb, 4096);
```

- 解析器状态机初始化为 IDLE（等待帧头 0xAA）
- 环形缓冲区大小 4096 字节，用于缓存从 Socket 收到的原始字节流

#### 步骤 5：信号处理

```c
signal(SIGTERM, sig_handler);
signal(SIGINT, sig_handler);
```

信号处理函数：

```c
static void sig_handler(int s) {
    (void)s;
    g_running = 0;
    _exit(0);
}
```

**关键细节**：使用 `_exit()` 而非 `exit()`。

- `exit()` 会调用 atexit 注册的回调函数、刷新 stdio 缓冲区，这些操作在信号处理函数中是**不安全的**（可重入性问题）
- `_exit()` 直接终止进程，不执行任何清理操作

#### 步骤 6：扫描音乐目录

```c
scan_music(AV_MUSIC_PATH);
```

扫描 `/music/` 目录下所有 `.wav` 文件（不区分大小写），跳过隐藏文件（以 `.` 开头）。

#### 步骤 7：自动播放

```c
if (g_track_count > 0) {
    g_current = 0;
    if (open_track(0) == 0) g_state = 1;
}
```

如果找到音乐文件，自动打开第一首并进入播放状态。

---

## 6. 主事件循环

### 6.1 循环结构

```c
while (g_running) {
    // 1. 准备 poll 监听的文件描述符
    struct pollfd pfds[2];
    int nfds = 0;
    pfds[nfds].fd = g_server_fd;
    pfds[nfds].events = POLLIN;
    nfds++;
    if (g_client_fd >= 0) {
        pfds[nfds].fd = g_client_fd;
        pfds[nfds].events = POLLIN;
        nfds++;
    }

    // 2. poll 等待事件（超时时间根据播放状态动态调整）
    poll(pfds, nfds, g_state == 1 ? 10 : 200);

    // 3. 音量轮询（每 2 秒）
    { ... 读取 /tmp/av_volume ... }

    // 4. 处理 Socket 指令
    handle_sockets();

    // 5. 如果正在播放，喂入下一数据块
    if (g_state == 1) play_chunk();
}
```

### 6.2 poll 超时策略

这是一个巧妙的设计：

| 播放状态 | poll 超时 | 原因 |
|---------|----------|------|
| 播放中 (g_state == 1) | **10 ms** | 需要频繁向 ALSA 喂数据，避免 underrun |
| 停止/暂停 | **200 ms** | 不需要喂数据，降低 CPU 占用 |

播放中每次循环最多等待 10ms 就会返回，然后调用 `play_chunk()` 写入 1024 帧。

**计算验证**：
- 44100 Hz 采样率，1024 帧的播放时间 = 1024 / 44100 ≈ **23.2 ms**
- poll 超时 10ms + writei 阻塞时间 ≈ 23ms，刚好匹配硬件消费速度
- ALSA 内部缓冲区（约 500ms 延迟配置）提供了足够的缓冲余量

### 6.3 每轮循环做什么

1. **poll**：等待 Socket 有数据可读，或超时
2. **音量检查**：每 2 秒读一次 `/tmp/av_volume`，有变化则更新硬件音量
3. **Socket 处理**：接受新连接、读取并分发控制指令
4. **播放推进**：如果状态为播放中，向 ALSA 写入最多 1024 帧 PCM 数据

---

## 7. 播放状态机与控制逻辑

### 7.1 状态定义

```c
// g_state 的取值:
0 = AV_PLAY_STOPPED   已停止
1 = AV_PLAY_PLAYING   播放中
2 = AV_PLAY_PAUSED    已暂停
```

### 7.2 状态转换图

```
                    ┌──────────┐
                    │  停止(0) │
                    └────┬─────┘
                         │ MSG_AV_PLAY
                         │ (从未播放过 → 打开第一首)
                         │ (停止后 → 重新打开当前曲目)
                         ▼
┌──────────┐    MSG_AV_PAUSE    ┌──────────┐
│  暂停(2) │ ◄───────────────── │ 播放(1)  │
└────┬─────┘                    └────┬─────┘
     │                                 │
     │ MSG_AV_PAUSE                    │ MSG_AV_STOP
     │ (恢复播放)                       │ (停止并释放资源)
     ▼                                 ▼
┌──────────┐                    ┌──────────┐
│  播放(1) │                    │  停止(0) │
└──────────┘                    └──────────┘
```

### 7.3 各指令的处理逻辑

#### MSG_AV_PLAY (0x60) — 播放/继续

```c
case MSG_AV_PLAY:
    if (g_state == 2)
        g_state = 1;                              // 暂停 → 继续
    else if (g_current < 0 && g_track_count > 0) {
        g_current = 0;                             // 从未播放过 → 从第一首开始
        open_track(0);
        g_state = 1;
    }
    else if (g_current >= 0) {
        open_track(g_current);                     // 停止后 → 重新打开当前曲目
        g_state = 1;
    }
    break;
```

三种场景：
1. **当前是暂停状态**：直接恢复播放（g_pcm_pos 保持不变，从暂停位置继续）
2. **从未播放过**（g_current == -1）：打开播放列表第一首
3. **停止状态但有当前曲目**：重新打开当前曲目（从头开始播放）

#### MSG_AV_STOP (0x61) — 停止

```c
case MSG_AV_STOP:
    g_state = 0;
    play_stop();
    break;
```

`play_stop()` 做了三件事：
1. 释放 PCM 数据缓冲区（`free(g_pcm_data)`）
2. 关闭文件句柄（`fclose(g_fp)`）
3. 丢弃 ALSA 硬件缓冲区中的剩余数据（`snd_pcm_drop(g_pcm)`）

#### MSG_AV_PAUSE (0x62) — 暂停/继续（切换）

```c
case MSG_AV_PAUSE:
    g_state = (g_state == 1) ? 2 : (g_state == 2) ? 1 : 0;
    break;
```

- 播放中 → 暂停
- 暂停中 → 播放
- 停止中 → 保持停止（无操作）

**注意**：暂停时并没有调用 `snd_pcm_pause()` 或 `snd_pcm_drop()`。暂停的实现方式是**不再调用 `play_chunk()`**，ALSA 硬件缓冲区中剩余的数据会继续播放完，然后硬件进入等待状态。恢复播放时，`g_pcm_pos` 保持不变，从暂停时的位置继续。

### 7.4 自动切歌

当一首曲目播放完毕（`g_pcm_pos >= g_pcm_total`），在 `play_chunk()` 中自动处理：

```c
if (chunk <= 0) {  // 曲目播放完毕
    free(g_pcm_data);
    g_pcm_data = NULL;
    g_current = (g_current + 1) % g_track_count;  // 下一首，到末尾循环回第一首
    if (open_track(g_current) != 0) g_state = 0;  // 打开失败则停止
    return;
}
```

播放列表是**循环播放**的：最后一首播完后回到第一首。

---

## 8. WAV 文件解析与播放列表

### 8.1 WAV 文件格式

WAV（Waveform Audio File Format）是基于 RIFF（Resource Interchange File Format）的音频文件格式。

标准 WAV 文件结构（PCM 数据）：

```
偏移   长度   字段名           说明
─────  ────  ───────────────  ──────────────────────────────
0      4     ChunkID           "RIFF" (0x52 0x49 0x46 0x46)
4      4     ChunkSize         文件大小 - 8
8      4     Format            "WAVE" (0x57 0x41 0x56 0x45)
12     4     Subchunk1ID       "fmt " (注意末尾空格)
16     4     Subchunk1Size     16 (PCM 格式)
20     2     AudioFormat       1 = PCM
22     2     NumChannels       1=单声道, 2=立体声
24     4     SampleRate        采样率 (如 44100)
28     4     ByteRate          SampleRate * NumChannels * BitsPerSample/8
32     2     BlockAlign        NumChannels * BitsPerSample/8
34     2     BitsPerSample     每采样位数 (如 16)
36     4     Subchunk2ID       "data"
40     4     Subchunk2Size     PCM 数据字节数
44     N     data              PCM 原始采样数据
```

### 8.2 音乐目录扫描 — scan_music()

```c
static int scan_music(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;

    int cnt = 0;
    struct dirent *e;
    while ((e = readdir(d)) && cnt < MAX_TRACKS) {
        if (e->d_name[0] == '.') continue;                    // 跳过隐藏文件
        const char *dot = strrchr(e->d_name, '.');
        if (dot && strcasecmp(dot, ".wav") == 0)              // 不区分大小写匹配 .wav
            snprintf(g_tracks[cnt++], 256, "%s/%s", dir, e->d_name);
    }
    closedir(d);
    g_track_count = cnt;
    LOG_INFO("av", "Found %d tracks", cnt);
    return cnt;
}
```

**要点**：
- 使用 `opendir/readdir` 遍历目录
- `strcasecmp` 不区分大小写（`.WAV` 和 `.wav` 都能匹配）
- 跳过以 `.` 开头的隐藏文件（包括 `.` 和 `..`）
- 最多扫描 64 首（`MAX_TRACKS`）
- 存储完整路径（如 `/music/song1.wav`）

### 8.3 曲目打开与 WAV 头部解析 — open_track()

```c
static int open_track(int idx) {
    // 1. 边界检查
    if (idx < 0 || idx >= g_track_count) return -1;

    // 2. 关闭之前打开的文件
    if (g_fp) { fclose(g_fp); g_fp = NULL; }

    // 3. 以二进制读模式打开
    g_fp = fopen(g_tracks[idx], "rb");
    if (!g_fp) return -1;

    // 4. 读取并验证 44 字节 WAV 头部
    unsigned char h[44];
    if (fread(h, 1, 44, g_fp) != 44 ||
        memcmp(h, "RIFF", 4) ||
        memcmp(h + 8, "WAVE", 4)) {
        fclose(g_fp);
        g_fp = NULL;
        return -1;
    }

    // 5. 从头部提取音频参数（小端序）
    g_ch   = h[22] | (h[23] << 8);                   // 声道数 (uint16 LE)
    g_sr   = h[24] | (h[25] << 8) | (h[26] << 16) | (h[27] << 24);  // 采样率 (uint32 LE)
    g_bits = h[34] | (h[35] << 8);                    // 每采样位数 (uint16 LE)
    fseek(g_fp, 44, SEEK_SET);                         // 文件指针移到 PCM 数据起始位置

    // 6. 打开或复用 ALSA PCM 设备
    if (!g_pcm) audio_playback_open("hw:0,0", g_sr, g_ch, &g_pcm);

    // 7. 将曲目名写入 /tmp/av_track 供 UI 显示
    {
        const char *fn = strrchr(g_tracks[idx], '/');
        const char *name = fn ? fn + 1 : g_tracks[idx];
        FILE *tf = fopen("/tmp/av_track", "w");
        if (tf) { fprintf(tf, "%s", name); fclose(tf); }
        LOG_INFO("av", "Track %d: %s", idx, name);
    }
    return 0;
}
```

**关键设计点**：

1. **头部验证简化**：只检查 "RIFF" 和 "WAVE" 两个魔数，不验证 fmt chunk 和 data chunk 的具体结构。这假设所有输入文件都是标准的 44 字节头部 PCM WAV。

2. **小端序手动解析**：通过位运算手动从字节数组中提取多字节值，而不是使用 `memcpy` 到 uint16_t/uint32_t。这样做在大端序平台上也能正确工作（虽然目标平台 ARM 是小端序）。

3. **ALSA 设备复用**：`if (!g_pcm)` 确保 PCM 设备只在第一次打开曲目时打开，后续切歌时复用同一个设备句柄。但这里有一个潜在问题：如果不同曲目有不同的采样率/声道数，ALSA 设备不会重新配置。

4. **曲目名通知**：通过写 `/tmp/av_track` 文件通知 UI 当前播放的曲目名（只写文件名，不含路径）。

### 8.4 PCM 数据加载 — play_chunk() 首次调用

PCM 数据不是在 `open_track()` 中加载的，而是在 `play_chunk()` 第一次被调用时**懒加载**的：

```c
static void play_chunk(void) {
    if (!g_pcm || g_state != 1) return;

    if (!g_pcm_data) {  // 首次调用：加载全部 PCM 数据
        if (!g_fp) { g_state = 0; return; }

        // 计算 PCM 数据大小 = 文件总大小 - 44 字节头部
        fseek(g_fp, 0, SEEK_END);
        long sz = ftell(g_fp);
        fseek(g_fp, 44, SEEK_SET);

        int ds = (int)(sz - 44);
        if (ds <= 0) { g_state = 0; return; }

        // 分配堆内存并读取全部 PCM 数据
        g_pcm_data = malloc((size_t)ds);
        if (!g_pcm_data) { g_state = 0; return; }
        fread(g_pcm_data, 1, (size_t)ds, g_fp);

        // 读取完毕后立即关闭文件 — 播放期间不再需要磁盘 I/O
        fclose(g_fp);
        g_fp = NULL;

        // 计算总帧数
        g_pcm_total = ds / (g_ch * g_bits / 8);
        g_pcm_pos = 0;
    }
    // ... 后续写入 ALSA ...
}
```

**帧数计算公式**：
```
总帧数 = PCM数据字节数 / (声道数 × 每采样字节数)
```
例如：立体声 16-bit，每帧 = 2 声道 × 2 字节 = 4 字节。

---

## 9. ALSA 音频核心层

### 9.1 ALSA 基础概念

ALSA（Advanced Linux Sound Architecture）是 Linux 的标准音频子系统。应用层通过 `alsa-lib` 库操作声卡。

#### PCM 数据格式

```
S16_LE: 16-bit signed, little-endian (WM8960 标准格式)

交织格式 (Interleaved):
  帧 0: [L0][R0]
  帧 1: [L1][R1]
  帧 2: [L2][R2]
  ...

1 帧 = 声道数 × sizeof(样本)
立体声 16-bit: 1 帧 = 4 字节
```

#### buffer_size 与 period_size

```
┌─────────────────────────────────────────────────┐
│              ALSA 硬件缓冲区 (buffer)             │
│  ┌──────────┬──────────┬──────────┬──────────┐ │
│  │ period 0 │ period 1 │ period 2 │ period 3 │ │ ...
│  └──────────┴──────────┴──────────┴──────────┘ │
└─────────────────────────────────────────────────┘

buffer_size = 总帧数 (如 8192)
period_size = 每次硬件中断的帧数 (如 1024)
buffer 中有 buffer_size / period_size 个 period (如 8 个)
```

- 编解码器每消费完一个 period 的数据触发一次硬件中断
- 应用程序应及时补充数据以避免缓冲区为空
- **XRUN (underrun)**：应用程序未及时提供数据，导致 ALSA 缓冲区为空，DAC 输出静音或重复采样，产生杂音

### 9.2 PCM 播放接口

#### audio_playback_open() — 打开 PCM 设备

```c
int audio_playback_open(const char *device, int sample_rate, int channels,
                        snd_pcm_t **pcm_handle)
{
    snd_pcm_t *pcm;
    int ret;

    // 1. 打开 PCM 设备（阻塞模式）
    ret = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
    // device: "hw:0,0" = 声卡0, 设备0
    // stream: SND_PCM_STREAM_PLAYBACK = 播放方向
    // mode: 0 = 阻塞模式

    // 2. 一站式配置参数
    ret = snd_pcm_set_params(pcm,
                              AV_FORMAT,                          // S16_LE
                              SND_PCM_ACCESS_RW_INTERLEAVED,     // 交织模式
                              (unsigned int)channels,             // 声道数
                              (unsigned int)sample_rate,          // 采样率
                              1,                                   // soft_resample: 允许软件重采样
                              500000);                             // latency: 500ms
    // ...
    *pcm_handle = pcm;
    return 0;
}
```

**snd_pcm_set_params 参数说明**：

| 参数 | 值 | 说明 |
|------|-----|------|
| format | SND_PCM_FORMAT_S16_LE | 16-bit 有符号小端 |
| access | SND_PCM_ACCESS_RW_INTERLEAVED | 标准交织模式（L R L R ...） |
| channels | 从 WAV 头部读取 | 1=单声道, 2=立体声 |
| rate | 从 WAV 头部读取 | 如 44100 |
| soft_resample | 1 | 允许 ALSA 在内部做软件重采样 |
| latency | 500000 (500ms) | 期望延迟，ALSA 据此设置 buffer/period 大小 |

500ms 延迟是一个比较保守的值，意味着 ALSA 内部缓冲区大约能存 500ms 的音频数据。这给了应用层充足的时间来喂数据，减少 underrun 的概率，但代价是播放延迟较高（从写入到听到声音最多 500ms）。

#### audio_playback_write() — 写入 PCM 数据

```c
int audio_playback_write(snd_pcm_t *pcm, const int16_t *data, int frames)
{
    int ret = snd_pcm_writei(pcm, data, (snd_pcm_uframes_t)frames);

    if (ret == -EPIPE) {
        // XRUN (underrun): 播放缓冲区空了！
        // 恢复: snd_pcm_recover() 内部调用 snd_pcm_prepare() 重置设备
        ret = snd_pcm_recover(pcm, ret, 0);
        return 0;  // 恢复后返回 0 帧
    }
    // ...
    return ret;  // 实际写入帧数
}
```

**注意**：`av_daemon.c` 中的 `play_chunk()` 并没有调用 `audio_playback_write()`，而是直接调用了 `snd_pcm_writei()`。这是因为 `play_chunk()` 需要自己处理 XRUN 恢复逻辑（直接调用 `snd_pcm_prepare()`）。

在 `play_chunk()` 中：

```c
int ret = snd_pcm_writei(g_pcm, pcm + g_pcm_pos * g_ch, chunk);
if (ret == -EPIPE) {
    snd_pcm_prepare(g_pcm);  // XRUN 恢复
    return;
}
if (ret > 0) g_pcm_pos += ret;
```

#### audio_playback_pause() — 暂停/恢复

```c
int audio_playback_pause(snd_pcm_t *pcm, int pause)
{
    int ret = snd_pcm_pause(pcm, pause ? 1 : 0);
    if (ret == -ENOSYS) {
        // 硬件不支持 pause: 用 drop/prepare 模拟
        if (pause) snd_pcm_drop(pcm);     // 丢弃缓冲区
        else       snd_pcm_prepare(pcm);   // 重新准备
        ret = 0;
    }
    return (ret < 0) ? ret : 0;
}
```

这个函数在 `av_daemon.c` 中**没有被调用**。暂停是通过"不调用 play_chunk()"实现的，而非真正暂停 ALSA 设备。

### 9.3 混音器控制接口

#### audio_mixer_open() — 打开混音器

ALSA 混音器的打开需要四步：

```c
int audio_mixer_open(const char *mixer_name, snd_mixer_t **mixer)
{
    // 1. 创建混音器句柄
    snd_mixer_open(mixer, 0);

    // 2. 关联到声卡
    snd_mixer_attach(*mixer, mixer_name);

    // 3. 注册简单元素 (selem)
    snd_mixer_selem_register(*mixer, NULL, NULL);

    // 4. 加载配置
    snd_mixer_load(*mixer);

    return 0;
}
```

#### audio_mixer_set_volume() — 设置音量

```c
int audio_mixer_set_volume(snd_mixer_t *mixer, const char *selem_name, int volume)
{
    snd_mixer_elem_t *elem;
    snd_mixer_selem_id_t *sid;
    long min, max, val;

    // 1. 查找混音器元素（如 "Speaker", "Headphone"）
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, selem_name);
    elem = snd_mixer_find_selem(mixer, sid);

    // 2. 获取硬件音量范围
    snd_mixer_selem_get_playback_volume_range(elem, &min, &max);

    // 3. 百分比 → 原始值（线性映射）
    val = min + (max - min) * volume / 100;

    // 4. 设置所有声道的播放音量（真正的 WM8960 寄存器操作）
    snd_mixer_selem_set_playback_volume_all(elem, val);

    return 0;
}
```

**WM8960 的音量范围**通常是 0-127（7-bit 数字音量控制），所以：
- 百分比 0 → 原始值 0（静音）
- 百分比 100 → 原始值 127（最大音量）

#### audio_mixer_set_switch() — 设置播放开关

某些混音器元素（如 "Left Output Mixer PCM"）除了音量滑块外还有一个布尔开关，用于控制信号通路的通断。

```c
int audio_mixer_set_switch(snd_mixer_t *mixer, const char *selem_name, int on)
{
    // 查找元素...
    if (snd_mixer_selem_has_playback_switch(elem))
        snd_mixer_selem_set_playback_switch_all(elem, on ? 1 : 0);
    return 0;
}
```

---

## 10. IPC 通信机制

### 10.1 通信通道总览

av_daemon 与 Qt UI 之间有三条通信通道：

| 通道 | 方向 | 用途 | 机制 |
|------|------|------|------|
| `/tmp/car_av.sock` | UI → av | 播放控制指令（播放/暂停/停止） | UNIX Domain Socket + 二进制协议帧 |
| `/tmp/av_volume` | UI → av | 音量设置 (0-100) | 纯文本文件，av 轮询读取 |
| `/tmp/av_track` | av → UI | 当前播放曲目名 | 纯文本文件，av 写入 |

### 10.2 UNIX Domain Socket 通信

#### Socket 消息帧格式

使用 common 库中定义的统一二进制协议帧格式：

```
┌────────┬────────┬────────┬──────────┬───────────┬────────┬────────┐
│ HEAD0  │ HEAD1  │  TYPE  │   LEN    │   DATA    │  CRC8  │  TAIL  │
│  0xAA  │  0x55  │ 1 Byte │ 2 Bytes  │ 0~65535 B │ 1 Byte │  0x55  │
└────────┴────────┴────────┴──────────┴───────────┴────────┴────────┘
```

- **HEAD0/HEAD1**：帧同步字 `0xAA 0x55`，用于从字节流中定位帧的起始
- **TYPE**：消息类型（1 字节），高 4 位为模块 ID，低 4 位为消息 ID
- **LEN**：数据长度（2 字节，大端序）
- **DATA**：消息负载（可变长度）
- **CRC8**：校验和（覆盖 TYPE + LEN + DATA）
- **TAIL**：帧尾 `0x55`

#### av_daemon 处理的消息类型

| 消息类型 | 值 | 说明 |
|---------|-----|------|
| MSG_AV_PLAY | 0x60 | 播放/继续 |
| MSG_AV_STOP | 0x61 | 停止 |
| MSG_AV_PAUSE | 0x62 | 暂停/继续（切换） |
| MSG_AV_VOLUME | 0x63 | 设置音量（当前通过文件实现，Socket 未处理） |
| MSG_AV_STATUS | 0x64 | 播放状态查询（当前未实现） |

#### Socket 处理流程 — handle_sockets()

```c
static void handle_sockets(void) {
    // 1. 接受新连接（非阻塞）
    int fd = sock_accept(g_server_fd);
    if (fd >= 0) {
        if (g_client_fd >= 0)
            sock_close(fd, NULL);        // 已有客户端，拒绝新连接
        else {
            g_client_fd = fd;             // 保存客户端 fd
            protocol_reset(&g_pp, &g_rb); // 重置协议解析器
            LOG_INFO("av", "UI connected");
        }
    }

    if (g_client_fd < 0) return;  // 无客户端，跳过

    // 2. 接收数据（非阻塞）
    uint8_t buf[512];
    int n = sock_recv(g_client_fd, buf, sizeof(buf));
    if (n < 0) {  // 客户端断开
        LOG_INFO("av", "UI disconnected");
        sock_close(g_client_fd, NULL);
        g_client_fd = -1;
        return;
    }

    if (n > 0) {
        // 3. 写入环形缓冲区
        ringbuffer_write(&g_rb, buf, (uint32_t)n);

        // 4. 循环解析所有完整帧
        uint8_t t, d[256];
        uint32_t l;
        while (protocol_unpack(&g_pp, &g_rb, &t, d, sizeof(d), &l) == 0) {
            // 5. 根据消息类型分发
            switch (t) {
            case MSG_AV_PLAY:  ... break;
            case MSG_AV_STOP:  ... break;
            case MSG_AV_PAUSE: ... break;
            }
        }
    }
}
```

#### 协议解析器工作原理

`protocol_unpack()` 使用 5 状态有限状态机（FSM）从连续字节流中找出帧边界：

```
IDLE → HEAD1 → HEADER → DATA → TAIL → (输出帧, 回到 IDLE)
```

- **IDLE**：等待 HEAD0 (0xAA)
- **HEAD1**：已收到 0xAA，等待 HEAD1 (0x55)
- **HEADER**：已收到帧头，等待 TYPE(1B) + LEN(2B)
- **DATA**：已收到头部，等待 DATA(LEN字节) + CRC(1B)
- **TAIL**：已收到数据+CRC，等待 TAIL (0x55)

**粘包/断包处理**：
- **粘包**：ringbuffer 缓存所有接收数据，while 循环逐帧解析
- **断包**：状态机记住当前进度，下次调用时从断点继续

### 10.3 共享文件通信

#### /tmp/av_volume — 音量输入

UI 进程将音量值（0-100 的纯文本数字）写入此文件。av_daemon 每 2 秒轮询读取一次。

```c
// 主循环中每 2 秒执行
{
    static time_t t0 = 0;
    if (time(NULL) - t0 >= 2) {
        t0 = time(NULL);
        FILE *f = fopen("/tmp/av_volume", "r");
        if (f) {
            int v;
            if (fscanf(f, "%d", &v) == 1 && v != g_volume && v >= 0 && v <= 100) {
                g_volume = v;
                if (g_mixer) {
                    int r = v == 0 ? 0 : 40 + v * 87 / 100;  // 非线性映射
                    audio_mixer_set_volume(g_mixer, "Speaker", r);
                    audio_mixer_set_volume(g_mixer, "Headphone", r);
                }
            }
            fclose(f);
        }
    }
}
```

#### /tmp/av_track — 当前曲目输出

av_daemon 在每次切换曲目时（`open_track()` 中）将当前曲目文件名写入此文件，UI 进程读取后显示。

---

## 11. 音量控制

### 11.1 音量控制链路

```
UI 调节音量滑块
    │
    ▼
写入 /tmp/av_volume (纯文本 0-100)
    │
    ▼
av_daemon 每 2 秒轮询读取
    │
    ▼
非线性映射: raw = (v == 0) ? 0 : (40 + v * 87 / 100)
    │
    ▼
audio_mixer_set_volume() → snd_mixer_selem_set_playback_volume_all()
    │
    ▼
WM8960 硬件寄存器 (数字音量控制)
```

### 11.2 非线性音量映射

```c
int r = v == 0 ? 0 : 40 + v * 87 / 100;
```

| UI 音量 (v) | 原始值 (r) | 说明 |
|-------------|-----------|------|
| 0 | 0 | 静音 |
| 1 | 40 | 最小可听音量 |
| 50 | 83 | 中等音量 |
| 100 | 127 | 最大音量 |

**为什么用非线性映射？**

人耳对音量的感知是对数的（韦伯-费希纳定律），线性的百分比映射会导致低音量区间变化不明显、高音量区间变化过于剧烈。

这个映射将 1-100 的 UI 音量映射到 41-127 的硬件原始值（跳过了 0-40 的低增益区间，因为 WM8960 在低增益时信噪比差、可听度低），提供了更符合人耳感知的音量曲线。

**注意**：这里传入 `audio_mixer_set_volume()` 的 `r` 值（40-127）已经是硬件原始值，而 `audio_mixer_set_volume()` 内部又会做一次百分比到原始值的线性映射。这意味着实际的硬件值是"映射的映射"，可能与预期不完全一致。这是代码中一个值得注意的设计点。

---

## 12. 构建系统

### 12.1 Makefile 结构

```makefile
# 编译模式选择
HOST ?= 0
ifeq ($(HOST),1)
  CROSS_COMPILE ?=           # x86 本地编译
  CC = gcc
else
  CROSS_COMPILE ?= arm-buildroot-linux-gnueabihf-  # ARM 交叉编译
  CC = $(CROSS_COMPILE)gcc
endif

# 依赖库
COMMON_LIB ?= ../common/lib/libcarcommon.a   # 公共库静态链接
COMMON_INC ?= ..                               # 公共库头文件根目录

# 源文件
SRCS  = av_daemon.c
SRCS += audio_core.c

# 编译选项
CFLAGS  = -Wall -Wextra -std=gnu99 -Os -g
CFLAGS += -ffunction-sections -fdata-sections   # 函数/数据分段，配合链接时 GC
CFLAGS += -Wno-format-truncation                 # 忽略 snprintf 截断警告
CFLAGS += -I$(COMMON_INC) ...                    # 头文件搜索路径

LDFLAGS = -Wl,--gc-sections                       # 链接时丢弃未使用段
LDFLAGS += -lpthread -lrt -lm -lasound            # 依赖库
```

### 12.2 编译命令

```bash
# ARM 交叉编译（默认，目标板）
make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-

# x86 本地编译（开发调试，需要安装 alsa-lib）
make HOST=1
# 或
make host

# 清理
make clean
```

### 12.3 编译优化选项说明

| 选项 | 作用 |
|------|------|
| `-Os` | 优化代码大小（嵌入式场景优先） |
| `-g` | 保留调试信息 |
| `-ffunction-sections` | 每个函数放在独立的段中 |
| `-fdata-sections` | 每个数据项放在独立的段中 |
| `-Wl,--gc-sections` | 链接时丢弃未使用的段（减小最终二进制体积） |

### 12.4 依赖库

| 库 | 用途 |
|----|------|
| `libcarcommon.a` | 项目公共库（日志、Socket、协议解析、环形缓冲区） |
| `libasound` | ALSA 音频库 |
| `libpthread` | POSIX 线程库（虽然单线程，但 ALSA 内部可能使用） |
| `librt` | POSIX 实时扩展库 |
| `libm` | 数学库 |

---

## 13. 与系统其他模块的关系

### 13.1 依赖的 common 库模块

```
av_daemon
    ├── common.h              # 统一类型、返回码、消息类型定义
    ├── log/log.h             # 滚动日志系统
    ├── socket/unix_socket.h  # UNIX Domain Socket 封装
    ├── protocol/protocol.h   # 二进制协议帧解析 (FSM)
    └── ringbuffer/ringbuffer.h  # 环形缓冲区 (协议解析的字节缓存)
```

### 13.2 消息编码体系

整个系统的消息类型采用**高 4 位模块 ID + 低 4 位消息 ID** 的编码方案：

```
字节:  7  6  5  4  3  2  1  0
      ├────模块 ID────┤├──消息 ID──┤

0x00-0x0F: 系统消息 (guard_daemon)
0x10-0x1F: GPS 定位 (gps_daemon)
0x20-0x2F: DVR 录像 (dvr_daemon)
0x30-0x3F: 传感器 (sensor_daemon)
0x40-0x4F: 网络 (net_daemon)
0x50-0x5F: CAN 总线 (canbus_daemon)
0x60-0x6F: 音视频 (av_daemon)    ← 本模块
0x70-0x7F: 按键输入 (input_daemon)
```

av_daemon 只处理模块 ID 为 `0x6` 的消息，其他模块的消息会被忽略。

### 13.3 与 guard_daemon 的关系

`guard_daemon` 是父进程，负责：
- 启动 av_daemon 子进程
- 监控 av_daemon 的运行状态
- av_daemon 异常退出时自动重启
- 系统关机时向 av_daemon 发送 SIGTERM

av_daemon 通过 `MSG_HEARTBEAT` (0x00) 向 guard_daemon 发送心跳（当前代码中未实现心跳发送，但架构上预留了）。

---

## 14. 设计权衡与改进思考

### 14.1 当前设计的优点

1. **简单可靠**：单线程 + 全局变量，没有复杂的同步机制，出问题的概率低
2. **播放稳定**：整文件加载到内存，播放期间无磁盘 I/O，不会因为文件系统延迟导致卡顿
3. **快速响应**：播放中 poll 超时仅 10ms，控制指令的响应延迟低
4. **资源可控**：固定大小的播放列表（64 首），内存使用有上限

### 14.2 潜在问题与改进方向

#### 问题 1：整文件加载的内存占用

一首 4 分钟、44.1kHz、16-bit 立体声的 WAV 文件：
```
44100 × 2 × 2 × 240秒 ≈ 42.3 MB
```

如果播放列表中有多首歌，虽然同一时间只有一首加载到内存，但切歌时需要 free + malloc，可能产生内存碎片。

**改进方向**：
- 流式读取 + 环形缓冲区，每次从文件读取一个 period 的数据
- 或使用 mmap 映射文件，让内核管理页面换入换出

#### 问题 2：WAV 头部解析过于简化

当前代码假设所有 WAV 文件都是标准的 44 字节头部、PCM 格式。但实际 WAV 文件可能有：
- 非标准的 fmt chunk 大小（如包含扩展信息）
- 多个 data chunk
- 其他元数据 chunk（LIST、INFO 等）
- 浮点格式（32-bit float）

**改进方向**：使用完整的 RIFF chunk 遍历解析，而非假设固定偏移。

#### 问题 3：不同采样率曲目切换

`g_pcm` 设备只在第一次打开曲目时配置（`if (!g_pcm)`），如果播放列表中有不同采样率的 WAV 文件，后续曲目不会重新配置 ALSA 设备，可能导致播放速度异常。

**改进方向**：每次 `open_track()` 时检查采样率/声道数是否变化，如果变化则关闭并重新打开 PCM 设备。

#### 问题 4：暂停实现不精确

当前暂停是通过"不再调用 play_chunk()"实现的，ALSA 硬件缓冲区中剩余的数据会继续播放完（最多 500ms），然后才真正停止。恢复播放时也没有调用 `snd_pcm_prepare()`，可能导致恢复时的 XRUN。

**改进方向**：暂停时调用 `snd_pcm_drop()` 丢弃缓冲区数据，恢复时调用 `snd_pcm_prepare()` 重新准备设备。

#### 问题 5：音量调节延迟

音量通过每 2 秒轮询文件实现，用户调节音量后最多等待 2 秒才生效。

**改进方向**：
- 通过 Socket 发送 `MSG_AV_VOLUME` 指令实现即时响应
- 或使用 inotify 监控文件变化

#### 问题 6：单客户端限制

当前只允许一个 UI 客户端连接，新连接会被直接拒绝。如果有多个 UI 实例（如本地 UI + 远程控制），无法同时工作。

**改进方向**：维护客户端 fd 数组，支持多客户端同时连接和广播状态通知。

#### 问题 7：缺少状态上报

av_daemon 不会主动向 UI 上报播放状态（当前曲目、播放进度、播放/暂停状态）。UI 只能通过读取 `/tmp/av_track` 获取曲目名，无法获取播放进度。

**改进方向**：
- 实现 `MSG_AV_STATUS` 消息，定时向 UI 上报状态
- 或在状态变化时主动推送通知

### 14.3 适合的应用场景

当前设计最适合的场景：
- 嵌入式车载终端，资源有限
- 音乐文件数量不多（< 64 首）
- 单 UI 界面控制
- 对播放稳定性要求高，对功能丰富度要求适中
- WAV 文件格式统一（相同采样率、声道数）

如果需要支持 MP3/AAC 等压缩格式、播放列表动态更新、多房间同步播放等高级功能，则需要更复杂的架构（如引入 GStreamer 或 libmpg123 + 多线程环形缓冲区）。

---

## 附录 A：关键函数速查表

| 函数 | 文件 | 作用 |
|------|------|------|
| `main()` | av_daemon.c | 进程入口，初始化 + 主事件循环 |
| `scan_music()` | av_daemon.c | 扫描音乐目录，构建播放列表 |
| `open_track()` | av_daemon.c | 打开曲目，解析 WAV 头部，通知 UI |
| `play_stop()` | av_daemon.c | 停止播放，释放资源 |
| `play_chunk()` | av_daemon.c | 向 ALSA 写入最多 1024 帧 PCM 数据 |
| `handle_sockets()` | av_daemon.c | 接受连接，接收并分发 Socket 指令 |
| `sig_handler()` | av_daemon.c | SIGTERM/SIGINT 信号处理 |
| `audio_playback_open()` | audio_core.c | 打开 ALSA PCM 设备 |
| `audio_playback_write()` | audio_core.c | 写入 PCM 数据（含 XRUN 恢复） |
| `audio_playback_pause()` | audio_core.c | 暂停/恢复播放 |
| `audio_playback_close()` | audio_core.c | 关闭 PCM 设备 |
| `audio_mixer_open()` | audio_core.c | 打开混音器 |
| `audio_mixer_set_volume()` | audio_core.c | 设置音量（百分比→原始值映射） |
| `audio_mixer_set_switch()` | audio_core.c | 设置播放开关 |
| `audio_mixer_close()` | audio_core.c | 关闭混音器 |

## 附录 B：全局变量速查表

| 变量 | 类型 | 初始值 | 作用 |
|------|------|--------|------|
| `g_pcm` | snd_pcm_t* | NULL | ALSA PCM 播放句柄 |
| `g_mixer` | snd_mixer_t* | NULL | ALSA 混音器句柄 |
| `g_running` | volatile int | 1 | 主循环运行标志 |
| `g_tracks[][]` | char[64][256] | - | 播放列表（文件路径） |
| `g_track_count` | int | 0 | 有效曲目数 |
| `g_current` | int | -1 | 当前曲目索引 |
| `g_fp` | FILE* | NULL | 当前打开的 WAV 文件句柄 |
| `g_state` | int | 0 | 播放状态 (0=停止,1=播放,2=暂停) |
| `g_volume` | int | 100 | 当前音量 (0-100) |
| `g_sr` | int | - | 当前曲目采样率 |
| `g_ch` | int | - | 当前曲目声道数 |
| `g_bits` | int | - | 当前曲目每采样位数 |
| `g_server_fd` | int | -1 | 监听 Socket fd |
| `g_client_fd` | int | -1 | 已连接客户端 fd |
| `g_pcm_data` | void* | NULL | PCM 数据堆缓冲区 |
| `g_pcm_total` | int | 0 | PCM 总帧数 |
| `g_pcm_pos` | int | 0 | 当前播放帧索引 |
| `g_pp` | protocol_parser_t | - | 协议帧解析器 |
| `g_rb` | ringbuffer_t | - | 接收字节环形缓冲区 |

---

> 文档版本：v1.0
> 基于代码版本：av_daemon.c (15645 bytes) / audio_core.c (9032 bytes)
> 生成时间：2026-08-23
