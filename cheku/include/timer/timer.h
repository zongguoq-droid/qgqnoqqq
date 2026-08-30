/**
 * @file    timer.h
 * @brief   基于 timerfd 的软件定时器 - 头文件
 *
 * Linux 提供了 timerfd 机制: 将定时器抽象为文件描述符。
 * timerfd 可以加入到 epoll 中统一监听, 实现"一切皆fd"的事件驱动模型。
 *
 * timerfd 的优势:
 *   1. 可以像 Socket 一样被 epoll 监听 (统一事件循环)
 *   2. 支持单次定时 (oneshot) 和周期定时 (periodic)
 *   3. 时间精度高 (纳秒级)
 *   4. 不受系统时间调整影响 (使用单调时钟 CLOCK_MONOTONIC)
 *
 * 与 setitimer/alarm 的对比:
 *   setitimer: 信号方式通知 (SIGALRM), 信号异步处理复杂
 *   timerfd:   fd 可读方式通知 → 自然融入 epoll 事件循环
 *
 * 本项目使用场景:
 *   - gps_daemon:  每秒定时推送 GPS 数据
 *   - sensor_daemon: 每2秒定时读取 DHT11
 *   - net_daemon:   每30秒定时上报数据
 *   - guard_daemon: 每5秒定时检测子进程心跳
 *   - dvr_daemon:   录像文件分段 (每60秒切换新文件)
 *
 * 使用示例:
 *   // 1. 创建定时器
 *   int timer_fd = timer_create(1000);  // 每1000ms触发一次
 *
 *   // 2. 加入 epoll
 *   struct epoll_event ev;
 *   ev.events = EPOLLIN;
 *   ev.data.fd = timer_fd;
 *   epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &ev);
 *
 *   // 3. 在 epoll 循环中处理
 *   if (events[i].data.fd == timer_fd) {
 *       uint64_t expirations;
 *       timer_get_expirations(timer_fd, &expirations);
 *       // 执行定时任务 (如推送 GPS 数据)
 *       LOG_DEBUG("timer", "tick! missed %llu times", expirations);
 *   }
 *
 *   // 4. 关闭定时器
 *   timer_close(timer_fd);
 *
 * 学习要点:
 *   1. timerfd_create / timerfd_settime - Linux 特有的定时器 API
 *   2. CLOCK_MONOTONIC vs CLOCK_REALTIME - 单调时钟 vs 墙上时钟
 *   3. struct itimerspec - 定时器规格 (初始超时 + 间隔)
 *   4. 定时器精度: timerfd 使用高精度定时器 (hrtimer), 纳秒级
 *   5. epoll + timerfd: 统一的文件描述符事件循环
 */

#ifndef _TIMER_H_
#define _TIMER_H_

#include <stdint.h>

/**
 * @brief 创建定时器 (周期模式)
 *
 * 创建一个每 interval_ms 毫秒触发一次的定时器。
 *
 * @param interval_ms  定时间隔 (毫秒), 最小 1ms
 * @return             成功: timer fd, 失败: -1
 *
 * 定时器创建后不会自动开始，需要调用 timer_start() 启动。
 * 这样设计是为了让调用者先加入 epoll 再启动, 避免丢失事件。
 */
int timerfd_create_ms(uint32_t interval_ms);  /* 注意: 不能叫 timer_create, 与 POSIX 冲突 */

/**
 * @brief 启动定时器
 *
 * 可以在创建后立即启动, 也可以延迟启动 (如 first_timeout_ms 参数)。
 *
 * @param fd                timer fd
 * @param first_timeout_ms  首次触发延迟 (ms)，0=立即按周期触发
 * @return                  0=成功, -1=失败
 *
 * 示例:
 *   timer_start(fd, 0);       // 立即开始, 按创建时的周期触发
 *   timer_start(fd, 5000);    // 首次5秒后触发, 之后按周期
 */
int timer_start(int fd, uint32_t first_timeout_ms);

/**
 * @brief 读取并清空定时器到期次数
 *
 * timerfd 到期时, 内核向 fd 写入一个 64 位无符号整数,
 * 表示从上次读取到本次读取之间到期的次数。
 *
 * 正常情况下每次返回 1 (没有错过定时),
 * 如果 CPU 繁忙导致处理延迟, 可能返回 > 1 (表示错过了多次)。
 *
 * 必须读取! 否则 epoll 会持续触发 (因为 fd 一直可读)。
 *
 * @param fd           timer fd
 * @param expirations  输出: 到期次数
 * @return             0=成功, -1=失败
 */
int timer_get_expirations(int fd, uint64_t *expirations);

/**
 * @brief 关闭定时器
 * @param fd  timer fd
 */
void timer_close(int fd);

#endif /* _TIMER_H_ */
