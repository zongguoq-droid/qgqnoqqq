# av_daemon — 音频播放服务

## 功能

- WAV 播放 (44100Hz 16-bit 立体声)
- 播放列表: 扫描 `/music/` 目录，启动自动播放第一首，播完自动切下一首
- 音量控制: ALSA Mixer Speaker (0-100)
- 暂停/停止/播放: 状态机控制
- UNIX Socket: Qt UI 遥控 (PLAY/STOP/PAUSE/VOLUME)

## 实现方式

整文件读入内存 → `snd_pcm_writei` 阻塞写入 `hw:0,0`，与 `aplay -Dhw:0,0` 逻辑一致。

每写一次检查 Socket 指令，播放中可响应暂停/停止。

## 编译运行

```bash
make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
./av_daemon
```

## IPC

| 方向 | 消息 | 说明 |
|------|------|------|
| UI → av | `MSG_AV_PLAY` (0x60) | 播放/继续 |
| UI → av | `MSG_AV_STOP` (0x61) | 停止 |
| UI → av | `MSG_AV_PAUSE` (0x62) | 暂停 |
| UI → av | `MSG_AV_VOLUME` (0x63) | 音量 0-100 |

## 配置

```ini
[audio]
pcm_device = hw:0,0
default_volume = 100    # 0-100
```
