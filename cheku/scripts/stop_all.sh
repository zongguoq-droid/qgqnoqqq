#!/bin/sh
# ==========================================
#  智能车载终端 — 一键停止
#
#  用法: stop_all.sh
#
#  停止顺序:
#    1. 先停 Qt 界面
#    2. 再停 guard_daemon (它会优雅地反向停止全部子进程:
#       SIGTERM → 等待 3s → SIGKILL, 并 waitpid 回收避免僵尸进程)
#    3. 兜底: 直接 kill 仍在运行的 daemon
#       (guard 未运行或已退出时, 子进程不会被第 2 步带走)
#    4. 关闭 CAN 接口, 清理 socket 与临时文件
# ==========================================

BIN=/usr/bin

echo "停止所有服务..."

# 1. 停止 Qt 界面
killall car_ui 2>/dev/null

# 2. 停止 guard_daemon
# 它收到 SIGTERM 后会依次停止并回收所有子进程, 因此要先停它
if killall -q guard_daemon 2>/dev/null; then
    echo "  已通知 guard_daemon 停止子进程..."
    # 等待 guard 完成子进程的优雅退出 (它内部每个子进程最多等 3 秒)
    sleep 4
fi

# 3. 兜底: 停止可能残留的 daemon
#    guard 未运行时, 子进程是直接启动的, 必须在这里清理
killall gps_daemon dvr_daemon input_daemon canbus_daemon 2>/dev/null
sleep 1

# av_daemon 可能在 ALSA 阻塞中, SIGTERM 无效时需要强杀
killall av_daemon 2>/dev/null
sleep 1
killall -9 av_daemon car_ui 2>/dev/null
killall -9 gps_daemon dvr_daemon input_daemon canbus_daemon guard_daemon 2>/dev/null

# 4. 关闭 CAN 并清理运行时残留
ip link set can0 down 2>/dev/null
rm -f /tmp/car_*.sock /tmp/dvr_preview.jpg /tmp/av_volume /tmp/av_track

# 5. 确认
left=$(ps | grep -E 'daemon|car_ui' | grep -v grep | wc -l)
if [ "$left" -gt 0 ]; then
    echo "警告: 仍有 $left 个进程未停止:"
    ps | grep -E 'daemon|car_ui' | grep -v grep
else
    echo "已全部停止"
fi
