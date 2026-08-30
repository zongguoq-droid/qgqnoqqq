/**
 * @file    key_parser.c
 * @brief   按键事件解析与手势识别 - 实现
 *
 * ================================================================
 *  核心算法: 按键手势状态机
 * ================================================================
 *
 * 每个按键独立运行以下状态机:
 *
 *                      ┌──────────┐
 *            ┌────────│  IDLE    │◄──────────────────────────┐
 *            │        └────┬─────┘                           │
 *            │       press │                                 │
 *            │   (value=1) ▼                                 │
 *            │        ┌──────────┐                           │
 *            │  ┌────│ PRESSED  │────┐                       │
 *            │  │     └────┬─────┘    │                       │
 *            │  │  release│    tick:  │                       │
 *            │  │(value=0)│  held >  │                       │
 *            │  │         │ LONG_MS  │                       │
 *            │  ▼         │          ▼                       │
 *            │ ┌────────┐│   ┌────────────┐                  │
 *            │ │ WAIT   ││   │ LONG_PRESS │                  │
 *            │ │ DOUBLE ││   │ (report)   │                  │
 *            │ └──┬─────┘│   └─────┬──────┘                  │
 *            │    │      │   release│                         │
 *            │    │ tick:│   (wait) │                         │
 *            │    │ >DC  │          │                         │
 *            │    │ _MS  │          │                         │
 *            │    ▼      │          │                         │
 *            │ ┌──────┐  │          │                         │
 *            │ │SHORT │  │          │                         │
 *            │ │PRESS │  │          │                         │
 *            │ │report│  │          │                         │
 *            │ └──┬───┘  │          │                         │
 *            │    │      │          │                         │
 *            │    └──────┘          │                         │
 *            │                     │                         │
 *            │    press within     │                         │
 *            │    DC window?       │                         │
 *            │    ┌────────────────┘                         │
 *            │    ▼                                         │
 *            │ ┌─────────────┐                              │
 *            │ │ PRESSED_2ND │                              │
 *            │ └──────┬──────┘                              │
 *            │  release│                                    │
 *            │         ▼                                    │
 *            │ ┌──────────────┐                             │
 *            │ │ DOUBLE_CLICK │                             │
 *            │ │ (report)     │                             │
 *            │ └──────────────┘                             │
 *            └──────────────────────────────────────────────┘
 *
 * 消抖策略:
 *   Linux input 子系统在驱动层已做硬件消抖。本模块额外做一层
 *   软件消抖: 忽略间隔小于 debounce_ms 的连续事件。
 *   这对于某些机械按键的抖动 (bounce) 有额外保护作用。
 *
 * 自动重复 (auto-repeat):
 *   长时间按住按键时, 内核会发送 value=2 的重复事件。
 *   本模块忽略 value=2, 仅用 value=1(首次按下) 和 value=0(释放)。
 *
 * 学习要点:
 *   1. 有限状态机 (FSM) — 手势识别的经典实现
 *   2. struct timeval 时间计算
 *   3. 环形队列 (事件缓冲)
 *   4. 消抖 (debounce) — 嵌入式输入的必备处理
 */

#include "key_parser.h"
#include "log/log.h"

#include <stdlib.h>     /* NULL */
#include <string.h>     /* memset, memcpy */
#include <sys/time.h>   /* struct timeval, timersub */

/* ================================================================
 *  内部辅助函数
 * ================================================================ */

/**
 * @brief 计算两个 timeval 之间的毫秒差
 *
 * 使用 Linux 提供的 timersub 宏进行安全的 timeval 减法:
 *   timersub(a, b, result) → result = a - b
 *   result.tv_sec 可能为负 (不适用于本场景, 但需要处理)
 *
 * @param later   较晚的时间
 * @param earlier 较早的时间
 * @return        时间差 (毫秒), 最大返回 INT_MAX
 */
static int timeval_diff_ms(const struct timeval *later,
                           const struct timeval *earlier)
{
    struct timeval diff;
    int ms;

    timersub(later, earlier, &diff);

    /* 处理负值 (时钟回拨等异常情况) */
    if (diff.tv_sec < 0) return 0;

    /* 计算毫秒: tv_sec*1000 + tv_usec/1000 */
    ms = (int)(diff.tv_sec * 1000 + diff.tv_usec / 1000);

    return ms;
}

/**
 * @brief 获取当前时间的 timeval
 * @param tv  输出: 当前时间
 */
static void get_now(struct timeval *tv)
{
    /* gettimeofday 精度为微秒, Linux 上通过 VDSO 加速 (不陷入内核)
     * 对于按键手势识别 (ms 级精度) 绰绰有余 */
    gettimeofday(tv, NULL);
}

/**
 * @brief 将手势事件加入队列
 *
 * 环形队列: event_head 写入, event_tail 读取。
 * 如果队列满, 丢弃最旧的事件 (覆盖策略: 旧事件不如新事件重要)。
 *
 * @param parser  解析器实例
 * @param key_id  逻辑按键 ID
 * @param type    事件类型 (1=短按, 2=长按, 3=双击)
 * @param duration_ms 按下持续时间
 */
static void event_enqueue(key_parser_t *parser,
                          key_id_t key_id, uint8_t type,
                          uint32_t duration_ms)
{
    key_event_t *ev;

    /* 队列满: 丢弃头 (最旧) 事件 */
    if (parser->event_count >= 16) {
        parser->event_tail = (parser->event_tail + 1) % 16;
        parser->event_count--;
    }

    /* 写入新事件 */
    ev = &parser->event_queue[parser->event_head];
    memset(ev, 0, sizeof(*ev));
    ev->key_id      = (uint8_t)key_id;
    ev->event_type  = type;
    ev->duration_ms = duration_ms;
    ev->timestamp   = time(NULL);

    parser->event_head = (parser->event_head + 1) % 16;
    parser->event_count++;
}

/**
 * @brief 查找或创建按键槽位
 *
 * 首次遇到某 Linux 键码时动态创建槽位。
 * 这样不需要预先知道所有按键的键码。
 *
 * @param parser     解析器实例
 * @param linux_code Linux 键码
 * @return           槽位指针, NULL=槽位已满
 */
static key_slot_t* find_or_create_slot(key_parser_t *parser, int linux_code)
{
    int i;

    /* 先查找已有槽位 */
    for (i = 0; i < parser->slot_count; i++) {
        if (parser->slots[i].linux_code == linux_code) {
            return &parser->slots[i];
        }
    }

    /* 未找到: 尝试创建新槽位 */
    if (parser->slot_count >= INPUT_MAX_KEYS) {
        LOG_WARN("key_parser", "Key slot full, ignoring code=%d", linux_code);
        return NULL;
    }

    /* 初始化新槽位 */
    i = parser->slot_count;
    memset(&parser->slots[i], 0, sizeof(key_slot_t));
    parser->slots[i].linux_code = linux_code;
    parser->slots[i].key_id     = key_parser_lookup(parser, linux_code);
    parser->slots[i].state      = KEY_STATE_IDLE;
    parser->slot_count++;

    LOG_DEBUG("key_parser", "New key slot: code=%d → key_id=%d",
              linux_code, parser->slots[i].key_id);

    return &parser->slots[i];
}

/* ================================================================
 *  API 实现
 * ================================================================ */

void key_parser_init(key_parser_t *parser,
                     key_mapping_t *key_map, int map_count,
                     int long_press_ms, int double_click_ms,
                     int debounce_ms)
{
    if (!parser) return;

    memset(parser, 0, sizeof(*parser));
    parser->key_map         = key_map;
    parser->map_count       = map_count;
    parser->long_press_ms   = long_press_ms;
    parser->double_click_ms = double_click_ms;
    parser->debounce_ms     = debounce_ms;

    LOG_INFO("key_parser", "Initialized: long=%dms double=%dms debounce=%dms",
             long_press_ms, double_click_ms, debounce_ms);
}

key_id_t key_parser_lookup(key_parser_t *parser, int linux_code)
{
    int i;

    if (!parser || !parser->key_map) return KEY_ID_NONE;

    for (i = 0; i < parser->map_count; i++) {
        if (parser->key_map[i].linux_key_code == linux_code) {
            return parser->key_map[i].key_id;
        }
    }

    return KEY_ID_NONE;  /* 未映射的按键 */
}

key_slot_t* key_parser_find_slot(key_parser_t *parser, int linux_code)
{
    int i;

    if (!parser) return NULL;

    for (i = 0; i < parser->slot_count; i++) {
        if (parser->slots[i].linux_code == linux_code) {
            return &parser->slots[i];
        }
    }

    return NULL;
}

int key_parser_feed(key_parser_t *parser, const struct input_event *ev)
{
    key_slot_t *slot;
    struct timeval now;
    int elapsed;

    if (!parser || !ev) return 0;

    /* 只处理 EV_KEY 事件 (忽略 EV_SYN, EV_REL 等) */
    if (ev->type != EV_KEY) return 0;

    /* 忽略自动重复事件 (value=2)
     *
     * Linux 输入子系统在按键持续按住时会发送 value=2 的重复事件。
     * 重复间隔由驱动或全局 kbdrate 设置 (通常 ~30ms)。
     * 对于手势识别, 我们只需要 value=1(按下)和 value=0(释放)。 */
    if (ev->value == 2) return 0;

    /* 查找或创建槽位 */
    slot = find_or_create_slot(parser, ev->code);
    if (!slot) return 0;

    /* 忽略未映射的按键 (无逻辑 key_id) */
    if (slot->key_id == KEY_ID_NONE) return 0;

    get_now(&now);

    if (ev->value == 1) {
        /* ==========================================
         *  按键按下 (EV_KEY value=1)
         * ========================================== */

        switch (slot->state) {

        case KEY_STATE_IDLE:
            /* 空闲 → 按下: 记录按下时间, 进入 PRESSED */
            slot->state      = KEY_STATE_PRESSED;
            slot->press_time = ev->time;  /* 使用内核时间戳 (更准确) */
            LOG_DEBUG("key_parser", "Key %d PRESSED (code=%d)",
                      slot->key_id, slot->linux_code);
            break;

        case KEY_STATE_WAIT_DOUBLE:
            /* 释放后再次按下 → 可能是双击!
             * 检查是否在双击窗口内 */
            elapsed = timeval_diff_ms(&ev->time, &slot->release_time);
            if (elapsed <= parser->double_click_ms) {
                /* 双击窗口内的第二次按下: 进入 PRESSED_2ND */
                slot->state      = KEY_STATE_PRESSED_2ND;
                slot->press_time = ev->time;
                LOG_DEBUG("key_parser", "Key %d DOUBLE-CLICK 2nd press (elapsed=%dms)",
                          slot->key_id, elapsed);
            } else {
                /* 窗口已过: 重新当作新按下的短按 */
                slot->state      = KEY_STATE_PRESSED;
                slot->press_time = ev->time;
            }
            break;

        case KEY_STATE_LONG_PRESS:
            /* 长按已报告的重复按下: 忽略 (等待释放) */
            break;

        default:
            /* PRESSED / PRESSED_2ND 状态下收到 press: 消抖忽略 */
            break;
        }

    } else if (ev->value == 0) {
        /* ==========================================
         *  按键释放 (EV_KEY value=0)
         * ========================================== */

        switch (slot->state) {

        case KEY_STATE_PRESSED:
            /* 按下后释放: 计算持续时间 */
            slot->duration_ms = (uint32_t)timeval_diff_ms(&ev->time,
                                                          &slot->press_time);

            /* 消抖检查: 持续时间太短 (< debounce_ms) 视为抖动 */
            if (slot->duration_ms < (uint32_t)parser->debounce_ms) {
                LOG_DEBUG("key_parser", "Key %d debounced (duration=%ums < %dms)",
                          slot->key_id, slot->duration_ms, parser->debounce_ms);
                slot->state = KEY_STATE_IDLE;
                break;
            }

            if (slot->duration_ms >= (uint32_t)parser->long_press_ms) {
                /* 已经满足长按条件 (tick 可能已经触发了
                 * 但释放前 tick 也可能会触发; 这里做保守处理:
                 * 如果已经超过长按时长, 按长按上报) */
                slot->state = KEY_STATE_LONG_PRESS;
                /* 立即排队长按事件 */
                event_enqueue(parser, slot->key_id,
                             2,  /* 长按 = 2 */
                             slot->duration_ms);
                slot->event_pending = 0;
                /* 长按处理后重置 */
                slot->state = KEY_STATE_IDLE;
                LOG_DEBUG("key_parser", "Key %d LONG_PRESS released (%ums)",
                          slot->key_id, slot->duration_ms);
            } else {
                /* 短按 → 进入双击等待 */
                slot->state        = KEY_STATE_WAIT_DOUBLE;
                slot->release_time = ev->time;
                LOG_DEBUG("key_parser", "Key %d released → WAIT_DOUBLE (%ums)",
                          slot->key_id, slot->duration_ms);
            }
            break;

        case KEY_STATE_PRESSED_2ND:
            /* 双击的第二次释放: 手势完成! */
            slot->duration_ms = (uint32_t)timeval_diff_ms(&ev->time,
                                                          &slot->press_time);
            event_enqueue(parser, slot->key_id,
                         3,  /* 双击 = 3 */
                         slot->duration_ms);
            slot->state = KEY_STATE_IDLE;
            LOG_DEBUG("key_parser", "Key %d DOUBLE_CLICK complete",
                      slot->key_id);
            return 1;  /* 产生了事件 */

        case KEY_STATE_LONG_PRESS:
            /* 长按释放: 状态机已完成上报, 回到 IDLE */
            slot->state = KEY_STATE_IDLE;
            LOG_DEBUG("key_parser", "Key %d released after long press",
                      slot->key_id);
            break;

        case KEY_STATE_WAIT_DOUBLE:
            /* 连续释放 (消抖忽略) */
            break;

        default:
            break;
        }
    }

    return 0;
}

int key_parser_tick(key_parser_t *parser)
{
    struct timeval now;
    int i;
    int events_generated = 0;

    if (!parser) return 0;

    get_now(&now);

    for (i = 0; i < parser->slot_count; i++) {
        key_slot_t *slot = &parser->slots[i];

        switch (slot->state) {

        case KEY_STATE_PRESSED:
            /* 检查是否达到长按阈值 */
            slot->duration_ms = (uint32_t)timeval_diff_ms(&now,
                                                          &slot->press_time);
            if (slot->duration_ms >= (uint32_t)parser->long_press_ms) {
                /* 长按! 进入 LONG_PRESS 状态 (等待释放后重置) */
                slot->state = KEY_STATE_LONG_PRESS;
                event_enqueue(parser, slot->key_id,
                             2,  /* 长按 = 2 */
                             slot->duration_ms);
                events_generated++;
                LOG_DEBUG("key_parser", "Key %d LONG_PRESS detected (%ums)",
                          slot->key_id, slot->duration_ms);
            }
            break;

        case KEY_STATE_WAIT_DOUBLE:
            /* 检查双击窗口是否过期 */
            slot->duration_ms = (uint32_t)timeval_diff_ms(&now,
                                                          &slot->release_time);
            if (slot->duration_ms >= (uint32_t)parser->double_click_ms) {
                /* 窗口过期 → 确认为短按 */
                event_enqueue(parser, slot->key_id,
                             1,  /* 短按 = 1 */
                             0); /* 短按不关心持续时间 */
                slot->state = KEY_STATE_IDLE;
                events_generated++;
                LOG_DEBUG("key_parser", "Key %d SHORT_PRESS confirmed (window expired)",
                          slot->key_id);
            }
            break;

        default:
            /* IDLE / LONG_PRESS / PRESSED_2ND: 不需要 tick 处理 */
            break;
        }
    }

    return events_generated;
}

int key_parser_get_event(key_parser_t *parser, key_event_t *out)
{
    key_event_t *ev;

    if (!parser || !out) return -1;
    if (parser->event_count == 0) return -1;  /* 队列空 */

    /* 从尾部取出 (FIFO) */
    ev = &parser->event_queue[parser->event_tail];
    memcpy(out, ev, sizeof(key_event_t));

    parser->event_tail = (parser->event_tail + 1) % 16;
    parser->event_count--;

    return 0;
}
