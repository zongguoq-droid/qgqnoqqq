/**
 * @file    ringbuffer.c
 * @brief   环形缓冲区 - 实现
 *
 * 实现策略:
 *   1. 容量向上调整为 2 的幂 (2^n)
 *      - 优点: 取模运算 index % capacity 可以优化为 index & (capacity-1)
 *      - 位运算比除法快 5~10 倍 (528MHz ARM 上没有硬件除法器)
 *   2. "非覆盖" 写入策略
 *      - 空间不够时只写能容纳的部分，不覆盖未读数据
 *      - 保证数据完整性优先于实时性
 *   3. read_index 和 write_index 各自只被一方修改
 *      - 单消费者只修改 read_index, 单生产者只修改 write_index
 *      - 因此不需要锁 (无竞态条件)
 *
 * 取模优化原理:
 *   对于任意正整数 x 和 2^n:
 *     x % (2^n) ≡ x & (2^n - 1)
 *
 *   例如: 10 % 8 = 2,  10 & 7 = 0b1010 & 0b0111 = 0b0010 = 2
 *   例如: 15 % 8 = 7,  15 & 7 = 0b1111 & 0b0111 = 0b0111 = 7
 *
 *   为什么 ARM 上这个优化重要?
 *     Cortex-A7 没有硬件除法指令，% 操作要通过软件除法 (几十个周期)
 *     而 & 操作只需 1 个周期
 *
 * 学习要点:
 *   1. 环形缓冲区的指针管理 (read/write 指针)
 *   2. 取模运算优化: 2^n 对齐 + 位掩码
 *   3. 内存拷贝策略: 分两段拷贝处理回绕
 *   4. 嵌入式性能: 避免除法、避免动态分配、预分配固定大小
 */

#include "ringbuffer.h"
#include <stdlib.h>   /* malloc, free */
#include <string.h>   /* memcpy */
#include <stdint.h>

/* ---- 内部辅助 ---- */

/**
 * @brief 将一个整数向上调整为 2 的幂
 *
 * 算法: 不断将最高位以下的位全部置 1，然后加 1。
 * 这是 Linux 内核 fls()/roundup_pow_of_two() 的经典实现。
 *
 * 示例:
 *   16 → return 16  (已经是 2 的幂, 不改)
 *   17 → return 32
 *   31 → return 32
 *   100 → return 128
 *
 * @param n  输入整数 (>0)
 * @return   >=n 的最小 2 的幂
 */
static uint32_t round_up_power_of_two(uint32_t n)
{
    /* 已经是 2 的幂，直接返回 */
    if (n && (n & (n - 1)) == 0) {
        return n;
    }

    /* 经典算法: 逐步将低位全部置 1 */
    n--;                          /* 防止 n 本身就是 2 的幂时翻倍 */
    n |= n >> 1;   /* 最高2位变1 */
    n |= n >> 2;   /* 最高4位变1 */
    n |= n >> 4;   /* 最高8位变1 */
    n |= n >> 8;   /* 最高16位变1 */
    n |= n >> 16;  /* 最高32位变1 (支持 32-bit 输入) */

    return n + 1;  /* 加 1 得到 2 的幂 */
}

/**
 * @brief 获取环形缓冲区索引掩码
 *
 * 对于容量为 2^n 的缓冲区，取模 = &这个掩码
 * 例如: capacity=1024 → mask=1023 (0x3FF)
 */
static inline uint32_t mask(const ringbuffer_t *rb)
{
    return rb->capacity - 1;
}

/* ================================================================
 *  API 实现
 * ================================================================ */

int ringbuffer_init(ringbuffer_t *rb, uint32_t capacity)
{
    if (!rb) {
        return -1;
    }

    /* 最小容量保护 */
    if (capacity < 16) {
        capacity = 16;
    }

    /* 向上取整到 2 的幂 */
    capacity = round_up_power_of_two(capacity);

    /* 分配底层缓冲区:
     *   calloc = malloc + memset(0)
     *   将内存清零有助于调试（未写入区域为 0x00，便于 hexdump 查看） */
    rb->buffer = (uint8_t*)calloc(1, capacity);
    if (!rb->buffer) {
        return -1;  /* 内存不足 */
    }

    rb->capacity    = capacity;
    rb->read_index  = 0;
    rb->write_index = 0;
    rb->data_size   = 0;

    return 0;
}

void ringbuffer_destroy(ringbuffer_t *rb)
{
    if (rb && rb->buffer) {
        free(rb->buffer);
        rb->buffer   = NULL;
        rb->capacity = 0;
        rb->read_index  = 0;
        rb->write_index = 0;
        rb->data_size   = 0;
    }
}

void ringbuffer_reset(ringbuffer_t *rb)
{
    if (!rb) return;
    rb->read_index  = 0;
    rb->write_index = 0;
    rb->data_size   = 0;
    /* 不释放 buffer，方便复用 */
}

uint32_t ringbuffer_write(ringbuffer_t *rb, const uint8_t *data, uint32_t len)
{
    uint32_t free_space;
    uint32_t write_pos;
    uint32_t first_chunk;
    uint32_t cap_mask;

    if (!rb || !data || len == 0) {
        return 0;
    }

    /* 1. 计算可写入长度 (不超过剩余空间) */
    free_space = ringbuffer_free_space(rb);
    if (len > free_space) {
        len = free_space;  /* 截断: 只写入能容纳的部分 */
    }
    if (len == 0) {
        return 0;
    }

    /* 2. 计算写入位置 (使用位掩码避免除法) */
    cap_mask  = mask(rb);
    write_pos = rb->write_index & cap_mask;

    /* 3. 写入数据 (分两段处理回绕情况)
     *
     * 环形缓冲区在物理上是一段连续内存，但逻辑上是环形的。
     * 当 write_pos + len > capacity 时，写入会"回绕"到开头。
     * 这时需要分两次 memcpy:
     *   第一次: write_pos → buffer 末尾
     *   第二次: buffer 开头 → 剩余数据
     *
     * 示意图:
     *   buffer: [D][D][D][ ][ ][ ][ ][ ]  capacity=8
     *                      ^w_pos=3
     *   要写入 6 字节:
     *     第一次: 写入 5 字节到 [3..7]
     *     第二次: 写入 1 字节到 [0]
     */

    /* 第一段: 从 write_pos 到缓冲区末尾 */
    first_chunk = rb->capacity - write_pos;
    if (first_chunk > len) {
        first_chunk = len;
    }
    memcpy(rb->buffer + write_pos, data, first_chunk);

    /* 第二段: 如果还有剩余数据，从缓冲区开头继续写 (回绕) */
    if (len > first_chunk) {
        memcpy(rb->buffer, data + first_chunk, len - first_chunk);
    }

    /* 4. 更新写指针和数据计数
     *    write_index 不断递增 (不取模), 只在实际访问 buffer 时才取模
     *    这样 read_index 和 write_index 的差值直接等于 data_size
     *    (不需要额外维护 data_size，但为了代码清晰保留此字段) */
    rb->write_index += len;
    rb->data_size   += len;

    return len;
}

uint32_t ringbuffer_read(ringbuffer_t *rb, uint8_t *buf, uint32_t len)
{
    uint32_t avail;
    uint32_t read_pos;
    uint32_t first_chunk;
    uint32_t cap_mask;

    if (!rb || !buf || len == 0) {
        return 0;
    }

    /* 1. 计算可读取长度 (不超过可用数据) */
    avail = ringbuffer_available(rb);
    if (len > avail) {
        len = avail;
    }
    if (len == 0) {
        return 0;
    }

    /* 2. 计算读位置 */
    cap_mask = mask(rb);
    read_pos = rb->read_index & cap_mask;

    /* 3. 读取数据 (分两段处理回绕, 与 write 对称) */
    first_chunk = rb->capacity - read_pos;
    if (first_chunk > len) {
        first_chunk = len;
    }
    memcpy(buf, rb->buffer + read_pos, first_chunk);

    if (len > first_chunk) {
        memcpy(buf + first_chunk, rb->buffer, len - first_chunk);
    }

    /* 4. 更新读指针 */
    rb->read_index += len;
    rb->data_size  -= len;

    return len;
}

uint32_t ringbuffer_peek(const ringbuffer_t *rb, uint8_t *buf, uint32_t len)
{
    uint32_t avail;
    uint32_t read_pos;
    uint32_t first_chunk;
    uint32_t cap_mask;

    if (!rb || !buf || len == 0) {
        return 0;
    }

    /* 与 ringbuffer_read 完全相同的逻辑，只是不修改 read_index 和 data_size */
    avail = ringbuffer_available(rb);
    if (len > avail) {
        len = avail;
    }
    if (len == 0) {
        return 0;
    }

    cap_mask = mask(rb);
    read_pos = rb->read_index & cap_mask;

    first_chunk = rb->capacity - read_pos;
    if (first_chunk > len) {
        first_chunk = len;
    }
    memcpy(buf, rb->buffer + read_pos, first_chunk);

    if (len > first_chunk) {
        memcpy(buf + first_chunk, rb->buffer, len - first_chunk);
    }

    return len;
}

uint32_t ringbuffer_skip(ringbuffer_t *rb, uint32_t len)
{
    uint32_t avail;

    if (!rb) return 0;

    avail = ringbuffer_available(rb);
    if (len > avail) {
        len = avail;
    }

    rb->read_index += len;
    rb->data_size  -= len;

    return len;
}

uint32_t ringbuffer_available(const ringbuffer_t *rb)
{
    if (!rb) return 0;
    return rb->data_size;
}

uint32_t ringbuffer_free_space(const ringbuffer_t *rb)
{
    if (!rb) return 0;
    return rb->capacity - rb->data_size;
}

int ringbuffer_is_empty(const ringbuffer_t *rb)
{
    return (rb && rb->data_size == 0) ? 1 : 0;
}

int ringbuffer_is_full(const ringbuffer_t *rb)
{
    return (rb && rb->data_size >= rb->capacity) ? 1 : 0;
}
