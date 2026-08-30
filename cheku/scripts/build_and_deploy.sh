#!/bin/bash
# ==========================================
#  智能车载终端 — 一键编译 + 部署
#
#  用法:
#    bash scripts/build_and_deploy.sh
#
#  路径配置 (均可用环境变量覆盖, 不再硬编码到某个用户的 home 目录):
#    PROJECT      项目根目录
#                 默认: 本脚本所在目录的上一级 (自动推导)
#                 覆盖: PROJECT=/path/to/cheku bash scripts/build_and_deploy.sh
#
#    TOOLCHAIN    ARM 交叉编译工具链根目录 (其 bin/ 下有 arm-*-gcc 与 qmake)
#                 默认: /home/book/100ask_imx6ull-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot
#                 覆盖: TOOLCHAIN=/opt/my-toolchain bash scripts/build_and_deploy.sh
#
#    CROSS_COMPILE  交叉编译器前缀, 默认 arm-buildroot-linux-gnueabihf-
#    DEPLOY_BIN     开发板上的二进制目录, 默认 /usr/bin
#    DEPLOY_CONF    开发板上的配置目录, 默认 /etc/car_terminal
#
#  前置条件:
#    - 工具链已安装且 $TOOLCHAIN/bin 下有对应编译器
#    - 开发板通过 adb 连接 (adb devices 可见)
# ==========================================
set -e

# ---- 路径推导 ----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="${PROJECT:-$(dirname "$SCRIPT_DIR")}"
TOOLCHAIN="${TOOLCHAIN:-/home/book/100ask_imx6ull-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-buildroot-linux-gnueabihf-}"
DEPLOY_BIN="${DEPLOY_BIN:-/usr/bin}"
DEPLOY_CONF="${DEPLOY_CONF:-/etc/car_terminal}"

# ---- 前置检查 ----
if [ ! -d "$PROJECT" ]; then
    echo "错误: 项目目录不存在: $PROJECT" >&2
    exit 1
fi
if [ ! -x "$TOOLCHAIN/bin/${CROSS_COMPILE}gcc" ]; then
    echo "错误: 找不到交叉编译器: $TOOLCHAIN/bin/${CROSS_COMPILE}gcc" >&2
    echo "请设置 TOOLCHAIN 环境变量指向工具链根目录, 例如:" >&2
    echo "  TOOLCHAIN=/opt/sdk bash scripts/build_and_deploy.sh" >&2
    exit 1
fi

echo "项目目录:   $PROJECT"
echo "工具链:     $TOOLCHAIN"
echo "部署目标:   $DEPLOY_BIN  $DEPLOY_CONF"
echo ""

# 1. 设置环境
echo "=== [1/6] 设置交叉编译环境 ==="
export ARCH=arm
export CROSS_COMPILE
export PATH="$PATH:$TOOLCHAIN/bin"

# 2. 编译全部后台进程
echo ""
echo "=== [2/6] 编译后台进程 ==="
cd "$PROJECT"
make clean 2>/dev/null || true
make
echo "后台进程编译完成:"
ls -l build/bin/

# 3. 交叉编译 Qt UI
echo ""
echo "=== [3/6] 编译 Qt UI ==="
cd "$PROJECT/qt_ui"
make clean 2>/dev/null || true
"$TOOLCHAIN/bin/qmake"
make
echo "Qt UI 编译完成:"
ls -l car_ui

# 4. 推送后台进程
echo ""
echo "=== [4/6] 推送后台进程 ==="
adb push "$PROJECT"/build/bin/* "$DEPLOY_BIN/"

# 5. 推送 Qt UI、配置与启停脚本
echo ""
echo "=== [5/6] 推送 Qt UI + 配置 + 脚本 ==="
adb push "$PROJECT/qt_ui/car_ui" "$DEPLOY_BIN/"
adb push "$PROJECT/config/config.ini" "$DEPLOY_CONF/"
adb push "$PROJECT/scripts/start_all.sh" "$DEPLOY_BIN/"
adb push "$PROJECT/scripts/stop_all.sh" "$DEPLOY_BIN/"

# 6. 完成
echo ""
echo "=== [6/6] 完成 ==="
echo ""
echo "文件已推送到开发板。在开发板上执行:"
echo "  chmod +x $DEPLOY_BIN/*.sh $DEPLOY_BIN/*_daemon $DEPLOY_BIN/car_ui"
echo "  start_all.sh            # guard 托管模式 (推荐)"
echo "  start_all.sh --manual   # 手动模式 (调试用)"
