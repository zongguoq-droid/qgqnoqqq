/**
 * @file    common.h
 * @brief   智能车载终端 - 公共库全局头文件
 * @author  Student
 * @date    2026-08-01
 *
 * 本文件定义了整个项目共用的：
 *   - 数据类型和枚举
 *   - 统一返回码 (所有函数通用, 语义明确)
 *   - 进程状态定义 (各后台进程的生命周期状态机)
 *   - 消息类型枚举 (高4位模块ID + 低4位消息ID 编码方案)
 *   - 进程间通信的数据结构 (GPS/DHT11/CAN/按键等, #pragma pack 保证对齐一致)
 *
 * 所有后台进程 (guard/gps/dvr/av/input/canbus/net/sensor)
 * 和公共库模块 (socket/log/protocol/config) 都应包含此头文件。
 *
 * 架构说明:
 *   本项目采用微服务风格的进程架构，各进程通过 UNIX Domain Socket
 *   进行 IPC 通信，消息帧格式由 protocol 模块定义，帧内承载的数据
 *   结构定义在本文件中。guard_daemon 作为父进程统一管理子进程生命周期。
 */

#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>   /* uint8_t, uint16_t, uint32_t, uint64_t, int8_t */
#include <stddef.h>   /* size_t, NULL */
#include <time.h>     /* time_t */
#include <sys/types.h>/* pid_t, ssize_t */

/* 前向声明 (完整定义在 ringbuffer.h / protocol.h 中, 此处避免循环依赖) */
struct ringbuffer;
typedef struct ringbuffer ringbuffer_t;
struct protocol_parser;
typedef struct protocol_parser protocol_parser_t;

/* ================================================================
 *  版本信息
 * ================================================================ */
#define CAR_TERMINAL_VERSION_MAJOR  1
#define CAR_TERMINAL_VERSION_MINOR  0
#define CAR_TERMINAL_VERSION_PATCH  0

/* ================================================================
 *  通用常量定义
 * ================================================================ */

/* 路径和名称最大长度 */
#define MAX_PATH_LEN        256     /* 文件路径最大长度 */
#define MAX_NAME_LEN        64      /* 名称(进程名/接口名)最大长度 */
#define MAX_IP_LEN          16      /* IP地址字符串最大长度 */

/* Socket 相关 */
#define MAX_CLIENTS         8       /* 每个服务端最大客户端连接数 */
#define SOCKET_BACKLOG      4       /* listen() 的 backlog 参数 */

/* 缓冲区大小 */
#define RECV_BUF_SIZE       4096    /* 通用接收缓冲区大小 */
#define SEND_BUF_SIZE       4096    /* 通用发送缓冲区大小 */
#define LOG_BUF_SIZE        512     /* 单条日志最大长度 */

/* 消息帧相关 (与 protocol 模块保持一致) */
#define MSG_FRAME_HEAD0     0xAA    /* 帧头字节0 */
#define MSG_FRAME_HEAD1     0x55    /* 帧头字节1 */
#define MSG_FRAME_TAIL      0x55    /* 帧尾字节 */
#define MSG_MAX_DATA_LEN    65535   /* 单帧数据最大长度 */
#define MSG_MAX_FRAME_LEN   (MSG_MAX_DATA_LEN + 7) /* 最大帧总长 */

/* ================================================================
 *  统一返回码 (所有函数通用)
 * ================================================================ */
typedef enum {
    RET_OK              =  0,   /* 操作成功 */
    RET_ERR_PARAM       = -1,   /* 参数错误 (空指针/越界/范围错误) */
    RET_ERR_MEM         = -2,   /* 内存分配失败 (malloc/calloc 返回 NULL) */
    RET_ERR_IO          = -3,   /* IO 操作失败 (read/write/open/close 错误) */
    RET_ERR_TIMEOUT     = -4,   /* 操作超时 */
    RET_ERR_CRC         = -5,   /* CRC 校验失败 */
    RET_ERR_PROTO       = -6,   /* 协议错误 (帧格式不正确) */
    RET_ERR_NOT_FOUND   = -7,   /* 资源未找到 (文件/设备/配置项) */
    RET_ERR_BUSY        = -8,   /* 资源忙 (设备已被占用) */
    RET_ERR_FULL        = -9,   /* 缓冲区/队列已满 */
    RET_ERR_EMPTY       = -10,  /* 缓冲区/队列为空 */
    RET_ERR_NOT_CONN    = -11,  /* 未连接 */
    RET_ERR_SYSTEM      = -99,  /* 系统错误 (一般伴随 errno 设置) */
} ret_code_t;

/**
 * @brief 将返回码转换为可读字符串 (用于日志输出)
 * @param code  返回码
 * @return      返回码对应的描述字符串
 */
static inline const char* ret_code_str(ret_code_t code)
{
    switch (code) {
    case RET_OK:            return "OK";
    case RET_ERR_PARAM:     return "PARAM_ERROR";
    case RET_ERR_MEM:       return "MEM_ERROR";
    case RET_ERR_IO:        return "IO_ERROR";
    case RET_ERR_TIMEOUT:   return "TIMEOUT";
    case RET_ERR_CRC:       return "CRC_ERROR";
    case RET_ERR_PROTO:     return "PROTO_ERROR";
    case RET_ERR_NOT_FOUND: return "NOT_FOUND";
    case RET_ERR_BUSY:      return "BUSY";
    case RET_ERR_FULL:      return "FULL";
    case RET_ERR_EMPTY:     return "EMPTY";
    case RET_ERR_NOT_CONN:  return "NOT_CONNECTED";
    case RET_ERR_SYSTEM:    return "SYSTEM_ERROR";
    default:                return "UNKNOWN";
    }
}

/* ================================================================
 *  进程状态枚举 (所有进程共用)
 * ================================================================ */
typedef enum {
    PROC_STOPPED    = 0,    /* 已停止 (初始状态或被停止后) */
    PROC_STARTING   = 1,    /* 启动中 (正在初始化硬件/资源) */
    PROC_RUNNING    = 2,    /* 运行中 (正常工作状态) */
    PROC_ERROR      = 3,    /* 异常 (遇到可恢复的错误) */
} proc_state_t;

/* ================================================================
 *  进程配置结构 (所有进程共用)
 *  每个进程启动时从配置文件读取，或使用默认值
 * ================================================================ */
typedef struct {
    char    socket_path[MAX_PATH_LEN]; /* UNIX Socket 路径，如 "/tmp/car_gps.sock" */
    char    log_path[MAX_PATH_LEN];    /* 日志文件路径 */
    int     log_level;                 /* 日志级别: 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR */
    int     heartbeat_interval;        /* 心跳间隔 (秒)，0 表示不发送心跳 */
} proc_config_t;

/* ================================================================
 *  IPC 消息类型枚举
 *
 *  编码方案: 1 字节 (uint8_t), 范围 0x00 ~ 0xFF
 *    高 4 位 (bits 7-4) = 模块 ID  —— 标识目标子系统 (0x0=系统, 0x1=GPS, ...)
 *    低 4 位 (bits 3-0) = 消息 ID  —— 模块内的具体操作 (最多 16 种)
 *
 *  路由规则:
 *    各后台进程根据 msg_get_module() 提取模块 ID 来决定是否处理该消息。
 *    不属于本模块的消息应忽略或转发。guard_daemon 处理所有 0x0x 系统消息。
 *
 *  扩展容量: 共可容纳 16 个模块 (0x0 ~ 0xF)，每个模块 16 种消息 (0x0 ~ 0xF)。
 * ================================================================ */

/* ---- 系统通用消息 (0x00 ~ 0x0F) - 由 guard_daemon 处理 ---- */
#define MSG_TYPE_SYS            0x00    /* 系统消息模块ID掩码 (高4位) */
#define MSG_HEARTBEAT           0x00    /* 心跳请求/响应 (guard ↔ 各子进程保活) */
#define MSG_SERVICE_STATUS      0x01    /* 服务状态通知 (子进程→guard 上报自身状态) */
#define MSG_SERVICE_RESTART     0x02    /* 请求重启某服务 (UI→guard 远程控制) */
#define MSG_SYSTEM_SHUTDOWN     0x03    /* 系统关机通知 (guard→所有子进程广播退出) */
#define MSG_LOG_REQUEST         0x04    /* 请求上传日志 (UI→guard 拉取远程日志) */

/* ---- GPS 定位消息 (0x10 ~ 0x1F) - 由 gps_daemon 处理 ---- */
#define MSG_TYPE_GPS            0x10    /* GPS 模块ID掩码 */
#define MSG_GPS_DATA            0x10    /* GPS数据推送 (gps→UI 主动上报, 载体: gps_data_t) */
#define MSG_GPS_QUERY           0x11    /* 请求最新GPS数据 (UI→gps 轮询模式) */
#define MSG_GPS_CONFIG          0x12    /* GPS配置 (UI→gps 设置上报频率/波特率等) */
#define MSG_GPS_STATUS          0x13    /* GPS状态 (gps→UI: 搜星数/信号质量/模块状态) */

/* ---- DVR 录像消息 (0x20 ~ 0x2F) - 由 dvr_daemon 处理 ---- */
#define MSG_TYPE_DVR            0x20    /* DVR 模块ID掩码 */
#define MSG_DVR_START           0x20    /* 开始录像 (UI→dvr, 可携带通道/分辨率参数) */
#define MSG_DVR_STOP            0x21    /* 停止录像 */
#define MSG_DVR_STATUS          0x22    /* 录像状态 (dvr→UI: 录制时长/文件大小/码率) */
#define MSG_DVR_SNAPSHOT        0x23    /* 抓拍一帧 (UI→dvr, 即时拍照并返回JPEG) */
#define MSG_DVR_FILE_LIST       0x24    /* 请求录像文件列表 (UI→dvr, 按时间范围查询) */
#define MSG_DVR_STORAGE_INFO    0x25    /* 存储空间信息 (dvr→UI: eMMC/SD 剩余容量) */
#define MSG_DVR_PAUSE           0x26    /* 暂停/继续录像 (toggle 模式) */

/* ---- 传感器消息 (0x30 ~ 0x3F) - 由 sensor_daemon 处理 ---- */
#define MSG_TYPE_SENSOR         0x30    /* 传感器模块ID掩码 */
#define MSG_SENSOR_DATA         0x30    /* 温湿度数据推送 (sensor→UI, 载体: dht11_data_t) */
#define MSG_SENSOR_QUERY        0x31    /* 请求最新数据 (UI→sensor) */
#define MSG_SENSOR_CONFIG       0x32    /* 传感器配置 (UI→sensor: 采样间隔/告警阈值) */
#define MSG_SENSOR_ALERT        0x33    /* 温湿度超阈值告警 (sensor→UI 事件推送) */

/* ---- 网络服务消息 (0x40 ~ 0x4F) - 由 net_daemon 处理 ---- */
#define MSG_TYPE_NET            0x40    /* 网络模块ID掩码 */
#define MSG_NET_STATUS          0x40    /* 网络状态 (net→UI: 当前链路类型/信号强度/RSSI) */
#define MSG_NET_SWITCH          0x41    /* 切换网络 (UI→net: WIFI→4G→ETH 优先级切换) */
#define MSG_NET_UPLOAD          0x42    /* 上传数据到云端 (触发录像/日志文件上传) */
#define MSG_NET_DOWNLOAD        0x43    /* 下载数据/OTA升级 (云端→终端固件更新) */

/* ---- CAN 总线消息 (0x50 ~ 0x5F) - 由 canbus_daemon 处理 ---- */
#define MSG_TYPE_CAN            0x50    /* CAN 模块ID掩码 */
#define MSG_CAN_DATA            0x50    /* CAN报文推送 (canbus→UI, 载体: can_msg_t) */
#define MSG_CAN_SEND            0x51    /* 发送CAN报文 (UI→canbus, 如控制车门/灯光) */
#define MSG_CAN_FILTER          0x52    /* 设置CAN过滤规则 (只上报关注的CAN ID) */
#define MSG_CAN_STATUS          0x53    /* CAN总线状态 (canbus→UI: BUS-OFF/错误计数) */

/* ---- 音视频消息 (0x60 ~ 0x6F) - 由 av_daemon 处理 ---- */
#define MSG_TYPE_AV             0x60    /* AV 模块ID掩码 */
#define MSG_AV_PLAY             0x60    /* 播放指定音频文件 (UI→av, 如报警提示音) */
#define MSG_AV_STOP             0x61    /* 停止播放 */
#define MSG_AV_PAUSE            0x62    /* 暂停/继续 (toggle 模式) */
#define MSG_AV_VOLUME           0x63    /* 设置音量 (0~100) */
#define MSG_AV_STATUS           0x64    /* 播放状态 (av→UI: 当前曲目/进度/状态) */

/* ---- 按键输入消息 (0x70 ~ 0x7F) - 由 input_daemon 处理 ---- */
#define MSG_TYPE_INPUT          0x70    /* 输入模块ID掩码 */
#define MSG_KEY_EVENT           0x70    /* 按键事件通知 (input→UI, 载体: key_event_t) */
#define MSG_LED_CONTROL         0x71    /* 控制用户LED (UI→input: 开关/闪烁模式) */

/**
 * @brief 从消息类型提取模块ID (高4位)
 * @param msg_type  消息类型值
 * @return          模块ID (0x00, 0x10, 0x20, ...)
 */
static inline uint8_t msg_get_module(uint8_t msg_type)
{
    return msg_type & 0xF0;  /* 取高4位 */
}

/**
 * @brief 从消息类型提取消息ID (低4位)
 * @param msg_type  消息类型值
 * @return          消息ID (0~15)
 */
static inline uint8_t msg_get_id(uint8_t msg_type)
{
    return msg_type & 0x0F;  /* 取低4位 */
}

/* ================================================================
 *  各进程核心数据结构
 *
 *  这些结构体定义放在 common.h 中，以便：
 *    - UI 进程 (可能是 C++/Qt) 和后台 daemon 进程 (C) 共享同一份数据定义
 *    - protocol 模块可以使用 sizeof() 确定序列化数据长度
 *    - 所有进程对数据字段的含义理解一致
 *
 *  实际代码中每个进程还有自己私有的 context 结构 (定义在各进程目录中)，
 *  用于维护 fd、缓冲区、状态机等运行时数据。
 *
 *  重要: #pragma pack(1) 确保结构体按 1 字节对齐 (无编译器填充)
 *    - 原因: 本项目使用原始字节流传输 (二进制序列化)，
 *      如果发送端和接收端的编译器对齐方式不同 (或语言不同)，
 *      结构体内存布局会产生偏移，导致收到的数据字段错位。
 *    - 代价: 非对齐访问在部分架构上可能有性能损失，
 *      但车载终端 CPU (ARM Cortex-A) 支持非对齐访问，影响可忽略。
 *    - 替代方案: Protocol Buffers / FlatBuffers 可实现平台无关序列化，
 *      但对本项目的嵌入式场景来说过重。
 * ================================================================ */

#pragma pack(push, 1)

/* ---- GPS 定位数据 ---- */
typedef struct {
    double      latitude;       /* 纬度 (度), 北纬为正, 范围 -90.0 ~ 90.0 */
    double      longitude;      /* 经度 (度), 东经为正, 范围 -180.0 ~ 180.0 */
    float       speed;          /* 地面速度 (km/h) */
    float       altitude;       /* 海拔高度 (m) */
    uint16_t    year;           /* UTC 年 (如 2026) */
    uint8_t     month;          /* UTC 月 (1~12) */
    uint8_t     day;            /* UTC 日 (1~31) */
    uint8_t     hour;           /* UTC 时 (0~23) */
    uint8_t     min;            /* UTC 分 (0~59) */
    uint8_t     sec;            /* UTC 秒 (0~59) */
    uint8_t     satellites;     /* 使用的卫星数量 */
    uint8_t     fix_quality;    /* 定位质量: 0=无效, 1=GPS, 2=DGPS */
    float       hdop;           /* 水平精度因子 (越小越好, <1.0为优秀) */
} gps_data_t;

/* ---- DHT11 温湿度数据 ---- */
typedef struct {
    float       temperature;    /* 温度 (℃), 范围 -20.0 ~ 60.0 */
    float       humidity;       /* 湿度 (%RH), 范围 20.0 ~ 95.0 */
    uint8_t     checksum_ok;    /* 校验结果: 0=失败, 1=通过 */
    time_t      timestamp;      /* 采集时间戳 */
} dht11_data_t;

/* ---- 按键事件 ---- */
typedef struct {
    uint8_t     key_id;         /* 按键标识: 1=KEY1, 2=KEY2 */
    uint8_t     event_type;     /* 事件类型: 1=短按, 2=长按(>2s), 3=双击, 4=释放 */
    uint32_t    duration_ms;    /* 按下持续时间 (ms) */
    time_t      timestamp;      /* 事件时间戳 */
} key_event_t;

/* ---- CAN 帧结构 ---- */
typedef struct {
    uint32_t    can_id;         /* CAN ID: 标准帧 11 位 或 扩展帧 29 位 */
    uint8_t     can_dlc;        /* 数据长度码 (DLC), 0~8 */
    uint8_t     data[8];        /* 数据字节 (最多8字节) */
    uint8_t     is_extended;    /* 是否扩展帧: 0=标准帧, 1=扩展帧 */
    uint8_t     is_remote;      /* 是否远程帧: 0=数据帧, 1=远程帧 */
} can_msg_t;

/* ---- 网络状态 ---- */
typedef enum {
    LINK_NONE   = 0,            /* 无链路 */
    LINK_ETH0   = 1,            /* 以太网口0 */
    LINK_ETH1   = 2,            /* 以太网口1 */
    LINK_WIFI   = 3,            /* WIFI */
    LINK_4G     = 4,            /* 4G 移动网络 */
} net_link_t;

/* ---- DVR 状态 ---- */
typedef enum {
    DVR_IDLE        = 0,        /* 空闲 */
    DVR_RECORDING   = 1,        /* 录像中 */
    DVR_PAUSED      = 2,        /* 暂停 */
    DVR_ERROR       = 3,        /* 错误 */
} dvr_state_t;

/* ---- 音视频播放状态 ---- */
typedef enum {
    AV_IDLE         = 0,        /* 空闲 */
    AV_PLAYING      = 1,        /* 播放中 */
    AV_PAUSED       = 2,        /* 暂停 */
} av_state_t;

#pragma pack(pop)

/* ================================================================
 *  实用宏定义
 * ================================================================ */

/**
 * @brief 获取结构体成员相对于结构体起始地址的偏移量
 * @param type  结构体类型
 * @param member 成员名
 *
 * 这是 Linux 内核中 container_of 宏的辅助宏。
 * 示例: offsetof(struct gps_data_t, speed) → 在结构体中的字节偏移
 */
#ifndef offsetof
#define offsetof(type, member)  ((size_t)&(((type*)0)->member))
#endif

/**
 * @brief 通过成员指针反推包含它的结构体指针 (Linux 内核经典宏)
 * @param ptr    成员变量的指针
 * @param type   包含该成员的结构体类型
 * @param member 成员在结构体中的名字
 *
 * 这是 Linux 内核中最常用的宏之一 (定义在 <linux/kernel.h>)。
 * 在链表、epoll event.data.ptr 回调等场景中非常实用 —
 * 可以将一个内嵌的 struct 成员地址"上溯"到外层结构体地址。
 *
 * 用法示例:
 *   // 假设有一个链表节点嵌入在进程信息结构中
 *   struct process_info {
 *       int pid;
 *       struct list_head list;  // 嵌入的链表节点
 *       char name[32];
 *   };
 *   struct list_head *pos = ...; // 从链表中取出的节点指针
 *   struct process_info *p = container_of(pos, struct process_info, list);
 *   // 现在可以通过 p 访问 pid, name 等字段了
 *
 * 原理: 成员地址 - 成员偏移量 = 结构体首地址
 *   (char*)&(ptr->member) - offsetof(type, member) = (char*)ptr
 */
#ifndef container_of
#define container_of(ptr, type, member) \
    ((type*)((char*)(ptr) - offsetof(type, member)))
#endif

/**
 * @brief 计算数组元素个数 (编译期确定)
 * @param arr  数组名 (不能是指针)
 *
 * 使用编译期的 sizeof 计算，不产生运行开销。
 * 注意: 传入指针会得到错误结果 (因为 sizeof(指针) != sizeof(数组))
 */
#define ARRAY_SIZE(arr)  (sizeof(arr) / sizeof((arr)[0]))

/**
 * @brief 取两个数中的较小值
 */
#define MIN(a, b)  (((a) < (b)) ? (a) : (b))

/**
 * @brief 取两个数中的较大值
 */
#define MAX(a, b)  (((a) > (b)) ? (a) : (b))

/**
 * @brief 将值限制在指定范围内
 * @param x   输入值
 * @param lo  下限
 * @param hi  上限
 * @return    限制后的值 [lo, hi]
 */
#define CLAMP(x, lo, hi)  (((x) < (lo)) ? (lo) : ((x) > (hi)) ? (hi) : (x))

#endif /* _COMMON_H_ */
