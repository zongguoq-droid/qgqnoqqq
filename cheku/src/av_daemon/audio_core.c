/**
 * @file    audio_core.c
 * @brief   ALSA 音频核心 — PCM 播放和混音器控制。
 *
 * 本模块将 alsa-lib API 封装为一组供 AV 守护进程使用的便捷函数。
 * 处理 PCM 设备打开/关闭/写入/暂停、硬件混音器音量和开关控制。
 *
 * ================================================================
 *  ALSA PCM 核心概念
 * ================================================================
 *
 * buffer_size: 内核侧环形缓冲区的总大小 (以帧为单位)。
 * period_size: 每次硬件中断之间的帧数。
 *
 * 示例: buffer=8192, period=1024 产生 8 个 period。
 * 编解码器每 1024 帧触发一次中断；应用程序应在每次中断时
 * 提供恰好一个 period 的数据以避免数据不足。
 *
 * XRUN (underrun):
 *   - underrun (播放): 因应用程序未及时提供数据导致 ALSA 缓冲区为空。
 *     DAC 会重复上一个采样或输出静音，造成可听到的杂音。
 *   - 恢复: 调用 snd_pcm_prepare() 在 XRUN 后重新准备设备。
 *
 * snd_pcm_writei 与 snd_pcm_write:
 *   - writei: 单位为交织的**帧** (推荐使用)。
 *   - write:  单位为**字节** (很少直接使用)。
 *   - 1 帧 = 声道数 × sizeof(sample)。立体声 16-bit: 4 字节/帧。
 *
 * WM8960 特定信息 (来自设备树):
 *   - CPU DAI: SAI2, 主时钟: 12.288 MHz。
 *   - 支持的采样率: 8k, 11.025k, 12k, 16k, 22.05k, 24k, 32k, 44.1k, 48k。
 *   - 本模块默认: 44.1 kHz, 16-bit signed LE, 立体声 (CD 音质)。
 */

#include "audio_core.h"
#include "log/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <alsa/asoundlib.h>  /* snd_pcm_*, snd_mixer_* */

/* ================================================================
 *  PCM Playback (PCM 播放)
 * ================================================================ */

int audio_playback_open(const char *device, int sample_rate, int channels,
                        snd_pcm_t **pcm_handle)
{
    snd_pcm_t *pcm;
    int ret;

    if (!device || !pcm_handle) return -1;

    /* snd_pcm_open(&pcm, name, stream, mode):
     *   name:   设备名 — "hw:0,0" (声卡 0, 设备 0)
     *           "default" (自动路由)
     *           "plughw:0,0" (自动格式转换)
     *   stream: SND_PCM_STREAM_PLAYBACK / SND_PCM_STREAM_CAPTURE
     *   mode:   0=阻塞 (默认), SND_PCM_NONBLOCK=非阻塞 */
    ret = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
    if (ret < 0) {
        LOG_ERROR("audio", "snd_pcm_open(%s) failed: %s",
                  device, snd_strerror(ret));
        return ret;
    }

    /* snd_pcm_set_params(pcm, format, access, channels, rate, soft_resample, latency_us):
     *   一站式配置: 格式/交错/声道/采样率/重采样/延迟
     *   内部调用 snd_pcm_set_params 会自动设置 buffer_size 和 period_size
     *
     *   format:  SND_PCM_FORMAT_S16_LE (16-bit signed, little-endian)
     *   access:  SND_PCM_ACCESS_RW_INTERLEAVED (标准交织模式)
     *   latency: 500000 us = 500ms (低延迟但不过于激进) */
    ret = snd_pcm_set_params(pcm,
                             AV_FORMAT,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             (unsigned int)channels,
                             (unsigned int)sample_rate,
                             1,        /* soft_resample: 允许软件重采样 */
                             500000);  /* latency: 500ms */
    if (ret < 0) {
        LOG_ERROR("audio", "snd_pcm_set_params failed: %s", snd_strerror(ret));
        snd_pcm_close(pcm);
        return ret;
    }

    LOG_INFO("audio", "Playback opened: %s %dHz %dch",
             device, sample_rate, channels);

    *pcm_handle = pcm;
    return 0;
}

int audio_playback_write(snd_pcm_t *pcm, const int16_t *data, int frames)
{
    int ret;

    if (!pcm || !data || frames <= 0) return -1;

    /* snd_pcm_writei(pcm, buffer, size):
     *   写入交织 PCM 帧。
     *   帧数 = size_frames, 每个帧大小 = channels × 2 bytes
     *   返回值: 实际写入的帧数 (可能 < size, 表示缓冲区满)
     *           负数 = 错误码 (如 -EPIPE = XRUN) */
    ret = snd_pcm_writei(pcm, data, (snd_pcm_uframes_t)frames);

    if (ret == -EPIPE) {
        /* XRUN (underrun): 播放缓冲区空了!
         *
         * 原因: 应用层没来得及喂数据。
         * 恢复: snd_pcm_prepare() 重置 PCM 设备。 */
        LOG_DEBUG("audio", "XRUN (underrun), recovering...");
        ret = snd_pcm_recover(pcm, ret, 0);
        if (ret < 0) {
            LOG_ERROR("audio", "snd_pcm_recover failed: %s", snd_strerror(ret));
        }
        return 0;  /* 恢复后返回0帧 */
    }

    if (ret < 0) {
        LOG_ERROR("audio", "snd_pcm_writei error: %s", snd_strerror(ret));
        return ret;
    }

    return ret;  /* 实际写入帧数 */
}

int audio_playback_pause(snd_pcm_t *pcm, int pause)
{
    int ret;
    if (!pcm) return -1;

    /* snd_pcm_pause(pcm, enable):
     *   暂停/恢复 PCM 流。
     *   并非所有硬件都支持 pause (返回 -ENOSYS),
     *   如果不支持, 用 snd_pcm_drop/snd_pcm_prepare 模拟 */
    ret = snd_pcm_pause(pcm, pause ? 1 : 0);
    if (ret == -ENOSYS) {
        /* 硬件不支持 pause: 用 drop/prepare 实现 */
        if (pause) {
            snd_pcm_drop(pcm);  /* 丢弃缓冲区 */
        } else {
            snd_pcm_prepare(pcm);  /* 重新准备 */
        }
        ret = 0;
    }
    return (ret < 0) ? ret : 0;
}

void audio_playback_close(snd_pcm_t *pcm)
{
    if (pcm) {
        snd_pcm_drop(pcm);   /* 停止播放, 丢弃缓冲区 */
        snd_pcm_close(pcm);
        LOG_INFO("audio", "Playback closed.");
    }
}

/* ================================================================
 *  Mixer Volume Control (混音器音量控制)
 * ================================================================ */

int audio_mixer_open(const char *mixer_name, snd_mixer_t **mixer)
{
    int ret;

    if (!mixer_name || !mixer) return -1;

    /* snd_mixer_open(&mixer, mode):
     *   mode: 0 = 默认 */
    ret = snd_mixer_open(mixer, 0);
    if (ret < 0) {
        LOG_ERROR("audio", "snd_mixer_open failed: %s", snd_strerror(ret));
        return ret;
    }

    /* snd_mixer_attach: 关联 Mixer 到声卡
     * snd_mixer_selem_register: 注册简单元素 (selem)
     * snd_mixer_load: 加载配置 */
    ret = snd_mixer_attach(*mixer, mixer_name);
    if (ret < 0) {
        snd_mixer_close(*mixer);
        return ret;
    }

    ret = snd_mixer_selem_register(*mixer, NULL, NULL);
    if (ret < 0) {
        snd_mixer_close(*mixer);
        return ret;
    }

    ret = snd_mixer_load(*mixer);
    if (ret < 0) {
        snd_mixer_close(*mixer);
        return ret;
    }

    return 0;
}

int audio_mixer_set_volume(snd_mixer_t *mixer, const char *selem_name,
                           int volume)
{
    snd_mixer_elem_t *elem;
    snd_mixer_selem_id_t *sid;
    long min, max, val;

    if (!mixer || !selem_name) return -1;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    /* 查找混音器元素 (如 "Speaker", "Headphone", "PCM") */
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, selem_name);

    elem = snd_mixer_find_selem(mixer, sid);
    if (!elem) {
        LOG_WARN("audio", "Mixer element '%s' not found", selem_name);
        return -1;
    }

    /* 获取音量范围 */
    snd_mixer_selem_get_playback_volume_range(elem, &min, &max);

    /* 百分比 → 原始值 (线性映射) */
    val = min + (max - min) * volume / 100;

    /* snd_mixer_selem_set_playback_volume_all(elem, val):
     *   设置所有声道的播放音量。
     *   硬件音量控制 — 这是真正的 WM8960 寄存器操作! */
    snd_mixer_selem_set_playback_volume_all(elem, val);

    LOG_INFO("audio", "Volume: %s = %d%% (raw=%ld, range=%ld-%ld)",
             selem_name, volume, val, min, max);

    return 0;
}

int audio_mixer_set_switch(snd_mixer_t *mixer, const char *selem_name, int on)
{
    snd_mixer_elem_t *elem;
    snd_mixer_selem_id_t *sid;
    if (!mixer || !selem_name) return -1;

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, selem_name);
    elem = snd_mixer_find_selem(mixer, sid);
    if (!elem) return -1;

    /* 设置 playback switch (0=off, 1=on) */
    if (snd_mixer_selem_has_playback_switch(elem))
        snd_mixer_selem_set_playback_switch_all(elem, on ? 1 : 0);

    return 0;
}

void audio_mixer_list_controls(snd_mixer_t *mixer)
{
    if (!mixer) return;
    snd_mixer_elem_t *elem;
    LOG_INFO("audio", "=== Mixer controls ===");
    for (elem = snd_mixer_first_elem(mixer); elem; elem = snd_mixer_elem_next(elem)) {
        LOG_INFO("audio", "  %s", snd_mixer_selem_get_name(elem));
    }
}

void audio_mixer_close(snd_mixer_t *mixer)
{
    if (mixer) snd_mixer_close(mixer);
}
