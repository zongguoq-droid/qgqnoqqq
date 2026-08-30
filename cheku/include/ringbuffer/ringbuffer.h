/**
 * @file    ringbuffer.h
 * @brief   环形缓冲区 (Ring Buffer / Circular Buffer) - 头文件
 *
 * 环形缓冲区是一种固定大小的 FIFO (先进先出) 数据结构，
 * 使用两个指针 (读/写) 在连续内存上循环移动。
 *
 * 特点:
 *   - 固定大小，不动态分配内存 (适合嵌入式)
 *   - 写入快于读取时自动覆盖旧数据 (可选行为)
 *   - 单生产者单消费者场景可无锁使用
 *   - 常用于: 串口数据缓冲、音频数据缓冲、Socket 接收缓冲
 *
 * 本项目使用场景:
 *   - protocol 模块: 接收到的原始字节先存 ringbuffer, 再逐帧解析
 *   - log 模块: 日志输出缓冲, 批量写入减少 eMMC 擦写
 *
 * 内存布局示例 (capacity=8):
 *
 *  初始状态:                         写入 5 字节 "hello" 后:
 *   [ ][ ][ ][ ][ ][ ][ ][ ]         [h][e][l][l][o][ ][ ][ ]
 *   ^r                               ^r              ^w
 *   ^w
 *   read=0, write=0, size=0          read=0, write=5, size=5
 *
 *  读取 3 字节后:                     再写入 5 字节 "world" (回绕):
 *   [ ][ ][ ][l][l][o][ ][ ]         [r][l][d][l][l][o][w][o]
 *        ^r         ^w                   ^w           ^r
 *   read=3, write=5, size=2          read=3, write=2, size=7 (r-w 顺序)
 *
 * 学习要点:
 *   1. 取模运算优化: 容量为 2^n 时, index % cap ≡ index & (cap-1)
 *   2. 满/空判断: (write+1) % cap == read 表示满, write == read 表示空
 *   3. 生产者-消费者模型: 无需锁 (单生产者+单消费者时)
 */

#ifndef _RINGBUFFER_H_
#define _RINGBUFFER_H_

#include <stdint.h>
#include <stddef.h>

/**
 * @brief 环形缓冲区结构体
 *
 * 所有字段为内部使用，通过 API 函数操作。
 * 调用者负责分配 ringbuffer_t 结构体本身的内存
 * (可放在栈上或全局数据段)，但 buffer 通过 ringbuffer_init 动态分配。
 */
typedef struct ringbuffer {
    uint8_t    *buffer;       /* 数据缓冲区 (动态分配) */
    uint32_t    capacity;     /* 缓冲区总容量 (字节数) */
    uint32_t    read_index;   /* 读指针 (只被消费者修改) */
    uint32_t    write_index;  /* 写指针 (只被生产者修改) */
    uint32_t    data_size;    /* 当前有效数据字节数 = (write - read) mod capacity */
} ringbuffer_t;

/* ================================================================
 *  API 函数声明
 * ================================================================ */

/**
 * @brief 初始化环形缓冲区
 *
 * 分配 capacity 字节的底层缓冲区。
 * 注意: capacity 会被向上调整为 2 的幂 (方便取模优化)。
 *
 * @param rb        指向 ringbuffer_t 结构体的指针 (调用者分配)
 * @param capacity  期望的缓冲区容量 (字节数), 最小 16
 * @return          0 = 成功, -1 = 失败 (参数错误或内存不足)
 *
 * 使用示例:
 *   ringbuffer_t rb;
 *   if (ringbuffer_init(&rb, 1024) != 0) {
 *       // 内存不足处理
 *   }
 */
int ringbuffer_init(ringbuffer_t *rb, uint32_t capacity);

/**
 * @brief 销毁环形缓冲区，释放底层 buffer 内存
 * @param rb  已初始化的环形缓冲区
 */
void ringbuffer_destroy(ringbuffer_t *rb);

/**
 * @brief 重置环形缓冲区 (清空所有数据，不释放内存)
 * @param rb  已初始化的环形缓冲区
 *
 * 等效于丢弃所有未读数据，读写指针都归零。
 * 常用于协议解析出错需要重新同步时。
 */
void ringbuffer_reset(ringbuffer_t *rb);

/**
 * @brief 向缓冲区写入数据
 *
 * @param rb    环形缓冲区
 * @param data  要写入的数据
 * @param len   数据长度 (字节)
 * @return      实际写入的字节数: 0 ~ len
 *              (缓冲区满时可能少于 len)
 *
 * 注意: 如果缓冲区空间不足, 只写入能容纳的部分, 不会覆盖旧数据。
 *       这是"非覆盖模式"，适合协议解析场景 (丢失旧数据比覆盖新数据更安全)。
 */
uint32_t ringbuffer_write(ringbuffer_t *rb, const uint8_t *data, uint32_t len);

/**
 * @brief 从缓冲区读取数据 (会消费数据, 读指针后移)
 *
 * @param rb    环形缓冲区
 * @param buf   读取数据存放的目标缓冲区
 * @param len   期望读取的字节数
 * @return      实际读取的字节数: 0 ~ len
 *              (缓冲区空时返回 0)
 */
uint32_t ringbuffer_read(ringbuffer_t *rb, uint8_t *buf, uint32_t len);

/**
 * @brief 查看缓冲区数据 (不消费数据, 读指针不变)
 *
 * 用于"先看再决定是否读"的场景，如协议帧解析:
 *   先 peek 帧头, 确认是完整帧后再 read。
 *
 * @param rb    环形缓冲区
 * @param buf   目标缓冲区
 * @param len   期望查看的字节数
 * @return      实际可查看的字节数
 */
uint32_t ringbuffer_peek(const ringbuffer_t *rb, uint8_t *buf, uint32_t len);

/**
 * @brief 跳过指定字节数 (不读取数据，只移动读指针)
 *
 * 等效于 read 但不复制数据。常用于丢弃数据。
 *
 * @param rb   环形缓冲区
 * @param len  要跳过的字节数
 * @return     实际跳过的字节数
 */
uint32_t ringbuffer_skip(ringbuffer_t *rb, uint32_t len);

/**
 * @brief 获取缓冲区中可读取的数据量
 * @param rb  环形缓冲区
 * @return    可读字节数
 */
uint32_t ringbuffer_available(const ringbuffer_t *rb);

/**
 * @brief 获取缓冲区剩余可写空间
 * @param rb  环形缓冲区
 * @return    可写字节数 (capacity - available)
 */
uint32_t ringbuffer_free_space(const ringbuffer_t *rb);

/**
 * @brief 检查缓冲区是否为空
 * @param rb  环形缓冲区
 * @return    1 = 空, 0 = 非空
 */
int ringbuffer_is_empty(const ringbuffer_t *rb);

/**
 * @brief 检查缓冲区是否已满
 * @param rb  环形缓冲区
 * @return    1 = 满, 0 = 未满
 */
int ringbuffer_is_full(const ringbuffer_t *rb);

#endif /* _RINGBUFFER_H_ */
