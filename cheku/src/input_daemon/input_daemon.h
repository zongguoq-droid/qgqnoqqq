/**
 * @file    input_daemon.h
 * @brief   按键输入管理守护进程 - 配置参数 & 数据结构
 *
 * 功能概述:
 *   - 监听 Linux input 子系统 /dev/input/eventX 的按键事件
 *   - 识别三种按键手势: 短按(SHORT) / 长按(LONG) / 双击(DOUBLE)
 *   - 通过 UNIX Domain Socket 将按键事件推送给 Qt UI 进程
 *   - 接收 Qt 通过 Socket 发来的 LED 控制指令 (MSG_LED_CONTROL)
 *   - 用户 LED 闪烁反馈: 短按触发 LED 亮起 150ms 后自动熄灭 (非 toggle)
 *   - 使用 poll() 单线程事件循环, 50ms 定时 tick 驱动按键状态机
 *
 * 架构 (poll 模式):
 *   poll([input_fd, server_fd, client_fd], 50ms)
 *   -> input_fd POLLIN  -> read input_event -> key_parser_feed -> tick -> gesture 识别
 *   -> server_fd POLLIN -> accept() 接受 Qt UI 连接 (单客户端, 新连接踢旧连接)
 *   -> client_fd POLLIN -> recv() 接收 Qt 指令 (LED 控制等)
 *
 * 手势定义:
 *   - 短按 (Short Press):   按下后在 long_press_ms 内释放, 且 double_click_ms 内无二次按下
 *   - 长按 (Long Press):    按下后持续 long_press_ms (默认 2000ms) 未释放
 *   - 双击 (Double Click):  第一次短按释放后 double_click_ms (默认 500ms) 内再次按下并释放
 *
 * 按键功能映射 (可配置):
 *   KEY1 → 菜单键, 短按=菜单 长按=语音助手
 *   KEY2 → 返回键, 短按=返回 长按=回主页
 *
 * Linux input 子系统背景:
 *   - 设备节点: /dev/input/eventX (X=0,1,2...)
 *   - struct input_event { struct timeval time; __u16 type; __u16 code; __s32 value; }
 *   - type=EV_KEY(0x01) 按键事件, code=KEY_xxx 键码, value=0释放/1按下/2重复
 *   - 可通过 /proc/bus/input/devices 查看设备列表
 *   - 内核已做硬件消抖, 应用层通过状态机做软件手势判断
 *
 * 学习要点:
 *   1. Linux input 子系统: 统一的输入设备抽象层, 通过 /dev/input/eventX 读取原始事件
 *   2. 按键状态机: IDLE -> PRESSED -> LONG_PRESS / WAIT_DOUBLE -> DOUBLE_CLICK
 *   3. poll() 多路复用: 替代 epoll, 同时监听 input 设备 fd + Socket fd, 50ms 定时驱动状态机
 *   4. UNIX Domain Socket (SOCK_STREAM): 进程间通信, Qt UI 与本守护进程通过 /tmp/car_input.sock 通信
 *   5. LED 闪烁一次 (非 toggle): gettimeofday 记录亮起时刻, 150ms 后 tick 中自动熄灭
 */

#ifndef _INPUT_DAEMON_H_
#define _INPUT_DAEMON_H_

#include "common.h"         /* ret_code_t, proc_config_t, key_event_t, ... */
#include <stdint.h>
#include <time.h>

/* ================================================================
 *  默认配置参数
 * ================================================================ */

/* ---- 输入设备 ---- */
#define INPUT_DEFAULT_DEVICE        "/dev/input/event2"  /* gpio-keys */

/* ---- 按键手势时间阈值 (毫秒) ---- */
#define INPUT_DEFAULT_LONG_PRESS_MS    2000    /* 长按判定时间 */
#define INPUT_DEFAULT_DOUBLE_CLICK_MS  500     /* 双击间隔窗口 */
#define INPUT_DEFAULT_DEBOUNCE_MS      30      /* 软件消抖时间 (ms) */

/* ---- LED ---- */
#define INPUT_LED_DEVICE            "/dev/100ask_led"  /* 100ask 内核 LED 驱动字符设备 (备用, 当前使用 led_control 模块的 sysfs 方式) */
#define INPUT_LED_BLINK_DURATION_MS 150                /* LED 闪烁持续时间 (ms): 短按后 LED 亮起此时间后自动熄灭 */

/* ---- Socket ---- */
#define INPUT_SOCKET_PATH           "/tmp/car_input.sock"

/* ---- 日志 ---- */
#define INPUT_LOG_PATH              "/var/log/car_terminal/input.log"

/* ---- poll 多路复用 ---- */
#define INPUT_MAX_POLL_FDS         8       /* poll 监听的最大 fd 数量 (input + server + client + 预留) */

/* ---- 最多支持的按键数 ---- */
#define INPUT_MAX_KEYS              8

/* ================================================================
 *  按键手势类型枚举 (与 common.h 中 key_event_t.event_type 一致)
 *  注: common.h 已定义 event_type: 1=短按, 2=长按, 3=双击, 4=释放
 * ================================================================ */

/* 按键逻辑标识 (映射 KEY1/KEY2 的 Linux 键码) */
typedef enum {
    KEY_ID_NONE     = 0,
    KEY_ID_KEY1     = 1,        /* KEY1 按键 (键码 KEY_F1 或自定义) */
    KEY_ID_KEY2     = 2,        /* KEY2 按键 (键码 KEY_F2 或自定义) */
    KEY_ID_CUSTOM   = 3,        /* 保留 */
} key_id_t;

/**
 * @brief 按键功能映射
 *
 * 将物理按键映射到逻辑功能。
 * 实际应用中 KEY1/KEY2 的 Linux 键码取决于设备树配置。
 * 常见配置: KEY1→KEY_F1(59), KEY2→KEY_F2(60)
 */
typedef struct {
    int         linux_key_code;     /* Linux input 键码 (如 KEY_F1=59) */
    key_id_t    key_id;             /* 逻辑按键 ID */
    const char *name;               /* 按键名称 (如 "KEY1-MENU") */
} key_mapping_t;

/* ================================================================
 *  按键状态机
 *
 *  每个物理按键独立跟踪状态。状态机由 key_parser_feed() 喂入 input_event,
 *  由 key_parser_tick() 每秒驱动一次 (通过 poll 50ms 循环中的 time() 检查实现)。
 *  使用 struct timeval 记录时间戳, 在 tick 中比对以判定超时。
 *
 *  状态转换图:
 *
 *                    ┌──────────────────────────────────┐
 *                    │            IDLE                   │
 *                    └──────┬──────────────┬────────────┘
 *                     press │              │ tick: double-click
 *                           ▼              │ window expired
 *                    ┌──────────┐          │ (release_time + double_click_ms)
 *                    │ PRESSED  │          │
 *                    └─┬────┬──┘          │
 *     tick: press_time │    │ release      │
 *     + long_press_ms  │    ▼              │
 *     fires             │  ┌─────────────┐  │
 *                     │  │ WAIT_DOUBLE │──┘
 *                     │  └──┬──────────┘
 *                     │     │ press within window
 *                     │     ▼
 *                     │  ┌─────────────┐
 *                     │  │ PRESSED_2ND │
 *                     │  └──┬──────────┘
 *                     │     │ release
 *                     │     ▼
 *                     │  ┌─────────────┐
 *                     │  │DOUBLE_CLICK │ → report & reset to IDLE
 *                     │  └─────────────┘
 *                     ▼
 *                  ┌─────────────┐
 *                  │ LONG_PRESS  │ → report & reset to IDLE
 *                  └─────────────┘
 *               (wait for release to return to IDLE, then report)
 *
 *  消抖机制: debounce_ms (默认 30ms) 内忽略连续的同键事件, 防止物理抖动误触发。
 *
 * ================================================================ */
typedef enum {
    KEY_STATE_IDLE          = 0,    /* 空闲, 等待按下 */
    KEY_STATE_PRESSED       = 1,    /* 已按下, 等待释放 或 长按超时 */
    KEY_STATE_LONG_PRESS    = 2,    /* 长按已触发, 等待释放 */
    KEY_STATE_WAIT_DOUBLE   = 3,    /* 已释放, 等待双击窗口内二次按下 */
    KEY_STATE_PRESSED_2ND   = 4,    /* 第二次按下, 等待释放完成双击 */
} key_state_t;

/* ---- 每按键独立状态 ---- */
typedef struct {
    int             linux_code;     /* Linux 键码 */
    key_id_t        key_id;         /* 逻辑按键 ID */
    key_state_t     state;          /* 当前状态 */
    struct timeval  press_time;     /* 按下时刻 (用于计算持续时间) */
    struct timeval  release_time;   /* 释放时刻 (用于双击窗口判断) */
    uint32_t        duration_ms;    /* 最近一次按下的持续时间 */
    int             event_pending;  /* 有待发送的事件 (0/1) */
    uint8_t         pending_type;   /* 待发送事件类型 (1=短按, 2=长按, 3=双击) */
} key_slot_t;

/* ================================================================
 *  进程配置
 * ================================================================ */
typedef struct {
    char        input_device[64];       /* 输入设备路径 */
    int         long_press_ms;          /* 长按判定时间 (ms) */
    int         double_click_ms;        /* 双击窗口时间 (ms) */
    int         debounce_ms;            /* 消抖时间 (ms) */
    /* 按键映射 (Linux键码 → 逻辑ID) */
    key_mapping_t key_map[INPUT_MAX_KEYS];
    int         key_count;              /* 已映射的按键数量 */
    /* 进程通用配置 */
    proc_config_t proc_cfg;
} input_config_t;

/* ================================================================
 *  函数声明
 * ================================================================ */

/**
 * @brief 加载按键输入配置
 *
 * 优先从 config.ini [input] 节读取, 文件不存在或读取失败则使用硬编码默认值。
 * 配置项包括: 输入设备路径、长按/双击时间阈值、Socket 路径。
 * 按键映射表 (KEY1/KEY2 的 Linux 键码 -> 逻辑 ID) 使用编译期硬编码,
 * 不从配置文件读取 (实际键码由设备树 gpio-keys 决定)。
 *
 * @param cfg       输出: 配置结构体 (调用前会被 memset 清零)
 * @param conf_file 配置文件路径 (NULL = 全部使用默认值)
 * @return          RET_OK 成功, RET_ERR_PARAM 参数无效
 */
int input_config_load(input_config_t *cfg, const char *conf_file);

/**
 * @brief 在 /proc/bus/input/devices 中自动查找按键输入设备
 *
 * 解析 /proc/bus/input/devices 文件, 扫描所有 Handlers 行,
 * 找到第一个包含 EV=... 且注册了 EV_KEY 事件的 eventX 设备。
 * 在 i.MX6ULL 开发板上通常为 /dev/input/event0 (无触摸屏时)
 * 或 /dev/input/event2 (gpio-keys)。
 *
 * 注意: 此函数为辅助工具函数, 当前 main() 直接使用默认设备路径,
 * 仅在需要自动探测设备时调用。
 *
 * @param out_device  输出: 设备路径缓冲区 (如 "/dev/input/event2")
 * @param buf_size    缓冲区大小 (建议 >= 64)
 * @return            RET_OK 成功, RET_ERR_NOT_FOUND 未找到符合条件的设备
 */
int input_find_device(char *out_device, size_t buf_size);

#endif /* _INPUT_DAEMON_H_ */
