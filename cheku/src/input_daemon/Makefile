# ================================================================
#  input_daemon/ Makefile — 按键输入管理进程编译
#
# 支持:
#   make                    — ARM 交叉编译
#   make HOST=1             — x86 本地编译 (测试用, 无真实 input 设备)
#   make clean
#
# 单元测试已集中到仓库根目录 tests/, 执行: make -C ../../tests
#
# 交叉编译:
#   make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
# ================================================================

HOST          ?= 0
ifeq ($(HOST),1)
  CROSS_COMPILE ?=
  CC     = gcc
else
  CROSS_COMPILE ?= arm-buildroot-linux-gnueabihf-
  CC     = $(CROSS_COMPILE)gcc
endif

RM            = rm -f
MKDIR         = mkdir -p

COMMON_LIB    ?= ../common/lib/libcarcommon.a
INC_DIR       ?= ../../include
OUTPUT_DIR    ?= ../../build/bin

BUILD_DIR     = build

SRCS  = input_daemon.c
SRCS += key_parser.c
SRCS += led_control.c

OBJS  = $(patsubst %.c, $(BUILD_DIR)/%.o, $(SRCS))
TARGET       = $(BUILD_DIR)/input_daemon

CFLAGS  = -Wall -Wextra -std=gnu99 -Os -g
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -I$(INC_DIR)

LDFLAGS = -Wl,--gc-sections -lpthread

# ---- 默认 ----
.PHONY: all
all: $(TARGET)
	@echo "  input_daemon built!"
	@$(MKDIR) $(OUTPUT_DIR) 2>/dev/null || true
	@cp $(TARGET) $(OUTPUT_DIR)/input_daemon 2>/dev/null || true
	@echo "  → $(OUTPUT_DIR)/input_daemon"

$(TARGET): $(OBJS)
	@$(MKDIR) $(BUILD_DIR)
	@echo "  LD    $@"
	@$(CC) -o $@ $^ $(COMMON_LIB) $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c
	@$(MKDIR) $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# ---- 本地编译 ----
.PHONY: host
host:
	@$(MAKE) HOST=1 all

# ---- 清理 ----
.PHONY: clean
clean:
	@echo "  Cleaning input_daemon..."
	@$(RM) -r $(BUILD_DIR)
	@echo "  Done."

.PHONY: help
help:
	@echo "input_daemon Build Targets:"
	@echo "  make           - ARM cross-compile"
	@echo "  make HOST=1    - x86 local build"
	@echo "  make clean     - Clean"
	@echo "  (unit tests: make -C ../../tests)"
