/**
 * @file    config.c
 * @brief   INI 格式配置文件解析器 - 实现
 *
 * 解析算法:
 *   1. fopen 打开文件
 *   2. fgets 逐行读取
 *   3. 跳过空白行 (只有空白字符) / 注释行 (以 ; 或 # 开头)
 *   4. 识别节行 [section_name] → 更新 current_section
 *   5. 识别键值行 key = value → 创建 config_entry_t, 加入链表
 *   6. 忽略无法识别的行
 *
 * 字符串处理技巧 (本模块大量使用):
 *   - strchr(s, c):  查找字符 c 在字符串 s 中首次出现的位置
 *   - strrchr(s, c): 从尾部查找 (取文件名时用到)
 *   - strstr(s, sub): 查找子串
 *   - strncpy:       安全的字符串拷贝
 *   - strtol/strtod: 带错误检测的字符串→数字转换
 *
 * 学习要点:
 *   1. 逐行文件解析的基本流程
 *   2. 字符串 trim (去除首尾空白)
 *   3. 单向链表的构建 (头插法)
 *   4. strtol 与 atoi 的区别 (错误检测能力)
 *   5. 大小写不敏感比较 (strcasecmp)
 */

#include "config/config.h"
#include <stdio.h>      /* FILE, fopen, fgets, fprintf */
#include <stdlib.h>     /* malloc, free, strtol, strtod */
#include <string.h>     /* strlen, strncpy, strchr, strrchr */
#include <ctype.h>      /* isspace */
#include <strings.h>    /* strcasecmp (POSIX) */

/* ================================================================
 *  内部辅助函数
 * ================================================================ */

/**
 * @brief 去除字符串首尾的空白字符 (原地修改)
 *
 * 空白字符包括: 空格(' '), 制表符('\t'), 回车('\r'), 换行('\n')
 * 使用 C 标准库的 isspace() 判断。
 *
 * 算法分两步:
 *   1. 尾部→头部扫描: 将尾部的空白字符替换为 '\0'
 *   2. 头部→尾部扫描: 找到第一个非空白字符
 *   3. 如果头部有空白, 用 memmove 将有效内容移到开头
 *
 * 为什么用 memmove 而非 memcpy?
 *   当源和目标内存区域有重叠时, memcpy 行为未定义,
 *   memmove 保证正确处理重叠区域。
 *
 * @param str  待处理的字符串 (原地修改, 必须是可读写的字符数组)
 */
static void trim_in_place(char *str)
{
    char *start = str;
    char *end;
    int   len;

    if (!str || *str == '\0') return;

    /* 去尾部 */
    len = strlen(str);
    end = str + len - 1;
    while (end >= start && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    /* 去头部 */
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }

    /* 将去除头部空白后的内容移到开头 */
    if (start != str) {
        memmove(str, start, strlen(start) + 1);  /* +1 拷贝 '\0' */
    }
}

/**
 * @brief 构建完整键名 "section.key"
 *
 * @param buf      输出缓冲区
 * @param buf_size 缓冲区大小
 * @param section  节名
 * @param key      键名
 * @return         0=成功, -1=缓冲区不足
 */
static int make_full_key(char *buf, size_t buf_size,
                         const char *section, const char *key)
{
    int written = snprintf(buf, buf_size, "%s.%s", section, key);
    if (written < 0 || (size_t)written >= buf_size) {
        return -1;  /* 截断或错误 */
    }
    return 0;
}

/**
 * @brief 查找配置项 (内部线性搜索)
 *
 * 项目配置项数量少 (< 100), 线性搜索足够。
 * 如果以后配置项增多，可改用哈希表。
 *
 * @param cfg       配置对象
 * @param full_key  完整键名 "section.key"
 * @return          找到的 entry 指针, NULL=未找到
 */
static config_entry_t* find_entry(const config_t *cfg, const char *full_key)
{
    config_entry_t *entry;

    if (!cfg || !full_key) return NULL;

    for (entry = cfg->entries; entry != NULL; entry = entry->next) {
        if (strcmp(entry->full_key, full_key) == 0) {
            return entry;  /* 找到 */
        }
    }
    return NULL;  /* 未找到 */
}

/* ================================================================
 *  API 实现
 * ================================================================ */

int config_load(config_t *cfg, const char *file_path)
{
    FILE *fp;
    char  line[CONFIG_LINE_MAX_LEN];
    char  current_section[64] = "";  /* 当前节名, 空=未进入任何节 */
    int   line_num = 0;

    if (!cfg || !file_path) {
        return -1;
    }

    /* 初始化配置对象 */
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->file_path, file_path, sizeof(cfg->file_path) - 1);

    /* 打开配置文件 */
    fp = fopen(file_path, "r");
    if (!fp) {
        fprintf(stderr, "[CONFIG] Cannot open '%s'\n", file_path);
        return -1;
    }

    /* 逐行读取 */
    while (fgets(line, sizeof(line), fp)) {
        char *p;
        line_num++;

        /* 去除首尾空白和换行 */
        trim_in_place(line);

        /* 跳过空行 */
        if (line[0] == '\0') {
            continue;
        }

        /* 跳过注释行 (; 或 # 开头) */
        if (line[0] == ';' || line[0] == '#') {
            continue;
        }

        /* ---- 解析节名 [section] ---- */
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) {
                *end = '\0';  /* 截断在 ']' 处 */
                strncpy(current_section, line + 1,
                        sizeof(current_section) - 1);
            } else {
                fprintf(stderr, "[CONFIG] Line %d: missing ']'\n", line_num);
            }
            continue;
        }

        /* ---- 解析键值对 key=value ---- */
        p = strchr(line, '=');
        if (p) {
            char key[128];
            char value[CONFIG_VALUE_MAX_LEN];
            config_entry_t *entry;

            /* 分离 key 和 value */
            *p = '\0';  /* 在 '=' 处切断 */
            strncpy(key, line, sizeof(key) - 1);
            key[sizeof(key) - 1] = '\0';
            trim_in_place(key);

            strncpy(value, p + 1, sizeof(value) - 1);
            value[sizeof(value) - 1] = '\0';
            trim_in_place(value);

            /* 忽略空的 key 或没有 section 的配置项 */
            if (key[0] == '\0' || current_section[0] == '\0') {
                continue;
            }

            /* 分配并填充 entry */
            entry = (config_entry_t*)calloc(1, sizeof(config_entry_t));
            if (!entry) {
                fprintf(stderr, "[CONFIG] malloc failed at line %d\n", line_num);
                continue;  /* 内存不足，跳过此项，尽量多加载 */
            }

            make_full_key(entry->full_key, sizeof(entry->full_key),
                          current_section, key);
            strncpy(entry->value, value, sizeof(entry->value) - 1);

            /* 头插法加入链表 (O(1), 顺序不重要因为查找是遍历) */
            entry->next = cfg->entries;
            cfg->entries = entry;
            cfg->count++;

            continue;
        }

        /* 无法识别的行 (既不是 section 也不是 key=value 也不是注释) */
        /* 忽略它，不报错 (容错) */
    }

    fclose(fp);
    return 0;
}

void config_unload(config_t *cfg)
{
    config_entry_t *entry, *next;

    if (!cfg) return;

    entry = cfg->entries;
    while (entry) {
        next = entry->next;
        free(entry);
        entry = next;
    }

    cfg->entries   = NULL;
    cfg->count     = 0;
    cfg->file_path[0] = '\0';
}

int config_get_str(const config_t *cfg, const char *section, const char *key,
                   const char *default_val, char *out_buf, size_t buf_size)
{
    char full_key[CONFIG_KEY_MAX_LEN];
    config_entry_t *entry;

    if (!cfg || !section || !key || !out_buf || buf_size == 0) {
        return -1;
    }

    make_full_key(full_key, sizeof(full_key), section, key);
    entry = find_entry(cfg, full_key);

    if (entry) {
        strncpy(out_buf, entry->value, buf_size - 1);
        out_buf[buf_size - 1] = '\0';
        return 0;
    }

    /* 未找到: 使用默认值 */
    if (default_val) {
        strncpy(out_buf, default_val, buf_size - 1);
        out_buf[buf_size - 1] = '\0';
    } else {
        out_buf[0] = '\0';
    }
    return -1;
}

int config_get_int(const config_t *cfg, const char *section, const char *key,
                   int default_val)
{
    char value[CONFIG_VALUE_MAX_LEN];
    char *endptr;
    long result;

    /* 获取原始字符串值 */
    if (config_get_str(cfg, section, key, NULL, value, sizeof(value)) != 0) {
        return default_val;  /* 未找到, 返回默认值 */
    }

    /* strtol 解析整数:
     *   参数1: 字符串
     *   参数2: endptr (指向第一个非数字字符, 用于检测错误)
     *   参数3: 进制 (0=自动判断, 10=十进制, 16=十六进制)
     *   返回值: 解析结果
     *
     *   strtol("123abc", &end, 10) → result=123, *end='a'
     *   strtol("abc", &end, 10)    → result=0,   end==str (未解析任何字符)
     */
    result = strtol(value, &endptr, 0);

    /* 检查是否成功解析 (endptr 应该指向 '\0' 或者至少移动了) */
    if (endptr == value) {
        /* 完全无法解析 → 返回默认值 */
        return default_val;
    }

    return (int)result;
}

float config_get_float(const config_t *cfg, const char *section, const char *key,
                       float default_val)
{
    char value[CONFIG_VALUE_MAX_LEN];
    char *endptr;
    double result;

    if (config_get_str(cfg, section, key, NULL, value, sizeof(value)) != 0) {
        return default_val;
    }

    /* strtod 解析浮点数 (string to double)
     *   与 strtol 用法类似, endptr 用于检测错误
     *   支持科学计数法: "1.5e3" → 1500.0
     */
    result = strtod(value, &endptr);

    if (endptr == value) {
        return default_val;
    }

    return (float)result;
}

int config_get_bool(const config_t *cfg, const char *section, const char *key,
                    int default_val)
{
    char value[CONFIG_VALUE_MAX_LEN];

    if (config_get_str(cfg, section, key, NULL, value, sizeof(value)) != 0) {
        return default_val;
    }

    /* 不区分大小写比较:
     *   strcasecmp 是 POSIX 函数 (Linux/Unix 系统可用)
     *   Windows 上需要使用 _stricmp
     *   这里假设编译环境为 Linux (ARM 交叉编译) */
    if (strcasecmp(value, "1") == 0
        || strcasecmp(value, "true") == 0
        || strcasecmp(value, "yes") == 0
        || strcasecmp(value, "on") == 0) {
        return 1;
    }

    if (strcasecmp(value, "0") == 0
        || strcasecmp(value, "false") == 0
        || strcasecmp(value, "no") == 0
        || strcasecmp(value, "off") == 0) {
        return 0;
    }

    /* 无法识别的值 → 返回默认值 */
    return default_val;
}
