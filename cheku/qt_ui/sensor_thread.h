/**
 * @file    sensor_thread.h
 * @brief   温湿度传感器读取线程
 *
 * ============================================================================
 * 设计说明
 * ============================================================================
 * SensorThread 是一个 QThread 子类，在独立线程中持续读取 /dev/mydht11
 * 设备文件获取 DHT11 温湿度传感器的数据。
 *
 * 为什么用独立线程而非 QTimer:
 *   /dev/mydht11 是 DHT11 的内核驱动设备文件，read() 会阻塞直到传感器
 *   完成一次采样。在 UI 线程中阻塞 read() 会导致界面卡死。
 *   独立线程 + isInterruptionRequested() 提供安全的退出机制。
 *
 * 为什么独占 /dev/mydht11:
 *   设备以 O_RDWR | O_NONBLOCK 打开。NONBLOCK 模式确保 read() 在无数据时
 *   立即返回而非阻塞整个线程。打开失败 (fd < 0) 则静默退出，不发送数据。
 *
 * 数据格式 (2 字节):
 *   buf[0] = 湿度 (%RH, 整数)
 *   buf[1] = 温度 (°C,   整数)
 *
 * 读取间隔: 每次成功读取后 msleep(2000)，即约 2 秒更新一次。
 *
 * 线程安全:
 *   dataReady 信号是跨线程的 (Qt::AutoConnection)，Qt 自动将信号投递到
 *   主线程的事件队列，无需手动加锁。
 *
 * ============================================================================
 * 使用方法
 * ============================================================================
 *   SensorThread *st = new SensorThread;
 *   st->setLabels(ui->humLabel, ui->tempLabel);
 *   connect(st, &SensorThread::dataReady, this, [=](int t, int h) {
 *       ui->tempLabel->setText(QString("%1°C").arg(t));
 *       ui->humLabel->setText(QString("%1%").arg(h));
 *   });
 *   st->start();
 *
 *   退出时:
 *     st->requestInterruption();
 *     st->wait(3000);  // 等待最多 3 秒
 */

#ifndef SENSOR_THREAD_H
#define SENSOR_THREAD_H

#include <QThread>
#include <QLabel>

class SensorThread : public QThread
{
    Q_OBJECT
public:
    /**
     * @brief 设置用于显示温湿度的 QLabel 控件指针
     * @param humi 湿度显示标签
     * @param temp 温度显示标签
     *
     * 注意: 这些指针仅用于 run() 中通过 dataReady 信号间接更新。
     *       不在工作线程中直接操作 UI 控件 (Qt 线程安全规则)。
     */
    void setLabels(QLabel *humi, QLabel *temp);

    /**
     * @brief 线程主循环: 打开 /dev/mydht11 并持续读取温湿度
     *
     * 循环逻辑:
     *   1. open("/dev/mydht11", O_RDWR | O_NONBLOCK)
     *   2. 若打开失败 (fd < 0) 直接返回
     *   3. while(!isInterruptionRequested()):
     *        read(fd, buf, 2) → 成功则 emit dataReady(temp, humi), msleep(2000)
     *   4. close(fd)
     *
     * 退出: 外部调用 requestInterruption() + wait() 清理。
     */
    void run() override;

signals:
    /**
     * @brief 温湿度数据就绪信号 (跨线程, 由主线程槽函数接收)
     * @param temp 温度值 (°C, 整数)
     * @param humi 湿度值 (%RH, 整数)
     */
    void dataReady(int temp, int humi);

private:
    QLabel *m_humi = nullptr; /**< 湿度显示标签指针 (主线程 UI 控件) */
    QLabel *m_temp = nullptr; /**< 温度显示标签指针 (主线程 UI 控件) */
};

#endif
