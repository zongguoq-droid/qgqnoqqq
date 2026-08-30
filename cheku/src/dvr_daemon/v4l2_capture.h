/**
 * @file    v4l2_capture.h
 * @brief   V4L2 USB 摄像头采集 - 头文件
 *
 * 采集方案:
 *   - open(O_RDWR) 阻塞模式
 *   - poll(fd, POLLIN, -1) 等待帧
 *   - VIDIOC_DQBUF → 直接 fwrite (MJPEG 已是 JPEG)
 *   - VIDIOC_QBUF 放回
 *   - 32 个 mmap 缓冲区
 */

#ifndef _V4L2_CAPTURE_H_
#define _V4L2_CAPTURE_H_

#include <stdint.h>
#include <linux/videodev2.h>
#include <stddef.h>
#define V4L2_BUF_COUNT  32      /* 内核缓冲区数量 (参照测试程序) */

typedef struct {
    void   *start;
    size_t  length;
} v4l2_buf_t;

typedef struct {
    int         fd;
    v4l2_buf_t *bufs;
    int         buf_count;
    int         width;
    int         height;
    int         frame_size;
} v4l2_ctx_t;

/**
 * @brief 打开摄像头, 枚举格式, 设置 MJPEG, mmap 缓冲区
 * @param dev    设备路径 ("/dev/video0")
 * @param width  期望宽度 (输出实际协商值)
 * @param height 期望高度
 * @param ctx    输出: V4L2 上下文
 * @return       0=成功, -1=失败
 */
int v4l2_open(const char *dev, int *width, int *height, v4l2_ctx_t *ctx);

/** @brief VIDIOC_STREAMON + QBUF all buffers */
int v4l2_start(v4l2_ctx_t *ctx);

/** @brief VIDIOC_DQBUF (阻塞直到有新帧, 配合 poll 使用) */
int v4l2_dequeue(v4l2_ctx_t *ctx, void **data, int *size, int *index);

/** @brief VIDIOC_QBUF 放回 */
int v4l2_queue(v4l2_ctx_t *ctx, int index);

/** @brief 获取 fd (用于 poll 注册) */
int v4l2_get_fd(v4l2_ctx_t *ctx);

/** @brief VIDIOC_STREAMOFF + munmap + close */
void v4l2_close(v4l2_ctx_t *ctx);

#endif
