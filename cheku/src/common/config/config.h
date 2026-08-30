/**
 * @file    config.h
 * @brief   INI 格式配置文件解析器 - 头文件
 *
 * 支持标准的 INI 文件格式:
 *   - 节 (Section):  [section_name]
 *   - 键值对:         key = value
 *   - 注释:           ; 或 # 开头的行为注释
 *   - 空行:           自动忽略
 *
 * 配置项存储方式: 简单的键值对链表
 *   键 = "section.key" (用点号连接节名和键名)
 *   值 = 原始字符串
 *
 * 使用示例:
 *   config_t cfg;
 *   if (config_load(&cfg, "/etc/car_terminal/config.ini") != 0) {
 *       // 文件不存在或格式错误
 *   }
 *
 *   char device[64];
 *   config_get_str(&cfg, "gps.uart_device", "/dev/ttyUSB0", device, sizeof(device));
 *
 *   int baud = config_get_int(&cfg, "gps.baud_rate", 9600);
 *   float temp = config_get_float(&cfg, "sensor.temp_high", 50.0);
 *
 *   config_unload(&cfg);
 *
 * 设计决策: 为什么不使用 libconfig / json-c 等第三方库?
 *   1. 减少外部依赖 (符合项目约束)
 *   2. INI 格式足够简单，100行代码即可实现
 *   3. 学习字符串处理: strtok/strchr/strstr/trim 等 C 标准库函数
 *
 * 学习要点:
 *   1. 文本文件解析: 逐行读取 → 跳过空白/注释 → 提取键值
 *   2. 字符串处理: 去空格 (trim)、查找字符 (strchr/strstr)
 *   3. 类型转换: atoi/strtol, atof/strtod
 *   4. 链接法处理冲突 (链式哈希/简单链表的键值存储)
 */

#ifndef _CONFIG_H_
#define _CONFIG_H_

#include <stdint.h>
#include <stddef.h>    /* size_t */

/* 配置项最大长度 */
#define CONFIG_KEY_MAX_LEN      128    /* section.key 最大长度 */
#define CONFIG_VALUE_MAX_LEN    512    /* 值字符串最大长度 */
#define CONFIG_LINE_MAX_LEN     512    /* 文件每行最大长度 */

/**
 * @brief 键值对节点 (链表节点)
 *
 * 每个节点存储一个 "section.key = value" 对。
 * 例如: section="gps", key="baud_rate", value="9600"
 */
typedef struct config_entry {
    char    full_key[CONFIG_KEY_MAX_LEN];  /* "section.key" 组合键 */
    char    value[CONFIG_VALUE_MAX_LEN];   /* 值字符串 */
    struct config_entry *next;             /* 链表下一个节点 */
} config_entry_t;

/**
 * @brief 配置对象
 *
 * 持有一个配置项链表头，以及文件路径 (用于运行时重新加载)。
 */
typedef struct {
    config_entry_t *entries;    /* 配置项单向链表头 */
    int             count;      /* 配置项数量 (便于调试) */
    char            file_path[256]; /* 配置文件路径 */
} config_t;

/* ================================================================
 *  API 函数声明
 * ================================================================ */

/**
 * @brief 从文件加载配置
 *
 * @param cfg        未初始化的 config_t 指针 (函数内部分配资源)
 * @param file_path  配置文件路径
 * @return           0=成功, -1=文件打开失败, -2=格式错误
 *
 * 加载过程:
 *   1. 打开文件
 *   2. 逐行读取
 *   3. 跳过空行和注释 (; 或 # 开头)
 *   4. 遇到 [section] → 记录当前节名
 *   5. 遇到 key=value → 组合成 "section.key" 存入链表
 *   6. 关闭文件
 */
int config_load(config_t *cfg, const char *file_path);

/**
 * @brief 释放配置对象的内存
 * @param cfg  已加载的配置对象
 */
void config_unload(config_t *cfg);

/**
 * @brief 获取字符串配置值
 *
 * @param cfg          配置对象
 * @param section      节名 (如 "gps")
 * @param key          键名 (如 "uart_device")
 * @param default_val  默认值 (配置项不存在时返回)
 * @param out_buf      输出缓冲区
 * @param buf_size     缓冲区大小
 * @return             0=成功, -1=未找到 (已填入默认值)
 *
 * 使用示例:
 *   char device[64];
 *   config_get_str(&cfg, "gps", "uart_device", "/dev/ttyUSB0", device, 64);
 */
int config_get_str(const config_t *cfg, const char *section, const char *key,
                   const char *default_val, char *out_buf, size_t buf_size);

/**
 * @brief 获取整数配置值
 *
 * @param cfg          配置对象
 * @param section      节名
 * @param key          键名
 * @param default_val  默认值
 * @return             配置的整数值
 *
 * 使用 strtol (而非 atoi) 解析，可以检测非法输入。
 * atoi("abc") = 0 (无法区分"0"和"解析失败")
 * strtol("abc") → endptr 指向 "abc" (可以判断是否解析成功)
 */
int config_get_int(const config_t *cfg, const char *section, const char *key,
                   int default_val);

/**
 * @brief 获取浮点数配置值
 *
 * @param cfg          配置对象
 * @param section      节名
 * @param key          键名
 * @param default_val  默认值
 * @return             配置的浮点数值
 */
float config_get_float(const config_t *cfg, const char *section, const char *key,
                       float default_val);

/**
 * @brief 获取布尔配置值
 *
 * 支持的真值: "1", "true", "yes", "on" (不区分大小写)
 * 支持的假值: "0", "false", "no", "off" (不区分大小写)
 *
 * @param cfg          配置对象
 * @param section      节名
 * @param key          键名
 * @param default_val  默认值 (0 或 1)
 * @return             1=true, 0=false
 */
int config_get_bool(const config_t *cfg, const char *section, const char *key,
                    int default_val);

#endif /* _CONFIG_H_ */
