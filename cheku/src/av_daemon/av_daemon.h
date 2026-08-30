/**
 * @file    av_daemon.h
 * @brief   音频播放服务守护进程 - 配置 & 数据结构
 *
 * 硬件平台 (从设备树):
 *   WM8960 codec on I2C2 (addr 0x1a), SAI2 CPU DAI
 *   12.288MHz MCLK, 支持 44.1kHz/48kHz 采样率
 *   输出: 喇叭 (SPK_L/R) + 耳机 (HP_L/R)
 *
 * ALSA 设备名: "hw:0,0" 或 "default" (通过配置文件指定)
 *
 * 播放方式:
 *   整文件读入内存 → snd_pcm_writei 阻塞写入 (和 aplay 一致)
 *   播放中 poll(...,0) 非阻塞检查 Socket 指令
 *
 * 仅保留音乐播放功能, 录音功能已删除。
 * 解码线程 / 环形缓冲区 / 播放列表等旧架构已删除,
 * 实际实现见 av_daemon.c (简化版: 全局变量 + 整文件读内存)。
 */

#ifndef _AV_DAEMON_H_
#define _AV_DAEMON_H_

#include "common.h"     /* 消息类型, 通用类型 */
#include "protocol/protocol.h"   /* ringbuffer_t, protocol_parser_t */
#include <stdint.h>

/* ================================================================
 *  默认配置
 * ================================================================ */
#define AV_DEFAULT_PCM_DEVICE       "hw:0,0"    /* wm8960audio HiFi */
#define AV_DEFAULT_SAMPLE_RATE      44100       /* 采样率 44.1kHz */
#define AV_DEFAULT_CHANNELS         2           /* 立体声 */
#define AV_DEFAULT_VOLUME           70          /* 默认音量 70% */

#define AV_MUSIC_PATH               "/music/"               /* 音乐目录 */

/* ---- Socket ---- */
#define AV_SOCKET_PATH              "/tmp/car_av.sock"
#define AV_LOG_PATH                 "/var/log/car_terminal/av.log"

#endif /* _AV_DAEMON_H_ */
