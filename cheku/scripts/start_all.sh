#!/bin/sh
# ==========================================
#  智能车载终端 — 一键启动
#
#  用法:
#    start_all.sh              guard 托管模式 (默认)
#    start_all.sh --manual     手动模式
#    start_all.sh --help       查看帮助
#
#  两种模式的区别:
#    guard 托管模式 (默认)
#      只启动 guard_daemon, 由它 fork+exec 拉起 5 个子进程,
#      并持续监控: 子进程崩溃会自动重启 (60 秒窗口内最多 5 次)。
#      停止时 guard 会反向停止全部子进程, 不会遗漏。
#      这是推荐模式, 也是嵌入式设备长期运行的标准做法。
#
#    手动模式 (--manual)
#      脚本逐个启动 5 个 daemon, 不使用 guard_daemon。
#      无自动重启能力, 进程崩溃后需要人工介入。
#      仅用于调试, 或 guard_daemon 出现异常时回退。
#
#  注意: 两种模式不可混用。guard 若发现同名子进程已在运行会重复拉起,
#        切换模式前请先执行 stop_all.sh。
#
#  停止: stop_all.sh
# ==========================================

export LD_LIBRARY_PATH=/usr/lib
BIN=/usr/bin
CONF=/etc/car_terminal/config.ini

# ---- 解析参数 ----
case "$1" in
    --manual) MODE=manual ;;
    --guard)  MODE=guard  ;;
    --help|-h)
        echo "用法: start_all.sh [--guard | --manual | --help]"
        echo "  --guard   (默认) 由 guard_daemon 托管全部子进程, 支持崩溃自动重启"
        echo "  --manual  脚本逐个启动 daemon, 不使用 guard (仅调试用)"
        echo "  --help    显示本帮助"
        exit 0
        ;;
    "")       MODE=guard  ;;
    *)        echo "未知参数: $1 (用 --help 查看用法)"; exit 1 ;;
esac

# 1. 创建运行时目录
mkdir -p /var/log/car_terminal /record /music

echo "========================================"
echo "  智能车载终端 - 启动中"
echo "  模式: $MODE"
echo "========================================"

# 2. 配置 CAN
echo "[1/5] 配置 CAN..."
ip link set can0 down 2>/dev/null
ip link set can0 type can bitrate 500000 loopback on restart-ms 100 2>/dev/null
ip link set can0 up 2>/dev/null

# 3. 启动后台进程
if [ "$MODE" = "guard" ]; then
    # ---- guard 托管模式 ----
    # guard_daemon 会自行 daemonize 并拉起子进程, 子进程启动延迟最大 3.5s
    echo "[2/5] guard_daemon (由它拉起 5 个子进程)..."
    $BIN/guard_daemon $CONF
    EXPECT_SOCK=6      # 5 个 daemon + guard 自身
else
    # ---- 手动模式 ----
    echo "[2/5] 启动各 daemon..."
    $BIN/gps_daemon    $CONF &
    $BIN/dvr_daemon    $CONF &
    $BIN/av_daemon     $CONF &
    $BIN/input_daemon  $CONF &
    $BIN/canbus_daemon $CONF &
    echo "      已启动 5 个 daemon (无自动重启)"
    EXPECT_SOCK=5
fi

# 4. 等待 Socket 就绪
# guard 模式下子进程是错开启动的 (dvr_daemon 最晚 +3500ms),
# 因此超时给到 15 秒, 避免误判为启动失败。
echo "[3/5] 等待后台服务就绪 (期望 $EXPECT_SOCK 个 socket)..."
i=0
while [ $i -lt 15 ]; do
    sleep 1
    i=$((i + 1))
    c=$(ls /tmp/car_*.sock 2>/dev/null | wc -l)
    echo "  Socket: $c/$EXPECT_SOCK  (${i}s)"
    [ "$c" -ge "$EXPECT_SOCK" ] && break
done
if [ "$c" -lt "$EXPECT_SOCK" ]; then
    echo "  警告: 只就绪 $c/$EXPECT_SOCK 个 socket, 部分服务可能启动失败"
    echo "  排查: cat /var/log/car_terminal/*.log"
fi

# 5. 显示系统状态
echo ""
echo "========================================"
echo "  运行中的进程"
echo "========================================"
ps | grep -E 'daemon|car_ui' | grep -v grep

# 6. 启动 Qt 界面
echo ""
echo "[4/5] 启动 Qt 界面..."
echo -e "\033[9;0]" > /dev/tty0
export QT_QPA_GENERIC_PLUGINS=tslib:/dev/input/event1
export QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0
export QT_QPA_FONTDIR=/usr/lib/fonts/
$BIN/car_ui &

sleep 2

# 7. 完成
echo ""
echo "[5/5] 完成"
echo "========================================"
echo "  全部启动完成"
if [ "$MODE" = "guard" ]; then
    echo "  服务由 guard_daemon 托管 (崩溃自动重启)"
else
    echo "  手动模式: 无自动重启, 崩溃后需手动拉起"
fi
echo "  停止: stop_all.sh"
echo "========================================"
