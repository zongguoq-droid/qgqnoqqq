# dvr_daemon 模块学习文档

> 本文档面向需要理解、维护或扩展 `dvr_daemon`（行车记录仪录制守护进程）的开发者，按照 **架构设计 → 使用逻辑 → 代码设计** 的顺序循序渐进讲解。

---

## 目录

1. [模块概述](#1-模块概述)
2. [整体架构设计](#2-整体架构设计)
3. [核心数据结构](#3-核心数据结构)
4. [V4L2 采集子系统](#4-v4l2-采集子系统)
5. [存储管理器 — 分段循环录制](#5-存储管理器--分段循环录制)
6. [主循环与事件驱动模型](#6-主循环与事件驱动模型)
7. [IPC 通信 — Unix Socket 与二进制协议](#7-ipc-通信--unix-socket-与二进制协议)
8. [录制控制逻辑详解](#8-录制控制逻辑详解)
9. [拍照与预览机制](#9-拍照与预览机制)
10. [状态推送机制](#10-状态推送机制)
11. [配置加载](#11-配置加载)
12. [构建系统 Makefile](#12-构建系统-makefile)
13. [单元测试](#13-单元测试)
14. [关键设计决策与踩坑记录](#14-关键设计决策与踩坑记录)
15. [学习路径建议](#15-学习路径建议)

---

## 1. 模块概述

### 1.1 是什么

`dvr_daemon` 是一个运行在嵌入式 Linux（ARM Buildroot）上的**行车记录仪后台守护进程**。它通过 V4L2 接口从 USB 摄像头采集 MJPEG 视频帧，实现分段循环录像、拍照、预览，并通过 Unix Domain Socket 与 Qt 图形界面通信。

### 1.2 核心能力一览

| 能力            | 说明                                            |
| ------------- | --------------------------------------------- |
| V4L2 MJPEG 采集 | `/dev/video1`，640×480，32 个 mmap 内核缓冲区，约 15fps |
| MJPEG 直存      | 帧本身就是 JPEG 数据，录像直接 `fwrite`，无需转码              |
| 分段循环录像        | 每 60 秒一个文件，空间超配额自动删除最旧分段                      |
| 拍照            | 用户触发或首帧自动，将当前帧存为 `.jpg`                       |
| 预览            | 每 15 帧更新 `/tmp/dvr_preview.jpg`，供 Qt 界面显示     |
| 暂停/恢复         | 跨暂停保留已累计录制时长                                  |
| IPC 控制        | Unix Socket + 自定义二进制协议，支持启停/暂停/拍照/状态推送        |

### 1.3 文件结构

```
dvr_daemon/
├── dvr_daemon.h      # 公共头文件：配置常量、数据结构、存储 API 声明
├── dvr_daemon.c      # 主程序：main()、poll 事件循环、命令处理、拍照、状态推送
├── v4l2_capture.h    # V4L2 采集子系统 API 声明
├── v4l2_capture.c    # V4L2 采集实现（open → mmap → DQBUF/QBUF → close）
├── storage_mgr.c     # 存储管理器：分段文件管理、循环删除、目录枚举
├── Makefile           # 构建脚本（支持交叉编译与主机编译）
├── README.md          # 简要说明
└── test/
    └── test_dvr.c     # 单元测试（存储管理器 + 状态枚举）
```

---

## 2. 整体架构设计

### 2.1 架构总览

```
┌─────────────────────────────────────────────────────────────────┐
│                        dvr_daemon 进程                           │
│                                                                   │
│  ┌──────────────┐    ┌──────────────────┐    ┌───────────────┐ │
│  │  V4L2 采集   │───→│   主事件循环      │───→│  存储管理器    │ │
│  │  (摄像头)    │    │   (poll 驱动)     │    │ (分段循环录像) │ │
│  └──────────────┘    └────────┬─────────┘    └───────────────┘ │
│                                │                                   │
│                    ┌───────────┴───────────┐                      │
│                    │                       │                      │
│              ┌─────▼─────┐          ┌─────▼──────┐              │
│              │ Unix Socket│          │  定时任务    │              │
│              │  (IPC)     │          │ 状态/清理    │              │
│              └─────┬─────┘          └────────────┘              │
└────────────────────┼──────────────────────────────────────────────┘
                     │ 二进制协议帧
              ┌──────▼──────┐
              │  Qt 界面进程  │
              └─────────────┘
```

### 2.2 设计原则

1. **单一全局上下文（Singleton）**：守护进程不是库，使用一个全局 `g_ctx` 持有所有状态，简化生命周期管理。
2. **事件驱动（Reactor 模式）**：主循环用 `poll()` 同时监听 V4L2 帧就绪、Socket 连接、Socket 数据，避免多线程复杂性。
3. **子系统解耦**：V4L2 采集、存储管理、IPC 通信各自独立封装，通过 `dvr_context_t` 聚合。
4. **零拷贝采集**：V4L2 使用 mmap 缓冲区，帧数据直接在内核与用户空间共享，采集路径无内存拷贝。
5. **MJPEG 直存**：摄像头输出的 MJPEG 帧本身就是合法 JPEG，录像和拍照都直接写原始字节，无需 CPU 编码。

### 2.3 数据流

```
摄像头硬件
   │ (USB)
   ▼
V4L2 内核驱动
   │ (mmap 缓冲区, 32 个)
   ▼
poll() 检测 POLLIN → v4l2_dequeue() 取出帧指针
   │
   ├──→ 正在录制且未暂停? → dvr_storage_write() → 分段文件 (.h264)
   ├──→ pending_snapshot?  → save_snapshot()      → 拍照文件 (.jpg)
   ├──→ 每 15 帧?          → fwrite /tmp/dvr_preview.jpg
   └──→ v4l2_queue() 归还缓冲区给内核
```

---

## 3. 核心数据结构

理解模块的第一步是掌握三个核心结构体。

### 3.1 `v4l2_ctx_t` — V4L2 采集上下文

定义于 `v4l2_capture.h`：

```c
typedef struct {
    void   *start;    // mmap 映射的用户空间起始地址
    size_t  length;   // 该缓冲区的长度
} v4l2_buf_t;

typedef struct {
    int         fd;           // 摄像头设备文件描述符
    v4l2_buf_t *bufs;         // mmap 缓冲区数组 (32 个)
    int         buf_count;    // 实际缓冲区数量
    int         width;        // 协商后的帧宽度
    int         height;       // 协商后的帧高度
    int         frame_size;   // 单帧最大字节数 (sizeimage)
} v4l2_ctx_t;
```

**要点**：

- `bufs` 是动态分配的数组，每个元素记录一块 mmap 映射区域的地址和长度。
- `fd` 同时用于 `poll()` 监听和 `ioctl()` 操作。
- `width/height` 是**实际协商值**，可能与请求值不同（驱动会就近选择支持的分辨率）。

### 3.2 `dvr_storage_t` — 存储管理器状态

定义于 `dvr_daemon.h`：

```c
typedef struct {
    char    base_path[256];   // 录制文件根目录 (如 "/record/")
    int     max_size_mb;       // 总存储配额 (MB)
    int     segment_sec;       // 每个分段的时长 (秒, 默认 60)
    int     current_segment;   // 单调递增的分段计数器 (调试用)
    FILE   *current_fp;        // 当前活动分段的文件句柄 (空闲/暂停时为 NULL)
    int     current_size;      // 当前分段已写入的字节数
} dvr_storage_t;
```

**要点**：

- `current_fp` 为 `NULL` 表示当前没有打开的分段文件（未录制或已停止）。
- `current_segment` 仅用于日志标识，不参与文件命名（文件名用时间戳）。

### 3.3 `dvr_context_t` — 守护进程全局上下文

定义于 `dvr_daemon.h`，是整个模块的"大脑"：

```c
typedef struct {
    // ---- 子系统状态 ----
    v4l2_ctx_t      v4l2;           // V4L2 采集状态
    dvr_storage_t   storage;        // 存储管理器状态
    char            camera_dev[64]; // 摄像头设备路径

    // ---- 录制状态 (支持暂停/恢复) ----
    int             recording;       // 0=已停止, 1=录制中 (可能处于暂停)
    int             paused;          // 0=活动, 1=已暂停 (帧不写入磁盘)
    time_t          rec_start_time;  // 当前录制周期开始的墙上时钟时间
    uint32_t        rec_elapsed_sec; // 累计录制秒数 (不含暂停时间, 跨暂停保留)

    // ---- 运行与通信 ----
    int             running;         // 主循环运行标志
    int             server_fd;       // 监听界面连接的 Unix Socket
    int             client_fd;       // 已连接的界面 Socket (-1 表示无连接)
    protocol_parser_t proto_parser;  // 二进制协议帧解析器
    ringbuffer_t    recv_rb;         // 接收环形缓冲区
    uint32_t        frame_total;     // 生命周期帧计数器

    // ---- 拍照支持 ----
    uint32_t        snap_count;      // 会话拍照序列号 (防同一秒文件名冲突)
    int             pending_snapshot;// 1=界面请求拍照但帧尚未到达
} dvr_context_t;
```

**全局实例**：`dvr_daemon.c` 中定义了 `static dvr_context_t g_ctx;`，整个进程只有一份。

**录制状态机**（由 `recording` + `paused` 组合表示）：

```
          START                     PAUSE
  IDLE ──────────→ RECORDING ──────────→ PAUSED
   ↑                  │  ↑                   │  │
   │                  │  └──── RESUME ──────┘  │
   │                  │                           │
   └──── STOP ────────┴────────── STOP ─────────┘
```

| 状态             | recording | paused | 行为        |
| -------------- | --------- | ------ | --------- |
| IDLE（空闲）       | 0         | 0      | 帧不写入磁盘    |
| RECORDING（录制中） | 1         | 0      | 帧写入当前分段   |
| PAUSED（已暂停）    | 1         | 1      | 帧不写入，时长冻结 |

---

## 4. V4L2 采集子系统

V4L2（Video for Linux 2）是 Linux 内核提供的视频设备 API。本子系统封装了从打开摄像头到取帧的完整流程。

### 4.1 采集流程总览

```
open() → QUERYCAP → S_FMT(MJPEG) → REQBUFS(32)
→ QUERYBUF + mmap × 32 → QBUF × 32 → STREAMON
→ poll(POLLIN) → DQBUF → [处理帧] → QBUF → (循环)
→ STREAMOFF → munmap × 32 → close()
```

### 4.2 逐步讲解：`v4l2_open()`

这是采集初始化的核心函数，执行 5 个步骤：

**步骤 1：打开设备**

```c
fd = open(dev, O_RDWR);   // 阻塞模式打开
```

- 使用 `O_RDWR` 而非 `O_RDONLY`，因为 V4L2 的 `ioctl` 需要写权限。
- 阻塞模式打开，但后续通过 `poll()` 实现非阻塞等待。

**步骤 2：查询能力（QUERYCAP）**

```c
ioctl(fd, VIDIOC_QUERYCAP, &cap);
// 检查是否支持视频采集 + 流式 I/O
if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
    !(cap.capabilities & V4L2_CAP_STREAMING)) { ... }
```

- 验证设备确实是摄像头且支持流式 I/O（mmap 方式需要 `V4L2_CAP_STREAMING`）。

**步骤 3：设置格式（S_FMT）**

```c
fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;  // 请求 MJPEG 格式
fmt.fmt.pix.width       = *width;
fmt.fmt.pix.height      = *height;
ioctl(fd, VIDIOC_S_FMT, &fmt);
// 读回实际协商值
ctx->width  = fmt.fmt.pix.width;
ctx->height = fmt.fmt.pix.height;
```

- **关键设计**：请求 `V4L2_PIX_FMT_MJPEG` 而非原始 YUV/RGB。MJPEG 帧本身就是 JPEG 压缩数据，直接存盘即可，省去了软件编码的 CPU 开销。
- 驱动可能不支持请求的分辨率，会就近选择并通过 `fmt.fmt.pix` 返回实际值，因此必须读回。

**步骤 4：请求缓冲区（REQBUFS）**

```c
rb.count  = 32;                    // 请求 32 个内核缓冲区
rb.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
rb.memory = V4L2_MEMORY_MMAP;      // mmap 模式
ioctl(fd, VIDIOC_REQBUFS, &rb);
ctx->buf_count = rb.count;          // 实际分配数量
```

- 32 个缓冲区是参考 USB 摄像头测试程序的经验值，足够应对 USB 传输抖动。
- `V4L2_MEMORY_MMAP` 表示内核分配物理连续内存，用户空间通过 mmap 映射访问。

**步骤 5：查询并映射每个缓冲区（QUERYBUF + mmap）**

```c
for (i = 0; i < ctx->buf_count; i++) {
    buf.index = i;
    ioctl(fd, VIDIOC_QUERYBUF, &buf);       // 获取缓冲区的偏移和长度
    ctx->bufs[i].length = buf.length;
    ctx->bufs[i].start = mmap(NULL, buf.length,
                               PROT_READ|PROT_WRITE, MAP_SHARED,
                               fd, buf.m.offset);  // 映射到用户空间
}
```

- `mmap` 的 `MAP_SHARED` 确保用户空间写入能被内核看到（虽然采集场景只读，但惯例如此）。
- `buf.m.offset` 是该缓冲区在设备文件中的偏移量，作为 mmap 的 offset 参数。

### 4.3 启动采集：`v4l2_start()`

```c
// 1. 将所有缓冲区放入内核的输入队列
for (i = 0; i < ctx->buf_count; i++) {
    buf.index = i;
    ioctl(fd, VIDIOC_QBUF, &buf);   // QBUF = Queue Buffer
}
// 2. 启动流
int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
ioctl(fd, VIDIOC_STREAMON, &type);
```

- **QBUF**（Queue Buffer）：把空缓冲区交给内核，内核填充数据后会标记为"可出队"。
- **STREAMON**：启动 DMA 传输，内核开始往队列中的缓冲区填数据。
- 必须先 QBUF 所有缓冲区再 STREAMON，否则内核没有可用缓冲区。

### 4.4 取帧与归还：`v4l2_dequeue()` / `v4l2_queue()`

```c
// 出队：取出一帧已填充数据的缓冲区
int v4l2_dequeue(v4l2_ctx_t *ctx, void **data, int *size, int *index) {
    struct v4l2_buffer buf;
    ioctl(fd, VIDIOC_DQBUF, &buf);       // DQBUF = Dequeue Buffer
    *data  = ctx->bufs[buf.index].start;  // 直接返回 mmap 地址, 零拷贝
    *size  = buf.bytesused;                // 实际数据长度
    *index = buf.index;                    // 缓冲区编号, 用于归还
}

// 归还：处理完毕后将缓冲区放回内核队列
int v4l2_queue(v4l2_ctx_t *ctx, int index) {
    struct v4l2_buffer buf;
    buf.index = index;
    ioctl(fd, VIDIOC_QBUF, &buf);
}
```

**关键要点**：

- **DQBUF 是阻塞的**：如果没有就绪帧，调用会阻塞。因此主循环先用 `poll()` 等待 `POLLIN`，确认有帧后再调用 DQBUF。
- **零拷贝**：`*data` 直接指向 mmap 映射的内核缓冲区地址，没有 `memcpy`。
- **必须及时归还**：处理完一帧后必须调用 `v4l2_queue()` 归还，否则 32 个缓冲区耗尽后采集会停滞。

### 4.5 关闭：`v4l2_close()`

```c
ioctl(fd, VIDIOC_STREAMOFF, &type);   // 停止流
for (i = 0; i < ctx->buf_count; i++)
    munmap(ctx->bufs[i].start, ctx->bufs[i].length);  // 解除映射
free(ctx->bufs);                        // 释放缓冲区描述数组
close(fd);                               // 关闭设备
```

---

## 5. 存储管理器 — 分段循环录制

存储管理器负责将视频帧写入分段文件，并在空间不足时自动删除最旧分段。所有函数实现在 `storage_mgr.c` 中。

### 5.1 循环录制策略

```
时间轴:  ├──── 60s ────┼──── 60s ────┼──── 60s ────┤
文件:     20260822_100000_dvr.h264
                    20260822_100100_dvr.h264
                                    20260822_100200_dvr.h264

空间管理: 总大小 > 配额 × 80% → 删除最旧文件 → 循环检查
```

四个核心策略：

1. **基于时间的分段**：每 `segment_sec`（默认 60 秒）轮转一个新文件。
2. **时间戳命名**：`YYYYMMDD_HHMMSS_dvr.h264`，按文件名即可排序。
3. **空间配额管理**：目录总大小超过配额的 80% 时，删除最旧分段，直到低于阈值。
4. **缓冲 I/O 优化**：64KB 全缓冲 + 每 15 次写入 fflush 一次，平衡吞吐量与数据安全。

### 5.2 初始化：`dvr_storage_init()`

```c
int dvr_storage_init(dvr_storage_t *st, const char *path, int max_mb, int seg_sec)
{
    // 先将 path 保存到本地缓冲区 (关键! 见踩坑记录)
    char saved_path[256];
    strncpy(saved_path, path, sizeof(saved_path) - 1);

    memset(st, 0, sizeof(*st));                     // 清零
    strncpy(st->base_path, saved_path, ...);        // 复制路径
    st->max_size_mb = max_mb;
    st->segment_sec = seg_sec;

    mkdir(saved_path, 0755);                         // 确保目录存在
}
```

### 5.3 打开分段：`dvr_storage_open_segment()`

```c
int dvr_storage_open_segment(dvr_storage_t *st, time_t start)
{
    // 1. 关闭上一个分段 (如有)
    if (st->current_fp) { fclose(st->current_fp); st->current_fp = NULL; }

    // 2. 确保目录存在
    mkdir(st->base_path, 0755);

    // 3. 生成时间戳文件名
    localtime_r(&start, &tm_info);
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_info);
    snprintf(filepath, sizeof(filepath), "%s/%s_dvr.h264", st->base_path, ts);

    // 4. 打开文件并设置 64KB 全缓冲
    st->current_fp = fopen(filepath, "wb");
    setvbuf(st->current_fp, NULL, _IOFBF, 65536);  // 64KB 缓冲区

    st->current_segment++;
    st->current_size = 0;
}
```

**关于 `.h264` 后缀**：虽然文件后缀是 `.h264`，但在 MJPEG 采集模式下，文件内容是**连续的 JPEG 帧**而非 Annex-B H.264 码流。这是历史命名约定，重命名操作已推迟。

### 5.4 写入帧：`dvr_storage_write()`

```c
int dvr_storage_write(dvr_storage_t *st, const uint8_t *data, int len)
{
    if (!st->current_fp) return -1;          // 文件未打开

    size_t n = fwrite(data, 1, len, st->current_fp);
    st->current_size += (int)n;

    // 每 15 次写入刷新一次 (~15fps 下约 1 秒)
    static int write_count = 0;
    if (++write_count % 15 == 0) {
        fflush(st->current_fp);
    }
    return (int)n;
}
```

**设计权衡**：

- 不每次写入都 fflush：减少系统调用，提升吞吐量。
- 每 15 帧 fflush 一次：在 15fps 下约 1 秒刷一次，意外断电最多丢失约 1 秒数据（行车记录仪可接受）。
- 如果不 fflush，64KB 缓冲区在 15fps 下可能缓存约 4 秒数据，断电丢失更多。

### 5.5 关闭分段：`dvr_storage_close()`

```c
void dvr_storage_close(dvr_storage_t *st)
{
    if (st->current_fp) {
        fflush(st->current_fp);    // 确保缓冲数据落盘
        fclose(st->current_fp);
        st->current_fp = NULL;
    }
}
```

**幂等设计**：可安全地多次调用，或在没有文件打开时调用。

### 5.6 空间清理：`dvr_storage_check_and_clean()`

```c
int dvr_storage_check_and_clean(dvr_storage_t *st)
{
    int total_kb = storage_total_size_kb(st->base_path);  // 统计总大小
    int max_kb   = st->max_size_mb * 1024;

    // 删除最旧文件直到使用量低于配额的 80%
    while (total_kb > max_kb * 80 / 100) {
        char oldest[320];
        if (storage_find_oldest(st->base_path, oldest, sizeof(oldest)) != 0)
            break;                                          // 没有更多文件

        stat(oldest, &stbuf);
        unlink(oldest);                                     // 删除最旧文件
        total_kb -= (int)(stbuf.st_size / 1024);
    }
}
```

**两个辅助函数**：

- `storage_total_size_kb()`：遍历目录，`stat()` 每个 `_dvr.h264` 文件，累加大小。
- `storage_find_oldest()`：遍历目录，比较 `stat.st_mtime`（修改时间），找到最旧的文件。

**80% 阈值的意义**：提供**滞后效应（Hysteresis）**。如果用 100% 作为阈值，每次检查可能只删一个文件就刚好低于阈值，导致频繁删除。用 80% 则批量删除到有足够余量，减少操作频率。

### 5.7 枚举分段：`dvr_storage_list()`

```c
int dvr_storage_list(dvr_storage_t *st, dvr_segment_t *list, int max)
```

遍历目录，将所有 `_dvr.h264` 文件的路径、修改时间、大小填入调用者提供的数组。返回实际条目数。

`dvr_segment_t` 结构：

```c
typedef struct {
    char    filepath[256];  // 绝对路径
    time_t  start_time;     // 文件修改时间 (作为录制开始时间的代理)
    int     size_kb;        // 文件大小 (KB)
} dvr_segment_t;
```

---

## 6. 主循环与事件驱动模型

主循环是整个守护进程的核心，使用 `poll()` 实现单线程事件驱动。

### 6.1 初始化序列

`main()` 函数按以下顺序初始化：

```
1. 解析配置文件路径 (argv[1], 默认 ../config/config.ini)
2. memset(&g_ctx, 0, sizeof(g_ctx));  g_ctx.client_fd = -1;
3. log_init() — 打开日志文件
4. dvr_config_load() — 加载配置
5. dvr_storage_init() — 初始化存储管理器 + 创建 /record/ 目录
6. v4l2_open() — 打开摄像头 (失败则 return 1)
7. v4l2_start() — 启动采集流
8. sock_create_server() — 创建 Unix Socket 服务端
9. ringbuffer_init() + protocol_parser_init() — 初始化协议解析
10. signal(SIGTERM/SIGINT, sig_handler) — 注册信号处理
```

### 6.2 poll 事件循环结构

```c
while (g_running) {
    // 构造 pollfd 数组
    pfds[0].fd = g_ctx.v4l2.fd;      // 摄像头
    pfds[1].fd = g_ctx.server_fd;     // 监听 Socket
    if (g_ctx.client_fd >= 0)
        pfds[2].fd = g_ctx.client_fd; // 已连接客户端

    int ret = poll(pfds, nfds, 1000);  // 1 秒超时

    // 1. V4L2 帧处理
    if (pfds[0].revents & POLLIN) { ... }

    // 2. Socket 事件 (接受连接 + 命令处理)
    handle_sockets(&g_ctx, pfds[1].revents, cli_ev);

    // 3. 每 2 秒推送状态
    if (now - last_status_send >= 2) { send_dvr_status(&g_ctx); }

    // 4. poll 超时时定期存储清理 (约每 30 秒)
    if (ret == 0 && ++tick % 30 == 0)
        dvr_storage_check_and_clean(&g_ctx.storage);
}
```

### 6.3 V4L2 帧处理流程

当 `poll()` 返回 V4L2 fd 有 `POLLIN` 事件时：

```
v4l2_dequeue() → 取出帧数据
   │
   ├── frame_total++
   │
   ├── 首帧? → save_snapshot() (自动拍照验证摄像头)
   │
   ├── 每 15 帧? → 写 /tmp/dvr_preview.jpg (预览图)
   │
   ├── 每 150 帧? → 记录状态日志 (帧计数、已录制时长)
   │
   ├── 正在录制且未暂停?
   │     ├── 分段时长超限? → dvr_storage_open_segment() (轮转新分段)
   │     └── dvr_storage_write() (写入当前分段)
   │
   ├── pending_snapshot? → save_snapshot() (用户拍照)
   │
   └── v4l2_queue() → 归还缓冲区给内核
```

**分段轮转逻辑**：

```c
time_t now = time(NULL);
if (seg_start == 0) seg_start = now;
if (now - seg_start >= g_ctx.storage.segment_sec) {
    dvr_storage_open_segment(&g_ctx.storage, now);  // 打开新分段 (自动关闭旧的)
    seg_start = now;
}
```

注意 `seg_start` 是 `main()` 的局部变量，跟踪当前分段的起始时间。

### 6.4 退出序列

收到 `SIGTERM` 或 `SIGINT` 后，`g_running` 被设为 0，主循环退出，执行清理：

```
1. dvr_storage_close() — 关闭当前录制分段 (fflush + fclose)
2. sock_close(client_fd) — 关闭客户端连接
3. ringbuffer_destroy() — 销毁接收缓冲区
4. sock_close(server_fd, socket_path) — 关闭并 unlink 服务端 Socket
5. v4l2_close() — 停止采集流, munmap, 关闭设备
6. log_close() — 关闭日志
```

---

## 7. IPC 通信 — Unix Socket 与二进制协议

### 7.1 通信架构

```
Qt 界面进程                    dvr_daemon 进程
     │                              │
     │──── connect() ─────────────→│  accept()
     │                              │
     │──── 二进制协议帧 ──────────→│  recv() → ringbuffer → protocol_unpack()
     │   (MSG_DVR_START 等)        │
     │                              │
     │←─── 二进制协议帧 ───────────│  sock_send_frame()
     │   (MSG_DVR_STATUS)          │
```

- **传输层**：Unix Domain Socket（`/tmp/car_dvr.sock`），本机进程间通信，比 TCP 快且无需网络协议栈。
- **单客户端模式**：同时只允许一个界面连接，新连接会被静默拒绝。
- **应用层**：自定义二进制帧协议（由 `common/protocol` 模块提供）。

### 7.2 二进制协议帧格式

```
┌────────┬────────┬────────┬──────────┬───────────┬────────┬────────┐
│ HEAD0  │ HEAD1  │  TYPE  │   LEN    │   DATA    │  CRC8  │  TAIL  │
│  0xAA  │  0x55  │ 1 Byte │ 2 Bytes  │ 0~65535 B │ 1 Byte │  0x55  │
└────────┴────────┴────────┴──────────┴───────────┴────────┴────────┘
```

- **帧开销**：7 字节（HEAD 2 + TYPE 1 + LEN 2 + CRC 1 + TAIL 1）。
- **LEN 大端序**：直接位运算构造/还原，不使用 `htons/ntohs`（避免 ARM 小端平台上的歧义）。
- **CRC8**：校验范围 TYPE + LEN + DATA，不含帧头帧尾。
- **粘包/断包处理**：接收方用 5 状态有限状态机（FSM）从字节流中找帧边界，`ringbuffer` 缓存不完整数据。

### 7.3 消息类型定义

定义于 `common/common.h`：

| 消息类型               | 值    | 方向       | 说明      |
| ------------------ | ---- | -------- | ------- |
| `MSG_DVR_START`    | 0x20 | UI → dvr | 开始/恢复录像 |
| `MSG_DVR_STOP`     | 0x21 | UI → dvr | 停止录像    |
| `MSG_DVR_STATUS`   | 0x22 | dvr → UI | 状态推送    |
| `MSG_DVR_SNAPSHOT` | 0x23 | UI → dvr | 拍照      |
| `MSG_DVR_PAUSE`    | 0x26 | UI → dvr | 暂停/继续录像 |

### 7.4 接收与解析流程

在 `handle_sockets()` 中：

```c
// 1. 接收原始字节到临时缓冲区
int n = sock_recv(ctx->client_fd, buf, sizeof(buf));

// 2. 写入环形缓冲区
ringbuffer_write(&ctx->recv_rb, buf, n);

// 3. 循环解析所有完整帧
uint8_t type, data[256]; uint32_t len;
while (protocol_unpack(&ctx->proto_parser, &ctx->recv_rb,
                        &type, data, sizeof(data), &len) == 0) {
    switch (type) {
        case MSG_DVR_START:    ...
        case MSG_DVR_STOP:     ...
        case MSG_DVR_PAUSE:    ...
        case MSG_DVR_SNAPSHOT: ...
    }
}
```

- `protocol_unpack()` 返回 0 表示成功解析一帧，返回 -2 表示数据不足（等待下次可读事件）。
- `while` 循环确保一次 `recv` 中的多个粘包帧都被处理。

### 7.5 连接管理

```c
// 接受新连接
if (server_ev & POLLIN) {
    int fd = sock_accept(ctx->server_fd);
    if (ctx->client_fd >= 0) {
        sock_close(fd, NULL);       // 已有连接, 拒绝新连接
    } else {
        ctx->client_fd = fd;
        protocol_reset(&ctx->proto_parser, &ctx->recv_rb);  // 重置解析器
        send_dvr_status(ctx);        // 连接后立即推送当前状态
    }
}

// 断开连接
if (client_ev & (POLLIN|POLLHUP|POLLERR)) {
    int n = sock_recv(...);
    if (n <= 0) {                     // 对端关闭或出错
        sock_close(ctx->client_fd, NULL);
        ctx->client_fd = -1;
    }
}
```

---

## 8. 录制控制逻辑详解

### 8.1 开始录制（MSG_DVR_START）

有两种场景：

**场景 A：从空闲开始**

```c
if (!ctx->recording) {
    ctx->recording = 1;
    ctx->paused = 0;
    ctx->rec_start_time = time(NULL);     // 记录开始时间
    ctx->rec_elapsed_sec = 0;             // 清零累计时长
    int ret = dvr_storage_open_segment(&ctx->storage, ctx->rec_start_time);
    if (ret != 0) {
        ctx->recording = 0;               // 失败回退, 避免向 NULL 写入
    }
}
```

**场景 B：从暂停恢复**

```c
else if (ctx->paused) {
    ctx->paused = 0;
    ctx->rec_start_time = time(NULL);     // 重置开始时间 (累计时长保留)
    // 当前分段文件保持打开, 继续写入
}
```

**关键设计**：恢复时**不打开新分段**，当前分段继续使用。这意味着暂停期间的时间缺口不会产生空文件，录制内容在同一个分段中是连续的（只是时间上有缺口）。

### 8.2 停止录制（MSG_DVR_STOP）

```c
if (ctx->recording) {
    ctx->recording = 0;
    ctx->paused = 0;
    dvr_storage_close(&ctx->storage);     // fflush + fclose
    ctx->rec_elapsed_sec = 0;             // 清零累计时长
}
```

停止是**不可逆**的：累计时长清零，下次开始是全新会话。

### 8.3 暂停/恢复（MSG_DVR_PAUSE）

这是一个**切换命令**：根据当前状态决定暂停还是恢复。

**暂停**：

```c
if (ctx->recording && !ctx->paused) {
    ctx->paused = 1;
    // 累计当前周期的录制时长
    ctx->rec_elapsed_sec += (uint32_t)(time(NULL) - ctx->rec_start_time);
    if (ctx->storage.current_fp) {
        fflush(ctx->storage.current_fp);  // 刷新缓冲, 确保数据落盘
    }
}
```

**恢复**：

```c
else if (ctx->recording && ctx->paused) {
    ctx->paused = 0;
    ctx->rec_start_time = time(NULL);     // 重置开始时间
}
```

### 8.4 时长计算逻辑

`rec_elapsed_sec` 是理解暂停/恢复的关键：

```
时间轴 (录制 30s → 暂停 10s → 恢复 20s → 暂停):

│← 30s 录制 →│← 10s 暂停 →│← 20s 录制 →│
rec_start    rec_elapsed=30              rec_elapsed=50

当前总时长 = rec_elapsed_sec + (now - rec_start_time)
           = 50 + (now - 恢复时的 rec_start_time)
```

在 `send_dvr_status()` 中：

```c
if (ctx->recording && !ctx->paused) {
    // 录制中: 累计时长 + 当前周期已过时间
    elapsed = (time(NULL) - ctx->rec_start_time) + ctx->rec_elapsed_sec;
} else if (ctx->recording && ctx->paused) {
    // 暂停中: 只显示累计时长 (冻结)
    elapsed = ctx->rec_elapsed_sec;
}
// 已停止: elapsed = 0
```

---

## 9. 拍照与预览机制

### 9.1 拍照函数：`save_snapshot()`

```c
static void save_snapshot(dvr_context_t *ctx, void *data, int size)
{
    // 1. 确保目录存在
    mkdir(dir, 0755);

    // 2. 生成文件名: YYYYMMDD_HHMMSS_NNN.jpg
    time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm);
    ctx->snap_count++;                              // 会话序列号自增
    snprintf(path, sizeof(path), "%s/%s_%03u.jpg", dir, ts, ctx->snap_count);

    // 3. 直接写入 (MJPEG 帧就是合法 JPEG)
    FILE *fp = fopen(path, "wb");
    fwrite(data, 1, size, fp);
    fclose(fp);

    // 4. 通知 UI 拍照完成 (发送文件名)
    if (ctx->client_fd >= 0) {
        uint8_t status[16];
        status[0] = 0;                              // state = idle
        status[1] = (uint8_t)(ctx->snap_count & 0xFF);
        memcpy(status + 2, filename, name_len);    // 文件名 (截断到 12 字节)
        sock_send_frame(ctx->client_fd, MSG_DVR_STATUS, status, sizeof(status));
    }
}
```

**文件名中的 `NNN` 序列号**：`snap_count` 是会话级递增计数器，防止同一秒内多次拍照产生文件名冲突。例如 `20260822_103000_001.jpg`、`20260822_103000_002.jpg`。

### 9.2 两种拍照触发方式

**方式一：首帧自动拍照**

```c
static int first_frame = 1;
if (first_frame) {
    save_snapshot(&g_ctx, data, size);
    first_frame = 0;
}
```

启动后第一帧自动保存，用于验证摄像头是否正常工作。

**方式二：用户异步触发**

```c
// Socket 命令处理中:
case MSG_DVR_SNAPSHOT:
    ctx->pending_snapshot = 1;   // 只设置标志, 不立即保存
    break;

// 帧处理中:
if (g_ctx.pending_snapshot) {
    save_snapshot(&g_ctx, data, size);
    g_ctx.pending_snapshot = 0;  // 清除标志
}
```

**异步设计的原因**：

- Socket 命令处理函数中没有帧数据（帧数据只在 V4L2 事件处理中可用）。
- 如果在 Socket 处理中阻塞等待下一帧，会卡住整个事件循环。
- 设置标志后，下一帧到达时自然保存，解耦了异步命令与同步帧循环。

### 9.3 预览图更新

```c
if (g_ctx.frame_total % 15 == 0) {
    FILE *fp = fopen("/tmp/dvr_preview.jpg", "wb");
    if (fp) { fwrite(data, 1, size, fp); fclose(fp); }
}
```

- 每 15 帧（约 15fps 下 1 秒）更新一次 `/tmp/dvr_preview.jpg`。
- Qt 界面通过读取该文件显示实时预览。
- 使用固定文件名覆盖写入，界面端只需定时读取同一文件。

---

## 10. 状态推送机制

### 10.1 状态包格式

`send_dvr_status()` 发送 16 字节的二进制状态包（`MSG_DVR_STATUS` 类型）：

| 字节偏移     | 字段          | 类型        | 说明                 |
| -------- | ----------- | --------- | ------------------ |
| [0]      | state       | uint8     | 0=空闲, 1=录制中, 2=已暂停 |
| [1..4]   | elapsed_sec | uint32 LE | 已录制秒数（不含暂停时间）      |
| [5..6]   | width       | uint16 LE | 帧宽度                |
| [7..8]   | height      | uint16 LE | 帧高度                |
| [9..12]  | frame_total | uint32 LE | 生命周期帧计数器           |
| [13..15] | reserved    | —         | 保留（填零）             |

**多字节字段使用小端序**（与协议帧 LEN 字段的大端序不同，这是历史设计，注意区分）。

### 10.2 推送时机

状态在以下时机发送：

1. **每 2 秒定时推送**：主循环中 `if (now - last_status_send >= 2)`。
2. **客户端连接时**：`accept()` 后立即调用 `send_dvr_status()`，界面无需等待 2 秒周期。
3. **命令执行后**：START / STOP / PAUSE 命令处理后立即推送，确保界面实时更新。
4. **拍照完成后**：`save_snapshot()` 中发送包含文件名的状态包。

---

## 11. 配置加载

### 11.1 配置文件格式

INI 格式，读取 `[dvr]` 节：

```ini
[dvr]
camera_dev = /dev/video1
record_path = /record
max_size_mb = 2000
seg_duration = 60
```

### 11.2 加载逻辑：`dvr_config_load()`

```c
int dvr_config_load(dvr_context_t *ctx, const char *conf_file)
{
    config_t conf;
    int loaded = (conf_file && config_load(&conf, conf_file) == 0);

    if (loaded) {
        // 从配置文件读取, 带默认值回退
        config_get_str(&conf, "dvr", "camera_dev", DVR_DEFAULT_DEVICE, ...);
        config_get_str(&conf, "dvr", "record_path", DVR_DEFAULT_STORAGE, ...);
        ctx->storage.max_size_mb = config_get_int(&conf, "dvr", "max_size_mb", DVR_DEFAULT_STORAGE_MB);
        ctx->storage.segment_sec = config_get_int(&conf, "dvr", "seg_duration", DVR_DEFAULT_SEGMENT_SEC);
        config_unload(&conf);
    } else {
        // 配置文件缺失: 使用编译期默认值
        strncpy(ctx->camera_dev, DVR_DEFAULT_DEVICE, ...);
        strncpy(ctx->storage.base_path, DVR_DEFAULT_STORAGE, ...);
        ctx->storage.max_size_mb = DVR_DEFAULT_STORAGE_MB;
        ctx->storage.segment_sec = DVR_DEFAULT_SEGMENT_SEC;
    }
}
```

**设计要点**：

- 配置文件缺失或格式错误时**静默回退**到默认值，不影响启动。
- 每个配置项都有默认值常量（`DVR_DEFAULT_*`），定义在 `dvr_daemon.h` 中。

### 11.3 默认值常量

```c
#define DVR_DEFAULT_DEVICE      "/dev/video1"
#define DVR_DEFAULT_WIDTH       640
#define DVR_DEFAULT_HEIGHT      480
#define DVR_DEFAULT_STORAGE     "/record/"
#define DVR_DEFAULT_STORAGE_MB  2000      // 2GB 配额
#define DVR_DEFAULT_SEGMENT_SEC 60        // 60 秒/段
#define DVR_SOCKET_PATH         "/tmp/car_dvr.sock"
#define DVR_LOG_PATH            "/var/log/car_terminal/dvr.log"
#define DVR_POLL_TIMEOUT_MS     1000      // poll 超时 1 秒
```

---

## 12. 构建系统 Makefile

### 12.1 构建目标

```makefile
SRCS  = dvr_daemon.c v4l2_capture.c storage_mgr.c
TARGET = build/dvr_daemon
```

三个源文件编译链接为一个可执行文件 `dvr_daemon`。

### 12.2 交叉编译与主机编译

```makefile
HOST ?= 0
ifeq ($(HOST),1)
  CC = gcc                          # 主机编译 (x86)
else
  CROSS_COMPILE ?= arm-buildroot-linux-gnueabihf-
  CC = $(CROSS_COMPILE)gcc          # 交叉编译 (ARM)
endif
```

- 默认交叉编译到 ARM Buildroot 平台。
- `make HOST=1` 或 `make host` 在主机上编译（用于开发测试）。

### 12.3 编译与链接选项

```makefile
CFLAGS  = -Wall -Wextra -std=gnu99 -Os -g -ffunction-sections -fdata-sections
CFLAGS += -I../common -I../common/log -I../common/config
LDFLAGS = -Wl,--gc-sections -lpthread -lrt -lm
```

| 选项                                                      | 作用                             |
| ------------------------------------------------------- | ------------------------------ |
| `-Os`                                                   | 优化大小（嵌入式设备资源有限）                |
| `-g`                                                    | 保留调试信息                         |
| `-ffunction-sections -fdata-sections` + `--gc-sections` | 链接时剔除未使用的函数/数据，减小二进制体积         |
| `-lpthread -lrt -lm`                                    | 依赖 POSIX 线程、实时库、数学库            |
| `-Wno-format-truncation`                                | 抑制 `snprintf` 截断警告（文件名截断是预期行为） |

### 12.4 依赖公共库

```makefile
COMMON_LIB ?= ../common/lib/libcarcommon.a
```

链接 `libcarcommon.a`，提供日志、配置、Socket、协议、环形缓冲区等公共功能。

### 12.5 常用命令

```bash
make              # 交叉编译 (ARM)
make HOST=1       # 主机编译 (x86)
make host         # 同上
make test         # 编译并运行单元测试
make clean        # 清理 build/ 目录
make help         # 显示帮助
```

---

## 13. 单元测试

测试文件 `test/test_dvr.c`，使用 `make test` 编译运行。

### 13.1 测试框架

极简的自定义测试宏（无外部依赖）：

```c
#define TS(n)  printf("\n========== [TEST] %s ==========\n", n)  // Test Suite
#define TC(d)  do { t++; printf("  [%d] %s ... ", t, d); } while(0)  // Test Case
#define OK()   do { printf("OK\n"); p++; } while(0)
#define FL(m)  do { printf("FAIL: %s\n", m); f++; return; } while(0)
#define AS(c,m) do { if(!(c)) FL(m); } while(0)  // Assert
```

### 13.2 测试覆盖

**测试 1：存储管理器（`test_storage`）**

| 用例                                      | 验证内容                            |
| --------------------------------------- | ------------------------------- |
| init → dir created                      | `dvr_storage_init()` 后目录存在      |
| open_segment → file created, write data | 打开分段后写入 256 字节成功                |
| list → 1 segment found                  | `dvr_storage_list()` 能枚举到刚创建的分段 |
| check_and_clean → within limit          | 配额内不删除文件                        |

**测试 2：状态枚举（`test_states`）**

验证 `DVR_IDLE=0`、`DVR_RECORDING=1`、`DVR_PAUSED=2`、`DVR_ERROR=3` 的值。

### 13.3 测试编译

```makefile
$(TEST): test/test_dvr.c v4l2_capture.c storage_mgr.c dvr_daemon.c
    $(CC) $(CFLAGS) -DTEST_MODE -o $@ test/test_dvr.c \
        v4l2_capture.c storage_mgr.c dvr_daemon.c $(COMMON_LIB) $(LDFLAGS)
```

- 定义 `TEST_MODE` 宏（当前代码中未使用条件编译，但预留了扩展空间）。
- 测试不依赖真实摄像头，只测试纯软件逻辑（存储管理器、配置、枚举）。

---

## 14. 关键设计决策与踩坑记录

### 14.1 为什么用 MJPEG 而不是 H.264？

- USB 摄像头普遍硬件支持 MJPEG 输出，无需 CPU 编码。
- MJPEG 帧本身就是合法 JPEG，拍照直接存盘，录像直接 fwrite。
- 缺点：文件体积比 H.264 大（无帧间压缩），但通过分段循环录制管理空间。

### 14.2 为什么用 poll 而不是多线程？

- 单线程事件循环避免了锁、竞态条件、死锁等并发问题。
- V4L2 采集和 Socket 通信都是 I/O 密集型，`poll()` 可以高效等待。
- 帧处理（fwrite）足够快，不会阻塞事件循环。
- 代码更简单，更适合嵌入式资源受限环境。

### 14.3 踩坑 A：memset 与路径别名

**问题**：`dvr_storage_init()` 的调用者通常传入 `&ctx->storage.base_path` 作为 `path` 参数，这与 `st->base_path` 是**同一块内存**。

```c
// 错误写法:
memset(st, 0, sizeof(*st));           // 先清零 → path 指向的内容也被清零!
strncpy(st->base_path, path, ...);    // 此时 path 已是空字符串
```

**修复**：在 memset 之前将 path 保存到栈上的本地缓冲区：

```c
char saved_path[256];
strncpy(saved_path, path, sizeof(saved_path) - 1);  // 先保存
memset(st, 0, sizeof(*st));                            // 再清零
strncpy(st->base_path, saved_path, ...);              // 从本地副本复制
```

**教训**：当函数参数可能指向结构体内部字段时，要注意 memset/memcpy 的操作顺序。

### 14.4 踩坑 B：fopen 前缺少 mkdir

**问题**：首次运行时 `/record/` 目录不存在，`fopen(path, "wb")` 因 `ENOENT` 静默失败，返回 NULL。后续 `dvr_storage_write()` 向 NULL 写入导致崩溃或录制静默失败。

**修复**：在 `dvr_storage_open_segment()` 和 `dvr_storage_init()` 中都添加 `mkdir()` 调用，`EEXIST` 错误被忽略（目录已存在是正常的）。

### 14.5 踩坑 C：开始录制失败未回退

**问题**：`MSG_DVR_START` 处理中，如果 `dvr_storage_open_segment()` 失败（如目录权限不足），`recording` 已被设为 1，但 `current_fp` 为 NULL。后续帧处理中 `dvr_storage_write()` 持续失败。

**修复**：打开分段失败时立即将 `recording` 设回 0：

```c
if (ret != 0) {
    ctx->recording = 0;   // 回退
}
```

### 14.6 设计权衡：fflush 频率

| 策略                | 吞吐量       | 崩溃数据丢失        |
| ----------------- | --------- | ------------- |
| 每次写入 fflush       | 低（频繁系统调用） | 最少            |
| 每 15 帧 fflush（当前） | 中         | ~1 秒（15fps）   |
| 仅 fclose 时 flush  | 高         | 最多 64KB（~4 秒） |

当前选择每 15 帧 fflush，在行车记录仪场景下是合理的折中。

### 14.7 设计权衡：80% 清理阈值

使用配额的 80% 而非 100% 作为清理触发阈值：

- 避免在配额边界频繁删除（抖动）。
- 批量删除到有足够余量，减少清理操作频率。
- 预留 20% 空间应对突发写入。

---

## 15. 学习路径建议

按照以下顺序学习，可以循序渐进地掌握整个模块：

### 第一阶段：理解整体框架

1. 阅读 `README.md`，了解模块功能和文件结构。
2. 阅读 `dvr_daemon.h`，掌握三个核心数据结构（`v4l2_ctx_t`、`dvr_storage_t`、`dvr_context_t`）。
3. 阅读 `dvr_daemon.c` 顶部的架构注释（第 1-44 行），理解 poll 事件循环的整体流程。

### 第二阶段：V4L2 采集子系统

4. 阅读 `v4l2_capture.h`，了解 API 接口。
5. 逐函数阅读 `v4l2_capture.c`：
   - `v4l2_open()`：理解 open → QUERYCAP → S_FMT → REQBUFS → mmap 流程。
   - `v4l2_start()`：理解 QBUF + STREAMON。
   - `v4l2_dequeue()` / `v4l2_queue()`：理解取帧与归还的零拷贝机制。
   - `v4l2_close()`：理解资源释放顺序。
6. 如果不熟悉 V4L2，可参考 Linux 内核文档 `Documentation/userspace-api/media/v4l/`。

### 第三阶段：存储管理器

7. 阅读 `storage_mgr.c` 顶部的策略说明（第 1-33 行）。
8. 逐函数阅读：
   - `dvr_storage_init()`：注意 memset 别名问题。
   - `dvr_storage_open_segment()`：理解文件命名和缓冲设置。
   - `dvr_storage_write()`：理解 fflush 频率权衡。
   - `dvr_storage_check_and_clean()`：理解循环删除算法和 80% 阈值。
   - `dvr_storage_list()`：理解目录枚举。

### 第四阶段：主循环与 IPC

9. 阅读 `dvr_daemon.c` 的 `main()` 函数，理解初始化序列和主循环结构。
10. 阅读 `handle_sockets()`，理解 Unix Socket 连接管理和命令分发。
11. 结合 `common/protocol/protocol.h` 理解二进制协议帧格式和状态机解析。
12. 阅读 `send_dvr_status()`，理解状态包格式和推送时机。

### 第五阶段：录制控制与拍照

13. 仔细阅读 `handle_sockets()` 中的 START / STOP / PAUSE 处理，结合 `rec_elapsed_sec` 理解暂停/恢复的时长计算。
14. 阅读 `save_snapshot()` 和 `pending_snapshot` 标志，理解异步拍照设计。
15. 理解首帧自动拍照、预览图更新、状态日志等定期任务。

### 第六阶段：构建与测试

16. 阅读 `Makefile`，理解交叉编译、编译选项、公共库依赖。
17. 阅读 `test/test_dvr.c`，理解单元测试覆盖范围。
18. 尝试 `make HOST=1` 主机编译，运行 `make test`。

### 扩展学习

- 阅读 `common/` 目录下的公共模块（log、config、socket、protocol、ringbuffer），理解 dvr_daemon 依赖的基础设施。
- 了解 Qt 界面端如何通过 Unix Socket 与 dvr_daemon 交互。
- 思考如何扩展功能：如添加音频录制、GPS 轨迹叠加、事件录像（碰撞锁定）等。

---

## 附录 A：关键常量速查

| 常量                         | 值                               | 含义             |
| -------------------------- | ------------------------------- | -------------- |
| `DVR_DEFAULT_DEVICE`       | `/dev/video1`                   | 默认摄像头设备        |
| `DVR_DEFAULT_WIDTH/HEIGHT` | 640/480                         | 默认分辨率          |
| `DVR_DEFAULT_STORAGE`      | `/record/`                      | 默认录制目录         |
| `DVR_DEFAULT_STORAGE_MB`   | 2000                            | 默认存储配额 (2GB)   |
| `DVR_DEFAULT_SEGMENT_SEC`  | 60                              | 默认分段时长 (60秒)   |
| `DVR_SOCKET_PATH`          | `/tmp/car_dvr.sock`             | Unix Socket 路径 |
| `DVR_LOG_PATH`             | `/var/log/car_terminal/dvr.log` | 日志文件路径         |
| `DVR_POLL_TIMEOUT_MS`      | 1000                            | poll 超时 (1秒)   |
| `V4L2_BUF_COUNT`           | 32                              | V4L2 内核缓冲区数量   |

## 附录 B：函数索引

### V4L2 采集（v4l2_capture.c）

| 函数               | 作用                         |
| ---------------- | -------------------------- |
| `v4l2_open()`    | 打开摄像头, 设置格式, 分配 mmap 缓冲区   |
| `v4l2_start()`   | QBUF 所有缓冲区 + STREAMON      |
| `v4l2_dequeue()` | DQBUF 取出一帧 (零拷贝)           |
| `v4l2_queue()`   | QBUF 归还缓冲区                 |
| `v4l2_get_fd()`  | 获取设备 fd                    |
| `v4l2_close()`   | STREAMOFF + munmap + close |

### 存储管理（storage_mgr.c）

| 函数                              | 作用                        |
| ------------------------------- | ------------------------- |
| `dvr_storage_init()`            | 初始化存储管理器, 创建目录            |
| `dvr_storage_open_segment()`    | 关闭旧分段, 打开新的时间戳分段          |
| `dvr_storage_write()`           | 追加帧数据到当前分段                |
| `dvr_storage_close()`           | fflush + fclose 当前分段 (幂等) |
| `dvr_storage_check_and_clean()` | 统计空间, 超配额删除最旧分段           |
| `dvr_storage_list()`            | 枚举所有分段文件                  |

### 主程序（dvr_daemon.c）

| 函数                  | 作用                    |
| ------------------- | --------------------- |
| `dvr_config_load()` | 从 INI 文件加载配置          |
| `save_snapshot()`   | 将当前帧保存为 .jpg 拍照文件     |
| `send_dvr_status()` | 向 UI 发送 16 字节二进制状态包   |
| `handle_sockets()`  | 处理 Socket 连接事件和命令分发   |
| `main()`            | 初始化 + poll 主循环 + 清理退出 |

---

> 文档版本：基于 dvr_daemon 模块当前源码生成。如有代码更新，请同步修订对应章节。
