#!/bin/bash
# ==========================================
#  智能车载终端 — 一键编译 + 部署
#  用法: cd ~/Desktop/cheku && bash scripts/build_and_deploy.sh
# ==========================================
set -e

PROJECT=~/Desktop/cheku
TOOLCHAIN=/home/book/100ask_imx6ull-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot

# 1. 设置环境
echo "=== [1/6] 设置交叉编译环境 ==="
export ARCH=arm
export CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
export PATH=$PATH:$TOOLCHAIN/bin

# 2. 编译全部后台进程
echo ""
echo "=== [2/6] 编译后台进程 ==="
cd $PROJECT
make clean 2>/dev/null || true
make
echo "后台进程编译完成:"
ls -l build/bin/

# 3. 交叉编译 Qt UI
echo ""
echo "=== [3/6] 编译 Qt UI ==="
cd $PROJECT/qt_ui
make clean 2>/dev/null || true
$TOOLCHAIN/bin/qmake
make
echo "Qt UI 编译完成:"
ls -l car_ui

# 4. 推送后台进程
echo ""
echo "=== [4/6] 推送后台进程 ==="
adb push $PROJECT/build/bin/* /usr/bin/

# 5. 推送 Qt UI 和配置
echo ""
echo "=== [5/6] 推送 Qt UI + 配置 ==="
adb push $PROJECT/qt_ui/car_ui /usr/bin/
adb push $PROJECT/config/config.ini /etc/car_terminal/
adb push $PROJECT/scripts/start_all.sh /usr/bin/
adb push $PROJECT/scripts/stop_all.sh /usr/bin/
# 6. 完成
echo ""
echo "=== [6/6] 完成 ==="
echo ""
echo "文件已推送到开发板。在开发板上执行:"
echo "  chmod +x /usr/bin/*.sh /usr/bin/*_daemon /usr/bin/car_ui"
echo "  start_all.sh"
