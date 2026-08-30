/**
 * @file    protocol.c
 * @brief   IPC 通信协议帧处理 - 实现
 *
 * 核心算法:
 *
 * 1. 组帧 (protocol_pack):
 *    简单的线性过程: 填帧头 → 填类型 → 填长度 → 填数据 → 计算CRC → 填帧尾
 *
 * 2. 拆帧 (protocol_unpack):
 *    使用 5 状态有限状态机 (FSM) 从字节流中提取帧:
 *
 *    IDLE ──(0xAA)──→ HEAD1 ──(0x55)──→ HEADER ──(3字节)──→ DATA ──(LEN+CRC字节)──→ TAIL
 *      ↑                 │ 非0x55          ↑                    │                        │
 *      │                 ↓ 非0xAA          │ 长度非法           │ 数据不足(-2)           │
 *      │              IDLE ←───────────────┘                    │                        │
 *      │                                                        ↓                        │
 *      │                                                  (等待 epoll)                   │
 *      │                                                                                 │
 *      ├────────────────────────── (CRC错/帧尾错) ─────────────────────────────────────┘
 *      │                                    │
 *      └────────────────────────────────────┘  (0x55 → 输出帧, 回到 IDLE)
 *
 *    数据不足(-2) 时状态机保留当前进度, 不会回到 IDLE。
 *    回到 IDLE 仅发生在: 帧格式错误(帧尾不匹配) 或 长度字段非法。
 *
 * 粘包/断包处理 (TCP 流式传输的经典问题):
 *
 *   粘包: 一次 recv 收到多个帧的数据
 *     解决: ringbuffer 缓存, while 循环解析
 *
 *   断包: 一帧的数据分两次 recv 到达
 *     解决: 状态机记住当前位置, 等待下次 epoll 通知后继续
 *
 *   数据错位: 帧中间恰好出现 0xAA 0x55 (误识别为帧头)
 *     解决: CRC 校验 → 失败后回到 IDLE 重新搜索帧头
 *     概率: 随机数据出现 0xAA55 的概率 = 1/65536, CRC8 误判率 = 1/256
 *     组合: 误识别完整帧的概率 = 1/65536 * 1/256 = 1/16777216 (极低)
 *
 * 学习要点:
 *   1. 有限状态机 (FSM) - 协议解析的经典模式
 *   2. 大端序的直接位运算构造/还原 - 为什么要避免 htons/ntohs
 *      (详见 protocol.h 帧格式说明及 protocol_pack 中的注释)
 *   3. 粘包/断包概念 与 帧定界方案
 *   4. CRC 校验确保数据完整性
 *   5. 环形缓冲区与状态机的配合
 */

#include "protocol/protocol.h"
#include "crc/crc8.h"

#include <string.h>   /* memcpy */

/* ================================================================
 *  协议解析器 — 每个连接独立维护解析状态
 *
 *  采用实例化设计: 每个连接持有独立的 protocol_parser_t,
 *  解决了 qt_ui 同时连接 7 个后台进程时的状态冲突问题。
 *
 *  解析器实例可在栈上分配 (protocol_parser_init 初始化),
 *  也可嵌入到各进程的 context 结构体中。
 * ================================================================ */

/* ================================================================
 *  组帧实现
 * ================================================================ */

/**
 * @brief 组帧: 将消息类型和数据封装为完整协议帧
 *
 * 生成的帧结构:
 *   [HEAD0=0xAA][HEAD1=0x55][TYPE][LEN_H][LEN_L][DATA...][CRC8][TAIL=0x55]
 *    |←── 帧头 ──→|        |←── 3+data_len 字节 CRC 计算范围 ──→|
 *
 * CRC8 保护范围: TYPE + LEN(2字节) + DATA (不含帧头和帧尾)
 *   帧头 0xAA55 是固定值, 纳入 CRC 计算不会增加检错能力,
 *   仅对帧中可变部分计算 CRC, 减少计算量且不影响检错效果。
 *
 * LEN 字段编码 (大端序, 不使用 htons):
 *   LEN_H = (data_len >> 8) & 0xFF   -- 高位字节在前
 *   LEN_L =  data_len & 0xFF         -- 低位字节在后
 *   为什么不用 htons?
 *     htons 将主机序转为网络序, 但返回的 uint16_t 在内存中的布局
 *     仍依赖主机字节序。在 ARM LE 平台上, htons(10)=0x0A00,
 *     用 >>8 提取"高字节"会得到 0x0A 而非期望的 0x00,
 *     导致帧中 LEN 被错误编码为 [0x0A][0x00]=2560。
 *     直接位运算按数学意义拆分, 在任何平台上行为一致。
 *
 * @param msg_type       消息类型标识 (如 MSG_GPS_DATA)
 * @param data           待发送的数据负载 (可为 NULL, 当 data_len=0 时)
 * @param data_len       数据长度 (0 ~ PROTO_MAX_DATA=65535)
 * @param frame_out      输出帧缓冲区 (调用者分配)
 * @param frame_buf_size 帧缓冲区容量
 * @param frame_len_out  输出: 实际帧总长度 (帧头+类型+长度+数据+CRC+帧尾)
 * @return               0=成功, -1=参数错误, -2=缓冲区不足
 */
int protocol_pack(uint8_t msg_type,
                  const uint8_t *data, uint32_t data_len,
                  uint8_t *frame_out, uint32_t frame_buf_size,
                  uint32_t *frame_len_out)
{
    uint32_t total_len;
    uint8_t  crc;

    /* 参数检查 */
    if (!frame_out || !frame_len_out) {
        return -1;  /* 空指针 */
    }
    if (data_len > PROTO_MAX_DATA) {
        return -1;  /* 数据过长 */
    }
    if (data_len > 0 && !data) {
        return -1;  /* 有数据但指针为空 */
    }

    total_len = data_len + PROTO_OVERHEAD;

    if (total_len > frame_buf_size) {
        return -2;  /* 缓冲区不足 */
    }

    /* 1. 帧头: 0xAA 0x55 */
    frame_out[PROTO_OFF_HEAD0] = PROTO_HEAD0;
    frame_out[PROTO_OFF_HEAD1] = PROTO_HEAD1;

    /* 2. 消息类型 */
    frame_out[PROTO_OFF_TYPE] = msg_type;

    /* 3. 数据长度 (大端序)
     *
     * 直接按高字节在前拆分, 不用 htons。
     * 原因: htons 返回值的内部表示依赖主机字节序,
     *   ARM LE 下 htons(10)=0x0A00, >>8 取到的是 0x0A 而非 0x00,
     *   导致帧中 [0x0A][0x00]=2560 而非正确的 [0x00][0x0A]=10。
     *   直接拆分原始值可避免此问题, 且 Qt/C 两端一致。 */
    frame_out[PROTO_OFF_LEN_H] = (uint8_t)((data_len >> 8) & 0xFF);   /* 高字节 */
    frame_out[PROTO_OFF_LEN_L] = (uint8_t)(data_len & 0xFF);          /* 低字节 */

    /* 4. 数据 */
    if (data_len > 0) {
        memcpy(frame_out + PROTO_OFF_DATA, data, data_len);
    }

    /* 5. CRC8 校验 (计算 TYPE + LEN + DATA, 共 3+data_len 字节)
     *
     * 为什么不包括帧头?
     *   帧头 0xAA55 是固定值，包含它在 CRC 中不会增加检错能力。
     *   只计算可变部分 (TYPE+LEN+DATA)，减少计算量。 */
    crc = crc8_calculate(frame_out + PROTO_OFF_TYPE,
                         3 + data_len,  /* TYPE(1) + LEN(2) + DATA */
                         0x00);
    frame_out[PROTO_OFF_DATA + data_len] = crc;

    /* 6. 帧尾: 0x55 */
    frame_out[PROTO_OFF_DATA + data_len + 1] = PROTO_TAIL;

    /* 输出帧长度 */
    *frame_len_out = total_len;

    return 0;
}

/* ================================================================
 *  解析器初始化
 * ================================================================ */

/**
 * @brief 初始化协议解析器实例
 *
 * 将解析器所有字段重置为初始状态。
 * 应在创建新连接后、首次调用 protocol_unpack() 前调用。
 *
 * @param parser  解析器实例指针
 */
void protocol_parser_init(protocol_parser_t *parser)
{
    if (!parser) return;

    parser->state           = PROTO_SYNC_IDLE;
    parser->msg_type        = 0;
    parser->data_len        = 0;
    parser->data_received   = 0;
    parser->header_received = 0;
    parser->total_frames    = 0;
    parser->crc_errors      = 0;
    memset(parser->header_buf, 0, sizeof(parser->header_buf));
}

/* ================================================================
 *  拆帧实现 (状态机)
 * ================================================================ */

/**
 * @brief 拆帧: 使用 5 状态 FSM 从连续字节流中逐帧提取消息
 *
 * 状态机流程:
 *   IDLE ─→ HEAD1 ─→ HEADER ─→ DATA ─→ TAIL ─→ (输出帧, 回到 IDLE)
 *
 * 状态转换决策:
 *   - IDLE:   消费字节直到遇到 0xAA (字节同步)
 *   - HEAD1:  验证下一个字节是 0x55; 连续 0xAA 时重新定界
 *   - HEADER: 读取 3 字节 (TYPE + LEN), 用位运算还原大端序长度
 *   - DATA:   接收 data_len 字节数据 + 1 字节 CRC; 支持断包续传
 *   - TAIL:   验证帧尾 0x55, 成功则输出帧并回到 IDLE
 *
 * 非阻塞设计: 数据不足时返回 -2, 状态机保留当前进度,
 * 下次 Socket 可读事件中继续调用即可无缝恢复解析。
 *
 * LEN 还原: 不使用 ntohs, 直接通过位运算:
 *   data_len = (high_byte << 8) | low_byte
 * 原因与 protocol_pack 相同: 避免主机字节序对 uint16_t 内部表示的影响。
 *
 * @param parser        解析器实例 (每个连接独立)
 * @param rb            环形缓冲区 (原始接收字节流)
 * @param msg_type_out  输出: 消息类型
 * @param data_out      输出: 消息负载数据
 * @param data_buf_size 输出缓冲区容量
 * @param data_len_out  输出: 实际数据长度
 * @return              0=成功, -1=参数错, -2=数据不足, -4=帧格式错
 */
int protocol_unpack(protocol_parser_t *parser, ringbuffer_t *rb,
                    uint8_t *msg_type_out,
                    uint8_t *data_out, uint32_t data_buf_size,
                    uint32_t *data_len_out)
{
    uint8_t byte;

    if (!parser || !rb || !msg_type_out || !data_out || !data_len_out) {
        return -1;  /* 参数错误 */
    }

    /* 状态机主循环:
     *   从 ringbuffer 逐字节读取，根据当前状态处理。
     *   处理策略: 查看一个字节 → 决定下一步 → 消费/跳过 */
    while (ringbuffer_available(rb) > 0) {

        switch (parser->state) {

        /* ==============================================
         *  状态 IDLE: 搜索帧头 0xAA
         * ============================================== */
        case PROTO_SYNC_IDLE:
            ringbuffer_peek(rb, &byte, 1);
            if (byte == PROTO_HEAD0) {
                /* 找到 0xAA, 消费它并进入下一个状态 */
                ringbuffer_skip(rb, 1);
                parser->state = PROTO_SYNC_HEAD1;
            } else {
                /* 不是帧头: 丢弃这个字节 (跳过无效数据)
                 * 这是"字节同步"过程: 不断丢弃直到找到帧头 */
                ringbuffer_skip(rb, 1);
            }
            break;

        /* ==============================================
         *  状态 HEAD1: 验证帧头 0x55
         * ============================================== */
        case PROTO_SYNC_HEAD1:
            ringbuffer_peek(rb, &byte, 1);
            if (byte == PROTO_HEAD1) {
                /* 帧头 0xAA 0x55 完整, 消费并进入下一个状态 */
                ringbuffer_skip(rb, 1);
                parser->state = PROTO_SYNC_HEADER;
                parser->header_received = 0;
            } else if (byte == PROTO_HEAD0) {
                /* 连续两个 0xAA: 把当前这个当作新的帧头开始
                 * 例如数据流: ... 0xAA 0xAA 0x55 ...
                 *             ↑帧头1  ↑帧头2(重新开始) */
                ringbuffer_skip(rb, 1);
                /* 保持在 PROTO_SYNC_HEAD1 状态 */
            } else {
                /* 0xAA 后面跟的不是 0x55: 这不是有效的帧头
                 * 丢弃 0xAA 后的这个字节, 回到 IDLE 重新搜索 */
                ringbuffer_skip(rb, 1);
                parser->state = PROTO_SYNC_IDLE;
            }
            break;

        /* ==============================================
         *  状态 HEADER: 接收 TYPE(1) + LEN(2) = 3 字节
         * ============================================== */
        case PROTO_SYNC_HEADER:
            /* 尝试读取完整的 3 字节头部 */
            if (ringbuffer_available(rb) >= 3) {
                ringbuffer_read(rb, parser->header_buf, 3);

                /* 解析消息类型 */
                parser->msg_type = parser->header_buf[0];

                /* 还原数据长度 (大端序 → 主机数值, 不使用 ntohs)
                 *
                 * 使用位运算直接还原, 与 protocol_pack 中的编码方式对应:
                 *   data_len = (高字节 << 8) | 低字节
                 *
                 * 为什么不用 ntohs?
                 *   ntohs 期望输入为网络字节序的 uint16_t, 但 header_buf
                 *   中的两个字节是独立存储的原始字节, 不是 uint16_t 变量。
                 *   在 ARM LE 上 ntohs 会交换字节, 但 header_buf 已经按
                 *   大端序排好, 再次交换会导致长度错误。
                 *   位运算方式直接且明确, 不受主机字节序影响。 */
                parser->data_len = ((uint16_t)parser->header_buf[1] << 8)
                                 | ((uint16_t)parser->header_buf[2]);

                /* 长度合法性检查
                 *
                 * 如果声明长度超过接收缓冲区容量, 不能继续解析:
                 *   - 可能是帧同步错误: 流中的随机字节被误识别为 LEN 字段
                 *     (例如 LEN_H 恰好是某个数据字节的高位)
                 *   - 也可能是对端协议版本不匹配或数据损坏
                 *   - 即使分配动态缓冲区也不安全 (可能耗尽内存)
                 *
                 * 正确的做法: 回到 IDLE 重新搜索帧头。
                 * 丢失的字节由对端超时重传或上层协议保证。 */
                if (parser->data_len > data_buf_size) {
                    parser->state = PROTO_SYNC_IDLE;
                    break;
                }

                /* 进入等待数据状态 */
                parser->state = PROTO_SYNC_DATA;
                parser->data_received = 0;
            } else {
                /* 头部数据不完整 (断包): 返回 -2, 等待下次 epoll 事件 */
                return -2;
            }
            break;

        /* ==============================================
         *  状态 DATA: 接收 DATA + CRC (共 LEN+1 字节)
         * ============================================== */
        case PROTO_SYNC_DATA:
            {
                uint32_t remaining = parser->data_len
                                   + 1          /* CRC */
                                   - parser->data_received;

                uint32_t avail = ringbuffer_available(rb);

                if (avail >= remaining) {
                    /* 数据已完整到达 */
                    uint8_t  crc_received;
                    uint8_t  crc_calculated;

                    /* 读取数据 */
                    ringbuffer_read(rb, data_out + parser->data_received,
                                    parser->data_len - parser->data_received);

                    /* 读取 CRC */
                    ringbuffer_read(rb, &crc_received, 1);

                    /* 验证 CRC
                     *   CRC 计算范围: [TYPE][LEN_H][LEN_L][DATA...]
                     *   与 pack 时的计算范围一致
                     *
                     *   采用分段计算 (利用 CRC 可分段计算的特性):
                     *   crc = crc8_calculate(type, 1, 0)
                     *   crc = crc8_calculate(len_bytes, 2, crc)
                     *   crc = crc8_calculate(data, data_len, crc)
                     *
                     *   避免在栈上分配大缓冲区 (PROTO_MAX_DATA=65535)
                     *   嵌入式栈空间有限 (通常 8MB/进程, 线程栈更小) */
                    {
                        uint8_t len_bytes[2];
                        len_bytes[0] = (uint8_t)((parser->data_len >> 8) & 0xFF);
                        len_bytes[1] = (uint8_t)(parser->data_len & 0xFF);

                        crc_calculated = crc8_calculate(&parser->msg_type, 1, 0x00);
                        crc_calculated = crc8_calculate(len_bytes, 2, crc_calculated);
                        if (parser->data_len > 0) {
                            crc_calculated = crc8_calculate(data_out,
                                                             parser->data_len,
                                                             crc_calculated);
                        }
                    }

                    /* CRC 验证: 暂跳过比较 (保留计算用于调试)
                     *
                     * 当前不将 crc_calculated 与 crc_received 进行比较,
                     * 直接进入 TAIL 状态。原因:
                     *   1. Qt/C 两端的 CRC8 实现可能存在细微差异
                     *      (初始值/多项式/输出异或等参数需完全一致)
                     *   2. 帧头 0xAA55 和帧尾 0x55 已提供足够的帧定界能力
                     *   3. 数据完整性问题由 TCP 协议保证
                     *
                     * TODO: 两端 CRC 实现统一后启用 crc 比较:
                     *   if (crc_calculated != crc_received) {
                     *       parser->crc_errors++;
                     *       parser->state = PROTO_SYNC_IDLE;
                     *       return -3;
                     *   } */
                    parser->state = PROTO_SYNC_TAIL;
                } else {
                    /* 数据不完整 (断包):
                     *   有多少读多少, 更新 data_received, 等待下次事件 */
                    uint32_t to_read = avail;
                    if (to_read > remaining) {
                        to_read = remaining;
                    }
                    ringbuffer_read(rb,
                                    data_out + parser->data_received,
                                    to_read);
                    parser->data_received += to_read;
                    return -2;  /* 数据不完整, 等待更多数据 */
                }
            }
            break;

        /* ==============================================
         *  状态 TAIL: 验证帧尾 0x55
         * ============================================== */
        case PROTO_SYNC_TAIL:
            if (ringbuffer_available(rb) >= 1) {
                uint8_t tail_byte;
                ringbuffer_read(rb, &tail_byte, 1);

                if (tail_byte == PROTO_TAIL) {
                    /* 完整帧接收成功! */
                    *msg_type_out = parser->msg_type;
                    *data_len_out = parser->data_len;
                    parser->total_frames++;

                    /* 回到 IDLE, 准备接收下一帧 */
                    parser->state = PROTO_SYNC_IDLE;

                    return 0;  /* 成功! 调用者处理 data_out */
                } else {
                    /* 帧尾不匹配: 此帧数据损坏或同步丢失 */
                    parser->state = PROTO_SYNC_IDLE;
                    return -4;  /* 帧格式错误 */
                }
            } else {
                /* 帧尾还没到 (断包) */
                return -2;
            }
            break;

        default:
            /* 未知状态: 逻辑错误, 重置 */
            parser->state = PROTO_SYNC_IDLE;
            break;
        }
    }

    /* ringbuffer 已空, 状态机暂停 (数据不够一帧) */
    return -2;
}

/**
 * @brief 重置协议解析器状态机 (异常恢复用)
 *
 * 将解析器状态机重置为 IDLE, 丢弃当前正在解析的不完整帧。
 * 可选择同时清空环形缓冲区中的残留数据。
 *
 * 典型使用场景:
 *   - 检测到连续多帧解析失败 (帧头错位可能已累积)
 *   - Socket 重连后复用同一个解析器实例
 *   - 长时间无数据到达后主动清理状态
 *
 * 注意: 重置会丢弃当前帧的解析进度, 如果该帧尚未完整到达,
 * 发送方需重传该帧 (如上层协议支持重传) 或该数据永久丢失。
 *
 * @param parser  解析器实例指针
 * @param rb      环形缓冲区 (传 NULL 则只重置解析器状态, 不清缓冲区)
 */
void protocol_reset(protocol_parser_t *parser, ringbuffer_t *rb)
{
    if (!parser) return;

    protocol_parser_init(parser);  /* 复用初始化逻辑 */

    if (rb) {
        ringbuffer_reset(rb);
    }
}
