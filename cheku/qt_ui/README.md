# qt_ui — 智能车载终端 Qt 触控界面

## 功能概述

- **5 路 Socket 连接**: 通过 QLocalSocket 连接 5 个后台守护进程 (gps/dvr/av/can/input)
- **二进制协议**: 与 C 后端完全一致的 0xAA55 帧协议
- **5 个标签页**: 仪表盘 / DVR / 音乐 / CAN / 温湿度
- **车载触控**: 1024x600 LCD, 大按钮/大字体, 全屏模式
- **物理按键分发**: KEY1/KEY2 根据当前标签页自动切换功能
- **传感器直读**: QThread 持续读 `/dev/mydht11` (温湿度)

## 页面说明

| 页面 | 功能 | 数据来源 |
|------|------|---------|
| 仪表盘 | 速度表(QPainter手绘)/UTC时间/经纬度/卫星数/海拔 | gps |
| DVR | 摄像头预览(1fps)/录像时长显示/开始-暂停-停止/拍照→/record/ | dvr |
| 音乐 | 播放/暂停/音量调节/曲目名显示 | av |
| CAN | CAN 报文实时监控 + 手动发送 | can |
| 温湿度 | 大字体温度+湿度实时数值 | Qt SensorThread 直读 /dev/mydht11 |

## 物理按键映射

| 当前标签页 | KEY1 短按 | KEY2 短按 | LED |
|-----------|----------|----------|-----|
| DVR | 📷 拍照 (MSG_DVR_SNAPSHOT) | ⏯ 暂停/继续 (MSG_DVR_PAUSE) | 闪烁一次(150ms) |
| 音乐 | ▶ 播放 (MSG_AV_PLAY) | ⏸ 暂停 (MSG_AV_PAUSE) | 闪烁一次(150ms) |
| 其他 | 无操作 (仅状态栏提示) | 无操作 | 闪烁一次(150ms) |

## 文件结构

```
qt_ui/
├── qt_ui.pro              # qmake 项目文件
├── main.cpp               # 入口 (全屏/窗口切换)
├── mainwindow.h/.cpp      # 主窗口 (5标签页 + 5客户端 + 按键分发 + 时长显示)
├── mainwindow.ui          # Qt Designer 布局
├── protocol_handler.h/.cpp  # 二进制帧协议 (pack/unpack/CRC8)
├── daemon_client.h/.cpp     # QLocalSocket 客户端 + 自动重连
├── sensor_thread.h/.cpp     # QThread 持续读 /dev/mydht11
├── pages/
│   └── speed_gauge.h        # QPainter 手绘速度表盘
└── README.md
```

## IPC 通信架构

```
Qt UI (QLocalSocket × 5)
  ├─ /tmp/car_gps.sock    → gps_daemon
  ├─ /tmp/car_dvr.sock    → dvr_daemon
  ├─ /tmp/car_av.sock     → av_daemon
  ├─ /tmp/car_can.sock    → canbus_daemon
  └─ /tmp/car_input.sock  → input_daemon (按键事件)

协议: [0xAA][0x55][TYPE][LEN_H][LEN_L][DATA...][0x00][0x55]
      LEN 直接大端序拆分 (不用 htons/ntohs — 见调试记录)
```

## DVR 录像控制

按钮状态机:
```
待机 ─[开始录像]→ 录像中(红闪) ─[暂停]→ 已暂停(橙) ─[继续]→ 录像中
  ↑                  ↓                        ↓
  └───[停止]──────←──┘                        └──[停止]→ 待机
任何状态 ─[拍照]→ 保存 JPEG 到 /record/
```

- 时长显示: QTimer 每秒自增 (录像中), daemon 每2秒同步校准
- 录像文件: `/record/YYYYMMDD_HHMMSS_dvr.h264` (60s/段, 循环覆盖)
- 拍照文件: `/record/YYYYMMDD_HHMMSS_NNN.jpg` (自动编号)

## 编译运行

```bash
# ARM 交叉编译
qmake && make

# 窗口模式 (开发调试)
./car_ui

# 全屏模式 (开发板 LCD)
./car_ui -fullscreen -platform linuxfb
```

## 涉及的 Qt 知识点

| 知识点 | 说明 |
|--------|------|
| **QLocalSocket** | UNIX Socket 客户端 (对应 C 后端 AF_UNIX SOCK_STREAM) |
| **QTabWidget** | 多页面容器 |
| **QThread** | 独立线程持续读传感器 (SensorThread) |
| **QTimer** | 定时刷新 (预览1s / 时长1s) |
| **QPainter** | 自定义控件 (SpeedGauge 速度表盘) |
| **信号槽 (signals/slots)** | Qt 核心机制: 跨线程/对象间异步通信 |
| **reinterpret_cast** | C++ POD 结构体与 QByteArray 的零拷贝转换 |
| **Qt 样式表** | CSS-like UI 美化 |
| **linuxfb + tslib** | 嵌入式 Linux 无 X11 显示 + 触控 |
