# input_daemon — 按键输入管理守护进程

## 功能概述

通过 Linux input 子系统监听开发板物理按键，识别手势并上报 UI：

- **KEY1 / KEY2 按键监听**：读取 `/dev/input/eventX`
- **三种手势识别**：短按 / 长按 (>2s) / 双击
- **软件消抖**：过滤 <30ms 的抖动信号
- **LED 闪烁反馈**：按键时异步闪烁 100ms
- **事件上报**：UNIX Socket 推送 `MSG_KEY_EVENT` 给 Qt UI

## 硬件连接

| 硬件 | 接口 | 说明 |
|------|------|------|
| KEY1 | GPIO (input 子系统) | 功能键 (菜单/语音助手) |
| KEY2 | GPIO (input 子系统) | 返回键 (返回/主页) |
| 用户 LED | sysfs LED | 按键反馈闪烁 |

## 文件结构

```
input_daemon/
├── input_daemon.h    # 配置宏、数据结构、状态机枚举
├── input_daemon.c    # 主程序 (epoll 循环、Socket 服务、LED 反馈)
├── key_parser.h      # 按键解析器头文件
├── key_parser.c      # 手势状态机实现 (消抖/短按/长按/双击)
├── led_control.h     # LED 控制头文件
├── led_control.c     # sysfs LED 操作
├── Makefile          # x86/ARM 双编译
├── test/
│   └── test_input.c  # 6 组单元测试 (纯软件)
└── README.md
```

## 编译运行

```bash
# ARM 交叉编译
make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-

# x86 本地测试
make HOST=1

# 单元测试
make test

# 运行 (需 root)
./input_daemon /etc/car_terminal/config.ini
```

## 手势状态机

```
IDLE ──press──→ PRESSED ──held>2s──→ LONG_PRESS → report
                   │
                release (<2s)
                   │
                   ▼
              WAIT_DOUBLE ──>500ms──→ SHORT_PRESS → report
                   │
              press<500ms
                   │
                   ▼
              PRESSED_2ND ──release──→ DOUBLE_CLICK → report
```

## 涉及的 Linux 知识点

| 知识点 | 说明 | 对应代码 |
|--------|------|---------|
| **input 子系统** | `/dev/input/eventX`, `struct input_event` | `input_daemon.c` |
| **epoll** | input fd + socket + timerfd×2 统一监听 | `input_daemon.c` |
| **按键手势 FSM** | 有限状态机: 短按/长按/双击 | `key_parser.c` |
| **消抖** | 软件过滤机械按键抖动 | `key_parser.c` |
| **timerfd** | 长按检测 & 双击窗口定时 | `input_daemon.c` |
| **signalfd** | 信号融入 epoll | `input_daemon.c` |
| **sysfs LED** | `/sys/class/leds/<name>/brightness` | `led_control.c` |
| **UNIX Socket** | IPC 推送按键事件 | `input_daemon.c` |

## IPC 消息

| 方向 | 消息类型 | 说明 |
|------|---------|------|
| input → UI | `MSG_KEY_EVENT` (0x70) | 按键手势通知 |
| UI → input | `MSG_LED_CONTROL` (0x71) | 远程 LED 控制 |

## 配置

```ini
[input]
long_press_ms = 2000    # 长按判定时间 (ms)
double_click_ms = 500   # 双击窗口 (ms)
device = /dev/input/event0
led_name = user-led
```

## 注意事项

1. **权限**：读取 `/dev/input/eventX` 和写入 `/sys/class/leds/` 需要 root
2. **键码映射**：KEY1/KEY2 的 Linux 键码取决于设备树，默认映射 `KEY_F1`/`KEY_F2`，如不匹配请查阅 `/proc/bus/input/devices` 或使用 `evtest` 工具确认
3. **LED 名称**：不同 BSP 下 LED 名称不同（`user-led`/`led0`/`sys-led`），可通过 `ls /sys/class/leds/` 查看
4. **自动重复**：内核发送的 `value=2` 重复事件被忽略，手势识别只依赖首次按下和释放
