/**
 * @file    guard_daemon.h
 * @brief   系统守护进程 — 配置常量 & 数据结构定义
 *
 * guard_daemon 是车载终端系统的"总管"进程，其核心职责:
 *
 *   1. 守护进程化 (daemonize)
 *      脱离终端 (fork + setsid + fork): 不受终端关闭影响
 *      工作目录切换到 /: 避免占用挂载点
 *      重定向标准 IO 到 /dev/null: 防止无终端写入失败
 *      umask(0): 确保创建的文件权限不受限制
 *
 *   2. 子进程生命周期管理
 *      fork() + execlp() 启动各后台进程 (gps/dvr/av/input/canbus)
 *      SIGCHLD → SIG_DFL → waitpid(WNOHANG) 回收僵尸子进程
 *      崩溃自动重启: 带滑动窗口速率限制, 防止重启风暴 (restart storm)
 *      优雅退出: SIGTERM → 反向顺序停止所有子进程 → waitpid 回收
 *
 *   3. 系统健康监控 (通过 /proc 文件系统)
 *      /proc/stat           — CPU 时间统计 (计算使用率)
 *      /proc/meminfo        — 内存总量/空闲
 *      /proc/loadavg        — 系统负载 (1分钟平均)
 *      /sys/class/thermal/thermal_zone0/temp — CPU 温度 (毫摄氏度)
 *      statfs("/")          — eMMC 剩余空间
 *      statfs("/mnt/sdcard")— SD 卡剩余空间
 *
 *   4. IPC 通信 (通过 UNIX Domain Socket)
 *      监听 /tmp/car_guard.sock, 接受 Qt UI 客户端连接
 *      响应 MSG_SERVICE_STATUS 查询 (返回系统状态)
 *      使用 poll 事件驱动模型 (非 epoll — guard 只有少量 fd)
 *
 * 学习要点:
 *   1. daemonize: fork→setsid→fork (双fork经典模式, 防止获取终端)
 *   2. fork+exec: 进程创建与程序替换 (Copy-On-Write 优化)
 *   3. SIGCHLD+waitpid(WNOHANG): 异步子进程回收 (避免僵尸)
 *   4. 重启速率限制: 滑动窗口计数器, 防止无限重启循环
 *   5. /proc 解析: 用户空间系统监控的标准方法
 *   6. poll 事件循环: 单线程处理定时任务+Socket IO
 */

#ifndef _GUARD_DAEMON_H_
#define _GUARD_DAEMON_H_

#include "common.h"
#include "protocol/protocol.h"   /* ringbuffer_t, protocol_parser_t */
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>   /* struct rusage */

/* ================================================================
 *  默认配置
 * ================================================================ */

/* 最大可管理的子进程数量 (当前5个, 预留3个槽位给 net/sensor/spare) */
#define GUARD_MAX_CHILDREN      8

/* 心跳间隔 (秒): poll timeout 时长, 也是系统状态采集周期 */
#define GUARD_HEARTBEAT_SEC     5

/* 重启速率限制:
 *   在 GUARD_RESTART_WINDOW_SEC (60秒) 的滑动窗口内,
 *   如果某个子进程重启超过 GUARD_MAX_RESTART (5次),
 *   判定为不可恢复的故障, 停止重启并标记 PROC_ERROR */
#define GUARD_MAX_RESTART        5
#define GUARD_RESTART_WINDOW_SEC 60

/* 存储空间告警阈值: eMMC/SD 剩余低于此值触发告警 */
#define GUARD_STORAGE_ALERT_MB   100

/* CPU 温度告警阈值 (摄氏度): 超过此值触发过热告警 */
#define GUARD_TEMP_ALERT_C       80.0f

/* ---- Socket ---- */
#define GUARD_SOCKET_PATH       "/tmp/car_guard.sock"   /* IPC Socket 文件路径 */
#define GUARD_LOG_PATH          "/var/log/car_terminal/guard.log"  /* 日志文件路径 */

/* ---- 子进程二进制路径前缀 ----
 * 在嵌入式开发板上所有 daemon 统一放在 /usr/bin/ 目录下,
 * 与 scripts/build_and_deploy.sh 的 adb push 目标、start_all.sh 的
 * BIN 变量保持一致 (早期版本为 "/bin/", 与部署路径不一致,
 * 会导致 execlp 找不到可执行文件)。
 * 完整路径 = GUARD_BIN_PREFIX + 进程名, 例如 /usr/bin/gps_daemon */
#define GUARD_BIN_PREFIX        "/usr/bin/"

/* ---- 透传给子进程的默认配置文件路径 ----
 * 子进程 (gps_daemon 等) 与 guard_daemon 使用同一份 config.ini。
 * 若希望子进程改用默认配置启动, 可将此宏置空或传入不存在的路径。 */
#define GUARD_DEFAULT_CONF_PATH "/etc/car_terminal/config.ini"

/* ================================================================
 *  子进程定义
 *
 *  每个被 guard_daemon 管理的子进程都由一个 child_process_t
 *  实例描述。该结构体同时承载:
 *    - 静态配置 (name/bin_path/auto_restart/startup_delay_ms)
 *    - 运行时状态 (pid/state/start_time)
 *    - 重启限速状态 (restart_count/first_restart)
 *    - 退出诊断信息 (exit_code/exit_signal)
 * ================================================================ */
typedef struct {
    /* ---- 静态配置 ---- */
    char        name[32];           /* 进程名, 用于日志 (如 "gps_daemon") */
    char        bin_path[256];      /* 可执行文件完整路径 (如 "/usr/bin/gps_daemon") */
    int         auto_restart;       /* 异常退出后是否自动重启 (1=是, 0=否) */
    int         startup_delay_ms;   /* 启动延迟 (ms): 错开启动避免资源争抢 */
    int         enabled;            /* 是否实际启动 (1=启动, 0=仅占位不启动)
                                     * 预留槽位 (如 net_daemon) 设为 0:
                                     * 已定义但未实现/未部署, 不参与启动与监控 */

    /* ---- 运行时状态 ---- */
    pid_t       pid;                /* 子进程 PID, 0 表示未启动 */
    int         state;              /* 进程状态: PROC_STOPPED/STARTING/RUNNING/ERROR */
    time_t      start_time;         /* 最近一次启动的时间戳 (time(NULL)) */

    /* ---- 重启速率限制 (滑动窗口计数器) ---- */
    int         restart_count;      /* 当前窗口内已重启次数 */
    time_t      first_restart;      /* 当前窗口的起始时间 (首次重启的时刻) */

    /* ---- 退出诊断 (用于日志记录和问题排查) ---- */
    int         exit_code;          /* 最后的退出码 (WEXITSTATUS), 仅 WIFEXITED 时有效 */
    int         exit_signal;        /* 最后的终止信号 (WTERMSIG), 仅 WIFSIGNALED 时有效 */
} child_process_t;

/* ================================================================
 *  系统状态 — 周期性采集的系统健康指标
 *
 *  所有指标在 update_status() 中通过解析 /proc 和 sysfs 获取。
 *  通过 IPC Socket 对外提供查询 (MSG_SERVICE_STATUS 消息)。
 * ================================================================ */
typedef struct {
    float       cpu_percent;        /* CPU 使用率 (%), 由 /proc/stat 差值计算 */
    int         mem_total_kb;       /* 总内存 (KB), 来自 /proc/meminfo MemTotal */
    int         mem_free_kb;        /* 空闲内存 (KB), 来自 /proc/meminfo MemFree */
    float       mem_percent;        /* 内存使用率 (%), (Total-Free)/Total*100 */
    float       load_1min;          /* 1分钟平均负载, 来自 /proc/loadavg */
    float       cpu_temp;           /* CPU 温度 (℃), 来自 /sys/class/thermal */
    long        emmc_free_mb;       /* eMMC 剩余空间 (MB), statfs("/") */
    long        sd_free_mb;         /* SD卡剩余空间 (MB), statfs("/mnt/sdcard") */
    time_t      update_time;        /* 最近一次采集的时间戳 */
} system_status_t;

/* ================================================================
 *  守护进程上下文 (guard_context_t) — 全局运行时状态
 *
 *  集中管理 guard_daemon 的全部运行时数据:
 *    - 子进程表 (children[])
 *    - 系统健康快照 (sys_status)
 *    - 网络 IO (server_fd/client_fd/ringbuffer/protocol_parser)
 *    - 控制标志 (running)
 *
 *  所有字段在 main() 启动时初始化, 在主循环中持续更新。
 *  使用全局变量 g_ctx 持有唯一实例 (单实例守护进程)。
 * ================================================================ */
typedef struct {
    /* ---- 子进程管理 ---- */
    child_process_t children[GUARD_MAX_CHILDREN];  /* 子进程描述符数组 */
    int             child_count;                    /* 实际注册的子进程数 */

    /* ---- 系统健康监控 ---- */
    system_status_t sys_status;    /* 最近一次采集的系统状态快照 */

    /* ---- 控制标志 ---- */
    int             running;       /* 1=主循环运行中, 0=收到退出信号 */

    /* ---- Socket IO (poll 模式) ---- */
    int             server_fd;     /* 服务端监听 socket fd (-1=未创建) */
    int             client_fd;     /* 客户端连接 socket fd (-1=无连接) */

    /* ---- 协议解析 ---- */
    protocol_parser_t proto_parser;  /* 帧解析器状态机 */
    ringbuffer_t    recv_rb;         /* 接收数据环形缓冲区 */

    /* ---- 进程自身配置 ---- */
    proc_config_t   proc_cfg;      /* socket路径/日志路径/日志级别/心跳间隔 */
} guard_context_t;

/* ================================================================
 *  函数声明
 * ================================================================ */

/**
 * @brief 标准守护进程化 (daemonize) — 双 fork 经典模式
 *
 * 将进程转变为脱离终端的系统守护进程:
 *   1. fork → 父进程退出 (子进程不再是进程组长, 为 setsid 做准备)
 *   2. setsid (创建新会话, 脱离控制终端)
 *   3. fork → 父进程退出 (孙进程不是会话组长, 永远无法获取终端)
 *   4. chdir("/") (切换到根目录, 避免占用挂载点)
 *   5. umask(0) (重置文件创建掩码)
 *   6. 重定向 stdin/stdout/stderr 到 /dev/null
 *
 * 注意: 此函数不返回 (如果 daemonize 成功, 原始进程已退出)。
 * 通过 -f 参数可跳过 daemonize (前台调试模式)。
 */
void guard_daemonize(void);

/**
 * @brief 加载守护进程配置, 构建子进程列表
 * @param ctx       守护进程上下文
 * @param conf_file 配置文件路径 (如 /etc/car_terminal/config.ini);
 *                  为 NULL 或文件不存在时使用内置的默认子进程表
 * @return          成功: RET_OK, 失败: 对应错误码
 *
 * 子进程表行为:
 *   1. 先填入内置的 7 个进程定义 (gps/sensor/input/canbus/av/dvr/net)。
 *      其中 sensor_daemon 与 net_daemon 为预留槽位, enabled=0,
 *      只占位不启动 (功能分别由 Qt SensorThread 与未部署的服务承担)。
 *   2. 若 conf_file 存在, 读取其 [processes] 节覆盖各进程的 enabled:
 *
 *        [processes]
 *        gps_daemon    = 1
 *        sensor_daemon = 0
 *        ...
 *
 *      键为进程名, 值为 0/1 (或 true/false/yes/no/on/off)。
 *      未在文件中出现的进程保持内置默认值。
 *
 * 为什么要区分 "已定义" 和 "已启用":
 *   让进程表同时表达 "系统有哪些服务" 与 "当前部署了哪些服务",
 *   避免为了临时下线某个服务而删掉它的定义和监控逻辑。
 */
int guard_config_load(guard_context_t *ctx, const char *conf_file);

/**
 * @brief 启动单个子进程 (fork + execlp)
 * @param child  子进程定义 (需填充 bin_path, name 等字段)
 * @return       父进程: 0=成功, -1=失败
 *
 * 流程: fork() → 子进程 execlp() 替换自身为子进程程序
 * 子进程通过 Copy-On-Write 共享父进程地址空间 (低开销 fork)。
 * execlp 失败时子进程调用 _exit(1) (非 exit, 避免 stdio 副作用)。
 */
int guard_child_start(child_process_t *child);

/**
 * @brief 优雅终止子进程: SIGTERM → 等待3秒 → SIGKILL
 * @param child  子进程定义
 *
 * 两阶段终止:
 *   阶段1: kill(SIGTERM) — 给子进程执行清理工作的机会
 *   阶段2: 3秒后未退出 → kill(SIGKILL) — 内核强制终止
 * 确保 waitpid 回收, 避免僵尸进程。
 */
void guard_child_stop(child_process_t *child);

/**
 * @brief 重启子进程 (stop + 500ms等待 + start) — 带速率限制
 * @param child  子进程定义
 *
 * 先停止旧进程并等待资源释放 (500ms), 然后启动新进程。
 * 内置滑动窗口速率限制: 60秒内最多重启5次,
 * 超限则放弃并标记 PROC_ERROR, 防止重启风暴。
 */
void guard_child_restart(child_process_t *child);

#endif /* _GUARD_DAEMON_H_ */
