# av_daemon/ Makefile
#   make          — ARM cross-compile
#   make HOST=1   — x86 local build (requires alsa-lib)
#   make clean

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

SRCS  = av_daemon.c
SRCS += audio_core.c

OBJS  = $(patsubst %.c, $(BUILD_DIR)/%.o, $(SRCS))
TARGET       = $(BUILD_DIR)/av_daemon

CFLAGS  = -Wall -Wextra -std=gnu99 -Os -g
CFLAGS += -ffunction-sections -fdata-sections -Wno-format-truncation
CFLAGS += -I$(INC_DIR)
LDFLAGS  = -Wl,--gc-sections -lpthread -lrt -lm -lasound

.PHONY: all
all: $(TARGET)
	@echo "  av_daemon built!"
	@$(MKDIR) $(OUTPUT_DIR) 2>/dev/null || true
	@cp $(TARGET) $(OUTPUT_DIR)/av_daemon 2>/dev/null || true

$(TARGET): $(OBJS)
	@$(MKDIR) $(BUILD_DIR)
	@$(CC) -o $@ $^ $(COMMON_LIB) $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c
	@$(MKDIR) $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

.PHONY: host
host:
	@$(MAKE) HOST=1 all

.PHONY: clean
clean:
	@$(RM) -r $(BUILD_DIR)
	@echo "  Cleaned."

.PHONY: help
help:
	@echo "av_daemon Build: make / make HOST=1 / make clean"
