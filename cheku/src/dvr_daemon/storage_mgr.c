/**
 * @file    storage_mgr.c
 * @brief   DVR分段循环录制存储管理器实现
 *
 * 循环录制策略:
 *   1. 基于时间的分段: 每个segment_sec(默认60秒)一个文件。
 *      主循环在当前分段的墙上时钟时长超过segment_sec时轮转到新文件。
 *   2. 命名规范: base_path下的 YYYYMMDD_HHMMSS_dvr.h264。
 *      .h264后缀仅为约定 — MJPEG采集下每帧是原始JPEG数据,
 *      而非Annex-B H.264。重命名操作已推迟。
 *   3. 空间管理: 当目录总大小超过max_size_mb时,
 *      删除最旧文件(按st_mtime)。重复此操作直到
 *      使用量降至配额的80%以下。
 *   4. 文件I/O: 缓冲FILE* (setvbuf _IOFBF, 64KB缓冲区),
 *      每15次写入(~15fps下约1秒)定期fflush, 以平衡
 *      吞吐量与崩溃安全性。
 *
 * 本模块已修复的关键错误:
 *   A. memset/路径别名 (dvr_storage_init):
 *      调用者将 &ctx->storage.base_path 作为 `path` 传入, 与
 *      st->base_path 别名相同。如果在strncpy读取 `path` 之前
 *      执行 memset(st, 0, ...), 源内存被清零 → 空路径。
 *      通过在调用memset前将 `path` 保存到栈本地缓冲区修复。
 *   B. fopen前缺少mkdir (dvr_storage_open_segment):
 *      首次运行时录制目录不存在, 导致 fopen(..., "wb")
 *      静默失败。通过在打开分段文件前添加mkdir()调用
 *      (抑制EEXIST错误)修复。
 *
 * 教育价值 (POSIX文件系统API):
 *   1. opendir / readdir / stat — 目录遍历与文件元数据
 *   2. fwrite缓冲I/O — 吞吐量与安全性的权衡, setvbuf调优
 *   3. 循环覆盖算法 — 按mtime排序, 优先删除最旧文件
 */

#include "dvr_daemon.h"
#include "log/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/stat.h>
#include <sys/types.h>

/**
 * @brief 初始化存储管理器状态并确保录制目录存在。
 *
 * 通过memset将所有字段初始化为零/已知值, 然后将基础路径
 * 和配额参数复制到结构体中。如果目录不存在则创建录制目录
 * (mkdir并抑制EEXIST错误)。
 *
 * 错误修复 (memset/路径别名): 调用者通常将 &ctx->storage.base_path
 * 作为 `path` 参数传入 — 这与 st->base_path 是同一块内存。如果先执行
 * memset(st, 0, ...), `path` 指向的源字符串在strncpy读取之前就被清零,
 * 导致base_path为空。修复方案: 在memset调用之前将 `path` 保存到
 * 栈本地缓冲区 (`saved_path`), 使内容在零填充后仍然保留。
 *
 * @param st      要初始化的存储管理器状态
 * @param path    录制文件的基础目录 (可能与 st->base_path 别名相同!)
 * @param max_mb  总存储配额(MB)
 * @param seg_sec 分段时长(秒)
 * @return        成功返回0, NULL参数返回-1
 */
int dvr_storage_init(dvr_storage_t *st, const char *path, int max_mb, int seg_sec)
{
    if (!st || !path) return -1;

    /* 错误修复: 在memset之前将 `path` 保存到本地缓冲区, 因为 `path`
     * 可能指向st->base_path (同一块内存)。如果先memset,
     * 源字节被清零 → 空路径。 */
    char saved_path[256];
    strncpy(saved_path, path, sizeof(saved_path) - 1);
    saved_path[sizeof(saved_path) - 1] = '\0';

    memset(st, 0, sizeof(*st));

    strncpy(st->base_path, saved_path, sizeof(st->base_path) - 1);
    st->max_size_mb  = max_mb;
    st->segment_sec  = seg_sec;

    /* 确保录制目录存在 (POSIX mkdir, 不依赖shell)。
     * EEXIST不是错误 — 目录可能已经存在。 */
    if (mkdir(saved_path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[storage] WARNING: Cannot create %s: %s\n", saved_path, strerror(errno));
    }

    LOG_INFO("storage", "Init: %s (max=%dMB, seg=%ds)", saved_path, max_mb, seg_sec);
    return 0;
}

/**
 * @brief 关闭上一个分段(如有)并打开新的带时间戳文件。
 *
 * 文件路径格式: <base_path>/YYYYMMDD_HHMMSS_dvr.h264
 * 文件以"wb"(写入二进制, 截断)方式打开, 并配置为
 * 全缓冲(64KB缓冲区)以最小化系统调用开销。
 *
 * 错误修复 (缺少mkdir): 首次执行时录制目录不存在,
 * 因此fopen("wb")会因ENOENT静默失败。现在在fopen()
 * 之前调用mkdir()以确保父目录存在。当目录已存在时
 * 此调用是安全的(EEXIST被忽略)。
 *
 * @param st    存储管理器状态
 * @param start 用于文件名中时间戳的墙上时钟时间
 * @return      成功返回0, fopen失败返回-1 (磁盘满、权限等)
 */
int dvr_storage_open_segment(dvr_storage_t *st, time_t start)
{
    char filepath[320];
    struct tm tm_info;
    char ts[32];

    if (!st) return -1;

    /* 如果上一个分段文件已打开, 则关闭它 */
    if (st->current_fp) {
        fclose(st->current_fp);
        st->current_fp = NULL;
    }

    /* 错误修复: 在fopen之前确保目录存在 (首次运行安全)。
     * EEXIST无害 — 目录已创建时为空操作。 */
    mkdir(st->base_path, 0755);

    /* 为文件名生成可读的时间戳 */
    localtime_r(&start, &tm_info);
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_info);

    snprintf(filepath, sizeof(filepath), "%s/%s_dvr.h264", st->base_path, ts);

    st->current_fp = fopen(filepath, "wb");
    if (!st->current_fp) {
        LOG_ERROR("storage", "Cannot create: %s (%s)", filepath, strerror(errno));
        return -1;
    }

    /* 全缓冲: 写入仅在缓冲区满或fflush时才到达内核。
     * _IOFBF = 全缓冲, 64KB缓冲区 (默认BUFSIZ仅8KB)。
     * 这显著减少了每秒write()系统调用的次数。 */
    setvbuf(st->current_fp, NULL, _IOFBF, 65536);  /* 64KB缓冲区 */

    st->current_segment++;
    st->current_size = 0;

    LOG_INFO("storage", "Segment #%d: %s", st->current_segment, filepath);
    return 0;
}

/**
 * @brief 将原始帧数据追加到当前打开的分段文件。
 *
 * 将 `data` 中的 `len` 字节写入分段FILE*流。该流使用全缓冲
 * (64KB, 在dvr_storage_open_segment中设置), 因此fwrite
 * 通常立即返回而不发出write()系统调用。
 *
 * 定期fflush: 每15次写入时显式地将缓冲区刷新到磁盘。
 * 在15fps下, 这意味着崩溃时约1秒的数据面临风险
 * (对于行车记录仪来说可以接受)。否则, 断电可能丢失
 * 高达64KB的缓冲数据 (~15fps下约4秒)。
 *
 * 错误处理 (调用者): 如果current_fp为NULL则返回-1。
 * dvr_daemon.c中的调用者检查返回值, 并可能在持续失败时
 * 终止录制 (每150帧记录一次, 避免日志刷屏)。
 *
 * @param st   存储管理器状态
 * @param data 指向原始帧字节的指针 (MJPEG或H.264数据)
 * @param len  要写入的字节数
 * @return     实际写入的字节数 (应等于len), 出错返回-1
 */
int dvr_storage_write(dvr_storage_t *st, const uint8_t *data, int len)
{
    if (!st || !data || len <= 0) return -1;
    if (!st->current_fp) {
        /* 文件未打开 — 目录可能不存在或权限不正确。
         * 守护进程主循环将此视为录制终止条件。 */
        return -1;
    }

    size_t n = fwrite(data, 1, (size_t)len, st->current_fp);
    st->current_size += (int)n;

    /* 每15次写入刷新到磁盘 (~15fps下约1秒)。
     * 权衡: 频率越高 = 崩溃时数据丢失越少,
     *       频率越低 = 吞吐量越好 (更少的系统调用)。 */
    static int write_count = 0;
    if (++write_count % 15 == 0) {
        fflush(st->current_fp);
    }

    return (int)n;
}

/**
 * @brief 刷新并关闭当前分段文件。
 *
 * 幂等操作 — 可安全地多次调用或在没有文件打开时调用。
 * 在fclose之前执行显式的fflush, 确保在释放文件句柄之前
 * 任何缓冲数据 (FILE*缓冲区中最多64KB) 都已写入磁盘。
 *
 * @param st  存储管理器状态
 */
void dvr_storage_close(dvr_storage_t *st)
{
    if (!st) return;
    if (st->current_fp) {
        fflush(st->current_fp);
        fclose(st->current_fp);
        st->current_fp = NULL;
    }
}

/**
 * @brief 获取目录下所有录像文件的总大小
 *
 * 遍历目录, stat() 获取每个文件大小, 累加。
 *
 * @param path  目录路径
 * @return      总大小 (KB), -1=错误
 */
static int storage_total_size_kb(const char *path)
{
    DIR *d;
    struct dirent *e;
    struct stat stbuf;
    char fp[320];
    int total = 0;

    d = opendir(path);
    if (!d) return -1;

    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        /* 只计算 .h264 文件 */
        if (!strstr(e->d_name, "_dvr.h264")) continue;

        snprintf(fp, sizeof(fp), "%s/%s", path, e->d_name);
        if (stat(fp, &stbuf) == 0) {
            total += (int)(stbuf.st_size / 1024);
        }
    }

    closedir(d);
    return total;
}

/**
 * @brief 获取目录下最老的录像文件路径
 *
 * 使用 stat.st_mtime 比较修改时间, 找到最早的文件。
 *
 * @param path    目录路径
 * @param oldest  输出: 最老文件路径
 * @param size    缓冲区大小
 * @return        0=找到, -1=目录空
 */
static int storage_find_oldest(const char *path, char *oldest, int size)
{
    DIR *d;
    struct dirent *e;
    struct stat stbuf;
    char fp[320];
    time_t oldest_time = 0;
    int found = 0;

    d = opendir(path);
    if (!d) return -1;

    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (!strstr(e->d_name, "_dvr.h264")) continue;

        snprintf(fp, sizeof(fp), "%s/%s", path, e->d_name);
        if (stat(fp, &stbuf) == 0) {
            if (!found || stbuf.st_mtime < oldest_time) {
                oldest_time = stbuf.st_mtime;
                strncpy(oldest, fp, (size_t)(size - 1));
                found = 1;
            }
        }
    }

    closedir(d);
    return found ? 0 : -1;
}

/**
 * @brief 检查目录总使用量, 超出配额时删除最旧分段。
 *
 * 由主循环定期调用 (约每30个poll超时, 即~30秒)。
 *
 * 算法:
 *   1. 计算base_path中所有_dvr.h264文件的总大小(KB)。
 *   2. 如果总量 > max_size_mb 的80%, 按st_mtime查找最旧文件。
 *   3. unlink()删除它并从累计总量中减去其大小。
 *   4. 重复直到总量 <= 配额的80% (或没有更多文件可删除)。
 *
 * 80%阈值提供了滞后效应 — 我们不会在每次检查中只删一个文件;
 * 而是批量删除直到有足够的余量。
 * 仅考虑匹配 "_dvr.h264" 后缀的.h264文件。
 *
 * @param st  存储管理器状态
 * @return    成功返回0, NULL参数返回-1
 */
int dvr_storage_check_and_clean(dvr_storage_t *st)
{
    int total_kb;
    int max_kb;

    if (!st) return -1;

    total_kb = storage_total_size_kb(st->base_path);
    max_kb   = st->max_size_mb * 1024;

    LOG_DEBUG("storage", "Usage: %dKB / %dKB", total_kb, max_kb);

    /* 删除最旧文件直到使用量低于配额的80%。
     * 80%阈值避免频繁抖动 — 批量删除以创建余量。 */
    while (total_kb > max_kb * 80 / 100) {
        char oldest[320];
        struct stat stbuf;

        if (storage_find_oldest(st->base_path, oldest, sizeof(oldest)) != 0) {
            break;  /* 没有更多文件可删除 */
        }

        if (stat(oldest, &stbuf) == 0) {
            LOG_INFO("storage", "Deleting old segment: %s (%ldKB)",
                     oldest, (long)(stbuf.st_size / 1024));
            unlink(oldest);
            total_kb -= (int)(stbuf.st_size / 1024);
        } else {
            break;
        }
    }

    return 0;
}

/**
 * @brief 枚举存储目录中所有 .h264 分段文件。
 *
 * 使用opendir()打开base_path, 按"_dvr.h264"后缀过滤条目,
 * 并用文件元数据(完整路径、mtime、大小KB)填充调用者提供的
 * `list` 数组。当收集了 `max` 个条目或目录遍历完毕时停止。
 *
 * 调用者分配并拥有 `list` 数组。返回值是实际写入的条目数
 * (可能小于 `max`)。
 *
 * @param st   存储管理器状态
 * @param list 调用者分配的输出数组
 * @param max  `list` 的容量
 * @return     写入 `list` 的条目数, 错误或空目录返回0
 */
int dvr_storage_list(dvr_storage_t *st, dvr_segment_t *list, int max)
{
    DIR *d;
    struct dirent *e;
    struct stat stbuf;
    char fp[320];
    int count = 0;

    if (!st || !list) return 0;

    d = opendir(st->base_path);
    if (!d) return 0;

    while ((e = readdir(d)) != NULL && count < max) {
        if (e->d_name[0] == '.') continue;
        if (!strstr(e->d_name, "_dvr.h264")) continue;

        snprintf(fp, sizeof(fp), "%s/%s", st->base_path, e->d_name);
        if (stat(fp, &stbuf) == 0) {
            snprintf(list[count].filepath, sizeof(list[count].filepath),
                     "%s", fp);
            list[count].start_time = stbuf.st_mtime;
            list[count].size_kb    = (int)(stbuf.st_size / 1024);
            count++;
        }
    }

    closedir(d);
    return count;
}
