/**
 * @file    can_driver.h
 * @brief   SocketCAN 驱动封装 - 头文件
 *
 * SocketCAN 是 Linux 2.6.25+ 引入的 CAN 总线协议栈,
 * 将 CAN 控制器驱动与网络协议栈结合, 提供标准的 Socket API。
 *
 * 与 tty/串口 API 的对比:
 *   串口:   open("/dev/ttyUSB0") + termios 配置
 *   CAN:    socket(PF_CAN, SOCK_RAW, CAN_RAW) + bind + ioctl 配置
 *   SocketCAN 的优势: 统一 Socket 接口, 支持 select/epoll, 支持多应用共享
 *
 * CAN 帧结构 (struct can_frame):
 *
 *   struct can_frame {
 *       canid_t can_id;   // 32位 CAN ID
 *                          //   bit 0-28:  CAN ID (11位标准 或 29位扩展)
 *                          //   bit 29:    错误帧标志 (CAN_ERR_FLAG)
 *                          //   bit 30:    远程帧标志 (CAN_RTR_FLAG)
 *                          //   bit 31:    扩展帧标志 (CAN_EFF_FLAG)
 *       __u8    can_dlc;  // 数据长度 (0~8)
 *       __u8    data[8];  // 数据字节
 *   };
 *
 * CAN ID 编码规则:
 *   标准帧 (11-bit): can_id = 0x000 ~ 0x7FF
 *     发送时直接使用此值
 *   扩展帧 (29-bit): can_id = 0x00000000 ~ 0x1FFFFFFF | CAN_EFF_FLAG
 *     发送时必须 或上 CAN_EFF_FLAG (0x80000000)
 *
 * CAN 错误帧:
 *   当总线发生错误时, 内核自动生成 error frame:
 *     can_id  & CAN_ERR_FLAG → 是错误帧
 *     can_id  & CAN_ERR_MASK → 错误类型
 *     data[0] & CAN_ERR_PROT_MASK → 协议错误细节
 *     data[6-7]                  → TEC/REC 错误计数器
 *
 * CAN 总线状态管理:
 *   通过 CAN_ERR_CRTL 类型的错误帧监控:
 *     CAN_ERR_CRTL_ACTIVE   → 总线正常 (主动错误)
 *     CAN_ERR_CRTL_WARNING  → 错误警告 (TEC/REC > 96)
 *     CAN_ERR_CRTL_PASSIVE  → 错误被动 (TEC/REC > 127)
 *     CAN_ERR_CRTL_BUS_OFF  → 总线关闭 (TEC > 255)
 *   总线关闭后需要手动恢复: ip link set can0 up type can ...
 *
 * 学习要点:
 *   1. PF_CAN / AF_CAN: CAN 协议族
 *   2. struct can_frame: CAN 帧的内核表示
 *   3. CAN ID 标志位: EFF/RTR/ERR 的位操作
 *   4. CAN 错误帧: 通过 socket 接收, 无需单独 API
 *   5. if_nametoindex: 网口名称 → 内核接口索引
 */

#ifndef _CAN_DRIVER_H_
#define _CAN_DRIVER_H_

#include "canbus_daemon.h"
#include <stdint.h>
#include <linux/can.h>      /* struct can_frame, CAN_RAW, SOL_CAN_* */

/* ================================================================
 *  API 函数声明
 * ================================================================ */

/**
 * @brief 创建并绑定 CAN Socket (非阻塞)
 *
 * 执行步骤:
 *   1. socket(PF_CAN, SOCK_RAW, CAN_RAW)
 *      - PF_CAN:     CAN 协议族
 *      - SOCK_RAW:   原始套接字 (直接操作 CAN 帧)
 *      - CAN_RAW:    原始 CAN 协议 (另有 CAN_BCM 广播管理器)
 *
 *   2. fcntl O_NONBLOCK: 设为非阻塞 (融入 epoll)
 *
 *   3. ioctl SIOCGIFINDEX: 获取接口索引 (if_nametoindex 也可)
 *
 *   4. bind(fd, &addr, sizeof(addr))
 *      - addr.can_family = AF_CAN
 *      - addr.can_ifindex = can0 的接口索引
 *
 *   5. 设置 CAN 错误帧过滤 (CAN_RAW_RECV_OWN_MSGS 等可选)
 *
 * @param ifname    CAN 接口名 (如 "can0")
 * @param ifindex   输出: 接口索引
 * @return          成功: socket fd, 失败: -1
 *
 * 注意: 此函数假设 can0 已由启动脚本配置好:
 *   ip link set can0 type can bitrate 500000
 *   ip link set can0 up
 * 如果 can0 未配置, bind 会失败。
 */
int can_socket_open(const char *ifname, int *ifindex);

/**
 * @brief 发送 CAN 报文
 *
 * 通过 write() 系统调用发送 struct can_frame。
 * 等价于 sendto(), 因为 Socket 已 bind 到特定接口。
 *
 * @param fd      CAN socket fd
 * @param msg     CAN 报文 (can_id / can_dlc / data / is_extended / is_remote)
 * @return        >0=发送字节数, -1=失败
 */
int can_send(int fd, const can_msg_t *msg);

/**
 * @brief 接收 CAN 报文 (非阻塞)
 *
 * 通过 read() 系统调用接收 struct can_frame。
 * 成功时转换为 can_msg_t 格式。
 *
 * @param fd      CAN socket fd
 * @param msg     输出: 接收到的 CAN 报文
 * @return        >0=接收字节数, 0=无数据, -1=错误/错误帧, -2=错误帧
 *
 * 返回值 -2 表示收到了错误帧 (can_id & CAN_ERR_FLAG),
 * 调用者应通过 can_parse_error_frame() 进一步处理。
 */
int can_recv(int fd, can_msg_t *msg);

/**
 * @brief 解析 CAN 错误帧, 提取错误信息
 *
 * CAN 错误帧的 can_id 编码:
 *   CAN_ERR_TX_TIMEOUT    — 发送超时
 *   CAN_ERR_BUSOFF        — 总线关闭
 *   CAN_ERR_BUSERROR      — 总线错误
 *   CAN_ERR_CRTL          — 控制器状态变化
 *
 * data[6] = TEC (发送错误计数)
 * data[7] = REC (接收错误计数)
 *
 * @param frame       原始 CAN 帧
 * @param error_info  输出: 解析后的错误信息
 */
void can_parse_error_frame(const struct can_frame *frame,
                           can_error_info_t *error_info);

/**
 * @brief 关闭 CAN Socket
 * @param fd  CAN socket fd
 */
void can_socket_close(int fd);

#endif /* _CAN_DRIVER_H_ */
