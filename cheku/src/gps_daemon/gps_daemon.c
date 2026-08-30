/**
 * @file    gps_daemon.c
 * @brief   GPS 定位守护进程 - 主程序
 *
 * ================================================================
 *  进程架构 (单线程事件驱动)
 * ================================================================
 *
 *   ┌──────────────────────────────────────────────────────┐
 *   │                 epoll_wait() 事件循环                 │
 *   │                                                      │
 *   │  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌────────┐  │
 *   │  │ uart fd  │ │ timer fd │ │ server   │ │ client │  │
 *   │  │/dev/tty  │ │ 1s 上报  │ │ socket   │ │ fd     │  │
 *   │  └────┬─────┘ └────┬─────┘ └────┬─────┘ └───┬────┘  │
 *   │       │            │           │            │        │
 *   │       ▼            ▼           ▼            ▼        │
 *   │  ringbuffer   push gps_data  accept     recv/响应   │
 *   │  → NMEA解析   → UI          → 客户端   → UI请求    │
 *   │  → 里程累计                                            │
 *   │  → 轨迹记录                                            │
 *   │  → 日志输出                                            │
 *   └──────────────────────────────────────────────────────┘
 *
 *  数据流向:
 *   GPS模块(UART) → ringbuffer → NMEA状态机 → gps_data_t
 *                                                 ├→ 里程累计 (Haversine)
 *                                                 ├→ 轨迹记录 (环形缓冲)
 *                                                 ├→ 定时推送给 UI
 *                                                 └→ 日志文件
 */

#include "gps_daemon.h"
#include "nmea_parser.h"
#include "uart_util.h"
#include "log/log.h"
#include "config/config.h"
#include "socket/unix_socket.h"
#include "protocol/protocol.h"
#include "timer/timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>

#define MAX_EPOLL_EVENTS    8

/* ================================================================
 *  全局上下文
 * ================================================================ */
static gps_context_t g_ctx;

/* ================================================================
 *  地理计算
 * ================================================================ */

/**
 * @brief NMEA 度分 → 十进制度 (公共API, 声明在 gps_daemon.h)
 *
 * 示例: 3912.3456 → 39 + 12.3456/60 = 39.205760
 */
double gps_nmea_to_degrees(double nmea_deg)
{
    int    deg = (int)(nmea_deg / 100.0);
    double min = nmea_deg - (double)(deg * 100);
    return (double)deg + min / 60.0;
}

/**
 * @brief Haversine 公式计算两点间球面距离 (米)
 *
 * Haversine 公式是大圆距离 (Great Circle Distance) 的一种计算方式,
 * 相比直接用余弦定理, 在短距离时有更高的浮点精度。
 *
 * 公式推导:
 *   haversin(θ) = sin²(θ/2) = (1-cos(θ))/2
 *
 *   d = 2·R·atan2(√a, √(1-a))
 *   其中:
 *     a = sin²(Δlat/2) + cos(lat1)·cos(lat2)·sin²(Δlon/2)
 *     R = 地球半径 (6371000 m, WGS-84 平均半径)
 *
 * @param lat1, lon1  起点 (十进制度)
 * @param lat2, lon2  终点 (十进制度)
 * @return            距离 (米)
 */
double gps_haversine_distance(double lat1, double lon1,
                              double lat2, double lon2)
{
    double dlat, dlon;
    double a, c;

    /* 转为弧度 */
    double rlat1 = lat1 * M_PI / 180.0;
    double rlat2 = lat2 * M_PI / 180.0;
    double rlon1 = lon1 * M_PI / 180.0;
    double rlon2 = lon2 * M_PI / 180.0;

    dlat = rlat2 - rlat1;
    dlon = rlon2 - rlon1;

    /* sin²(Δlat/2) = pow(sin(dlat/2), 2) */
    a = sin(dlat / 2.0) * sin(dlat / 2.0)
      + cos(rlat1) * cos(rlat2)
      * sin(dlon / 2.0) * sin(dlon / 2.0);

    /* atan2(√a, √(1-a)): 保持数值稳定性 */
    c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));

    return GPS_EARTH_RADIUS_M * c;
}

/* ================================================================
 *  里程累计
 * ================================================================ */

/**
 * @brief 当有新定位数据时更新里程
 *
 * 如果 fix_quality > 0 (有定位), 计算与上一个有效点的距离并累加。
 * 过滤条件: 两点距离 > 1 米 (避免静止时因 GPS 飘移产生微小里程)
 *
 * @param ctx  进程上下文
 */
static void update_odometer(gps_context_t *ctx)
{
    double dist_m;

    /* 需要有效定位 */
    if (ctx->latest.fix_quality == 0) return;

    if (ctx->odo.has_last) {
        dist_m = gps_haversine_distance(
            ctx->odo.last_lat, ctx->odo.last_lon,
            ctx->latest.latitude, ctx->latest.longitude);

        /* 过滤微小位移 (GPS 静止飘移通常 < 5m)
         * 阈值 1m: 比实际飘移小, 确保不遗漏真实低速移动 */
        if (dist_m > 1.0) {
            ctx->odo.total_km += dist_m / 1000.0;
            ctx->odo.point_count++;
            LOG_DEBUG("gps", "Odometer: +%.1fm, total=%.3fkm (pts=%u)",
                      dist_m, ctx->odo.total_km, ctx->odo.point_count);
        }
    }

    /* 更新上一个有效点 */
    ctx->odo.last_lat = ctx->latest.latitude;
    ctx->odo.last_lon = ctx->latest.longitude;
    ctx->odo.has_last = 1;
}

/**
 * @brief 将当前定位点记录到轨迹缓冲区
 *
 * 环形缓冲: 当缓冲区满时覆盖最旧的记录 (FIFO)。
 *
 * @param ctx  进程上下文
 */
static void record_track_point(gps_context_t *ctx)
{
    if (ctx->latest.fix_quality == 0) return;  /* 仅记录有效点 */
    if (!ctx->track_buf) return;

    int idx = ctx->track_count % ctx->track_max;

    ctx->track_buf[idx].lat       = ctx->latest.latitude;
    ctx->track_buf[idx].lon       = ctx->latest.longitude;
    ctx->track_buf[idx].speed     = ctx->latest.speed;
    ctx->track_buf[idx].timestamp = time(NULL);

    ctx->track_count++;
}

/* ================================================================
 *  配置加载
 * ================================================================ */

int gps_config_load(gps_context_t *ctx, const char *conf_file)
{
    config_t conf;
    int conf_loaded = 0;

    if (!ctx) return RET_ERR_PARAM;

    if (conf_file && config_load(&conf, conf_file) == 0) {
        conf_loaded = 1;
    }

    /* 串口设备 */
    if (conf_loaded) {
        config_get_str(&conf, "gps", "uart_device",
                       GPS_DEFAULT_UART_DEVICE,
                       ctx->uart_device, sizeof(ctx->uart_device));
    } else {
        strncpy(ctx->uart_device, GPS_DEFAULT_UART_DEVICE,
                sizeof(ctx->uart_device) - 1);
    }

    /* 上报间隔 */
    ctx->report_interval_ms = conf_loaded ?
        config_get_int(&conf, "gps", "report_interval", 1) * 1000
        : GPS_DEFAULT_REPORT_INTERVAL_MS;

    /* 进程通用配置 */
    snprintf(ctx->proc_cfg.socket_path, sizeof(ctx->proc_cfg.socket_path),
             "%s", GPS_SOCKET_PATH);
    snprintf(ctx->proc_cfg.log_path, sizeof(ctx->proc_cfg.log_path),
             "%s", GPS_LOG_PATH);

    if (conf_loaded) config_unload(&conf);

    LOG_INFO("gps", "Config: device=%s report=%dms",
             ctx->uart_device, ctx->report_interval_ms);
    return RET_OK;
}

/* ================================================================
 *  初始化 / 清理
 * ================================================================ */

/**
 * @brief GPS 守护进程上下文的完整一次性初始化。
 *
 * 按顺序执行以下步骤:
 *   1. 将上下文结构体填零并将初始 fd 值设为 -1。
 *   2. 打开滚动日志文件。
 *   3. 加载配置 (串口设备、上报间隔、socket 路径)。
 *   4. 分配轨迹点环形缓冲区 (GPS_MAX_TRACK_POINTS 个条目)。
 *   5. 以配置的波特率打开串口设备。
 *   6. 初始化串口环形缓冲区和 socket 协议解析器。
 *   7. 创建 UNIX Domain 监听 socket。
 *   8. 创建 timerfd 用于向 UI 定时推送数据。
 *   9. 创建 epoll 实例，注册串口 fd、服务端 fd 和定时器 fd 的 EPOLLIN 事件。
 *  10. 阻塞 SIGTERM 和 SIGINT；创建 signalfd 用于干净关闭。
 *  11. 启动定时器。
 *
 * @param ctx        指向未初始化的 gps_context_t (将被填零)。
 * @param conf_file  config.ini 路径 (可为 NULL 以使用默认值)。
 * @return           成功返回 0，任何致命初始化错误返回 -1。
 */
static int init_all(gps_context_t *ctx, const char *conf_file)
{
    struct epoll_event ev;
    sigset_t sigmask;
    (void)0;

    memset(ctx, 0, sizeof(*ctx));
    ctx->uart_fd   = -1;
    ctx->client_fd = -1;
    ctx->running   = 1;

    /* 1. 日志 */
    log_init(GPS_LOG_PATH, LOG_INFO, 1024 * 1024, 3);
    LOG_INFO("gps", "===== gps_daemon starting =====");

    /* 2. 配置 */
    gps_config_load(ctx, conf_file);

    /* 3. 轨迹缓冲区 */
    ctx->track_max = GPS_MAX_TRACK_POINTS;
    ctx->track_buf = (gps_track_point_t*)calloc(
        (size_t)ctx->track_max, sizeof(gps_track_point_t));
    if (!ctx->track_buf) {
        LOG_ERROR("gps", "Cannot allocate track buffer");
        return -1;
    }

    /* 4. 初始化里程 */
    ctx->odo.has_last = 0;
    ctx->odo.total_km = 0.0;

    /* 5. 打开串口 */
    ctx->uart_fd = uart_open(ctx->uart_device, GPS_DEFAULT_BAUD_RATE);
    if (ctx->uart_fd < 0) {
        LOG_ERROR("gps", "UART open failed");
        return -1;
    }

    /* 6. 环形缓冲区 (串口接收 + Socket接收) */
    ringbuffer_init(&ctx->uart_rb, GPS_UART_BUF_SIZE);
    ringbuffer_init(&ctx->recv_rb, 4096);
    protocol_parser_init(&ctx->proto_parser);

    /* 7. Socket 服务端 */
    ctx->server_fd = sock_create_server(ctx->proc_cfg.socket_path);
    if (ctx->server_fd < 0) {
        LOG_ERROR("gps", "Socket server failed");
        return -1;
    }

    /* 8. 定时器 (数据上报) */
    ctx->timer_fd = timerfd_create_ms((uint32_t)ctx->report_interval_ms);
    if (ctx->timer_fd < 0) {
        LOG_ERROR("gps", "Timer create failed");
        return -1;
    }

    /* 9. epoll */
    ctx->epoll_fd = epoll_create1(0);
    if (ctx->epoll_fd < 0) { perror("epoll_create1"); return -1; }

    ev.events  = EPOLLIN;
    ev.data.fd = ctx->uart_fd;
    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->uart_fd, &ev);

    ev.data.fd = ctx->server_fd;
    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->server_fd, &ev);

    ev.data.fd = ctx->timer_fd;
    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->timer_fd, &ev);

    /* 10. 信号 */
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGTERM);
    sigaddset(&sigmask, SIGINT);
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
    ctx->signal_fd = signalfd(-1, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
    ev.data.fd = ctx->signal_fd;
    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->signal_fd, &ev);

    /* 11. 启动定时器 */
    timer_start(ctx->timer_fd, (uint32_t)ctx->report_interval_ms);

    LOG_INFO("gps", "Init complete. uart_fd=%d", ctx->uart_fd);
    return 0;
}

/**
 * @brief 释放 init_all() 获取的所有资源。
 *
 * 关闭所有文件描述符 (客户端 socket、定时器、服务端 socket、signal fd、
 * epoll fd、串口)，销毁环形缓冲区，释放轨迹点缓冲区，
 * 删除 UNIX socket 路径，并关闭日志子系统。
 *
 * 即使某些资源从未分配，此函数也可以安全调用
 * (每个指针和 fd 在释放前都会检查 NULL / -1)。
 *
 * @param ctx  指向完全初始化的守护进程上下文。
 */
static void cleanup_all(gps_context_t *ctx)
{
    LOG_INFO("gps", "Cleaning up...");

    if (ctx->client_fd >= 0) sock_close(ctx->client_fd, NULL);
    ringbuffer_destroy(&ctx->uart_rb);
    ringbuffer_destroy(&ctx->recv_rb);
    if (ctx->timer_fd >= 0)  timer_close(ctx->timer_fd);
    if (ctx->server_fd >= 0) sock_close(ctx->server_fd, ctx->proc_cfg.socket_path);
    if (ctx->signal_fd >= 0) close(ctx->signal_fd);
    if (ctx->epoll_fd >= 0)  close(ctx->epoll_fd);
    if (ctx->uart_fd >= 0)   uart_close(ctx->uart_fd);
    free(ctx->track_buf);
    ctx->track_buf = NULL;

    log_close();
    printf("[gps] gps_daemon stopped.\n");
}

/* ================================================================
 *  事件处理
 * ================================================================ */

/**
 * @brief 从串口读取原始字节，喂入 NMEA 解析器，
 *        并在解码出完整语句时更新里程和轨迹日志。
 *
 * 每当串口 fd 变为可读时，此函数由 epoll 事件循环调用。
 * 它执行非阻塞读取循环 (在 EAGAIN 时退出)，将每个字节传递给
 * nmea_parser_feed()。当解析器发出完整语句信号时 (返回值 1)，
 * nmea_extract_data() 用解析出的字段更新 ctx->latest，
 * 如果该语句包含位置数据 (GGA、RMC 或 GLL)，
 * 则更新里程和轨迹点环形缓冲区。
 *
 * NMEA 解析器实例声明为 static，因此其内部状态在调用之间保持不变 —
 * 串口数据以任意大小的数据块到达，解析器必须在调用之间保持连续性。
 *
 * @param ctx  守护进程上下文 (原地更新: latest, fix_type, odo, track)。
 */
static void handle_uart_event(gps_context_t *ctx)
{
    uint8_t buf[256];
    static nmea_parser_t nmea;
    static int nmea_inited = 0;
    int n, i, ret;

    if (!nmea_inited) {
        nmea_parser_init(&nmea);
        nmea_inited = 1;
    }

    /* 循环读取串口 (非阻塞, 读到 EAGAIN 为止) */
    while (1) {
        n = uart_read(ctx->uart_fd, buf, sizeof(buf));
        if (n <= 0) break;  /* 无数据或错误 */

        /* 逐字符喂入 NMEA 解析器 */
        for (i = 0; i < n; i++) {
            ret = nmea_parser_feed(&nmea, (char)buf[i]);

            if (ret == 1) {
                /* 成功解析一条语句: 提取数据 */
                nmea_extract_data(&nmea.result, &ctx->latest);

                /* 更新定位类型 */
                if (ctx->latest.fix_quality >= 2) {
                    ctx->fix_type = GPS_FIX_3D;
                } else if (ctx->latest.fix_quality == 1) {
                    ctx->fix_type = GPS_FIX_2D;
                } else {
                    ctx->fix_type = GPS_FIX_NONE;
                }

                /* 里程累计 + 轨迹记录
                 * GGA: 经纬度/时间/质量/HDOP/海拔
                 * RMC: 经纬度/时间/速度/日期
                 * GLL: 经纬度/时间/状态
                 * VTG: 速度(km/h)/航向
                 * GSA: 定位类型(2D/3D)/HDOP
                 * GSV: 卫星数量 */
                if (nmea.result.type == NMEA_GPRMC
                    || nmea.result.type == NMEA_GPGGA
                    || nmea.result.type == NMEA_GPGLL) {
                    update_odometer(ctx);
                    record_track_point(ctx);
                }
            }
            else if (ret == -1) {
                /* 校验和错误: 已记录到 nmea.checksum_errors */
                LOG_DEBUG("gps", "NMEA checksum error (total=%u)",
                          nmea.checksum_errors);
            }
        }
    }
}

/**
 * @brief 定时器回调 — 向 UI 客户端推送最新 GPS 数据并写入日志摘要。
 *
 * 每个 report_interval_ms (默认 1000 ms) 触发一次。读取 timerfd
 * 以消费过期计数，然后:
 *   - 如果 UI 客户端已连接，将完整的 gps_data_t 结构体序列化
 *     并通过 UNIX socket 作为 MSG_GPS_DATA 帧发送。
 *   - 输出一行人类可读的 INFO 日志，包含当前定位质量、
 *     卫星数、位置、速度、高度和里程读数。
 *
 * 这是 UI 接收定时位置更新的唯一路径；按需查询 (MSG_GPS_QUERY)
 * 在 handle_client_event() 中处理。
 *
 * @param ctx  守护进程上下文 (读取 latest 和 odo; 不写入)。
 */
static void handle_timer_event(gps_context_t *ctx)
{
    uint64_t exp;

    timer_get_expirations(ctx->timer_fd, &exp);

    if (ctx->client_fd >= 0) {
        /* 向 UI 推送最新 GPS 数据快照 */
        gps_data_t send_data;
        memcpy(&send_data, &ctx->latest, sizeof(gps_data_t));

        int ret = sock_send_frame(ctx->client_fd, MSG_GPS_DATA,
                                  (uint8_t*)&send_data, sizeof(send_data));
        if (ret != 0) {
            LOG_WARN("gps", "Failed to push GPS data to UI");
        }
    }

    /* 用于诊断和回放的定时日志行 */
    LOG_INFO("gps", "Fix=%d Sats=%d Lat=%.6f Lon=%.6f Spd=%.1fkm/h "
             "Alt=%.1fm Odo=%.3fkm",
             ctx->latest.fix_quality,
             ctx->latest.satellites,
             ctx->latest.latitude,
             ctx->latest.longitude,
             ctx->latest.speed,
             ctx->latest.altitude,
             ctx->odo.total_km);
}

/**
 * @brief 处理通过 signalfd 送达的信号。
 *
 * 从 signalfd 读取 siginfo 结构体。如果信号是 SIGTERM 或 SIGINT，
 * 设置 ctx->running = 0，使 run_loop() 干净退出并触发 cleanup_all()。
 * 其他信号被消费但忽略。
 *
 * 使用 signalfd (而非传统信号处理函数) 可以在 epoll 事件循环中
 * 保持信号处理同步，避免重入问题。
 *
 * @param ctx  守护进程上下文 (写入 ctx->running)。
 */
static void handle_signal_event(gps_context_t *ctx)
{
    struct signalfd_siginfo si;
    if (read(ctx->signal_fd, &si, sizeof(si)) == sizeof(si)) {
        if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) {
            LOG_INFO("gps", "Signal %d, shutting down.", si.ssi_signo);
            ctx->running = 0;
        }
    }
}

/**
 * @brief 在监听 socket 上接受新的 UI 客户端连接。
 *
 * 强制执行单客户端策略: 如果已有客户端连接 (ctx->client_fd >= 0)，
 * 新 fd 被接受后立即关闭。否则将 fd 注册到 epoll 的 EPOLLIN 事件，
 * 并存入 ctx->client_fd。协议解析器被重置，使新连接从干净的解码状态开始。
 *
 * @param ctx  守护进程上下文 (写入 client_fd 和 proto_parser)。
 */
static void handle_server_event(gps_context_t *ctx)
{
    struct epoll_event ev;
    int fd;

    if (ctx->client_fd >= 0) {
        fd = sock_accept(ctx->server_fd);
        if (fd >= 0) sock_close(fd, NULL);
        return;
    }

    fd = sock_accept(ctx->server_fd);
    if (fd < 0) return;

    ev.events  = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, fd, &ev);

    ctx->client_fd = fd;
    protocol_reset(&ctx->proto_parser, &ctx->recv_rb);
    LOG_INFO("gps", "UI client connected: fd=%d", fd);
}

/**
 * @brief 处理来自已连接 UI 客户端的传入协议消息。
 *
 * 使用 sock_recv() 读取原始字节，追加到 recv 环形缓冲区，
 * 并运行 protocol_unpack() 解码完整帧。处理两种消息类型:
 *
 *   - MSG_GPS_QUERY:  UI 请求即时数据快照。守护进程以包含 ctx->latest
 *     的 MSG_GPS_DATA 帧响应。
 *
 *   - MSG_GPS_STATUS: UI 请求定位类型和卫星数。守护进程以 4 字节
 *     状态帧响应: [fix_type, satellites, fix_quality, 0]。
 *
 * 如果 sock_recv() 返回 <= 0 (客户端断开或错误)，fd 从 epoll 集合中移除，
 * 关闭，且 ctx->client_fd 重置为 -1。
 *
 * @param ctx  守护进程上下文 (读取 client_fd; 写入 recv_rb, proto_parser)。
 */
static void handle_client_event(gps_context_t *ctx)
{
    uint8_t buf[512];
    int n = sock_recv(ctx->client_fd, buf, sizeof(buf));

    if (n <= 0) {
        LOG_INFO("gps", "UI disconnected");
        epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, ctx->client_fd, NULL);
        sock_close(ctx->client_fd, NULL);
        ctx->client_fd = -1;
        return;
    }

    ringbuffer_write(&ctx->recv_rb, buf, (uint32_t)n);

    uint8_t  type;
    uint8_t  data[64];
    uint32_t len;

    int ret;
    while ((ret = protocol_unpack(&ctx->proto_parser, &ctx->recv_rb,
                                   &type, data, sizeof(data), &len)) == 0) {
        switch (type) {
        case MSG_GPS_QUERY:
            /* UI 请求最新数据: 立即推送 */
            if (ctx->client_fd >= 0) {
                sock_send_frame(ctx->client_fd, MSG_GPS_DATA,
                               (uint8_t*)&ctx->latest,
                               sizeof(ctx->latest));
            }
            break;
        case MSG_GPS_STATUS:
            /* UI 请求 GPS 状态 */
            if (ctx->client_fd >= 0) {
                uint8_t status[4];
                status[0] = (uint8_t)ctx->fix_type;
                status[1] = ctx->latest.satellites;
                status[2] = ctx->latest.fix_quality;
                status[3] = 0;
                sock_send_frame(ctx->client_fd, MSG_GPS_STATUS,
                               status, sizeof(status));
            }
            break;
        default:
            break;
        }
    }
}

/* ================================================================
 *  主事件循环
 * ================================================================ */

/**
 * @brief 阻塞式 epoll 事件循环 — GPS 守护进程的核心。
 *
 * 在 init_all() 创建的 epoll fd 上无限期调用 epoll_wait() (timeout = -1)。
 * 每次唤醒时，根据触发的 fd 分发到相应的处理函数:
 *
 *   - uart_fd    → handle_uart_event()   — 读取 NMEA 字节, 更新位置。
 *   - timer_fd   → handle_timer_event()  — 向 UI 推送数据, 写日志行。
 *   - signal_fd  → handle_signal_event() — SIGTERM/INT 时优雅关闭。
 *   - server_fd  → handle_server_event() — 接受新 UI 客户端。
 *   - client_fd  → handle_client_event() — 处理 UI 协议消息。
 *
 * 当 handle_signal_event() 将 ctx->running 设为 0 或
 * epoll_wait() 返回致命错误 (EINTR 除外) 时，循环退出。
 *
 * @param ctx  完全初始化的守护进程上下文。
 */
static void run_loop(gps_context_t *ctx)
{
    struct epoll_event events[MAX_EPOLL_EVENTS];
    int nfds, i;

    LOG_INFO("gps", "Event loop started.");

    while (ctx->running) {
        nfds = epoll_wait(ctx->epoll_fd, events, MAX_EPOLL_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == ctx->uart_fd)
                handle_uart_event(ctx);
            else if (fd == ctx->timer_fd)
                handle_timer_event(ctx);
            else if (fd == ctx->signal_fd)
                handle_signal_event(ctx);
            else if (fd == ctx->server_fd)
                handle_server_event(ctx);
            else if (fd == ctx->client_fd)
                handle_client_event(ctx);
        }
    }
}

/* ================================================================
 *  main
 * ================================================================ */

/**
 * @brief GPS 守护进程入口点。
 *
 * 从命令行确定配置文件路径 (默认为 "../config/config.ini")，
 * 调用 init_all() 设置串口、socket、定时器和 epoll 循环，
 * 然后进入 run_loop()。循环退出时 (信号或致命错误)，
 * cleanup_all() 释放所有资源。
 *
 * @param argc  参数个数。
 * @param argv  argv[1] 可选指定配置文件路径。
 * @return      干净关闭时返回 0，init_all() 失败时返回 1。
 */
int main(int argc, char *argv[])
{
    const char *conf = (argc >= 2) ? argv[1] : "../config/config.ini";

    printf("[gps] gps_daemon starting...\n");
    printf("[gps] Config: %s\n", conf);

    if (init_all(&g_ctx, conf) != 0) {
        fprintf(stderr, "[gps] Init failed.\n");
        return 1;
    }

    run_loop(&g_ctx);
    cleanup_all(&g_ctx);

    return 0;
}
