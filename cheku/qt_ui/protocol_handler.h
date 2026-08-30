/**
 * @file    protocol_handler.h
 * @brief   二进制协议帧处理 (Qt/C++ 版本)
 *
 * ============================================================================
 * 帧格式 (与 C 后端 common/protocol 完全一致)
 * ============================================================================
 *   偏移量  大小  字段        描述
 *   ------  ----  ----------  -------------------------------------------
 *   0       1     HEAD0       帧头第一字节, 固定 0xAA
 *   1       1     HEAD1       帧头第二字节, 固定 0x55
 *   2       1     TYPE        消息类型 (MSG_* 宏定义)
 *   3       1     LEN_H       数据长度高字节 (大端)
 *   4       1     LEN_L       数据长度低字节 (大端)
 *   5..N    可变  DATA        数据载荷 (长度为 LEN)
 *   N+1     1     CRC8        CRC8 校验值 (多项式 0x07, 当前固定填 0x00)
 *   N+2     1     TAIL        帧尾, 固定 0x55
 *
 *   最小帧长: 7 字节 (HEAD + TYPE + LEN(0) + CRC + TAIL)
 *   最大帧长: 7 + 65535 字节
 *
 *   示例帧 (MSG_DVR_SNAPSHOT = 0x23, 无数据):
 *     AA 55 23 00 00 00 55
 *     |     |  |----|  |  |
 *     HEAD  TYPE LEN=0 CRC TAIL
 *
 * ============================================================================
 * CRC8 说明
 * ============================================================================
 *   CRC8 多项式: 0x07 (x^8 + x^2 + x^1 + 1), 初始值 0x00。
 *   查找表 crc8_table[256] 与 C 后端 common/crc/crc8.c 完全一致。
 *
 *   当前实现:
 *     pack(): CRC 字段固定填 0x00 (未启用 CRC 校验)
 *     unpack(): 跳过 CRC 字节, 不验证 (feed() 中 DATA 状态包含 CRC 字节但忽略)
 *
 *   为什么 CRC 当前未启用?
 *     Unix domain socket 基于内核内存传输, 不存在比特错误。
 *     CRC 字段保留用于未来可能的跨网络扩展 (如 TCP 中继)。
 *     如需启用: 在 pack() 中计算 CRC 替换 0x00, 在 feed() 中解析 CRC 并比对。
 *
 * ============================================================================
 * 拆帧状态机 (feed)
 * ============================================================================
 *   状态转换图:
 *
 *     IDLE ──[0xAA]──> HEAD1 ──[0x55]──> HEADER ──[3 bytes]──> DATA
 *       ^                |                 ^                     |
 *       |   [!0x55]      |   [0xAA]        |                     | [LEN+1 bytes]
 *       +───[!0xAA]──────+                 |                     |
 *       |                                  |                     v
 *       +─────────────[!0x55]──────────────+                   TAIL
 *       ^                                                       |
 *       +──────────────────────[0x55]───────────────────────────+
 *                                   (成功: 返回完整帧)
 *
 *   状态说明:
 *     IDLE:   等待帧头 0xAA。任何非 0xAA 字节被丢弃。
 *     HEAD1:  已收到 0xAA, 等待 0x55。
 *             - 收到 0x55 → 进入 HEADER
 *             - 收到 0xAA → 保持 HEAD1 (连续头字节, 重新开始)
 *             - 其他字节  → 回到 IDLE (同步丢失)
 *     HEADER: 收集 3 字节头部 (TYPE + LEN_H + LEN_L)。
 *             收到后解析 msgType 和 dataLen, 进入 DATA。
 *     DATA:   收集 (dataLen + 1) 字节 (DATA + CRC)。
 *             收集完毕后提取 DATA 部分 (不含 CRC), 进入 TAIL。
 *     TAIL:   等待帧尾 0x55。收到后返回完整帧并回到 IDLE。
 *
 * ============================================================================
 * 与 C 后端的一致性
 * ============================================================================
 *   此文件在 Qt 端独立实现了 CRC8 查找表和拆帧状态机，与 C 后端
 *   common/crc/crc8.c 和 common/protocol/protocol.c 逻辑完全一致。
 *   选择独立实现而非引入 C 库是为了:
 *     1. 避免交叉编译 C 源码到 Qt 项目的复杂度
 *     2. 保持 Qt 项目纯 C++ 构建
 *     3. 代码量很小 (~30 行 CRC 表 + ~60 行状态机)
 */

#ifndef PROTOCOL_HANDLER_H
#define PROTOCOL_HANDLER_H

#include <QObject>
#include <QByteArray>
#include <QVector>
#include <cstdint>

/* 帧常量 (与 C 端 protocol.h 一致) */
#define PH_HEAD0        0xAA    /**< 帧头第一字节 */
#define PH_HEAD1        0x55    /**< 帧头第二字节 */
#define PH_TAIL         0x55    /**< 帧尾 */
#define PH_OVERHEAD     7       /**< 帧开销: HEAD(2)+TYPE(1)+LEN(2)+CRC(1)+TAIL(1) */

/* ================================================================
 *  消息类型定义 (与 C 端 common.h 完全一致)
 * ================================================================ */
#define MSG_HEARTBEAT       0x00  /**< 心跳包 */
#define MSG_SERVICE_STATUS  0x01  /**< 服务状态查询/上报 */
#define MSG_SERVICE_RESTART 0x02  /**< 重启服务 */

#define MSG_GPS_DATA        0x10  /**< GPS 数据上报 (GpsData 结构体) */
#define MSG_GPS_QUERY       0x11  /**< GPS 数据查询 */
#define MSG_GPS_STATUS      0x13  /**< GPS 模组状态 */

#define MSG_DVR_START       0x20  /**< 开始/继续录像 */
#define MSG_DVR_STOP        0x21  /**< 停止录像 */
#define MSG_DVR_STATUS      0x22  /**< DVR 状态回传 (state/elapsed/resolution/frames) */
#define MSG_DVR_SNAPSHOT    0x23  /**< 拍照: 保存当前帧为 JPEG */
#define MSG_DVR_FILE_LIST   0x24  /**< 录像文件列表查询 */
#define MSG_DVR_PAUSE       0x26  /**< 暂停/恢复录像 */

#define MSG_SENSOR_DATA     0x30  /**< 传感器数据上报 (SensorData 结构体) */
#define MSG_SENSOR_QUERY    0x31  /**< 传感器数据查询 */
#define MSG_SENSOR_ALERT    0x33  /**< 传感器告警 */

#define MSG_NET_STATUS      0x40  /**< 网络状态 */

#define MSG_CAN_DATA        0x50  /**< CAN 数据上报 */
#define MSG_CAN_SEND        0x51  /**< CAN 数据发送 */
#define MSG_CAN_STATUS      0x53  /**< CAN 控制器状态 */

#define MSG_AV_PLAY         0x60  /**< 开始播放 */
#define MSG_AV_STOP         0x61  /**< 停止播放 */
#define MSG_AV_PAUSE        0x62  /**< 暂停/恢复播放 */
#define MSG_AV_VOLUME       0x63  /**< 音量调节 */
#define MSG_AV_STATUS       0x64  /**< 播放状态回传 */

#define MSG_KEY_EVENT       0x70  /**< 物理按键事件 (KeyEvent 结构体) */
#define MSG_LED_CONTROL     0x71  /**< LED 控制 (已移除, 保留定义) */

/* ================================================================
 *  C 端数据结构 (POD, 与 common.h 字节布局完全一致)
 *
 *  #pragma pack(push, 1) 确保:
 *    - 无结构体成员对齐填充
 *    - 与 ARM 端 C 代码的内存布局逐字节一致
 *    - 可直接 reinterpret_cast 从 QByteArray 解析
 * ================================================================ */
#pragma pack(push, 1)
struct GpsData {
    double   latitude;      /**< 纬度 (度, WGS-84) */
    double   longitude;     /**< 经度 (度, WGS-84) */
    float    speed;         /**< 地面速度 (km/h) */
    float    altitude;      /**< 海拔高度 (m) */
    uint16_t year;          /**< UTC 年份 */
    uint8_t  month, day, hour, min, sec;  /**< UTC 月/日/时/分/秒 */
    uint8_t  satellites;    /**< 可见卫星数量 */
    uint8_t  fix_quality;   /**< 定位质量: 0=无效, 1=2D, 2=3D */
    float    hdop;          /**< 水平精度因子 */
};

struct SensorData {
    float    temperature;   /**< 温度 (°C) */
    float    humidity;      /**< 湿度 (%RH) */
    uint8_t  checksum_ok;   /**< DHT11 校验和是否通过 */
    uint32_t timestamp;     /**< 采样时间戳 (ARM 32-bit time_t) */
};

/**
 * @brief CAN 消息帧结构 (can_msg_t)
 *
 * 固定 15 字节, 对应 CAN 标准/扩展帧格式:
 *   - is_extended=0: 标准帧 (11-bit ID)
 *   - is_extended=1: 扩展帧 (29-bit ID)
 *   - is_remote=1:   远程帧 (无数据, DLC 表示请求长度)
 */
struct CanMsg {
    uint32_t can_id;       /**< CAN 标识符 (标准 11-bit 或扩展 29-bit) */
    uint8_t  can_dlc;      /**< 数据长度码 (0-8) */
    uint8_t  data[8];      /**< CAN 数据字段 (8 字节, 仅前 DLC 字节有效) */
    uint8_t  is_extended;  /**< 扩展帧标志 (0=标准帧, 1=扩展帧) */
    uint8_t  is_remote;    /**< 远程帧标志 (0=数据帧, 1=远程帧) */
};

/**
 * @brief 物理按键事件结构
 *
 * event_type 定义:
 *   1 = 短按 (short press)
 *   2 = 长按 (long press)
 *   3 = 双击 (double click)
 *
 * 仅短按触发功能操作; 长按和双击仅记录到 statusbar 用于诊断。
 */
struct KeyEvent {
    uint8_t  key_id;       /**< 按键编号 (1=KEY1, 2=KEY2, ...) */
    uint8_t  event_type;   /**< 事件类型: 1=短按, 2=长按, 3=双击 */
    uint32_t duration_ms;  /**< 按键持续时间 (ms, 长按有效) */
    uint32_t timestamp;    /**< 事件时间戳 (ARM 32-bit time_t) */
};
#pragma pack(pop)

/**
 * @brief 协议帧处理器 — 组帧、拆帧、CRC8 计算
 *
 * 提供静态方法 pack() 组帧和成员方法 unpack() 拆帧。
 * unpack() 内部维护状态机, 支持从流式字节数据中逐步解析完整帧。
 *
 * 使用示例:
 * @code
 *   // 组帧发送
 *   QByteArray frame = ProtocolHandler::pack(MSG_DVR_START, QByteArray());
 *   socket->write(frame);
 *
 *   // 拆帧接收
 *   ProtocolHandler proto;
 *   QByteArray buffer;
 *   buffer.append(socket->readAll());
 *   uint8_t msgType; QByteArray data;
 *   while (proto.unpack(buffer, msgType, data)) {
 *       handleMessage(msgType, data);
 *   }
 * @endcode
 */
class ProtocolHandler : public QObject
{
    Q_OBJECT
public:
    explicit ProtocolHandler(QObject *parent = nullptr);

    /**
     * @brief 组帧: 将 (msgType, data) 组装为完整协议帧
     * @param msgType 消息类型 (MSG_*)
     * @param data    数据载荷
     * @return        完整帧字节数组
     *
     * 帧格式: [0xAA][0x55][TYPE][LEN_H][LEN_L][DATA][CRC=0x00][0x55]
     *
     * 注意: CRC 字段当前固定填 0x00 (见文件头 CRC8 说明)。
     */
    static QByteArray pack(uint8_t msgType, const QByteArray &data);

    /**
     * @brief 拆帧: 从字节流缓冲区中解析完整帧
     * @param buffer     输入+输出: 累积的原始数据流
     *                   成功解析后已消费的字节被移除
     * @param msgType    输出: 解析出的消息类型
     * @param data       输出: 解析出的数据载荷
     * @return           true=成功解析一帧, false=数据不足或格式错误
     *
     * 非破坏性: 解析失败时 buffer 内容不变 (feed 按字节消费)。
     * 可循环调用直到返回 false, 每帧触发一次消息处理。
     */
    bool unpack(QByteArray &buffer, uint8_t &msgType, QByteArray &data);

    /**
     * @brief CRC8 计算
     * @param data 数据指针
     * @param len  数据长度
     * @return     CRC8 校验值
     *
     * 多项式: 0x07, 初始值: 0x00, 使用预计算查找表。
     * 与 C 后端 common/crc/crc8.c 的 crc8_calculate() 完全一致。
     */
    static uint8_t crc8(const uint8_t *data, int len);

    /**
     * @brief 重置拆帧状态机到初始状态 (IDLE)
     *
     * 连接断开/重连后调用, 丢弃未完成的半帧数据。
     */
    void reset();

private:
    /**
     * @brief 拆帧状态机状态
     *
     * 状态转换详见文件头"拆帧状态机"文档。
     */
    enum State { IDLE, HEAD1, HEADER, DATA, TAIL };

    State m_state = IDLE;          /**< 当前状态 */
    uint8_t m_msgType = 0;         /**< 当前帧的消息类型 (HEADER 状态解析) */
    uint16_t m_dataLen = 0;        /**< 当前帧的数据长度 (HEADER 状态解析) */
    uint16_t m_dataReceived = 0;   /**< DATA 状态已接收的字节计数 */
    QByteArray m_headerBuf;        /**< HEADER 状态临时缓冲 (3 字节) */
    QByteArray m_dataBuf;          /**< DATA 状态临时数据缓冲 */
    uint8_t m_crc = 0;             /**< CRC 校验值 (保留, 当前未使用) */

    /**
     * @brief 增量式字节馈送 — 状态机核心
     * @param byte     输入: 单个字节
     * @param msgType  输出: 解析出的消息类型 (仅返回 true 时有效)
     * @param data     输出: 解析出的数据载荷 (仅返回 true 时有效)
     * @return         true=完整帧解析成功
     *
     * 每次馈送 1 字节, 根据当前状态转换。收到完整帧时返回 true。
     * 同步丢失 (非预期字节) 时静默回到 IDLE 而非报错, 确保从
     * 字节流中间开始接收时能重新同步。
     */
    bool feed(uint8_t byte, uint8_t &msgType, QByteArray &data);
};

#endif // PROTOCOL_HANDLER_H
