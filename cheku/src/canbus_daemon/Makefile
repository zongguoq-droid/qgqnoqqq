# canbus_daemon/ Makefile
#   make              — ARM 交叉编译
#   make HOST=1       — x86 本地编译 (需 Linux + can0 存在)
#   make clean
#
# 单元测试已集中到仓库根目录 tests/, 执行: make -C ../../tests

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

SRCS  = canbus_daemon.c
SRCS += can_driver.c

OBJS  = $(patsubst %.c, $(BUILD_DIR)/%.o, $(SRCS))
TARGET       = $(BUILD_DIR)/canbus_daemon

CFLAGS  = -Wall -Wextra -std=gnu99 -Os -g
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -I$(INC_DIR)

LDFLAGS = -Wl,--gc-sections -lpthread -lrt -lm

.PHONY: all
all: $(TARGET)
	@echo "  canbus_daemon built!"
	@$(MKDIR) $(OUTPUT_DIR) 2>/dev/null || true
	@cp $(TARGET) $(OUTPUT_DIR)/canbus_daemon 2>/dev/null || true

$(TARGET): $(OBJS)
	@$(MKDIR) $(BUILD_DIR)
	@echo "  LD    $@"
	@$(CC) -o $@ $^ $(COMMON_LIB) $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c
	@$(MKDIR) $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

.PHONY: host
host:
	@$(MAKE) HOST=1 all

.PHONY: clean
clean:
	@echo "  Cleaning canbus_daemon..."
	@$(RM) -r $(BUILD_DIR)
	@echo "  Done."

.PHONY: help
help:
	@echo "canbus_daemon Build:"
	@echo "  make          - ARM cross-compile"
	@echo "  make HOST=1   - x86 local"
	@echo "  make clean    - Clean"
	@echo "  (unit tests: make -C ../../tests)"
