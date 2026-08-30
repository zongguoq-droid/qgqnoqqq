# 更新日志 (Changelog)

本项目所有值得记录的变更都写在这里。
格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

## [0.1.0] — 2026-08-30

首个规范化版本。以"目录结构工程化"为主线，同时修复了重构过程中暴露的多个真实缺陷。

### Added — 新增

**目录结构**
- `include/` — 公共头文件目录，8 个头文件从 `src/common/` 迁出，实现与接口分离
- `tests/` — 全项目单元测试统一入口，`tests/Makefile` 统一编译并运行 6 个测试
- `docs/modules/` — 7 份模块学习文档统一命名后归拢（如 `guard_daemon模块学习文档.md` → `docs/modules/guard_daemon.md`）
- `README.md` — 项目根说明文档（架构、目录、编译运行、已知问题）
- `LICENSE` — MIT 开源协议
- `CHANGELOG.md` — 本文件
- `.gitattributes` — 强制 LF 换行，防止 `.sh` 在 Windows 被转为 CRLF 后开发板执行失败
- `.clang-format` — C/C++ 格式化配置（已关闭注释重排，保护既有 ASCII 注释排版）
- `.editorconfig` — 跨编辑器缩进/编码/换行统一
- `.github/workflows/ci.yml` — GitHub Actions：x86 编译 + 单元测试 + cppcheck 静态检查

**功能**
- `guard_config_load()` — 实现此前仅有声明的预留接口，支持从配置文件 `[processes]` 节动态控制各子进程启停
- `child_process_t.enabled` — 新增字段，区分"已定义"与"已启用"，使预留槽位可保留定义但不启动
- `scripts/start_all.sh --guard` — guard 托管模式（`--manual` 回退到旧的手动启动模式）

### Changed — 变更

- 源码 `#include` 统一为基于 `-Iinclude` 的规范形式（如 `#include "log/log.h"`），消除 `../` 相对路径
- 各模块 Makefile 变量 `COMMON_INC` 更名为 `INC_DIR`，指向 `include/`
- 根 Makefile `test` 目标改为调用 `tests/Makefile`，统一管理全部单元测试
- `start_all.sh` 默认改为 guard 托管模式：`guard_daemon` 负责拉起、监控、自动重启全部子进程
- 文档中所有 `src/common/*.h` 路径引用同步更新为 `include/*.h`

### Fixed — 修复

**严重：源码从未完整入库**
- `.gitignore` 中 `gps_daemon`、`dvr_daemon` 等**裸文件名规则**会匹配任意层级同名目录，
  导致 `src/gps_daemon/`、`src/dvr_daemon/` 等**整个源码目录被忽略**，
  6 个 daemon 约 12000 行源码从未进入版本控制。已移除这些规则并补交全部源码。

**严重：单元测试从未真正编译通过**
- 原各模块 Makefile 使用 `-DTEST_MODE` 规避测试与 daemon 源码的 `main()` 冲突，
  但源码中 `TEST_MODE` **零命中**，该宏完全无效——`test_can`/`test_dvr`/`test_guard`
  一旦链接 daemon 源码必然 `main` 重复定义。
  改为链接时以 `-Dmain=<file>_main_disabled` 重命名，零侵入解决冲突。
- `make test` 原先默认走 ARM 交叉编译器，产出的二进制无法在 x86 上运行，
  且 `2>/dev/null || true` 吞掉全部失败（看起来"通过"实则全挂）。现固定使用本地编译器。
- `guard_config_load()` 只有声明没有实现，而 `test_guard.c` 调用了它，
  导致链接必然失败。已实现该接口。

**功能缺陷**
- `guard_daemon` 的 `GUARD_BIN_PREFIX` 为 `/bin/`，与部署脚本的 `/usr/bin/` 不一致，
  会导致 `execlp` 找不到可执行文件。已改为 `/usr/bin/`。
- `guard_daemon` 启动子进程时未透传配置文件参数，与脚本手动启动行为不一致。
  现在 guard 会将自身收到的配置文件路径透传给所有子进程。
- 根 Makefile 的 `guard_daemon` 目标用 `| grep -E "..." || true` 包裹，
  掩盖真实编译错误且编译失败也返回成功。已移除。

**脚本**
- `start_all.sh` 步骤编号错误（标注 `[1/5]` 实际执行 7 步）。已修正为 `[1/7]`。
- `stop_all.sh` 未停止 `guard_daemon`。已补充，并改为优先通过 guard 优雅停止全部子进程。
- `scripts/` 中项目目录与工具链路径硬编码为特定用户（`~/Desktop/cheku`、`/home/book/...`）。
  改为自动推导或通过环境变量覆盖。

**文档**
- `docs/1.md` 引用了不存在的根目录 `cloud.md`（实际为 `docs/CLOUDE.md`）。已修正。
- `docs/1.md` 含有开发者本机绝对路径（`C:\Users\...\Linux-4.9.88\...`）。已移除。
- `docs/architecture.md` 与 `src/guard_daemon/README.md` 中关于子进程数量的描述
  （文档说 7 个 / 代码 5 个）不一致。已统一为"7 个定义，其中 sensor_daemon 与
  net_daemon 为 enabled=0 的预留槽位，实际启动 5 个"。

### Known Issues — 已知问题

- `av_daemon` 暂无单元测试（依赖 ALSA 声卡，需在开发板上验证）。
- `start_all.sh` 的 guard 托管模式尚未在开发板实测，若异常可用 `--manual` 回退。
- `docs/architecture.md` 中的 Mermaid 架构图仍为原始设计（8 进程），
  正文已标注差异，图本身待更新。
- `scripts/` 部分路径仍假设开发板目录布局（`/usr/bin`、`/etc/car_terminal`）。
