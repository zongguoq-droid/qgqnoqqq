# dvr_daemon — DVR 行车录像守护进程

## 功能

- **V4L2 MJPEG 采集**: `/dev/video1`, 640x480, 32 mmap buffer, ~15fps
- **MJPEG 直存**: 帧已是 JPEG 数据，录像直接 `fwrite`，拍照存 `.jpg`
- **循环录像**: 60s/段，超限自动删旧文件
- **自动拍照**: 启动时第一帧自动保存为 `/record/<ts>_001.jpg`
- **UNIX Socket**: UI 控制录像启停/暂停/拍照

## 编译运行

```bash
make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
./dvr_daemon /etc/car_terminal/config.ini
```

## 配置

```ini
[dvr]
camera_dev = /dev/video1
record_path = /record
max_size_mb = 2000
seg_duration = 60
```

## 文件结构

```
dvr_daemon/
├── dvr_daemon.h    # 配置/structs
├── dvr_daemon.c    # 主程序 (poll: v4l2 + socket)
├── v4l2_capture.h  # V4L2 采集 API
├── v4l2_capture.c  # V4L2 实现 (open→mmap→DQBUF/QBUF)
├── storage_mgr.c   # 循环存储
├── Makefile
└── README.md
```

## IPC

| 方向 | 消息 | 说明 |
|------|------|------|
| UI → dvr | `MSG_DVR_START` (0x20) | 开始/继续录像 |
| UI → dvr | `MSG_DVR_STOP` (0x21) | 停止录像 |
| UI → dvr | `MSG_DVR_SNAPSHOT` (0x23) | 拍照 |
| UI → dvr | `MSG_DVR_PAUSE` (0x26) | 暂停/继续录像 |
| dvr → UI | `MSG_DVR_STATUS` (0x22) | 状态推送 (状态/时长/分辨率/帧数) |
