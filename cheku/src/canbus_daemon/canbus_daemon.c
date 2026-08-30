/**
 * @file    canbus_daemon.c
 * @brief   CAN 总线通信守护进程 (poll 模式)
 *
 * ============================================================================
 * 架构概述
 * ============================================================================
 *
 * 本守护进程是车载终端 CAN 总线通信的核心, 负责:
 *   1. 通过 Linux SocketCAN 接口收发 CAN 报文 (标准帧/扩展帧)
 *   2. 所有收到的 CAN 报文自动转发给 Qt UI (通过 UNIX Domain Socket)
 *   3. 接收 Qt UI 发来的 CAN 发送指令 (MSG_CAN_SEND), 写入 CAN 总线
 *   4. CAN 报文环形缓冲区 (最近 200 条), 用于 UI 历史查询和调试
 *   5. 每秒输出 RX/TX 统计日志
 *
 * ============================================================================
 * 主循环 (poll 模式, 单线程)
 * ============================================================================
 *
 *   while (running) {
 *       poll([can_fd, server_fd, client_fd], 1000ms)
 *
 *       // 路径 1: CAN 报文接收
 *       if can_fd POLLIN:
 *           can_recv() 循环读取 struct can_frame
 *           -> 存入环形缓冲区 (can_ring_push, dir=0 表示 RX)
 *           -> 通过 Socket 转发给 Qt UI (MSG_CAN_DATA)
 *
 *       // 路径 2: 接受 Qt UI 连接
 *       if server_fd POLLIN:
 *           accept() -> 单客户端模式 (新连接踢旧连接)
 *
 *       // 路径 3: 接收 Qt UI 的 CAN 发送指令
 *       if client_fd POLLIN:
 *           recv() -> protocol_unpack() -> MSG_CAN_SEND
 *           -> 构造 can_msg_t (memset+memcpy, 防止栈垃圾)
 *           -> can_send() 写入 CAN 总线
 *           -> 存入环形缓冲区 (can_ring_push, dir=1 表示 TX)
 *
 *       // 每秒统计日志
 *       if time(NULL) changed: LOG_INFO RX/TX counts
 *   }
 *
 * ============================================================================
 * 已知 Bug 修复记录
 * ============================================================================
 *
 * [已修复] 接收 Qt CAN 发送指令时的栈垃圾 bug (严重):
 *   问题: 原先使用直接类型转换将协议数据缓冲区转换为 can_msg_t*,
 *         但 can_msg_t 结构体可能因编译器对齐/填充而包含未初始化的栈字节,
 *         导致 is_extended / is_remote 等标志位被栈垃圾随机置位,
 *         发送的 CAN 报文出现随机扩展帧标志或远程帧标志。
 *   症状: Qt 发送标准帧, CAN 总线上却出现扩展帧; 或随机出现远程帧。
 *   修复: 在栈上声明 can_msg_t 后先 memset 清零, 再用 memcpy 从协议数据
 *         中逐字节拷贝 (不超过 sizeof(can_msg_t))。
 *         这确保所有未显式设置的字段均为 0, 不受栈垃圾影响。
 *   位置: main() 中处理 MSG_CAN_SEND 的分支。
 */

#include "canbus_daemon.h"
#include "can_driver.h"
#include "log/log.h"
#include "config/config.h"
#include "socket/unix_socket.h"
#include "protocol/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <time.h>

static canbus_context_t g_ctx;
static volatile int g_running = 1;
static void sig_handler(int s) { (void)s; g_running = 0; }

/**
 * @brief 初始化 CAN 报文环形缓冲区
 *
 * 使用 calloc 分配内存 (自动清零), 容量由 CAN_RING_SIZE 宏指定 (默认 200 条)。
 * 环形缓冲区为 FIFO 结构: head 指向下一个写入位置, count 跟踪当前条目数。
 * 缓冲区满时覆盖最旧的数据 (overwrite 模式)。
 *
 * @param ring     环形缓冲区实例指针
 * @param capacity 最大容量 (条目数)
 * @return         RET_OK 成功, RET_ERR_MEM 内存分配失败
 */
int can_ring_init(can_ring_t *ring, int capacity) {
    ring->buffer=(can_ring_entry_t*)calloc((size_t)capacity,sizeof(can_ring_entry_t));
    if(!ring->buffer)return RET_ERR_MEM;
    ring->capacity=capacity;ring->head=ring->count=0;return RET_OK;
}

/**
 * @brief 向环形缓冲区写入一条 CAN 报文
 *
 * 在 head 位置写入报文数据 (memcpy 完整 can_msg_t)、时间戳、方向标志,
 * 然后推进 head 指针。缓冲区满时覆盖最旧条目 (count 不再增长)。
 *
 * @param ring 环形缓冲区
 * @param msg  CAN 报文指针 (完整拷贝, 非浅拷贝)
 * @param dir  方向: 0=接收(RX, 来自 CAN 总线), 1=发送(TX, 来自 Qt UI 指令)
 */
void can_ring_push(can_ring_t *ring,const can_msg_t *msg,int dir){
    if(!ring||!ring->buffer||!msg)return;
    int idx=ring->head;
    /* 拷贝完整的 can_msg_t (can_id, can_dlc, data[8], is_extended, is_remote) */
    memcpy(&ring->buffer[idx].msg,msg,sizeof(can_msg_t));
    ring->buffer[idx].timestamp=time(NULL);
    ring->buffer[idx].direction=(uint8_t)dir;
    ring->head=(ring->head+1)%ring->capacity;  /* 循环推进写入位置 */
    if(ring->count<ring->capacity)ring->count++;
}

/**
 * @brief 获取环形缓冲区当前条目数
 * @param ring 环形缓冲区
 * @return     当前缓存的报文条数 (0 ~ CAN_RING_SIZE)
 */
int can_ring_count(const can_ring_t *ring){return ring?ring->count:0;}

/**
 * @brief 释放环形缓冲区内存
 *
 * 释放 calloc 分配的 buffer 内存并置空指针。
 * 可安全重复调用 (检查 ring 和 ring->buffer 是否为 NULL)。
 *
 * @param ring 环形缓冲区
 */
void can_ring_destroy(can_ring_t *ring){if(ring){free(ring->buffer);ring->buffer=NULL;}}

/**
 * @brief 加载 CAN 总线配置
 *
 * 从 config.ini [canbus] 节读取配置, 失败则使用硬编码默认值。
 * 可配置项: CAN 接口名称 (如 can0/can1)。
 * Socket 路径和环形缓冲区大小使用编译期宏定义, 不从配置文件读取。
 *
 * @param ctx       进程上下文 (输出: can_ifname, proc_cfg.socket_path)
 * @param conf_file 配置文件路径 (当前 main() 传入 NULL, 全部使用默认值)
 * @return          RET_OK
 */
int canbus_config_load(canbus_context_t *ctx,const char *conf_file){
    config_t conf;int loaded=0;
    if(conf_file&&config_load(&conf,conf_file)==0)loaded=1;
    /* CAN 接口名: config.ini [canbus] interface=can0 */
    if(loaded)config_get_str(&conf,"canbus","interface",CAN_DEFAULT_INTERFACE,ctx->can_ifname,sizeof(ctx->can_ifname));
    else strncpy(ctx->can_ifname,CAN_DEFAULT_INTERFACE,sizeof(ctx->can_ifname)-1);
    /* Socket 路径: 从 canbus_daemon.h 宏定义, 固定不可配置 */
    snprintf(ctx->proc_cfg.socket_path,sizeof(ctx->proc_cfg.socket_path),"%s",CAN_SOCKET_PATH);
    if(loaded)config_unload(&conf);
    LOG_INFO("canbus","Config: %s",ctx->can_ifname);return RET_OK;
}

/**
 * @brief CAN 总线守护进程主函数
 *
 * ============================================================================
 * 初始化阶段:
 *   1. 加载配置 (接口名称 can0)
 *   2. 初始化 CAN 报文环形缓冲区 (200 条容量)
 *   3. 打开 SocketCAN 套接字并绑定到 can0 接口
 *   4. 创建 UNIX Domain Socket 监听 (Qt UI 连接端点)
 *   5. 初始化协议解析器和接收环形缓冲区
 *   6. 注册信号处理 (SIGTERM/SIGINT 优雅退出)
 *
 * ============================================================================
 * 主循环 (poll 模式, 1000ms 超时):
 *
 *   Step 1 - CAN 报文接收:
 *     can_recv() 非阻塞循环读取, 每条报文:
 *       - rx_count++ 统计
 *       - 存入环形缓冲区 (dir=0 表示 RX)
 *       - 通过 Socket 打包为 MSG_CAN_DATA 帧转发给 Qt UI
 *
 *   Step 2 - 接受 Qt UI 连接:
 *     单客户端模式: 新连接踢旧连接
 *
 *   Step 3 - 接收 Qt UI 的 CAN 发送指令:
 *     recv() -> ringbuffer_write() -> protocol_unpack()
 *     MSG_CAN_SEND 处理 (关键: 栈垃圾 bug 修复):
 *       can_msg_t m;
 *       memset(&m, 0, sizeof(m));                    // [修复] 先清零整个结构体
 *       memcpy(&m, d, min(l, sizeof(m)));             // [修复] 逐字节拷贝, 不超过结构体大小
 *       can_send() 写入 CAN 总线 -> tx_count++ -> 存入环形缓冲区 (dir=1 表示 TX)
 *
 *       BUG 修复说明: 原先直接 cast 协议数据缓冲区为 can_msg_t*,
 *       未初始化的栈字节 (编译器对齐填充) 会随机污染 is_extended / is_remote 标志位,
 *       导致 Qt 发送的标准帧在 CAN 总线上变成扩展帧或远程帧。
 *       通过 memset 清零 + memcpy 拷贝固定大小, 保证未设置字段为 0。
 *
 *   Step 4 - 每秒统计日志:
 *     time(NULL) 秒级变化时输出 RX/TX 累计计数
 *
 * ============================================================================
 * 清理阶段:
 *   关闭客户端 -> 销毁环形缓冲区 -> 关闭 CAN socket -> 删除 Socket 文件 -> 关闭日志
 *
 * @return 0 正常退出, 1 初始化失败
 */
int main(int argc,char *argv[]){
    (void)argc;(void)argv;
    printf("[canbus] canbus_daemon starting...\n");

    /* ---- 初始化全局上下文 ---- */
    memset(&g_ctx,0,sizeof(g_ctx));g_ctx.client_fd=-1;  /* -1 表示无 Qt 连接 */
    log_init(CAN_LOG_PATH,LOG_INFO,1024*1024,3);
    LOG_INFO("canbus","===== canbus_daemon starting =====");

    /* ---- 加载配置 ---- */
    canbus_config_load(&g_ctx,NULL);

    /* ---- 初始化 CAN 环形缓冲区 (最近 200 条报文) ---- */
    can_ring_init(&g_ctx.ring,CAN_RING_SIZE);

    /* ---- 打开 SocketCAN 接口 ---- */
    g_ctx.can_fd=can_socket_open(g_ctx.can_ifname,&g_ctx.can_ifindex);
    if(g_ctx.can_fd<0){LOG_ERROR("canbus","CAN open failed");return 1;}

    /* ---- 创建 Socket 服务端 + 协议解析器 ---- */
    g_ctx.server_fd=sock_create_server(CAN_SOCKET_PATH);
    if(g_ctx.server_fd<0)return 1;
    ringbuffer_init(&g_ctx.recv_rb,4096);
    protocol_parser_init(&g_ctx.proto_parser);

    /* ---- 注册信号处理 (优雅退出) ---- */
    signal(SIGTERM,sig_handler);signal(SIGINT,sig_handler);
    LOG_INFO("canbus","Init complete. Loop starting...");

    /* ========================================================================
     *  主事件循环 (poll 模式)
     * ======================================================================== */
    time_t last=0;
    while(g_running){
        /* -- 构建 poll fd 数组 -- */
        struct pollfd pfds[4];int nfds=0;
        pfds[nfds].fd=g_ctx.can_fd;pfds[nfds].events=POLLIN;nfds++;     /* [0] CAN socket */
        pfds[nfds].fd=g_ctx.server_fd;pfds[nfds].events=POLLIN;nfds++;  /* [1] 监听 Socket */
        if(g_ctx.client_fd>=0){pfds[nfds].fd=g_ctx.client_fd;pfds[nfds].events=POLLIN;nfds++;} /* [2] 客户端 */
        poll(pfds,nfds,1000);  /* 1 秒超时: CAN 总线无需高频轮询 */

        /* -- Step 1: CAN 报文接收 -- */
        if(pfds[0].revents&POLLIN){
            can_msg_t m;
            while(can_recv(g_ctx.can_fd,&m)>0){  /* 非阻塞循环读取所有待处理帧 */
                g_ctx.rx_count++;
                can_ring_push(&g_ctx.ring,&m,0);   /* dir=0: 接收方向 */
                if(g_ctx.client_fd>=0)
                    sock_send_frame(g_ctx.client_fd,MSG_CAN_DATA,(uint8_t*)&m,sizeof(m));
            }
        }

        /* -- Step 2: 接受 Qt UI 连接 -- */
        if(pfds[1].revents&POLLIN){
            int fd=sock_accept(g_ctx.server_fd);
            if(fd>=0){
                if(g_ctx.client_fd>=0) sock_close(fd,NULL);  /* 已有连接, 拒绝新连接 */
                else{g_ctx.client_fd=fd;LOG_INFO("canbus","UI connected");}
            }
        }

        /* -- Step 3: 接收 Qt UI 的 CAN 发送指令 -- */
        if(nfds>=3&&(pfds[2].revents&(POLLIN|POLLHUP|POLLERR))){
            uint8_t buf[512];int n=sock_recv(g_ctx.client_fd,buf,sizeof(buf));
            if(n<=0){sock_close(g_ctx.client_fd,NULL);g_ctx.client_fd=-1;}
            else{
                ringbuffer_write(&g_ctx.recv_rb,buf,(uint32_t)n);
                uint8_t t,d[256];uint32_t l;
                while(protocol_unpack(&g_ctx.proto_parser,&g_ctx.recv_rb,&t,d,sizeof(d),&l)==0){
                    if(t==MSG_CAN_SEND&&l>=13){
                        /*
                         * [Bug 修复] 栈垃圾污染 can_msg_t 标志位
                         *
                         * 问题: 原先直接将协议数据 d 强制转换为 can_msg_t*,
                         *       未初始化的栈字节 (对齐填充) 会随机置位 is_extended 和 is_remote,
                         *       导致 Qt 发送的标准帧在 CAN 总线上随机变成扩展帧/远程帧。
                         *
                         * 修复: 先 memset 清零整个结构体, 再用 memcpy 逐字节拷贝
                         *       (限制拷贝量不超过 sizeof(can_msg_t), 防止溢出)。
                         *       确保所有未显式设置的字段为 0。
                         */
                        can_msg_t m; memset(&m,0,sizeof(m));
                        memcpy(&m,d,l<sizeof(m)?l:sizeof(m));
                        if(can_send(g_ctx.can_fd,&m)>0){
                            g_ctx.tx_count++;
                            can_ring_push(&g_ctx.ring,&m,1);  /* dir=1: 发送方向 */
                            LOG_INFO("canbus","TX: 0x%03X",m.can_id);
                        }
                    }
                }
            }
        }

        /* -- Step 4: 每秒统计日志 -- */
        time_t now=time(NULL);
        if(now!=last){
            last=now;
            LOG_INFO("canbus","RX=%u TX=%u",g_ctx.rx_count,g_ctx.tx_count);
        }
    }

    /* ---- 清理资源 ---- */
    LOG_INFO("canbus","Exiting...");
    if(g_ctx.client_fd>=0)sock_close(g_ctx.client_fd,NULL);
    ringbuffer_destroy(&g_ctx.recv_rb);
    can_ring_destroy(&g_ctx.ring);
    sock_close(g_ctx.server_fd,CAN_SOCKET_PATH);  /* 关闭监听 Socket 并删除文件 */
    can_socket_close(g_ctx.can_fd);                /* 关闭 CAN socket */
    log_close();
    return 0;
}
