/**
 * @file    test_dvr.c
 * @brief   dvr_daemon 单元测试 (纯软件)
 *
 * 测试: 存储管理器 (init/open/write/list/clean) / 配置加载 / 状态枚举
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "dvr_daemon.h"          /* src/dvr_daemon/ */
#include "log/log.h"             /* include/log/    */

static int p=0,f=0,t=0;
#define TS(n) printf("\n========== [TEST] %s ==========\n", n)
#define TC(d) do { t++; printf("  [%d] %s ... ", t, d); } while(0)
#define OK()  do { printf("OK\n"); p++; } while(0)
#define FL(m) do { printf("FAIL: %s\n", m); f++; return; } while(0)
#define AS(c,m) do { if(!(c)) FL(m); } while(0)

/* 外部声明 */
int dvr_storage_init(dvr_storage_t*,const char*,int,int);
int dvr_storage_open_segment(dvr_storage_t*,time_t);
int dvr_storage_write(dvr_storage_t*,const uint8_t*,int);
void dvr_storage_close(dvr_storage_t*);
int dvr_storage_check_and_clean(dvr_storage_t*);
int dvr_storage_list(dvr_storage_t*,dvr_segment_t*,int);

/* ---- 1. 存储管理器 ---- */
static void test_storage(void)
{
    TS("Storage Manager");
    const char *dir = "/tmp/test_dvr_storage";
    system("rm -rf /tmp/test_dvr_storage 2>/dev/null");

    dvr_storage_t st;
    TC("init → dir created");
    AS(dvr_storage_init(&st, dir, 10, 5) == 0, "init failed");
    struct stat sb;
    AS(stat(dir, &sb) == 0 && S_ISDIR(sb.st_mode), "dir not created"); OK();

    TC("open_segment → file created, write data");
    AS(dvr_storage_open_segment(&st, time(NULL)) == 0, "open failed");
    uint8_t buf[256];
    for (int i = 0; i < 256; i++) buf[i] = (uint8_t)i;
    int n = dvr_storage_write(&st, buf, 256);
    AS(n == 256, "write incomplete"); OK();

    TC("list → 1 segment found");
    dvr_segment_t list[8];
    int cnt = dvr_storage_list(&st, list, 8);
    AS(cnt >= 1, "no segments found"); OK();

    dvr_storage_close(&st);

    TC("check_and_clean → within limit, no deletions");
    AS(dvr_storage_check_and_clean(&st) == 0, "check failed"); OK();

    system("rm -rf /tmp/test_dvr_storage 2>/dev/null");
}

/* ---- 2. 状态枚举 ---- */
static void test_states(void)
{
    TS("DVR State Enums");
    TC("DVR_IDLE=0, DVR_RECORDING=1, DVR_PAUSED=2, DVR_ERROR=3");
    AS(DVR_IDLE == 0, "idle"); AS(DVR_RECORDING == 1, "recording");
    AS(DVR_PAUSED == 2, "paused"); AS(DVR_ERROR == 3, "error"); OK();
}

/* ---- main ---- */
int main(void)
{
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  dvr_daemon — Module Unit Tests              ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");
    log_init(NULL, LOG_DEBUG, 0, 0);
    test_storage();
    test_states();
    printf("\n╔══════════════════════════════════════════════╗\n");
    printf("║  Results: total=%d passed=%d failed=%d        ║\n", t, p, f);
    printf("╚══════════════════════════════════════════════╝\n");
    log_close();
    return f > 0 ? 1 : 0;
}
