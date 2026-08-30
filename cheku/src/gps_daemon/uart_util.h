/**
 * @file    uart_util.h
 * @brief   UART 串口工具 - 头文件
 *
 * 封装 Linux termios 串口配置, 提供:
 *   - 打开串口设备 (非阻塞)
 *   - 配置波特率 / 数据位 / 停止位 / 校验位 (8N1)
 *   - 设置为原始模式 (raw mode)
 *
 * termios 是 POSIX 标准的终端 IO 接口, Linux 串口通过它来配置:
 *   struct termios {
 *       tcflag_t c_iflag;  // 输入标志
 *       tcflag_t c_oflag;  // 输出标志
 *       tcflag_t c_cflag;  // 控制标志 (波特率/数据位/校验等)
 *       tcflag_t c_lflag;  // 本地标志 (回显/规范模式等)
 *       cc_t     c_cc[NCCS]; // 特殊控制字符 (Vmin/Vtime等)
 *   };
 *
 * 学习要点:
 *   1. termios 结构体: 串口配置的核心
 *   2. cfmakeraw / cfsetispeed / cfsetospeed: 原始模式 & 波特率
 *   3. VMIN / VTIME: 非规范模式下的读取行为控制
 *   4. fcntl O_NONBLOCK: 文件描述符级非阻塞
 */

#ifndef _UART_UTIL_H_
#define _UART_UTIL_H_

#include <stdint.h>
#include <termios.h>    /* speed_t, B9600 */

/**
 * @brief 打开并配置串口 (非阻塞, 8N1, 原始模式)
 *
 * 配置流程:
 *   1. open(device, O_RDWR | O_NOCTTY | O_NONBLOCK)
 *      - O_RDWR:     读写模式 (GPS 只需读, 但某些模块需要写配置命令)
 *      - O_NOCTTY:   不将此设备设为控制终端 (防止意外收到 Ctrl+C)
 *      - O_NONBLOCK: 非阻塞读 (融入 epoll)
 *
 *   2. tcgetattr → 获取当前属性 → 修改 → tcsetattr
 *      - cfmakeraw:  设为原始模式 (关闭回显/行缓冲/信号字符)
 *      - cfsetspeed: 设置输入+输出波特率
 *      - 8N1:        CS8 (8数据位), 无校验, 1停止位 (默认)
 *      - VMIN=0:     非规范模式, 有数据就返回
 *      - VTIME=1:    读取超时 0.1s (在无数据时)
 *
 *   3. fcntl F_SETFL O_NONBLOCK: 确保非阻塞 (read 无数据时返回 EAGAIN)
 *
 * @param device    设备路径 (如 "/dev/ttyUSB0")
 * @param baud_rate  波特率常量 (如 B9600, B115200, 来自 termios.h)
 * @return           成功: fd (≥0), 失败: -1
 *
 * 常见波特率常量:
 *   B4800, B9600, B19200, B38400, B57600, B115200, B230400
 */
int uart_open(const char *device, speed_t baud_rate);

/**
 * @brief 从串口读取数据 (非阻塞)
 *
 * @param fd    串口 fd
 * @param buf   接收缓冲区
 * @param size  缓冲区大小
 * @return      >0=读取字节数, 0=无数据, -1=错误
 */
int uart_read(int fd, uint8_t *buf, uint32_t size);

/**
 * @brief 关闭串口
 *
 * 恢复原始 termios 设置 (如果保存了) 并 close(fd)。
 *
 * @param fd  串口 fd
 */
void uart_close(int fd);

#endif /* _UART_UTIL_H_ */
