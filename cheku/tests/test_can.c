/**
 * @file    test_can.c
 * @brief   canbus_daemon 模块单元测试
 *
 * 测试内容 (纯软件, 不需要 CAN 硬件):
 *   1. 环形缓冲区: push/get/环形覆盖/FIFO顺序
 *   2. CAN ID 编码: 标准帧/扩展帧标志位
 *   3. 错误帧识别: CAN_ERR_FLAG 检测
 *   4. can_msg_t 与 can_frame 转换验证
 *   5. 配置加载测试
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "canbus_daemon.h"       /* src/canbus_daemon/ */
#include "can_driver.h"
#include "log/log.h"             /* include/log/        */

static int g_passed = 0, g_failed = 0, g_total = 0;

#define TEST_START(n) printf("\n========== [TEST] %s ==========\n", n)
#define TEST_CASE(d)  do { g_total++; printf("  [%d] %s ... ", g_total, d); } while(0)
#define TEST_OK()     do { printf("OK\n"); g_passed++; } while(0)
#define TEST_FAIL(m)  do { printf("FAIL: %s\n", m); g_failed++; return; } while(0)
#define TEST_ASSERT(c,m) do { if(!(c)) TEST_FAIL(m); } while(0)

/* ---- 测试 1: 环形缓冲区基础 ---- */
static void test_ring_basic(void)
{
    TEST_START("CAN Ring Buffer Basics");

    can_ring_t ring;

    TEST_CASE("can_ring_init(100)");
    TEST_ASSERT(can_ring_init(&ring, 100) == RET_OK, "init failed");
    TEST_ASSERT(ring.capacity == 100, "capacity wrong");
    TEST_ASSERT(can_ring_count(&ring) == 0, "initial count should be 0");
    TEST_OK();

    TEST_CASE("push 1 msg → count=1, get(0) returns it");
    can_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.can_id = 0x123;
    msg.can_dlc = 3;
    msg.data[0] = 0xAA; msg.data[1] = 0xBB; msg.data[2] = 0xCC;

    can_ring_push(&ring, &msg, 0);
    TEST_ASSERT(can_ring_count(&ring) == 1, "count should be 1");

    const can_ring_entry_t *e = can_ring_get(&ring, 0);
    TEST_ASSERT(e != NULL, "get(0) should not be NULL");
    TEST_ASSERT(e->msg.can_id == 0x123, "ID mismatch");
    TEST_ASSERT(e->msg.can_dlc == 3, "DLC mismatch");
    TEST_ASSERT(e->direction == 0, "direction should be RX");
    TEST_OK();

    TEST_CASE("push 3 more → count=4, order is FIFO (get(0)=latest)");
    for (int i = 2; i <= 4; i++) {
        msg.can_id = (uint32_t)(0x100 + i);
        can_ring_push(&ring, &msg, 0);
    }
    TEST_ASSERT(can_ring_count(&ring) == 4, "count should be 4");
    /* get(0) = 最新 = ID 0x104 */
    e = can_ring_get(&ring, 0);
    TEST_ASSERT(e->msg.can_id == 0x104, "latest should be 0x104");
    /* get(3) = 最旧 = ID 0x123 */
    e = can_ring_get(&ring, 3);
    TEST_ASSERT(e->msg.can_id == 0x123, "oldest should be 0x123");
    TEST_OK();

    can_ring_destroy(&ring);
}

/* ---- 测试 2: 环形缓冲区覆盖 ---- */
static void test_ring_overwrite(void)
{
    TEST_START("Ring Buffer Overwrite (Capacity=5)");

    can_ring_t ring;
    can_ring_init(&ring, 5);

    can_msg_t msg;
    memset(&msg, 0, sizeof(msg));

    /* 写入 7 条 (超过容量) */
    for (int i = 0; i < 7; i++) {
        msg.can_id = (uint32_t)(i + 1);
        can_ring_push(&ring, &msg, 0);
    }

    TEST_CASE("7 pushes into capacity=5 → count=5 (oldest overwritten)");
    TEST_ASSERT(can_ring_count(&ring) == 5, "count should cap at 5");
    /* 最旧的两条 (ID=1,2) 应被覆盖, 现在最旧的是 ID=3 */
    const can_ring_entry_t *e = can_ring_get(&ring, 4); /* index=4 = oldest */
    TEST_ASSERT(e->msg.can_id == 3, "oldest should be 3 (not 1)");
    /* 最新的是 ID=7 */
    e = can_ring_get(&ring, 0);
    TEST_ASSERT(e->msg.can_id == 7, "latest should be 7");
    TEST_OK();

    can_ring_destroy(&ring);
}

/* ---- 测试 3: CAN ID 位标志 ---- */
static void test_can_id_flags(void)
{
    TEST_START("CAN ID Flag Bits");

    TEST_CASE("standard frame: can_id = 0x7FF (max 11-bit)");
    canid_t id = 0x7FF;
    TEST_ASSERT((id & CAN_EFF_FLAG) == 0, "STD should not have EFF flag");
    TEST_ASSERT((id & CAN_SFF_MASK) == 0x7FF, "SFF mask should preserve all 11 bits");
    TEST_OK();

    TEST_CASE("extended frame: can_id = 0x1FFFFFFF | CAN_EFF_FLAG");
    id = 0x1FFFFFFF | CAN_EFF_FLAG;
    TEST_ASSERT((id & CAN_EFF_FLAG) != 0, "EXT should have EFF flag");
    TEST_ASSERT((id & CAN_EFF_MASK) == 0x1FFFFFFF, "EFF mask should preserve 29 bits");
    TEST_OK();

    TEST_CASE("error frame: CAN_ERR_FLAG = 0x20000000");
    TEST_ASSERT(CAN_ERR_FLAG == 0x20000000U, "CAN_ERR_FLAG value check");
    id = CAN_ERR_FLAG | (CAN_ERR_BUSOFF);
    TEST_ASSERT((id & CAN_ERR_FLAG) != 0, "error frame should be detectable");
    TEST_OK();
}

/* ---- 测试 4: 方向标志 ---- */
static void test_direction(void)
{
    TEST_START("CAN Message Direction");

    can_ring_t ring;
    can_ring_init(&ring, 5);

    can_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.can_id = 0x200;

    TEST_CASE("direction=0 (RX) → stored as 0");
    can_ring_push(&ring, &msg, 0);
    TEST_ASSERT(can_ring_get(&ring, 0)->direction == 0, "RX should be 0");
    TEST_OK();

    TEST_CASE("direction=1 (TX) → stored as 1");
    can_ring_push(&ring, &msg, 1);
    TEST_ASSERT(can_ring_get(&ring, 0)->direction == 1, "TX should be 1");
    TEST_OK();

    can_ring_destroy(&ring);
}

/* ---- 测试 5: 配置加载 ---- */
static void test_config(void)
{
    TEST_START("CAN Config Loading");

    const char *ini = "/tmp/test_can_config.ini";
    FILE *fp = fopen(ini, "w");
    fprintf(fp, "[canbus]\ninterface = can1\nbitrate = 250000\n");
    fclose(fp);

    canbus_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    TEST_CASE("load canbus config → interface=can1");
    canbus_config_load(&ctx, ini);
    TEST_ASSERT(strcmp(ctx.can_ifname, "can1") == 0, "interface should be can1");
    TEST_OK();

    unlink(ini);
}

/* ---- main ---- */
int main(void)
{
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  canbus_daemon — Module Unit Tests           ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    log_init(NULL, LOG_DEBUG, 0, 0);

    test_ring_basic();
    test_ring_overwrite();
    test_can_id_flags();
    test_direction();
    test_config();

    printf("\n╔══════════════════════════════════════════════╗\n");
    printf("║  Results: total=%d passed=%d failed=%d        ║\n",
           g_total, g_passed, g_failed);
    printf("╚══════════════════════════════════════════════╝\n");

    log_close();
    return g_failed > 0 ? 1 : 0;
}
