/**
 * @file    canbus_daemon.h
 * @brief   CAN 总线通信守护进程 - 配置参数 & 数据结构
 *
 * 功能概述:
 *   - 初始化 CAN 接口 (can0), 设置 500kbps 波特率
 *   - 收发 CAN 标准帧/扩展帧 (使用 Linux SocketCAN)
 *   - CAN 报文环形缓冲区 (最近 200 条)
 *   - UNIX Socket 推送给 UI, 接收 UI 的 CAN 发送指令
 *   - CAN 总线错误处理 (被动错误/总线关闭恢复)
 *
 * 硬件平台 (从设备树 100ask_imx6ull-14x14.dts):
 *   &flexcan1 {
 *       pinctrl-0 = <&pinctrl_flexcan1>;
 *       xceiver-supply = <&reg_can_3v3>;  // 3.3V CAN 收发器
 *       status = "okay";
 *   };
 *
 *   引脚映射:
 *     CAN1_TX → UART3_CTS_B (MX6UL_PAD_UART3_CTS_B__FLEXCAN1_TX)
 *     CAN1_RX → UART3_RTS_B (MX6UL_PAD_UART3_RTS_B__FLEXCAN1_RX)
 *
 * SocketCAN 简介:
 *   Linux 内核原生 CAN 协议栈, 将 CAN 总线抽象为网络接口 (如 can0)。
 *   应用层通过标准 Socket API 操作 CAN 设备:
 *     socket(PF_CAN, SOCK_RAW, CAN_RAW) → 创建 CAN 原始套接字
 *     bind(fd, &addr, sizeof(addr))    → 绑定到指定 CAN 接口
 *     read/write 或 sendto/recvfrom    → 收发 struct can_frame
 *
 * 学习要点:
 *   1. SocketCAN: Linux CAN 子系统的应用层接口
 *   2. struct can_frame: CAN 标准帧/扩展帧的内核表示
 *   3. CAN 错误帧与总线状态: 通过 error frame 监控总线健康
 *   4. 环形缓冲区: CAN 报文高速缓存 (200条, ~3KB)
 */

#ifndef _CANBUS_DAEMON_H_
#define _CANBUS_DAEMON_H_

#include "common.h"         /* can_msg_t, ret_code_t, proc_config_t, ... */
#include "protocol/protocol.h"   /* ringbuffer_t, protocol_parser_t */
#include <stdint.h>
#include <time.h>
#include <linux/can.h>      /* struct can_frame, CAN_RAW, CAN_EFF_FLAG, ... */
#include <linux/can/error.h>/* CAN_ERR_*, can_err_cntr_t, ... */

/* ================================================================
 *  默认配置
 * ================================================================ */

/* ---- CAN 接口 ---- */
#define CAN_DEFAULT_INTERFACE   "can0"
#define CAN_DEFAULT_BITRATE     500000      /* 500 kbps */

/* ---- 环形缓冲区 ---- */
#define CAN_RING_SIZE           200         /* 缓存最近 200 条报文 */
#define CAN_RING_OVERWRITE      1           /* 满时覆盖旧数据 */

/* ---- Socket ---- */
#define CAN_SOCKET_PATH         "/tmp/car_can.sock"

/* ---- 日志 ---- */
#define CAN_LOG_PATH            "/var/log/car_terminal/canbus.log"

/* ================================================================
 *  CAN 总线状态
 * ================================================================ */
typedef enum {
    CAN_BUS_OK          = 0,    /* 总线正常 */
    CAN_BUS_WARNING     = 1,    /* 错误警告 (TEC/REC > 96) */
    CAN_BUS_PASSIVE     = 2,    /* 错误被动 (TEC/REC > 127) */
    CAN_BUS_OFF         = 3,    /* 总线关闭 (TEC > 255) */
} can_bus_state_t;

/**
 * @brief CAN 报文环形缓冲区条目
 *
 * 每条记录包含 can_msg_t (ID/DLC/数据/标志) 和时间戳。
 * can_msg_t 在 common.h 中定义 (can_id, can_dlc, data[8], is_extended, is_remote)。
 */
typedef struct {
    can_msg_t   msg;            /* CAN 报文 (与 common.h 保持一致) */
    time_t      timestamp;      /* 接收/发送时间 */
    uint8_t     direction;      /* 方向: 0=接收, 1=发送 */
} can_ring_entry_t;

/**
 * @brief CAN 报文环形缓冲区
 *
 * 固定大小的 FIFO 环形缓冲, 缓存最近 CAN_RING_SIZE 条报文。
 * 用于 UI 历史查询和调试。
 */
typedef struct {
    can_ring_entry_t *buffer;   /* 数据缓冲区 (动态分配) */
    int             capacity;   /* 最大容量 (CAN_RING_SIZE) */
    int             head;       /* 写入位置 */
    int             count;      /* 当前条目数 (≤capacity) */
} can_ring_t;

/**
 * @brief CAN 接口错误计数器 (从 error frame 提取)
 *
 * TEC (Transmit Error Counter) / REC (Receive Error Counter):
 *   通过 CAN_ERR_CNT 类型的 error frame 获取。
 *   内核在 CAN 状态变化时自动发送此帧。
 */
typedef struct {
    uint8_t     tec;            /* 发送错误计数 */
    uint8_t     rec;            /* 接收错误计数 */
    can_bus_state_t state;      /* 当前总线状态 */
} can_error_info_t;

/* ================================================================
 *  进程上下文
 * ================================================================ */
typedef struct {
    /* CAN */
    char        can_ifname[32];     /* CAN 接口名 (如 "can0") */
    int         can_fd;             /* CAN socket fd */
    int         can_ifindex;        /* CAN 接口索引 */
    /* 环形缓冲 */
    can_ring_t  ring;               /* CAN 报文环形缓冲 */
    /* 错误状态 */
    can_error_info_t error_info;    /* 当前错误计数 */
    /* 统计 */
    uint32_t    rx_count;           /* 接收报文计数 */
    uint32_t    tx_count;           /* 发送报文计数 */
    uint32_t    err_count;          /* 错误帧计数 */
    /* 配置 */
    proc_config_t proc_cfg;
    /* 运行 */
    int         running;
    int         epoll_fd;
    int         timer_fd;           /* 状态上报定时器 */
    int         signal_fd;
    int         server_fd;
    int         client_fd;
    protocol_parser_t proto_parser;
    ringbuffer_t recv_rb;
} canbus_context_t;

/* ================================================================
 *  函数声明
 * ================================================================ */

/**
 * @brief 初始化 CAN 报文环形缓冲区
 * @param ring      环形缓冲区实例
 * @param capacity  容量 (条)
 * @return          RET_OK 成功, RET_ERR_MEM 内存不足
 */
int can_ring_init(can_ring_t *ring, int capacity);

/**
 * @brief 向环形缓冲区写入一条 CAN 报文
 * @param ring      环形缓冲区
 * @param msg       CAN 报文
 * @param direction 方向 (0=RX, 1=TX)
 */
void can_ring_push(can_ring_t *ring, const can_msg_t *msg, int direction);

/**
 * @brief 获取环形缓冲区中第 N 条记录 (0=最新)
 * @param ring  环形缓冲区
 * @param index 索引 (0=最新, 1=上一条, ...)
 * @return      记录指针, NULL=超出范围
 */
const can_ring_entry_t* can_ring_get(const can_ring_t *ring, int index);

/**
 * @brief 获取环形缓冲区当前条目数
 * @param ring  环形缓冲区
 * @return      条目数
 */
int can_ring_count(const can_ring_t *ring);

/**
 * @brief 释放环形缓冲区
 * @param ring  环形缓冲区
 */
void can_ring_destroy(can_ring_t *ring);

/**
 * @brief 加载 CAN 配置 (从 config.ini [canbus] 节)
 * @param ctx       进程上下文
 * @param conf_file 配置文件路径
 * @return          RET_OK 成功
 */
int canbus_config_load(canbus_context_t *ctx, const char *conf_file);

#endif /* _CANBUS_DAEMON_H_ */
