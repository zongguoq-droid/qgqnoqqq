# canbus_daemon — CAN 总线通信守护进程

## 功能概述

通过 Linux SocketCAN 接口收发 CAN 总线报文：

- **CAN 接口管理**: 绑定 can0 (FlexCAN1), 500kbps
- **报文收发**: 标准帧 (11-bit) / 扩展帧 (29-bit) / 远程帧
- **环形缓冲**: 缓存最近 200 条 CAN 报文 (RX+TX)
- **错误监控**: 检测 TEC/REC 和总线状态 (主动/被动/关闭)
- **UNIX Socket**: 推送给 UI, 接收 UI 发送指令
- **日志记录**: 收发统计和总线状态记录

## 硬件平台

从设备树 `100ask_imx6ull-14x14.dts`:

```dts
&flexcan1 {
    pinctrl-0 = <&pinctrl_flexcan1>;
    xceiver-supply = <&reg_can_3v3>;  /* 3.3V 供电 */
    status = "okay";
};

pinctrl_flexcan1: flexcan1grp {
    fsl,pins = <
        MX6UL_PAD_UART3_CTS_B__FLEXCAN1_TX  0x000010B0
        MX6UL_PAD_UART3_RTS_B__FLEXCAN1_RX  0x000010B0
    >;
};
```

CAN 收发器: 3.3V, 连接到 CAN_H / CAN_L 总线。

## 文件结构

```
canbus_daemon/
├── canbus_daemon.h  # 配置宏、数据结构 (can_ring_t, can_error_info_t)
├── canbus_daemon.c  # 主程序 (epoll + socket + ring buffer)
├── can_driver.h     # SocketCAN 驱动封装头文件
├── can_driver.c     # SocketCAN 实现 (socket/bind/recv/send/error)
├── Makefile
├── test/test_can.c  # 5 组测试 (环形缓冲/ID标志/配置)
└── README.md
```

## SocketCAN 架构

```
应用层:  socket(PF_CAN, SOCK_RAW, CAN_RAW)
              ↓ bind(can0)
协议层:  CAN RAW / CAN BCM (net/can/)
              ↓
驱动层:  flexcan.ko (drivers/net/can/flexcan.c)
              ↓
硬件层:  i.MX6ULL FlexCAN 控制器 → 收发器 → CAN_H/CAN_L
```

## 编译运行

```bash
make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
make test    # 单元测试 (无需 CAN 硬件)

# 运行前配置 CAN 接口
ip link set can0 type can bitrate 500000 restart-ms 100
ip link set can0 up

# 运行进程
./canbus_daemon /etc/car_terminal/config.ini
```

## 涉及的 Linux 知识点

| 知识点 | 说明 | 对应代码 |
|--------|------|---------|
| **SocketCAN / PF_CAN** | Linux CAN 协议族 socket | `can_driver.c` |
| **struct can_frame** | CAN 帧内核表示 (ID/DLC/data) | `can_driver.c` |
| **CAN ID 标志位** | EFF_FLAG / RTR_FLAG / ERR_FLAG | `can_driver.c` |
| **CAN 错误帧** | TEC/REC 计数器, 总线状态 | `can_driver.c` |
| **ioctl SIOCGIFINDEX** | 网口名 → 内核接口索引 | `can_driver.c` |
| **epoll** | can fd + socket + timerfd 统一监听 | `canbus_daemon.c` |
| **环形缓冲区** | 最近 200 条报文高速缓存 | `canbus_daemon.c` |

## IPC 消息

| 方向 | 消息类型 | 说明 |
|------|---------|------|
| can → UI | `MSG_CAN_DATA` (0x50) | CAN 报文实时推送 |
| UI → can | `MSG_CAN_SEND` (0x51) | 发送 CAN 报文 |
| UI → can | `MSG_CAN_FILTER` (0x52) | 设置过滤规则 (预留) |
| can → UI | `MSG_CAN_STATUS` (0x53) | 总线状态/统计 |

## 配置

```ini
[canbus]
interface = can0
bitrate = 500000
```

## 注意事项

1. **内核模块**: 确保加载了 `can`, `can_raw`, `flexcan` 模块
2. **接口配置**: CAN 接口需要用 `ip link` 命令预先配置并 up
3. **总线终端**: CAN 总线两端需要 120Ω 终端电阻
4. **权限**: CAN socket 操作通常需要 root 或 `CAP_NET_ADMIN`
5. **波特率**: 标准汽车 CAN 为 500kbps, 工业 CAN 常用 250kbps
6. **restart-ms**: 建议设置 `restart-ms 100` 以自动从 BUS_OFF 恢复
