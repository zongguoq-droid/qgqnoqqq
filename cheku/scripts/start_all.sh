#!/bin/sh
# ==========================================
#  智能车载终端 — 一键启动
#  用法: start_all.sh
#  停止: stop_all.sh
# ==========================================

export LD_LIBRARY_PATH=/usr/lib
BIN=/usr/bin
CONF=/etc/car_terminal/config.ini

# 1. 创建目录
mkdir -p /var/log/car_terminal /record /music

echo "========================================"
echo "  智能车载终端 - 启动中..."
echo "========================================"

# 2. 配置 CAN
echo "[1/5] 配置 CAN..."
ip link set can0 down 2>/dev/null
ip link set can0 type can bitrate 500000 loopback on restart-ms 100 2>/dev/null
ip link set can0 up 2>/dev/null

# 3. 启动后台进程
echo "[2/5] gps_daemon...";    $BIN/gps_daemon    $CONF &
echo "[3/5] dvr_daemon...";    $BIN/dvr_daemon    $CONF &
echo "[4/5] av_daemon...";     $BIN/av_daemon     $CONF &
echo "[5/5] input_daemon...";  $BIN/input_daemon  $CONF &
echo "       canbus_daemon..."; $BIN/canbus_daemon $CONF &

# 4. 等 Socket 就绪
echo "等待后台就绪..."
for i in 1 2 3 4 5; do
    sleep 1
    c=$(ls /tmp/car_*.sock 2>/dev/null | wc -l)
    echo "  Socket: $c/5"
    [ $c -ge 5 ] && break
done

# 5. 系统状态
echo ""
echo "=== 运行中的进程 ==="
ps | grep daemon | grep -v grep

# 6. 启动 Qt
echo ""
echo "=== 启动 Qt 界面 ==="
echo -e "\033[9;0]" > /dev/tty0
export QT_QPA_GENERIC_PLUGINS=tslib:/dev/input/event1
export QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0
export QT_QPA_FONTDIR=/usr/lib/fonts/
$BIN/car_ui &

sleep 2
echo ""
echo "========================================"
echo "  全部启动完成!"
echo "  停止: stop_all.sh"
echo "========================================"
