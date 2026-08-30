# ================================================================
#  common/ Makefile — 公共库编译
#
# 支持两种编译模式:
#   make              — 编译静态库 libcarcommon.a (默认推荐)
#   make shared       — 编译动态库 libcarcommon.so
#   make clean        — 清理编译产物
#
# 单元测试已集中到仓库根目录 tests/, 由 tests/Makefile 统一管理
#
# 交叉编译:
#   make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
#
# 学习要点:
#   1. ar — 静态库打包工具 (archive)
#   2. gcc -shared — 生成动态库
#   3. gcc -fPIC — 位置无关代码 (动态库必需)
#   4. Makefile 变量和自动推导规则
# ================================================================

# ---- 工具链配置 ----
CROSS_COMPILE ?=
CC      = $(CROSS_COMPILE)gcc
AR      = $(CROSS_COMPILE)ar
RM      = rm -f
MKDIR   = mkdir -p

# ---- 编译选项 ----
# -Wall -Wextra:  启用所有常见警告
# -std=c99:        使用 C99 标准
# -Os:             体积优化 (适合嵌入式)
# -g:              包含调试符号
# -fPIC:           位置无关代码 (静态库和动态库都需要, 方便以后切换)
# -ffunction-sections -fdata-sections: 将每个函数/数据放入独立段
#
# 公共头文件已统一迁移到 include/ 目录, 本模块源码通过 -I 引用:
#   #include "log/log.h"  /  #include "common.h"  /  #include "crc/crc8.h"
INC_DIR ?= ../../include
CFLAGS  = -Wall -Wextra -std=gnu99 -Os -g -fPIC
CFLAGS  += -ffunction-sections -fdata-sections
CFLAGS  += -I$(INC_DIR)

# 链接选项:
# -Wl,--gc-sections: 链接时移除未使用的段 (配合 -ffunction-sections)
# -lpthread:         链接 pthread 库 (log 模块使用互斥锁)
# -lrt:              链接实时库 (timer 模块使用 timerfd)
LDFLAGS_TEST = -Wl,--gc-sections -lpthread -lrt

# ---- 目录结构 ----
SRC_DIR  = .
BUILD_DIR = build
LIB_DIR  = lib

# ---- 源文件列表 (每个模块的 .c 文件) ----
SRCS  = log/log.c
SRCS += crc/crc8.c
SRCS += ringbuffer/ringbuffer.c
SRCS += config/config.c
SRCS += protocol/protocol.c
SRCS += socket/unix_socket.c
SRCS += timer/timer.c

# ---- 生成目标文件列表 ----
# patsubst: 将 .c 替换为 .o
# 例如: log/log.c → build/log/log.o
OBJS = $(patsubst %.c, $(BUILD_DIR)/%.o, $(SRCS))

# ---- 库名称 ----
# 注意: 单元测试已迁至仓库根目录 tests/, 由 tests/Makefile 统一管理
STATIC_LIB  = $(LIB_DIR)/libcarcommon.a
SHARED_LIB  = $(LIB_DIR)/libcarcommon.so

# ================================================================
#  默认目标: 编译静态库
# ================================================================
.PHONY: all
all: $(STATIC_LIB)
	@echo "================================================"
	@echo "  libcarcommon.a built successfully!"
	@echo "  Static library: $(STATIC_LIB)"
	@echo "  Objects: $(OBJS)"
	@echo "  Use 'make shared' for shared library."
	@echo "  Use 'make -C ../../tests test' to run unit tests."
	@echo "================================================"

# ---- 编译静态库 ----
$(STATIC_LIB): $(OBJS)
	@$(MKDIR) $(LIB_DIR)
	@echo "  AR    $@"
	@$(AR) rcs $@ $^
	# ar 参数说明:
	#   r = 替换或添加文件到归档中
	#   c = 创建归档文件 (如果不存在)
	#   s = 写入索引 (加速链接时的符号查找)
	# $@ = 目标文件 (libcarcommon.a)
	# $^ = 所有依赖文件 (所有 .o)

# ---- 编译动态库 ----
.PHONY: shared
shared: $(SHARED_LIB)
	@echo "  Shared library: $(SHARED_LIB)"

$(SHARED_LIB): $(OBJS)
	@$(MKDIR) $(LIB_DIR)
	@echo "  LD    $@"
	@$(CC) -shared -o $@ $^ -lpthread -lrt
	# -shared: 生成动态库 (而非可执行文件)
	# 动态库在运行时由 ld.so 加载, 多个进程共享同一份代码段

# ---- 编译 .c → .o 的通用规则 ----
# $< = 第一个依赖 (源文件 .c)
# $@ = 目标文件 (.o)
$(BUILD_DIR)/%.o: %.c
	@$(MKDIR) $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# ================================================================
#  清理
# ================================================================
.PHONY: clean
clean:
	@echo "  Cleaning common library..."
	@$(RM) -r $(BUILD_DIR) $(LIB_DIR)
	@echo "  Done."

# ================================================================
#  帮助
# ================================================================
.PHONY: help
help:
	@echo "Common Library Build Targets:"
	@echo "  make              - Build static library (libcarcommon.a)"
	@echo "  make shared       - Build shared library (libcarcommon.so)"
	@echo "  make clean        - Remove build artifacts"
	@echo "  make help         - Show this help"
	@echo ""
	@echo "Cross-compilation:"
	@echo "  make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-"
	@echo ""
	@echo "Library usage in other modules:"
	@echo "  gcc -o myapp myapp.c -L/path/to/common/lib -lcarcommon -lpthread -lrt"
