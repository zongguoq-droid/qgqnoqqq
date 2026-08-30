# ================================================================
#  tests/ Makefile — 全项目单元测试统一入口
#
#  用法:
#    make              — 编译并运行全部单元测试 (默认 x86 本地)
#    make HOST=0       — 使用 ARM 交叉编译器 (仅编译, 无法在本机运行)
#    make <name>       — 只运行指定测试, 如: make test_gps
#    make build        — 只编译不运行
#    make clean        — 清理
#    make help         — 帮助
#
#  由根 Makefile 调用:
#    make test         →  $(MAKE) -C tests test HOST=1 COMMON_LIB=...
#
#  设计说明:
#    1. 所有测试源码集中在 tests/, 统一在此编译, 不再散落于各模块。
#    2. 公共头文件位于 include/, 通过 -I$(INC_DIR) 引用;
#       各模块私有头文件通过 -I$(SRC_ROOT)/<模块> 引用。
#    3. 测试程序需要链接 daemon 的 .c 源文件 (如 guard_daemon.c) 以调用
#       其中的函数, 但这些文件同时定义了 main(), 会与测试自身的 main()
#       冲突。解决办法: 编译这些源文件时用 -Dmain=<文件名>_main_disabled
#       将其 main 重命名 (等价于 #define main xxx_main_disabled)。
#
#       该技巧安全的依据:
#         - 注释中的 "main()" 在预处理阶段前已被移除, 不受宏替换影响
#         - 项目中不存在含 "main" 的字符串常量
#         - main 仅作为函数名出现, 替换后不影响其他符号
#       相比在各 daemon 源码中添加 #ifndef TEST_MODE, 此方案零侵入。
# ================================================================

# ---- 工具链 ----
# 测试程序需要在编译它的机器上直接运行, 因此默认使用本地编译器。
HOST ?= 1

ifeq ($(HOST),1)
  CROSS_COMPILE ?=
  CC     = gcc
else
  CROSS_COMPILE ?= arm-buildroot-linux-gnueabihf-
  CC     = $(CROSS_COMPILE)gcc
endif

RM      = rm -f
MKDIR   = mkdir -p

# ---- 目录 (本 Makefile 位于 tests/, 项目根在其上一级) ----
ROOT_DIR      = ..
SRC_ROOT      = $(ROOT_DIR)/src
INC_DIR       ?= $(ROOT_DIR)/include
COMMON_DIR    = $(ROOT_DIR)/src/common
COMMON_LIB    ?= $(COMMON_DIR)/lib/libcarcommon.a
BUILD_DIR     = build

# ---- 编译选项 ----
CFLAGS  = -Wall -Wextra -std=gnu99 -O0 -g
CFLAGS += -I$(INC_DIR)
# 各模块私有头文件 (供 test_*.c 直接 #include "gps_daemon.h" 这类写法)
CFLAGS += -I$(SRC_ROOT)/guard_daemon
CFLAGS += -I$(SRC_ROOT)/gps_daemon
CFLAGS += -I$(SRC_ROOT)/input_daemon
CFLAGS += -I$(SRC_ROOT)/canbus_daemon
CFLAGS += -I$(SRC_ROOT)/dvr_daemon
CFLAGS += -I$(SRC_ROOT)/av_daemon

LDFLAGS = -lpthread -lrt -lm

# ================================================================
#  测试清单
# ================================================================
TEST_NAMES  = test_common
TEST_NAMES += test_gps
TEST_NAMES += test_input
TEST_NAMES += test_can
TEST_NAMES += test_guard
TEST_NAMES += test_dvr
TEST_NAMES += test_av

TEST_BINS = $(addprefix $(BUILD_DIR)/,$(TEST_NAMES))

# 各测试除自身 test_*.c 之外, 还需链接的模块源文件 (写 src/ 下的相对路径)
SRC_test_common =
SRC_test_gps    = gps_daemon/nmea_parser.c
SRC_test_input  = input_daemon/key_parser.c
SRC_test_can    = canbus_daemon/can_driver.c canbus_daemon/canbus_daemon.c
SRC_test_guard  = guard_daemon/guard_daemon.c
SRC_test_dvr    = dvr_daemon/v4l2_capture.c dvr_daemon/storage_mgr.c dvr_daemon/dvr_daemon.c
# test_av.c 通过 #include "av_daemon.c" 访问其中的 static 函数 (scan_music),
# 因此 av_daemon.c 本身不在此列出 (重复编译会导致符号重复定义)。
#
# 但必须链接 audio_core.c:
#   av_daemon.c 的 open_track()/play_chunk()/main() 调用了
#   audio_playback_open() / audio_mixer_set_volume() 等 audio_core.c 中的函数。
#   C 链接器会解析整个目标文件的重定位项, 即使测试没有直接调用这些函数,
#   只要它们被编译进可执行文件, 其引用的符号就必须有定义,
#   否则报 undefined reference。(除非启用 -ffunction-sections + --gc-sections)
#   audio_core.c 自身依赖 ALSA, 链接时需要 -lasound (见下方链接规则)。
SRC_test_av     = av_daemon/audio_core.c

# 转换为 .o 路径
# $(1) = 测试名 (如 test_gps)
obj_for = $(addprefix $(BUILD_DIR)/,$(patsubst %.c,%.o,$(SRC_$(1))))

# ================================================================
#  默认目标: 编译并运行全部测试
# ================================================================
.PHONY: all
all: test

.PHONY: test
test: build
	@for t in $(TEST_NAMES); do \
		echo ""; \
		echo "================================================"; \
		echo "  Running $$t ..."; \
		echo "================================================"; \
		$(BUILD_DIR)/$$t || exit 1; \
	done
	@echo ""
	@echo "================================================"
	@echo "  All tests passed."
	@echo "================================================"

.PHONY: build
build: $(TEST_BINS)

# ================================================================
#  单测试快捷目标: make test_gps
# ================================================================
$(TEST_NAMES): %: $(BUILD_DIR)/%
	@echo ""
	@echo "================================================"
	@echo "  Running $* ..."
	@echo "================================================"
	@$(BUILD_DIR)/$*

# ================================================================
#  链接规则
# ================================================================
# 依赖说明: | $(COMMON_LIB) 为 order-only 依赖, 不会进入 $^,
#           避免静态库在链接命令中重复出现。
$(BUILD_DIR)/test_common: test_common.c $(call obj_for,test_common) | $(COMMON_LIB)
	@$(MKDIR) $(BUILD_DIR)
	@echo "  LD    $@"
	@$(CC) $(CFLAGS) -o $@ $^ $(COMMON_LIB) $(LDFLAGS)

$(BUILD_DIR)/test_gps: test_gps.c $(call obj_for,test_gps) | $(COMMON_LIB)
	@$(MKDIR) $(BUILD_DIR)
	@echo "  LD    $@"
	@$(CC) $(CFLAGS) -o $@ $^ $(COMMON_LIB) $(LDFLAGS)

$(BUILD_DIR)/test_input: test_input.c $(call obj_for,test_input) | $(COMMON_LIB)
	@$(MKDIR) $(BUILD_DIR)
	@echo "  LD    $@"
	@$(CC) $(CFLAGS) -o $@ $^ $(COMMON_LIB) $(LDFLAGS)

$(BUILD_DIR)/test_can: test_can.c $(call obj_for,test_can) | $(COMMON_LIB)
	@$(MKDIR) $(BUILD_DIR)
	@echo "  LD    $@"
	@$(CC) $(CFLAGS) -o $@ $^ $(COMMON_LIB) $(LDFLAGS)

$(BUILD_DIR)/test_guard: test_guard.c $(call obj_for,test_guard) | $(COMMON_LIB)
	@$(MKDIR) $(BUILD_DIR)
	@echo "  LD    $@"
	@$(CC) $(CFLAGS) -o $@ $^ $(COMMON_LIB) $(LDFLAGS)

$(BUILD_DIR)/test_dvr: test_dvr.c $(call obj_for,test_dvr) | $(COMMON_LIB)
	@$(MKDIR) $(BUILD_DIR)
	@echo "  LD    $@"
	@$(CC) $(CFLAGS) -o $@ $^ $(COMMON_LIB) $(LDFLAGS)

# test_av 特殊之处:
#   1. 它 #include 了 av_daemon.c, 需要 -I$(SRC_ROOT)/av_daemon 才能找到
#      同目录下的 audio_core.h (av_daemon.c 用 #include "audio_core.h" 引用)
#   2. audio_core.h 依赖 <alsa/asoundlib.h>, 需要 ALSA 开发库:
#        Ubuntu/Debian: sudo apt-get install libasound2-dev
#      链接需要 -lasound
$(BUILD_DIR)/test_av: test_av.c $(call obj_for,test_av) | $(COMMON_LIB)
	@$(MKDIR) $(BUILD_DIR)
	@echo "  LD    $@"
	@$(CC) $(CFLAGS) -I$(SRC_ROOT)/av_daemon -o $@ $^ \
		$(COMMON_LIB) $(LDFLAGS) -lasound

# ================================================================
#  编译 src/ 下的模块源文件
#
#  -Dmain=$(notdir $(basename $<))_main_disabled
#      将模块源文件中的 main() 重命名, 避免与测试框架的 main() 冲突。
#      对不含 main() 的源文件 (如 nmea_parser.c) 该宏无副作用。
# ================================================================
$(BUILD_DIR)/%.o: $(SRC_ROOT)/%.c
	@$(MKDIR) $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -Dmain=$(notdir $(basename $<))_main_disabled -c $< -o $@

# ================================================================
#  公共库 (若不存在则自动构建)
# ================================================================
$(COMMON_LIB):
	@echo "  Building common library..."
	@$(MAKE) -C $(COMMON_DIR) CROSS_COMPILE=$(CROSS_COMPILE)

# ================================================================
#  清理
# ================================================================
.PHONY: clean
clean:
	@echo "  Cleaning tests..."
	@$(RM) -r $(BUILD_DIR)
	@echo "  Done."

# ================================================================
#  帮助
# ================================================================
.PHONY: help
help:
	@echo "Unit Tests (tests/)"
	@echo ""
	@echo "  make              - Build and run all tests (x86 local, default)"
	@echo "  make build        - Build tests only"
	@echo "  make test_gps     - Run a single test"
	@echo "  make HOST=0       - Cross-compile for ARM (cannot run on host)"
	@echo "  make clean        - Clean build artifacts"
	@echo "  make help         - Show this help"
	@echo ""
	@echo "Available tests:"
	@for t in $(TEST_NAMES); do echo "  - $$t"; done
