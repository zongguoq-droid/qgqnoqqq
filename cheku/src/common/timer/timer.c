/**
 * @file    timer.c
 * @brief   基于 timerfd 的软件定时器 - 实现
 *
 * timerfd 是 Linux 2.6.25+ 引入的机制, 它将 POSIX 定时器
 * 和文件描述符结合在一起。
 *
 * 核心 API 链路:
 *
 * 1. timerfd_create(clockid, flags)
 *      clockid:
 *        CLOCK_MONOTONIC  — 单调时钟, 不受系统时间修改影响 (推荐!)
 *        CLOCK_REALTIME   — 墙上时钟, 受 NTP/手动调整影响
 *      flags:
 *        TFD_NONBLOCK     — 非阻塞模式
 *        TFD_CLOEXEC      — exec 时自动关闭 (防止泄露到子进程)
 *
 * 2. timerfd_settime(fd, flags, new_value, old_value)
 *      flags:
 *        0               — 相对时间 (从调用时刻开始计时)
 *        TFD_TIMER_ABSTIME— 绝对时间 (指定具体的到期时间点)
 *      new_value: 类型 struct itimerspec
 *        it_value:      首次到期时间
 *        it_interval:   之后每次到期的时间间隔
 *
 *      struct itimerspec 使用秒+纳秒的组合:
 *        struct timespec tv_sec;   (秒)
 *        struct timespec tv_nsec;  (纳秒, 0~999999999)
 *
 * 3. read(fd, &count, sizeof(uint64_t))
 *      定时器到期时, fd 变为可读。
 *      读取获得到期次数 (unsigned 64-bit)。
 *      如果不读取, epoll 会一直触发 (因为 fd 仍可读)!
 *
 * 为什么必须 read()?
 *   timerfd 使用"事件计数"机制:
 *     - 每次到期, 内核内部计数器 +1
 *     - read 读取并清空计数器
 *     - fd 可读 = 计数器 > 0
 *   epoll 看到 fd 可读 → 必须 read → 否则下一轮 epoll_wait 立刻返回
 *
 * CLOCK_MONOTONIC vs CLOCK_REALTIME 的选择:
 *   - 嵌入式设备可能没有 NTP, 系统时间不准确
 *   - CLOCK_MONOTONIC 从系统启动开始计时, 不受任何调整影响
 *   - 对于周期性的数据采集/上报, MONOTONIC 是正确的选择
 *   - 如果需要"每天9点上报数据" → 用 CLOCK_REALTIME
 *   - 本项目所有定时任务都是周期性, 用 MONOTONIC
 *
 * 学习要点:
 *   1. timerfd_create / timerfd_settime: Linux 特有的定时器 API
 *   2. struct itimerspec: 秒+纳秒的定时器规格
 *   3. 为什么需要 read(): 事件计数机制
 *   4. CLOCK_MONOTONIC 与 CLOCK_REALTIME 的选择
 *   5. 定时器精度: timerfd 底层使用高精度定时器 (hrtimer)
 */

#include "timer.h"

#include <unistd.h>         /* close, read */
#include <stdio.h>          /* perror */
#include <stdlib.h>         /* NULL */
#include <string.h>         /* memset */
#include <errno.h>          /* errno */
#include <sys/timerfd.h>    /* timerfd_create, timerfd_settime, TFD_* */
#include <time.h>           /* clock_gettime, CLOCK_MONOTONIC, timespec */

/* ================================================================
 *  内部: fd → interval 映射表
 *
 *  timer_create 创建的每个定时器都需要记住其周期,
 *  以便 timer_start 时自动设为周期模式。
 *
 *  使用简单的静态数组, 容量足够本项目所有定时器使用。
 *  (8个后台进程, 每进程通常 1~2 个定时器, 总数 < 32)
 * ================================================================ */
#define TIMER_MAX_COUNT  32

static struct {
    int      fd;
    uint32_t interval_ms;
    int      used;          /* 0=空闲槽位, 1=已占用 */
} g_timer_registry[TIMER_MAX_COUNT];

/**
 * @brief 注册定时器 (内部)
 * @return  注册的槽位索引, -1=表满
 */
static int timer_register(int fd, uint32_t interval_ms)
{
    int i;
    for (i = 0; i < TIMER_MAX_COUNT; i++) {
        if (!g_timer_registry[i].used) {
            g_timer_registry[i].fd          = fd;
            g_timer_registry[i].interval_ms = interval_ms;
            g_timer_registry[i].used        = 1;
            return i;
        }
    }
    return -1;  /* 表满 */
}

/**
 * @brief 查找定时器对应的间隔 (内部)
 * @return  间隔 (ms), -1=未找到
 */
static int timer_lookup_interval(int fd)
{
    int i;
    for (i = 0; i < TIMER_MAX_COUNT; i++) {
        if (g_timer_registry[i].used && g_timer_registry[i].fd == fd) {
            return (int)g_timer_registry[i].interval_ms;
        }
    }
    return -1;
}

/**
 * @brief 注销定时器 (内部)
 */
static void timer_unregister(int fd)
{
    int i;
    for (i = 0; i < TIMER_MAX_COUNT; i++) {
        if (g_timer_registry[i].used && g_timer_registry[i].fd == fd) {
            g_timer_registry[i].used = 0;
            g_timer_registry[i].fd   = -1;
            return;
        }
    }
}

/**
 * @brief 将毫秒转换为 struct itimerspec
 *
 * struct itimerspec 使用秒+纳秒表示时间, 而用户接口使用毫秒。
 * 这个辅助函数完成转换。
 *
 * 例如: 1500ms = 1秒 + 500,000,000纳秒
 *
 * @param first_ms    首次超时 (ms)
 * @param interval_ms 定时间隔 (ms)
 * @param its         输出: struct itimerspec
 */
static void ms_to_itimerspec(uint32_t first_ms, uint32_t interval_ms,
                             struct itimerspec *its)
{
    /* 首次超时 */
    its->it_value.tv_sec  = first_ms / 1000;
    its->it_value.tv_nsec = (long)(first_ms % 1000) * 1000000L;

    /* 间隔时间 */
    its->it_interval.tv_sec  = interval_ms / 1000;
    its->it_interval.tv_nsec = (long)(interval_ms % 1000) * 1000000L;
}

/* ================================================================
 *  API 实现
 * ================================================================ */

int timerfd_create_ms(uint32_t interval_ms)
{
    int fd;

    if (interval_ms == 0) {
        return -1;  /* 不允许0间隔 (会变成忙等) */
    }

    /* timerfd_create:
     *   CLOCK_MONOTONIC: 单调时钟, 不受系统时间修改影响
     *   TFD_NONBLOCK:    非阻塞模式 (融入 epoll 事件循环)
     *   TFD_CLOEXEC:    fork+exec 时自动关闭, 防止泄露到子进程
     *
     * 注意: TFD_CLOEXEC 是 Linux 特性, 等价于 fcntl(fd, F_SETFD, FD_CLOEXEC)
     *       如果编译环境不支持, 可以用 0 代替, 然后手动 fcntl */
    fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd == -1) {
        perror("timerfd_create");
        return -1;
    }

    /* 注册 fd → interval 映射 (timer_start 需要此信息来设置周期模式) */
    if (timer_register(fd, interval_ms) < 0) {
        close(fd);
        return -1;  /* 注册表满 (极端情况) */
    }

    /* 仅创建, 不启动。调用 timer_start() 启动。 */
    return fd;
}

int timer_start(int fd, uint32_t first_timeout_ms)
{
    struct itimerspec its;
    int interval_ms;

    if (fd < 0) {
        return -1;
    }

    /* 查找 timer_create 时注册的定时间隔
     *   找到了 → 设为周期模式 (it_interval = 查到的间隔)
     *   未找到 → 单次触发模式 (it_interval = 0)
     *
     * 这样调用者只需:
     *   fd = timer_create(1000);     // 创建, 指定周期 1000ms
     *   timer_start(fd, 0);          // 立即开始周期触发
     *   timer_start(fd, 5000);       // 首次 5s 后触发, 之后每 1s */
    interval_ms = timer_lookup_interval(fd);
    if (interval_ms < 0) {
        interval_ms = 0;  /* 未注册的 fd → 单次模式 */
    }

    /* 如果 first_timeout_ms 为 0, 使用周期时间作为首次超时 */
    if (first_timeout_ms == 0) {
        first_timeout_ms = (uint32_t)interval_ms;
    }

    ms_to_itimerspec(first_timeout_ms, (uint32_t)interval_ms, &its);

    /* timerfd_settime:
     *   参数2 flags=0: 相对时间 (从现在起算)
     *   参数3 new_value: 新的定时器设置
     *   参数4 old_value: NULL=不关心旧设置
     *
     *   it_interval != 0 → 周期触发
     *   it_interval == 0 → 单次触发 (oneshot) */
    if (timerfd_settime(fd, 0, &its, NULL) == -1) {
        perror("timerfd_settime");
        return -1;
    }

    return 0;
}

int timer_get_expirations(int fd, uint64_t *expirations)
{
    ssize_t ret;

    if (fd < 0 || !expirations) {
        return -1;
    }

    /* 读取到期次数:
     *   read 在 timerfd 上是特殊的 — 内核返回一个 uint64_t
     *   这是 Linux 特有的机制, 不同于普通文件的 read。
     *
     *   缓冲区必须是 8 字节 (sizeof(uint64_t)) 或更大。
     *   如果提供的缓冲区小于 8 字节, read 返回 -1 (errno=EINVAL)。 */
    ret = read(fd, expirations, sizeof(uint64_t));
    if (ret == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* 没有到期事件 (可能被 epoll 误触发或其他原因)
             * 这不是错误, 返回 0 次到期 */
            *expirations = 0;
            return 0;
        }
        perror("timerfd read");
        return -1;
    }

    /* 检查返回值:
     *   应该等于 sizeof(uint64_t) = 8
     *   如果不是... 几乎不可能发生 */
    if (ret != sizeof(uint64_t)) {
        return -1;
    }

    return 0;
}

void timer_close(int fd)
{
    if (fd >= 0) {
        timer_unregister(fd);  /* 从注册表中移除 */
        /* 关闭 timerfd: close 会自动停止定时器
         * 不需要先 timerfd_settime 停止 */
        close(fd);
    }
}
