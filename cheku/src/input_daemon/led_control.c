/**
 * @file    led_control.c
 * @brief   用户 LED 控制 — 内核驱动 /dev/100ask_led 字符设备
 *
 * 驱动接口 (内核 LED 字符设备):
 *   write(fd, [led_index, value], 2) → value: 0=亮, 1=灭
 *   read(fd, [led_index], 2)         → 返回状态
 */

#include "led_control.h"
#include "log/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int g_fd = -1;

int led_init(void)
{
    g_fd = open("/dev/100ask_led", O_RDWR);
    if (g_fd < 0) {
        LOG_WARN("led", "Cannot open /dev/100ask_led: %s (请确认 LED 驱动已加载)", strerror(errno));
        return -1;
    }
    LOG_INFO("led", "Device opened: /dev/100ask_led (fd=%d)", g_fd);
    return 0;
}

int led_on(int which)
{
    char buf[2];
    if (g_fd < 0) return -1;
    buf[0] = (char)which;
    buf[1] = 0;  /* 0 = 亮 */
    return (write(g_fd, buf, 2) == 2) ? 0 : -1;
}

int led_off(int which)
{
    char buf[2];
    if (g_fd < 0) return -1;
    buf[0] = (char)which;
    buf[1] = 1;  /* 1 = 灭 */
    return (write(g_fd, buf, 2) == 2) ? 0 : -1;
}

void led_deinit(void)
{
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
}
