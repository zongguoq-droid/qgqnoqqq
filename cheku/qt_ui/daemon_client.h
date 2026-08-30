/**
 * @file    daemon_client.h
 * @brief   后台守护进程客户端 — QLocalSocket 封装
 *
 * ============================================================================
 * 概述
 * ============================================================================
 * DaemonClient 封装了与单个后台守护进程的 Unix domain socket 通信。
 * 每个实例对应一个守护进程，通过 QLocalSocket 连接到指定的 .sock 文件。
 *
 * 当前连接的守护进程及其 socket 路径:
 *   gps   → /tmp/car_gps.sock     GPS 定位数据
 *   input → /tmp/car_input.sock   物理按键事件上报
 *   can   → /tmp/car_can.sock     CAN 总线消息收发
 *   av    → /tmp/car_av.sock      音视频播放控制
 *   dvr   → /tmp/car_dvr.sock     行车记录仪控制
 *
 * 历史设计 (已不再使用, 仅供参考):
 *   sensor → /tmp/car_sensor.sock  传感器数据 (现由 SensorThread 直接读取设备)
 *   net    → /tmp/car_net.sock     网络状态 (未部署)
 *   guard  → /tmp/car_guard.sock   看门狗守护进程 (未部署)
 *
 * ============================================================================
 * 自动重连机制
 * ============================================================================
 *   连接断开或失败时, m_reconnectTimer 启动 1 秒单次定时器。
 *   定时器触发后调用 onReconnect() → connectToServer() 重试。
 *   连接成功后 (onConnected) 停止定时器。
 *   send() 不检查连接状态 — 消息在未连接时由 QLocalSocket 排队或丢弃。
 *
 * ============================================================================
 * 数据接收与帧解析
 * ============================================================================
 *   QLocalSocket 是面向流的, 可能一次收到多个不完整帧。
 *   m_recvBuffer 累积原始数据, ProtocolHandler::unpack() 循环解析:
 *     每识别出一个完整帧 → emit messageReceived(msgType, data)
 *     未消费的字节留在 buffer 中等待后续数据到达。
 *
 * 协议帧格式 (详见 protocol_handler.h):
 *   [0xAA][0x55][TYPE][LEN_H][LEN_L][DATA...][CRC8][0x55]
 */

#ifndef DAEMON_CLIENT_H
#define DAEMON_CLIENT_H

#include <QObject>
#include <QLocalSocket>
#include <QTimer>
#include <QByteArray>
#include "protocol_handler.h"

class DaemonClient : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief 构造客户端
     * @param name     守护进程逻辑名称 (用于信号标识和调试输出)
     * @param sockPath Unix domain socket 文件路径
     * @param parent   父 QObject
     *
     * 初始化 QLocalSocket + 1 秒重连定时器 + ProtocolHandler,
     * 连接所有 QLocalSocket 信号到内部槽函数。
     */
    explicit DaemonClient(const QString &name, const QString &sockPath,
                          QObject *parent = nullptr);

    /** @brief 析构: 停止重连定时器, 断开 socket */
    ~DaemonClient();

    /**
     * @brief 发起连接到 Unix domain socket
     *
     * 可在任意时刻调用, 包括重连时。
     * QLocalSocket::connectToServer() 是异步的, 成功/失败通过信号通知。
     */
    void connectToServer();

    /**
     * @brief 发送协议帧到守护进程
     * @param msgType 消息类型 (MSG_* 宏定义)
     * @param data    数据载荷 (可选, 默认为空)
     *
     * 内部调用 ProtocolHandler::pack() 组帧, 写入 QLocalSocket 后 flush()。
     * 消息以完整帧发出: [0xAA][0x55][TYPE][LEN][DATA][CRC8][0x55]
     */
    void send(uint8_t msgType, const QByteArray &data = QByteArray());

    /**
     * @brief 检查当前连接状态
     * @return true 如果 QLocalSocket 处于 ConnectedState
     */
    bool isConnected() const;

signals:
    /**
     * @brief 收到完整协议帧
     * @param msgType 消息类型 (MSG_*)
     * @param data    数据载荷字节数组
     */
    void messageReceived(uint8_t msgType, const QByteArray &data);

    /**
     * @brief 连接状态变化信号
     * @param name 守护进程名称
     */
    void connected(const QString &name);
    void disconnected(const QString &name);

private slots:
    /** @brief socket 连接成功: 停止重连定时器, 清空接收缓冲区, 重置协议解析器 */
    void onConnected();

    /** @brief socket 断开: 启动重连定时器 (1秒后重试) */
    void onDisconnected();

    /**
     * @brief socket 有数据可读
     *
     * 追加到 m_recvBuffer, 循环调用 ProtocolHandler::unpack() 解析完整帧,
     * 每帧 emit messageReceived()。未消费的字节留在 buffer 中。
     */
    void onReadyRead();

    /** @brief socket 错误: 启动重连定时器 */
    void onError(QLocalSocket::LocalSocketError err);

    /** @brief 重连定时器超时: 重新调用 connectToServer() */
    void onReconnect();

private:
    QString      m_name;           /**< 守护进程逻辑名称 (gps/input/can/av/dvr) */
    QString      m_sockPath;       /**< Unix domain socket 文件路径 */
    QLocalSocket *m_socket;        /**< QLocalSocket 实例 (异步 I/O) */
    QTimer       *m_reconnectTimer;/**< 重连定时器 (1秒, 单次触发) */
    QByteArray   m_recvBuffer;     /**< 接收缓冲区: 累积未完整解析的原始数据 */
    ProtocolHandler m_proto;       /**< 协议帧解析器 (状态机驱动) */
};

#endif // DAEMON_CLIENT_H
