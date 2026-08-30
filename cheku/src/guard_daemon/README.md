# guard_daemon — 系统守护进程

## 功能

- **进程管理**: fork+exec 启动子进程, SIGCHLD+waitpid 监控, 崩溃自动重启
- **守护进程化**: 双fork标准daemonize, 脱离终端
- **系统监控**: /proc读取 CPU/内存/负载, /sys读取温度, statfs检查磁盘
- **告警处理**: 温度>80℃ / 存储<100MB 告警 + 自动清理日志
- **UNIX Socket**: UI查看系统状态, 手动重启指定进程

## 子进程管理

进程表共 **7 个定义**, 其中 **5 个默认启用**, 2 个为预留槽位:

| 进程 | 默认 | 说明 |
|---|---|---|
| gps_daemon | 启用 | GPS 定位 |
| sensor_daemon | **预留** | 功能由 Qt SensorThread 承担 |
| input_daemon | 启用 | 按键输入 |
| canbus_daemon | 启用 | CAN 总线 |
| av_daemon | 启用 | 音频播放 |
| dvr_daemon | 启用 | 行车记录 |
| net_daemon | **预留** | 云端通信, 未部署 |

启动顺序与延迟 (按依赖关系, 延迟为累积值):

```
gps_daemon → input_daemon → canbus_daemon → av_daemon → dvr_daemon
    0ms         1000ms         1500ms        2500ms       3500ms
```

预留进程 (`enabled=0`) 只占位, 不启动、不监控, 但仍保留定义,
以便未来部署时无需改代码。

启停可通过 `config/config.ini` 的 `[processes]` 节配置:

```ini
[processes]
gps_daemon    = 1
sensor_daemon = 0
net_daemon    = 0
```

重启策略: 窗口内 (60s) 最多重启 5 次 → 超过后标记为 ERROR 停止重试

## 文件结构

```
guard_daemon/
├── guard_daemon.h  # 配置/structs (child_process_t, system_status_t)
├── guard_daemon.c  # 主程序 (daemonize + fork/exec + SIGCHLD + /proc + epoll)
├── Makefile
├── test/test_guard.c
└── README.md
```

## 涉及的 Linux 知识点

| 知识点 | 说明 | 代码 |
|--------|------|------|
| **daemonize** | 双fork + setsid + chdir + umask | `guard_daemonize()` |
| **fork+exec** | 子进程创建与程序替换 | `guard_child_start()` |
| **SIGCHLD+waitpid** | 子进程退出回收 (避免僵尸) | `handle_sigchld()` |
| **signalfd** | 信号→fd 融入epoll | `setup_signals` |
| **/proc文件系统** | CPU/内存/负载监控 | `read_cpu_percent()` 等 |
| **statfs** | 文件系统空间查询 | `read_fs_free_mb()` |
| **指数退避重启** | 窗口内限制重试次数 | `guard_child_restart()` |

## 编译运行

```bash
make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-

# 单元测试已集中到仓库根目录 tests/
make -C ../../tests

# 守护进程模式 (后台) — 拉起并监控全部子进程
./guard_daemon /etc/car_terminal/config.ini

# 调试模式 (前台, 不daemonize)
./guard_daemon /etc/car_terminal/config.ini -f

# 不传配置文件时使用内置默认进程表
./guard_daemon
```

配置文件路径会被透传给所有子进程, 因此子进程与 guard 使用同一份配置。

## IPC 消息

| 方向 | 消息 | 说明 |
|------|------|------|
| UI → guard | `MSG_SERVICE_STATUS` (0x01) | 查询系统状态 |
| guard → UI | `MSG_SERVICE_STATUS` | 返回 CPU/内存/磁盘/子进程表 |
| UI → guard | `MSG_SERVICE_RESTART` (0x02) | 重启指定进程 |

## 配置

```ini
[guard]
heartbeat_interval = 5   # 状态检查间隔 (秒)
max_restart_count = 5    # 窗口内最大重启次数
restart_window = 60      # 重启计数窗口 (秒)
```
