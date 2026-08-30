/**
 * @file    protocol_handler.cpp
 * @brief   二进制协议帧处理 — 实现
 *
 * ============================================================================
 * CRC8 实现
 * ============================================================================
 *   多项式: 0x07 (x^8 + x^2 + x^1 + 1)
 *   初始值: 0x00
 *   查找表: 256 项预计算表, 与 C 后端 common/crc/crc8.c 完全一致。
 *
 *   提供两个函数:
 *     crc8(data, len)              — 完整计算
 *     crc8_continue(data, len, crc) — 分段计算 (与 C 端 crc8_calculate 接口一致)
 *
 *   当前 CRC 在 pack() 中固定填 0x00, unpack() 中不验证。
 *   详见 protocol_handler.h 文件头 CRC8 说明。
 *
 * ============================================================================
 * 拆帧状态机 (feed)
 * ============================================================================
 *   状态机设计原则:
 *     1. 每字节驱动一次状态转换, 适合流式解析
 *     2. 同步丢失时静默回到 IDLE (通过连续 0xAA 处理支持粘包)
 *     3. 非预期字节被丢弃, 从下一个 0xAA 重新开始同步
 *     4. 无内存分配 — 使用 QByteArray::left() 提取有效数据
 *
 *   状态流程 (详见 protocol_handler.h 状态转换图):
 *     IDLE  → HEAD1  → HEADER  → DATA  → TAIL  → (返回帧) → IDLE
 *        ^       |        |        |
 *        +---(!0xAA)----+--------+-------- (同步丢失, 重新开始)
 */

#include "protocol_handler.h"
#include <QDebug>

/**
 * @brief CRC8 查找表
 *
 * 多项式: 0x07 (x^8 + x^2 + x^1 + 1)
 * 初始值: 0x00, 不反射输入/输出, 不异或最终值
 *
 * 与 C 后端 common/crc/crc8.c 的 crc8_table 逐字节一致。
 * 生成算法 (C 伪代码):
 *   for i in 0..255:
 *     crc = i
 *     for j in 0..7:
 *       crc = (crc << 1) ^ ((crc & 0x80) ? 0x07 : 0)
 *     table[i] = crc & 0xFF
 */
static const uint8_t crc8_table[256] = {
    0x00,0x07,0x0E,0x09,0x1C,0x1B,0x12,0x15,0x38,0x3F,0x36,0x31,0x24,0x23,0x2A,0x2D,
    0x70,0x77,0x7E,0x79,0x6C,0x6B,0x62,0x65,0x48,0x4F,0x46,0x41,0x54,0x53,0x5A,0x5D,
    0xE0,0xE7,0xEE,0xE9,0xFC,0xFB,0xF2,0xF5,0xD8,0xDF,0xD6,0xD1,0xC4,0xC3,0xCA,0xCD,
    0x90,0x97,0x9E,0x99,0x8C,0x8B,0x82,0x85,0xA8,0xAF,0xA6,0xA1,0xB4,0xB3,0xBA,0xBD,
    0xC7,0xC0,0xC9,0xCE,0xDB,0xDC,0xD5,0xD2,0xFF,0xF8,0xF1,0xF6,0xE3,0xE4,0xED,0xEA,
    0xB7,0xB0,0xB9,0xBE,0xAB,0xAC,0xA5,0xA2,0x8F,0x88,0x81,0x86,0x93,0x94,0x9D,0x9A,
    0x27,0x20,0x29,0x2E,0x3B,0x3C,0x35,0x32,0x1F,0x18,0x11,0x16,0x03,0x04,0x0D,0x0A,
    0x57,0x50,0x59,0x5E,0x4B,0x4C,0x45,0x42,0x6F,0x68,0x61,0x66,0x73,0x74,0x7D,0x7A,
    0x89,0x8E,0x87,0x80,0x95,0x92,0x9B,0x9C,0xB1,0xB6,0xBF,0xB8,0xAD,0xAA,0xA3,0xA4,
    0xF9,0xFE,0xF7,0xF0,0xE5,0xE2,0xEB,0xEC,0xC1,0xC6,0xCF,0xC8,0xDD,0xDA,0xD3,0xD4,
    0x69,0x6E,0x67,0x60,0x75,0x72,0x7B,0x7C,0x51,0x56,0x5F,0x58,0x4D,0x4A,0x43,0x44,
    0x19,0x1E,0x17,0x10,0x05,0x02,0x0B,0x0C,0x21,0x26,0x2F,0x28,0x3D,0x3A,0x33,0x34,
    0x4E,0x49,0x40,0x47,0x52,0x55,0x5C,0x5B,0x76,0x71,0x78,0x7F,0x6A,0x6D,0x64,0x63,
    0x3E,0x39,0x30,0x37,0x22,0x25,0x2C,0x2B,0x06,0x01,0x08,0x0F,0x1A,0x1D,0x14,0x13,
    0xAE,0xA9,0xA0,0xA7,0xB2,0xB5,0xBC,0xBB,0x96,0x91,0x98,0x9F,0x8A,0x8D,0x84,0x83,
    0xDE,0xD9,0xD0,0xD7,0xC2,0xC5,0xCC,0xCB,0xE6,0xE1,0xE8,0xEF,0xFA,0xFD,0xF4,0xF3
};

ProtocolHandler::ProtocolHandler(QObject *parent) : QObject(parent) {}

/**
 * @brief 计算 CRC8 校验值 (完整计算, 初始 CRC=0x00)
 * @param data 数据字节数组
 * @param len  数据长度
 * @return     CRC8 值 (0x00-0xFF)
 */
uint8_t ProtocolHandler::crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0x00;
    for (int i = 0; i < len; i++)
        crc = crc8_table[(crc ^ data[i]) & 0xFF];
    return crc;
}

/**
 * @brief 分段 CRC8 计算
 *
 * 在已有 CRC 值基础上继续计算后续数据, 与 C 端 crc8_calculate(data, len, crc)
 * 接口完全一致。用于跨多个缓冲区分段计算 CRC。
 *
 * @param data 新增数据字节数组
 * @param len  新增数据长度
 * @param crc  已有的 CRC 值 (上次计算的中间结果)
 * @return     更新后的 CRC 值
 */
static uint8_t crc8_continue(const uint8_t *data, int len, uint8_t crc)
{
    for (int i = 0; i < len; i++)
        crc = crc8_table[(crc ^ data[i]) & 0xFF];
    return crc;
}

/**
 * @brief 组帧: 数据 + 消息类型 → 完整协议帧
 *
 * 帧结构:
 *   [0xAA] [0x55] [TYPE] [LEN_H] [LEN_L] [DATA...] [CRC=0x00] [0x55]
 *
 * 长度字段为大端序 (网络字节序), 最大 65535。
 * CRC 字段当前固定为 0x00 (未启用验证, 见 .h 文件 CRC8 说明)。
 */
QByteArray ProtocolHandler::pack(uint8_t msgType, const QByteArray &data)
{
    QByteArray frame;
    uint16_t len = (uint16_t)data.size();

    frame.append((char)PH_HEAD0);                       /* 帧头 0xAA */
    frame.append((char)PH_HEAD1);                       /* 帧头 0x55 */
    frame.append((char)msgType);                        /* 消息类型 */
    frame.append((char)((len >> 8) & 0xFF));            /* 长度高字节 */
    frame.append((char)(len & 0xFF));                   /* 长度低字节 */
    frame.append(data);                                  /* 数据载荷 */

    /* CRC: 当前固定填 0x00, 未启用校验。
     * 如需启用, 替换为:
     *   uint8_t crc = crc8((const uint8_t*)frame.constData()+2, 3+len);
     *   frame.append((char)crc);
     */
    frame.append((char)0x00);
    frame.append((char)PH_TAIL);                        /* 帧尾 0x55 */

    return frame;
}

void ProtocolHandler::reset()
{
    m_state = IDLE;
    m_dataReceived = 0;
    m_headerBuf.clear();
    m_dataBuf.clear();
}

/**
 * @brief 增量式字节馈送 — 状态机核心实现
 *
 * 状态转换逻辑:
 *
 *   IDLE:
 *     收到 0xAA → 进入 HEAD1 (可能的帧开始)
 *     收到其他 → 保持 IDLE (丢弃垃圾字节)
 *
 *   HEAD1:
 *     收到 0x55 → 进入 HEADER (帧头确认, 开始接收头部)
 *     收到 0xAA → 保持 HEAD1 (连续帧头, 重新开始 — 处理粘包场景)
 *     收到其他 → 回到 IDLE (帧头不完整, 同步丢失)
 *
 *   HEADER:
 *     收集 3 字节: [TYPE][LEN_H][LEN_L]
 *     收齐后解析 msgType 和 dataLen → 进入 DATA
 *
 *   DATA:
 *     收集 (dataLen + 1) 字节: [DATA...][CRC]
 *     收齐后提取有效数据 (left(dataLen), 排除 CRC 字节) → 进入 TAIL
 *
 *   TAIL:
 *     收到 0x55 → 返回 true (完整帧!), 回到 IDLE
 *     收到其他 → 回到 IDLE (帧尾无效, 丢弃整帧)
 *
 * @param byte    输入字节
 * @param msgType 输出消息类型 (仅返回 true 时有效)
 * @param data    输出数据载荷 (仅返回 true 时有效)
 * @return        true=完整帧解析成功
 */
bool ProtocolHandler::feed(uint8_t byte, uint8_t &msgType, QByteArray &data)
{
    switch (m_state) {
    case IDLE:
        if (byte == PH_HEAD0) {
            m_state = HEAD1;
            m_dataReceived = 0;
            m_headerBuf.clear();
            m_dataBuf.clear();
        }
        /* else: 丢弃非帧头字节, 保持 IDLE */
        break;

    case HEAD1:
        if (byte == PH_HEAD1) {
            m_state = HEADER;           /* 帧头 0xAA 0x55 确认 */
        } else if (byte == PH_HEAD0) {
            /* 连续 0xAA: 前一个 0xAA 不是真正的帧头, 重新开始 */
            /* 保持 HEAD1, 数据已在上一步清除 */
        } else {
            m_state = IDLE;             /* 同步丢失, 回到初始状态 */
        }
        break;

    case HEADER:
        m_headerBuf.append((char)byte);
        if (m_headerBuf.size() >= 3) {  /* 收到完整 3 字节头部 */
            m_msgType = (uint8_t)m_headerBuf[0];
            m_dataLen = (uint16_t)(((uint8_t)m_headerBuf[1] << 8) | (uint8_t)m_headerBuf[2]);
            m_state = DATA;
        }
        break;

    case DATA:
        m_dataBuf.append((char)byte);
        m_dataReceived++;
        /* +1 表示 CRC 字节也被计入, 但提取数据时排除 */
        if (m_dataReceived >= m_dataLen + 1) {
            data    = m_dataBuf.left(m_dataLen);  /* 提取有效数据 (不含 CRC) */
            msgType = m_msgType;
            m_state = TAIL;                        /* 等待帧尾确认 */
        }
        break;

    case TAIL:
        if (byte == PH_TAIL) {
            m_state = IDLE;
            return true;  /* 完整帧解析成功! 回到 IDLE 准备下一帧 */
        }
        /* 帧尾不是 0x55: 帧损坏, 丢弃 */
        m_state = IDLE;
        break;
    }

    return false;
}

/**
 * @brief 从字节流缓冲区中解析完整帧
 *
 * 将 buffer 中的字节逐个喂给 feed() 状态机。
 * 每次 feed() 返回 true 时, buffer 中已消费的字节已被 remove(0, 1) 移除,
 * 返回的数据是该帧的有效载荷。
 *
 * 调用方应循环调用此方法直到返回 false:
 * @code
 *   uint8_t msgType; QByteArray data;
 *   while (proto.unpack(buffer, msgType, data)) {
 *       processMessage(msgType, data);  // 处理每一帧
 *   }
 *   // buffer 中剩余的是不完整的下一帧数据
 * @endcode
 *
 * 时间复杂度: O(n), n = buffer 大小。每个字节仅处理一次。
 * 空间复杂度: O(1), 仅使用固定大小的内部缓冲区。
 */
bool ProtocolHandler::unpack(QByteArray &buffer, uint8_t &msgType, QByteArray &data)
{
    while (!buffer.isEmpty()) {
        uint8_t byte = (uint8_t)buffer.at(0);
        buffer.remove(0, 1);  /* 逐字节消费 (FIFO) */

        if (feed(byte, msgType, data)) {
            return true;      /* 完整帧! 调用方可立即处理 */
        }
    }
    return false;             /* 数据不足, 等待更多字节 */
}
