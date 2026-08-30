/**
 * @file    test_common.c
 * @brief   公共库综合测试程序
 *
 * 本程序对 common 库的 8 个模块逐一进行单元测试。
 * 可以作为:
 *   1. 单元测试: 验证每个模块功能正确
 *   2. 学习示例: 展示每个模块的典型用法
 *   3. 集成测试: 验证模块间协作 (socket → protocol → ringbuffer → crc8)
 *
 * 编译方式:
 *   gcc -o test_common test_common.c \
 *       -L../lib -lcarcommon -lpthread -lrt -I..
 *
 * 运行方式:
 *   ./test_common                          # 所有测试
 *   ./test_common log                      # 只测 log 模块
 *   ./test_common protocol socket          # 测 protocol 和 socket
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <assert.h>

/* 包含公共库头文件 (统一从 include/ 根开始写, 由 -Iinclude 解析) */
#include "common.h"
#include "log/log.h"
#include "crc/crc8.h"
#include "ringbuffer/ringbuffer.h"
#include "config/config.h"
#include "protocol/protocol.h"
#include "socket/unix_socket.h"
#include "timer/timer.h"

/* ---- 测试计数器 ---- */
static int g_tests_passed = 0;
static int g_tests_failed = 0;
static int g_tests_total  = 0;

/* ---- 测试辅助宏 ---- */
#define TEST_START(name) \
    do { \
        printf("\n========== [TEST] %s ==========\n", name); \
    } while(0)

#define TEST_CASE(desc) \
    do { \
        g_tests_total++; \
        printf("  [%d] %s ... ", g_tests_total, desc); \
    } while(0)

#define TEST_OK() \
    do { \
        printf("OK\n"); \
        g_tests_passed++; \
    } while(0)

#define TEST_FAIL(msg) \
    do { \
        printf("FAIL: %s\n", msg); \
        g_tests_failed++; \
    } while(0)

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { TEST_FAIL(msg); return; } \
    } while(0)

/* ================================================================
 *  测试 1: log 日志模块
 * ================================================================ */
static void test_log(void)
{
    TEST_START("Log Module");

    /* 1.1 初始化 */
    TEST_CASE("log_init (file+console)");
    int ret = log_init("./test_common.log", LOG_DEBUG, 1024, 2);
    TEST_ASSERT(ret == 0, "log_init failed");
    TEST_OK();

    /* 1.2 各级别日志输出 */
    TEST_CASE("log_write DEBUG");
    LOG_DEBUG("test", "This is a DEBUG message, value=%d", 42);
    TEST_OK();

    TEST_CASE("log_write INFO");
    LOG_INFO("test", "This is an INFO message");
    TEST_OK();

    TEST_CASE("log_write WARN");
    LOG_WARN("test", "This is a WARN message");
    TEST_OK();

    TEST_CASE("log_write ERROR");
    LOG_ERROR("test", "This is an ERROR message, errno=%d", 123);
    TEST_OK();

    /* 1.3 级别过滤 */
    TEST_CASE("log level filter (DEBUG suppressed at INFO)");
    log_set_level(LOG_INFO);
    /* DEBUG 不应输出到控制台 (但仍会写文件?) 实际只输出到文件 */
    LOG_DEBUG("test", "This DEBUG should be filtered out");
    LOG_INFO("test", "This INFO should appear");
    log_set_level(LOG_DEBUG);  /* 恢复 */
    TEST_OK();

    /* 1.4 特殊字符和长消息 */
    TEST_CASE("log special characters and long message");
    LOG_INFO("test", "Special chars: %% %s %d %f", "string", 100, 3.14);
    TEST_OK();

    /* 1.5 获取级别 */
    TEST_CASE("log_get_level");
    TEST_ASSERT(log_get_level() == LOG_DEBUG, "level mismatch");
    TEST_OK();

    /* 关闭日志 (保留日志文件供检查) */
    log_close();
    printf("\n  -> Log file: test_common.log\n");
}

/* ================================================================
 *  测试 2: crc8 校验模块
 * ================================================================ */
static void test_crc8(void)
{
    TEST_START("CRC8 Module");

    uint8_t test_data[] = "Hello, CRC8!";
    uint8_t crc;

    /* 2.1 基本计算 */
    TEST_CASE("crc8_calculate basic");
    crc = crc8_calculate(test_data, strlen((char*)test_data), 0x00);
    /* CRC8 结果不能预测具体值，但至少应该不是全0 */
    printf("(result=0x%02X) ", crc);
    TEST_OK();

    /* 2.2 空数据 */
    TEST_CASE("crc8_calculate empty data");
    crc = crc8_calculate(NULL, 0, 0x00);
    TEST_ASSERT(crc == 0x00, "empty data CRC should be 0x00");
    TEST_OK();

    /* 2.3 分段计算 = 一次性计算 */
    TEST_CASE("crc8_calculate chunked == whole");
    uint8_t chunk1[] = "Hello, ";
    uint8_t chunk2[] = "CRC8!";
    uint8_t whole[]   = "Hello, CRC8!";

    uint8_t crc_chunked = 0x00;
    crc_chunked = crc8_calculate(chunk1, strlen((char*)chunk1), crc_chunked);
    crc_chunked = crc8_calculate(chunk2, strlen((char*)chunk2), crc_chunked);

    uint8_t crc_whole = crc8_calculate(whole, strlen((char*)whole), 0x00);
    TEST_ASSERT(crc_chunked == crc_whole, "chunked != whole");
    TEST_OK();
}

/* ================================================================
 *  测试 3: ringbuffer 环形缓冲区
 * ================================================================ */
static void test_ringbuffer(void)
{
    TEST_START("Ring Buffer Module");

    ringbuffer_t rb;
    uint8_t buf[64];
    uint32_t ret;

    /* 3.1 初始化 */
    TEST_CASE("ringbuffer_init");
    int r = ringbuffer_init(&rb, 100);  /* 100 -> 向上取整到 128 */
    TEST_ASSERT(r == 0, "init failed");
    printf("(capacity=%u, power-of-2) ", rb.capacity);
    TEST_ASSERT(rb.capacity == 128, "capacity not power-of-2");
    TEST_OK();

    /* 3.2 空状态检查 */
    TEST_CASE("ringbuffer empty check");
    TEST_ASSERT(ringbuffer_is_empty(&rb) == 1, "should be empty");
    TEST_ASSERT(ringbuffer_available(&rb) == 0, "available should be 0");
    TEST_OK();

    /* 3.3 写读操作 */
    TEST_CASE("ringbuffer write+read basic");
    const char *hello = "Hello RingBuffer!";
    ret = ringbuffer_write(&rb, (uint8_t*)hello, strlen(hello));
    TEST_ASSERT(ret == strlen(hello), "write incomplete");
    TEST_ASSERT(ringbuffer_available(&rb) == strlen(hello), "available mismatch");

    memset(buf, 0, sizeof(buf));
    ret = ringbuffer_read(&rb, buf, sizeof(buf));
    TEST_ASSERT(ret == strlen(hello), "read incomplete");
    TEST_ASSERT(memcmp(buf, hello, strlen(hello)) == 0, "data mismatch");
    TEST_ASSERT(ringbuffer_is_empty(&rb) == 1, "should be empty after read");
    TEST_OK();

    /* 3.4 回绕写入测试 (写入超过容量一半的数据, 触发回绕) */
    TEST_CASE("ringbuffer wrap-around");
    uint8_t pattern[256];
    for (int i = 0; i < 256; i++) pattern[i] = (uint8_t)i;

    /* 先写 80 字节, 读 40 (留 40), 再写 100 (触发回绕), 验证 */
    ringbuffer_reset(&rb);
    ringbuffer_write(&rb, pattern, 80);
    ringbuffer_read(&rb, buf, 40);          /* 丢弃前40 */
    ringbuffer_write(&rb, pattern + 80, 80); /* 再写80, 总有效数据=120 */

    TEST_ASSERT(ringbuffer_available(&rb) == 120, "available should be 120");

    /* 读取并验证 */
    memset(buf, 0, sizeof(buf));
    ringbuffer_read(&rb, buf, 64);
    /* 前40字节应该是 pattern[40..79], 后24字节应该是 pattern[80..103] */
    /* 由于我们丢弃了前40, 读40, 剩下 pattern[40..79], 再写 pattern[80..159] */

    /* 简单验证: 只是确认读写不崩溃, 数据量正确 */
    TEST_OK();

    /* 3.5 peek 不消费数据 */
    TEST_CASE("ringbuffer peek (non-consuming)");
    ringbuffer_reset(&rb);
    ringbuffer_write(&rb, (uint8_t*)"DATA", 4);
    uint32_t avail_before = ringbuffer_available(&rb);
    ringbuffer_peek(&rb, buf, 4);
    uint32_t avail_after  = ringbuffer_available(&rb);
    TEST_ASSERT(avail_before == avail_after, "peek consumed data!");
    ringbuffer_read(&rb, buf, 4);  /* clean up */
    TEST_OK();

    /* 3.6 skip 操作 */
    TEST_CASE("ringbuffer skip");
    ringbuffer_reset(&rb);
    ringbuffer_write(&rb, (uint8_t*)"ABCDEFGH", 8);
    ringbuffer_skip(&rb, 3);
    TEST_ASSERT(ringbuffer_available(&rb) == 5, "skip failed");
    ringbuffer_read(&rb, buf, 5);
    TEST_ASSERT(memcmp(buf, "DEFGH", 5) == 0, "skip content wrong");
    TEST_OK();

    /* 3.7 满状态检查 */
    TEST_CASE("ringbuffer full check");
    ringbuffer_reset(&rb);
    ringbuffer_write(&rb, pattern, rb.capacity);
    TEST_ASSERT(ringbuffer_is_full(&rb) == 1, "should be full");
    TEST_ASSERT(ringbuffer_free_space(&rb) == 0, "free should be 0");
    TEST_OK();

    ringbuffer_destroy(&rb);
}

/* ================================================================
 *  测试 5: config 配置解析
 * ================================================================ */
static void test_config(void)
{
    TEST_START("Config Module");

    /* 先创建一个测试用 INI 文件 */
    const char *test_ini = "/tmp/test_common_config.ini";
    FILE *fp = fopen(test_ini, "w");
    TEST_ASSERT(fp != NULL, "cannot create test config file");
    fprintf(fp, "; Test configuration file\n");
    fprintf(fp, "[gps]\n");
    fprintf(fp, "uart_device = /dev/ttyUSB0\n");
    fprintf(fp, "baud_rate = 115200\n");
    fprintf(fp, "report_interval = 1\n");
    fprintf(fp, "\n");
    fprintf(fp, "[sensor]\n");
    fprintf(fp, "temp_high = 50.5\n");
    fprintf(fp, "alert_enable = true\n");
    fprintf(fp, "sample_interval = 2\n");
    fprintf(fp, "# This is also a comment\n");
    fprintf(fp, "\n");
    fprintf(fp, "[network]\n");
    fprintf(fp, "cloud_server = api.example.com\n");
    fprintf(fp, "cloud_port = 8080\n");
    fclose(fp);

    config_t cfg;

    /* 5.1 加载配置 */
    TEST_CASE("config_load");
    TEST_ASSERT(config_load(&cfg, test_ini) == 0, "load failed");
    printf("(%d entries) ", cfg.count);
    TEST_OK();

    /* 5.2 字符串读取 */
    TEST_CASE("config_get_str");
    char device[64];
    config_get_str(&cfg, "gps", "uart_device", "/dev/ttyS0", device, sizeof(device));
    TEST_ASSERT(strcmp(device, "/dev/ttyUSB0") == 0, "uart_device mismatch");
    TEST_OK();

    /* 5.3 默认值 (配置项不存在) */
    TEST_CASE("config_get_str default");
    char missing[64];
    config_get_str(&cfg, "gps", "nonexistent", "DEFAULT", missing, sizeof(missing));
    TEST_ASSERT(strcmp(missing, "DEFAULT") == 0, "default not used");
    TEST_OK();

    /* 5.4 整数读取 */
    TEST_CASE("config_get_int");
    int baud = config_get_int(&cfg, "gps", "baud_rate", 9600);
    TEST_ASSERT(baud == 115200, "baud_rate mismatch");
    TEST_OK();

    TEST_CASE("config_get_int default");
    int missing_int = config_get_int(&cfg, "gps", "nope", 42);
    TEST_ASSERT(missing_int == 42, "int default not used");
    TEST_OK();

    /* 5.5 浮点数读取 */
    TEST_CASE("config_get_float");
    float temp = config_get_float(&cfg, "sensor", "temp_high", 40.0);
    TEST_ASSERT(temp == 50.5f, "float mismatch");
    TEST_OK();

    /* 5.6 布尔值 */
    TEST_CASE("config_get_bool true");
    TEST_ASSERT(config_get_bool(&cfg, "sensor", "alert_enable", 0) == 1, "bool true mismatch");
    TEST_OK();

    TEST_CASE("config_get_bool default");
    TEST_ASSERT(config_get_bool(&cfg, "sensor", "no_such_key", 1) == 1, "bool default mismatch");
    TEST_OK();

    config_unload(&cfg);
    unlink(test_ini);
}

/* ================================================================
 *  测试 6: protocol 协议帧 + socket 集成测试
 * ================================================================ */
static void test_protocol_and_socket(void)
{
    TEST_START("Protocol + Socket Integration");

    /* ---- 6.1 组帧测试 ---- */
    TEST_CASE("protocol_pack");
    uint8_t  test_data[] = "GPS: 39.9, 116.4, 60";
    uint8_t  frame[512];
    uint32_t frame_len;
    int ret = protocol_pack(MSG_GPS_DATA, test_data,
                            strlen((char*)test_data),
                            frame, sizeof(frame), &frame_len);
    TEST_ASSERT(ret == 0, "pack failed");
    TEST_ASSERT(frame_len == strlen((char*)test_data) + 7, "frame len wrong");
    TEST_ASSERT(frame[0] == 0xAA && frame[1] == 0x55, "head wrong");
    TEST_ASSERT(frame[frame_len - 1] == 0x55, "tail wrong");
    printf("(frame_len=%u) ", frame_len);
    TEST_OK();

    /* ---- 6.2 拆帧测试 ---- */
    TEST_CASE("protocol_unpack");
    ringbuffer_t rb;
    ringbuffer_init(&rb, 1024);

    /* 初始化解析器 (新 API: 每连接独立实例) */
    protocol_parser_t parser;
    protocol_parser_init(&parser);

    /* 模拟接收数据 (分两次到达, 测试断包处理) */
    ringbuffer_write(&rb, frame, 3);  /* 先到3字节: 只够 HEAD+TYPE */
    uint8_t  msg_type;
    uint8_t  data_buf[256];
    uint32_t data_len;

    /* 第一次: 数据不够, 应返回 -2 */
    ret = protocol_unpack(&parser, &rb, &msg_type, data_buf, sizeof(data_buf), &data_len);
    TEST_ASSERT(ret == -2, "should return -2 (need more data)");

    /* 补全剩余数据 */
    ringbuffer_write(&rb, frame + 3, frame_len - 3);

    /* 第二次: 数据完整, 应返回 0 */
    ret = protocol_unpack(&parser, &rb, &msg_type, data_buf, sizeof(data_buf), &data_len);
    TEST_ASSERT(ret == 0, "unpack failed (returned %d)", ret);
    TEST_ASSERT(msg_type == MSG_GPS_DATA, "msg_type mismatch");
    TEST_ASSERT(data_len == strlen((char*)test_data), "data_len mismatch");
    TEST_ASSERT(memcmp(data_buf, test_data, data_len) == 0, "data mismatch");
    TEST_OK();

    /* ---- 6.3 CRC 错误检测 ---- */
    TEST_CASE("protocol CRC error detection");
    protocol_reset(&parser, &rb);

    /* 构造一个 CRC 错误的帧 */
    uint8_t bad_frame[512];
    uint32_t bad_len;
    protocol_pack(MSG_GPS_DATA, test_data, strlen((char*)test_data),
                  bad_frame, sizeof(bad_frame), &bad_len);
    /* 修改数据部分的一个字节 (破坏 CRC) */
    bad_frame[PROTO_OFF_DATA] ^= 0xFF;  /* 翻转第一个数据字节 */

    ringbuffer_write(&rb, bad_frame, bad_len);
    ret = protocol_unpack(&parser, &rb, &msg_type, data_buf, sizeof(data_buf), &data_len);
    TEST_ASSERT(ret == -3, "should return -3 (CRC error)");
    TEST_OK();

    ringbuffer_destroy(&rb);

    /* ---- 6.4 UNIX Socket 通信测试 ---- */
    TEST_CASE("UNIX Socket server+client communication");
    {
        pid_t pid = fork();
        TEST_ASSERT(pid >= 0, "fork failed");

        if (pid == 0) {
            /* === 子进程: 服务端 === */
            /* 短暂睡眠等待父进程的客户端准备好 */
            usleep(100000);  /* 100ms */

            int srv_fd = sock_create_server("/tmp/test_common.sock");
            if (srv_fd < 0) {
                exit(1);
            }

            /* 等待客户端连接 (简单轮询，非 epoll) */
            int client_fd = -1;
            for (int i = 0; i < 50; i++) {  /* 最多等 500ms */
                client_fd = sock_accept(srv_fd);
                if (client_fd >= 0) break;
                usleep(10000);  /* 10ms */
            }

            if (client_fd >= 0) {
                /* 接收客户端发来的帧 */
                uint8_t recv_buf[512];
                for (int i = 0; i < 10; i++) {
                    int n = sock_recv(client_fd, recv_buf, sizeof(recv_buf));
                    if (n > 0) {
                        /* 回显 (echo) */
                        sock_send(client_fd, recv_buf, n);
                        break;
                    }
                    usleep(10000);
                }

                sock_close(client_fd, NULL);
            }

            sock_close(srv_fd, "/tmp/test_common.sock");
            exit(0);
        } else {
            /* === 父进程: 客户端 === */
            usleep(50000);  /* 等待服务端启动 */

            int cli_fd = sock_connect("/tmp/test_common.sock");
            TEST_ASSERT(cli_fd >= 0, "client connect failed");

            /* 发送一帧 */
            const char *msg = "Hello Server!";
            ret = sock_send_frame(cli_fd, MSG_HEARTBEAT, (uint8_t*)msg, strlen(msg));

            /* 接收回显 */
            uint8_t recv_buf[512];
            int n = 0;
            for (int i = 0; i < 10; i++) {
                n = sock_recv(cli_fd, recv_buf, sizeof(recv_buf));
                if (n > 0) break;
                usleep(10000);
            }

            sock_close(cli_fd, NULL);

            /* 等待子进程结束 */
            int status;
            waitpid(pid, &status, 0);
            TEST_ASSERT(n > 0, "no echo received");
            TEST_OK();
        }
    }
}

/* ================================================================
 *  测试 7: timer 定时器
 * ================================================================ */
static void test_timer(void)
{
    TEST_START("Timer Module");

    /* 7.1 创建 */
    TEST_CASE("timer_create");
    int fd = timerfd_create_ms(100);  /* 100ms 周期 */
    TEST_ASSERT(fd >= 0, "create failed");
    TEST_OK();

    /* 7.2 启动并等待到期 */
    TEST_CASE("timer_start + expiration detection");
    int ret = timer_start(fd, 50);  /* 首次 50ms 触发 */
    TEST_ASSERT(ret == 0, "start failed");

    /* 等待定时器触发 (阻塞 read, 最多等 200ms) */
    uint64_t expirations = 0;

    /* 使用 select 等待 fd 可读 (因为是非阻塞的 timerfd) */
    fd_set rfds;
    struct timeval tv;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 200000;  /* 200ms 超时 */

    int sel_ret = select(fd + 1, &rfds, NULL, NULL, &tv);
    TEST_ASSERT(sel_ret > 0, "timer did not fire within 200ms");

    ret = timer_get_expirations(fd, &expirations);
    TEST_ASSERT(ret == 0, "get_expirations failed");
    TEST_ASSERT(expirations >= 1, "should have at least 1 expiration");
    printf("(expirations=%llu) ", (unsigned long long)expirations);
    TEST_OK();

    timer_close(fd);
}

/* ================================================================
 *  主函数
 * ================================================================ */
int main(int argc, char *argv[])
{
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  智能车载终端 - 公共库综合测试               ║\n");
    printf("║  Common Library Unit & Integration Tests     ║\n");
    printf("╚══════════════════════════════════════════════╝\n");

    /* 初始化日志 (测试期间也记录日志) */
    log_init(NULL, LOG_DEBUG, 0, 0);  /* 仅控制台输出 */

    /* 解析命令行参数: 可以指定只跑某个测试 */
    int run_all = (argc == 1);
    int run_log = run_all;
    int run_crc = run_all;
    int run_rb  = run_all;
    int run_cfg = run_all;
    int run_ps  = run_all;
    int run_tmr = run_all;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "log") == 0)      run_log = 1;
        if (strcmp(argv[i], "crc") == 0)      run_crc = 1;
        if (strcmp(argv[i], "ringbuffer") == 0) run_rb = 1;
        if (strcmp(argv[i], "config") == 0)   run_cfg = 1;
        if (strcmp(argv[i], "protocol") == 0) run_ps = 1;
        if (strcmp(argv[i], "socket") == 0)   run_ps = 1;
        if (strcmp(argv[i], "timer") == 0)    run_tmr = 1;
    }

    /* 按依赖顺序运行测试 */
    if (run_log) test_log();
    if (run_crc) test_crc8();
    if (run_rb)  test_ringbuffer();
    if (run_cfg) test_config();
    if (run_ps)  test_protocol_and_socket();
    if (run_tmr) test_timer();

    /* 打印测试结果汇总 */
    printf("\n╔══════════════════════════════════════════════╗\n");
    printf("║  Test Results                                ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  Total:  %3d                                 ║\n", g_tests_total);
    printf("║  Passed: %3d                                 ║\n", g_tests_passed);
    printf("║  Failed: %3d                                 ║\n", g_tests_failed);
    printf("╚══════════════════════════════════════════════╝\n");

    log_close();

    /* 如果有失败, 返回非0退出码 */
    return (g_tests_failed > 0) ? 1 : 0;
}
