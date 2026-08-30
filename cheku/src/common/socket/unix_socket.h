/**
 * @file    unix_socket.h
 * @brief   UNIX Domain Socket 封装 - 头文件
 *
 * UNIX Domain Socket (AF_UNIX) 是 Linux 下最高效的本地 IPC 方式之一:
 *   - 不需要经过网络协议栈 (无 TCP/IP 开销)
 *   - 使用文件系统路径作为地址 (如 /tmp/car_gps.sock)
 *   - 支持流式 (SOCK_STREAM, 类似 TCP) 和数据报 (SOCK_DGRAM, 类似 UDP)
 *   - 本模块使用 SOCK_STREAM (可靠、有序、双向)
 *
 * 项目中的角色:
 *   后台进程 = 服务端: 创建 Socket, bind, listen, accept 客户端连接
 *   Qt UI   = 客户端:  连接各个后台进程的 Socket, 收发数据
 *
 * 本模块封装了服务端和客户端两种模式, 简化各进程的 Socket 代码。
 *
 * 使用示例 (服务端, 在 gps_daemon 中):
 *   int sock_fd = sock_create_server("/tmp/car_gps.sock");
 *   // 将 sock_fd 加入 epoll 监听 (EPOLLIN 事件)
 *   int client_fd = sock_accept(sock_fd);
 *
 * 使用示例 (客户端, 在 Qt UI 中使用 QLocalSocket, 不用此模块):
 *   作为 C 语言测试工具:
 *   int fd = sock_connect("/tmp/car_gps.sock");
 *   sock_send_frame(fd, MSG_GPS_QUERY, NULL, 0);
 *
 * 学习要点:
 *   1. UNIX Domain Socket 与 TCP Socket 的异同 (API 完全一样!)
 *   2. sockaddr_un 结构体与文件系统路径
 *   3. 非阻塞模式: fcntl O_NONBLOCK
 *   4. SO_REUSEADDR: 避免 bind "Address already in use" 错误
 *   5. 服务端 listen/accept 模型
 */

#ifndef _UNIX_SOCKET_H_
#define _UNIX_SOCKET_H_

#include <stdint.h>
#include <stddef.h>

/**
 * @brief 创建 UNIX Socket 服务端
 *
 * 执行步骤:
 *   1. socket(AF_UNIX, SOCK_STREAM, 0) 创建 Socket
 *   2. fcntl O_NONBLOCK 设为非阻塞
 *   3. unlink 删除可能存在的旧 Socket 文件
 *   4. bind 绑定到指定路径
 *   5. listen 开始监听
 *
 * @param socket_path  Socket 文件路径 (如 "/tmp/car_gps.sock")
 * @return             成功: Socket fd (>=0), 失败: -1
 *
 * 注意: Socket 文件会创建在文件系统中。
 *       进程退出时应调用 sock_close() 清理 Socket 文件。
 */
int sock_create_server(const char *socket_path);

/**
 * @brief 接受客户端连接 (非阻塞)
 *
 * 如果没有等待的连接, 立即返回 -1 (errno = EAGAIN/EWOULDBLOCK)。
 * 这就是非阻塞 accept 的行为。
 *
 * @param server_fd  服务端 Socket fd
 * @return           成功: 客户端 fd, 失败: -1 (检查 errno)
 */
int sock_accept(int server_fd);

/**
 * @brief 连接 UNIX Socket 服务端 (客户端模式)
 *
 * 执行步骤:
 *   1. socket(AF_UNIX, SOCK_STREAM, 0)
 *   2. fcntl O_NONBLOCK 设为非阻塞
 *   3. connect 连接服务端
 *
 * @param socket_path  服务端 Socket 路径
 * @return             成功: Socket fd, 失败: -1
 *
 * 注意: 非阻塞 connect 可能返回 -1 且 errno=EINPROGRESS。
 *       这不代表失败, 而是"连接正在进行中",
 *       需要用 epoll 监听 EPOLLOUT 事件来确认连接成功。
 *       为简化使用, 本模块内部用阻塞 connect (但设了超时)。
 */
int sock_connect(const char *socket_path);

/**
 * @brief 发送数据 (自动处理部分发送)
 *
 * 在非阻塞 Socket 上, send 可能只发送了部分数据。
 * 本函数循环调用 send 直到所有数据发送完毕或出错。
 *
 * @param fd    Socket fd
 * @param data  数据
 * @param len   数据长度
 * @return      成功: 发送的字节数 (= len), 失败: -1
 */
int sock_send(int fd, const uint8_t *data, uint32_t len);

/**
 * @brief 接收数据 (非阻塞, 有多少读多少)
 *
 * @param fd    Socket fd
 * @param buf   接收缓冲区
 * @param size  缓冲区大小
 * @return      成功: 收到的字节数 (>0), 0=对端关闭, -1=错误 (检查 errno)
 */
int sock_recv(int fd, uint8_t *buf, uint32_t size);

/**
 * @brief 通过 Socket 发送一帧协议数据 (便捷函数)
 *
 * 内部调用 protocol_pack 组帧 + sock_send 发送。
 * 这是最常用的调用方式。
 *
 * @param fd        Socket fd
 * @param msg_type  消息类型
 * @param data      数据
 * @param data_len  数据长度
 * @return          0=成功, -1=失败
 */
int sock_send_frame(int fd, uint8_t msg_type,
                    const uint8_t *data, uint32_t data_len);

/**
 * @brief 关闭 Socket 并清理 Socket 文件
 *
 * @param fd          Socket fd
 * @param socket_path Socket 文件路径 (服务端传入, 客户端传 NULL)
 */
void sock_close(int fd, const char *socket_path);

#endif /* _UNIX_SOCKET_H_ */
