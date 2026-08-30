/**
 * @file    nmea_parser.h
 * @brief   NMEA-0183 协议解析器 - 头文件
 *
 * 采用字符级状态机实现 NMEA 语句解析。
 * 不使用 sscanf —— 手动逐字符处理，状态驱动。
 *
 * NMEA-0183 语句格式:
 *
 *   $TTSSS,field1,field2,...,fieldN*CC\r\n
 *   │ │  │ │                     │  │  │
 *   │ │  │ │                     │  │  └── LF (0x0A)
 *   │ │  │ │                     │  └───── CR (0x0D)
 *   │ │  │ │                     └──────── 校验和 (2字节HEX)
 *   │ │  │ └────────────────────────────── 语句类型 (GGA/RMC/GSV/...)
 *   │ │  └──────────────────────────────── 1-2个字母 (G=GPS, L=GLONASS, N=Multi)
 *   │ └─────────────────────────────────── Talker ID 第1字母
 *   └───────────────────────────────────── 起始符 '$'
 *
 *   校验和 = 所有在 '$' 和 '*' 之间字节的 XOR (不含 $ 和 *)
 *
 * GGA 语句字段:
 *   $GPGGA,hhmmss.ss,ddmm.mmmm,N,dddmm.mmmm,W,q,nn,hdop,alt,M,geoid,M,,*CC
 *   [0]    [1]        [2]        [3][4]        [5][6][7][8]  [9] [10]  [11]
 *
 * RMC 语句字段:
 *   $GPRMC,hhmmss.ss,A,ddmm.mmmm,N,dddmm.mmmm,W,speed,course,ddmmyy,,,mode*CC
 *   [0]    [1]        [2][3]        [4][5]        [6]   [7]    [8]
 *
 * GSV 语句字段:
 *   $GPGSV,nMsg,msgNum,nSats,sat1,elev1,az1,snr1,...*CC
 *   [0]    [1]  [2]    [3]    [4..] (每组4字段: PRN/仰角/方位角/SNR)
 *
 * 设计要点:
 *   - 状态机逐字符处理, 适合流式串口数据
 *   - 环形缓冲区配合: ringbuffer 缓存原始字节, 解析器逐字符消费
 *   - 无效语句 (校验和不匹配) 丢弃并计数
 *   - 支持 GP/GL/GN 等多种 Talker ID
 */

#ifndef _NMEA_PARSER_H_
#define _NMEA_PARSER_H_

#include "gps_daemon.h"
#include <stdint.h>

/* NMEA 语句最大字段数 */
#define NMEA_MAX_FIELDS     32

/**
 * @brief NMEA 解析器状态机状态。
 *
 * 解析器在扫描传入 NMEA 语句的每个字符时在这些状态之间转换:
 *
 *   IDLE → ( '$' ) → TALKER → (2 字符) → TYPE → (3 字符) → FIELD
 *     → ( ',' ) → FIELD (下一字段)
 *     → ( '*' ) → CHECKSUM → (2 个十六进制字符) → CR → ( '\n' ) → IDLE
 *
 * 任何意外字符会导致回退到 IDLE (对串口链路上的线路噪声具有弹性)。
 */
typedef enum {
    NMEA_STATE_IDLE     = 0,    /* 等待 '$' (帧起始定界符) */
    NMEA_STATE_TALKER   = 1,    /* 读取 Talker ID (2 字符: GP, GL, GN 等) */
    NMEA_STATE_TYPE     = 2,    /* 读取语句类型 (3 字符: GGA, RMC, GSV, ...) */
    NMEA_STATE_FIELD    = 3,    /* 读取字段值 (以 ',' 或 '*' 终止) */
    NMEA_STATE_CHECKSUM = 4,    /* 读取 '*' 后的 2 字节十六进制校验和 */
    NMEA_STATE_CR       = 5,    /* 已收到 CR; 等待 LF 完成语句 */
} nmea_parser_state_t;

/**
 * @brief 解析一条完整 NMEA 语句的结果。
 *
 * fields[] 数组包含指向解析器内部缓冲区 (nmea_parser_t.buf) 的指针。
 * 这些指针仅在下次调用 nmea_parser_feed() 之前有效 —
 * 调用者必须在喂入更多字节之前复制数据。
 */
typedef struct {
    nmea_type_t     type;           /* 枚举的语句类型 (GGA, RMC, ...) */
    char            talker[4];      /* Talker ID 字符串，以 null 结尾 (如 "GP") */
    char            sentence[8];    /* 语句类型字符串，以 null 结尾 (如 "GGA") */
    char           *fields[NMEA_MAX_FIELDS]; /* 指向内部 buf 中字段字符串的指针 */
    int             field_count;    /* 实际填充的字段数 */
    uint8_t         checksum_ok;    /* 计算出的 XOR 与接收到的校验和匹配时为 1 */
    uint8_t         raw_checksum;   /* 语句中收到的校验和值 (0–255) */
    uint8_t         calc_checksum;  /* 对语句主体计算出的校验和值 */
} nmea_parsed_t;

/**
 * @brief 完整的 NMEA 解析器实例 (每个串口流一个)。
 *
 * 维护状态机的当前位置、运行中的 XOR 校验和累加器、
 * 用于字段存储的可重用字符缓冲区，以及用于诊断的会话级统计计数器。
 */
typedef struct {
    nmea_parser_state_t state;      /* 当前状态机状态 */
    nmea_parsed_t       result;     /* 累积的解析结果 (进行中或已完成) */
    /* 内部字符缓冲区 (字段指针指向此处) */
    char    buf[NMEA_MAX_FIELDS * 32]; /* 字段内容存储 (跨语句重用) */
    int     buf_pos;                    /* buf 的写入游标 */
    int     field_start;                /* 当前字段开始的 buf 偏移 */
    int     field_idx;                  /* 当前字段索引 (0 = 类型后的第一个字段) */
    uint8_t checksum;                   /* '$' 和 '*' 之间所有字节的累计 XOR */
    int     checksum_pos;               /* 目前已读取的十六进制校验和字符数 (0–2) */
    char    checksum_hex[3];            /* 两个十六进制字符 + null 终止符 */
    /* 诊断 / 统计 (单调递增) */
    uint32_t total_sentences;           /* 已处理的语句总数 (有效 + 无效) */
    uint32_t checksum_errors;           /* 校验和验证失败的语句数 */
    uint32_t unknown_types;             /* Talker+Type 无法识别的语句数 */
} nmea_parser_t;

/* ================================================================
 *  API 函数声明
 * ================================================================ */

/**
 * @brief 初始化 (或重新初始化) NMEA 解析器实例。
 *
 * 将整个解析器结构体清零并设置状态为 NMEA_STATE_IDLE。
 * 必须在首次调用 nmea_parser_feed() 之前调用一次。
 *
 * @param parser  指向未初始化或陈旧的 nmea_parser_t。
 */
void nmea_parser_init(nmea_parser_t *parser);

/**
 * @brief 将串口流中的一个字符喂入解析器状态机。
 *
 * 这是 NMEA 解码器的核心 — 从串口收到的每个字节调用一次。
 * 解析器内部跨调用跟踪状态，使得跨多个 read() 操作拆分的语句
 * 能够正确组装。
 *
 * @param parser  已初始化的解析器实例。
 * @param ch      来自串口流的一个字节 (通常来自串口环形缓冲区
 *                或直接 read())。
 * @return        0 = 语句仍在处理中 (需要更多字符)，
 *                1 = 一条完整、有效的语句已解码 (parser->result
 *                    已填充且校验和通过)，
 *               -1 = 语句已解码但校验和验证失败
 *                    (parser->result 仍已填充，可供检查)。
 */
int nmea_parser_feed(nmea_parser_t *parser, char ch);

/**
 * @brief 将解析器重置为初始状态，丢弃任何部分完成的语句。
 *
 * 在通信超时后或切换 GPS 模块时有用。
 * 所有内部缓冲区、计数器和运行中的校验和均被清除；
 * 统计计数器 (total_sentences, checksum_errors) 被保留。
 *
 * @param parser  已初始化的解析器实例。
 */
void nmea_parser_reset(nmea_parser_t *parser);

/**
 * @brief 将完全解析的 NMEA 语句根据其类型分发到相应的字段提取器。
 *
 * 检查 parsed->type 并调用相应的 nmea_extract_*() 函数
 * (gga、rmc、gsv、gll、vtg 或 gsa) 来更新 gps_data_t。
 * 仅写入该语句类型中存在的字段 — gps_data 中的其他字段保留其之前的值。
 *
 * @param parsed     指向已完成的 nmea_parsed_t (来自 nmea_parser_feed)。
 * @param gps_data   要用提取值更新的输出结构体。
 * @return           成功返回 0，parsed 为 NULL、gps_data 为 NULL
 *                   或语句类型为 NMEA_UNKNOWN 时返回 -1。
 */
int nmea_extract_data(const nmea_parsed_t *parsed, gps_data_t *gps_data);

/**
 * @brief 从 $GPGGA (全球定位系统定位数据) 语句提取字段。
 *
 * GGA 提供: UTC 时间、纬度、北/南指示、经度、东/西指示、
 * 定位质量 (0 = 无效, 1 = GPS, 2 = DGPS)、卫星数、HDOP、
 * 高度 (大地水准面以上米数) 和大地水准面分离度。
 *
 * @param parsed     解析完成的 GGA 语句 (parsed->type 必须为 NMEA_GPGGA)。
 * @param gps_data   输出: 写入 hour, min, sec, latitude, longitude,
 *                   fix_quality, satellites, hdop 和 altitude。
 * @return           成功返回 0，field_count 不足时返回 -1。
 */
int nmea_extract_gga(const nmea_parsed_t *parsed, gps_data_t *gps_data);

/**
 * @brief 从 $GPRMC (推荐最小特定 GPS/Transit 数据) 语句提取字段。
 *
 * RMC 提供: UTC 时间、状态 (A = 有效, V = 无效)、纬度、北/南、
 * 经度、东/西、地面速度 (节)、地面航向 (度)、日期 (ddmmyy)、
 * 磁偏角和模式指示。
 *
 * 如果状态字段为 'V' (无效)，fix_quality 设为 0 且位置字段不更新。
 * 速度在内部从节转换为 km/h。
 *
 * @param parsed     解析完成的 RMC 语句 (parsed->type 必须为 NMEA_GPRMC)。
 * @param gps_data   输出: 写入时间、位置、速度、日期和 fix_quality
 *                   (仅在状态为 'A' 时)。
 * @return           成功返回 0，field_count 不足时返回 -1。
 */
int nmea_extract_rmc(const nmea_parsed_t *parsed, gps_data_t *gps_data);

/**
 * @brief 从 $GPGSV (可见卫星) 语句提取卫星数量。
 *
 * GSV 语句以每组最多 4 颗卫星的方式报告可见卫星信息。
 * 可见卫星总数在组内每条 GSV 语句的字段 [2] 中报告。
 *
 * 此提取器仅在解析值大于当前值时更新 gps_data->satellites
 * (同一周期中的多条 GSV 语句各自携带相同的总数，
 * 但不同的 Talker ID 可能报告不同的星座大小)。
 *
 * @param parsed     解析完成的 GSV 语句 (parsed->type 必须为 NMEA_GPGSV)。
 * @param gps_data   输出: satellites 字段可能会增加。
 * @return           成功返回 0，field_count 不足时返回 -1。
 */
int nmea_extract_gsv(const nmea_parsed_t *parsed, gps_data_t *gps_data);

/**
 * @brief 从 $GPGLL (地理位置 — 纬度/经度) 语句提取字段。
 *
 * GLL 字段:
 *   [0]=纬度 (ddmm.mmmm) [1]=北/南 [2]=经度 (dddmm.mmmm) [3]=东/西
 *   [4]=UTC 时间 (hhmmss.ss) [5]=状态 (A=有效, V=无效) [6]=模式
 *
 * 如果状态为 'V'，位置不会被更新。
 *
 * @param parsed     解析完成的 GLL 语句 (parsed->type 必须为 NMEA_GPGLL)。
 * @param gps_data   输出: 时间、纬度、经度和 fix_quality。
 * @return           成功返回 0，field_count 不足时返回 -1。
 */
int nmea_extract_gll(const nmea_parsed_t *parsed, gps_data_t *gps_data);

/**
 * @brief 从 $GPVTG (地面航向和地面速度) 语句提取速度。
 *
 * VTG 字段:
 *   [0]=真北航向 [1]=T [2]=磁北航向 [3]=M
 *   [4]=速度(节) [5]=N [6]=速度(km/h) [7]=K [8]=模式
 *
 * 优先使用 km/h 速度 (字段 [6])。如果为空或缺失，
 * 则将节为单位的速度 (字段 [4]) 转换为 km/h (1 节 = 1.852 km/h)。
 *
 * @param parsed     解析完成的 VTG 语句 (parsed->type 必须为 NMEA_GPVTG)。
 * @param gps_data   输出: 写入 speed (km/h)。
 * @return           成功返回 0，field_count 不足时返回 -1。
 */
int nmea_extract_vtg(const nmea_parsed_t *parsed, gps_data_t *gps_data);

/**
 * @brief 从 $GPGSA (GNSS DOP 和活跃卫星) 语句提取 DOP 和定位类型。
 *
 * GSA 字段:
 *   [0]=模式 (M=手动, A=自动) [1]=定位类型 (1=无, 2=2D, 3=3D)
 *   [2..13]=解算中使用的卫星 PRN 号
 *   [14]=PDOP [15]=HDOP [16]=VDOP
 *
 * 此提取器从字段 [1] 更新 fix_quality，从字段 [15] 更新 HDOP
 * (如果存在则覆盖 GGA 的 HDOP 值)。
 *
 * @param parsed     解析完成的 GSA 语句 (parsed->type 必须为 NMEA_GPGSA)。
 * @param gps_data   输出: 写入 fix_quality 和可选的 hdop。
 * @return           成功返回 0，field_count 不足时返回 -1。
 */
int nmea_extract_gsa(const nmea_parsed_t *parsed, gps_data_t *gps_data);

/**
 * @brief 检查解析器是否已完成一条语句的解码。
 *
 * 这是一个便捷包装器 — 在实践中，调用者通常直接检查
 * nmea_parser_feed() 的返回值 (1 = 完成, -1 = 校验和错误)
 * 而不是单独调用此函数。
 *
 * @param parser  已初始化的解析器实例。
 * @return        0 (始终 — 当前实现始终返回 0;
 *                语句完成通过 nmea_parser_feed 的返回值发出信号)。
 */
int nmea_parser_done(const nmea_parser_t *parser);

#endif /* _NMEA_PARSER_H_ */
