/**
 * @file    test_av.c
 * @brief   av_daemon 模块单元测试 — 音乐目录扫描 (scan_music)
 *
 * 覆盖范围:
 *   scan_music() 是 av_daemon 中唯一不依赖 ALSA 硬件的纯逻辑函数,
 *   测试其行为: 扩展名过滤 / 隐藏文件跳过 / 数量上限 / 目录异常处理。
 *
 * 为什么不测播放相关函数:
 *   open_track / play_chunk / handle_sockets 都需要真实的 PCM 设备与
 *   socket 连接, 只能在开发板上验证, 不适合放入自动化单元测试。
 *
 * 如何测试 static 函数:
 *   av_daemon.c 中的函数全部是 static, 外部无法直接调用。
 *   这里通过 #include 源文件的方式让测试代码处于同一编译单元,
 *   从而访问其 static 函数与静态变量。
 *   为避免 av_daemon.c 的 main() 与测试框架的 main() 冲突,
 *   include 期间用宏将其重命名 (include 后立即 #undef)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

/* 重命名 av_daemon.c 的 main, 使其不覆盖本文件的 main */
#define main av_daemon_main_disabled
#include "av_daemon.c"
#undef main

static int p = 0, f = 0, t = 0;
#define TS(n) printf("\n========== [TEST] %s ==========\n", n)
#define TC(d) do { t++; printf("  [%d] %s ... ", t, d); } while (0)
#define OK()  do { printf("OK\n"); p++; } while (0)
#define FL(m) do { printf("FAIL: %s\n", m); f++; return; } while (0)
#define AS(c, m) do { if (!(c)) FL(m); } while (0)

/* ---- 辅助: 在指定目录创建空文件 ---- */
static void touch(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *fp = fopen(path, "w");
    if (fp) fclose(fp);
}

/* ---- 辅助: 递归删除临时目录 ---- */
static void rmrf(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

/* ---- 1. 目录不存在 / 空目录 ---- */
static void test_empty_and_missing(void)
{
    TS("Empty & Missing Directory");

    TC("missing directory → 0");
    AS(scan_music("/tmp/__av_test_no_such_dir__") == 0, "should return 0");
    AS(g_track_count == 0, "track count should be 0");
    OK();

    char tmpl[] = "/tmp/av_test_empty_XXXXXX";
    char *dir = mkdtemp(tmpl);
    AS(dir != NULL, "mkdtemp failed");

    TC("empty directory → 0");
    AS(scan_music(dir) == 0, "should return 0");
    OK();

    rmrf(dir);
}

/* ---- 2. 扩展名过滤与隐藏文件 ---- */
static void test_filter_rules(void)
{
    TS("File Filtering Rules");

    char tmpl[] = "/tmp/av_test_filter_XXXXXX";
    char *dir = mkdtemp(tmpl);
    AS(dir != NULL, "mkdtemp failed");

    touch(dir, "track_a.wav");
    touch(dir, "track_b.WAV");     /* 大写扩展名: 应识别 */
    touch(dir, "track_c.wav.bak"); /* 非 .wav 结尾: 应忽略 */
    touch(dir, "song.mp3");        /* 其他音频: 应忽略 */
    touch(dir, "cover.jpg");
    touch(dir, ".hidden.wav");     /* 隐藏文件: 应跳过 */
    touch(dir, "readme.txt");

    TC("only .wav counted (case-insensitive), hidden skipped");
    int n = scan_music(dir);
    AS(n == 2, "expected 2 tracks (a.wav + b.WAV)");
    AS(g_track_count == 2, "g_track_count mismatch");
    OK();

    TC("collected paths are absolute and end with filename");
    int found_a = 0, found_b = 0;
    for (int i = 0; i < g_track_count; i++) {
        if (strstr(g_tracks[i], "track_a.wav")) found_a = 1;
        if (strstr(g_tracks[i], "track_b.WAV")) found_b = 1;
        AS(strncmp(g_tracks[i], dir, strlen(dir)) == 0,
           "path should start with dir");
    }
    AS(found_a, "track_a.wav missing");
    AS(found_b, "track_b.WAV missing (case-insensitive check)");
    OK();

    TC("'.wav.bak' not treated as wav");
    for (int i = 0; i < g_track_count; i++) {
        AS(strstr(g_tracks[i], ".bak") == NULL, ".bak file should be ignored");
    }
    OK();

    rmrf(dir);
}

/* ---- 3. 数量上限 MAX_TRACKS ---- */
static void test_max_tracks(void)
{
    TS("MAX_TRACKS Cap");

    char tmpl[] = "/tmp/av_test_cap_XXXXXX";
    char *dir = mkdtemp(tmpl);
    AS(dir != NULL, "mkdtemp failed");

    /* 创建超过 MAX_TRACKS 的文件 */
    const int created = MAX_TRACKS + 10;
    for (int i = 0; i < created; i++) {
        char name[64];
        snprintf(name, sizeof(name), "t%03d.wav", i);
        touch(dir, name);
    }

    TC("count capped at MAX_TRACKS");
    int n = scan_music(dir);
    AS(n == MAX_TRACKS, "should cap at MAX_TRACKS");
    AS(n < created, "should not count all files");
    OK();

    rmrf(dir);
}

int main(void)
{
    printf("╔══════════════════════════════════════════╗\n");
    printf("║  av_daemon — Module Unit Tests           ║\n");
    printf("╚══════════════════════════════════════════╝\n");

    /* scan_music 内部会调用 LOG_INFO, 初始化日志避免空指针 */
    log_init(NULL, LOG_ERROR, 0, 0);

    test_empty_and_missing();
    test_filter_rules();
    test_max_tracks();

    printf("\n╔══════════════════════════════════════════╗\n");
    printf("║  Results: total=%d passed=%d failed=%d     ║\n", t, p, f);
    printf("╚══════════════════════════════════════════╝\n");

    log_close();
    return f > 0 ? 1 : 0;
}
