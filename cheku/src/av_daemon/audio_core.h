/**
 * @file    audio_core.h
 * @brief   ALSA 音频核心: 播放 / 混音控制 - 头文件
 *
 * ALSA (Advanced Linux Sound Architecture) 是 Linux 标准音频子系统。
 * 应用层通过 alsa-lib 库操作声卡:
 *   snd_pcm_open()    打开 PCM 设备
 *   snd_pcm_set_params() 配置采样率/格式/声道数
 *   snd_pcm_writei()  写交织的 PCM 帧 (播放)
 *   snd_mixer_*()     混音器控制 (音量/静音/通路切换)
 *
 * PCM 数据格式:
 *   S16_LE: 16-bit signed, little-endian (WM8960 标准格式)
 *   交织格式 (Interleaved): L0 R0 L1 R1 L2 R2 ...
 *
 * 学习要点:
 *   1. ALSA PCM API: snd_pcm_open/set_params/writei
 *   2. 缓冲区/周期: buffer_size vs period_size, XRUN(underrun)
 *   3. ALSA Mixer: 音量控制 (snd_mixer_selem_set_playback_volume)
 *   4. 非阻塞 vs 阻塞模式
 */

#ifndef _AUDIO_CORE_H_
#define _AUDIO_CORE_H_

#include "av_daemon.h"
#include <stdint.h>
#include <alsa/asoundlib.h>    /* snd_pcm_t, snd_mixer_t */

/* 音频格式常量 */
#define AV_FORMAT            SND_PCM_FORMAT_S16_LE
#define AV_FORMAT_BITS       16
#define AV_FORMAT_BYTES      2       /* 每采样字节数 */

/**
 * @brief 打开 ALSA PCM 播放设备
 *
 * 配置: 采样率/声道/S16_LE/非交错 → snd_pcm_set_params
 *
 * @param device       PCM 设备名 (如 "hw:0,0" 或 "default")
 * @param sample_rate  采样率 (44100/48000)
 * @param channels     声道数 (1=单声道, 2=立体声)
 * @param pcm_handle   输出: snd_pcm_t 句柄
 * @return             0=成功, 负数=ALSA错误码
 */
int audio_playback_open(const char *device, int sample_rate, int channels,
                        snd_pcm_t **pcm_handle);

/**
 * @brief 写入 PCM 数据到播放设备 (阻塞)
 *
 * snd_pcm_writei(pcm, data, frames):
 *   参数1: PCM 句柄
 *   参数2: PCM 数据 (16-bit signed, 交织)
 *   参数3: 帧数 (每帧 = channels × sizeof(int16_t))
 *   返回:  实际写入的帧数 (可能少于 frames, 表示 buffer full)
 *
 * @param pcm    PCM 句柄
 * @param data   PCM 样本数据
 * @param frames 帧数
 * @return       实际写入帧数, 负数=错误
 */
int audio_playback_write(snd_pcm_t *pcm, const int16_t *data, int frames);

/**
 * @brief 暂停 / 恢复播放
 * @param pcm   PCM 句柄
 * @param pause 1=暂停, 0=恢复
 */
int audio_playback_pause(snd_pcm_t *pcm, int pause);

/**
 * @brief 关闭 PCM 播放设备
 * @param pcm   PCM 句柄
 */
void audio_playback_close(snd_pcm_t *pcm);

/**
 * @brief 打开混音器 (Mixer) 用于音量控制
 *
 * WM8960 混音器元素 (通过 alsamixer 可见):
 *   "Headphone"    — 耳机音量
 *   "Speaker"      — 喇叭音量
 *   "Playback"     — 总播放音量
 *
 * @param mixer_name  混音器名称 (通常为 "default" 或 "hw:0")
 * @param mixer       输出: 混音器句柄
 * @return            0=成功
 */
int audio_mixer_open(const char *mixer_name, snd_mixer_t **mixer);

/**
 * @brief 设置指定混音器元素的音量 (0-100)
 * @param mixer    混音器句柄
 * @param selem_name 元素名 (如 "Speaker", "Headphone")
 * @param volume   音量百分比 0-100
 */
int audio_mixer_set_volume(snd_mixer_t *mixer, const char *selem_name,
                           int volume);

/**
 * @brief 枚举所有混音器控制项并输出到日志 (用于调试)。
 *
 * 遍历每个简单混音器元素并记录其名称。用于在新硬件平台上
 * 发现可用的控制项名称。
 *
 * @param mixer  从 audio_mixer_open() 获取的混音器句柄。
 */
void audio_mixer_list_controls(snd_mixer_t *mixer);

/**
 * @brief 设置混音器元素的播放开关 (静音/取消静音)。
 *
 * 某些混音器元素 (如 "Left Output Mixer PCM") 除了音量滑块外
 * 还有一个布尔开关。此函数用于设置该开关。
 * 如果该元素没有播放开关功能，则此调用为空操作。
 *
 * @param mixer       混音器句柄。
 * @param selem_name  简单元素名称 (如 "Left Output Mixer PCM")。
 * @param on          1 = 开关打开 (取消静音), 0 = 开关关闭 (静音)。
 * @return            成功返回 0，mixer 或 selem_name 为 NULL 时返回 -1。
 */
int  audio_mixer_set_switch(snd_mixer_t *mixer, const char *selem_name, int on);

/**
 * @brief 关闭混音器句柄并释放资源。
 *
 * @param mixer  要关闭的混音器句柄 (可安全传入 NULL)。
 */
void audio_mixer_close(snd_mixer_t *mixer);

#endif /* _AUDIO_CORE_H_ */
