/**
 * @file    test_input.c
 * @brief   input_daemon 模块单元测试
 *
 * 测试内容:
 *   1. 按键映射查找
 *   2. 手势状态机: 短按 / 长按 / 双击
 *   3. 消抖过滤
 *   4. 事件队列 FIFO
 *   5. 默认配置加载
 *
 * 本测试纯软件验证, 不需要真实按键硬件。
 * 模拟 input_event 序列注入 key_parser。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/time.h>

#include "input_daemon.h"        /* src/input_daemon/ */
#include "key_parser.h"
#include "log/log.h"             /* include/log/      */

static int g_passed = 0, g_failed = 0, g_total = 0;

#define TEST_START(n)  printf("\n========== [TEST] %s ==========\n", n)
#define TEST_CASE(d)   do { g_total++; printf("  [%d] %s ... ", g_total, d); } while(0)
#define TEST_OK()      do { printf("OK\n"); g_passed++; } while(0)
#define TEST_FAIL(m)   do { printf("FAIL: %s\n", m); g_failed++; return; } while(0)
#define TEST_ASSERT(c,m) do { if(!(c)) TEST_FAIL(m); } while(0)

/* 模拟 input_event 发送按键按下 */
static struct input_event make_press(int code, int sec, int usec) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = EV_KEY;
    ev.code  = code;
    ev.value = 1;
    ev.time.tv_sec  = sec;
    ev.time.tv_usec = usec;
    return ev;
}

/* 模拟 input_event 发送按键释放 */
static struct input_event make_release(int code, int sec, int usec) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = EV_KEY;
    ev.code  = code;
    ev.value = 0;
    ev.time.tv_sec  = sec;
    ev.time.tv_usec = usec;
    return ev;
}

/* 按键映射表 */
static key_mapping_t test_map[] = {
    { KEY_F1, KEY_ID_KEY1, "KEY1" },
    { KEY_F2, KEY_ID_KEY2, "KEY2" },
};

/* ---- 测试 1: 按键映射 ---- */
static void test_mapping(void)
{
    TEST_START("Key Mapping Lookup");

    key_parser_t parser;
    key_parser_init(&parser, test_map, 2, 2000, 500, 30);

    TEST_CASE("lookup KEY_F1 → KEY1");
    TEST_ASSERT(key_parser_lookup(&parser, KEY_F1) == KEY_ID_KEY1,
                "KEY_F1 should map to KEY1");
    TEST_OK();

    TEST_CASE("lookup KEY_F2 → KEY2");
    TEST_ASSERT(key_parser_lookup(&parser, KEY_F2) == KEY_ID_KEY2,
                "KEY_F2 should map to KEY2");
    TEST_OK();

    TEST_CASE("lookup unmapped KEY_ENTER → NONE");
    TEST_ASSERT(key_parser_lookup(&parser, KEY_ENTER) == KEY_ID_NONE,
                "unmapped key should return NONE");
    TEST_OK();
}

/* ---- 测试 2: 短按手势 ---- */
static void test_short_press(void)
{
    TEST_START("Short Press Gesture");

    key_parser_t parser;
    key_parser_init(&parser, test_map, 2, 2000, 500, 30);

    struct input_event ev;
    key_event_t kev;

    /* 模拟: KEY_F1 按下 100ms 后释放 (短按) */
    /* t=0s: 按下 */
    ev = make_press(KEY_F1, 0, 0);
    key_parser_feed(&parser, &ev);

    /* t=0.1s: 释放 (100ms < 2000ms长按, 也 > 30ms消抖) */
    ev = make_release(KEY_F1, 0, 100000);
    key_parser_feed(&parser, &ev);

    /* 此时进入 WAIT_DOUBLE 状态, 需要 tick 推进时间窗口 */
    TEST_CASE("SHORT: press+release → enters WAIT_DOUBLE");
    /* 验证事件尚未产生 (还在等双击窗口) */
    TEST_ASSERT(key_parser_get_event(&parser, &kev) == -1,
                "should not have event yet (waiting for double-click window)");
    TEST_OK();

    /* tick 推进时间 > 500ms (双击窗口过期) */
    TEST_CASE("SHORT: tick after 600ms → SHORT_PRESS confirmed");
    /* 模拟时间流逝: tick 检查时间差
     * 注意: tick 使用 gettimeofday() 获取当前真实时间,
     * 我们需要等待实际时间流逝。用 usleep 模拟。 */
    usleep(600000);  /* 等待 600ms 真实时间 */
    key_parser_tick(&parser);

    /* 现在应该能取出短按事件 */
    int ret = key_parser_get_event(&parser, &kev);
    TEST_ASSERT(ret == 0, "should have event after tick");
    TEST_ASSERT(kev.key_id == KEY_ID_KEY1, "should be KEY1");
    TEST_ASSERT(kev.event_type == 1, "should be SHORT_PRESS (1)");
    printf("(key=%d type=%d) ", kev.key_id, kev.event_type);
    TEST_OK();
}

/* ---- 测试 3: 长按手势 ---- */
static void test_long_press(void)
{
    TEST_START("Long Press Gesture");

    key_parser_t parser;
    key_parser_init(&parser, test_map, 2, 2000, 500, 30);

    struct input_event ev;
    key_event_t kev;

    /* 模拟: KEY_F2 持续按下 */
    /* t=0: 按下 */
    ev = make_press(KEY_F2, 0, 0);
    key_parser_feed(&parser, &ev);

    /* tick 检查前: 不应有事件 */
    TEST_CASE("LONG: press → no event immediately");
    TEST_ASSERT(key_parser_get_event(&parser, &kev) == -1,
                "should not have event (not long enough)");
    TEST_OK();

    /* 等待 2100ms (超过 2000ms 长按阈值) */
    TEST_CASE("LONG: tick after 2100ms → LONG_PRESS detected");
    usleep(2100000);  /* 等待 2.1s */
    key_parser_tick(&parser);

    /* 应取出长按事件 */
    int ret = key_parser_get_event(&parser, &kev);
    TEST_ASSERT(ret == 0, "should have long press event");
    TEST_ASSERT(kev.key_id == KEY_ID_KEY2, "should be KEY2");
    TEST_ASSERT(kev.event_type == 2, "should be LONG_PRESS (2)");
    printf("(key=%d type=%d dur=%ums) ", kev.key_id, kev.event_type, kev.duration_ms);
    TEST_OK();

    /* t=2.1s: 释放 */
    ev = make_release(KEY_F2, 2, 100000);
    key_parser_feed(&parser, &ev);
}

/* ---- 测试 4: 消抖 ---- */
static void test_debounce(void)
{
    TEST_START("Software Debounce");

    key_parser_t parser;
    /* 消抖阈值设为 50ms */
    key_parser_init(&parser, test_map, 2, 2000, 500, 50);

    struct input_event ev;
    key_event_t kev;

    /* 模拟干扰: 10ms 极短按下+释放 */
    ev = make_press(KEY_F1, 0, 0);
    key_parser_feed(&parser, &ev);

    /* 10ms 后释放 → < debounce_ms(50), 应被过滤 */
    ev = make_release(KEY_F1, 0, 10000);  /* 10ms */
    key_parser_feed(&parser, &ev);

    TEST_CASE("DEBOUNCE: 10ms press → filtered (returned to IDLE)");
    /* tick 推进并确认没有事件产生 */
    usleep(600000);  /* 等待 600ms */
    key_parser_tick(&parser);
    int ret = key_parser_get_event(&parser, &kev);
    TEST_ASSERT(ret == -1, "debounce should filter 10ms press");
    TEST_OK();
}

/* ---- 测试 5: 事件队列 ---- */
static void test_event_queue(void)
{
    TEST_START("Event Queue (FIFO)");

    key_parser_t parser;
    key_parser_init(&parser, test_map, 2, 2000, 500, 30);

    struct input_event ev;
    key_event_t kev;

    /* 快速产生两个短按 */
    /* 第1次: KEY_F1 短按 */
    ev = make_press(KEY_F1, 0, 0);
    key_parser_feed(&parser, &ev);
    ev = make_release(KEY_F1, 0, 100000);
    key_parser_feed(&parser, &ev);
    usleep(600000);
    key_parser_tick(&parser);

    /* 第2次: KEY_F2 短按 */
    ev = make_press(KEY_F2, 10, 0);
    key_parser_feed(&parser, &ev);
    ev = make_release(KEY_F2, 10, 100000);
    key_parser_feed(&parser, &ev);
    usleep(600000);
    key_parser_tick(&parser);

    TEST_CASE("QUEUE: two events → FIFO order");
    /* 取出第1个 */
    int ret = key_parser_get_event(&parser, &kev);
    TEST_ASSERT(ret == 0, "should get 1st event");
    TEST_ASSERT(kev.key_id == KEY_ID_KEY1, "1st should be KEY1");
    TEST_ASSERT(kev.event_type == 1, "1st should be SHORT");

    /* 取出第2个 */
    ret = key_parser_get_event(&parser, &kev);
    TEST_ASSERT(ret == 0, "should get 2nd event");
    TEST_ASSERT(kev.key_id == KEY_ID_KEY2, "2nd should be KEY2");
    TEST_ASSERT(kev.event_type == 1, "2nd should be SHORT");

    /* 队列应为空 */
    ret = key_parser_get_event(&parser, &kev);
    TEST_ASSERT(ret == -1, "queue should be empty after two pops");
    printf("(FIFO order confirmed) ");
    TEST_OK();
}

/* ---- 测试 6: 未映射按键忽略 ---- */
static void test_unmapped_key(void)
{
    TEST_START("Unmapped Key Ignored");

    key_parser_t parser;
    key_parser_init(&parser, test_map, 2, 2000, 500, 30);

    struct input_event ev;
    key_event_t kev;

    /* 按下未映射的 KEY_ENTER */
    ev = make_press(KEY_ENTER, 0, 0);
    key_parser_feed(&parser, &ev);
    ev = make_release(KEY_ENTER, 0, 200000);  /* 200ms */
    key_parser_feed(&parser, &ev);

    usleep(600000);
    key_parser_tick(&parser);

    TEST_CASE("UNMAPPED: KEY_ENTER → no event");
    int ret = key_parser_get_event(&parser, &kev);
    TEST_ASSERT(ret == -1, "unmapped key should not produce event");
    TEST_OK();
}

/* ---- main ---- */
int main(void)
{
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  input_daemon — Module Unit Tests            ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    log_init(NULL, LOG_DEBUG, 0, 0);

    test_mapping();
    test_short_press();
    test_long_press();
    test_debounce();
    test_event_queue();
    test_unmapped_key();

    printf("\n╔══════════════════════════════════════════════╗\n");
    printf("║  Results: total=%d passed=%d failed=%d        ║\n",
           g_total, g_passed, g_failed);
    printf("╚══════════════════════════════════════════════╝\n");

    log_close();
    return g_failed > 0 ? 1 : 0;
}
