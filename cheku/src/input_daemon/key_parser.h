/**
 * @file    key_parser.h
 * @brief   按键事件解析与手势识别 - 头文件
 *
 * 对 Linux input 子系统的原始事件进行:
 *   1. 读取 struct input_event
 *   2. 按键码映射 (Linux code → 逻辑 key_id)
 *   3. 软件消抖
 *   4. 手势状态机 (短按/长按/双击)
 *   5. 生成 key_event_t 上报事件
 *
 * 输入设备驱动的数据格式:
 *   struct input_event {
 *       struct timeval time;   // 事件时间戳 (内核生成, 精度 μs)
 *       __u16 type;            // 事件类型: EV_KEY=0x01, EV_SYN=0x00
 *       __u16 code;            // 键码: KEY_F1=59, KEY_ENTER=28, ...
 *       __s32 value;           // 值: 0=释放, 1=按下, 2=自动重复
 *   };
 *
 *   每次按键产生三个事件:
 *     [EV_KEY, KEY_F1, 1]    ← 按下
 *     [EV_SYN, SYN_REPORT, 0] ← 同步分隔符
 *     [EV_KEY, KEY_F1, 0]    ← 释放
 *     [EV_SYN, SYN_REPORT, 0] ← 同步分隔符
 *
 * 设计要点:
 *   - 读取时必须按 struct input_event 大小 (16或24字节) 对齐读取
 *   - EV_SYN/SYN_REPORT 是帧分隔符, 表示一组完整事件结束
 *   - 内核驱动已完成硬件消抖, 到达应用层的 value=2(重复) 需要适当处理
 */

#ifndef _KEY_PARSER_H_
#define _KEY_PARSER_H_

#include "input_daemon.h"
#include <stdint.h>
#include <linux/input.h>    /* struct input_event, EV_KEY, EV_SYN, KEY_* */

/* ================================================================
 *  按键解析器上下文
 * ================================================================ */

/**
 * @brief 按键解析器 (状态机 + 事件队列)
 *
 * 管理所有按键的状态机, 驱动手势识别。
 * 外部 (input_daemon.c) 需要:
 *   1. 调用 key_parser_feed() 喂入原始 input_event
 *   2. 周期性调用 key_parser_tick() 驱动定时器 (如每 10ms)
 *   3. 调用 key_parser_get_event() 取出已识别的手势事件
 */
typedef struct {
    key_slot_t      slots[INPUT_MAX_KEYS];  /* 按键状态槽 */
    int             slot_count;             /* 已激活的槽数 */
    key_mapping_t  *key_map;               /* 按键映射表 (指外部内存) */
    int             map_count;              /* 映射表条目数 */
    int             long_press_ms;          /* 长按阈值 (ms) */
    int             double_click_ms;        /* 双击窗口 (ms) */
    int             debounce_ms;            /* 消抖时间 (ms) */

    /* 事件环形队列 (生产者=feed, 消费者=get_event) */
    key_event_t     event_queue[16];
    int             event_head;             /* 写入位置 */
    int             event_tail;             /* 读取位置 */
    int             event_count;            /* 队列中事件数 */
} key_parser_t;

/* ================================================================
 *  API 函数声明
 * ================================================================ */

/**
 * @brief 初始化按键解析器
 *
 * @param parser            解析器实例
 * @param key_map           按键映射表
 * @param map_count         映射条目数
 * @param long_press_ms     长按判定时间 (ms)
 * @param double_click_ms   双击窗口 (ms)
 * @param debounce_ms       消抖时间 (ms)
 */
void key_parser_init(key_parser_t *parser,
                     key_mapping_t *key_map, int map_count,
                     int long_press_ms, int double_click_ms,
                     int debounce_ms);

/**
 * @brief 喂入一个原始 input_event
 *
 * 由 epoll 循环中读取 /dev/input/eventX 后调用。
 * 内部完成:
 *   1. 按键码 → 逻辑ID 映射
 *   2. 消抖过滤
 *   3. 驱动状态机
 *   4. 手势识别后入队
 *
 * @param parser  解析器实例
 * @param ev      原始 input_event (从设备读取)
 * @return        非0=产生了新手势事件 (需调用 key_parser_get_event 取出)
 */
int key_parser_feed(key_parser_t *parser, const struct input_event *ev);

/**
 * @brief 周期性驱动状态机的定时器逻辑
 *
 * 必须周期性调用 (建议每 ~10ms)。
 * 检查所有按键的:
 *   - 长按超时 (PRESSED 状态持续超过 long_press_ms)
 *   - 双击窗口超时 (WAIT_DOUBLE 状态超过 double_click_ms)
 *
 * @param parser  解析器实例
 * @return        非0=产生了新手势事件
 */
int key_parser_tick(key_parser_t *parser);

/**
 * @brief 取出已识别的手势事件 (消费)
 *
 * @param parser  解析器实例
 * @param out     输出: 手势事件数据
 * @return        0=成功取出, -1=队列为空
 */
int key_parser_get_event(key_parser_t *parser, key_event_t *out);

/**
 * @brief 根据 Linux 键码查找逻辑按键槽位
 * @param parser     解析器实例
 * @param linux_code Linux 键码
 * @return           槽位指针, NULL=未找到
 */
key_slot_t* key_parser_find_slot(key_parser_t *parser, int linux_code);

/**
 * @brief 根据 Linux 键码查找映射的逻辑 ID
 * @param parser     解析器实例
 * @param linux_code Linux 键码
 * @return           逻辑按键 ID, KEY_ID_NONE=未映射
 */
key_id_t key_parser_lookup(key_parser_t *parser, int linux_code);

#endif /* _KEY_PARSER_H_ */
