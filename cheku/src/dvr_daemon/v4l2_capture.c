/**
 * @file    v4l2_capture.c
 * @brief   V4L2 采集实现
 *
 * 采集流程:
 *   open → QUERYCAP → ENUM_FMT → S_FMT(MJPEG) → REQBUFS(32)
 *   → QUERYBUF+mmap x32 → QBUF x32 → STREAMON
 *   poll(fd, POLLIN) → DQBUF → [存储] → QBUF
 */

#include "v4l2_capture.h"
#include "log/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <poll.h>

int v4l2_open(const char *dev, int *width, int *height, v4l2_ctx_t *ctx)
{
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers rb;
    int fd, i;

    if (!dev || !ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));

    /* 1. open (阻塞模式 — 与测试程序一致) */
    fd = open(dev, O_RDWR);
    if (fd < 0) { 
        LOG_ERROR("v4l2","Cannot open %s: %s",dev,strerror(errno)); return -1; 
    }
    ctx->fd = fd;
    /* 2. QUERYCAP */
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0 ||
        !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING)) {
        LOG_ERROR("v4l2","Device does not support video capture+streaming");
        close(fd); return -1;
    }
    LOG_INFO("v4l2","Camera: %s (driver: %s)", cap.card, cap.driver);

    /* 3. S_FMT: MJPEG */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = (__u32)(*width);
    fmt.fmt.pix.height      = (__u32)(*height);
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
        LOG_ERROR("v4l2","Cannot set MJPEG format — camera may not support it");
        close(fd); return -1;
    }
    ctx->width      = (int)fmt.fmt.pix.width;
    ctx->height     = (int)fmt.fmt.pix.height;
    ctx->frame_size = (int)fmt.fmt.pix.sizeimage;
    *width  = ctx->width;
    *height = ctx->height;
    LOG_INFO("v4l2","Format: MJPEG %dx%d frame_size=%d", ctx->width, ctx->height, ctx->frame_size);

    /* 4. REQBUFS: 32 buffers */
    memset(&rb, 0, sizeof(rb));
    rb.count  = V4L2_BUF_COUNT;
    rb.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &rb) != 0) {
        LOG_ERROR("v4l2","REQBUFS failed"); close(fd); return -1;
    }
    ctx->buf_count = (int)rb.count;

    /* 5. QUERYBUF + mmap */
    ctx->bufs = (v4l2_buf_t*)calloc((size_t)ctx->buf_count, sizeof(v4l2_buf_t));
    for (i = 0; i < ctx->buf_count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = (__u32)i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) { LOG_ERROR("v4l2","QUERYBUF[%d]",i); goto err; }
        ctx->bufs[i].length = (size_t)buf.length;
        ctx->bufs[i].start  = mmap(NULL, (size_t)buf.length,
                                   PROT_READ|PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (ctx->bufs[i].start == MAP_FAILED) { LOG_ERROR("v4l2","mmap[%d]",i); goto err; }
    }
    LOG_INFO("v4l2","Mapped %d buffers", ctx->buf_count);
    return 0;

err:
    for (int j = 0; j < i; j++) munmap(ctx->bufs[j].start, ctx->bufs[j].length);
    free(ctx->bufs); close(fd); return -1;
}

int v4l2_start(v4l2_ctx_t *ctx)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct v4l2_buffer buf;

    /* QBUF all */
    for (int i = 0; i < ctx->buf_count; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = (__u32)i;
        if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) != 0) { LOG_ERROR("v4l2","QBUF[%d]",i); return -1; }
    }

    /* STREAMON */
    if (ioctl(ctx->fd, VIDIOC_STREAMON, &type) != 0) {
        LOG_ERROR("v4l2","STREAMON failed"); return -1;
    }
    LOG_INFO("v4l2","Capture started.");
    return 0;
}

int v4l2_dequeue(v4l2_ctx_t *ctx, void **data, int *size, int *index)
{
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(ctx->fd, VIDIOC_DQBUF, &buf) != 0) return -1;

    *data  = ctx->bufs[buf.index].start;
    *size  = (int)buf.bytesused;
    *index = (int)buf.index;
    return 0;
}

int v4l2_queue(v4l2_ctx_t *ctx, int index)
{
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = (__u32)index;
    return (ioctl(ctx->fd, VIDIOC_QBUF, &buf) == 0) ? 0 : -1;
}

int v4l2_get_fd(v4l2_ctx_t *ctx) { return ctx ? ctx->fd : -1; }

void v4l2_close(v4l2_ctx_t *ctx)
{
    if (!ctx || ctx->fd < 0) return;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < ctx->buf_count; i++) munmap(ctx->bufs[i].start, ctx->bufs[i].length);
    free(ctx->bufs);
    close(ctx->fd);
    ctx->fd = -1;
    LOG_INFO("v4l2","Closed.");
}
