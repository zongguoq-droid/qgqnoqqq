/**
 * @file    led_control.h
 * @brief   用户 LED 控制 - 头文件 (内核驱动版本)
 *
 * 使用开发板 /dev/100ask_led 字符设备驱动:
 *   write(fd, [led_index, value], 2) — value: 0=on, 1=off
 *   read(fd, [led_index], 2)         — 返回 [led_index, status]
 */

#ifndef _LED_CONTROL_H_
#define _LED_CONTROL_H_

int  led_init(void);                  /* 打开 /dev/100ask_led */
int  led_on(int which);               /* 点亮 led[which] */
int  led_off(int which);              /* 熄灭 led[which] */
void led_deinit(void);                /* 关闭设备 */

#endif
