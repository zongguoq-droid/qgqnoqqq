/**
 * @file    dvr_daemon.c
 * @brief   DVR行车记录仪录制守护进程 — 主循环与命令处理(poll模式)
 *
 * 核心职责:
 *   - V4L2 MJPEG采集: 通过poll()实现非阻塞、事件驱动的帧出队
 *   - 分段录制: MJPEG帧追加到带时间戳的.h264分段文件中
 *   - 录制控制: 通过Unix域套接字实现开始/停止/暂停/恢复
 *   - 拍照: 将下一个可用的V4L2帧保存sizeof为JPEG到/record/
 *   - 预览: 每15帧(~1fps)写入/tmp/dvr_preview.jpg供Qt界面显示
 *   - 状态推送: 每隔2秒向界面发送二进制状态包(状态、已录制秒数、分辨率、帧计数)
 *
 * 架构(poll事件循环):
 *   poll([v4l2_fd, server_fd, client_fd], 1000ms超时)
 *   |
 *   +-- v4l2_fd POLLIN  -->  DQBUF → MJPEG帧
 *   |     |-- 首帧: 自动保存拍照(摄像头验证)
 *   |     |-- 每15帧: 更新/tmp/dvr_preview.jpg
 *   |     |-- 每150帧: 记录状态日志(帧计数、已录制时长)
 *   |     |-- 如果正在录制且未暂停: fwrite写入当前分段
 *   |     |     |-- 分段时长超限? → 打开下一个分段
 *   |     |-- 如果有待处理拍照: 调用save_snapshot()并清除标志
 *   |     +-- QBUF (将缓冲区归还内核队列)
 *   |
 *   +-- server_fd POLLIN -->  accept() 新的界面客户端(同时仅一个)
 *   |
 *   +-- client_fd POLLIN -->  recv() → ringbuffer → protocol_unpack()
 *         |-- MSG_DVR_START:   开始或恢复录制
 *         |-- MSG_DVR_STOP:    停止录制, 关闭分段
 *         |-- MSG_DVR_PAUSE:   切换暂停(暂停时累计已录制时长,
 *         |                    刷新缓冲区, 以新的开始时间恢复)
 *         |-- MSG_DVR_SNAPSHOT: 设置 pending_snapshot = 1
 *         +-- (未知):           忽略
 *   |
 *   +-- 超时 (ret == 0): 约每30个poll周期执行一次定期存储清理
 *   +-- 每2秒: 如果界面已连接, 调用send_dvr_status()
 *
 * 关键设计决策:
 *   - 单一全局上下文 (g_ctx) — 守护进程, 非库
 *   - 暂停/恢复在rec_elapsed_sec中追踪已录制秒数; 恢复时重置rec_start_time,
 *     确保时长计算始终是累加的
 *   - 拍照是异步的: 界面命令设置标志, 下一帧处理器保存图像
 *     (避免泄漏帧缓冲区或阻塞套接字处理器)
 */

#include "dvr_daemon.h"
#include "log/log.h"
#include "config/config.h"
#include "socket/unix_socket.h"
#include "protocol/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/stat.h>

static dvr_context_t g_ctx;
static volatile int g_running = 1;
static void sig_handler(int s) { (void)s; g_running = 0; }

/**
 * @brief 从INI文件加载守护进程配置, 附带编译期默认值。
 *
 * 读取 [dvr] 节的键值: camera_dev, record_path, max_size_mb, seg_duration。
 * 如果配置文件缺失或不可读, 静默回退到默认值
 * (DVR_DEFAULT_DEVICE, DVR_DEFAULT_STORAGE 等)。
 *
 * @param ctx       需要填充的守护进程上下文 (storage + camera_dev 字段)
 * @param conf_file config.ini 的路径 (可为 NULL)
 * @return          成功时返回 RET_OK
 */
/* ---- 配置 ---- */
int dvr_config_load(dvr_context_t *ctx, const char *conf_file)
{
    config_t conf; int loaded = 0;
    if (conf_file && config_load(&conf, conf_file) == 0) loaded = 1;

    if (loaded) {
        config_get_str(&conf, "dvr", "camera_dev", DVR_DEFAULT_DEVICE,
                       ctx->camera_dev, sizeof(ctx->camera_dev));
        config_get_str(&conf, "dvr", "record_path", DVR_DEFAULT_STORAGE,
                       ctx->storage.base_path, sizeof(ctx->storage.base_path));
    } else {
        strncpy(ctx->camera_dev, DVR_DEFAULT_DEVICE, sizeof(ctx->camera_dev)-1);
        strncpy(ctx->storage.base_path, DVR_DEFAULT_STORAGE, sizeof(ctx->storage.base_path)-1);
    }
    ctx->storage.max_size_mb = loaded ?
        config_get_int(&conf, "dvr", "max_size_mb", DVR_DEFAULT_STORAGE_MB) : DVR_DEFAULT_STORAGE_MB;
    ctx->storage.segment_sec = loaded ?
        config_get_int(&conf, "dvr", "seg_duration", DVR_DEFAULT_SEGMENT_SEC) : DVR_DEFAULT_SEGMENT_SEC;

    if (loaded) config_unload(&conf);

    LOG_INFO("dvr","Config: camera=%s record=%s max=%dMB seg=%ds",
             ctx->camera_dev, ctx->storage.base_path, ctx->storage.max_size_mb, ctx->storage.segment_sec);
    return RET_OK;
}

/**
 * @brief 将MJPEG帧直接保存为.jpg拍照文件到录制目录。
 *
 * MJPEG帧本身就是有效的JPEG数据, 无需编码 — 直接fwrite即可生成
 * 可查看的JPEG文件。文件名格式为 YYYYMMDD_HHMMSS_NNN.jpg,
 * 其中NNN是会话范围内的snap_count(防止同一秒内文件名冲突)。
 *
 * 副作用:
 *   - 自增 ctx->snap_count (用于文件名)
 *   - 如果目录不存在则创建录制目录 (mkdir -p 风格)
 *   - 向界面发送包含拍照文件名的二进制状态包
 *
 * @param ctx  守护进程上下文 (读取base_path和帧数据; 更新snap_count)
 * @param data 指向原始MJPEG/JPEG帧数据的指针
 * @param size 帧数据的字节大小
 */
/* ---- 拍照: 将MJPEG帧直接保存为.jpg到/record/ ---- */
static void save_snapshot(dvr_context_t *ctx, void *data, int size)
{
    /* 确保目录存在 */
    const char *dir = ctx->storage.base_path[0] ? ctx->storage.base_path : "/record/";
    mkdir(dir, 0755);

    char path[320];
    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    char ts[32]; strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm);

    ctx->snap_count++;
    snprintf(path, sizeof(path), "%s/%s_%03u.jpg", dir, ts, ctx->snap_count);

    FILE *fp = fopen(path, "wb");
    if (fp) {
        fwrite(data, 1, (size_t)size, fp);
        fclose(fp);
        LOG_INFO("dvr","Snapshot saved: %s (%d bytes)", path, size);
    } else {
        LOG_ERROR("dvr","Cannot save snapshot: %s", path);
    }

    /* 通知 UI 拍照完成 */
    if (ctx->client_fd >= 0) {
        uint8_t status[16];
        memset(status, 0, sizeof(status));
        status[0] = 0;  /* state = idle */
        status[1] = (uint8_t)(ctx->snap_count & 0xFF);
        /* 写入文件名 (截断) */
        const char *fn = strrchr(path, '/');
        fn = fn ? fn + 1 : path;
        int name_len = (int)strlen(fn);
        if (name_len > 12) name_len = 12;
        memcpy(status + 2, fn, (size_t)name_len);
        sock_send_frame(ctx->client_fd, MSG_DVR_STATUS,
                       status, sizeof(status));
    }
}

/**
 * @brief 向已连接的界面客户端发送二进制DVR状态包。
 *
 * 二进制传输格式 (16字节, 小端序, MSG_DVR_STATUS 消息类型):
 *   byte[0]      — 录制状态: 0=空闲, 1=录制中, 2=已暂停
 *   byte[1..4]   — 已录制秒数 (uint32 小端序; 不含暂停时间)
 *   byte[5..6]   — 帧宽度  (uint16 小端序)
 *   byte[7..8]   — 帧高度 (uint16 小端序)
 *   byte[9..12]  — 生命周期帧计数器 (uint32 小端序)
 *   byte[13..15] — 保留 (填零)
 *
 * 调用时机: 主循环中每2秒, 客户端连接时, 以及
 * START/STOP/PAUSE 命令之后, 确保界面始终显示当前状态。
 *
 * 时长计算:
 *   - 如果正在录制: elapsed = (now - rec_start_time) + rec_elapsed_sec
 *     这正确计算了暂停/恢复周期之前已录制的时长。
 *   - 如果已暂停: elapsed = rec_elapsed_sec (冻结; 不含当前暂停时长)
 *   - 如果已停止: elapsed = 0
 *
 * @param ctx  守护进程上下文 (读取状态字段; 无客户端时无副作用)
 */
/* ---- 向界面发送DVR状态 (状态 + 已录制时长) ---- */
static void send_dvr_status(dvr_context_t *ctx)
{
    if (ctx->client_fd < 0) return;

    /* 计算录制时长 */
    uint32_t elapsed = 0;
    if (ctx->recording && !ctx->paused) {
        elapsed = (uint32_t)(time(NULL) - ctx->rec_start_time) + ctx->rec_elapsed_sec;
    } else if (ctx->recording && ctx->paused) {
        elapsed = ctx->rec_elapsed_sec;
    }

    uint8_t status[16];
    memset(status, 0, sizeof(status));
    /* status[0]: 录像状态  0=idle, 1=recording, 2=paused */
    if (!ctx->recording)       status[0] = 0;
    else if (ctx->paused)      status[0] = 2;
    else                       status[0] = 1;

    /* status[1-4]: 录制时长 (秒, little-endian) */
    status[1] = (uint8_t)(elapsed & 0xFF);
    status[2] = (uint8_t)((elapsed >> 8) & 0xFF);
    status[3] = (uint8_t)((elapsed >> 16) & 0xFF);
    status[4] = (uint8_t)((elapsed >> 24) & 0xFF);

    /* status[5-6]: 分辨率 */
    status[5] = (uint8_t)(ctx->v4l2.width & 0xFF);
    status[6] = (uint8_t)((ctx->v4l2.width >> 8) & 0xFF);
    status[7] = (uint8_t)(ctx->v4l2.height & 0xFF);
    status[8] = (uint8_t)((ctx->v4l2.height >> 8) & 0xFF);

    /* status[9-12]: 帧计数 */
    status[9]  = (uint8_t)(ctx->frame_total & 0xFF);
    status[10] = (uint8_t)((ctx->frame_total >> 8) & 0xFF);
    status[11] = (uint8_t)((ctx->frame_total >> 16) & 0xFF);
    status[12] = (uint8_t)((ctx->frame_total >> 24) & 0xFF);

    sock_send_frame(ctx->client_fd, MSG_DVR_STATUS, status, sizeof(status));
}

/**
 * @brief 处理服务器和客户端套接字的poll事件。
 *
 * 接受路径 (server_ev & POLLIN):
 *   - 接受一个界面连接。如果已有界面连接, 新的连接将被静默拒绝
 *     (单客户端模式)。
 *   - 连接时立即推送当前DVR状态, 使界面无需等待下一个2秒周期
 *     即可显示正确状态。
 *
 * 命令路径 (client_ev & POLLIN/POLLHUP/POLLERR):
 *   将原始字节读入接收ringbuffer, 然后循环调用protocol_unpack()
 *   提取分帧消息。分发逻辑:
 *
 *   MSG_DVR_START:
 *     - 如果未在录制: 设置 recording=1, paused=0, rec_start_time=now,
 *       rec_elapsed_sec=0, 并打开第一个分段文件。
 *       错误处理修复: 如果 dvr_storage_open_segment() 失败,
 *       立即将recording设回0, 避免守护进程向NULL文件指针写入。
 *     - 如果正在录制且已暂停: 恢复 — 清除paused, 重置rec_start_time,
 *       保留已累计的rec_elapsed_sec (当前分段保持打开)。
 *
 *   MSG_DVR_STOP:
 *     - 清除recording和paused标志, 关闭当前分段文件, 清零rec_elapsed_sec。
 *
 *   MSG_DVR_PAUSE:
 *     - 暂停: 保存自rec_start_time以来的已录制时长到rec_elapsed_sec,
 *       设置paused=1, fflush分段文件使缓冲数据落盘。
 *     - 恢复(已在暂停): 清除paused, 重置rec_start_time。
 *     - 未在录制时无操作。
 *
 *   MSG_DVR_SNAPSHOT:
 *     - 设置 pending_snapshot=1。实际保存在下一次帧处理器迭代中完成
 *       (异步操作, 避免阻塞套接字处理器)。
 *
 *   断连处理: POLLHUP或POLLERR, 或recv()返回<=0时,
 *   关闭客户端套接字并将client_fd设为-1。
 *
 * @param ctx       守护进程上下文
 * @param server_ev 监听套接字的poll返回事件
 * @param client_ev 已连接客户端套接字的poll返回事件
 */
/* ---- 套接字事件处理 (接受连接 + 命令分发) ---- */
static void handle_sockets(dvr_context_t *ctx, int server_ev, int client_ev)
{
    /* 接受连接 */
    if (server_ev & POLLIN) {
        int fd = sock_accept(ctx->server_fd);
        if (fd >= 0) {
            if (ctx->client_fd >= 0) { sock_close(fd, NULL); }
            else {
                ctx->client_fd = fd;
                protocol_reset(&ctx->proto_parser, &ctx->recv_rb);
                LOG_INFO("dvr","UI connected: fd=%d", fd);
                /* 连接后立即发送当前状态 */
                send_dvr_status(ctx);
            }
        }
    }
    /* recv 指令 */
    if (ctx->client_fd < 0) return;
    if (client_ev & (POLLIN|POLLHUP|POLLERR)) {
        uint8_t buf[512];
        int n = sock_recv(ctx->client_fd, buf, sizeof(buf));
        if (n <= 0) {
            LOG_INFO("dvr","UI disconnected");
            sock_close(ctx->client_fd, NULL);
            ctx->client_fd = -1;
            return;
        }
        ringbuffer_write(&ctx->recv_rb, buf, (uint32_t)n);
        uint8_t t, d[256]; uint32_t l;
        while (protocol_unpack(&ctx->proto_parser, &ctx->recv_rb, &t, d, sizeof(d), &l) == 0) {
            switch (t) {
            case MSG_DVR_START:
                /* 开始录像 */
                if (!ctx->recording) {
                    ctx->recording = 1;
                    ctx->paused = 0;
                    ctx->rec_start_time = time(NULL);
                    ctx->rec_elapsed_sec = 0;
                    int ret = dvr_storage_open_segment(&ctx->storage, ctx->rec_start_time);
                    if (ret != 0) {
                        LOG_ERROR("dvr","Failed to create segment file in %s — check directory permissions",
                                  ctx->storage.base_path);
                        ctx->recording = 0;  /* 回退: 录像失败 */
                    } else {
                        LOG_INFO("dvr","Recording STARTED → %s", ctx->storage.base_path);
                    }
                } else if (ctx->paused) {
                    /* 已暂停 → 继续录像 */
                    ctx->paused = 0;
                    ctx->rec_start_time = time(NULL);
                    LOG_INFO("dvr","Recording RESUMED (elapsed=%us)",
                             ctx->rec_elapsed_sec);
                }
                send_dvr_status(ctx);
                break;

            case MSG_DVR_STOP:
                /* 停止录像 */
                if (ctx->recording) {
                    ctx->recording = 0;
                    ctx->paused = 0;
                    dvr_storage_close(&ctx->storage);
                    LOG_INFO("dvr","Recording STOPPED (total=%us)",
                             ctx->rec_elapsed_sec);
                    ctx->rec_elapsed_sec = 0;
                }
                send_dvr_status(ctx);
                break;

            case MSG_DVR_PAUSE:
                /* 暂停/继续录像 */
                if (ctx->recording && !ctx->paused) {
                    /* 暂停: 累计已录制时长, 停止写入 */
                    ctx->paused = 1;
                    ctx->rec_elapsed_sec += (uint32_t)(time(NULL) - ctx->rec_start_time);
                    if (ctx->storage.current_fp) {
                        fflush(ctx->storage.current_fp);
                    }
                    LOG_INFO("dvr","Recording PAUSED (elapsed=%us)",
                             ctx->rec_elapsed_sec);
                } else if (ctx->recording && ctx->paused) {
                    /* 继续 */
                    ctx->paused = 0;
                    ctx->rec_start_time = time(NULL);
                    LOG_INFO("dvr","Recording RESUMED (elapsed=%us)",
                             ctx->rec_elapsed_sec);
                }
                send_dvr_status(ctx);
                break;

            case MSG_DVR_SNAPSHOT:
                /* 拍照标记: 下一帧到达时保存 */
                ctx->pending_snapshot = 1;
                LOG_INFO("dvr","Snapshot requested");
                break;

            default:
                break;
            }
        }
    }
}

/**
 * @brief DVR守护进程入口 — 初始化各子系统并运行poll循环。
 *
 * 初始化顺序:
 *   1. 从 argv[1] 解析可选的配置文件路径
 *   2. 零初始化全局上下文 (g_ctx), 设置 client_fd = -1
 *   3. 打开syslog风格的日志文件
 *   4. 加载配置 (摄像头设备、存储路径、配额)
 *   5. 初始化存储管理器并创建 /record/ 目录
 *   6. 打开V4L2摄像头设备并启动采集流
 *   7. 创建Unix域套接字用于界面IPC
 *   8. 注册信号处理函数 (SIGTERM, SIGINT) 以便干净退出
 *
 * 主循环 (g_running):
 *   - poll() 监听最多3个fd: V4L2, 服务器套接字, 客户端套接字
 *   - 帧处理: 首帧拍照、定期预览更新、
 *     分段轮转、录制写入、异步拍照
 *   - 套接字分发: 接受界面连接、处理命令
 *   - 定期任务: 状态推送(2秒)、存储清理(poll超时时)
 *
 * 退出序列 (SIGTERM/SIGINT或出错时):
 *   1. 关闭当前录制分段 (fflush + fclose)
 *   2. 关闭界面客户端套接字
 *   3. 销毁ringbuffer
 *   4. 关闭并unlink服务器套接字
 *   5. 停止V4L2采集流并关闭设备
 *   6. 关闭日志文件
 *
 * @param argc 参数数量
 * @param argv argv[1] 可选地覆盖配置文件路径
 * @return     正常退出返回0, 致命错误返回1 (摄像头不可用)
 */
/* ---- 主函数 ---- */
int main(int argc, char *argv[])
{
    const char *conf = (argc >= 2) ? argv[1] : "../config/config.ini";
    int w = DVR_DEFAULT_WIDTH, h = DVR_DEFAULT_HEIGHT;

    printf("[dvr] dvr_daemon starting...\n");

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.client_fd = -1;

    log_init(DVR_LOG_PATH, LOG_INFO, 1024*1024, 3);
    LOG_INFO("dvr","===== dvr_daemon starting =====");

    dvr_config_load(&g_ctx, conf);
    dvr_storage_init(&g_ctx.storage, g_ctx.storage.base_path,
                     g_ctx.storage.max_size_mb, g_ctx.storage.segment_sec);

    /* 确保 /record/ 目录存在 */
    mkdir(g_ctx.storage.base_path, 0755);

    /* V4L2摄像头 */
    if (v4l2_open(g_ctx.camera_dev, &w, &h, &g_ctx.v4l2) != 0) {
        LOG_ERROR("dvr","Camera not available");
        return 1;
    }
    v4l2_start(&g_ctx.v4l2);

    /* 套接字 */
    g_ctx.server_fd = sock_create_server(DVR_SOCKET_PATH);
    if (g_ctx.server_fd < 0) return 1;
    ringbuffer_init(&g_ctx.recv_rb, 4096);
    protocol_parser_init(&g_ctx.proto_parser);
    signal(SIGTERM, sig_handler); signal(SIGINT, sig_handler);

    LOG_INFO("dvr","Init complete (poll mode). Loop starting...");

    /* 主循环 (poll v4l2 fd + socket) */
    time_t seg_start = 0;
    int first_frame = 1;

    while (g_running) {
        struct pollfd pfds[4]; int nfds = 0;
        pfds[nfds].fd = g_ctx.v4l2.fd; pfds[nfds].events = POLLIN; nfds++;
        pfds[nfds].fd = g_ctx.server_fd; pfds[nfds].events = POLLIN; nfds++;
        if (g_ctx.client_fd >= 0) {
            pfds[nfds].fd = g_ctx.client_fd; pfds[nfds].events = POLLIN; nfds++;
        }

        int ret = poll(pfds, nfds, DVR_POLL_TIMEOUT_MS);
        if (ret < 0) { if (errno == EINTR) continue; break; }

        int v4l2_ev = pfds[0].revents;
        int srv_ev  = pfds[1].revents;
        int cli_ev  = (nfds >= 3) ? pfds[2].revents : 0;

        /* ---- V4L2 帧处理 ---- */
        if (v4l2_ev & POLLIN) {
            void *data; int size, idx;
            if (v4l2_dequeue(&g_ctx.v4l2, &data, &size, &idx) == 0) {
                g_ctx.frame_total++;

                /* 首帧自动拍照验证 */
                if (first_frame) {
                    save_snapshot(&g_ctx, data, size);
                    first_frame = 0;
                }

                /* 每 15 帧 (~1s) 更新预览图, 供 Qt UI 显示 */
                if (g_ctx.frame_total % 15 == 0) {
                    FILE *fp = fopen("/tmp/dvr_preview.jpg", "wb");
                    if (fp) { fwrite(data, 1, (size_t)size, fp); fclose(fp); }
                }

                /* 每 150 帧 (~10s) 打印状态 */
                if (g_ctx.frame_total % 150 == 0) {
                    uint32_t elap = 0;
                    if (g_ctx.recording && !g_ctx.paused)
                        elap = (uint32_t)(time(NULL) - g_ctx.rec_start_time) + g_ctx.rec_elapsed_sec;
                    else if (g_ctx.recording && g_ctx.paused)
                        elap = g_ctx.rec_elapsed_sec;
                    LOG_INFO("dvr","frame=%u %dx%d %s elapsed=%us",
                             g_ctx.frame_total, g_ctx.v4l2.width, g_ctx.v4l2.height,
                             g_ctx.recording ? (g_ctx.paused ? "PAUSED" : "REC") : "idle",
                             elap);
                }

                /* ---- 录像写入 ---- */
                if (g_ctx.recording && !g_ctx.paused) {
                    time_t now = time(NULL);
                    if (seg_start == 0) seg_start = now;
                    if (now - seg_start >= g_ctx.storage.segment_sec) {
                        if (dvr_storage_open_segment(&g_ctx.storage, now) != 0) {
                            LOG_ERROR("dvr","Segment open failed — recording ABORTED");
                            g_ctx.recording = 0;
                        }
                        seg_start = now;
                    }
                    if (g_ctx.recording) {
                        int wrote = dvr_storage_write(&g_ctx.storage, (uint8_t*)data, size);
                        if (wrote < 0 && g_ctx.frame_total % 150 == 0) {
                            LOG_ERROR("dvr","Write failed — file not open? Check /record/ permissions");
                        }
                    }
                }

                /* ---- 拍照 (用户触发) ---- */
                if (g_ctx.pending_snapshot) {
                    save_snapshot(&g_ctx, data, size);
                    g_ctx.pending_snapshot = 0;
                }

                v4l2_queue(&g_ctx.v4l2, idx);
            }
        }

        /* ---- Socket 事件 ---- */
        handle_sockets(&g_ctx, srv_ev, cli_ev);

        /* 每2秒推送状态给 UI */
        static time_t last_status_send = 0;
        time_t now = time(NULL);
        if (now - last_status_send >= 2) {
            last_status_send = now;
            if (g_ctx.client_fd >= 0) {
                send_dvr_status(&g_ctx);
            }
        }

        /* poll 超时: 定期存储检查 (约每30s) */
        static int tick = 0;
        if (ret == 0 && ++tick % 30 == 0)
            dvr_storage_check_and_clean(&g_ctx.storage);
    }

    LOG_INFO("dvr","Exiting...");
    if (g_ctx.recording) dvr_storage_close(&g_ctx.storage);
    if (g_ctx.client_fd >= 0) sock_close(g_ctx.client_fd, NULL);
    ringbuffer_destroy(&g_ctx.recv_rb);
    sock_close(g_ctx.server_fd, DVR_SOCKET_PATH);
    v4l2_close(&g_ctx.v4l2);
    log_close();
    printf("[dvr] dvr_daemon stopped.\n");
    return 0;
}
