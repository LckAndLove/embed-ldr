# 宿主文件流参考入口

此可选模块对接调用方提供的二进制 `FILE` 流，完整支持同步与异步双向 `IMPORT`（文件导入至设备目标）与 `EXPORT`（设备目标导出至文件），并可通过 `ecat_transfer_file_make_plugin_desc()` 注册到统一通道插件中心。它用于桌面工具、回放测试与第二种通道架构验证，不是硬件总线驱动。

## 构建

```sh
# 文件流默认关闭。此处只构建通用引擎和文件入口，不含 FoE 适配器。
make build-upgrade ENABLE_FOE=0 ENABLE_FILE=1 UPGRADE_BUILD_DIR=build/file-upgrade
# 纯中性裁剪模式（零 FoE 核心、零 EFW 插件）+ 宿主文件流：
make build-upgrade ENABLE_FOE=0 ENABLE_FOE_CORE=0 ENABLE_EFW_PLUGIN=0 ENABLE_FILE=1 UPGRADE_BUILD_DIR=build/neutral-file
# 两入口同时启用，FoE 默认选择 SSC 适配器。
make build-upgrade ENABLE_FILE=1 UPGRADE_BUILD_DIR=build/shared-upgrade
```

需要重新构建时使用新目录，或先用 `make clean BUILD_DIR=build/file-upgrade` 清理已登记的构建产物。

## 接入次序

1. 初始化目标注册表与 `ecat_transfer_t`（或顶层 `ldr_loader_t`），固件目标还需配置固件流水线或固件核心；异步目标须配置对应异步后端和暂存区。
2. 用 `ecat_transfer_file_init` 建立文件入口并提供静态分块缓冲。导入时以二进制读打开流，导出时以二进制写打开流。
3. 构造 `ecat_transfer_request_t`，指定通道 ID、目标 ID 和方向（`ECAT_TRANSFER_DIR_IMPORT` 或 `ECAT_TRANSFER_DIR_EXPORT`），固件导入还需填写候选版本。返回 `BUSY` 或 `DENIED` 时不会触碰文件流。
4. 每次调度调用 `ecat_transfer_file_step`（或注册为通道插件后由 `ldr_loader_poll` 统一推进）。`PENDING` 表示继续调度；`COMPLETE` 表示提交并 `fflush` 落盘成功；`FAILED` 时检查管理器诊断，`io_error` 表示文件读写或刷盘失败。
5. 超时或用户取消时调用 `ecat_transfer_file_cancel`。文件入口不会关闭或回绕 `FILE`，调用方负责 `fclose`。入口、流和缓冲必须活到操作终止，活跃入口不可移动或复制。

FoE、文件流与自定义通道可绑定同一个传输管理器，共用目标注册表、访问控制和会话仲裁。

`fread`/`fwrite`/`fflush` 可能阻塞，此入口仅作宿主参考；不能直接放进 ISR 或声称具有 MCU 实时时序保证。掉电安全与启动检查仍由目标驱动和 Bootloader 实现。

## 验证

`make test-end-to-end`（D3.5 / D3.5.4）与 `make test-transport-plugin` 覆盖文件流同步/异步双向 `IMPORT` 与 `EXPORT`、CRC 损坏/截断文件、FoE/文件流互斥抢占与异常取消；`make test-upgrade-build` 验证默认关闭文件入口、显式启用且关闭 FoE 时的实际静态库与链接结果。
