/**
 * @file    guard_daemon.c
 * @brief   系统守护进程 (poll 模式) — 启动/监控/重启子进程
 *
 * guard_daemon 是整个车载终端系统的"总管"进程，负责:
 *
 *   1. 守护进程化 (daemonize)
 *      脱离终端，成为独立的系统服务，不受 SSH/串口会话影响。
 *
 *   2. 子进程生命周期管理
 *      启动时按顺序 fork+exec 所有子进程 (gps/dvr/av/input/canbus)，
 *      通过 SIGCHLD + waitpid(WNOHANG) 回收退出子进程，
 *      对异常退出的子进程执行自动重启 (带速率限制防止重启风暴)。
 *
 *   3. 系统健康监控
 *      定期解析 /proc 文件系统获取 CPU/内存/负载/温度/磁盘空间信息,
 *      通过 Socket 对外提供系统状态查询服务。
 *
 *   4. IPC 服务端
 *      创建 UNIX Domain Socket 监听 /tmp/car_guard.sock,
 *      接受 Qt UI 客户端连接, 响应 MSG_SERVICE_STATUS 等系统消息。
 *
 * 主循环架构 (poll 模式):
 *   poll([server_fd, client_fd], timeout=5s) 等待事件
 *     → timeout 到期: 执行心跳 (更新系统状态)
 *     → server_fd POLLIN: accept 新客户端
 *     → client_fd POLLIN: 接收并分发消息
 *   waitpid(WNOHANG): 每轮循环回收已退出的子进程
 *
 * 信号处理:
 *   SIGCHLD → 默认处理 (SIG_DFL): 子进程退出变为僵尸, 由 waitpid 回收
 *   SIGTERM → sig_handler: 设置 g_running=0, 触发优雅退出
 *   SIGINT  → sig_handler: 同上 (方便前台调试模式 -f)
 */

#include "guard_daemon.h"
#include "log/log.h"
#include "config/config.h"
#include "socket/unix_socket.h"
#include "protocol/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>

/* 透传给子进程的配置文件路径, 由 main() 根据命令行参数设置。
 * guard_child_start() 在 execlp 时读取它, 子进程因此与 guard 使用同一份配置。 */
static char g_conf_path[256] = GUARD_DEFAULT_CONF_PATH;
#include <time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/statfs.h>

static guard_context_t g_ctx;
static volatile int g_running = 1;
static void sig_handler(int s) { (void)s; g_running = 0; }

/* ================================================================
 *  守护进程化 (daemonize) — 双 fork 经典模式
 *
 *  目的: 将当前进程转变为不受终端控制的系统守护进程。
 *
 *  步骤分解:
 *   1. fork() → 父进程 _exit(0)
 *      子进程不再是进程组组长 (process group leader),
 *      这是调用 setsid() 的前提条件 (进程组长调用 setsid 会失败)。
 *
 *   2. setsid()
 *      子进程创建新会话 (session) 并成为会话组长,
 *      同时脱离原来的控制终端 (controlling terminal)。
 *      setsid 之后, 子进程不再接收终端的 SIGHUP 信号。
 *
 *   3. fork() → 父进程 _exit(0)  [第二次 fork]
 *      孙进程不再是会话组长, 因此永远无法重新获取控制终端
 *      (只有会话组长可以通过 open() 一个 tty 获取控制终端)。
 *      这提供了一道额外的"隔离防线"。
 *
 *   4. chdir("/")
 *      切换到根目录, 避免占用某个挂载点导致 umount 失败。
 *
 *   5. umask(0)
 *      重置文件创建掩码, 确保守护进程创建的文件/目录权限
 *      完全由 open()/creat() 参数决定, 不受继承的 umask 影响。
 *
 *   6. stdin/stdout/stderr → /dev/null
 *      重定向标准 IO 到空设备, 防止:
 *        - printf 写入已关闭的终端导致 SIGPIPE
 *        - 被后台的其他进程意外读取
 *        - 日志输出与终端输出混淆 (日志应走 log 模块)
 *
 *   调试模式:
 *     通过 -f (foreground) 参数跳过 daemonize,
 *     标准 IO 保持连接, 方便在终端直接观察输出。
 * ================================================================ */
void guard_daemonize(void)
{
    /* 第一次 fork: 父进程退出, shell 收回控制权 */
    if (fork() > 0) _exit(0);

    /* 创建新会话, 脱离终端 */
    setsid();

    /* 第二次 fork: 确保进程永远无法获取控制终端 */
    if (fork() > 0) _exit(0);

    /* 切换到根目录, 避免占用挂载点 */
    chdir("/");

    /* 重置文件创建掩码 */
    umask(0);

    /* 重定向 stdin(0), stdout(1), stderr(2) 到 /dev/null */
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, 0);  /* stdin  → /dev/null (读返回 EOF) */
        dup2(fd, 1);  /* stdout → /dev/null (写被丢弃) */
        dup2(fd, 2);  /* stderr → /dev/null (写被丢弃) */
        if (fd > 2) close(fd);  /* 关闭原始 fd, 保留重定向后的 0/1/2 */
    }
}

/* ================================================================
 *  子进程管理
 *
 *  fork + exec 是 UNIX 中创建新进程的唯一方法 (除 vfork/clone 外):
 *    fork():   复制当前进程 (地址空间/文件描述符/信号处理等)
 *              → 父进程中返回子进程 PID, 子进程中返回 0
 *    exec():   用新程序替换当前进程的地址空间
 *              → 成功则永不返回 (进程映像被替换), 失败返回 -1
 *
 *  fork 的写时复制 (Copy-On-Write, COW):
 *    现代 Linux 内核不会在 fork 时立即复制整个地址空间,
 *    而是让父子进程共享物理页并标记为只读。
 *    只有当一方尝试写入时, 内核才复制该页 (COW)。
 *    这大大降低了 fork 的开销, 尤其是大进程 fork 后立即 exec 的场景。
 *
 *  execlp vs execvp vs execve:
 *    execlp: 按 PATH 搜索可执行文件, 参数为可变长列表 (NULL 结尾)
 *    execvp: 按 PATH 搜索, 参数为 char*[] 数组
 *    execve: 不搜索 PATH (需完整路径), 参数为数组, 可指定环境变量
 *    本项目用 execlp 因为它最简单且 PATH 中能找到目标程序。
 *
 *  子进程终止:
 *    子进程退出后变为僵尸 (zombie), 保留 PID/退出状态等信息,
 *    由父进程通过 waitpid 回收 (reap)。如果不回收会累积僵尸进程,
 *    占用 PID 表项, 最终可能导致 fork 失败 (errno=EAGAIN)。
 * ================================================================ */
int guard_child_start(child_process_t *child)
{
    if (!child || !child->bin_path[0]) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        /* fork 失败: 可能原因:
         *   EAGAIN: 达到 RLIMIT_NPROC 进程数上限 或 内核内存不足
         *   ENOMEM: 内核无法分配必要的内核结构 */
        LOG_ERROR("guard","fork: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* ---- 子进程 ---- */
        /* execlp: 在 PATH 中查找 bin_path, 以该名称作为 argv[0] 启动
         * 成功: 当前进程被替换, 这行之后不会执行
         * 失败: 返回 -1 (如文件不存在/无执行权限)
         *
         * argv[1] 透传配置文件路径, 使子进程与 guard 使用同一份配置
         * (等价于脚本中手动执行 "gps_daemon /etc/car_terminal/config.ini")。
         * 路径为空时不传该参数, 子进程回退到自身内置默认配置。 */
        if (g_conf_path[0] != '\0') {
            execlp(child->bin_path, child->bin_path, g_conf_path, NULL);
        } else {
            execlp(child->bin_path, child->bin_path, NULL);
        }

        /* exec 失败才走到这里 — 必须用 _exit() 而非 exit()
         * _exit(1): 直接终止进程, 不执行 atexit 回调/stdio 缓冲区刷新
         * exit(1):  会执行 atexit 回调并刷新 stdio 缓冲区,
         *            可能产生副作用 (如污染父进程继承的文件描述符) */
        _exit(1);
    }

    /* ---- 父进程 ---- */
    child->pid = pid;
    child->state = PROC_RUNNING;
    child->start_time = time(NULL);

    LOG_INFO("guard","Started %s (PID=%d)", child->name, pid);
    return 0;
}

/**
 * @brief 优雅终止子进程: SIGTERM → 等待 → SIGKILL
 *
 * 终止策略 (两阶段):
 *   阶段1 — SIGTERM (可捕获信号, 优雅退出):
 *     子进程如果注册了 SIGTERM 处理函数, 可以执行清理工作
 *     (关闭文件/释放锁/保存状态) 然后自行退出。
 *     等待最多 3 秒 (30 次 × 100ms), 每 100ms 检查一次。
 *
 *   阶段2 — SIGKILL (不可捕获, 强制杀死):
 *     如果 SIGTERM 后 3 秒仍未退出, 说明子进程可能卡在
 *     不可中断的系统调用 (如阻塞 IO) 或死循环中。
 *     SIGKILL 由内核直接终止进程, 进程无法捕获或忽略。
 *     waitpid(..., 0) 阻塞等待直到进程确实被回收。
 *
 * 注意: waitpid 返回 0 表示进程还未退出 (WNOHANG 模式),
 *       返回 pid 表示已回收, 返回 -1 表示错误 (如 ESRCH 进程不存在)。
 */
void guard_child_stop(child_process_t *child)
{
    if (!child || child->pid <= 0) return;

    /* 阶段1: 发送 SIGTERM, 等待至多 3 秒 */
    kill(child->pid, SIGTERM);

    int status, wait_cnt = 0;
    while (waitpid(child->pid, &status, WNOHANG) == 0 && wait_cnt < 30) {
        usleep(100000);  /* 100ms */
        wait_cnt++;
    }

    /* 阶段2: 仍未退出, 使用 SIGKILL 强制终止 */
    if (waitpid(child->pid, &status, WNOHANG) == 0) {
        LOG_WARN("guard","%s did not respond to SIGTERM, sending SIGKILL", child->name);
        kill(child->pid, SIGKILL);
        waitpid(child->pid, &status, 0);  /* 阻塞等待, 确保回收 */
    }

    child->pid = 0;
    child->state = PROC_STOPPED;
    LOG_INFO("guard","Stopped %s", child->name);
}

/**
 * @brief 重启子进程 (stop + start) — 带速率限制防重启风暴
 *
 * 重启风暴 (restart storm) 问题:
 *   如果子进程因致命错误 (如硬件故障/配置错误) 启动后立即崩溃,
 *   无条件重启会导致进程不断 fork→crash→fork→crash 循环,
 *   消耗 CPU 资源并产生大量日志, 甚至可能影响整个系统稳定性。
 *
 * 解决方案 — 滑动窗口速率限制:
 *   在 GUARD_RESTART_WINDOW_SEC (60秒) 的滑动时间窗口内,
 *   如果重启次数超过 GUARD_MAX_RESTART (5次), 说明子进程
 *   出现了短时间内无法恢复的故障, 停止重启并标记为 PROC_ERROR。
 *
 *   窗口过期后 (距首次重启超过 60 秒), 重置计数器,
 *   允许新一轮重启尝试 (可能是暂时故障后恢复了)。
 *
 * 两次重启之间 sleep 500ms: 给前一个进程的 socket 文件/SIGCHLD
 * 足够的清理时间, 避免竞态条件。
 */
void guard_child_restart(child_process_t *child)
{
    if (!child) return;

    time_t now = time(NULL);

    /* 滑动窗口管理: 距首次重启超过窗口时间 → 重置计数 */
    if (child->restart_count == 0 ||
        now - child->first_restart > GUARD_RESTART_WINDOW_SEC) {
        child->restart_count = 0;
        child->first_restart = now;
    }

    child->restart_count++;

    /* 超过最大重启次数 → 放弃, 标记为异常状态 */
    if (child->restart_count > GUARD_MAX_RESTART) {
        /* 注意: 格式串必须用 %d 而非 %ld —
         * GUARD_MAX_RESTART / GUARD_RESTART_WINDOW_SEC 都是 int 常量,
         * 在 LP64 平台 (如 x86_64 Linux) 上 long 为 8 字节而 int 为 4 字节,
         * 若用 %ld 读取 int 实参会多读 4 字节, 属于未定义行为 (输出垃圾值)。 */
        LOG_ERROR("guard","%s: max restart exceeded (%d in %ds), stopped",
                  child->name, GUARD_MAX_RESTART, GUARD_RESTART_WINDOW_SEC);
        child->state = PROC_ERROR;
        return;
    }

    LOG_WARN("guard","Restarting %s (%d/%d)",
             child->name, child->restart_count, GUARD_MAX_RESTART);

    /* 停止旧进程 (SIGTERM → wait → SIGKILL) */
    guard_child_stop(child);

    /* 等待 500ms 确保旧进程的 socket/资源完全释放 */
    usleep(500000);

    /* 启动新进程 (fork + exec) */
    guard_child_start(child);
}

/* ================================================================
 *  /proc 文件系统监控 — 采集系统健康指标
 *
 * Linux /proc 是一个伪文件系统 (procfs), 内核通过文件接口导出
 * 运行时信息。读取 /proc 文件不会产生磁盘 IO, 内核在每次读取时
 * 动态生成数据。这是用户空间程序获取系统状态的标准方式。
 *
 * 采集指标:
 *   CPU 使用率:  /proc/stat  → 计算两次采样间的时间差
 *   内存使用率:  /proc/meminfo → MemTotal / MemFree
 *   系统负载:    /proc/loadavg → 1分钟平均负载
 *   CPU 温度:   /sys/class/thermal/thermal_zone0/temp (毫摄氏度)
 *   磁盘空间:   statfs() 系统调用 → eMMC 和 SD 卡剩余空间
 * ================================================================ */
static void update_status(system_status_t *st)
{
    /* ---- CPU 使用率 (通过 /proc/stat 差值计算) ----
     *
     * /proc/stat 第一行格式: "cpu  user nice system idle iowait irq softirq steal"
     * 各字段为自开机以来的累计 jiffies (通常 1 jiffy = 10ms)。
     *
     * CPU 使用率 = (总差值 - idle差值) / 总差值 × 100%
     *
     * 为什么用差值? /proc/stat 给出的是累计值, 我们需要的是
     * "过去5秒内的平均使用率", 因此要保存上次的值并计算差值。
     *
     * static 变量 pt (previous total), pi (previous idle)
     * 在函数调用间保持值不变, 用于计算两次采样间的增量。 */
    {
        static long pt = 0, pi = 0;  /* 上次的 total 和 idle 值 */
        FILE *f = fopen("/proc/stat", "r");
        if (f) {
            char l[8];
            long u, n, s, i, io, ir, si, stl;
            fscanf(f, "%s %ld %ld %ld %ld %ld %ld %ld %ld",
                   l, &u, &n, &s, &i, &io, &ir, &si, &stl);
            fclose(f);

            /* 计算总 jiffies 和 idle jiffies */
            long tot = u + n + s + i + io + ir + si + stl;
            long td  = tot - pt;   /* 总时间差 */
            long id  = i - pi;     /* idle 时间差 */
            pt = tot;
            pi = i;

            /* CPU% = 非idle时间 / 总时间 */
            st->cpu_percent = td ? (float)(td - id) * 100.0f / td : 0.0f;
        }
    }

    /* ---- 内存使用率 (通过 /proc/meminfo) ----
     *
     * /proc/meminfo 是 key:value 格式, 单位为 kB。
     * 只读我们关心的两行: MemTotal 和 MemFree。
     * 内存使用率 = (Total - Free) / Total × 100% */
    {
        FILE *f = fopen("/proc/meminfo", "r");
        if (f) {
            char l[64];
            long v;
            while (fscanf(f, "%s %ld", l, &v) == 2) {
                if (!strcmp(l, "MemTotal:"))  st->mem_total_kb = (int)v;
                if (!strcmp(l, "MemFree:"))   st->mem_free_kb  = (int)v;
            }
            fclose(f);
        }
        st->mem_percent = st->mem_total_kb
            ? (float)(st->mem_total_kb - st->mem_free_kb) * 100.0f
                 / st->mem_total_kb
            : 0.0f;
    }

    /* ---- 系统负载 (通过 /proc/loadavg) ----
     *
     * 格式: "0.15 0.10 0.05 1/120 1234"
     *   字段1: 1分钟平均负载 (可运行+不可中断的进程数)
     *   字段2: 5分钟平均负载
     *   字段3: 15分钟平均负载
     *   字段4: 当前运行进程数/总进程数
     *   字段5: 最近创建的 PID */
    {
        FILE *f = fopen("/proc/loadavg", "r");
        if (f) {
            fscanf(f, "%f", &st->load_1min);
            fclose(f);
        }
    }

    /* ---- CPU 温度 (通过 sysfs) ----
     *
     * /sys/class/thermal/thermal_zone0/temp 存储温度值,
     * 单位为毫摄氏度 (milli-degree Celsius) — 需要除以 1000。
     * 不同的 SoC 可能有不同的 thermal_zone 编号。 */
    {
        FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
        if (f) {
            int t;
            fscanf(f, "%d", &t);
            fclose(f);
            st->cpu_temp = (float)t / 1000.0f;
        }
    }

    /* ---- 磁盘剩余空间 (通过 statfs 系统调用) ----
     *
     * statfs 获取文件系统统计信息, 比 df 命令更高效 (无需 fork)。
     * struct statfs 关键字段:
     *   f_bsize:  文件系统块大小 (通常 4096)
     *   f_bavail: 非 root 用户可用块数 (包含为 root 保留的块)
     *
     * 剩余 MB = 可用块数 × 块大小 / (1024 × 1024) */
    {
        struct statfs fs;

        /* eMMC: 根文件系统 */
        if (statfs("/", &fs) == 0) {
            st->emmc_free_mb = (long)((uint64_t)fs.f_bavail
                                      * (uint64_t)fs.f_bsize
                                      / 1048576);
        }

        /* SD 卡: 如果有挂载 */
        if (statfs("/mnt/sdcard", &fs) == 0) {
            st->sd_free_mb = (long)((uint64_t)fs.f_bavail
                                   * (uint64_t)fs.f_bsize
                                   / 1048576);
        }
    }

    st->update_time = time(NULL);
}

/* ================================================================
 *  Socket 事件处理 — poll 事件分发
 *
 *  guard 作为 IPC 服务端, 维护两个 socket fd:
 *    server_fd: 监听 Socket, 接受新客户端连接 (仅 POLLIN)
 *    client_fd: 已连接的客户端, 收发消息 (POLLIN|POLLHUP|POLLERR)
 *
 *  设计规定: 同时间只接受一个客户端连接。
 *    如果已有客户端连接且新客户端到来, 直接关闭新连接。
 *
 *  消息处理流程:
 *    1. recv 原始字节 → ringbuffer (接收缓冲区)
 *    2. protocol_unpack 逐个解帧 → 消息类型 + 数据
 *    3. 根据消息类型分发处理
 * ================================================================ */
static void handle_sockets(guard_context_t *ctx, int srv_ev, int cli_ev)
{
    /* ---- 处理新连接 ---- */
    if (srv_ev & POLLIN) {
        int fd = sock_accept(ctx->server_fd);
        if (fd >= 0) {
            if (ctx->client_fd >= 0) {
                /* 已有客户端连接, 拒绝新连接 */
                sock_close(fd, NULL);
            } else {
                ctx->client_fd = fd;
                LOG_INFO("guard","UI connected (fd=%d)", fd);
            }
        }
    }

    /* ---- 处理客户端消息 ---- */
    if (ctx->client_fd < 0 || !(cli_ev & (POLLIN | POLLHUP | POLLERR)))
        return;

    uint8_t buf[512];
    int n = sock_recv(ctx->client_fd, buf, sizeof(buf));

    /* n <= 0: 无数据 (EAGAIN), 对端关闭 (n=0), 或错误 (n=-1)
     * 对端关闭时断开连接, 等待 UI 重新连接 */
    if (n <= 0) {
        sock_close(ctx->client_fd, NULL);
        ctx->client_fd = -1;
        LOG_INFO("guard","UI disconnected");
        return;
    }

    /* 写入接收 ringbuffer, 供 protocol_unpack 解帧 */
    ringbuffer_write(&ctx->recv_rb, buf, (uint32_t)n);

    /* 循环解帧: ringbuffer 中可能包含多个完整的消息帧,
     * 每个帧由 protocol_unpack 解析出 msg_type+data */
    uint8_t  t;        /* 消息类型 */
    uint8_t  d[256];   /* 消息数据载荷 */
    uint32_t l;        /* 数据长度 */
    while (protocol_unpack(&ctx->proto_parser, &ctx->recv_rb,
                           &t, d, sizeof(d), &l) == 0) {
        /* 目前只处理 MSG_SERVICE_STATUS 查询:
         * UI 请求系统状态 → 回复当前采集的系统健康数据 */
        if (t == MSG_SERVICE_STATUS && ctx->client_fd >= 0) {
            sock_send_frame(ctx->client_fd, MSG_SERVICE_STATUS,
                            (uint8_t*)&ctx->sys_status,
                            sizeof(ctx->sys_status));
        }
        /* 其他消息类型在此扩展... */
    }
}

/* ================================================================
 *  构建子进程表 — 定义需要管理的后台进程列表
 *
 *  每个子进程的配置:
 *    name:            进程显示名称 (用于日志)
 *    bin_path:        可执行文件路径 = GUARD_BIN_PREFIX + name
 *                     例如: "/bin/gps_daemon"
 *    auto_restart:    异常退出后是否自动重启 (所有 daemon 都开启)
 *    startup_delay_ms: 启动延迟 (错开启动避免资源争抢)
 *                     第 i 个子进程延迟 i*500ms, 防止所有进程同时启动
 *                     导致 CPU/IO 尖峰 (thundering herd)。
 *
 *  启动顺序 (Startup Order):
 *    0ms:    gps_daemon    (GPS/定位最先启动)
 *    500ms:  dvr_daemon    (录像服务)
 *    1000ms: av_daemon     (音视频服务)
 *    1500ms: input_daemon  (按键输入服务)
 *    2000ms: canbus_daemon (CAN总线服务)
 *
 *  待扩展: net_daemon (网络管理), sensor_daemon (传感器采集)
 * ================================================================ */
/* ================================================================
 *  内置子进程定义表
 *
 *  顺序即启动顺序 (按依赖关系排列), 延迟为累积值,
 *  避免所有进程同时启动造成 CPU/IO 尖峰 (thundering herd)。
 *
 *  enabled=0 的条目为预留槽位:
 *    sensor_daemon — 功能已由 Qt UI 的 SensorThread 直接读取
 *                    /dev/mydht11 实现, 不再需要独立进程
 *    net_daemon    — 云端通信功能尚未部署
 *
 *  这两个条目仍然出现在进程表中, 以便:
 *    1. UI 的服务健康面板能完整展示系统规划中的服务
 *    2. 未来部署时只需在配置文件中改为 1, 无需修改代码
 * ================================================================ */
typedef struct {
    const char *name;
    int         enabled;
    int         startup_delay_ms;
} child_def_t;

static const child_def_t g_child_defs[] = {
    { "gps_daemon",    1,    0 },
    { "sensor_daemon", 0,  500 },
    { "input_daemon",  1, 1000 },
    { "canbus_daemon", 1, 1500 },
    { "av_daemon",     1, 2500 },
    { "dvr_daemon",    1, 3500 },
    { "net_daemon",    0, 4500 },
};
#define CHILD_DEF_COUNT  ((int)(sizeof(g_child_defs) / sizeof(g_child_defs[0])))

/**
 * @brief 加载守护进程配置, 构建子进程列表
 *
 * 先用内置定义表填充, 再用配置文件 [processes] 节覆盖 enabled 标志。
 * conf_file 为 NULL 或文件不存在时, 全部使用内置默认值 (不视为错误)。
 */
int guard_config_load(guard_context_t *ctx, const char *conf_file)
{
    if (!ctx) return RET_ERR_PARAM;

    /* ---- 1. 用内置定义表填充 ---- */
    memset(ctx->children, 0, sizeof(ctx->children));

    /* 防御: 内置条目数超过数组容量时截断, 避免越界 */
    int n = CHILD_DEF_COUNT;
    if (n > GUARD_MAX_CHILDREN) n = GUARD_MAX_CHILDREN;

    for (int i = 0; i < n; i++) {
        child_process_t *c = &ctx->children[i];

        snprintf(c->name, sizeof(c->name), "%s", g_child_defs[i].name);
        snprintf(c->bin_path, sizeof(c->bin_path),
                 "%s%s", GUARD_BIN_PREFIX, g_child_defs[i].name);

        c->auto_restart     = 1;   /* 异常退出自动重启 */
        c->enabled          = g_child_defs[i].enabled;
        c->startup_delay_ms = g_child_defs[i].startup_delay_ms;
        c->pid              = 0;
        c->state            = PROC_STOPPED;
    }
    ctx->child_count = n;

    /* ---- 2. 配置文件覆盖 enabled ---- */
    if (!conf_file) return RET_OK;

    config_t cfg;
    if (config_load(&cfg, conf_file) != 0) {
        /* 配置文件缺失是允许的: 使用内置默认值继续运行。
         * 这里不返回错误, 否则在没有 guard.conf 的环境下会直接启动失败。 */
        return RET_OK;
    }

    int enabled_count = 0;
    for (int i = 0; i < ctx->child_count; i++) {
        ctx->children[i].enabled =
            config_get_bool(&cfg, "processes", ctx->children[i].name,
                            ctx->children[i].enabled);
        if (ctx->children[i].enabled) enabled_count++;
    }
    config_unload(&cfg);

    if (enabled_count == 0) {
        LOG_WARN("guard", "config %s: no process enabled, nothing to start",
                 conf_file);
    }
    return RET_OK;
}

/**
 * @brief guard_daemon 主入口
 *
 * 启动流程:
 *   1. 可选守护进程化 (daemonize) — -f 参数跳过
 *   2. 初始化日志系统 (文件日志, 滚动策略)
 *   3. 构建子进程表并依次启动 (fork+exec, 错开 500ms)
 *   4. 创建 IPC Socket 服务端
 *   5. 注册信号处理
 *   6. 进入主事件循环 (poll-based)
 *
 * 主循环:
 *   poll() 最多等待 GUARD_HEARTBEAT_SEC 秒,
 *   timeout 到期 → 执行心跳任务 (更新系统状态),
 *   有事件 → 处理 Socket 事件。
 *   每轮循环都通过 waitpid(WNOHANG) 回收已退出的子进程。
 *
 * 优雅退出:
 *   SIGTERM/SIGINT → g_running = 0 → 退出主循环
 *     → 反向顺序停止所有子进程 (后启动的先停)
 *     → 关闭客户端和服务端 socket
 *     → 关闭日志
 *
 * @param argc  参数个数
 * @param argv  参数数组, argv[1] 可选 "-f" 表示前台运行
 * @return      0=正常退出
 */
int main(int argc, char *argv[])
{
    /* ---- 解析命令行参数 ----
     *   -f            前台运行 (不 daemonize), 方便 gdb 调试和观察日志
     *   其他参数       视为配置文件路径, 并透传给所有子进程
     *
     * 用法示例:
     *   guard_daemon                                  # 后台运行, 使用默认配置
     *   guard_daemon /etc/car_terminal/config.ini     # 指定配置
     *   guard_daemon /etc/car_terminal/config.ini -f  # 指定配置 + 前台
     */
    int daemon_mode = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0) {
            daemon_mode = 0;  /* 前台模式: 方便 gdb 调试和日志观察 */
        } else {
            snprintf(g_conf_path, sizeof(g_conf_path), "%s", argv[i]);
        }
    }

    /* ---- 守护进程化 (daemonize) ---- */
    if (daemon_mode) guard_daemonize();

    /* 此后终端已脱离 (除非 -f 模式), printf 输出不会显示在终端上,
     * 所有信息应通过 LOG_xxx 宏写入日志文件 */
    printf("[guard] guard_daemon starting...\n");

    /* ---- 初始化上下文 ---- */
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.client_fd = -1;  /* -1 表示无客户端连接 */

    /* ---- 初始化日志 (文件滚动: 1MB, 保留3个备份) ---- */
    log_init(GUARD_LOG_PATH, LOG_INFO, 1024 * 1024, 3);
    LOG_INFO("guard","===== guard_daemon starting =====");

    /* ---- 构建子进程表并依次启动 ----
     * guard_config_load 会填入 7 个进程定义, 其中 enabled=0 的
     * 预留槽位 (sensor_daemon / net_daemon) 跳过, 不启动也不监控。 */
    if (guard_config_load(&g_ctx, g_conf_path) != RET_OK) {
        LOG_ERROR("guard","Failed to load child process table");
    }

    int started = 0;
    for (int i = 0; i < g_ctx.child_count; i++) {
        if (!g_ctx.children[i].enabled) {
            LOG_INFO("guard","Skip %s (disabled)", g_ctx.children[i].name);
            continue;
        }
        /* startup_delay_ms 错开: 避免所有进程同时启动造成 CPU 尖峰 */
        if (g_ctx.children[i].startup_delay_ms > 0) {
            usleep(g_ctx.children[i].startup_delay_ms * 1000);
        }
        guard_child_start(&g_ctx.children[i]);
        started++;
    }
    LOG_INFO("guard","Child table: %d defined, %d started",
             g_ctx.child_count, started);

    /* ---- 创建 IPC Socket 服务端 ---- */
    g_ctx.server_fd = sock_create_server(GUARD_SOCKET_PATH);
    if (g_ctx.server_fd < 0) {
        LOG_ERROR("guard","Failed to create server socket: %s", GUARD_SOCKET_PATH);
    }

    /* ---- 信号处理 ----
     * SIGCHLD → SIG_DFL (默认处理): 子进程退出时变为僵尸,
     *           由 waitpid() 回收。不用 SIG_IGN 因为那样
     *           waitpid 的行为在不同 UNIX 系统上不一致。
     * SIGTERM → sig_handler: 优雅退出触发器
     * SIGINT  → sig_handler: Ctrl-C 终止 (前台模式用) */
    sigset_t mask;
    sigemptyset(&mask);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);

    /* ---- 初始化系统状态采集 ---- */
    update_status(&g_ctx.sys_status);

    LOG_INFO("guard","Init complete. %d children, Loop starting...",
             g_ctx.child_count);

    /* ============================================================
     *  主事件循环
     *
     *  循环频率: 由 poll timeout (GUARD_HEARTBEAT_SEC × 1000ms)
     *  控制, 通常 5 秒一轮。事件驱动: 有 socket 事件时立即唤醒。
     *
     *  pfds 布局:
     *    pfds[0] = server_fd (监听 socket)
     *    pfds[1] = client_fd (客户端连接, 可选)
     * ============================================================ */
    while (g_running) {
        struct pollfd pfds[3];
        int nfds = 0;

        /* 构建 poll fd 数组 */
        if (g_ctx.server_fd >= 0) {
            pfds[nfds].fd     = g_ctx.server_fd;
            pfds[nfds].events = POLLIN;  /* 监听新连接 */
            nfds++;
        }
        if (g_ctx.client_fd >= 0) {
            pfds[nfds].fd     = g_ctx.client_fd;
            pfds[nfds].events = POLLIN;  /* 监听客户端数据/断开 */
            nfds++;
        }

        /* poll: 等待事件或超时 (心跳间隔) */
        poll(pfds, nfds, GUARD_HEARTBEAT_SEC * 1000);

        /* ---- 回收僵尸子进程 ----
         *
         * waitpid(-1, &status, WNOHANG) 回收任意已退出的子进程:
         *   返回 pid (>0): 成功回收一个子进程, status 包含退出信息
         *   返回 0:       没有已退出的子进程 (WNOHANG 模式正常)
         *   返回 -1:      错误 (ECHILD=没有子进程, 已被信号中断等)
         *
         * WIFEXITED/WIFSIGNALED 宏解析退出原因:
         *   WIFEXITED(status)    → 正常退出 (exit/_exit), WEXITSTATUS(status) 取退出码
         *   WIFSIGNALED(status)  → 被信号杀死 (SIGSEGV/SIGKILL等), WTERMSIG(status) 取信号编号
         *   WCOREDUMP(status)    → 是否生成 core dump */
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            /* 在子进程表中查找对应的 child_process_t */
            for (int i = 0; i < g_ctx.child_count; i++) {
                if (g_ctx.children[i].pid == pid) {
                    child_process_t *c = &g_ctx.children[i];

                    /* 记录退出原因 */
                    if (WIFEXITED(status)) {
                        LOG_WARN("guard","%s exited with code=%d",
                                 c->name, WEXITSTATUS(status));
                    }
                    if (WIFSIGNALED(status)) {
                        LOG_ERROR("guard","%s killed by signal=%d (%s)",
                                  c->name, WTERMSIG(status),
                                  WCOREDUMP(status) ? "core dumped" : "");
                    }

                    c->pid   = 0;
                    c->state = PROC_STOPPED;

                    /* 如果开启了自动重启 且 系统仍在运行,
                     * 则触发重启流程 (带速率限制) */
                    if (c->auto_restart && g_running) {
                        guard_child_restart(c);
                    }
                    break;
                }
            }
        }

        /* ---- 定期心跳任务: 更新系统健康状态 ----
         * 使用 static 变量记录上次更新时间,
         * poll timeout 也会每 5s 到期, 双重保障。 */
        static time_t last = 0;
        time_t now = time(NULL);
        if (now - last >= GUARD_HEARTBEAT_SEC) {
            last = now;
            update_status(&g_ctx.sys_status);
        }

        /* ---- 处理 Socket 事件 ----
         * pfds[0].revents: server_fd 事件 (POLLIN=有新连接)
         * pfds[1].revents: client_fd 事件 (POLLIN=有数据, POLLHUP=断开) */
        handle_sockets(&g_ctx,
                       nfds >= 1 ? pfds[0].revents : 0,
                       nfds >= 2 ? pfds[1].revents : 0);
    }

    /* ============================================================
     *  优雅退出
     *
     *  停止顺序: 后启动的先停 (LIFO), 因为后启动的进程
     *  可能依赖先启动进程的服务 (如 av 可能依赖 dvr)。
     * ============================================================ */
    LOG_INFO("guard","Shutting down...");

    /* 反向停止所有子进程 */
    for (int i = g_ctx.child_count - 1; i >= 0; i--) {
        if (g_ctx.children[i].pid > 0) {
            guard_child_stop(&g_ctx.children[i]);
        }
    }

    /* 关闭客户端连接 */
    if (g_ctx.client_fd >= 0) {
        sock_close(g_ctx.client_fd, NULL);
    }

    /* 关闭服务端 Socket, 删除 socket 文件 */
    if (g_ctx.server_fd >= 0) {
        sock_close(g_ctx.server_fd, GUARD_SOCKET_PATH);
    }

    /* 关闭日志 */
    log_close();

    return 0;
}
