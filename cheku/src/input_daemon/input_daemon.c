/**
 * @file    input_daemon.c
 * @brief   按键输入管理守护进程 (poll 模式)
 *
 * ============================================================================
 * 架构概述
 * ============================================================================
 *
 * 本守护进程是车载终端输入子系统的核心, 负责:
 *   1. 监听 Linux input 子系统 (/dev/input/eventX) 的物理按键事件
 *   2. 通过按键状态机识别手势 (短按/长按/双击)
 *   3. 短按时驱动用户 LED 闪烁一次 (亮 150ms 后自动熄灭, 非 toggle)
 *   4. 通过 UNIX Domain Socket 将按键事件推送给 Qt UI 进程
 *   5. 接收 Qt UI 通过 Socket 发来的控制指令 (如 LED 控制)
 *
 * ============================================================================
 * 主循环 (poll 模式, 单线程)
 * ============================================================================
 *
 *   while (running) {
 *       poll([input_fd, server_fd, client_fd], 50ms)
 *
 *       // 路径 1: 物理按键事件
 *       if input_fd POLLIN:
 *           read input_event -> key_parser_feed() 喂入状态机
 *
 *       // 路径 2: 每隔 1 秒驱动一次状态机 tick (通过 time() 秒级变化检测)
 *       if time(NULL) changed:
 *           key_parser_tick() 检查超时 (长按判定 / 双击窗口过期)
 *           取出已识别的手势 -> key_parser_get_event()
 *           手势处理:
 *             - 短按: led_on() + 记录亮起时刻 (led_on_time)
 *             - 所有手势: sock_send_frame() 推送 key_event_t 给 Qt UI
 *
 *       // 路径 3: Socket 事件
 *       handle_sockets():
 *         - server_fd POLLIN: accept() 接受 Qt UI 连接 (新连接踢旧)
 *         - client_fd POLLIN: recv() -> protocol_unpack() -> 处理指令
 *           支持 MSG_LED_CONTROL: Qt 远程控制 LED 开关
 *
 *       // 路径 4: LED 闪烁超时检查
 *       if led_blinking:
 *           计算自 led_on_time 起的经过时间
 *           if elapsed >= 150ms: led_off() 熄灭 LED
 *   }
 *
 * ============================================================================
 * 关键设计决策
 * ============================================================================
 *
 * - 使用 poll() 而非 epoll: 本场景 fd 数量极少 (<=3), poll 更简单直接
 * - LED 闪烁一次而非 toggle: 短按触发 LED 亮 150ms 后自动熄灭,
 *   使用 gettimeofday 微秒级计时, 在每次 poll 循环末尾检查超时
 * - 单客户端模式: 新的 Qt 连接会踢掉旧连接 (典型的一对一 IPC)
 * - 1 秒 tick: 手势时间判定精度为秒级, 不需要更高频率的定时器
 *
 * ============================================================================
 * 已知 Bug 修复记录
 * ============================================================================
 *
 * [已修复] LED toggle 改为 blink-once:
 *   问题: 原先每次短按 LED toggle 一次 (亮<->灭), 语义不明确
 *   修复: 改为短按后亮起 150ms 自动熄灭, 与按键操作形成明确对应关系
 *   实现: 使用 led_on_time 记录亮起时刻, 在 poll 循环中检查 150ms 超时
 */

#include "input_daemon.h"
#include "key_parser.h"
#include "led_control.h"
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
#include <time.h>
#include <sys/time.h>       /* gettimeofday, struct timeval */
#include <linux/input.h>

static volatile int g_running = 1;
static void sig_handler(int s) { (void)s; g_running = 0; }

#define LED_BLINK_MS  150   /* LED 闪烁持续时间 */

/**
 * @brief 输入守护进程全局上下文
 *
 * 所有状态集中管理, 作为全局变量 g_ctx 使用。
 */
typedef struct {
    input_config_t      cfg;            /* 配置 (设备路径、时间阈值、Socket 路径等) */
    key_parser_t        parser;         /* 按键状态机 (手势识别核心) */
    int                 input_fd;       /* input 设备文件描述符 (/dev/input/eventX) */
    int                 server_fd;      /* UNIX Socket 监听 fd (accept Qt 连接) */
    int                 client_fd;      /* Qt UI 连接 fd (-1 表示无连接) */
    protocol_parser_t   proto_parser;   /* 协议解析器 (从字节流中提取完整帧) */
    ringbuffer_t        recv_rb;        /* 接收环形缓冲区 (暂存来自 Qt 的原始字节) */
    time_t              last_tick;      /* 上次 tick 时刻 (秒级, 用于每秒驱动一次状态机) */
    time_t              last_alert_time;/* 上次告警时间 (预留, 用于限流告警输出) */
    int                 led_blinking;   /* LED 闪烁状态: 1=已亮起等待超时熄灭, 0=空闲 */
    struct timeval      led_on_time;    /* LED 亮起时刻 (微秒精度, 用于计算 150ms 超时) */
} input_ctx_t;

static input_ctx_t g_ctx;

/* 默认按键映射 (设备树 gpio-keys: KEY_1=2, KEY_2=3) */
static key_mapping_t g_key_map[] = {
    { KEY_1, KEY_ID_KEY1, "KEY1" },
    { KEY_2, KEY_ID_KEY2, "KEY2" },
};

/**
 * @brief 加载按键输入配置
 *
 * 从 config.ini [input] 节读取配置, 失败则回退到硬编码默认值。
 * 按键映射表 (Linux 键码 -> 逻辑 key_id) 使用编译期 g_key_map,
 * 不从配置文件读取 —— 键码由设备树 gpio-keys 节点决定, 修改需重新编译。
 *
 * @param cfg       输出: 配置结构体 (调用前被 memset 清零)
 * @param conf_file 配置文件路径 (NULL 则全部使用默认值)
 * @return          RET_OK 成功, RET_ERR_PARAM cfg 为 NULL
 */
int input_config_load(input_config_t *cfg, const char *conf_file)
{
    config_t conf; int loaded = 0;
    if (!cfg) return RET_ERR_PARAM;
    memset(cfg, 0, sizeof(*cfg));

    /* 尝试加载配置文件, 失败则使用默认值 */
    if (conf_file && config_load(&conf, conf_file) == 0) loaded = 1;

    /* 输入设备路径: config.ini [input] device=... */
    if (loaded)
        config_get_str(&conf, "input", "device", INPUT_DEFAULT_DEVICE, cfg->input_device, sizeof(cfg->input_device));
    else
        strncpy(cfg->input_device, INPUT_DEFAULT_DEVICE, sizeof(cfg->input_device)-1);

    /* 手势时间阈值: 可配置, 有合理默认值 */
    cfg->long_press_ms   = loaded ? config_get_int(&conf,"input","long_press_ms",INPUT_DEFAULT_LONG_PRESS_MS) : INPUT_DEFAULT_LONG_PRESS_MS;
    cfg->double_click_ms = loaded ? config_get_int(&conf,"input","double_click_ms",INPUT_DEFAULT_DOUBLE_CLICK_MS) : INPUT_DEFAULT_DOUBLE_CLICK_MS;
    cfg->debounce_ms     = INPUT_DEFAULT_DEBOUNCE_MS;  /* 消抖时间固定, 暂不开放配置 */

    /* 按键映射: 编译期固定 (设备树 gpio-keys 定义键码) */
    memcpy(cfg->key_map, g_key_map, sizeof(g_key_map));
    cfg->key_count = sizeof(g_key_map) / sizeof(g_key_map[0]);

    /* Socket 路径: 固定, 从 input_daemon.h 宏定义 */
    snprintf(cfg->proc_cfg.socket_path, sizeof(cfg->proc_cfg.socket_path), "%s", INPUT_SOCKET_PATH);
    if (loaded) config_unload(&conf);

    LOG_INFO("input","Config: device=%s long=%dms double=%dms",
             cfg->input_device, cfg->long_press_ms, cfg->double_click_ms);
    return RET_OK;
}

/**
 * @brief 处理 Socket 事件: 接受 Qt UI 连接 + 接收控制指令
 *
 * 本函数在每次 poll 循环中被调用, 处理两类 Socket 事件:
 *
 * 1. server_fd (监听 Socket) POLLIN:
 *    - accept() 接受新客户端连接
 *    - 单客户端策略: 如果已有连接 (client_fd >= 0), 关闭新连接
 *    - 连接建立后重置协议解析器状态
 *
 * 2. client_fd (客户端连接) POLLIN / POLLHUP / POLLERR:
 *    - recv() 读取原始字节, 写入环形缓冲区
 *    - protocol_unpack() 循环解包, 提取完整协议帧
 *    - 当前支持的指令:
 *        MSG_LED_CONTROL: d[0]=1 点亮 LED, d[0]=0 熄灭 LED
 *    - 收到 0 字节 (对端关闭) 或 -1 (错误) 时断开连接
 *
 * 注意: Qt 通过此 Socket 发送 MSG_LED_CONTROL 实现远程 LED 控制,
 *       与本地短按触发的 150ms 闪烁是独立的两条控制路径。
 *
 * @param ctx    全局上下文
 * @param srv_ev server_fd 的 poll revents
 * @param cli_ev client_fd 的 poll revents (仅当 client_fd >= 0 时有效)
 */
static void handle_sockets(input_ctx_t *ctx, int srv_ev, int cli_ev)
{
    /* --- 接受新的 Qt UI 连接 --- */
    if (srv_ev & POLLIN) {
        int fd = sock_accept(ctx->server_fd);
        if (fd >= 0) {
            if (ctx->client_fd >= 0) sock_close(fd, NULL);  /* 已有连接, 拒绝新连接 */
            else { ctx->client_fd = fd; protocol_reset(&ctx->proto_parser, &ctx->recv_rb);
                   LOG_INFO("input","UI connected: fd=%d",fd); }
        }
    }
    /* --- 处理 Qt UI 发来的数据 --- */
    if (ctx->client_fd < 0) return;
    if (cli_ev & (POLLIN|POLLHUP|POLLERR)) {
        uint8_t buf[512]; int n = sock_recv(ctx->client_fd, buf, sizeof(buf));
        if (n <= 0) { LOG_INFO("input","UI disconnected"); sock_close(ctx->client_fd,NULL); ctx->client_fd=-1; return; }
        /* 将原始字节写入环形缓冲区, 由协议解析器逐帧提取 */
        ringbuffer_write(&ctx->recv_rb, buf, (uint32_t)n);
        uint8_t t,d[256]; uint32_t l;
        while (protocol_unpack(&ctx->proto_parser, &ctx->recv_rb, &t, d, sizeof(d), &l) == 0) {
            if (t == MSG_LED_CONTROL && l >= 1) {
                /* Qt 远程 LED 控制: d[0]=1 亮, d[0]=0 灭 */
                if (d[0]) led_on(0); else led_off(0);
            }
        }
    }
}

/**
 * @brief 输入守护进程主函数
 *
 * 初始化 -> poll 事件循环 -> 清理退出的标准守护进程生命周期。
 *
 * ============================================================================
 * 初始化阶段:
 *   1. 加载配置 (config.ini 或默认值)
 *   2. 初始化 LED 控制模块 (sysfs 或 /dev 字符设备)
 *   3. 打开 input 设备 (非阻塞 O_NONBLOCK)
 *   4. 初始化按键状态机 (key_parser_init, 绑定按键映射和时间阈值)
 *   5. 创建 UNIX Domain Socket 监听 (Qt UI 连接端点)
 *   6. 初始化协议解析器和接收环形缓冲区
 *   7. 注册信号处理 (SIGTERM/SIGINT 优雅退出)
 *
 * ============================================================================
 * 主循环 (每次迭代约 50ms):
 *
 *   Step 1 - poll() 等待事件:
 *     poll([input_fd, server_fd, client_fd], 50ms)
 *     超时 50ms 保证循环频率, 即使无事件也能驱动 LED 超时检查
 *
 *   Step 2 - 读取 input 事件:
 *     非阻塞 read() 循环读取 input_event, 喂入 key_parser_feed()
 *     (一次 poll 可能对应多个 input_event, 需全部读完)
 *
 *   Step 3 - 状态机 tick (每秒一次):
 *     通过 time(NULL) 秒级变化触发, 而非每次循环都 tick
 *     key_parser_tick() 检查各按键超时 (长按/双击窗口过期)
 *     key_parser_get_event() 取出已识别的手势:
 *       - 短按 (event_type=1): LED 亮起 150ms 后熄灭 (blink-once)
 *       - 所有手势: 通过 Socket 打包为 MSG_KEY_EVENT 帧发送给 Qt UI
 *
 *   Step 4 - Socket 事件处理:
 *     handle_sockets(): 接受 Qt 连接 + 接收并处理控制指令
 *
 *   Step 5 - LED 闪烁超时:
 *     如果 LED 正在闪烁, 计算自 led_on_time 起的毫秒数,
 *     超过 LED_BLINK_MS (150ms) 则调用 led_off() 熄灭
 *
 * ============================================================================
 * 清理阶段:
 *   关闭客户端连接 -> 销毁环形缓冲区 -> 删除 Socket 文件 ->
 *   关闭 CAN fd -> 释放 LED -> 关闭日志
 *
 * @param argc 参数个数
 * @param argv argv[1] 可选: 配置文件路径 (默认 "../config/config.ini")
 * @return      0 正常退出, 1 初始化失败
 */
int main(int argc, char *argv[])
{
    const char *conf = (argc >= 2) ? argv[1] : "../config/config.ini";
    printf("[input] input_daemon starting...\n");

    /* ---- 初始化全局上下文 ---- */
    memset(&g_ctx, 0, sizeof(g_ctx)); g_ctx.client_fd = -1;  /* -1 表示无 Qt 连接 */
    log_init(INPUT_LOG_PATH, LOG_INFO, 1024*1024, 3);
    LOG_INFO("input","===== input_daemon starting =====");

    /* ---- 加载配置 ---- */
    input_config_load(&g_ctx.cfg, conf);

    /* ---- 初始化硬件 ---- */
    led_init();

    /* ---- 打开 input 设备 (非阻塞) ---- */
    g_ctx.input_fd = open(g_ctx.cfg.input_device, O_RDONLY | O_NONBLOCK);
    if (g_ctx.input_fd < 0) {
        LOG_ERROR("input","Cannot open %s: %s", g_ctx.cfg.input_device, strerror(errno));
        return 1;
    }
    LOG_INFO("input","Input device: %s (fd=%d)", g_ctx.cfg.input_device, g_ctx.input_fd);

    /* ---- 初始化按键状态机 ---- */
    key_parser_init(&g_ctx.parser, g_ctx.cfg.key_map, g_ctx.cfg.key_count,
                    g_ctx.cfg.long_press_ms, g_ctx.cfg.double_click_ms, g_ctx.cfg.debounce_ms);

    /* ---- 创建 Socket 服务端 + 协议解析器 ---- */
    g_ctx.server_fd = sock_create_server(INPUT_SOCKET_PATH);
    if (g_ctx.server_fd < 0) return 1;
    ringbuffer_init(&g_ctx.recv_rb, 4096);
    protocol_parser_init(&g_ctx.proto_parser);

    /* ---- 注册信号处理 (优雅退出) ---- */
    signal(SIGTERM, sig_handler); signal(SIGINT, sig_handler);
    g_ctx.last_tick = time(NULL);

    LOG_INFO("input","Init complete. Loop starting...");

    /* ========================================================================
     *  主事件循环 (poll 模式)
     * ======================================================================== */
    while (g_running) {
        /* -- 构建 poll fd 数组 -- */
        struct pollfd pfds[4]; int nfds = 0;
        pfds[nfds].fd = g_ctx.input_fd;  pfds[nfds].events = POLLIN; nfds++;  /* [0] input 设备 */
        pfds[nfds].fd = g_ctx.server_fd; pfds[nfds].events = POLLIN; nfds++;  /* [1] 监听 Socket */
        if (g_ctx.client_fd >= 0) { pfds[nfds].fd = g_ctx.client_fd; pfds[nfds].events = POLLIN; nfds++; } /* [2] 客户端 */

        int ret = poll(pfds, nfds, 50);  /* 50ms 超时: 保证 LED 闪烁检查频率 */
        if (ret < 0) { if (errno == EINTR) continue; break; }

        /* -- Step 1: 读取 input 事件, 喂入状态机 -- */
        if (pfds[0].revents & POLLIN) {
            struct input_event ev;
            while (read(g_ctx.input_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                key_parser_feed(&g_ctx.parser, &ev);
            }
        }

        /* -- Step 2: 每秒驱动一次状态机 tick, 取出已识别的手势 -- */
        time_t now = time(NULL);
        if (now != g_ctx.last_tick) {
            key_parser_tick(&g_ctx.parser);          /* 检查超时: 长按判定 / 双击窗口过期 */
            g_ctx.last_tick = now;

            /* 取出所有已识别的手势事件 */
            key_event_t kev;
            while (key_parser_get_event(&g_ctx.parser, &kev) == 0) {
                const char *t = kev.event_type == 1 ? "SHORT" : kev.event_type == 2 ? "LONG" : "DOUBLE";
                LOG_INFO("input","Key%d %s (dur=%ums)", kev.key_id, t, kev.duration_ms);

                /*
                 * LED 闪烁一次 (blink-once, 非 toggle):
                 * 仅短按时触发。立即亮起 LED, 记录亮起时刻,
                 * 在后续 poll 循环中检查 150ms 超时后自动熄灭。
                 */
                if (kev.event_type == 1) {  /* SHORT press */
                    led_on(0);
                    gettimeofday(&g_ctx.led_on_time, NULL);
                    g_ctx.led_blinking = 1;
                }

                /* 将按键事件通过 Socket 推送给 Qt UI */
                if (g_ctx.client_fd >= 0)
                    sock_send_frame(g_ctx.client_fd, MSG_KEY_EVENT, (uint8_t*)&kev, sizeof(kev));
            }
        }

        /* -- Step 3: 处理 Socket 事件 (accept / recv 指令) -- */
        handle_sockets(&g_ctx, pfds[1].revents, nfds >= 3 ? pfds[2].revents : 0);

        /* -- Step 4: LED 闪烁超时检查 -- */
        if (g_ctx.led_blinking) {
            struct timeval now_tv;
            gettimeofday(&now_tv, NULL);
            long elapsed_ms = (now_tv.tv_sec - g_ctx.led_on_time.tv_sec) * 1000
                            + (now_tv.tv_usec - g_ctx.led_on_time.tv_usec) / 1000;
            if (elapsed_ms >= LED_BLINK_MS) {  /* 150ms 超时 */
                led_off(0);
                g_ctx.led_blinking = 0;
            }
        }
    }

    /* ---- 清理资源 ---- */
    LOG_INFO("input","Exiting...");
    if (g_ctx.client_fd >= 0) sock_close(g_ctx.client_fd, NULL);
    ringbuffer_destroy(&g_ctx.recv_rb);
    sock_close(g_ctx.server_fd, INPUT_SOCKET_PATH);  /* 关闭监听 Socket 并删除文件 */
    if (g_ctx.input_fd >= 0) close(g_ctx.input_fd);
    led_deinit();
    log_close();
    printf("[input] input_daemon stopped.\n");
    return 0;
}
