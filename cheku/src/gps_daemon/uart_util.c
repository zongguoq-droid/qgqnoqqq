/**
 * @file    uart_util.c
 * @brief   UART 串口工具 - 实现 (termios 配置)
 *
 * termios 串口配置详解:
 *
 * struct termios 主要字段:
 *
 *   c_cflag (控制标志):
 *     CS8        — 8 位数据位 (CS5/CS6/CS7/CS8)
 *     CSTOPB     — 2 位停止位 (不设 = 1位)
 *     PARENB     — 启用校验位
 *     PARODD     — 奇校验 (不设+PARENB = 偶校验)
 *     CREAD      — 启用接收器
 *     CLOCAL     — 忽略调制解调器控制线
 *     HUPCL      — 关闭时挂断
 *
 *   c_lflag (本地标志):
 *     ICANON     — 规范模式 (行缓冲, 等换行才返回)
 *     ECHO       — 回显输入字符
 *     ECHOE      — 回显擦除
 *     ISIG       — 信号字符 (Ctrl+C → SIGINT)
 *     以上全部关闭 = 原始模式 (raw mode)
 *
 *   c_iflag (输入标志):
 *     ICRNL      — 将输入 CR 转为 NL
 *     IXON       — 软件流控 (Ctrl+S/Ctrl+Q)
 *     INLCR      — 将输入 NL 转为 CR
 *
 *   c_oflag (输出标志):
 *     OPOST      — 启用输出处理
 *     ONLCR      — 将输出 NL 转为 CR+NL
 *
 *   c_cc[VMIN] / c_cc[VTIME] (非规范模式读取控制):
 *     VMIN=0, VTIME=0  → 有数据立即返回, 无数据返回0
 *     VMIN=0, VTIME>0  → 有数据返回, 无数据等 VTIME×0.1s 超时
 *     VMIN>0, VTIME=0  → 阻塞直到至少 VMIN 字节
 *     VMIN>0, VTIME>0  → 收到任何字节后等 VTIME×0.1s, 或收满 VMIN
 *
 *   GPS 串口使用 VMIN=0, VTIME=1:
 *     - 有数据时立即返回 (适合 epoll 事件驱动)
 *     - 无数据时最多等 100ms (防止 epoll 误触发时 CPU 空转)
 *
 * 学习要点:
 *   1. cfmakeraw(): 一行代码设置原始模式
 *   2. cfsetispeed/cfsetospeed: 为什么输入输出要分开设置?
 *      (某些硬件支持非对称速率)
 *   3. O_NOCTTY: 嵌入式串口编程的最佳实践
 *   4. VMIN/VTIME: 非规范模式下精细控制读取行为
 */

#include "uart_util.h"
#include "log/log.h"
#include <stdio.h>      /* perror, snprintf */
#include <stdlib.h>     /* NULL */
#include <string.h>     /* memset, strerror */
#include <unistd.h>     /* close, read, write */
#include <fcntl.h>      /* open, fcntl, O_RDWR, O_NOCTTY, O_NONBLOCK */
#include <errno.h>      /* errno, EAGAIN */
#include <termios.h>    /* struct termios, tcgetattr, tcsetattr, cfmakeraw, ... */

/* 保存原始 termios 属性 (用于退出时恢复) */
static struct termios g_old_termios;
static int g_old_saved = 0;

int uart_open(const char *device, speed_t baud_rate)
{
    int fd;
    struct termios tio;

    if (!device) return -1;

    /* 1. 打开串口设备
     *
     * O_RDWR:    GPS 通常只需读, 但某些模块(如 AT 指令 GPS)需要写
     * O_NOCTTY:  关键! 防止设备被误认为控制终端
     *             (否则设备收到 Ctrl+C 可能会发送 SIGINT 给进程)
     * O_NONBLOCK: 非阻塞 — open() 立即返回, 不等待 DCD 信号 */
    fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        LOG_ERROR("uart", "Cannot open %s: %s", device, strerror(errno));
        return -1;
    }

    /* 2. 保存当前 termios (用于退出时恢复) */
    if (tcgetattr(fd, &g_old_termios) == 0) {
        g_old_saved = 1;
    }

    /* 3. 配置新的 termios */
    memset(&tio, 0, sizeof(tio));

    /* 3a. 设为原始模式
     *
     * cfmakeraw() 等价于:
     *   tio.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON);
     *   tio.c_oflag &= ~OPOST;
     *   tio.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
     *   tio.c_cflag &= ~(CSIZE|PARENB);
     *   tio.c_cflag |= CS8;
     *
     * 原始模式 = 字符直接透传, 内核不做任何加工。
     * 这正是 GPS NMEA 解析需要的: 每个字节按原样接收。 */
    cfmakeraw(&tio);

    /* 3b. 设置波特率 (输入+输出)
     *
     * cfsetispeed: 输入波特率 (PC←模块)
     * cfsetospeed: 输出波特率 (PC→模块)
     * GPS 模块标配 9600 bps */
    cfsetispeed(&tio, baud_rate);
    cfsetospeed(&tio, baud_rate);

    /* 3c. 启用接收器和本地模式
     *
     * CREAD:  启用接收器 (必须! 否则收不到数据)
     * CLOCAL: 忽略调制解调器状态线 (DCD/DTR/DSR)
     *         对于 GPS 模块 (通常只有 TX/RX/GND), 必须设置此项。
     *         否则 open 会阻塞等待 DCD 信号。 */
    tio.c_cflag |= CREAD | CLOCAL;

    /* 3d. 8N1 配置
     *
     * CS8:      8 数据位 (CS5=5, CS6=6, CS7=7, CS8=8)
     * PARENB:   不设 = 无校验位
     * CSTOPB:   不设 = 1 停止位 */
    tio.c_cflag &= ~PARENB;    /* 无校验 */
    tio.c_cflag &= ~CSTOPB;    /* 1 停止位 */
    tio.c_cflag &= ~CSIZE;     /* 清除数据位掩码 */
    tio.c_cflag |= CS8;        /* 8 数据位 */

    /* 3e. 关闭硬件流控
     *
     * CRTSCTS: 硬件流控 (RTS/CTS 引脚)
     * 大多数 GPS 模块不使用硬件流控, 关闭以避免意外 */
    tio.c_cflag &= ~CRTSCTS;

    /* 3f. 非规范模式读取参数
     *
     * VMIN=0:  不要求最少字节数, 有数据就返回
     * VTIME=1: 无数据时超时 100ms (1×0.1s)
     *          如果有数据, 读到第一个字节后立即返回
     *
     * 这个组合适合 epoll + 非阻塞:
     *   epoll 通知可读 → read → 读多少算多少 → EAGAIN → 等下次 epoll */
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 1;

    /* 4. 写入配置 (TCSANOW = 立即生效)
     *
     * tcsetattr 的 action 参数:
     *   TCSANOW:   立即生效
     *   TCSADRAIN: 等待输出队列排空后生效
     *   TCSAFLUSH: 等待输出队列排空 + 清空输入队列 */
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        LOG_ERROR("uart", "tcsetattr failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* 5. 清空串口缓冲区
     *
     * tcflush 清除内核缓冲区中的残留数据:
     *   TCIFLUSH:  清空输入缓冲 (未读数据)
     *   TCOFLUSH:  清空输出缓冲 (未发送数据)
     *   TCIOFLUSH: 两者都清空
     *
     * 打开串口时清空输入缓冲: 避免读到之前残留的垃圾数据 */
    tcflush(fd, TCIOFLUSH);

    /* 6. 再次确保非阻塞 (fcntl)
     *
     * open 时用了 O_NONBLOCK, 但某些驱动可能重置。
     * fcntl F_SETFL 再次确认非阻塞标志。 */
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags != -1) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    /* speed_t 是索引值不是实际bps, 转为可读数字 */
    int bps = 9600;
    switch (baud_rate) {
    case B4800: bps=4800; break; case B9600: bps=9600; break;
    case B19200: bps=19200; break; case B38400: bps=38400; break;
    case B57600: bps=57600; break; case B115200: bps=115200; break;
    default: break;
    }
    LOG_INFO("uart", "Serial port opened: %s (fd=%d, %dbps, 8N1)",
             device, fd, bps);

    return fd;
}

int uart_read(int fd, uint8_t *buf, uint32_t size)
{
    ssize_t n;

    if (fd < 0 || !buf || size == 0) return -1;

    /* read() 在非阻塞模式下的行为:
     *   - 有数据: 返回读取的字节数 (1~size)
     *   - 无数据: 返回 -1, errno=EAGAIN/EWOULDBLOCK
     *   - 串口断开: 返回 -1, errno=EIO
     *
     * 注意: 即使 epoll 通知可读, read 仍可能返回 0 字节
     * (在非规范模式下, VMIN=0 允许返回 0) */
    n = read(fd, buf, size);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  /* 无数据 (正常) */
        }
        if (errno == EINTR) {
            return 0;  /* 被信号中断 */
        }
        LOG_ERROR("uart", "read error: %s", strerror(errno));
        return -1;
    }

    return (int)n;  /* n≥0: 读取的字节数 */
}

void uart_close(int fd)
{
    if (fd < 0) return;

    /* 恢复原始 termios (尊重其他可能使用此串口的程序) */
    if (g_old_saved) {
        tcsetattr(fd, TCSANOW, &g_old_termios);
        g_old_saved = 0;
    }

    close(fd);
    LOG_INFO("uart", "Serial port closed (fd=%d)", fd);
}
