# gps_daemon — GPS 定位守护进程

## 功能概述

通过串口读取 GPS 模块 NMEA-0183 数据, 解析定位信息并上报 UI:

- **串口通信**: termios 配置, 9600bps/8N1/非阻塞
- **NMEA-0183 解析**: 字符级状态机, 支持 GGA/RMC/GSV
- **里程累计**: Haversine 公式计算相邻定位点距离
- **轨迹记录**: 环形缓冲保存历史轨迹点 (1小时)
- **实时推送**: UNIX Socket 每秒推送 gps_data_t 给 UI
- **定位状态**: 无定位/2D/3D 指示

## 硬件连接

| GPS 模块 | i.MX6ULL | 说明 |
|---------|----------|------|
| TX | RXD (UART) | GPS→开发板 |
| RX | TXD (UART) | 开发板→GPS (配置用) |
| VCC | 3.3V/5V | 供电 |
| GND | GND | 地 |

设备节点: `/dev/ttymxc5` (i.MX6ULL 原生 UART5)

## 文件结构
```
gps_daemon/
├── gps_daemon.h    # 配置宏、数据结构 (gps_context_t, gps_odometer_t)
├── gps_daemon.c    # 主程序 (epoll + 里程 + 轨迹 + Socket)
├── nmea_parser.h   # NMEA 解析器头文件 (状态机设计)
├── nmea_parser.c   # NMEA 解析器实现 (字符级状态机 + GGA/RMC/GSV提取)
├── uart_util.h     # 串口工具头文件
├── uart_util.c     # termios 串口配置
├── Makefile
├── test/test_gps.c # 7 组测试 (度分转换/Haversine/GGA/RMC/GSV/校验和/容错)
└── README.md
```

## NMEA 解析状态机

```
IDLE ──'$'──→ TALKER ──2chars──→ TYPE ──3chars──→ FIELD ──','──→ FIELD (循环)
  ↑              │                  │               │
  └──CR+LF───────┴──────────────────┴───────────────┴──'*'──→ CHECKSUM ──CR──→ LF → IDLE
                                                                          (完成, 返回1)
```

## Harversine 距离计算

```
a = sin²(Δlat/2) + cos(lat1)·cos(lat2)·sin²(Δlon/2)
c = 2·atan2(√a, √(1-a))
d = 6371000 · c  (米)
```

## 涉及的 Linux 知识点

| 知识点 | 说明 | 对应代码 |
|--------|------|---------|
| **termios** | 串口配置 (波特率/8N1/非阻塞/原始模式) | `uart_util.c` |
| **字符级状态机** | NMEA-0183 协议解析 (不使用 sscanf) | `nmea_parser.c` |
| **XOR 校验和** | NMEA 数据完整性验证 | `nmea_parser.c` |
| **epoll** | uart fd + timer fd + socket + signal 统一监听 | `gps_daemon.c` |
| **Haversine** | 球面两点距离 (里程累计) | `gps_daemon.c` |
| **环形缓冲区** | 串口流式数据缓冲 | `ringbuffer` (common) |
| **UNIX Socket** | GPS 数据推送给 UI | `gps_daemon.c` |

## IPC 消息

| 方向 | 消息类型 | 说明 |
|------|---------|------|
| gps → UI | `MSG_GPS_DATA` (0x10) | 每秒推送定位数据 |
| UI → gps | `MSG_GPS_QUERY` (0x11) | 请求最新数据 |
| gps → UI | `MSG_GPS_STATUS` (0x13) | 定位状态 (fix/sats) |

## 配置

```ini
[gps]
uart_device = /dev/ttymxc5
baud_rate = 9600
report_interval = 1000   # 毫秒, 数据上报间隔
```

## 编译运行

```bash
make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
make test    # 单元测试 (无需硬件)

# 运行
./gps_daemon /etc/car_terminal/config.ini
```

## 注意事项

1. **权限**: 串口设备通常需要 `dialout` 组权限或 root
2. **波特率**: GPS 模块出厂默认 9600bps, 部分模块支持 115200
3. **天线**: GPS 需要开阔天空, 室内测试可能无法定位
4. **冷启动**: GPS 模块上电后首次定位 (冷启动) 可能需要 30s~2min
5. **NMEA 校验和**: 部分廉价 GPS 模块不发送校验和, 代码已做容错处理
