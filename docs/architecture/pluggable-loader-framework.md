# 统一可扩展双向 Loader 框架架构与插件接入指南

本文档描述统一中性 `ldr_` Loader 框架的分层架构、三类插件接入契约、构建裁剪矩阵与嵌入式编码规范。

## 1. 总体分层架构

框架采用 **1 个中性双向核心 + 3 类可插拔插件层**，全程零动态内存分配（zero `malloc`），严格遵循 C99 与 BARR-C:2018 / MISRA-C 友好风格：

| 层级 | 核心头文件 / 源文件 | 职责 |
| :--- | :--- | :--- |
| **顶层中性门面** | `include/ldr_loader.h`, `src/ldr_loader.c` | 统一整合三大插件注册表与双向传输核心，提供纯中性 `ldr_loader_init()`、`ldr_loader_poll()`、`ldr_loader_abort_all()` 及全套 `ldr_transport_endpoint_*` 封装 |
| **双向传输核心** | `include/ecat_transfer.h`, `src/ecat_transfer.c`, `include/ldr_crc32.h` | 双向 `IMPORT` / `EXPORT` 状态机、单租约互斥仲裁、可选固件后端虚表 `ldr_fw_backend_ops_t`（链接级零 FoE/EFW 依赖） |
| **插件层 1：目标存储插件** | `include/ecat_target_registry.h`, `src/ecat_target_registry.c` | 注册任意数量固件区/数据区（ID `1..65535`）与细粒度 `(transport_id, target_id)` ACL 权限矩阵 |
| **插件层 2：传输通道插件** | `include/ecat_transport_plugin.h`, `src/ecat_transport_plugin.c`, `include/ecat_transport_endpoint.h` | 统一通道能力声明（`IMPORT` / `EXPORT` / `ASYNC`）、注册与主循环轮询；内置可选 FoE（WRQ + RRQ）、File 流，支持自定义 UART/CAN/USB/BLE 通道 |
| **插件层 3：格式与固件刷写流水线** | `include/ldr_format_plugin.h`, `include/ldr_fw_pipeline.h`, `src/ldr_fw_pipeline.c`, `src/plugins/ldr_format_efw.c`, `src/plugins/ldr_format_raw.c` | 通过 `ldr_format_registry_t` 自动探测并校验 EFW v2、原始 `.bin` (`RAW`) 或自定义镜像格式，由 `ldr_fw_pipeline_t` 驱动防变砖刷写（同步与基于 `ldr_loader_poll()` 的非阻塞异步状态机均支持流式验签、防回滚版本下限原子事务提交与流/回读双 CRC 核验） |

## 2. 双向能力矩阵

| 通道插件 | IMPORT (同步) | IMPORT (异步) | EXPORT (同步) | EXPORT (异步) | 说明 |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **自定义通道 (`ldr_transport_endpoint_t`)** | 支持 | 支持 | 支持 | 支持 | 参考 [ldr_uart_transport_template.h](../../examples/custom_transport_template/ldr_uart_transport_template.h) 快速接入 UART/CAN/USB/BLE |
| **宿主文件流 (`ENABLE_FILE=1`)** | 支持 | 支持 | 支持 | 支持 | `ecat_transfer_file_t`，用于桌面工具与本地回放 |
| **FoE SSC (`ENABLE_FOE=1 FOE_STACK=ssc`)** | 支持 | 支持 | 支持 | 暂不支持 | 支持目标名与 `<PROJECT>_<TARGET>` 规范名、整包 EOF、连续 `offset` 与 ACL 拦截 |
| **FoE SOES (`ENABLE_FOE=1 FOE_STACK=soes`)** | 支持 | 暂不支持 | 暂不支持 | 暂不支持 | 仅用于兼容现有 SOES WRQ 升级链路 |

> 架构建议：新项目统一使用 `ldr_fw_pipeline_t` + `driver.async_ops`（同步与异步均支持流式验签与防回滚原子提交）；`ecat_foe_core` / `driver.fw_async_ops` 作为旧版 FoE 兼容后端保留。

## 3. 构建裁剪矩阵

通过 `Makefile` 参数按需裁剪，未启用插件不参与编译链接（0 字节 Flash/RAM 占用）：

| 配置场景 | 裁剪参数 | 说明 |
| :--- | :--- | :--- |
| 纯中性无 FoE/EFW/文件流 | `ENABLE_FOE=0 ENABLE_FOE_CORE=0 ENABLE_FILE=0 ENABLE_EFW_PLUGIN=0` | 仅含 9 个纯中性核心源文件（`nm -u` 零 FoE 符号依赖），对接自定义总线与格式 |
| 中性核心 + EFW 格式插件 | `ENABLE_FOE=0 ENABLE_FOE_CORE=0 ENABLE_FILE=0 ENABLE_EFW_PLUGIN=1` | 无 EtherCAT 协议栈，复用 EFW v2 容器头校验 |
| 中性核心 + 宿主文件流 | `ENABLE_FOE=0 ENABLE_FOE_CORE=0 ENABLE_FILE=1 ENABLE_EFW_PLUGIN=0` | 用于桌面工具、回放测试或本地存储双向导入导出 |
| 完整栈（FoE SSC + 文件流） | `ENABLE_FOE=1 ENABLE_FOE_CORE=1 FOE_STACK=ssc ENABLE_FILE=1` | 同时启用 EtherCAT FoE 双向升级/导出与文件流插件 |

执行 `make test-ldr-trim-matrix` 可一键验证上述 4 种裁剪矩阵的独立编译与自定义 UART 双向模板运行。

## 4. 嵌入式编码规范

新增与重构模块统一遵循 BARR-C:2018 / MISRA-C 友好规范：

- 严格 `-Wall -Wextra -Werror -std=c99 -pedantic` 零警告；
- 禁止 `malloc`/`free`，固定静态槽位容量且无结构体自引用指针；
- 使用 `<stdint.h>` 定宽整型、`<stdbool.h>` 布尔类型、显式无符号后缀（`U`/`UL`）与大括号完整包裹的控制语句；
- 统一 `snake_case` 命名、`_t` 类型后缀、`UPPER_SNAKE_CASE` 宏常量与 `s_` 文件静态变量前缀。
