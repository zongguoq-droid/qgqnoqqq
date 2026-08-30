/**
 * @file    crc8.h
 * @brief   CRC8 校验算法 - 头文件
 *
 * CRC (Cyclic Redundancy Check, 循环冗余校验) 是一种常用的数据校验算法。
 * 本模块实现 CRC-8，用于 IPC 通信消息帧的完整性校验。
 *
 * 算法参数:
 *   多项式 (Polynomial): 0x07 (x^8 + x^2 + x + 1)
 *   初始值 (Init):       0x00
 *   输入反转 (RefIn):    false
 *   输出反转 (RefOut):   false
 *   异或输出 (XorOut):   0x00
 *
 * 这个参数组合通常称为 CRC-8。
 *
 * 为什么用 CRC8 而不是 CRC16/CRC32?
 *   - 本项目的消息通常 < 256 字节，CRC8 足够检出错误
 *   - CRC8 只需 256 字节查找表，内存占用极小
 *   - 计算量小，适合 528MHz 嵌入式 CPU
 *
 * 学习要点:
 *   1. 查表法 vs 逐位计算法 — 空间换时间的经典案例
 *   2. CRC 原理: 多项式除法在 GF(2) 域上的实现
 *   3. 为什么 CRC 能检测 1bit 错误和突发错误
 */

#ifndef _CRC8_H_
#define _CRC8_H_

#include <stdint.h>   /* uint8_t, uint32_t */
#include <stddef.h>   /* size_t */

/**
 * @brief 计算 CRC8 校验值 (查表法, 高性能版本)
 *
 * 对给定数据逐字节查表计算 CRC8。
 * 时间复杂度 O(n)，空间复杂度 O(256) (查找表)。
 *
 * @param data   待校验的数据指针
 * @param len    数据长度 (字节数)
 * @param crc    初始 CRC 值 (首次调用传 0x00，追加计算时传上次结果)
 * @return       CRC8 校验值 (0x00 ~ 0xFF)
 *
 * 使用示例:
 *   // 计算一段数据的 CRC8
 *   uint8_t crc = crc8_calculate(data, 100, 0x00);
 *
 *   // 分段计算 (如数据分多次到达)
 *   uint8_t crc = 0x00;
 *   crc = crc8_calculate(chunk1, 50, crc);
 *   crc = crc8_calculate(chunk2, 50, crc);
 *   // 结果与一次性 crc8_calculate(whole, 100, 0x00) 相同
 */
uint8_t crc8_calculate(const uint8_t *data, size_t len, uint8_t crc);

#endif /* _CRC8_H_ */
