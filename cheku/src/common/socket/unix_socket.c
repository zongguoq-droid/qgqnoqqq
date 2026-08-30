/**
 * @file    unix_socket.c
 * @brief   UNIX Domain Socket 封装 - 实现
 *
 * UNIX Domain Socket 地址结构 (struct sockaddr_un):
 *
 *   struct sockaddr_un {
 *       sa_family_t sun_family;    // AF_UNIX (固定值)
 *       char        sun_path[108]; // Socket 文件路径
 *   };
 *
 *   与 TCP Socket 的 struct sockaddr_in 对比:
 *     TCP:    AF_INET,  IP地址+端口号 (网络可达)
 *     UNIX:   AF_UNIX,  文件系统路径 (仅本地可达)
 *     API:    完全一样! socket/bind/listen/accept/connect/send/recv
 *
 *   这就是 Linux "一切皆文件" 哲学的体现:
 *     网络通信和本地通信使用同一套 API,
 *     只需改变地址族 (AF_INET vs AF_UNIX)。
 *
 * 非阻塞 IO 关键概念:
 *   fcntl(fd, F_SETFL, O_NONBLOCK) 设置后:
 *     accept → 无等待连接时返回 -1 (errno=EAGAIN) 而非阻塞
 *     recv   → 无数据时返回 -1 (errno=EAGAIN) 而非阻塞
 *     send   → 缓冲区满时返回部分发送的字节数
 *     connect → 可能返回 -1 (errno=EINPROGRESS)
 *
 *   这就是 epoll 事件驱动模型的基础:
 *     epoll 通知 "fd 可读/可写" → 调用 recv/send → 立即成功
 *
 * 学习要点:
 *   1. sockaddr_un 结构体的使用
 *   2. fcntl 设置文件描述符标志 (O_NONBLOCK)
 *   3. bind 失败处理: unlink 旧 Socket 文件
 *   4. SO_REUSEADDR 在 UNIX Socket 上的作用
 *   5. send 返回值处理: 部分发送 (短写) 问题
 */

#include "socket/unix_socket.h"
#include "protocol/protocol.h"
#include "log/log.h"

#include <stdio.h>          /* perror */
#include <stdlib.h>         /* NULL */
#include <string.h>         /* memset, strncpy */
#include <unistd.h>         /* close, unlink, read, write */
#include <fcntl.h>          /* fcntl, F_GETFL, F_SETFL, O_NONBLOCK */
#include <errno.h>          /* errno, EAGAIN, EINTR */
#include <sys/socket.h>     /* socket, bind, listen, accept, connect, send, recv */
#include <sys/un.h>         /* struct sockaddr_un, AF_UNIX */

/**
 * @brief 设置文件描述符为非阻塞模式
 *
 * 非阻塞模式的本质:
 *   默认 (阻塞):      read(fd) → 卡住直到有数据
 *   O_NONBLOCK:       read(fd) → 立即返回 -1 (errno=EAGAIN) 或返回已有数据
 *
 * fcntl 操作:
 *   1. F_GETFL: 获取当前标志
 *   2. flags | O_NONBLOCK: 添加非阻塞标志
 *   3. F_SETFL: 设置新标志
 *
 * @param fd  文件描述符
 * @return    0=成功, -1=失败
 */
static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL O_NONBLOCK");
        return -1;
    }

    return 0;
}

/* ================================================================
 *  API 实现
 * ================================================================ */

int sock_create_server(const char *socket_path)
{
    int fd;
    struct sockaddr_un addr;

    if (!socket_path) {
        return -1;
    }

    /* 1. 创建 Socket
     *
     * socket() 参数说明:
     *   AF_UNIX:       地址族 = UNIX 本地通信
     *   SOCK_STREAM:   流式 Socket (类似 TCP, 可靠有序)
     *   0:             协议 = 自动选择 (UNIX Socket 只有一种)
     *
     * 返回值: 新的文件描述符, 失败返回 -1 */
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket AF_UNIX");
        return -1;
    }

    /* 2. 设置为非阻塞 (epoll 事件驱动的基础)
     *
     * 必须在 bind 之前设置, 因为:
     *   - listen 后的 accept 也需要非阻塞
     *   - 非阻塞是 fd 属性, 与 socket 状态无关 */
    if (set_nonblocking(fd) == -1) {
        close(fd);
        return -1;
    }

    /* 3. 构造 UNIX Socket 地址 */
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    /* 4. 删除可能存在的旧 Socket 文件 (关键步骤)
     *
     * 上次程序异常退出 (kill -9 / 崩溃 / 断电) 时可能残留
     * socket 文件, 不删除会导致 bind 失败 (EADDRINUSE)。
     *
     * 为什么不用 SO_REUSEADDR?
     *   SO_REUSEADDR 在 UNIX Socket 上语义不同于 TCP:
     *   它允许 bind 到一个已存在的 socket 文件，但旧连接仍存在时
     *   行为不可移植。unlink + rebind 是更可靠的做法。
     *
     * 注意: unlink 与 fork 的竞态条件:
     *   如果同一程序的两个实例同时启动, 两个都可能 unlink 然后 bind,
     *   后者覆盖前者的 socket 文件。本项目通过单实例设计 + daemonize
     *   避免此问题。 */
    unlink(socket_path);

    /* 5. bind: 将 Socket 绑定到文件路径
     *
     * bind 成功后, 文件系统中会出现 socket_path 文件:
     *   $ ls -l /tmp/car_gps.sock
     *   srwxr-xr-x 1 root root 0 Aug  1 21:00 /tmp/car_gps.sock
     *   注意文件类型是 's' (Socket), 文件大小为 0 (只作为地址入口) */
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(fd);
        return -1;
    }

    /* 6. listen: 开始监听客户端连接
     *
     * backlog: 内核维护的"已完成连接队列"最大长度
     *   (注意: Linux 2.2+ 中 backlog 是已完成队列的长度,
     *    未完成队列由 /proc/sys/net/core/somaxconn 控制)
     *
     * 对于本项目 (只有 Qt UI 一个客户端): backlog=5 足够。
     *
     * listen 后, Socket 变为"被动模式"(被动套接字):
     *   只能 accept 连接, 不能 send/recv 数据。
     *   如果调用 send/recv 会返回 ENOTSOCK 或类似错误。 */
    if (listen(fd, 5) == -1) {
        perror("listen");
        unlink(socket_path);
        close(fd);
        return -1;
    }

    LOG_INFO("socket", "Server socket created: %s (fd=%d)", socket_path, fd);

    return fd;
}

/**
 * @brief 接受一个客户端连接 (非阻塞 accept)
 *
 * 从已完成三次握手的连接队列中取出一个客户端:
 *
 * 非阻塞 accept 的三种结果:
 *   ① 返回 >=0 → 新的客户端 fd, 连接已建立
 *   ② 返回 -1 + EAGAIN/EWOULDBLOCK → 无等待连接, 正常
 *   ③ 返回 -1 + 其他 errno → 无法恢复的错误 (EMFILE/ENFILE/ENOMEM等)
 *
 * 参数2 (struct sockaddr*) 和参数3 (socklen_t*):
 *   NULL 表示不关心客户端地址信息 (对端地址),
 *   因为本项目只有本地连接, 不需要区分客户端身份。
 *
 * 新创建的 client_fd 必须设置为非阻塞:
 *   否则后续 recv/send 会阻塞整个事件循环。
 *   继承自 server_fd 的非阻塞标志不会自动传递给 client_fd,
 *   需要显式设置。
 *
 * @param server_fd  监听 socket 文件描述符
 * @return           >=0: 客户端 fd, -1: 无连接或错误
 */
int sock_accept(int server_fd)
{
    int client_fd;

    /* accept: 从"已完成连接队列"中取出一个客户端
     * 内核维护两个队列:
     *   未完成队列 (半连接): 收到 SYN 但三次握手未完成
     *   已完成队列 (全连接):   三次握手完成, 等待 accept 取出
     * accept 从已完成队列中取出队首连接 */
    client_fd = accept(server_fd, NULL, NULL);
    if (client_fd == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* 已完成队列为空: 正常情况 (非阻塞), 调用者继续 epoll 循环 */
            return -1;
        }
        if (errno == EINTR) {
            /* 被信号中断 (如 SIGCHLD): 少见但无害, 当作无连接处理 */
            return -1;
        }
        /* 不可恢复错误: EMFILE (进程fd耗尽), ENFILE (系统fd耗尽),
         * ENOMEM (内核内存不足), EBADF (server_fd 不是 socket) 等 */
        perror("accept");
        return -1;
    }

    /* 将客户端 Socket 也设为非阻塞:
     * 必须显式设置, server_fd 的非阻塞标志不会通过 accept 继承 */
    if (set_nonblocking(client_fd) == -1) {
        close(client_fd);
        return -1;
    }

    LOG_INFO("socket", "Client connected: fd=%d", client_fd);

    return client_fd;
}

int sock_connect(const char *socket_path)
{
    int fd;
    struct sockaddr_un addr;

    if (!socket_path) {
        return -1;
    }

    /* 1. 创建 Socket (与 sock_create_server 完全相同的 API) */
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket AF_UNIX");
        return -1;
    }

    /* 2. 设置为非阻塞
     *
     * 客户端也需要非阻塞: 后续通过 epoll 事件循环统一处理,
     * 避免 recv 阻塞导致整个事件循环卡住。 */
    if (set_nonblocking(fd) == -1) {
        close(fd);
        return -1;
    }

    /* 3. 构造服务端地址 (sun_path 必须与服务端 bind 的路径完全一致) */
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    /* 4. connect: 连接到服务端
     *
     * 非阻塞 connect 的三种结果:
     *   ① 返回 0       → 连接立即成功 (本地 UNIX Socket 的常见情况)
     *   ② 返回 -1 + EINPROGRESS → 连接正在建立中 (几乎只出现在 TCP)
     *                   需要用 epoll 监听 POLLOUT, 待 fd 可写后
     *                   调用 getsockopt(SO_ERROR) 确认连接成功。
     *   ③ 返回 -1 + 其他 errno → 连接失败
     *       ECONNREFUSED: 服务端未监听 (socket 文件存在但无 listen)
     *       ENOENT:       socket 文件不存在 (服务端未启动)
     *       EACCES:       socket 文件无访问权限
     *
     * 对于本项目的 UNIX Socket 场景: 由于没有网络往返延迟,
     * connect 几乎总是立即成功 (①) 或立即失败 (③),
     * EINPROGRESS 情况极其罕见, 简化处理即可。 */
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        if (errno != EINPROGRESS) {
            /* 连接立即失败: 服务端未启动/路径错误/权限问题 */
            perror("connect");
            close(fd);
            return -1;
        }
        /* EINPROGRESS: 连接进行中 (UNIX Socket 几乎不会出现此分支) */
    }

    LOG_INFO("socket", "Connected to: %s (fd=%d)", socket_path, fd);

    return fd;
}

/**
 * @brief 发送数据 (循环发送至全部发送完毕)
 *
 * 非阻塞 send 的关键行为:
 *   全部发送    → 返回 len (理想情况)
 *   部分发送    → 返回实际字节数 (< len), 内核发送缓冲区已满
 *                 上层需保存剩余数据, 等待 EPOLLOUT 事件后继续
 *   缓冲区满   → 返回 -1, errno=EAGAIN/EWOULDBLOCK
 *                 等待下次 EPOLLOUT 通知再重试
 *   对端关闭    → 返回 -1, errno=EPIPE (管道破裂)
 *                 如未设置 MSG_NOSIGNAL, 内核还会发送 SIGPIPE 信号
 *   被信号中断  → 返回 -1, errno=EINTR (继续循环重试即可)
 *
 * MSG_NOSIGNAL 标志:
 *   默认行为: 向已关闭的 socket 写入 → 内核发送 SIGPIPE,
 *            默认处理是终止进程 (如进程未捕获此信号)。
 *   MSG_NOSIGNAL: 抑制 SIGPIPE 信号, 仅返回 EPIPE 错误,
 *                 让调用者通过返回值优雅处理, 不会意外崩溃。
 *
 * 循环发送的意义:
 *   SOCK_STREAM 不保证一次 send 发送全部数据 — send 的返回值
 *   只表示"本次调用写入内核缓冲区的字节数", 与实际送达无关。
 *   如果内核缓冲区只能容纳部分数据, send 会返回部分发送量,
 *   需要循环调用直到全部数据送入内核缓冲区。
 *
 * @param fd    目标 socket 文件描述符
 * @param data  待发送数据缓冲区
 * @param len   待发送数据字节数
 * @return      >=0: 实际发送字节数, -1: 错误(EPIPE等不可恢复错误)
 */
int sock_send(int fd, const uint8_t *data, uint32_t len)
{
    uint32_t total_sent = 0;
    int      ret;

    if (fd < 0 || !data || len == 0) {
        return -1;
    }

    /* 循环发送直到所有数据发送完毕
     *
     * send 在非阻塞模式下的行为:
     *   全部发送  → 返回 len
     *   部分发送  → 返回实际发送字节数 (< len), 缓冲区已满
     *   无法发送  → 返回 -1, errno=EAGAIN (需等待 EPOLLOUT 事件)
     *   对端关闭  → 返回 -1, errno=EPIPE (收到 SIGPIPE 信号)
     *   被中断    → 返回 -1, errno=EINTR (应重试)
     *
     * 为什么需要循环?
     *   非阻塞 send 可能只发了一部分就返回 (内核缓冲区满了),
     *   需要继续发送剩余部分。每次都可能要等待下一次 EPOLLOUT。 */
    while (total_sent < len) {
        ret = send(fd, data + total_sent, len - total_sent,
                   MSG_NOSIGNAL);  /* 不发送 SIGPIPE 信号, 改为返回 EPIPE 错误 */
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 发送缓冲区满: 在 epoll 循环中应等待 EPOLLOUT 再重试
                 * 这里简单返回已发送的字节数, 由上层决定是否重试 */
                return total_sent;
            }
            if (errno == EINTR) {
                continue;  /* 被信号中断, 重试 */
            }
            /* 真正的错误 (EPIPE连接断开等) */
            perror("send");
            return -1;
        }
        total_sent += ret;
    }

    return total_sent;
}

/**
 * @brief 接收数据 (非阻塞, 单次读取)
 *
 * 非阻塞 recv 的返回值含义:
 *   ret > 0 : 成功接收 ret 字节数据
 *   ret = 0 : 对端已关闭连接 (收到 FIN, TCP 四次挥手完成)
 *             调用者应关闭 fd 并清理资源
 *   ret = -1, errno=EAGAIN/EWOULDBLOCK: 暂无数据可读, 正常返回 0 给调用者
 *   ret = -1, errno=EINTR: 被信号中断, 当作无数据处理
 *   ret = -1, 其他 errno: 真正的 IO 错误, 应关闭连接
 *
 * 注意: 本函数不同于 sock_send — 不做循环读取。
 *   sock_send 循环是因为"已送入内核"不等于"已发送" (需等待),
 *   sock_recv 不循环是因为 recv 返回的数据已经在内核缓冲区中,
 *   一次 recv 返回多少就处理多少, 上层通过 ringbuffer 拼接完整帧。
 *
 * @param fd    源 socket 文件描述符
 * @param buf   接收缓冲区 (调用者分配)
 * @param size  缓冲区大小
 * @return      >0: 接收字节数, 0: 暂无数据或对端关闭, -1: 错误
 */
int sock_recv(int fd, uint8_t *buf, uint32_t size)
{
    int ret;

    if (fd < 0 || !buf || size == 0) {
        return -1;
    }

    /* recv 在非阻塞模式下的行为:
     *   有数据   → 返回接收到的字节数 (1~size)
     *   无数据   → 返回 -1, errno=EAGAIN
     *   对端关闭 → 返回 0 (优雅关闭, 收到 FIN)
     *   被中断   → 返回 -1, errno=EINTR */
    ret = recv(fd, buf, size, 0);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  /* 无数据, 正常 */
        }
        if (errno == EINTR) {
            return 0;  /* 信号中断, 当作无数据处理 */
        }
        perror("recv");
        return -1;
    }

    return ret;  /* ret=0 表示对端关闭, ret>0 表示收到字节数 */
}

int sock_send_frame(int fd, uint8_t msg_type,
                    const uint8_t *data, uint32_t data_len)
{
    uint8_t  frame[PROTO_MAX_FRAME];
    uint32_t frame_len;
    int      ret;

    /* 组帧 */
    ret = protocol_pack(msg_type, data, data_len,
                        frame, sizeof(frame), &frame_len);
    if (ret != 0) {
        LOG_ERROR("socket", "protocol_pack failed: ret=%d", ret);
        return -1;
    }

    /* 发送 */
    ret = sock_send(fd, frame, frame_len);
    if ((uint32_t)ret != frame_len) {
        LOG_ERROR("socket", "sock_send incomplete: %d/%u", ret, frame_len);
        return -1;
    }

    return 0;
}

void sock_close(int fd, const char *socket_path)
{
    if (fd >= 0) {
        close(fd);
    }

    /* 服务端关闭时清理 socket 文件:
     *
     * close(fd) 只释放内核中的 socket 数据结构,
     * 不会自动删除文件系统中的 socket 文件。
     * 如果不 unlink, 下次启动时 sock_create_server 中的
     * unlink+bind 组合会处理 — 但显式清理是好习惯。
     *
     * 为什么另一次 close+unlink 存在竞态?
     *   正常的操作顺序应该是先 unlink 再 close,
     *   避免 close 后 unlink 前有另一个进程 bind 了同名路径。
     *   本项目是单实例守护进程, 竞态不存在, 顺序无所谓。
     *
     * @param fd          socket 文件描述符
     * @param socket_path 服务端 socket 路径 (客户端为 NULL)
     */
    if (socket_path) {
        unlink(socket_path);
        LOG_INFO("socket", "Socket closed: %s", socket_path);
    }
}
