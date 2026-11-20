/**
 * @file ldr_loader.h
 * @brief 统一可扩展双向 Loader 框架中性门面与命名层 (Unified Pluggable Loader Framework)
 *
 * 架构分层 (BARR-C:2018 / MISRA-C 友好, C99 零动态分配):
 * 1. 中性传输核心 (ldr_transfer_t): 双向 IMPORT / EXPORT、单租约互斥仲裁、同步与非阻塞异步状态机。
 * 2. 目标存储插件层 (ldr_target_registry_t): 任意数量存储区块 + 细粒度 ACL 权限矩阵。
 * 3. 传输通道插件层 (ldr_transport_registry_t): 内置可选通道 (FoE、文件流) 与自定义通道 (UART/CAN/USB/BLE 等) 统一注册与轮询。
 * 4. 格式与安全插件层 (ldr_format_registry_t): EFW、原始镜像或自定义加密/签名格式按需插拔。
 *
 * 本头文件不依赖任何具体传输协议 (FoE / SOES / SSC / FILE)，未启用插件 0 字节开销。
 */

#ifndef LDR_LOADER_H
#define LDR_LOADER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifndef LDR_NEUTRAL_BUILD
#define LDR_NEUTRAL_BUILD 1
#endif
#include "ecat_target_registry.h"
#include "ecat_transfer.h"
#include "ecat_transport_endpoint.h"
#include "ecat_transport_plugin.h"
#include "ldr_format_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                     中性类型别名 (统一 ldr_ 命名规范)                      */
/* -------------------------------------------------------------------------- */

/* 目标存储插件层 */
typedef ecat_foe_target_driver_t  ldr_storage_io_ops_t;
typedef ecat_target_type_t        ldr_target_type_t;
typedef ecat_async_op_result_t    ldr_async_op_result_t;
typedef ecat_async_data_op_kind_t ldr_async_data_op_kind_t;
typedef ecat_async_data_op_t      ldr_async_data_op_t;
typedef ecat_async_data_ops_t     ldr_async_data_ops_t;
typedef ecat_target_driver_t      ldr_target_driver_t;
typedef ecat_target_desc_t        ldr_target_desc_t;
typedef ecat_target_acl_rule_t    ldr_target_acl_rule_t;
typedef ecat_target_registry_t    ldr_target_registry_t;
typedef ecat_upgrade_session_t    ldr_session_t;
typedef ecat_upgrade_async_result_t ldr_fw_async_result_t;
typedef ecat_upgrade_async_ops_t  ldr_fw_async_ops_t;

#define LDR_PERM_READ            ECAT_PERM_READ
#define LDR_PERM_WRITE           ECAT_PERM_WRITE
#define LDR_PERM_RW              ECAT_PERM_RW
#define LDR_TARGET_TYPE_FIRMWARE ECAT_TARGET_TYPE_FIRMWARE
#define LDR_TARGET_TYPE_DATA     ECAT_TARGET_TYPE_DATA

#define LDR_ASYNC_OP_FAILED      ECAT_ASYNC_OP_FAILED
#define LDR_ASYNC_OP_COMPLETE    ECAT_ASYNC_OP_COMPLETE
#define LDR_ASYNC_OP_PENDING     ECAT_ASYNC_OP_PENDING

#define LDR_ASYNC_DATA_OP_INIT                ECAT_ASYNC_DATA_OP_INIT
#define LDR_ASYNC_DATA_OP_INVALIDATE          ECAT_ASYNC_DATA_OP_INVALIDATE
#define LDR_ASYNC_DATA_OP_ERASE_TO            ECAT_ASYNC_DATA_OP_ERASE_TO
#define LDR_ASYNC_DATA_OP_WRITE               ECAT_ASYNC_DATA_OP_WRITE
#define LDR_ASYNC_DATA_OP_READ                ECAT_ASYNC_DATA_OP_READ
#define LDR_ASYNC_DATA_OP_FINALIZE            ECAT_ASYNC_DATA_OP_FINALIZE
#define LDR_ASYNC_DATA_OP_READ_OPEN           ECAT_ASYNC_DATA_OP_READ_OPEN
#define LDR_ASYNC_DATA_OP_READ_CLOSE          ECAT_ASYNC_DATA_OP_READ_CLOSE
#define LDR_ASYNC_DATA_OP_FINALIZE_WITH_FLOOR ECAT_ASYNC_DATA_OP_FINALIZE_WITH_FLOOR

/* 双向传输核心层 */
typedef ecat_transfer_result_t   ldr_transfer_result_t;
typedef ecat_transfer_dir_t      ldr_transfer_dir_t;
typedef ecat_transfer_err_t      ldr_transfer_err_t;
typedef ecat_transfer_request_t  ldr_transfer_request_t;
typedef ecat_transfer_t          ldr_transfer_t;

#define LDR_DIR_IMPORT           ECAT_TRANSFER_DIR_IMPORT
#define LDR_DIR_EXPORT           ECAT_TRANSFER_DIR_EXPORT

#define LDR_RES_COMPLETE         ECAT_TRANSFER_COMPLETE
#define LDR_RES_CONTINUE         ECAT_TRANSFER_CONTINUE
#define LDR_RES_PENDING          ECAT_TRANSFER_PENDING
#define LDR_RES_FAILED           ECAT_TRANSFER_FAILED
#define LDR_RES_BUSY             ECAT_TRANSFER_BUSY
#define LDR_RES_INVALID          ECAT_TRANSFER_INVALID
#define LDR_RES_STALE            ECAT_TRANSFER_STALE
#define LDR_RES_DENIED           ECAT_TRANSFER_DENIED

/* 传输通道插件层 */
typedef ecat_transport_state_t        ldr_transport_state_t;
typedef ecat_transport_callbacks_t    ldr_transport_callbacks_t;
typedef ecat_transport_endpoint_t     ldr_transport_endpoint_t;
typedef ecat_transport_plugin_ops_t   ldr_transport_plugin_ops_t;
typedef ecat_transport_plugin_desc_t  ldr_transport_plugin_desc_t;
typedef ecat_transport_plugin_entry_t ldr_transport_plugin_entry_t;
typedef ecat_transport_registry_t     ldr_transport_registry_t;

#define LDR_TRANSPORT_ID_GENERIC   ECAT_TRANSPORT_ID_GENERIC
#define LDR_TRANSPORT_ID_FOE       ECAT_TRANSPORT_ID_FOE
#define LDR_TRANSPORT_ID_FILE      ECAT_TRANSPORT_ID_FILE
#define LDR_TRANSPORT_ID_UART      ECAT_TRANSPORT_ID_UART
#define LDR_TRANSPORT_ID_CAN       ECAT_TRANSPORT_ID_CAN
#define LDR_TRANSPORT_ID_USER_BASE ECAT_TRANSPORT_ID_USER_BASE

#define LDR_TRANSPORT_CAP_IMPORT        ECAT_TRANSPORT_CAP_IMPORT
#define LDR_TRANSPORT_CAP_EXPORT        ECAT_TRANSPORT_CAP_EXPORT
#define LDR_TRANSPORT_CAP_ASYNC         ECAT_TRANSPORT_CAP_ASYNC
#define LDR_TRANSPORT_CAP_BIDIRECTIONAL ECAT_TRANSPORT_CAP_BIDIRECTIONAL

/* -------------------------------------------------------------------------- */
/*                         统一 Loader 框架顶层句柄                           */
/* -------------------------------------------------------------------------- */

typedef struct {
    ldr_target_registry_t    targets;    /* 目标存储插件注册表 + ACL */
    ldr_transport_registry_t transports; /* 传输通道插件注册与轮询中心 */
    ldr_format_registry_t    formats;    /* 镜像格式与安全校验插件注册表 */
    ldr_transfer_t           transfer;   /* 统一双向传输管理器 */
    bool                     initialized;
} ldr_loader_t;

/* -------------------------------------------------------------------------- */
/*                         统一 Loader 框架 API 声明                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化统一中性 Loader 框架 (链接级零 FoE / 零 EFW 依赖)
 * @param loader 框架实例指针 (不可为 NULL)
 * @return 0 成功，-1 参数非法
 */
int ldr_loader_init(ldr_loader_t *loader);

/**
 * @brief 向统一 Loader 框架绑定可选固件升级后端虚表
 * @param loader 框架实例指针
 * @param fw_ctx 固件后端上下文指针 (如 ldr_fw_pipeline_t 或兼容 ecat_foe_core_t)
 * @param ops 固件后端操作虚表
 * @param shared_session 可选共享会话仲裁器指针 (NULL 时使用内置会话)
 * @return 0 成功，-1 参数非法或绑定冲突
 */
int ldr_loader_bind_fw_backend(
    ldr_loader_t *loader,
    void *fw_ctx,
    const ldr_fw_backend_ops_t *ops,
    ldr_session_t *shared_session);

/**
 * @brief 向统一 Loader 框架绑定可选异步固件操作虚表
 */
int ldr_loader_bind_fw_async_ops(
    ldr_loader_t *loader,
    const ldr_fw_async_ops_t *ops,
    void *async_ctx);

/**
 * @brief 注册目标存储区块插件 (固件区 / 参数区 / 日志区 / 自定义介质)
 */
int ldr_loader_register_target(
    ldr_loader_t *loader,
    const ldr_target_desc_t *desc);

/**
 * @brief 添加通道-目标访问控制规则 (ACL)
 */
int ldr_loader_add_acl(
    ldr_loader_t *loader,
    uint16_t transport_id,
    uint16_t target_id,
    uint8_t allowed_perms);

/**
 * @brief 设置是否启用强制 ACL 白名单模式
 */
void ldr_loader_set_enforce_acl(ldr_loader_t *loader, bool enforce);

/**
 * @brief 注册传输通道插件 (FoE / File / UART / CAN / USB / 自定义通道)
 */
int ldr_loader_register_transport(
    ldr_loader_t *loader,
    const ldr_transport_plugin_desc_t *desc);

/**
 * @brief 注销传输通道插件 (通道活跃时拒绝注销返回 -1)
 */
int ldr_loader_unregister_transport(
    ldr_loader_t *loader,
    uint16_t transport_id);

/**
 * @brief 注册镜像格式与安全校验插件 (EFW / RAW / 自定义格式)
 */
int ldr_loader_register_format(
    ldr_loader_t *loader,
    const ldr_format_plugin_desc_t *desc);

/**
 * @brief 注销镜像格式插件
 */
int ldr_loader_unregister_format(
    ldr_loader_t *loader,
    uint16_t format_id);

/**
 * @brief 自动探测并校验镜像头部，提取中性元数据
 */
ldr_format_probe_result_t ldr_loader_probe_format(
    const ldr_loader_t *loader,
    const uint8_t *buf,
    size_t len,
    uint16_t *out_header_size,
    ldr_format_metadata_t *out_meta);

/**
 * @brief 绑定异步 I/O 静态暂存缓冲区
 */
int ldr_loader_set_staging_buffer(
    ldr_loader_t *loader,
    uint8_t *buf,
    size_t capacity);

/**
 * @brief 主循环统一轮询入口：推进当前活跃通道插件
 */
ldr_transfer_result_t ldr_loader_poll(
    ldr_loader_t *loader,
    uint16_t *out_transport_id);

/**
 * @brief 紧急中止所有活跃传输通道会话并释放底层传输锁
 */
void ldr_loader_abort_all(ldr_loader_t *loader);

/**
 * @brief 查询当前是否有任意传输会话正在进行
 */
bool ldr_loader_is_busy(const ldr_loader_t *loader);

/**
 * @brief 获取当前活跃传输通道插件条目 (空闲时返回 NULL)
 */
const ldr_transport_plugin_entry_t *ldr_loader_get_active_transport(
    const ldr_loader_t *loader);

/**
 * @brief 获取内部双向传输管理器句柄 (供初始化传输端点或适配器)
 */
ldr_transfer_t *ldr_loader_get_transfer(ldr_loader_t *loader);

/**
 * @brief 获取目标注册表句柄
 */
ldr_target_registry_t *ldr_loader_get_targets(ldr_loader_t *loader);

/**
 * @brief 获取传输通道插件注册表句柄
 */
ldr_transport_registry_t *ldr_loader_get_transports(ldr_loader_t *loader);

/**
 * @brief 获取镜像格式插件注册表句柄
 */
ldr_format_registry_t *ldr_loader_get_formats(ldr_loader_t *loader);

/* -------------------------------------------------------------------------- */
/*                  纯 ldr_ 前缀传输端点封装 (消除 ecat_ 混用)                */
/* -------------------------------------------------------------------------- */

static inline int ldr_transport_endpoint_init(
    ldr_transport_endpoint_t *ep,
    uint16_t transport_id,
    const char *name,
    ldr_transfer_t *mgr,
    void *user_ctx)
{
    return ecat_transport_endpoint_init(ep, transport_id, name, mgr, user_ctx);
}

static inline void ldr_transport_endpoint_set_callbacks(
    ldr_transport_endpoint_t *ep,
    const ldr_transport_callbacks_t *cbs)
{
    ecat_transport_endpoint_set_callbacks(ep, cbs);
}

static inline ldr_transfer_result_t ldr_transport_endpoint_begin_import(
    ldr_transport_endpoint_t *ep,
    uint16_t target_id,
    uint32_t expected_size,
    uint32_t fw_version)
{
    return ecat_transport_endpoint_begin_import(ep, target_id, expected_size, fw_version);
}

static inline ldr_transfer_result_t ldr_transport_endpoint_write_chunk(
    ldr_transport_endpoint_t *ep,
    const uint8_t *data,
    size_t length)
{
    return ecat_transport_endpoint_write_chunk(ep, data, length);
}

static inline ldr_transfer_result_t ldr_transport_endpoint_finish_import(
    ldr_transport_endpoint_t *ep)
{
    return ecat_transport_endpoint_finish_import(ep);
}

static inline ldr_transfer_result_t ldr_transport_endpoint_begin_export(
    ldr_transport_endpoint_t *ep,
    uint16_t target_id,
    uint32_t *out_total_size)
{
    return ecat_transport_endpoint_begin_export(ep, target_id, out_total_size);
}

static inline uint32_t ldr_transport_endpoint_get_total_export_size(
    const ldr_transport_endpoint_t *ep)
{
    return ecat_transport_endpoint_get_total_export_size(ep);
}

static inline ldr_transfer_result_t ldr_transport_endpoint_read_chunk(
    ldr_transport_endpoint_t *ep,
    uint8_t *out_buf,
    size_t buf_capacity,
    size_t *actual_read)
{
    return ecat_transport_endpoint_read_chunk(ep, out_buf, buf_capacity, actual_read);
}

static inline ldr_transfer_result_t ldr_transport_endpoint_finish_export(
    ldr_transport_endpoint_t *ep)
{
    return ecat_transport_endpoint_finish_export(ep);
}

static inline void ldr_transport_endpoint_abort(ldr_transport_endpoint_t *ep)
{
    ecat_transport_endpoint_abort(ep);
}

static inline bool ldr_transport_endpoint_is_active(const ldr_transport_endpoint_t *ep)
{
    return ecat_transport_endpoint_is_active(ep);
}

static inline bool ldr_transport_endpoint_is_pending(const ldr_transport_endpoint_t *ep)
{
    return ecat_transport_endpoint_is_pending(ep);
}

static inline ldr_transfer_result_t ldr_transport_endpoint_poll(ldr_transport_endpoint_t *ep)
{
    return ecat_transport_endpoint_poll(ep);
}

static inline int ldr_transport_endpoint_make_plugin_desc(
    ldr_transport_endpoint_t *ep,
    uint32_t capabilities,
    ldr_transport_plugin_desc_t *out_desc)
{
    return ecat_transport_endpoint_make_plugin_desc(ep, capabilities, out_desc);
}

#ifdef __cplusplus
}
#endif

#endif /* LDR_LOADER_H */
