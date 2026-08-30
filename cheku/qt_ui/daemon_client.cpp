/**
 * @file    daemon_client.cpp
 * @brief   后台守护进程客户端 — 实现
 *
 * 每个 DaemonClient 实例管理一条 QLocalSocket 连接及其自动重连逻辑。
 * 发送侧: ProtocolHandler::pack() 将 (msgType, data) 组帧后写入 socket。
 * 接收侧: onReadyRead() 累积字节 → ProtocolHandler::unpack() 循环解帧。
 *
 * 自动重连:
 *   连接断开/错误 → 1秒定时器 → onReconnect() → connectToServer()
 *   连接成功 → onConnected() 停止定时器
 */

#include "daemon_client.h"
#include <QDebug>

/**
 * @brief 构造客户端: 初始化 QLocalSocket, 重连定时器, 协议处理器
 *
 * 连接信号:
 *   QLocalSocket::connected    → onConnected     (停止重连, 清空缓冲)
 *   QLocalSocket::disconnected → onDisconnected  (启动重连)
 *   QLocalSocket::readyRead    → onReadyRead     (累积数据, 解帧)
 *   QLocalSocket::error        → onError         (启动重连)
 *   m_reconnectTimer::timeout  → onReconnect     (重试连接)
 *
 * 重连定时器配置: 间隔 1 秒, 单次触发 (setSingleShot=true),
 * 避免在连接失败后立即连续重试。
 */
DaemonClient::DaemonClient(const QString &name, const QString &sockPath,
                           QObject *parent)
    : QObject(parent), m_name(name), m_sockPath(sockPath)
{
    m_socket = new QLocalSocket(this);
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(1000);  /* 断线后 1 秒重试连接 */
    m_reconnectTimer->setSingleShot(true);

    /* QLocalSocket 信号 → 内部槽函数 */
    connect(m_socket, &QLocalSocket::connected,
            this, &DaemonClient::onConnected);
    connect(m_socket, &QLocalSocket::disconnected,
            this, &DaemonClient::onDisconnected);
    connect(m_socket, &QLocalSocket::readyRead,
            this, &DaemonClient::onReadyRead);
    connect(m_socket, QOverload<QLocalSocket::LocalSocketError>::of(&QLocalSocket::error),
            this, &DaemonClient::onError);

    /* 重连定时器 */
    connect(m_reconnectTimer, &QTimer::timeout,
            this, &DaemonClient::onReconnect);
}

DaemonClient::~DaemonClient()
{
    m_reconnectTimer->stop();
    m_socket->disconnectFromServer();
}

/**
 * @brief 发起连接到 Unix domain socket
 *
 * 连接是异步的 — 成功/失败通过 QLocalSocket::connected/error 信号通知。
 * 调用方 (如 MainWindow) 在构造完所有客户端后统一调用此方法。
 */
void DaemonClient::connectToServer()
{
    qDebug() << "[client]" << m_name << "connecting to" << m_sockPath;
    m_socket->connectToServer(m_sockPath);
}

bool DaemonClient::isConnected() const
{
    return m_socket->state() == QLocalSocket::ConnectedState;
}

/**
 * @brief 发送协议帧
 *
 * 流程:
 *   1. ProtocolHandler::pack(msgType, data) 组装完整帧
 *      → [0xAA][0x55][TYPE][LEN_H][LEN_L][DATA][CRC=0x00][0x55]
 *   2. m_socket->write(frame) 写入 socket 发送缓冲区
 *   3. m_socket->flush()      立即发送 (不等待事件循环)
 *   4. 调试输出: 打印帧的十六进制内容
 *
 * 注意: send() 不检查 isConnected() — 若未连接, write() 会失败或数据丢失。
 *       消息未实现应用层重传, 依赖 TCP 的可靠传输保证 (Unix socket 等同于本地 TCP)。
 */
void DaemonClient::send(uint8_t msgType, const QByteArray &data)
{
    QByteArray frame = ProtocolHandler::pack(msgType, data);
    m_socket->write(frame);
    m_socket->flush();
    QString hex;
    for (int i = 0; i < frame.size(); i++)
        hex += QString("%1 ").arg((unsigned char)frame[i], 2, 16, QChar('0'));
    qDebug() << "[client]" << m_name << "msg 0x" << hex << msgType << "frame:" << hex;
}

/**
 * @brief 连接成功回调
 *
 * 执行:
 *   1. 停止重连定时器 (不再需要重试)
 *   2. 清空接收缓冲区 (丢弃上次连接残留数据)
 *   3. 重置 ProtocolHandler 状态机到 IDLE
 *   4. emit connected(m_name) → 通知 MainWindow 更新连接计数
 */
void DaemonClient::onConnected()
{
    qDebug() << "[client]" << m_name << "connected!";
    m_reconnectTimer->stop();
    m_recvBuffer.clear();
    m_proto.reset();
    emit connected(m_name);
}

/**
 * @brief 连接断开回调
 *
 * 执行:
 *   1. emit disconnected(m_name) → 通知 MainWindow 更新连接计数
 *   2. 启动重连定时器 (1 秒后触发 onReconnect)
 *
 * 注意: 不在此处清空 m_recvBuffer — 重连后 onConnected 会清空。
 */
void DaemonClient::onDisconnected()
{
    qDebug() << "[client]" << m_name << "disconnected.";
    emit disconnected(m_name);
    /* 启动自动重连 */
    m_reconnectTimer->start();
}

/**
 * @brief 数据就绪回调 — 累积数据并循环解帧
 *
 * Unix domain socket 是面向流的: 一次 readyRead 可能包含:
 *   - 不完整的帧 (数据未到齐)
 *   - 多个完整帧 (粘包)
 *   - 完整帧 + 部分下一帧
 *
 * 处理流程:
 *   1. m_recvBuffer.append(m_socket->readAll())  — 读取所有可用数据
 *   2. while(m_proto.unpack(buffer, msgType, data)) — 循环解帧
 *      - 每次成功解析一个完整帧 → emit messageReceived(msgType, data)
 *      - unpack() 内部消费已解析的字节, 不完整的数据留在 buffer 中
 *   3. 若有残留数据, 留在 m_recvBuffer 等待下一次 readyRead
 */
void DaemonClient::onReadyRead()
{
    /* QLocalSocket::readAll() 读取全部可用数据 */
    m_recvBuffer.append(m_socket->readAll());

    /* 循环解析完整帧 */
    uint8_t msgType;
    QByteArray data;
    while (m_proto.unpack(m_recvBuffer, msgType, data)) {
        emit messageReceived(msgType, data);
    }
}

/**
 * @brief socket 错误回调
 *
 * 执行: 若重连定时器未在运行, 启动之。
 * 典型场景: 守护进程未启动时 connectToServer() 触发 ConnectionRefusedError。
 * 不在此处 emit disconnected — QLocalSocket 会在 error 后自行 emit disconnected,
 * onDisconnected() 也会启动重连定时器, 此处作为双保险。
 */
void DaemonClient::onError(QLocalSocket::LocalSocketError err)
{
    Q_UNUSED(err);
    qDebug() << "[client]" << m_name << "error:" << m_socket->errorString();
    /* 启动重连 (双保险: onDisconnected 也会启动) */
    if (!m_reconnectTimer->isActive())
        m_reconnectTimer->start();
}

/**
 * @brief 重连回调: 重新发起连接
 *
 * 由 m_reconnectTimer (1 秒单次定时器) 触发。
 * 直接调用 connectToServer() 重试, 不区分首次连接还是重连。
 */
void DaemonClient::onReconnect()
{
    qDebug() << "[client]" << m_name << "reconnecting...";
    m_socket->connectToServer(m_sockPath);
}
