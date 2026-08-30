/**
 * @file    gps_daemon.h
 * @brief   GPS 定位守护进程 - 配置参数 & 数据结构
 *
 * 功能概述:
 *   - 通过串口 (UART) 读取 GPS 模块 NMEA-0183 原始数据
 *   - 解析 GGA / RMC / GSV 语句, 提取定位信息
 *   - 里程累计: 基于 Haversine 公式计算相邻定位点距离
 *   - UNIX Socket 推送 gps_data_t 给 Qt UI
 *   - 轨迹日志: 记录历史定位点, 支持轨迹回放
 *
 * GPS 模块接口:
 *   - 硬件接口: USB转串口(/dev/ttyUSB0) 或 原生UART(/dev/ttymxc2)
 *   - 波特率: 9600 bps (GPS NMEA 标准)
 *   - 数据格式: 8N1 (8数据位, 无校验, 1停止位)
 *   - 协议: NMEA-0183 (ASCII 文本, 以 '$' 开头, CRLF 结尾)
 *
 * NMEA-0183 关键语句:
 *   GGA — 定位信息 (时间/经纬度/质量/卫星数/高度/HDOP)
 *   RMC — 推荐最小导航信息 (时间/经纬度/速度/航向/日期)
 *   GSV — 可见卫星信息 (卫星数/仰角/方位角/信噪比)
 *
 * 学习要点:
 *   1. termios 串口编程: 波特率/数据位/校验/停止位/非阻塞
 *   2. NMEA-0183 协议: ASCII 文本解析, 校验和验证, 状态机
 *   3. 地理计算: Haversine 公式, 度分转换 → 十进制度
 *   4. 环形缓冲区: 串口数据的流式缓冲
 */

#ifndef _GPS_DAEMON_H_
#define _GPS_DAEMON_H_

#include "common.h"         /* gps_data_t, ret_code_t, proc_config_t, ... */
#include "protocol/protocol.h"   /* ringbuffer_t, protocol_parser_t */
#include <stdint.h>
#include <time.h>
#include <termios.h>        /* B9600, speed_t */

/* ================================================================
 *  默认配置
 * ================================================================ */

/* ---- 串口 ---- */
#define GPS_DEFAULT_UART_DEVICE  "/dev/ttymxc5"   /* i.MX6ULL 原生 UART5 */
#define GPS_DEFAULT_BAUD_RATE    B9600    /* termios 常量, 9600 bps */

/* ---- 上报间隔 ---- */
#define GPS_DEFAULT_REPORT_INTERVAL_MS  1000   /* 每秒上报一次 */

/* ---- 里程累计 ---- */
#define GPS_EARTH_RADIUS_M      6371000.0  /* 地球平均半径 (米), WGS-84 */

/* ---- 轨迹日志 ---- */
#define GPS_MAX_TRACK_POINTS    3600      /* 最多保存轨迹点数 (1Hz×1小时) */

/* ---- Socket ---- */
#define GPS_SOCKET_PATH         "/tmp/car_gps.sock"

/* ---- 日志 ---- */
#define GPS_LOG_PATH            "/var/log/car_terminal/gps.log"

/* ---- 缓冲区 ---- */
#define GPS_UART_BUF_SIZE       4096      /* 串口环形缓冲区大小 */
#define GPS_NMEA_MAX_LEN        128       /* 单条 NMEA 语句最大长度 */

/* ================================================================
 *  定位状态枚举
 * ================================================================ */
typedef enum {
    GPS_FIX_NONE    = 0,    /* 未定位 (搜星中或无信号) */
    GPS_FIX_2D      = 1,    /* 2D 定位 (3颗卫星, 无高度) */
    GPS_FIX_3D      = 2,    /* 3D 定位 (4+颗卫星, 含高度) */
} gps_fix_type_t;

/* ================================================================
 *  NMEA 语句类型枚举
 * ================================================================ */
typedef enum {
    NMEA_UNKNOWN    = 0,
    NMEA_GPGGA      = 1,    /* $GPGGA — GPS 定位数据 (时间/经纬度/质量/卫星数/HDOP/海拔) */
    NMEA_GPRMC      = 2,    /* $GPRMC — 推荐最小定位信息 (时间/经纬度/速度/日期) */
    NMEA_GPGSV      = 3,    /* $GPGSV — 可见卫星状态信息 (卫星数/仰角/方位角/SNR) */
    NMEA_GPGSA      = 4,    /* $GPGSA — 当前卫星信息 (DOP 精度因子) */
    NMEA_GPGLL      = 5,    /* $GPGLL — 地理定位信息 (经纬度/时间/状态) */
    NMEA_GPVTG      = 6,    /* $GPVTG — 地面速度信息 (航向/速度) */
} nmea_type_t;

/* ================================================================
 *  里程累计器
 * ================================================================ */
typedef struct {
    double      total_km;           /* 总里程 (公里) */
    double      last_lat;           /* 上一个有效定位点的纬度 */
    double      last_lon;           /* 上一个有效定位点的经度 */
    int         has_last;           /* 是否有上一个有效点 (0/1) */
    uint32_t    point_count;        /* 累计定位点数量 */
} gps_odometer_t;

/* ================================================================
 *  轨迹点 (用于存储历史轨迹)
 * ================================================================ */
typedef struct {
    double      lat;                /* 纬度 */
    double      lon;                /* 经度 */
    float       speed;              /* 速度 (km/h) */
    time_t      timestamp;          /* 时间戳 */
} gps_track_point_t;

/* ================================================================
 *  进程上下文
 * ================================================================ */
typedef struct {
    /* 串口 */
    char        uart_device[64];    /* 串口设备路径 */
    int         uart_fd;            /* 串口文件描述符 */
    /* 最新定位数据 */
    gps_data_t  latest;             /* 最新解析的 GPS 数据 */
    gps_fix_type_t fix_type;        /* 当前定位状态 */
    /* 里程 */
    gps_odometer_t odo;             /* 里程累计器 */
    /* 轨迹 */
    gps_track_point_t *track_buf;   /* 轨迹缓冲区 (动态分配) */
    int         track_count;        /* 当前轨迹点数 */
    int         track_max;          /* 最大轨迹点数 */
    /* 配置 */
    int         report_interval_ms; /* 上报间隔 */
    proc_config_t proc_cfg;         /* 进程通用配置 */
    /* 运行状态 */
    int         running;            /* 运行标志 */
    int         epoll_fd;           /* epoll 实例 */
    int         timer_fd;           /* 数据上报定时器 */
    int         signal_fd;          /* 信号 fd */
    int         server_fd;          /* Socket 服务端 */
    int         client_fd;          /* UI 客户端连接 */
    protocol_parser_t proto_parser; /* 协议解析器 */
    ringbuffer_t uart_rb;           /* 串口接收环形缓冲 */
    ringbuffer_t recv_rb;           /* Socket 接收缓冲 */
} gps_context_t;

/* ================================================================
 *  函数声明
 * ================================================================ */

/**
 * @brief 将 NMEA 度分格式 (ddmm.mmmm) 转换为十进制度。
 *
 * NMEA 纬度和经度以固定格式数字打包:
 *   - 纬度:  ddmm.mmmm  →  dd + mm.mmmm / 60
 *   - 经度: dddmm.mmmm →  ddd + mm.mmmm / 60
 *
 * 示例:
 *   - 纬度  3912.3456 → 39 + 12.3456 / 60 = 39.205760 度
 *   - 经度 11623.4567 → 116 + 23.4567 / 60 = 116.390945 度
 *
 * 符号 (N/S, E/W) 由调用者根据 NMEA 语句中的半球指示字段单独应用。
 *
 * @param nmea_deg  原始 NMEA 度分值 (例如 3912.3456)。
 * @return          十进制度 (double)。输入为零或无法解析时返回 NAN。
 */
double gps_nmea_to_degrees(double nmea_deg);

/**
 * @brief 使用 Haversine 公式计算两个地理坐标之间的大圆距离。
 *
 * 对于短距离，Haversine 优于球面余弦定理，因为它避免了
 * 浮点运算中的灾难性消去。
 *
 * 公式:
 *   a = sin²(Δlat / 2) + cos(lat1) * cos(lat2) * sin²(Δlon / 2)
 *   c = 2 * atan2(√a, √(1 - a))
 *   d = R * c
 *
 * 其中 R = GPS_EARTH_RADIUS_M (6,371,000 m, WGS-84 平均半径)。
 *
 * 所有输入角均为十进制度；内部完成到弧度的转换。
 *
 * @param lat1  第一个点的纬度 (十进制度)。
 * @param lon1  第一个点的经度 (十进制度)。
 * @param lat2  第二个点的纬度 (十进制度)。
 * @param lon2  第二个点的经度 (十进制度)。
 * @return      距离 (米)。
 */
double gps_haversine_distance(double lat1, double lon1,
                              double lat2, double lon2);

/**
 * @brief 从 INI 格式配置文件加载 GPS 守护进程配置。
 *
 * 读取 [gps] 节并填充:
 *   - ctx->uart_device         (默认: /dev/ttymxc5)
 *   - ctx->report_interval_ms  (默认: 1000)
 *   - ctx->proc_cfg.socket_path (GPS_SOCKET_PATH)
 *   - ctx->proc_cfg.log_path    (GPS_LOG_PATH)
 *
 * 如果 conf_file 为 NULL 或无法解析，所有字段使用硬编码默认值。
 *
 * @param ctx        指向守护进程上下文的指针 (原地修改)。
 * @param conf_file  config.ini 路径，或 NULL 以使用全部默认值。
 * @return           成功返回 RET_OK，ctx 为 NULL 时返回 RET_ERR_PARAM。
 */
int gps_config_load(gps_context_t *ctx, const char *conf_file);

#endif /* _GPS_DAEMON_H_ */
