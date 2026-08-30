/**
 * @file    protocol.h
 * @brief   IPC 通信协议帧处理 - 头文件
 *
 * 消息帧格式 (7 字节帧开销 + 可变数据):
 *
 *   ┌────────┬────────┬────────┬──────────┬───────────┬────────┬────────┐
 *   │ HEAD0  │ HEAD1  │  TYPE  │   LEN    │   DATA    │  CRC8  │  TAIL  │
 *   │  0xAA  │  0x55  │ 1 Byte │ 2 Bytes  │ 0~65535 B │ 1 Byte │  0x55  │
 *   └────────┴────────┴────────┴──────────┴───────────┴────────┴────────┘
 *     偏移0    偏移1    偏移2     偏移3-4    偏移5..     偏移-2   偏移-1
 *
 * LEN 使用大端序，直接构造/还原，不使用 htons/ntohs:
 *   大端序 = 高位字节在低地址，与人类书写习惯一致
 *   例如: 256 = 0x0100, 存储为 [0x01][0x00] (高字节在前)
 *   构造: LEN_H = (data_len >> 8) & 0xFF, LEN_L = data_len & 0xFF
 *   还原: data_len = (LEN_H << 8) | LEN_L
 *
 * 为什么用大端序?
 *   1. 直观: 在 hexdump 中直接可读 (0x0100 显示为 01 00)
 *   2. 便于以后扩展为网络通信 (网络字节序即大端序)
 *
 * 为什么不用 htons/ntohs?
 *   1. htons 在 ARM LE 下将值转换为网络字节序后返回的 uint16_t
 *      内部表示仍受主机字节序影响: htons(10) 的内存值为 0x0A00,
 *      用 >>8 提取"高字节"得到的是 0x0A 而非期望的 0x00,
 *      导致帧中长度被错误编码为 [0x0A][0x00]=2560 而非 [0x00][0x0A]=10。
 *   2. 直接位运算构造/还原大端序完全不受主机字节序影响,
 *      Qt 和 C 两端行为一致 (位移运算符按数学定义工作)。
 *
 * 使用示例 (发送方):
 *   // 1. 准备数据
 *   gps_data_t gps = {...};
 *   // 2. 组帧
 *   uint8_t frame[512];
 *   uint32_t frame_len;
 *   protocol_pack(MSG_GPS_DATA, (uint8_t*)&gps, sizeof(gps),
 *                 frame, sizeof(frame), &frame_len);
 *   // 3. 发送
 *   send(sockfd, frame, frame_len, 0);
 *
 * 使用示例 (接收方):
 *   // 每个连接独立一个解析器
 *   protocol_parser_t parser;
 *   protocol_parser_init(&parser);
 *   // 1. 接收原始数据到 ringbuffer
 *   ringbuffer_write(&rb, recv_data, recv_len);
 *   // 2. 逐帧解析
 *   uint8_t msg_type, data[256];
 *   uint32_t data_len;
 *   while (protocol_unpack(&parser, &rb, &msg_type, data, sizeof(data), &data_len) == 0) {
 *       // 3. 根据 msg_type 处理 data
 *   }
 *
 * 学习要点:
 *   1. 二进制协议设计: 帧头/帧长/CRC/帧尾
 *   2. 大端序编码与位运算: 直接位运算构造/还原多字节值, 避免 htons/ntohs
 *      在不同主机字节序平台上的歧义 (详见 LEN 编码说明)
 *   3. 状态机实现帧解析 (同步/搜索帧头)
 *   4. 粘包/断包处理 (TCP流式传输的经典问题)
 */

#ifndef _PROTOCOL_H_
#define _PROTOCOL_H_

#include <stdint.h>
#include <stddef.h>
#include "../common.h"              /* MSG_FRAME_HEAD0/HEAD1/TAIL 等公共常量 */
#include "../ringbuffer/ringbuffer.h"  /* ringbuffer_t */

/* ================================================================
 *  帧格式常量
 *  与 common.h 中的定义保持一致 (不重复定义, 直接引用)
 * ================================================================ */

/* 帧定界符 — 引用 common.h 公共定义, 保持全项目统一 */
#define PROTO_HEAD0         MSG_FRAME_HEAD0  /* 帧头字节0 0xAA */
#define PROTO_HEAD1         MSG_FRAME_HEAD1  /* 帧头字节1 0x55 */
#define PROTO_TAIL          MSG_FRAME_TAIL   /* 帧尾字节 0x55 */

/* 帧各部分偏移 */
#define PROTO_OFF_HEAD0     0       /* HEAD0 偏移 */
#define PROTO_OFF_HEAD1     1       /* HEAD1 偏移 */
#define PROTO_OFF_TYPE      2       /* TYPE 偏移 */
#define PROTO_OFF_LEN_H     3       /* LEN 高字节偏移 */
#define PROTO_OFF_LEN_L     4       /* LEN 低字节偏移 */
#define PROTO_OFF_DATA      5       /* DATA 起始偏移 */

/* 帧开销和限制 */
#define PROTO_OVERHEAD      7       /* 帧开销: HEAD(2) + TYPE(1) + LEN(2) + CRC(1) + TAIL(1) */
#define PROTO_MAX_DATA      65535   /* DATA 最大长度 */
#define PROTO_MAX_FRAME     (PROTO_MAX_DATA + PROTO_OVERHEAD) /* 帧最大长度 */

/**
 * @brief 协议解析状态 (内部状态机)
 *
 * 接收方需要从连续的字节流中找到帧边界。
 * 状态机跟踪当前解析进度。
 *
 * 状态转换图:
 *
 *    +-------+   收到0xAA    +----------+
 *    | IDLE  | ───────────→ | HEAD1    |
 *    +-------+              +----------+
 *       ↑  CRC错│                 │ 收到0x55
 *       │ 或帧  │                 ↓
 *       │ 尾错  │            +-----------+   数据不足(-2)
 *       │       │            | HEADER    | ──→ 返回,等待epoll
 *       │       │            +-----------+
 *       │       │                 │ 收到3字节
 *       │       │ 长度非法        ↓
 *       │       ├──(回IDLE)── +--------+   数据不足(-2)
 *       │       │             | DATA   | ──→ 返回,等待epoll
 *       │       │             +--------+
 *       │       │                 │ 收到LEN+CRC字节
 *       │       │                 ↓
 *       │       │            +----------+
 *       │       └─────────── | TAIL     |
 *       │          帧尾错    +----------+
 *       │                       │ 收到0x55
 *       │                       ↓
 *       └────────── 输出帧, total_frames++, 回到 IDLE
 */
typedef enum {
    PROTO_SYNC_IDLE,        /* 空闲: 等待 HEAD0 (0xAA) */
    PROTO_SYNC_HEAD1,       /* 已收到 HEAD0: 等待 HEAD1 (0x55) */
    PROTO_SYNC_HEADER,      /* 已收到帧头: 等待 TYPE(1B) + LEN(2B) = 3字节 */
    PROTO_SYNC_DATA,        /* 已收到头部: 等待 DATA(LEN字节) + CRC(1B) */
    PROTO_SYNC_TAIL,        /* 已收到数据+CRC: 等待 TAIL (0x55) */
} protocol_sync_state_t;

/**
 * @brief 协议解析器实例 (每个连接独立维护)
 *
 * 设计要点:
 *   qt_ui 同时连接 7 个后台进程, 每个连接需要独立的解析状态,
 *   否则不同 Socket 的数据会相互干扰导致帧同步错乱。
 *
 *   后台守护进程虽然只服务一个 client, 但也使用独立实例,
 *   保持代码一致性和可测试性。
 *
 * 用法:
 *   protocol_parser_t parser;
 *   protocol_parser_init(&parser);
 *   // 在 epoll 事件循环中:
 *   protocol_unpack(&parser, &rb, &type, data, sizeof(data), &len);
 */
typedef struct protocol_parser {
    protocol_sync_state_t state;    /* 当前状态 */
    uint8_t  msg_type;              /* 当前帧的消息类型 (HEADER 状态解析得到) */
    uint16_t data_len;              /* 当前帧期望的数据负载长度 (不含CRC, 从LEN字段还原) */
    uint16_t data_received;         /* 已收到的数据字节数 (用于断包续传进度跟踪) */
    uint8_t  header_buf[3];         /* 头部缓冲: TYPE(1) + LEN(2) */
    int      header_received;       /* 已收到的头部字节数 */
    uint32_t total_frames;          /* 统计: 共解析帧数 */
    uint32_t crc_errors;            /* 统计: CRC错误数 */
} protocol_parser_t;

/* ================================================================
 *  API 函数声明
 * ================================================================ */

/**
 * @brief 初始化协议解析器实例
 *
 * 在使用 protocol_unpack() 之前必须调用此函数初始化解析器。
 * 将所有字段清零, 状态机设为 IDLE。
 *
 * 每个 Socket 连接需要独立的 protocol_parser_t 实例,
 * 因为每个连接的数据流是独立的, 共享实例会导致帧同步错乱。
 *
 * 实例生命周期管理:
 *   - 可在栈上分配 (适用于短生命周期的连接处理函数)
 *   - 也可嵌入到各进程的 context/connection 结构体中 (推荐)
 *   - 连接关闭时无需特殊清理 (无动态分配的资源)
 *
 * @param parser  解析器实例指针 (调用者分配, 传 NULL 则安全返回)
 *
 * 使用示例:
 *   protocol_parser_t parser;
 *   protocol_parser_init(&parser);
 */
void protocol_parser_init(protocol_parser_t *parser);

/**
 * @brief 组帧: 将数据封装为完整消息帧
 *
 * @param msg_type      消息类型 (如 MSG_GPS_DATA)
 * @param data          要发送的数据
 * @param data_len      数据长度 (0~65535)
 * @param frame_out     输出帧缓冲区 (调用者分配)
 * @param frame_buf_size 帧缓冲区大小
 * @param frame_len_out 输出: 实际帧长度
 * @return              0=成功, -1=参数错误, -2=缓冲区不足
 *
 * 生成的帧结构:
 *   [0xAA][0x55][TYPE][LEN_H][LEN_L][DATA...][CRC8][0x55]
 *
 * CRC8 计算范围: TYPE + LEN_H + LEN_L + DATA (不含帧头/帧尾)
 */
int protocol_pack(uint8_t msg_type,
                  const uint8_t *data, uint32_t data_len,
                  uint8_t *frame_out, uint32_t frame_buf_size,
                  uint32_t *frame_len_out);

/**
 * @brief 拆帧: 从环形缓冲区中逐帧解析消息
 *
 * 使用 5 状态有限状态机 (FSM) 从连续字节流中找出帧边界并提取数据:
 *   IDLE → HEAD1 → HEADER → DATA → TAIL → (输出帧, 回到 IDLE)
 *
 * 非阻塞设计: 如果没有足够数据完成一帧，返回 -2 并保留解析进度。
 * 调用者需在 epoll 事件循环中反复调用，直到 ringbuffer 中的数据被全部消费。
 *
 * 粘包/断包处理:
 *   - 粘包: ringbuffer 缓存所有接收数据, while 循环逐帧解析
 *   - 断包: 状态机记住当前进度 (state/data_received), 下次调用时继续
 *
 * LEN 字段的大端序还原不使用 ntohs, 原因与 protocol_pack 相同:
 *   直接通过位运算还原: data_len = (high_byte << 8) | low_byte
 *   避免 ntohs 在 ARM LE 等平台上的歧义。
 *
 * @param parser        解析器实例指针 (每个连接独立, 调用者分配)
 * @param rb            环形缓冲区 (包含从 Socket 接收到的原始字节流)
 * @param msg_type_out  输出: 解析出的消息类型 (如 MSG_GPS_DATA)
 * @param data_out      输出: 提取出的消息负载数据
 * @param data_buf_size data_out 缓冲区大小 (最大可接收的数据长度)
 * @param data_len_out  输出: 实际数据长度 (data_out 中有效字节数)
 * @return              0 = 成功解析一帧 (msg_type_out/data_out/data_len_out 有效)
 *                      -1 = 参数错误 (空指针等)
 *                      -2 = 数据不足 (不是错误, 等待 Socket 下次可读事件)
 *                      -4 = 帧格式错误 (帧尾不匹配, 当前帧已丢弃)
 *
 * 调用模式 (epoll 事件循环中):
 *   protocol_parser_t parser;
 *   protocol_parser_init(&parser);
 *   // Socket 有数据到达
 *   ringbuffer_write(&rb, recv_buf, n);
 *   // 循环解析所有完整帧
 *   while (protocol_unpack(&parser, &rb, &type, buf, sizeof(buf), &len) == 0) {
 *       handle_message(type, buf, len);
 *   }
 *   // -2 表示数据不够，等待下次 epoll 事件
 */
int protocol_unpack(protocol_parser_t *parser, ringbuffer_t *rb,
                    uint8_t *msg_type_out,
                    uint8_t *data_out, uint32_t data_buf_size,
                    uint32_t *data_len_out);

/**
 * @brief 重置解析器状态机
 *
 * 当检测到通信异常 (如连续 CRC 错误) 时调用，
 * 状态机回到 IDLE 状态，可选择同时清空 ringbuffer。
 *
 * @param parser  解析器实例
 * @param rb      环形缓冲区 (传 NULL 则只重置解析器, 不清缓冲区)
 */
void protocol_reset(protocol_parser_t *parser, ringbuffer_t *rb);

#endif /* _PROTOCOL_H_ */
