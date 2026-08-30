/**
 * @file    nmea_parser.c
 * @brief   NMEA-0183 协议解析器 - 实现 (字符级状态机)
 *
 * ================================================================
 *  设计选择: 为什么用状态机而不是 sscanf?
 * ================================================================
 *
 * sscanf 方式 (常见于 Arduino/简单 GPS 库):
 *   sscanf(sentence, "$GPGGA,%f,%f,%c,%f,%c,%d,%d,%f,%f,M,...", ...);
 *   优点: 代码短
 *   缺点: 格式错误时行为不可预测; 字段为空时解析中断; 无校验和验证
 *
 * 状态机方式 (本实现):
 *   逐字符处理, 每步状态转换明确:
 *     - 遇到非 '$' → 跳过 (噪声)
 *     - '$' → 进入 TALKER 状态
 *     - GP → 进入 TYPE 状态
 *     - GGA → 进入 FIELD 状态
 *     - ',' → 字段+1
 *     - '*' → 进入 CHECKSUM 状态
 *     - CR+LF → 完成, 触发回调
 *
 *   优点: 稳健 (对格式错误容错); 可校验; 不跳字段
 *
 * ================================================================
 *  校验和算法
 * ================================================================
 *
 *   NMEA 校验和 = 所有在 '$' 和 '*' 之间字符的 XOR
 *   (不含 '$' 和 '*' 本身)
 *
 *   示例: $GPGGA,...*5A
 *         计算范围: GPGGA,... 的逐字节 XOR
 *         G^P^G^G^A^,^... = 0x5A
 *
 *   校验: 接收到的2位HEX值(字符串"5A") → 转整数 → 与计算结果比较
 *
 * 学习要点:
 *   1. 字符级状态机: 完整的协议解析器设计范例
 *   2. XOR 校验和: 最简单的数据完整性校验
 *   3. 容错设计: 超时重置 / 缓冲区溢出保护 / 非法字符跳过
 *   4. 度分转换: NMEA ddmm.mmmm → 十进制度数
 */

#include "nmea_parser.h"
#include "log/log.h"

#include <stdio.h>      /* NULL */
#include <stdlib.h>     /* strtod, strtol */
#include <string.h>     /* memcpy, strncmp, strchr, memset */
#include <ctype.h>      /* isdigit, isxdigit */
#include <math.h>       /* NAN */

/* ================================================================
 *  内部辅助函数
 * ================================================================ */

/**
 * @brief 将 NMEA 度分字符串 (如 "3912.3456") 转换为十进制度。
 *
 * 解析算法:
 *   1. strtod() 将字符串转换为 double (raw = 3912.3456)。
 *   2. (raw / 100) 的整数部分给出度数 (39)。
 *   3. raw - (度数 * 100) 给出分数 (12.3456)。
 *   4. 结果 = 度数 + 分数 / 60.0 = 39.205760。
 *
 * NMEA 纬度格式:  ddmm.mmmm  (2位整数度数 + 分数)。
 * NMEA 经度格式: dddmm.mmmm (3位整数度数 + 分数)。
 * 此函数同时处理两者，因为 /100 拆分适用于任意数量的前导度数字。
 *
 * @param nmea_str  以 null 结尾的 NMEA 坐标字符串 (如 "3912.3456")。
 * @return          十进制度 (double)，字符串为 NULL、空字符串或
 *                  不包含有效数字前缀时返回 NAN。
 */
static double parse_nmea_degrees(const char *nmea_str)
{
    double raw, degrees, minutes;
    int int_deg;
    char *endptr;

    if (!nmea_str || !nmea_str[0]) return NAN;

    /* strtod: 字符串 → double, endptr 指向第一个非数字字符 */
    raw = strtod(nmea_str, &endptr);
    if (endptr == nmea_str) return NAN;

    /* 分离度和分:
     * 纬度: ddmm.mmmm / 100 → dd (整数部分拿掉后, ddmm.mmmm mod 100 = mm.mmmm)
     * 经度: dddmm.mmmm / 100 → ddd (同理) */
    int_deg  = (int)(raw / 100.0);
    minutes  = raw - (double)(int_deg * 100);
    degrees  = (double)int_deg + minutes / 60.0;

    return degrees;
}

/**
 * @brief 解析 NMEA hhmmss.ss 格式的 UTC 时间字符串。
 *
 * 示例: "123519.00" 解析为 hour=12, min=35, sec=19。
 * 秒的小数部分 (.ss) 通过将原始 double 乘以 100 并取模 100 余数来提取。
 *
 * @param str    NMEA 时间字符串 ("hhmmss.ss")，可为 NULL 或空。
 * @param hour   输出: 小时 (0–23)。str 为空时设为 0。
 * @param min    输出: 分钟 (0–59)。str 为空时设为 0。
 * @param sec    输出: 秒 (0–59)。str 为空时设为 0。
 */
static void parse_utc_time(const char *str,
                           uint8_t *hour, uint8_t *min, uint8_t *sec)
{
    double raw;

    if (!str || !str[0]) {
        *hour = *min = *sec = 0;
        return;
    }

    raw = strtod(str, NULL);
    int t = (int)raw;

    *hour = (uint8_t)(t / 10000);
    *min  = (uint8_t)((t % 10000) / 100);
    *sec  = (uint8_t)((int)(raw * 100.0) % 100);  /* 小数部分 → 秒 */
}

/**
 * @brief 解析 NMEA ddmmyy 格式的 UTC 日期字符串。
 *
 * 示例: "010826" 解析为 day=1, month=8, year=2026。
 * 两位数年 (26) 映射到 2000+year (2026) — 此简单映射
 * 适用于 2000–2099 年，不处理 2000 年之前的日期。
 *
 * @param str    NMEA 日期字符串 ("ddmmyy")，可为 NULL 或空。
 * @param year   输出: 4位年份 (如 2026)。str 为空时设为 0。
 * @param month  输出: 月份 (1–12)。str 为空时设为 0。
 * @param day    输出: 日 (1–31)。str 为空时设为 0。
 */
static void parse_utc_date(const char *str,
                           uint16_t *year, uint8_t *month, uint8_t *day)
{
    long raw;

    if (!str || !str[0]) {
        *year = 0; *month = 0; *day = 0;
        return;
    }

    raw = strtol(str, NULL, 10);

    *day   = (uint8_t)(raw / 10000);
    *month = (uint8_t)((raw % 10000) / 100);
    *year  = (uint16_t)((raw % 100) + 2000);  /* 26 → 2026 */
}

/**
 * @brief 解析以节为单位的速度值并转换为 km/h。
 *
 * 1 节 = 1 海里每小时 = 1.852 km/h (国际标准精确值)。
 * 输入字符串可为空 (返回 NAN) 或任何有效的浮点数表示。
 * 负值被视为无效。
 *
 * @param str  以节为单位的速度字符串 (如 "15.5")，可为 NULL 或空。
 * @return     速度 (km/h, float)，字符串为 NULL、空或包含负值时返回 NAN。
 */
static float parse_speed_knots(const char *str)
{
    double knots;

    if (!str || !str[0]) return NAN;

    knots = strtod(str, NULL);
    if (knots < 0.0) return NAN;

    /* 1 节 = 1.852 km/h */
    return (float)(knots * 1.852);
}

/**
 * @brief 将 2 字符大写十六进制字符串转换为 8 位整数。
 *
 * 用于解码 NMEA 校验和字段 (如 "*5A" → 0x5A)。
 * 同时接受大写和小写十六进制数字。如果任一字符不是有效的十六进制数字，
 * 则返回 -1。
 *
 * @param hex  指向恰好 2 个十六进制字符的指针 (一般情况不以 null 结尾，
 *             但通常后跟 CR/LF)。
 * @return     整数值 [0, 255]，任一字符无效时返回 -1。
 */
static int hex_to_byte(const char hex[2])
{
    int val = 0;
    int i;

    for (i = 0; i < 2; i++) {
        char ch = hex[i];
        val <<= 4;
        if (ch >= '0' && ch <= '9')      val |= (ch - '0');
        else if (ch >= 'A' && ch <= 'F') val |= (ch - 'A' + 10);
        else if (ch >= 'a' && ch <= 'f') val |= (ch - 'a' + 10);
        else return -1;  /* Non-hex character in checksum field */
    }

    return val;
}

/**
 * @brief 从 3 字符类型代码识别 NMEA 语句类型。
 *
 * 将 3 字符类型字符串 (如 "GGA", "RMC") 与已知的 GPS 语句集进行比较。
 * Talker ID 前缀 (如 "GP") 不在此处检查 — 它已由解析器状态机提取。
 *
 * @param type_str  指向恰好 3 个字符的指针 (如 "GGA")。
 * @return          对应的 nmea_type_t 枚举值，类型未识别时返回
 *                  NMEA_UNKNOWN。
 */
static nmea_type_t identify_sentence_type(const char *type_str)
{
    if (!type_str) return NMEA_UNKNOWN;

    if (memcmp(type_str, "GGA", 3) == 0) return NMEA_GPGGA;
    if (memcmp(type_str, "RMC", 3) == 0) return NMEA_GPRMC;
    if (memcmp(type_str, "GSV", 3) == 0) return NMEA_GPGSV;
    if (memcmp(type_str, "GSA", 3) == 0) return NMEA_GPGSA;
    if (memcmp(type_str, "GLL", 3) == 0) return NMEA_GPGLL;
    if (memcmp(type_str, "VTG", 3) == 0) return NMEA_GPVTG;

    return NMEA_UNKNOWN;
}

/* ================================================================
 *  API implementation
 * ================================================================ */

/**
 * @brief 将 NMEA 解析器实例初始化为其起始状态。
 *
 * 将整个结构体清零并设置 state = NMEA_STATE_IDLE。
 * 可安全地在未初始化或之前使用过的解析器上调用。
 * parser 为 NULL 时立即返回。
 *
 * @param parser  要初始化的解析器 (可为 NULL)。
 */
void nmea_parser_init(nmea_parser_t *parser)
{
    if (!parser) return;

    memset(parser, 0, sizeof(*parser));
    parser->state = NMEA_STATE_IDLE;
}

/**
 * @brief 在流中重置解析器 — 丢弃任何部分组装完成的语句。
 *
 * 清除状态、缓冲区游标、字段指针、运行中的校验和以及进行中的结果结构体。
 * 统计计数器 (total_sentences, checksum_errors, unknown_types) 在重置时
 * 保留，以便诊断数据继续累积。
 *
 * @param parser  要重置的解析器 (可为 NULL — 调用为空操作)。
 */
void nmea_parser_reset(nmea_parser_t *parser)
{
    if (!parser) return;

    parser->state       = NMEA_STATE_IDLE;
    parser->buf_pos     = 0;
    parser->field_start = 0;
    parser->field_idx   = 0;
    parser->checksum    = 0;
    parser->checksum_pos = 0;

    memset(&parser->result, 0, sizeof(parser->result));
    memset(parser->buf, 0, sizeof(parser->buf));
    memset(parser->checksum_hex, 0, sizeof(parser->checksum_hex));
}

int nmea_parser_feed(nmea_parser_t *parser, char ch)
{
    if (!parser) return 0;

    switch (parser->state) {

    /* ==========================================
     *  IDLE: 等待 '$' (帧起始)
     *  跳过所有非 '$' 字符 (噪声/残留数据)
     * ========================================== */
    case NMEA_STATE_IDLE:
        if (ch == '$') {
            /* 开始一条新语句: 重置所有内部状态 */
            nmea_parser_reset(parser);
            parser->state    = NMEA_STATE_TALKER;
            /* 重置后 buf_pos 回到 0, field_start 回到 0 */
        }
        /* 非 '$' 字符: 继续留在 IDLE, 等下一字节 */
        break;

    /* ==========================================
     *  TALKER: 读 Talker ID (2 字符, GP/GL/GN)
     * ========================================== */
    case NMEA_STATE_TALKER:
        /* 读 Talker ID 第1+2字符 */
        if (parser->buf_pos < 2) {
            parser->buf[parser->buf_pos++] = ch;
        } else {
            /* 第3字符 → 进入 TYPE 状态 */
            parser->buf[parser->buf_pos++] = ch;
            parser->state = NMEA_STATE_TYPE;
        }
        /* 开始计算校验和 (从 '$' 后的第1字节开始) */
        parser->checksum ^= (uint8_t)ch;
        break;

    /* ==========================================
     *  TYPE: 读语句类型 (3 字符, GGA/RMC/GSV/...)
     * ========================================== */
    case NMEA_STATE_TYPE:
        if (parser->buf_pos < 5) {
            /* 还在读 TYPE 的第2/3字节 */
            parser->buf[parser->buf_pos++] = ch;
        } else {
            /* TYPE 已读完 (共5字节: GP+GGA), 进入字段解析 */
            /* 提取 Talker ID 和 语句类型 到 result */
            memcpy(parser->result.talker, parser->buf, 2);
            parser->result.talker[2] = '\0';
            memcpy(parser->result.sentence, parser->buf + 2, 3);
            parser->result.sentence[3] = '\0';

            parser->result.type = identify_sentence_type(
                parser->result.sentence);

            /* 初始化第一个字段 */
            parser->state       = NMEA_STATE_FIELD;
            parser->field_idx   = 0;
            parser->field_start = parser->buf_pos; /* 字段起始位置 */

            /* 把当前字符存入 buf (作为字段的第一字节) */
            parser->buf[parser->buf_pos++] = ch;
        }
        parser->checksum ^= (uint8_t)ch;
        break;

    /* ==========================================
     *  FIELD: 读字段值 (在 ',' 或 '*' 处结束)
     * ========================================== */
    case NMEA_STATE_FIELD:
        if (ch == ',') {
            /* 字段分隔符: 结束当前字段, 开始下一个 */
            parser->buf[parser->buf_pos++] = '\0'; /* 字段结束 */

            /* 存储字段指针 */
            if (parser->field_idx < NMEA_MAX_FIELDS) {
                parser->result.fields[parser->field_idx] =
                    parser->buf + parser->field_start;
                parser->result.field_count = parser->field_idx + 1;
            }

            parser->field_idx++;
            parser->field_start = parser->buf_pos;  /* 下一字段起始 */
            parser->checksum   ^= (uint8_t)ch;       /* ',' 也参与校验 */
        }
        else if (ch == '*') {
            /* 校验和分隔符: 结束当前字段, 进入校验和 */
            parser->buf[parser->buf_pos++] = '\0';

            if (parser->field_idx < NMEA_MAX_FIELDS) {
                parser->result.fields[parser->field_idx] =
                    parser->buf + parser->field_start;
                parser->result.field_count = parser->field_idx + 1;
            }

            /* 进入校验和状态 (* 不参与校验和计算) */
            parser->state        = NMEA_STATE_CHECKSUM;
            parser->checksum_pos = 0;
            memset(parser->checksum_hex, 0, sizeof(parser->checksum_hex));
        }
        else if (ch == '\r' || ch == '\n') {
            /* 部分 GPS 模块不发送校验和 (简化版 NMEA)
             * 直接视为语句结束 (容错) */
            parser->buf[parser->buf_pos++] = '\0';
            if (parser->field_idx < NMEA_MAX_FIELDS) {
                parser->result.fields[parser->field_idx] =
                    parser->buf + parser->field_start;
                parser->result.field_count = parser->field_idx + 1;
            }

            /* 计算出的校验和 = 运行的 XOR 值 */
            parser->result.calc_checksum = parser->checksum;
            parser->result.raw_checksum  = 0;
            parser->result.checksum_ok   = 1; /* 无校验和 → 默认通过 */
            parser->total_sentences++;

            /* 跳过后续 CR/LF */
            parser->state = (ch == '\r') ? NMEA_STATE_CR : NMEA_STATE_IDLE;
            if (ch == '\n') parser->state = NMEA_STATE_IDLE;

            return (parser->result.type != NMEA_UNKNOWN) ? 1 : 0;
        }
        else {
            /* 普通字段字符: 写入 buf */
            /* 缓冲区溢出保护 (buf 大小 = NMEA_MAX_FIELDS * 32) */
            if (parser->buf_pos < (int)(sizeof(parser->buf) - 2)) {
                parser->buf[parser->buf_pos++] = ch;
            }
            parser->checksum ^= (uint8_t)ch;
        }
        break;

    /* ==========================================
     *  CHECKSUM: 读2位 HEX 校验和
     * ========================================== */
    case NMEA_STATE_CHECKSUM:
        if (parser->checksum_pos < 2) {
            parser->checksum_hex[parser->checksum_pos++] = ch;
        }

        if (parser->checksum_pos >= 2) {
            /* 校验和已完整: 验证 */
            parser->result.raw_checksum = (uint8_t)hex_to_byte(
                parser->checksum_hex);
            parser->result.calc_checksum = parser->checksum;
            parser->result.checksum_ok =
                (parser->result.raw_checksum == parser->result.calc_checksum)
                ? 1 : 0;

            if (!parser->result.checksum_ok) {
                parser->checksum_errors++;
            }
            parser->total_sentences++;

            /* 进入等待 CR 状态 */
            parser->state = NMEA_STATE_CR;
        }
        break;

    /* ==========================================
     *  CR: 等待 LF (语句结束)
     * ========================================== */
    case NMEA_STATE_CR:
        if (ch == '\n') {
            /* CR+LF 完整 → 语句解析完成! */
            parser->state = NMEA_STATE_IDLE;

            if (parser->result.checksum_ok) {
                return (parser->result.type != NMEA_UNKNOWN) ? 1 : 0;
            } else {
                return -1;  /* 校验和不匹配 */
            }
        } else if (ch != '\r') {
            /* 意外字符 (某些模块可能不严格按照 CRLF):
             * 仍然视为完成 (容错) */
            parser->state = NMEA_STATE_IDLE;
            return (parser->result.type != NMEA_UNKNOWN
                    && parser->result.checksum_ok) ? 1 : -1;
        }
        break;
    }

    return 0;  /* 需要更多字符 */
}

/**
 * @brief 检查解析器是否已完成一条语句。
 *
 * 注意: 当前实现始终返回 0。调用者应通过 nmea_parser_feed() 的返回值
 * 来检测语句完成 (1 = 有效语句, -1 = 校验和错误)。
 * 此函数存在是为了 API 对称性，未来可能会增强。
 *
 * @param parser  解析器实例 (可为 NULL)。
 * @return        当前实现中始终返回 0。
 */
int nmea_parser_done(const nmea_parser_t *parser)
{
    if (!parser) return 0;
    return (parser->state == NMEA_STATE_IDLE) ? 0 : 0;
    /* 调用者通过 nmea_parser_feed() 返回值检测完成 */
}

/* ================================================================
 *  Data extraction: populate gps_data_t from parsed NMEA fields
 * ================================================================ */

/**
 * @brief 将解析完成的 NMEA 语句分发到对应类型的字段提取器。
 *
 * 这是顶层路由函数: 检查 parsed->type 并调用相应的 nmea_extract_*() 函数。
 * 因为每种 NMEA 语句类型携带不同的 GPS 数据子集，提取器只写入它们提供的字段 —
 * gps_data 中的其他字段保留其之前的值。这允许来自多种语句类型
 * (GGA + RMC + GSV) 的数据累积到一个连贯的定位结果中。
 *
 * @param parsed     指向已完成的 nmea_parsed_t。
 * @param gps_data   输出结构体 (根据语句类型选择性更新)。
 * @return           成功返回 0，parsed 或 gps_data 为 NULL 或
 *                   语句类型为 NMEA_UNKNOWN 时返回 -1。
 */
int nmea_extract_data(const nmea_parsed_t *parsed, gps_data_t *gps_data)
{
    if (!parsed || !gps_data) return -1;

    switch (parsed->type) {
    case NMEA_GPGGA: return nmea_extract_gga(parsed, gps_data);
    case NMEA_GPRMC: return nmea_extract_rmc(parsed, gps_data);
    case NMEA_GPGSV: return nmea_extract_gsv(parsed, gps_data);
    case NMEA_GPGLL: return nmea_extract_gll(parsed, gps_data);
    case NMEA_GPVTG: return nmea_extract_vtg(parsed, gps_data);
    case NMEA_GPGSA: return nmea_extract_gsa(parsed, gps_data);
    default:         return -1;  /* 未知类型 */
    }
}

/**
 * @brief 从 $GPGGA 语句提取 GPS 定位数据。
 *
 * GGA 是位置和定位质量的主要语句。字段布局:
 *   [0] = UTC 时间 (hhmmss.ss)
 *   [1] = 纬度  (ddmm.mmmm)
 *   [2] = 北/南指示 ('N' 或 'S')
 *   [3] = 经度 (dddmm.mmmm)
 *   [4] = 东/西指示 ('E' 或 'W')
 *   [5] = 定位质量 (0 = 无效, 1 = GPS, 2 = DGPS)
 *   [6] = 使用中的卫星数
 *   [7] = 水平精度因子 (HDOP)
 *   [8] = 大地水准面以上高度 (米)
 *   [9] = 'M' (高度单位)
 *
 * 至少需要 6 个字段。如果缺少北/南或东/西指示，
 * 对应的坐标不会被更新。
 *
 * @param parsed     解析完成的 GGA 语句。
 * @param gps_data   输出: 时间、纬度、经度、fix_quality、
 *                   satellites、hdop、altitude。
 * @return           成功返回 0，field_count < 6 时返回 -1。
 */
int nmea_extract_gga(const nmea_parsed_t *parsed, gps_data_t *gps_data)
{
    if (parsed->field_count < 6) return -1;

    /* 时间 */
    if (parsed->fields[0] && parsed->fields[0][0]) {
        parse_utc_time(parsed->fields[0],
                       &gps_data->hour, &gps_data->min, &gps_data->sec);
    }

    /* 纬度 */
    if (parsed->fields[1] && parsed->fields[2] && parsed->fields[2][0]) {
        double lat = parse_nmea_degrees(parsed->fields[1]);
        if (!isnan(lat)) {
            gps_data->latitude = (parsed->fields[2][0] == 'S') ? -lat : lat;
        }
    }

    /* 经度 */
    if (parsed->fields[3] && parsed->fields[4] && parsed->fields[4][0]) {
        double lon = parse_nmea_degrees(parsed->fields[3]);
        if (!isnan(lon)) {
            gps_data->longitude = (parsed->fields[4][0] == 'W') ? -lon : lon;
        }
    }

    /* 定位质量: 0=无效 1=GPS 2=DGPS */
    if (parsed->fields[5] && parsed->fields[5][0]) {
        gps_data->fix_quality = (uint8_t)strtol(parsed->fields[5], NULL, 10);
    }

    /* 卫星数 */
    if (parsed->fields[6] && parsed->fields[6][0]) {
        gps_data->satellites = (uint8_t)strtol(parsed->fields[6], NULL, 10);
    }

    /* HDOP */
    if (parsed->fields[7] && parsed->fields[7][0]) {
        gps_data->hdop = (float)strtod(parsed->fields[7], NULL);
    }

    /* 高度 */
    if (parsed->fields[8] && parsed->fields[8][0]) {
        gps_data->altitude = (float)strtod(parsed->fields[8], NULL);
    }

    return 0;
}

/**
 * @brief 从 $GPRMC 语句提取导航数据。
 *
 * RMC (推荐最小定位信息) 是最常用的导航 NMEA 语句。字段布局:
 *   [0]  = UTC 时间 (hhmmss.ss)
 *   [1]  = 状态 ('A' = 有效, 'V' = 无效/导航接收器警告)
 *   [2]  = 纬度  (ddmm.mmmm)
 *   [3]  = 北/南指示
 *   [4]  = 经度 (dddmm.mmmm)
 *   [5]  = 东/西指示
 *   [6]  = 地面速度 (节)
 *   [7]  = 地面航向 (真北度数)
 *   [8]  = 日期 (ddmmyy)
 *   [9]  = 磁偏角 (度)
 *   [10] = 模式指示 (A/D/E/N)
 *
 * 如果状态为 'V' (无效)，不提取位置或速度数据 — 仅将 fix_quality 设为 0。
 * 节为单位的速度在内部转换为 km/h。如果有日期字段，则更新年/月/日。
 */
int nmea_extract_rmc(const nmea_parsed_t *parsed, gps_data_t *gps_data)
{

    if (parsed->field_count < 9) return -1;

    /* 状态: A=有效 V=无效 */
    if (parsed->fields[1] && parsed->fields[1][0] == 'V') {
        /* V = 数据无效, 不更新定位 */
        gps_data->fix_quality = 0;
        return 0;
    }

    /* 时间 */
    if (parsed->fields[0] && parsed->fields[0][0]) {
        parse_utc_time(parsed->fields[0],
                       &gps_data->hour, &gps_data->min, &gps_data->sec);
    }

    /* 纬度 */
    if (parsed->fields[2] && parsed->fields[3] && parsed->fields[3][0]) {
        double lat = parse_nmea_degrees(parsed->fields[2]);
        if (!isnan(lat)) {
            gps_data->latitude = (parsed->fields[3][0] == 'S') ? -lat : lat;
        }
    }

    /* 经度 */
    if (parsed->fields[4] && parsed->fields[5] && parsed->fields[5][0]) {
        double lon = parse_nmea_degrees(parsed->fields[4]);
        if (!isnan(lon)) {
            gps_data->longitude = (parsed->fields[5][0] == 'W') ? -lon : lon;
        }
    }

    /* 速度 (节 → km/h) */
    if (parsed->fields[6] && parsed->fields[6][0]) {
        float spd = parse_speed_knots(parsed->fields[6]);
        if (!isnan(spd)) {
            gps_data->speed = spd;
        }
    }

    /* 日期 */
    if (parsed->fields[8] && parsed->fields[8][0]) {
        parse_utc_date(parsed->fields[8],
                       &gps_data->year, &gps_data->month, &gps_data->day);
    }

    /* 定位质量 (来自 GGA, RMC 不提供 fix_quality
     * 但如果之前没有 GGA 数据, 根据 RMC 状态推算) */
    if (gps_data->fix_quality == 0 && gps_data->latitude != 0.0) {
        gps_data->fix_quality = 1;  /* 有位置 → 至少 GPS fix */
    }

    return 0;
}

/**
 * @brief 从 $GPGSV 语句提取可见卫星数量。
 *
 * GSV 语句报告接收器当前可见的卫星信息。
 * 多条 GSV 语句可能按顺序发送 (最多 6 条) 以覆盖所有卫星，
 * 每条携带最多 4 个卫星条目。
 *
 * 字段布局:
 *   [0] = 本周期 GSV 语句总数
 *   [1] = 当前语句编号 (从1开始)
 *   [2] = 可见卫星总数 (所有星座合计)
 *   [3..] = 卫星条目 (每组4字段: PRN, 仰角, 方位角, SNR)
 *
 * 此提取器仅更新 gps_data->satellites — 单个卫星条目
 * (仰角、方位角、SNR) 在当前实现中不持久保存。
 * 卫星计数仅在解析值大于当前值时更新
 * (取同一周期所有 GSV 语句中的最大值)。
 */
int nmea_extract_gsv(const nmea_parsed_t *parsed, gps_data_t *gps_data)
{

    if (parsed->field_count < 3) return -1;

    /* 可见卫星总数 (来自每条 GSV 语句) */
    if (parsed->fields[2] && parsed->fields[2][0]) {
        uint8_t total_sats = (uint8_t)strtol(parsed->fields[2], NULL, 10);
        /* 使用 GSV 中的卫星数 (通常是所有 Talker 中最大的) */
        if (total_sats > gps_data->satellites) {
            gps_data->satellites = total_sats;
        }
    }

    return 0;
}

/**
 * @brief 从 $GPGLL 语句提取地理位置。
 *
 * GLL 提供纬度/经度，不含高度或 DOP 数据。
 * 字段布局:
 *   [0] = 纬度  (ddmm.mmmm)
 *   [1] = 北/南指示
 *   [2] = 经度 (dddmm.mmmm)
 *   [3] = 东/西指示
 *   [4] = UTC 时间 (hhmmss.ss)
 *   [5] = 状态 ('A' = 有效, 'V' = 无效)
 *   [6] = 模式指示 (A = 自主定位, D = 差分定位, E = 估算定位,
 *         N = 数据无效)
 *
 * 示例: $GPGLL,4005.22599,N,11632.58234,E,082559.00,A,A*7A
 *
 * 如果状态为 'V' (无效)，fix_quality 设为 0 且不提取位置数据。
 */
int nmea_extract_gll(const nmea_parsed_t *parsed, gps_data_t *gps_data)
{

    if (parsed->field_count < 5) return -1;

    /* 状态检查: V=无效数据 */
    if (parsed->fields[5] && parsed->fields[5][0] == 'V') {
        gps_data->fix_quality = 0;
        return 0;
    }

    /* 时间 */
    if (parsed->fields[4] && parsed->fields[4][0]) {
        parse_utc_time(parsed->fields[4],
                       &gps_data->hour, &gps_data->min, &gps_data->sec);
    }

    /* 纬度 */
    if (parsed->fields[0] && parsed->fields[1] && parsed->fields[1][0]) {
        double lat = parse_nmea_degrees(parsed->fields[0]);
        if (!isnan(lat)) {
            gps_data->latitude = (parsed->fields[1][0] == 'S') ? -lat : lat;
        }
    }

    /* 经度 */
    if (parsed->fields[2] && parsed->fields[3] && parsed->fields[3][0]) {
        double lon = parse_nmea_degrees(parsed->fields[2]);
        if (!isnan(lon)) {
            gps_data->longitude = (parsed->fields[3][0] == 'W') ? -lon : lon;
        }
    }

    return 0;
}

/**
 * @brief 从 $GPVTG 语句提取地面速度。
 *
 * VTG 提供地面航向和地面速度信息。
 * 字段布局:
 *   [0] = 地面航向，真北度数
 *   [1] = 'T' (真北航向指示)
 *   [2] = 地面航向，磁北度数
 *   [3] = 'M' (磁北航向指示)
 *   [4] = 地面速度，节
 *   [5] = 'N' (节指示)
 *   [6] = 地面速度，km/h
 *   [7] = 'K' (km/h 指示)
 *   [8] = 模式指示 (A/D/E/N)
 *
 * 示例: $GPVTG,180.0,T,185.0,M,15.5,N,28.7,K,A*3F
 *
 * 速度提取优先级:
 *   1. 字段 [6] (km/h) — 存在时直接使用。
 *   2. 字段 [4] (节) — 作为后备转换为 km/h (1 节 = 1.852 km/h)。
 * 相比 RMC 速度优先使用此值，因为 VTG 可能已经报告 km/h。
 */
int nmea_extract_vtg(const nmea_parsed_t *parsed, gps_data_t *gps_data)
{

    if (parsed->field_count < 7) return -1;

    /* 速度 (km/h) — 优先使用 VTG 自己提供的 km/h */
    if (parsed->fields[6] && parsed->fields[6][0]) {
        double speed_kmh = strtod(parsed->fields[6], NULL);
        if (speed_kmh >= 0.0) {
            gps_data->speed = (float)speed_kmh;
        }
    }
    /* 如果 km/h 为空, 回退到节转换 */
    else if (parsed->fields[4] && parsed->fields[4][0]) {
        gps_data->speed = parse_speed_knots(parsed->fields[4]);
    }

    return 0;
}

/**
 * @brief 从 $GPGSA 语句提取 DOP (精度因子) 和定位类型。
 *
 * GSA 报告卫星几何质量以及位置解算中正在使用哪些卫星。
 *
 * 字段布局:
 *   [0]  = 操作模式 ('M' = 手动, 'A' = 自动/2D 或 3D)
 *   [1]  = 定位类型 (1 = 无定位, 2 = 2D, 3 = 3D)
 *   [2..13] = 解算中使用的卫星 PRN 号 (最多 12 个)
 *   [14] = PDOP (位置精度因子)
 *   [15] = HDOP (水平精度因子)
 *   [16] = VDOP (垂直精度因子)
 *
 * 示例: $GPGSA,A,3,01,02,03,04,05,06,,,,,,,1.5,0.9,1.2*3E
 *
 * 此提取器从字段 [1] 更新 fix_quality，从字段 [15] 更新 HDOP。
 * HDOP 值后续可能被 GSA 之后到达的 GGA 语句覆盖。
 */
int nmea_extract_gsa(const nmea_parsed_t *parsed, gps_data_t *gps_data)
{

    if (parsed->field_count < 15) return -1;

    /* 定位类型: 1=无定位, 2=2D, 3=3D */
    if (parsed->fields[1] && parsed->fields[1][0]) {
        int fix_type = (int)strtol(parsed->fields[1], NULL, 10);
        gps_data->fix_quality = (uint8_t)fix_type;
    }

    /* HDOP (水平精度因子) — 也出现在 GGA 中, GSA 的值作为补充 */
    if (parsed->fields[14] && parsed->fields[14][0]) {
        float hdop = (float)strtod(parsed->fields[14], NULL);
        if (hdop > 0.0f) {
            gps_data->hdop = hdop;
        }
    }

    return 0;
}
