/**
 * @file    sensor_thread.cpp
 * @brief   温湿度传感器读取线程 — 实现
 *
 * DHT11 传感器通过内核驱动暴露为 /dev/mydht11 字符设备。
 * 每次 read() 返回 2 字节: buf[0]=湿度(%RH), buf[1]=温度(°C)。
 *
 * 线程生命周期:
 *   start()  → run() 开始循环读取
 *   requestInterruption() → 设置中断标志
 *   run() 检测到标志 → close(fd) → 线程退出
 *   wait(3000) → 主线程等待最多 3 秒确认退出
 */

#include "sensor_thread.h"
#include <fcntl.h>
#include <unistd.h>

/**
 * @brief 保存主线程 UI 标签指针 (不在工作线程中直接操作)
 */
void SensorThread::setLabels(QLabel *humi, QLabel *temp)
{
    m_humi = humi;
    m_temp = temp;
}

/**
 * @brief 线程主循环: 打开 /dev/mydht11 并持续读取温湿度
 *
 * 设备打开:
 *   - 路径: /dev/mydht11 (DHT11 内核驱动字符设备)
 *   - 标志: O_RDWR | O_NONBLOCK
 *     O_RDWR:    读写模式 (驱动要求)
 *     O_NONBLOCK: 非阻塞模式, read() 无数据时立即返回 -1 而非阻塞线程
 *
 * 读取循环:
 *   1. read(fd, buf, 2) — 尝试读取 2 字节
 *      - 返回 2: 成功读取完整数据
 *        buf[0] = 湿度 (%RH)
 *        buf[1] = 温度 (°C)
 *        → emit dataReady(temperature, humidity)
 *        → msleep(2000) 等待传感器下一次采样
 *      - 返回其他值: 数据未就绪或错误, 不 sleep 直接重试
 *
 *   2. isInterruptionRequested() — 检查退出标志
 *      由父线程调用 requestInterruption() 设置, run() 检测到后跳出循环
 *
 * 清理:
 *   close(fd) — 释放设备文件描述符
 */
void SensorThread::run()
{
    /* 打开 DHT11 设备文件 (非阻塞模式) */
    int fd = open("/dev/mydht11", O_RDWR | O_NONBLOCK);
    if (fd < 0) return;  /* 设备不可用, 静默退出 */

    char buf[2];  /* buf[0]=湿度, buf[1]=温度 */
    while (!isInterruptionRequested()) {
        if (read(fd, buf, 2) == 2) {
            /* 发送温湿度数据到主线程:
             *   dataReady(temp = buf[1], humi = buf[0])
             *   unsigned char 转换确保无符号扩展 */
            emit dataReady((unsigned char)buf[1], (unsigned char)buf[0]);
            msleep(2000);  /* 等待 ~2 秒后进行下一次读取 */
        }
        /* 读取失败或数据未就绪 → 立即重试 (NONBLOCK 模式不会阻塞) */
    }
    close(fd);
}
