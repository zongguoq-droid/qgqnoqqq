# dvr_daemon/ Makefile
HOST          ?= 0
ifeq ($(HOST),1)
  CROSS_COMPILE ?=
  CC     = gcc
else
  CROSS_COMPILE ?= arm-buildroot-linux-gnueabihf-
  CC     = $(CROSS_COMPILE)gcc
endif

RM = rm -f
MKDIR = mkdir -p
COMMON_LIB ?= ../common/lib/libcarcommon.a
INC_DIR    ?= ../../include
OUTPUT_DIR ?= ../../build/bin
BUILD_DIR   = build

SRCS  = dvr_daemon.c v4l2_capture.c storage_mgr.c
OBJS  = $(patsubst %.c, $(BUILD_DIR)/%.o, $(SRCS))
TARGET = $(BUILD_DIR)/dvr_daemon

CFLAGS  = -Wall -Wextra -std=gnu99 -Os -g -ffunction-sections -fdata-sections -Wno-format-truncation
CFLAGS += -I$(INC_DIR)
LDFLAGS = -Wl,--gc-sections -lpthread -lrt -lm

.PHONY: all host clean help

all: $(TARGET)
	@echo "  dvr_daemon built!"; $(MKDIR) $(OUTPUT_DIR) 2>/dev/null || true
	@cp $(TARGET) $(OUTPUT_DIR)/dvr_daemon 2>/dev/null || true

$(TARGET): $(OBJS)
	@$(MKDIR) $(BUILD_DIR); $(CC) -o $@ $^ $(COMMON_LIB) $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c
	@$(MKDIR) $(dir $@); $(CC) $(CFLAGS) -c $< -o $@

host:
	@$(MAKE) HOST=1 all

clean:
	@$(RM) -r $(BUILD_DIR); @echo "  Cleaned."

help:
	@echo "dvr_daemon: make / make HOST=1 / make clean"
	@echo "  (unit tests: make -C ../../tests)"
