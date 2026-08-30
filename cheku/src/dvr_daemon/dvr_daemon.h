/**
 * @file    dvr_daemon.h
 * @brief   DVR行车记录仪录制守护进程 — 配置、数据结构与存储API
 *
 * 核心设计:
 *   - V4L2 MJPEG采集 (poll模式, 32个内核缓冲区)
 *   - MJPEG帧直接保存为.jpg (已是JPEG编码, 无需转码)
 *   - 分段循环录制: 基于时间的文件分段 + 空间配额管理
 *
 * 主要功能 (近期添加):
 *   - 暂停/恢复: 跨暂停保留已累计录制时长。
 *     暂停时, 帧不写入磁盘; 已录制时长冻结。
 *     恢复时, 设置新的rec_start_time, 现有分段继续使用。
 *   - 时长追踪: rec_elapsed_sec 累计实际录制的秒数,
 *     可经历多次暂停/恢复周期。通过状态消息发送给Qt界面。
 *   - 拍照计数器 (snap_count): 每会话的序列号, 嵌入
 *     JPEG文件名中以防止同一秒内的冲突。
 *   - 待处理拍照标志 (pending_snapshot): 解耦异步界面命令
 *     与同步帧循环 — 下一个V4L2帧即保存为JPEG。
 */

#ifndef _DVR_DAEMON_H_
#define _DVR_DAEMON_H_

#include "common.h"
#include "protocol/protocol.h"
#include "v4l2_capture.h"
#include <stdint.h>
#include <stdio.h>      /* FILE */
#include <time.h>

#define DVR_DEFAULT_DEVICE      "/dev/video1"
#define DVR_DEFAULT_WIDTH       640
#define DVR_DEFAULT_HEIGHT      480
#define DVR_DEFAULT_STORAGE     "/record/"
#define DVR_DEFAULT_STORAGE_MB  2000
#define DVR_DEFAULT_SEGMENT_SEC 60
#define DVR_SOCKET_PATH         "/tmp/car_dvr.sock"
#define DVR_LOG_PATH            "/var/log/car_terminal/dvr.log"
#define DVR_POLL_TIMEOUT_MS     1000    /* poll 超时 (存储检查间隔) */

/* ---- 存储管理器 (分段循环录制) ---- */
typedef struct {
    char    base_path[256];     /**< 录制文件根目录 (如 "/record/") */
    int     max_size_mb;        /**< 总存储配额(MB); 超出时删除最旧分段 */
    int     segment_sec;        /**< 每个分段文件的时长(秒, 默认60) */
    int     current_segment;    /**< 单调递增的分段计数器 (调试用) */
    FILE   *current_fp;         /**< 当前活动分段的文件句柄 (空闲/暂停时为NULL) */
    int     current_size;       /**< 当前分段已写入的字节数 */
} dvr_storage_t;

/* ---- 守护进程上下文 (全局单例) ---- */
typedef struct {
    v4l2_ctx_t      v4l2;           /**< V4L2采集状态 (fd, 缓冲区, 格式) */
    dvr_storage_t   storage;        /**< 存储管理器状态 (分段文件, 配额) */
    char            camera_dev[64]; /**< 摄像头设备路径 (如 "/dev/video1") */

    /* ---- 录制状态 (支持暂停/恢复) ---- */
    int             recording;      /**< 0=已停止, 1=录制中 (可能处于暂停) */
    int             paused;         /**< 0=活动, 1=已暂停 (帧不写入磁盘) */
    time_t          rec_start_time; /**< 当前录制周期开始的墙上时钟时间 */
    uint32_t        rec_elapsed_sec;/**< 当前会话中累计的录制秒数
                                         (不含暂停时间; 跨暂停/恢复边界
                                         保留, STOP时清零) */

    int             running;        /**< 主循环运行标志 */
    int             server_fd;      /**< 监听界面连接的Unix域套接字 */
    int             client_fd;      /**< 已连接的界面套接字 (-1表示无界面连接) */
    protocol_parser_t proto_parser; /**< 二进制传输协议的帧解析器 */
    ringbuffer_t    recv_rb;        /**< 供给协议解析器的接收ring-buffer */
    uint32_t        frame_total;    /**< 生命周期帧计数器 (用于定期任务) */

    /* ---- 拍照支持 ---- */
    uint32_t        snap_count;     /**< 会话拍照序列号 (自动递增,
                                         防止同一秒内文件名冲突) */
    int             pending_snapshot;/**< 1表示界面请求拍照但帧尚未到达;
                                         调用save_snapshot()后清除 */
} dvr_context_t;

/* ---- 存储API (实现在 storage_mgr.c 中) ---- */

/** 初始化存储管理器状态。
 *  错误修复 (memset别名): `path` 参数可能指向 `st->base_path`
 *  (调用者传入 &ctx->storage.base_path)。如果先执行memset(st,0,...),
 *  源字节在strncpy读取之前就被清零 — 导致空路径。
 *  修复方案: 在调用memset之前将 `path` 保存到本地缓冲区。 */
int  dvr_storage_init(dvr_storage_t *st, const char *path, int max_mb, int seg_sec);

/** 关闭上一个分段(如有)并打开新的带时间戳文件。
 *  错误修复 (缺少mkdir): 在fopen()之前添加 mkdir(st->base_path, 0755),
 *  使守护进程在首次运行时不会因 /record/ 目录不存在而失败。 */
int  dvr_storage_open_segment(dvr_storage_t *st, time_t start);

/** 将原始帧数据追加到当前分段文件。
 *  性能: 每15帧(~15fps下约1秒)调用一次fflush(), 在I/O吞吐量
 *  与数据安全性之间取得平衡 — 最大限度地减少意外关机时的数据丢失。 */
int  dvr_storage_write(dvr_storage_t *st, const uint8_t *data, int len);

/** 刷新并关闭当前分段文件。幂等操作 (可安全重复调用)。 */
void dvr_storage_close(dvr_storage_t *st);

/** 检查目录总使用量; 删除最旧分段直到低于80%配额。 */
int  dvr_storage_check_and_clean(dvr_storage_t *st);

/** 单个录制分段的描述符 (由 dvr_storage_list 返回)。 */
typedef struct {
    char    filepath[256];  /**< 磁盘上的绝对路径 */
    time_t  start_time;     /**< 文件修改时间 (作为录制开始时间的代理) */
    int     size_kb;        /**< 文件大小(KB) */
} dvr_segment_t;

/** 枚举存储目录中所有 .h264 分段文件。
 *  @param st    存储管理器状态
 *  @param list  输出数组 (调用者分配)
 *  @param max   `list` 的容量
 *  @return      实际写入 `list` 的条目数量 */
int  dvr_storage_list(dvr_storage_t *st, dvr_segment_t *list, int max);

/** 从INI格式文件加载守护进程配置。
 *  如果文件缺失或格式错误, 回退到编译期默认值。 */
int  dvr_config_load(dvr_context_t *ctx, const char *conf_file);

#endif
