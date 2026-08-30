# common — 公共库

编译产出 `libcarcommon.a` 静态库（或 `libcarcommon.so`），供全部守护进程链接。

## 目录说明

本目录只放**实现**（`.c`），公共头文件已统一迁移到仓库根目录的 `include/`。
源码通过 `-Iinclude` 引用，例如：

```c
#include "common.h"
#include "log/log.h"
#include "protocol/protocol.h"
```

## 子模块

| 子模块 | 源文件 | 头文件 | 功能 |
|---|---|---|---|
| log | `log/log.c` | `include/log/log.h` | 分级日志（DEBUG/INFO/WARN/ERROR），按大小轮转，互斥锁保护 |
| protocol | `protocol/protocol.c` | `include/protocol/protocol.h` | 二进制帧组包/拆包，5 状态机，CRC8 校验 |
| socket | `socket/unix_socket.c` | `include/socket/unix_socket.h` | UNIX Domain Socket 封装，非阻塞 IO |
| ringbuffer | `ringbuffer/ringbuffer.c` | `include/ringbuffer/ringbuffer.h` | 环形缓冲区，解决流式数据粘包/断包 |
| config | `config/config.c` | `include/config/config.h` | INI 配置文件解析 |
| crc | `crc/crc8.c` | `include/crc/crc8.h` | CRC8 查表法校验 |
| timer | `timer/timer.c` | `include/timer/timer.h` | 基于 `timerfd` 的定时器封装 |

`include/common.h` 定义公共类型（`ret_code_t`、`proc_config_t`）、消息类型常量与进程状态枚举，不属于任何单个子模块。

## 编译

```bash
make                # 编译静态库 lib/libcarcommon.a
make shared         # 编译动态库 lib/libcarcommon.so
make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-    # 交叉编译
make clean
```

单元测试已集中到仓库根目录的 `tests/`：

```bash
make -C ../../tests      # 编译并运行全部单元测试
```

## 在其他模块中使用

守护进程的 Makefile 传入 `INC_DIR` 与 `COMMON_LIB` 两个变量：

```makefile
INC_DIR    ?= ../../include
COMMON_LIB ?= ../common/lib/libcarcommon.a

CFLAGS  += -I$(INC_DIR)
LDFLAGS += $(COMMON_LIB) -lpthread -lrt
```

## 依赖关系

```
timer ──────────────┐
crc ────────────────┤
ringbuffer ──┐      │
             ▼      │
config    protocol ─┤     所有模块依赖 common.h
   │          │     │
   └──────────┴─────┤
                    ▼
                  socket ── 依赖 protocol + log
                  log ───── 依赖 common.h
```

- `protocol.h` 依赖 `common.h` 与 `ringbuffer.h`
- `socket` 依赖 `protocol` 与 `log`
- `log` 依赖 `common.h`
- 其余模块无内部依赖

## 学习要点

| 知识点 | 出处 |
|---|---|
| 静态库打包 (`ar rcs`) | `Makefile` |
| 位置无关代码 (`-fPIC`) | `Makefile` |
| 段回收 (`-ffunction-sections` + `-Wl,--gc-sections`) | `Makefile` |
| 有限状态机 | `protocol/protocol.c` |
| 环形缓冲区 | `ringbuffer/ringbuffer.c` |
| 可变参数与线程安全时间函数 | `log/log.c` |
| `timerfd` 高精度定时器 | `timer/timer.c` |

各模块的实现细节见 [docs/modules/](../../docs/modules/) 与 [docs/学习文档.md](../../docs/学习文档.md)。
