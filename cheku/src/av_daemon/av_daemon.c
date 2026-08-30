/**
 * @file    av_daemon.c
 * @brief   音视频守护进程 — WAV 播放与音量控制服务。
 *
 * 本守护进程作为独立后台进程运行，负责:
 *   - 扫描指定目录下的 .wav 文件，构建内存播放列表。
 *   - 解码 WAV PCM 数据并写入 ALSA 硬件设备 (hw:0,0)，
 *     使用 snd_pcm_writei() 阻塞模式。
 *   - 通过 UNIX Domain Socket 接收 Qt UI 发来的播放控制指令
 *     (播放 / 暂停 / 停止 / 下一曲)。
 *   - 通过定时轮询从共享文件 (/tmp/av_volume) 读取音量调节值，
 *     并将当前播放曲目名称写入 /tmp/av_track 供 UI 显示。
 *
 * 架构:
 *   - 单线程，基于 poll() 的事件循环。
 *   - 当前曲目的全部 PCM 数据在 open_track() 后首次调用 play_chunk()
 *     时加载到堆缓冲区 (g_pcm_data)。这避免了流式解码线程的复杂性，
 *     代价是长曲目的内存占用较高。
 *   - 播放时每次循环迭代推进 1024 帧数据块，播放时使用短 poll() 超时
 *     (10 ms)，停止或暂停时使用较长超时 (200 ms)。
 *
 * 通信通道 (Qt UI <-> 守护进程):
 *   - UNIX socket  /tmp/car_av.sock  :  播放 / 暂停 / 停止 / 曲目控制。
 *   - 文件         /tmp/av_volume    :  UI 写入音量 0–100，守护进程轮询读取。
 *   - 文件         /tmp/av_track     :  守护进程写入当前曲目文件名。
 *
 * 硬件目标:
 *   - WM8960 编解码器 (I2C2), SAI2 CPU DAI, 12.288 MHz MCLK。
 *   - ALSA 设备 "hw:0,0" 或 "default"。
 */

#include "av_daemon.h"
#include "audio_core.h"
#include "log/log.h"
#include "socket/unix_socket.h"
#include "protocol/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <dirent.h>
#include <strings.h>

/* ================================================================
 *  全局状态
 * ================================================================ */

/* ---- ALSA 句柄 ---- */
static snd_pcm_t   *g_pcm   = NULL;    /**< PCM 播放句柄 (hw:0,0) */
static snd_mixer_t *g_mixer = NULL;    /**< 混音器句柄 (用于音量控制) */

/* ---- 进程生命周期 ---- */
static volatile int g_running = 1;     /**< 主循环运行标志; 由 SIGTERM/SIGINT 清除 */

/**
 * @brief 信号处理函数 — 设置 g_running=0 并立即退出。
 *
 * 使用 _exit() 而非 exit()，避免在信号处理函数中调用 atexit 回调
 * 或刷新 stdio 缓冲区，这些操作在信号处理函数中是不安全的。
 *
 * @param s  信号编号 (未使用)。
 */
static void sig_handler(int s) { (void)s; g_running = 0; _exit(0); }

/* ---- WAV 播放列表 (内存存储, 固定大小) ---- */
#define MAX_TRACKS 64
static char  g_tracks[MAX_TRACKS][256]; /**< 已发现的 .wav 文件路径数组 */
static int   g_track_count = 0;         /**< g_tracks[] 中的有效条目数 */
static int   g_current     = -1;        /**< 当前选中曲目的索引 (-1 = 无) */

/* ---- 当前曲目状态 ---- */
static FILE *g_fp     = NULL;          /**< 当前打开的 .wav 文件的 FILE* (如果有) */
static int   g_state  = 0;             /**< 播放状态: 0=已停止, 1=播放中, 2=已暂停 */
static int   g_volume = 100;           /**< 当前音量 (0–100), 从 /tmp/av_volume 读取 */

/* ---- PCM 格式 (从当前曲目的 WAV 头部解析) ---- */
static int   g_sr;                     /**< 采样率 (Hz), 来自 WAV 头部 */
static int   g_ch;                     /**< 声道数, 来自 WAV 头部 */
static int   g_bits;                   /**< 每采样位数, 来自 WAV 头部 */

/* ---- IPC (UNIX Domain Socket) ---- */
static int   g_server_fd = -1;         /**< 监听 socket 文件描述符 */
static int   g_client_fd = -1;         /**< 已连接的 UI 客户端 fd (-1 = 无客户端) */

/* ---- PCM 缓冲区 (当前曲目的全部解码数据) ---- */
static void *g_pcm_data = NULL;        /**< 堆缓冲区，存储原始 16-bit PCM 采样数据 */
static int   g_pcm_total = 0;          /**< g_pcm_data 中的总帧数 */
static int   g_pcm_pos   = 0;          /**< 下一个要写入 ALSA 的帧索引 */

/* ---- 协议解码 ---- */
static protocol_parser_t g_pp;         /**< Socket 消息的帧级协议解析器 */
static ringbuffer_t g_rb;              /**< 环形缓冲区，向解析器提供原始字节 */

/* ---- 音乐目录扫描 ---- */

/**
 * @brief 扫描目录中的 .wav 文件并填充全局播放列表。
 *
 * 打开指定目录，枚举所有条目，将扩展名为 ".wav" (不区分大小写) 的每个文件
 * 的绝对路径存储到静态 g_tracks[][] 数组中。隐藏文件 (以 '.' 开头的名称)
 * 会被跳过。当收集到 MAX_TRACKS 个条目或目录扫描完毕时停止。
 *
 * 该函数用找到的曲目数量更新全局 g_track_count。如果目录无法打开，
 * g_track_count 设为 0 且函数返回 0。
 *
 * @param dir  音乐目录的绝对或相对路径 (例如 "/music/")。
 * @return     找到的 .wav 文件数量 (目录为空或无法打开时返回 0)。
 */
static int scan_music(const char *dir) {
    DIR *d = opendir(dir); if (!d) return 0;
    int cnt = 0; struct dirent *e;
    while ((e=readdir(d)) && cnt<MAX_TRACKS) {
        if (e->d_name[0]=='.') continue;
        const char *dot=strrchr(e->d_name,'.');
        if (dot && strcasecmp(dot,".wav")==0)
            snprintf(g_tracks[cnt++],256,"%s/%s",dir,e->d_name);
    }
    closedir(d); g_track_count=cnt;
    LOG_INFO("av","Found %d tracks",cnt); return cnt;
}

/* ---- 曲目管理 ---- */

/**
 * @brief 按播放列表索引打开曲目，解析其 WAV 头部，并通知 UI。
 *
 * 执行步骤:
 *   1. 关闭之前打开的曲目 (fclose)。
 *   2. 以二进制读模式打开 g_tracks[idx] 处的 .wav 文件。
 *   3. 读取并验证 44 字节的 RIFF/WAVE 头部:
 *      - 检查偏移 0 处的 "RIFF" 魔数。
 *      - 检查偏移 8 处的 "WAVE" 魔数。
 *   4. 从 fmt chunk 提取音频参数:
 *      - 声道数    (偏移 22, uint16 LE)。
 *      - 采样率    (偏移 24, uint32 LE)。
 *      - 每采样位数 (偏移 34, uint16 LE)。
 *   5. 通过 audio_playback_open() 打开或复用 ALSA PCM 设备。
 *   6. 将曲目的基本文件名 (不含路径) 写入 /tmp/av_track，
 *      以便 UI 显示当前播放的曲目名称。
 *
 * @param idx  播放列表索引 (从0开始)。必须在 [0, g_track_count) 范围内。
 * @return     成功返回 0，索引越界、文件无法打开或 WAV 头部无效时返回 -1。
 */
static int open_track(int idx) {
    if (idx<0||idx>=g_track_count) return -1;
    if (g_fp) { fclose(g_fp); g_fp=NULL; }
    g_fp=fopen(g_tracks[idx],"rb");
    if (!g_fp) return -1;
    unsigned char h[44];
    if (fread(h,1,44,g_fp)!=44||memcmp(h,"RIFF",4)||memcmp(h+8,"WAVE",4))
        { fclose(g_fp); g_fp=NULL; return -1; }
    g_ch=h[22]|(h[23]<<8); g_sr=h[24]|(h[25]<<8)|(h[26]<<16)|(h[27]<<24);
    g_bits=h[34]|(h[35]<<8); fseek(g_fp,44,SEEK_SET);
    if (!g_pcm) audio_playback_open("hw:0,0",g_sr,g_ch,&g_pcm);

    /* 将曲目名称写入共享文件供 UI 读取 */
    { const char *fn=strrchr(g_tracks[idx],'/');
      const char *name=fn?fn+1:g_tracks[idx];
      FILE *tf=fopen("/tmp/av_track","w");
      if (tf) { fprintf(tf,"%s",name); fclose(tf); }
      LOG_INFO("av","Track %d: %s",idx,name); }
    return 0;
}

/* ---- 播放控制 ---- */

/**
 * @brief 停止播放并释放当前曲目关联的所有资源。
 *
 * 释放内存中的 PCM 缓冲区 (g_pcm_data)，关闭文件句柄 (g_fp)，
 * 并调用 snd_pcm_drop() 丢弃 ALSA 硬件缓冲区中的剩余数据。
 * 即使没有加载任何曲目，此函数也可以安全调用 (所有指针在释放/关闭前
 * 都会检查是否为 NULL)。
 *
 * 调用此函数后，如果 g_state 还未设为 0 (AV_PLAY_STOPPED)，
 * 调用者应将其设置为 0。
 */
static void play_stop(void) {
    free(g_pcm_data); g_pcm_data=NULL; g_pcm_pos=0;
    if (g_fp) { fclose(g_fp); g_fp=NULL; }
    if (g_pcm) snd_pcm_drop(g_pcm);
}

/* ---- 基于数据块的 PCM 播放 ---- */

/**
 * @brief 向 ALSA 设备写入一个数据块 (最多 1024 帧) 的 PCM 数据。
 *
 * 这是核心播放函数，当 g_state == AV_PLAY_PLAYING (1) 时，
 * 主事件循环的每次迭代都会调用一次。
 *
 * 首次调用新打开的曲目时 (g_pcm_data == NULL):
 *   - 将整个 PCM 载荷从 .wav 文件 (从字节偏移 44 到 EOF)
 *     读入堆分配的缓冲区 (g_pcm_data)。
 *   - 读取完成后立即关闭文件句柄 — 曲目播放期间不再需要磁盘 I/O。
 *   - g_pcm_total 设置为交织 PCM 帧的总数。
 *
 * 后续调用时:
 *   - 通过 snd_pcm_writei() 向 ALSA 写入最多 1024 帧。
 *   - 发生 XRUN (underrun, -EPIPE) 时，调用 snd_pcm_prepare() 恢复。
 *   - 整首曲目播放完毕后 (chunk <= 0)，自动切换到播放列表中的
 *     下一曲目 (循环)。如果 open_track() 打开下一曲目失败，
 *     则停止播放 (g_state = 0)。
 *
 * 当 g_pcm 为 NULL 或 g_state 不为 1 (播放中) 时，此函数为空操作。
 */
static void play_chunk(void) {
    if (!g_pcm||g_state!=1) return;
    if (!g_pcm_data) {
        if (!g_fp) { g_state=0; return; }
        fseek(g_fp,0,SEEK_END); long sz=ftell(g_fp); fseek(g_fp,44,SEEK_SET);
        int ds=(int)(sz-44); if(ds<=0){g_state=0;return;}
        g_pcm_data=malloc((size_t)ds);
        if(!g_pcm_data){g_state=0;return;}
        fread(g_pcm_data,1,(size_t)ds,g_fp); fclose(g_fp); g_fp=NULL;
        g_pcm_total=ds/(g_ch*g_bits/8); g_pcm_pos=0;
    }
    int16_t *pcm=(int16_t*)g_pcm_data;
    int chunk=g_pcm_total-g_pcm_pos;
    if (chunk<=0) { /* 曲目播放完毕 → 切换到下一曲 (循环) */
        free(g_pcm_data); g_pcm_data=NULL;
        g_current=(g_current+1)%g_track_count;
        if (open_track(g_current)!=0) g_state=0;
        return;
    }
    if (chunk>1024) chunk=1024;
    int ret=snd_pcm_writei(g_pcm,pcm+g_pcm_pos*g_ch,chunk);
    if (ret==-EPIPE) { snd_pcm_prepare(g_pcm); return; }
    if (ret>0) g_pcm_pos+=ret;
}

/* ---- Socket 指令处理 ---- */

/**
 * @brief 接受新的 UI 连接并处理传入的协议消息。
 *
 * 此函数在一次调用中处理两类事务:
 *
 *   1. **连接管理:** 在监听服务端 socket 上调用 sock_accept()。
 *      如果新客户端到来时已有连接，则新 fd 被拒绝 (立即关闭)，
 *      强制执行单客户端策略。否则将 fd 存入 g_client_fd 并重置协议解析器。
 *
 *   2. **消息分发:** 通过 sock_recv() 从已连接的客户端读取原始字节，
 *      喂入环形缓冲区，使用 protocol_unpack() 解码完整帧。
 *      每条解码消息触发状态转换:
 *        - MSG_AV_PLAY:  暂停时恢复播放，从未播放过则启动第一首，
 *                        停止后则重新打开当前曲目。
 *        - MSG_AV_STOP:  停止播放并释放资源。
 *        - MSG_AV_PAUSE: 在播放 (1) 和暂停 (2) 之间切换。
 *
 * 客户端断开连接时 (recv 返回 < 0)，关闭 fd 并将 g_client_fd
 * 重置为 -1，以便下次迭代时新客户端可以连接。
 */
static void handle_sockets(void) {
    int fd=sock_accept(g_server_fd);
    if (fd>=0) {
        if (g_client_fd>=0) sock_close(fd,NULL);
        else { g_client_fd=fd; protocol_reset(&g_pp,&g_rb); LOG_INFO("av","UI connected"); }
    }
    if (g_client_fd<0) return;
    uint8_t buf[512]; int n=sock_recv(g_client_fd,buf,sizeof(buf));
    if (n<0) { LOG_INFO("av","UI disconnected"); sock_close(g_client_fd,NULL); g_client_fd=-1; return; }
    if (n>0) {
        ringbuffer_write(&g_rb,buf,(uint32_t)n);
        uint8_t t,d[256]; uint32_t l;
        while (protocol_unpack(&g_pp,&g_rb,&t,d,sizeof(d),&l)==0) {
            switch (t) {
            case MSG_AV_PLAY:
                if (g_state==2) g_state=1;
                else if (g_current<0&&g_track_count>0) { g_current=0; open_track(0); g_state=1; }
                else if (g_current>=0) { open_track(g_current); g_state=1; }
                break;
            case MSG_AV_STOP: g_state=0; play_stop(); break;
            case MSG_AV_PAUSE: g_state=(g_state==1)?2:(g_state==2)?1:0; break;
            }
        }
    }
}

/**
 * @brief AV 守护进程入口点。
 *
 * 初始化序列:
 *   1. 打开滚动日志文件 (最大 1 MiB, 3 个轮转副本)。
 *   2. 打开 ALSA 混音器 ("default") 并将 Speaker/Headphone 音量设为 100%，
 *      同时启用输出混音开关。
 *   3. 在 AV_SOCKET_PATH 创建 UNIX Domain 监听 socket。
 *   4. 初始化线协议解析器和环形缓冲区。
 *   5. 注册 SIGTERM 和 SIGINT 信号处理函数。
 *   6. 扫描 AV_MUSIC_PATH 中的 .wav 文件; 如果有则自动开始播放第一首。
 *
 * 主循环 (运行直到 g_running == 0):
 *   - 对服务端 socket 和客户端 socket (如果已连接) 进行 poll()。
 *     播放中超时 10 ms (频繁向 ALSA 喂数据)，停止或暂停中超时 200 ms
 *     (节省 CPU)。
 *   - 每 2 秒: 读取 /tmp/av_volume 并将音量变化应用到硬件混音器。
 *     ALSA 原始音量映射为:
 *       raw = 0 (当 volume == 0 时), 否则 raw = 40 + volume * 87 / 100。
 *     这为 WM8960 提供了可用的类对数范围。
 *   - handle_sockets(): 接受新连接并分发指令。
 *   - play_chunk(): 如果正在播放，向 ALSA 喂入下 1024 帧 PCM 数据。
 *
 * @param argc  参数个数 (未使用 — 配置通过宏硬编码)。
 * @param argv  参数数组 (未使用)。
 * @return      正常关闭时返回 0 (SIGTERM/SIGINT), socket 创建失败返回 1。
 */
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("[av] av_daemon starting...\n");
    log_init(AV_LOG_PATH,LOG_INFO,1024*1024,3);
    LOG_INFO("av","===== av_daemon starting =====");
    if (audio_mixer_open("default",&g_mixer)==0) {
        audio_mixer_set_volume(g_mixer,"Speaker",100);
        audio_mixer_set_volume(g_mixer,"Headphone",100);
        audio_mixer_set_switch(g_mixer,"Left Output Mixer PCM",1);
        audio_mixer_set_switch(g_mixer,"Right Output Mixer PCM",1);
    }
    g_server_fd=sock_create_server(AV_SOCKET_PATH);
    if (g_server_fd<0) return 1;
    protocol_parser_init(&g_pp); ringbuffer_init(&g_rb,4096);
    signal(SIGTERM,sig_handler); signal(SIGINT,sig_handler);
    scan_music(AV_MUSIC_PATH);
    if (g_track_count>0) { g_current=0; if (open_track(0)==0) g_state=1; }
    LOG_INFO("av","Loop starting...");

    while (g_running) {
        struct pollfd pfds[2]; int nfds=0;
        pfds[nfds].fd=g_server_fd; pfds[nfds].events=POLLIN; nfds++;
        if (g_client_fd>=0) { pfds[nfds].fd=g_client_fd; pfds[nfds].events=POLLIN; nfds++; }
        poll(pfds,nfds,g_state==1?10:200);

        /* 音量轮询: 每 2 秒读取 /tmp/av_volume。
         * ALSA 原始音量映射:
         *   volume == 0  →  raw = 0       (静音)
         *   volume >  0  →  raw = 40 + volume * 87 / 100
         * 这将 1–100 映射到约 41–127，为 WM8960 编解码器的内部增益级
         * 提供了可用的范围。 */
        { static time_t t0=0; if(time(NULL)-t0>=2){t0=time(NULL);
          FILE *f=fopen("/tmp/av_volume","r"); if(f){int v;
          if(fscanf(f,"%d",&v)==1&&v!=g_volume&&v>=0&&v<=100){g_volume=v;
          if(g_mixer){int r=v==0?0:40+v*87/100;
          audio_mixer_set_volume(g_mixer,"Speaker",r);
          audio_mixer_set_volume(g_mixer,"Headphone",r);}}
          fclose(f);}}}

        handle_sockets();
        if (g_state==1) play_chunk();
    }
    return 0;
}
