/**
 * @file    can_driver.c
 * @brief   SocketCAN 驱动封装 - 实现
 *
 * ================================================================
 *  SocketCAN 架构简介
 * ================================================================
 *
 *  应用层:
 *    socket(PF_CAN, SOCK_RAW, CAN_RAW) → send/recv can_frame
 *
 *  协议层 (net/can/):
 *    CAN RAW 协议: 原始 CAN 帧收发
 *    CAN BCM 协议: 广播管理器 (周期发送/内容过滤)
 *    CAN GW:       CAN 网关 (can0 ↔ can1 路由)
 *
 *  驱动层 (drivers/net/can/):
 *    flexcan.ko:    i.MX6ULL FlexCAN 控制器驱动
 *    mcp251x.ko:    SPI-CAN 控制器 (MCP2515)
 *
 *  硬件层:
 *    i.MX6ULL FlexCAN 控制器 → CAN 收发器 (如 TJA1050) → CAN 总线
 *
 * ================================================================
 *  CAN 错误处理策略
 * ================================================================
 *
 *  CAN 控制器有三种错误状态 (CAN 2.0 规范):
 *    1. Error Active  (主动错误): TEC≤96 且 REC≤96
 *    2. Error Passive (被动错误): TEC>127 或 REC>127
 *    3. Bus Off       (总线关闭): TEC>255
 *
 *  SocketCAN 在状态转换时自动生成 error frame,
 *  应用层通过接收这些 error frame 监控总线健康。
 *
 *  总线关闭恢复:
 *    内核可以配置自动恢复 (restart-ms), 或应用层手动恢复:
 *      ip link set can0 down && ip link set can0 up type can bitrate 500000
 *    本模块检测 BUS_OFF 后记录日志, 建议在启动脚本中启用自动恢复。
 *
 * 学习要点:
 *   1. socket()/bind() 在 CAN 总线上的使用
 *   2. struct can_frame 的 can_id 位布局
 *   3. ioctl SIOCGIFINDEX: 网络接口名 → 索引
 *   4. 错误帧识别: can_id & CAN_ERR_FLAG
 *   5. TEC/REC 计数器: 总线健康指标
 */

#include "can_driver.h"
#include "log/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>     /* socket, bind, AF_CAN, PF_CAN, sockaddr */
#include <sys/ioctl.h>      /* ioctl, SIOCGIFINDEX */
#include <net/if.h>         /* struct ifreq, if_nametoindex */

/* 内核 4.9 头文件未导出, 手动定义 */
#ifndef SOL_CAN_RAW
#define SOL_CAN_RAW         101
#endif
#ifndef CAN_RAW_ERR_FILTER
#define CAN_RAW_ERR_FILTER  5
#endif

int can_socket_open(const char *ifname, int *ifindex)
{
    int fd;
    struct sockaddr_can addr;
    struct ifreq ifr;

    if (!ifname || !ifindex) return -1;

    /* 1. 创建 CAN 原始套接字
     *
     * socket(协议族, 类型, 协议):
     *   PF_CAN:    CAN 协议族 (Address Family CAN)
     *   SOCK_RAW:  原始套接字 — 直接构造/解析链路层帧
     *   CAN_RAW:   原始 CAN 协议 (无额外协议头)
     *
     * 也可用 AF_CAN (等同 PF_CAN),
     * 或 SOCK_DGRAM + CAN_BCM (广播管理器, 支持周期发送) */
    fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        LOG_ERROR("can", "socket(PF_CAN) failed: %s", strerror(errno));
        LOG_ERROR("can", "Is CAN kernel module loaded? (modprobe can; modprobe can_raw; modprobe flexcan)");
        return -1;
    }

    /* 2. 非阻塞模式 */
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags != -1) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    /* 3. 获取 CAN 接口索引
     *
     * 方法1: if_nametoindex(ifname) — 简单的 POSIX 函数
     * 方法2: ioctl(SIOCGIFINDEX)     — 更底层的 ioctl
     *
     * 接口索引是内核中网络接口的唯一标识符 (整数),
     * 如 lo=1, eth0=2, can0=3 (取决于注册顺序)。 */
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        LOG_ERROR("can", "ioctl SIOCGIFINDEX for %s failed: %s",
                  ifname, strerror(errno));
        LOG_ERROR("can", "Is '%s' up? Try: ip link set %s type can bitrate 500000 && ip link set %s up",
                  ifname, ifname, ifname);
        close(fd);
        return -1;
    }

    *ifindex = ifr.ifr_ifindex;

    /* 4. 绑定到指定 CAN 接口
     *
     * struct sockaddr_can {
     *     sa_family_t can_family;  // AF_CAN
     *     int         can_ifindex; // 接口索引
     * };
     *
     * bind 后, 此 socket 只收发来自 can0 的帧。
     * 如果想接收所有 CAN 接口的帧, 可以设置 can_ifindex=0。 */
    memset(&addr, 0, sizeof(addr));
    addr.can_family  = AF_CAN;
    addr.can_ifindex = *ifindex;

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("can", "bind to %s (ifindex=%d) failed: %s",
                  ifname, *ifindex, strerror(errno));
        close(fd);
        return -1;
    }

    /* 5. 启用 CAN 错误帧接收
     *
     * 默认情况下, SocketCAN 过滤掉错误帧。
     * 设置 CAN_RAW_ERR_FILTER 后, 所有错误帧也会发送到本 socket。
     *
     * setsockopt(socket, level, optname, optval, optlen):
     *   SOL_CAN_RAW:        协议级别 (CAN RAW)
     *   CAN_RAW_ERR_FILTER: 错误帧过滤器
     *   optval = CAN_ERR_MASK: 不过滤, 接收所有类型错误帧 */
    {
        can_err_mask_t err_mask = CAN_ERR_MASK;
        setsockopt(fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER,
                   &err_mask, sizeof(err_mask));
    }

    LOG_INFO("can", "CAN socket opened: %s (fd=%d, ifindex=%d)",
             ifname, fd, *ifindex);

    return fd;
}

int can_send(int fd, const can_msg_t *msg)
{
    struct can_frame frame;
    ssize_t n;

    if (fd < 0 || !msg) return -1;
    if (msg->can_dlc > 8) return -1;  /* 标准 CAN 最大 8 字节 */

    memset(&frame, 0, sizeof(frame));

    /* 构造 CAN ID (标准帧/扩展帧)
     *
     * CAN ID 编码:
     *   标准帧 (11-bit): frame.can_id = msg->can_id & 0x7FF
     *   扩展帧 (29-bit): frame.can_id = (msg->can_id & 0x1FFFFFFF) | CAN_EFF_FLAG
     *
     * CAN_EFF_FLAG (0x80000000) 是扩展帧标志位,
     * 发送时必须设置, 否则内核会当作标准帧处理。
     *
     * CAN_RTR_FLAG (0x40000000) 是远程帧标志位。 */
    if (msg->is_extended) {
        frame.can_id = (msg->can_id & CAN_EFF_MASK) | CAN_EFF_FLAG;
    } else {
        frame.can_id = msg->can_id & CAN_SFF_MASK;
    }

    /* 远程帧 */
    if (msg->is_remote) {
        frame.can_id |= CAN_RTR_FLAG;
    }

    frame.can_dlc = msg->can_dlc;
    memcpy(frame.data, msg->data, msg->can_dlc);

    /* write() 等价于 sendto() — socket 已 bind, 不需要指定地址 */
    n = write(fd, &frame, sizeof(frame));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  /* 发送缓冲区满 */
        }
        LOG_ERROR("can", "CAN send failed: %s (errno=%d)", strerror(errno), errno);
        if (errno == ENETDOWN) {
            LOG_ERROR("can", "CAN network down — bus off? Try: ip link set can0 up");
        }
        return -1;
    }

    if (n != sizeof(frame)) {
        LOG_WARN("can", "CAN partial send: %zd/%zu bytes", n, sizeof(frame));
        return -1;
    }

    return (int)n;
}

int can_recv(int fd, can_msg_t *msg)
{
    struct can_frame frame;
    ssize_t n;

    if (fd < 0 || !msg) return -1;

    memset(&frame, 0, sizeof(frame));
    memset(msg, 0, sizeof(*msg));

    /* read() 在非阻塞模式下返回:
     *   >0 → 收到完整 CAN 帧 (CAN_MTU = 16, CANFD_MTU = 72)
     *   0  → 对端关闭 (CAN socket 不会出现此情况)
     *   -1 → EAGAIN=无数据, 其他=错误 */
    n = read(fd, &frame, sizeof(frame));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  /* 无数据 */
        }
        if (errno == EINTR) {
            return 0;
        }
        if (errno == ENETDOWN) {
            LOG_WARN("can", "CAN interface is DOWN (bus off?)");
            return -1;
        }
        LOG_ERROR("can", "CAN recv error: %s", strerror(errno));
        return -1;
    }

    if (n != CAN_MTU && n != CANFD_MTU) {
        LOG_WARN("can", "CAN recv unexpected size: %zd (expected %d or %d)",
                 n, CAN_MTU, CANFD_MTU);
        return -1;
    }

    /* ---- 检查错误帧 ---- */
    if (frame.can_id & CAN_ERR_FLAG) {
        /* 这是内核生成的错误帧 (CAN_ERR_FLAG = 0x20000000)
         *
         * 错误帧不是正常的 CAN 报文, 而是内核的带内通知:
         * "总线发生了某类错误"。
         *
         * 调用者应解析错误帧获取 TEC/REC/状态信息 */
        return -2;  /* 特殊返回值: 错误帧 */
    }

    /* ---- 解析正常 CAN 帧 ---- */

    /* CAN ID 解码 */
    if (frame.can_id & CAN_EFF_FLAG) {
        /* 扩展帧: 取低 29 位 */
        msg->can_id     = frame.can_id & CAN_EFF_MASK;
        msg->is_extended = 1;
    } else {
        /* 标准帧: 取低 11 位 */
        msg->can_id     = frame.can_id & CAN_SFF_MASK;
        msg->is_extended = 0;
    }

    /* 远程帧 */
    msg->is_remote = (frame.can_id & CAN_RTR_FLAG) ? 1 : 0;

    /* 数据 */
    msg->can_dlc = frame.can_dlc;
    if (msg->can_dlc > 8) msg->can_dlc = 8;  /* 保护 */
    memcpy(msg->data, frame.data, msg->can_dlc);

    return (int)n;
}

void can_parse_error_frame(const struct can_frame *frame,
                           can_error_info_t *error_info)
{
    if (!frame || !error_info) return;
    if (!(frame->can_id & CAN_ERR_FLAG)) return;  /* 非错误帧 */

    /* 提取错误计数器 (data[6]=TEC, data[7]=REC)
     *
     * 这两个字节在 CAN_ERR_CNT 类型的错误帧中有效。
     * 并非所有错误帧都包含计数器。 */
    error_info->tec = frame->data[6];
    error_info->rec = frame->data[7];

    /* 判断总线状态 (根据 CAN_ERR_CRTL 类型) */
    if (frame->can_id & CAN_ERR_CRTL) {
        /* 控制器状态变化 (tec/rec 是 uint8_t, 最大值 255) */
        if (error_info->tec > 127 || error_info->rec > 127) {
            error_info->state = CAN_BUS_PASSIVE;
        } else if (error_info->tec > 96 || error_info->rec > 96) {
            error_info->state = CAN_BUS_WARNING;
        } else {
            error_info->state = CAN_BUS_OK;
        }
    }

    /* 特定错误类型检查 */
    if (frame->can_id & CAN_ERR_BUSOFF) {
        error_info->state = CAN_BUS_OFF;
        LOG_ERROR("can", "BUS OFF detected! TEC=%d REC=%d",
                  error_info->tec, error_info->rec);
    } else if (frame->can_id & CAN_ERR_BUSERROR) {
        LOG_WARN("can", "Bus error: TEC=%d REC=%d",
                 error_info->tec, error_info->rec);
    }
}

void can_socket_close(int fd)
{
    if (fd >= 0) {
        close(fd);
        LOG_INFO("can", "CAN socket closed (fd=%d)", fd);
    }
}
