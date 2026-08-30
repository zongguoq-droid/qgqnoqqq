# ================================================================
#  智能车载终端 — 顶层 Makefile
#
#  使用方式:
#   make                    — 编译全部模块 (ARM 交叉编译)
#   make common             — 只编译 common 公共库
#   make gps / sensor / ... — 只编译指定模块
#   make test               — 编译并运行公共库测试
#   make clean              — 清理全部编译产物
#   make deploy             — 打包部署文件到 build/deploy/
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
COMMON_DIR    = $(SRC_DIR)/common
COMMON_LIB    = $(COMMON_DIR)/lib/libcarcommon.a
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
MAKE_VARS += COMMON_INC=$(CURDIR)/$(SRC_DIR)
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
	@$(MAKE) -C $(SRC_DIR)/guard_daemon $(MAKE_VARS) 2>&1 | grep -E "CC|LD|built|Error|error" || true


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
.PHONY: test
test: common
	@echo ">>> 运行公共库测试..."
	@$(MAKE) -C $(COMMON_DIR) test

.PHONY: test-all
test-all: test
	@for mod in $(MODULES); do \
		if [ -f $(SRC_DIR)/$$mod/Makefile ]; then \
			echo ">>> Testing $$mod ..."; \
			$(MAKE) -C $(SRC_DIR)/$$mod $(MAKE_VARS) test 2>/dev/null || true; \
		fi \
	done

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
	@echo "  make test         — 运行公共库测试"
	@echo "  make test-all     — 运行全部模块测试"
	@echo "  make clean        — 清理全部编译产物"
	@echo "  make deploy       — 打包部署文件"
	@echo "  make help         — 帮助"
