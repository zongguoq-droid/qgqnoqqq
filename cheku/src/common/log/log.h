/**
 * @file    log.h
 * @brief   日志系统 - 头文件
 *
 * 功能特性:
 *   - 四级日志: DEBUG(0) / INFO(1) / WARN(2) / ERROR(3)
 *   - 支持写文件和/或控制台
 *   - 自动日志轮转 (按文件大小)
 *   - 线程安全 (通过互斥锁，虽然本项目主要用单线程事件驱动)
 *   - 带时间戳和来源标识
 *
 * 使用示例:
 *   log_init("/var/log/car_terminal/gps.log", LOG_INFO, 1024*1024, 5);
 *   LOG_INFO("gps", "GPS module initialized, device=%s", "/dev/ttyUSB0");
 *   LOG_DEBUG("gps", "read %d bytes from uart", n);
 *   LOG_ERROR("gps", "Failed to open UART: %s", strerror(errno));
 *   log_close();
 *
 * 学习要点:
 *   1. 可变参数 va_list / vsnprintf - C 语言变参函数
 *   2. 文件 IO 缓冲策略 - 何时 fflush / 何时依赖内核缓冲
 *   3. 日志轮转算法 - 重命名旧文件实现轮转
 *   4. 时间格式化 - strftime / localtime_r (线程安全版)
 */

#ifndef _LOG_H_
#define _LOG_H_

#include <stdio.h>      /* FILE, vsnprintf */
#include <stdint.h>     /* uint32_t */

/* ================================================================
 *  日志级别定义
 *  数值越小越详细，ERROR 数值最大仅输出严重问题
 * ================================================================ */
typedef enum {
    LOG_DEBUG   = 0,    /* 调试信息 (开发阶段使用，发布后关闭) */
    LOG_INFO    = 1,    /* 常规信息 (系统启动/停止/配置变更) */
    LOG_WARN    = 2,    /* 警告信息 (可恢复的异常，如重试成功) */
    LOG_ERROR   = 3,    /* 错误信息 (操作失败，需要关注) */
    LOG_NONE    = 4,    /* 关闭日志 (用于 log_set_level) */
} log_level_t;

/* ================================================================
 *  日志级别字符串映射 (内部使用)
 *  格式: [YYYY-MM-DD HH:MM:SS] [LEVEL] [TAG] message
 * ================================================================ */
#define LOG_LEVEL_STR_DEBUG  "DEBUG"
#define LOG_LEVEL_STR_INFO   "INFO "
#define LOG_LEVEL_STR_WARN   "WARN "
#define LOG_LEVEL_STR_ERROR  "ERROR"

/* ================================================================
 *  API 函数声明
 * ================================================================ */

/**
 * @brief 初始化日志系统
 *
 * 打开日志文件，设置日志级别和轮转参数。
 * 日志同时输出到文件和控制台 (stdout for DEBUG/INFO, stderr for WARN/ERROR)。
 *
 * @param file_path     日志文件路径 (如 "/var/log/gps.log")
 *                      传 NULL 或空字符串则只输出到控制台
 * @param level         最低输出级别 (如 LOG_INFO 则抑制 DEBUG 信息)
 * @param max_file_size 单个日志文件最大字节数 (超过则触发轮转)
 *                      传 0 表示不按大小轮转
 * @param max_files     保留的历史日志文件数量 (轮转时使用)
 *                      传 0 表示不轮转，一直追加
 * @return              成功返回 0，失败返回 -1
 *
 * 轮转规则示例 (max_file_size=1MB, max_files=3):
 *   首次满: gps.log → gps.log.1, 创建新的 gps.log
 *   再次满: gps.log.1 → gps.log.2, gps.log → gps.log.1, 创建新的 gps.log
 *   第三次: 同上, gps.log.2 → gps.log.3
 *   第四次: gps.log.3 被丢弃, 其他依次前移
 */
int log_init(const char *file_path, log_level_t level,
             uint32_t max_file_size, int max_files);

/**
 * @brief 关闭日志系统
 *
 * 刷新缓冲区并关闭日志文件。程序退出前必须调用。
 * 调用后仍然可以 log_write()，但只输出到控制台。
 */
void log_close(void);

/**
 * @brief 动态修改日志级别 (运行时调整)
 * @param level  新的日志级别
 *
 * 例如调试时临时改为 LOG_DEBUG 查看详细日志。
 */
void log_set_level(log_level_t level);

/**
 * @brief 获取当前日志级别
 * @return  当前级别
 */
log_level_t log_get_level(void);

/**
 * @brief 核心写日志函数 (不直接调用，使用下面的便捷宏)
 *
 * @param tag     模块标识 (如 "gps", "dvr", "guard")
 *                用于在日志中区分不同模块
 * @param level   日志级别
 * @param file    源码文件名 (通常传 __FILE__)
 * @param line    源码行号 (通常传 __LINE__)
 * @param fmt     格式化字符串 (printf 风格)
 * @param ...     可变参数
 */
void log_write(const char *tag, log_level_t level,
               const char *file, int line,
               const char *fmt, ...)
#ifdef __GNUC__
    /* GCC/Clang 编译器的格式检查属性:
     *   __attribute__((format(printf, 5, 6)))
     *   告诉编译器第5个参数(fmt)是 printf 风格的格式串，
     *   第6个参数开始是可变参数。编译器会自动检查 %d/%s 等
     *   是否与参数类型匹配。 */
    __attribute__((format(printf, 5, 6)))
#endif
;

/**
 * @brief 强制刷新日志缓冲区到磁盘
 *
 * 正常情况下日志系统在每行后自动 fflush，
 * 此函数用于需要立即确保数据落盘的关键场景。
 */
void log_flush(void);

/* ================================================================
 *  便捷宏 - 在代码中使用这些宏写日志
 *  自动填充 __FILE__ 和 __LINE__，方便定位问题
 * ================================================================ */
#define LOG_DEBUG(tag, fmt, ...) \
    log_write(tag, LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_INFO(tag, fmt, ...) \
    log_write(tag, LOG_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_WARN(tag, fmt, ...) \
    log_write(tag, LOG_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_ERROR(tag, fmt, ...) \
    log_write(tag, LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/*
 * ##__VA_ARGS__ 说明:
 *   GCC 扩展语法。当可变参数为空时，前面的逗号会被自动吞掉。
 *   例如: LOG_INFO("gps", "started") 展开为
 *   log_write("gps", LOG_INFO, __FILE__, __LINE__, "started")
 *   注意 "started" 后面没有多余的逗号。
 *
 *   如果用标准的 __VA_ARGS__ (不带 ##)，空参数时会产生尾随逗号，
 *   导致编译错误 (C99 标准)。GNU C 的 ## 解决了这个问题。
 *
 *   如果强制使用 C99 标准编译 (-std=c99 -pedantic)，可以用:
 *   #define LOG_INFO(tag, ...) log_write(tag, LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
 *   但要求至少传一个参数 (fmt 必需)。
 */

#endif /* _LOG_H_ */
