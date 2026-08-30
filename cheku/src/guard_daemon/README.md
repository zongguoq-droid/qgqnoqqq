# guard_daemon — 系统守护进程

## 功能

- **进程管理**: fork+exec 启动7个子进程, SIGCHLD+waitpid 监控, 崩溃自动重启
- **守护进程化**: 双fork标准daemonize, 脱离终端
- **系统监控**: /proc读取 CPU/内存/负载, /sys读取温度, statfs检查磁盘
- **告警处理**: 温度>80℃ / 存储<100MB 告警 + 自动清理日志
- **UNIX Socket**: UI查看系统状态, 手动重启指定进程

## 子进程管理

启动顺序 (按依赖):
```
gps_daemon → sensor_daemon → input_daemon → canbus_daemon → av_daemon → dvr_daemon → net_daemon
    0ms       +500ms         +500ms         +500ms         +1000ms      +1000ms      +1000ms
```

重启策略: 窗口内最多重启5次 → 超过后标记为ERROR停止重试

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
make test

# 守护进程模式 (后台)
./guard_daemon /etc/car_terminal/config.ini

# 调试模式 (前台, 不daemonize)
./guard_daemon /etc/car_terminal/config.ini -f
```

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
