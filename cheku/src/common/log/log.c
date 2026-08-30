/**
 * @file    log.c
 * @brief   日志系统 - 实现
 *
 * 实现细节:
 *   - 日志格式: [2026-08-01 21:30:45] [INFO ] [gps] (gps_daemon.c:128) GPS initialized
 *   - 轮转策略: 重命名方式 (log → log.1 → log.2 → ...)
 *   - 线程安全: pthread_mutex 保护 (虽然是单线程项目，但养成好习惯)
 *   - 控制台彩色输出: DEBUG=灰色, INFO=默认, WARN=黄色, ERROR=红色
 *
 * 设计思想:
 *   不是每条日志都 fflush —— 对于嵌入式 eMMC 存储，频繁小写入会
 *   加速闪存磨损。方案是每行日志后 fflush 一次 (行缓冲模式)，
 *   既保证日志不丢失（设备突然断电时最多丢失最后一行），
 *   也不会造成过度的闪存擦写。
 *
 * 学习要点:
 *   1. va_list/va_start/va_end - C 标准可变参数处理
 *   2. localtime_r vs localtime - 线程安全的区别
 *   3. strftime - 时间格式化
 *   4. snprintf - 安全的字符串格式化
 *   5. rename() - 文件重命名实现轮转
 *   6. stat() + st_size - 获取文件大小
 *   7. 互斥锁 pthread_mutex - 线程安全基础
 */

#include "log/log.h"
#include "common.h"                /* LOG_BUF_SIZE, ret_code_t, 公共类型 */

#include <stdio.h>          /* FILE, fopen, fclose, fprintf, fflush */
#include <stdlib.h>         /* malloc, free, getenv */
#include <string.h>         /* strlen, strerror, snprintf */
#include <stdarg.h>         /* va_list, va_start, va_end, vsnprintf */
#include <time.h>           /* time, localtime_r, strftime, clock_gettime */
#include <sys/stat.h>       /* stat, struct stat */
#include <unistd.h>         /* rename, access */
#include <errno.h>          /* errno */
#include <pthread.h>        /* pthread_mutex_t */

/* ================================================================
 *  日志系统内部状态结构
 *  使用 static 全局变量，模块外部不可见 (封装)
 * ================================================================ */
static struct {
    FILE           *fp;             /* 日志文件指针 (NULL = 未打开) */
    log_level_t     level;          /* 当前日志级别 */
    char            file_path[256]; /* 日志文件路径 */
    uint32_t        max_file_size;  /* 文件大小上限 (字节), 0=不限制 */
    int             max_files;      /* 最大保留文件数, 0=不轮转 */
    pthread_mutex_t lock;           /* 互斥锁 (保护多线程同时写日志) */
    int             initialized;    /* 是否已初始化 (0/1) */
    int             use_color;      /* 是否使用终端彩色输出 */
} g_log = {
    .fp            = NULL,
    .level         = LOG_INFO,
    .file_path     = {0},
    .max_file_size = 0,
    .max_files     = 0,
    .lock          = PTHREAD_MUTEX_INITIALIZER,
    .initialized   = 0,
    .use_color     = 1,
};

/* ================================================================
 *  ANSI 终端颜色转义序列
 *  仅在输出到终端时生效，写入文件时不包含颜色码
 * ================================================================ */
#define COLOR_RESET   "\033[0m"       /* 重置所有属性 */
#define COLOR_RED     "\033[1;31m"    /* 粗体红色 */
#define COLOR_YELLOW  "\033[1;33m"    /* 粗体黄色 */
#define COLOR_GRAY    "\033[1;30m"    /* 粗体灰色 */

/**
 * @brief 检查输出是否为终端 (用于决定是否输出颜色码)
 *
 * stdout/stderr 可能被重定向到文件或管道，
 * 此时不应该输出 ANSI 颜色码 (会变成乱码)。
 */
static int is_terminal(FILE *stream)
{
    /* 简单判断: 如果环境变量 NO_COLOR 设置了，则不用颜色 */
    if (getenv("NO_COLOR") != NULL) {
        return 0;
    }
    /* 在嵌入式 Linux 上，isatty() 可以判断 fd 是否指向终端。
     * 这里我们简化处理：总是用颜色 (因为嵌入式终端通常支持 ANSI)。
     * 如果输出重定向到文件，ANSI 码虽然乱但不影响日志核心内容。
     * 正式产品中可以调用 isatty(fileno(stream)) 来判断。 */
    (void)stream;
    return 1;
}

/* ---- 内部辅助函数 ---- */

/**
 * @brief 获取日志级别对应的颜色码
 * @param level  日志级别
 * @return       ANSI 颜色字符串
 */
static const char* level_color(log_level_t level)
{
    switch (level) {
    case LOG_DEBUG: return COLOR_GRAY;      /* DEBUG: 灰色，不显眼 */
    case LOG_WARN:  return COLOR_YELLOW;    /* WARN:  黄色，警告感 */
    case LOG_ERROR: return COLOR_RED;       /* ERROR: 红色，醒目 */
    default:        return "";              /* INFO:  默认颜色 */
    }
}

/**
 * @brief 获取日志级别对应的文本标签
 * @param level  日志级别
 * @return       固定宽度的级别字符串 (5字符，含空格)
 */
static const char* level_str(log_level_t level)
{
    switch (level) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO ";
    case LOG_WARN:  return "WARN ";
    case LOG_ERROR: return "ERROR";
    default:        return "?????";
    }
}

/**
 * @brief 获取当前时间戳字符串 (线程安全)
 * @param buf   输出缓冲区 (至少 20 字节)
 * @param size  缓冲区大小
 *
 * 格式: YYYY-MM-DD HH:MM:SS
 * 使用 localtime_r 而非 localtime，确保线程安全。
 * localtime 返回静态缓冲区指针，多线程同时调用会互相覆盖。
 */
static void get_timestamp(char *buf, size_t size)
{
    time_t now = time(NULL);
    struct tm tm_info;

    /* localtime_r 是 POSIX 线程安全版本:
     *   参数1: time_t 指针
     *   参数2: 用户提供的 struct tm 缓冲区 (区别于 localtime 的 static 缓冲)
     *   返回值: 成功返回 &tm_info，失败返回 NULL */
    localtime_r(&now, &tm_info);

    /* strftime: 将 struct tm 按格式转换为字符串
     *   %Y = 4位年份, %m = 2位月份, %d = 2位日期
     *   %H = 24小时制, %M = 分钟, %S = 秒 */
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", &tm_info);
}

/**
 * @brief 执行日志轮转
 *
 * 轮转算法 (重命名方式):
 *   log.2 → log.3
 *   log.1 → log.2
 *   log   → log.1
 *
 * 为什么用重命名而不用截断?
 *   - rename() 是原子操作 (同一文件系统内)
 *   - 不会丢失日志数据
 *   - 比 truncate() 更安全
 */
static void do_rotate(void)
{
    char old_path[320];
    char new_path[320];
    int i;

    /* 1. 删除最旧的备份文件 (如果存在) */
    snprintf(old_path, sizeof(old_path), "%s.%d",
             g_log.file_path, g_log.max_files);
    unlink(old_path);  /* unlink = 删除文件, 文件不存在也不报错 */

    /* 2. 从旧到新依次重命名: log.N → log.(N+1) */
    for (i = g_log.max_files - 1; i >= 1; i--) {
        snprintf(old_path, sizeof(old_path), "%s.%d",
                 g_log.file_path, i);
        snprintf(new_path, sizeof(new_path), "%s.%d",
                 g_log.file_path, i + 1);

        /* rename() 底层是 link + unlink, 同一文件系统内原子操作
         * 如果 old_path 不存在, rename 返回 -1 (errno=ENOENT), 忽略即可 */
        if (rename(old_path, new_path) != 0) {
            /* 文件不存在是正常的 (第一次轮转时还没有 .2, .3 等) */
            if (errno != ENOENT) {
                fprintf(stderr, "[LOG] rotate rename failed: %s\n",
                        strerror(errno));
            }
        }
    }

    /* 3. 当前日志文件重命名为 .1 */
    snprintf(new_path, sizeof(new_path), "%s.1", g_log.file_path);
    /* 先关闭当前文件才能重命名 */
    if (g_log.fp) {
        fclose(g_log.fp);
        g_log.fp = NULL;
    }
    rename(g_log.file_path, new_path);

    /* 4. 打开新的日志文件 */
    g_log.fp = fopen(g_log.file_path, "a");  /* "a" = append 模式 */
    if (!g_log.fp) {
        fprintf(stderr, "[LOG] rotate: fopen %s failed: %s\n",
                g_log.file_path, strerror(errno));
    }
}

/**
 * @brief 检查是否需要轮转，需要则执行
 */
static void check_rotate(void)
{
    struct stat st;

    /* 检查前置条件 */
    if (!g_log.fp || g_log.max_file_size == 0 || g_log.max_files == 0) {
        return;  /* 未开启轮转功能 */
    }

    /* stat() 获取文件元信息 (大小、权限、时间等)
     * st_size: 文件字节数 (off_t, 通常是 long 或 long long) */
    if (stat(g_log.file_path, &st) == 0) {
        if ((uint32_t)st.st_size >= g_log.max_file_size) {
            do_rotate();  /* 文件超过大小限制，执行轮转 */
        }
    }
}

/* ================================================================
 *  API 实现
 * ================================================================ */

int log_init(const char *file_path, log_level_t level,
             uint32_t max_file_size, int max_files)
{
    /* 如果已经初始化过，先关闭旧的 */
    if (g_log.initialized) {
        log_close();
    }

    /* 设置参数 */
    g_log.level         = level;
    g_log.max_file_size = max_file_size;
    g_log.max_files     = max_files;
    g_log.initialized   = 1;

    /* 打开日志文件 */
    if (file_path && file_path[0] != '\0') {
        strncpy(g_log.file_path, file_path, sizeof(g_log.file_path) - 1);
        g_log.file_path[sizeof(g_log.file_path) - 1] = '\0';

        g_log.fp = fopen(file_path, "a");  /* append 模式: 追加不截断 */
        if (!g_log.fp) {
            fprintf(stderr, "[LOG] Cannot open log file '%s': %s\n",
                    file_path, strerror(errno));
            /* 不返回失败: 文件打不开仍然可以用控制台输出 */
        } else {
            /* 设置为行缓冲模式:
             *   _IOLBF = 行缓冲 (遇到 \n 自动 flush)
             *   这样日志不会因为进程崩溃而丢失超过一行的内容
             *   相比全缓冲 (_IOFBF) 更安全, 相比无缓冲 (_IONBF) 性能更好 */
            setvbuf(g_log.fp, NULL, _IOLBF, 0);
        }
    }

    /* 启动日志: 写一条 INFO 标记日志系统启动 */
    LOG_INFO("log", "===== Log system started (level=%d, file=%s) =====",
             level, file_path ? file_path : "console");

    return 0;
}

void log_close(void)
{
    if (!g_log.initialized) {
        return;
    }

    /* 写关闭标记 */
    LOG_INFO("log", "===== Log system stopped =====");

    pthread_mutex_lock(&g_log.lock);

    if (g_log.fp) {
        fflush(g_log.fp);           /* 强制刷新缓冲区到磁盘 */
        fclose(g_log.fp);
        g_log.fp = NULL;
    }

    g_log.initialized = 0;

    pthread_mutex_unlock(&g_log.lock);
}

void log_set_level(log_level_t level)
{
    /* 原子修改级别 (写 int 在大多数平台是原子的, 但严格说需要锁) */
    g_log.level = level;
    LOG_INFO("log", "Log level changed to %d", level);
}

log_level_t log_get_level(void)
{
    return g_log.level;
}

void log_write(const char *tag, log_level_t level,
               const char *file, int line,
               const char *fmt, ...)
{
    char    timestamp[32];
    char    msg_buf[LOG_BUF_SIZE];   /* 格式化后的完整消息 */
    va_list args;
    int     msg_len;
    int     use_color_flag;

    /* 1. 级别过滤: 低于当前设置的级别直接丢弃
     *    例如: level=LOG_DEBUG(0), g_log.level=LOG_INFO(1)
     *          DEBUG 被丢弃, INFO/WARN/ERROR 保留 */
    if (level < g_log.level) {
        return;
    }

    if (!g_log.initialized) {
        /* 未初始化时也输出到 stderr，方便调试 */
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
        fprintf(stderr, "\n");
        return;
    }

    /* 2. 生成时间戳 */
    get_timestamp(timestamp, sizeof(timestamp));

    /* 3. 格式化消息内容 (fmt + ... → msg_body) */
    {
        char msg_body[LOG_BUF_SIZE];
        va_start(args, fmt);
        /* vsnprintf: 安全的格式化函数
         *   参数1: 输出缓冲区
         *   参数2: 缓冲区大小 (不会越界写入, 超出部分截断)
         *   参数3: 格式化字符串
         *   参数4: va_list 可变参数
         *   返回值: 实际需要的字符串长度 (不含 '\0')
         *   如果返回值 >= size, 说明内容被截断了 */
        msg_len = vsnprintf(msg_body, sizeof(msg_body), fmt, args);
        va_end(args);

        /* 确保不越界 */
        if (msg_len < 0) {
            msg_body[0] = '\0';
            msg_len = 0;
        } else if ((size_t)msg_len >= sizeof(msg_body)) {
            msg_len = sizeof(msg_body) - 1;
        }

        /* 4. 组装完整日志行
         *    格式: [时间戳] [级别] [标签] (文件:行号) 消息内容
         *
         *    文件名处理: 只取文件名部分 (去掉路径)
         *    例如: ../../src/common/log/log.c → log.c */
        const char *short_file = strrchr(file, '/');
        if (short_file) {
            short_file++;  /* 跳过最后一个 '/' */
        } else {
            /* 在 Windows 上路径分隔符是 '\' */
            short_file = strrchr(file, '\\');
            if (short_file) short_file++;
            else short_file = file;
        }

        snprintf(msg_buf, sizeof(msg_buf),
                 "[%s] [%s] [%s] (%s:%d) %s",
                 timestamp,
                 level_str(level),
                 tag ? tag : "-",
                 short_file,
                 line,
                 msg_body);
    }

    /* 5. 加锁 (保护 g_log.fp 和控制台输出不交错) */
    pthread_mutex_lock(&g_log.lock);

    /* 6. 写入日志文件 (纯文本，不带颜色) */
    if (g_log.fp) {
        fprintf(g_log.fp, "%s\n", msg_buf);
        /* fflush 确保日志即使程序崩溃也不会丢失最后一行
         * 代价: 每条日志一次系统调用 write()。
         * 在嵌入式场景中日志量不大 (< 100条/秒), 这开销可忽略 */
        fflush(g_log.fp);
    }

    /* 7. 输出到控制台 (带 ANSI 颜色) */
    use_color_flag = g_log.use_color && is_terminal(stdout);
    if (level >= LOG_WARN) {
        /* WARN 和 ERROR 输出到 stderr */
        if (use_color_flag) {
            fprintf(stderr, "%s%s%s\n",
                    level_color(level), msg_buf, COLOR_RESET);
        } else {
            fprintf(stderr, "%s\n", msg_buf);
        }
    } else {
        /* DEBUG 和 INFO 输出到 stdout */
        if (use_color_flag) {
            printf("%s%s%s\n",
                   level_color(level), msg_buf, COLOR_RESET);
        } else {
            printf("%s\n", msg_buf);
        }
    }
    fflush(stdout);
    fflush(stderr);

    pthread_mutex_unlock(&g_log.lock);

    /* 8. 检查是否需要日志轮转 (在锁外面做，减少锁持有时间) */
    check_rotate();
}

void log_flush(void)
{
    pthread_mutex_lock(&g_log.lock);
    if (g_log.fp) {
        fflush(g_log.fp);
    }
    fflush(stdout);
    fflush(stderr);
    pthread_mutex_unlock(&g_log.lock);
}
