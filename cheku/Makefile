# ================================================================
#  智能车载终端 — 顶层 Makefile
#
#  使用方式:
#   make                    — 编译全部模块 (ARM 交叉编译)
#   make HOST=1             — 编译全部模块 (x86 本地)
#   make common             — 只编译 common 公共库
#   make gps / can / ...    — 只编译指定模块
#   make test               — 编译并运行全部单元测试 (tests/)
#   make clean              — 清理全部编译产物
#   make deploy             — 打包部署文件到 build/deploy/
#
#  目录约定:
#    src/      各守护进程与公共库的实现 (.c)
#    include/  公共头文件 (.h), 所有模块通过 -Iinclude 引用
#    tests/    全项目单元测试, 由 tests/Makefile 统一管理
#    config/   运行时配置文件
#    docs/     设计文档与各模块学习文档
#    scripts/  编译 / 部署 / 启停脚本
#    qt_ui/    Qt 触控界面工程
#    build/    编译输出 (已 gitignore)
#
#  交叉编译 (环境变量方式):
#   export ARCH=arm
#   export CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
#   export PATH=$PATH:/home/book/100ask_imx6ull-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/bin
#   make
# ================================================================

# ---- 工具链 ----
HOST          ?= 0

ifeq ($(HOST),1)
  # x86 本地编译 (虚拟机测试)
  CROSS_COMPILE ?=
  CC     = gcc
  STRIP  = strip
else
  # ARM 交叉编译 (默认)
  CROSS_COMPILE ?= arm-buildroot-linux-gnueabihf-
  CC     = $(CROSS_COMPILE)gcc
  STRIP  = $(CROSS_COMPILE)strip
endif

RM            = rm -f
MKDIR         = mkdir -p
CP            = cp -r

# ---- 目录 ----
SRC_DIR       = src
INC_DIR       = include
COMMON_DIR    = $(SRC_DIR)/common
COMMON_LIB    = $(COMMON_DIR)/lib/libcarcommon.a
TESTS_DIR     = tests
BUILD_DIR     = build
BIN_DIR       = $(BUILD_DIR)/bin
LOG_DIR       = $(BUILD_DIR)/log

# ---- 模块列表 (编译顺序: 先 common, 再各 daemon) ----
MODULES  = guard_daemon
MODULES += gps_daemon
MODULES += input_daemon
MODULES += canbus_daemon
MODULES += av_daemon
MODULES += dvr_daemon

# ---- 传递给子 Makefile 的变量 ----
MAKE_VARS  = CROSS_COMPILE=$(CROSS_COMPILE)
MAKE_VARS += COMMON_LIB=$(CURDIR)/$(COMMON_LIB)
MAKE_VARS += INC_DIR=$(CURDIR)/$(INC_DIR)
MAKE_VARS += OUTPUT_DIR=$(CURDIR)/$(BIN_DIR)

# ================================================================
#  默认目标: 全部模块
# ================================================================
.PHONY: all
all: dirs common $(MODULES)
	@echo ""
	@echo "================================================"
	@echo "  全部模块编译完成!"
	@echo "  可执行文件: $(BIN_DIR)/"
	@echo "================================================"
	@ls -la $(BIN_DIR)/ 2>/dev/null || true

.PHONY: dirs
dirs:
	@$(MKDIR) $(BIN_DIR)
	@$(MKDIR) $(LOG_DIR)

# ---- common 公共库 ----
.PHONY: common
common:
	@echo ">>> [common] 编译公共库..."
	@$(MAKE) -C $(COMMON_DIR) $(MAKE_VARS)
	@echo ""

# ---- 各守护进程 (独立编译, 依赖 common) ----
.PHONY: guard_daemon
guard_daemon: common
	@echo ">>> [guard_daemon]"
	@$(MAKE) -C $(SRC_DIR)/guard_daemon $(MAKE_VARS)


.PHONY: gps_daemon
gps_daemon: common
	@echo ">>> [gps_daemon]"
	@$(MAKE) -C $(SRC_DIR)/gps_daemon $(MAKE_VARS)

.PHONY: input_daemon
input_daemon: common
	@echo ">>> [input_daemon]"
	@$(MAKE) -C $(SRC_DIR)/input_daemon $(MAKE_VARS)

.PHONY: canbus_daemon
canbus_daemon: common
	@echo ">>> [canbus_daemon]"
	@$(MAKE) -C $(SRC_DIR)/canbus_daemon $(MAKE_VARS)

.PHONY: av_daemon
av_daemon: common
	@echo ">>> [av_daemon]"
	@$(MAKE) -C $(SRC_DIR)/av_daemon $(MAKE_VARS)

.PHONY: dvr_daemon
dvr_daemon: common
	@echo ">>> [dvr_daemon]"
	@$(MAKE) -C $(SRC_DIR)/dvr_daemon $(MAKE_VARS)


# ---- 便捷别名 ----
.PHONY: guard gps input can av dvr
guard:  guard_daemon
gps:    gps_daemon
input:  input_daemon
can:    canbus_daemon
av:     av_daemon
dvr:    dvr_daemon

# ================================================================
#  测试
# ================================================================
# 单元测试已集中到 tests/ 目录, 由 tests/Makefile 统一编译与运行。
# 注意: 测试程序需要在本机直接执行, 因此固定使用本地编译器 (HOST=1);
#       若跟随全局 HOST=0 走 ARM 交叉编译器, 产出的二进制无法在 x86 上运行。
.PHONY: test
test: common
	@echo ">>> 运行全部单元测试..."
	@$(MAKE) -C $(TESTS_DIR) test \
		HOST=1 \
		COMMON_LIB=$(CURDIR)/$(COMMON_LIB)

.PHONY: test-all
test-all: test

# ================================================================
#  清理
# ================================================================
.PHONY: clean
clean:
	@echo ">>> 清理所有编译产物..."
	@$(MAKE) -C $(COMMON_DIR) clean 2>/dev/null || true
	@for mod in $(MODULES); do \
		$(MAKE) -C $(SRC_DIR)/$$mod clean 2>/dev/null || true; \
	done
	@$(MAKE) -C $(TESTS_DIR) clean 2>/dev/null || true
	@$(RM) -r $(BUILD_DIR)
	@echo "  清理完成."

# ================================================================
#  部署打包
# ================================================================
.PHONY: deploy
deploy: all
	@echo ">>> 打包部署文件..."
	@$(MKDIR) $(BUILD_DIR)/deploy/car_terminal/bin
	@$(MKDIR) $(BUILD_DIR)/deploy/car_terminal/config
	@$(MKDIR) $(BUILD_DIR)/deploy/car_terminal/scripts
	@$(MKDIR) $(BUILD_DIR)/deploy/car_terminal/log
	@$(CP) $(BIN_DIR)/* $(BUILD_DIR)/deploy/car_terminal/bin/ 2>/dev/null || true
	@$(CP) config/config.ini $(BUILD_DIR)/deploy/car_terminal/config/
	@echo "  部署包: $(BUILD_DIR)/deploy/car_terminal/"
	@echo "  开发板部署命令:"
	@echo "    adb push $(BUILD_DIR)/deploy/car_terminal /opt/"

# ================================================================
#  帮助
# ================================================================
.PHONY: help
help:
	@echo "智能车载终端 — 编译系统"
	@echo ""
	@echo "环境设置 (ARM 交叉编译):"
	@echo "  export ARCH=arm"
	@echo "  export CROSS_COMPILE=arm-buildroot-linux-gnueabihf-"
	@echo "  export PATH=\$$PATH:/home/book/100ask_imx6ull-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/bin"
	@echo ""
	@echo "目标:"
	@echo "  make              — 编译全部模块 (ARM)"
	@echo "  make common       — 只编译公共库"
	@echo "  make gps          — 只编译 gps_daemon"
	@echo "  make input        — 只编译 input_daemon"
	@echo "  make can          — 只编译 canbus_daemon"
	@echo "  make av           — 只编译 av_daemon"
	@echo "  make dvr          — 只编译 dvr_daemon"
	@echo "  make guard        — 只编译 guard_daemon"
	@echo "  make test         — 运行全部单元测试 (tests/)"
	@echo "  make clean        — 清理全部编译产物"
	@echo "  make deploy       — 打包部署文件"
	@echo "  make help         — 帮助"
