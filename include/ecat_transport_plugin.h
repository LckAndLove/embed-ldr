/**
 * @file ecat_transport_plugin.h
 * @brief 统一传输通道插件抽象与多通道注册/轮询中心 (Unified Transport Plugin Registry)
 *
 * 设计原则 (BARR-C:2018 / MISRA-C 友好, C99 零动态分配):
 * 1. 统一插件契约: 内置通道 (FoE、文件流) 与用户自定义通道 (UART、CAN、USB、BLE 等)
 *    通过同一套 ecat_transport_plugin_ops_t 虚表接入，支持双向 IMPORT / EXPORT 能力声明。
 * 2. 零耦合裁剪: 本模块不依赖任何具体传输实现 (FoE / stdio)，按需链接，未启用插件 0 字节占用。
 * 3. 统一主循环推进: 通过 ecat_transport_registry_poll_active() 与 abort_all() 统一调度多通道。
 * 4. 零动态分配与无自引用指针: 固定槽位静态存储，安全可拷贝。
 */

#ifndef ECAT_TRANSPORT_PLUGIN_H
#define ECAT_TRANSPORT_PLUGIN_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "ecat_transfer.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ECAT_TRANSPORT_REGISTRY_MAX_PLUGINS
#define ECAT_TRANSPORT_REGISTRY_MAX_PLUGINS 8U
#endif

#define ECAT_TRANSPORT_PLUGIN_NAME_MAX_LEN 16U

/* 通道能力位掩码 */
#define ECAT_TRANSPORT_CAP_IMPORT        (1UL << 0U)
#define ECAT_TRANSPORT_CAP_EXPORT        (1UL << 1U)
#define ECAT_TRANSPORT_CAP_ASYNC         (1UL << 2U)
#define ECAT_TRANSPORT_CAP_BIDIRECTIONAL (ECAT_TRANSPORT_CAP_IMPORT | ECAT_TRANSPORT_CAP_EXPORT)

#define ECAT_TRANSPORT_CAP_ALL_MASK \
    (ECAT_TRANSPORT_CAP_IMPORT | ECAT_TRANSPORT_CAP_EXPORT | ECAT_TRANSPORT_CAP_ASYNC)

/* 统一传输通道插件操作虚表 */
typedef struct {
    /** 推进一步或轮询当前活动通道的异步状态 (纯事件驱动通道可为 NULL) */
    ecat_transfer_result_t (*poll)(void *plugin_ctx);
    /** 强制中止当前通道的活动会话并释放底层传输锁 (可选，可为 NULL) */
    void (*abort)(void *plugin_ctx);
    /** 查询当前通道是否持有活动会话 (必填，不可为 NULL) */
    bool (*is_active)(const void *plugin_ctx);
    /** 查询当前通道是否处于异步挂起态 (可选，NULL 等价于 false) */
    bool (*is_pending)(const void *plugin_ctx);
} ecat_transport_plugin_ops_t;

/* 通道插件注册描述符 (用于注册入参) */
typedef struct {
    uint16_t transport_id;                   /* 通道唯一 ID (同一注册表内不可重复) */
    const char *name;                        /* 通道名称 (非空，长度 < ECAT_TRANSPORT_PLUGIN_NAME_MAX_LEN) */
    uint32_t capabilities;                   /* ECAT_TRANSPORT_CAP_* 能力掩码 (至少含 IMPORT 或 EXPORT) */
    const ecat_transport_plugin_ops_t *ops;  /* 插件操作虚表 */
    void *plugin_ctx;                        /* 插件实例上下文指针 */
} ecat_transport_plugin_desc_t;

/* 注册表内部静态存储槽位 (无自引用指针) */
typedef struct {
    uint16_t transport_id;
    char name[ECAT_TRANSPORT_PLUGIN_NAME_MAX_LEN];
    uint32_t capabilities;
    const ecat_transport_plugin_ops_t *ops;
    void *plugin_ctx;
    bool in_use;
} ecat_transport_plugin_entry_t;

/* 多通道统一注册与轮询中心 */
typedef struct {
    ecat_transfer_t *transfer_mgr;
    ecat_transport_plugin_entry_t entries[ECAT_TRANSPORT_REGISTRY_MAX_PLUGINS];
    uint8_t count;
} ecat_transport_registry_t;

/* -------------------------------------------------------------------------- */
/*                                API 接口声明                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化统一传输通道插件注册表
 * @param reg 注册表上下文指针 (必须非 NULL)
 * @param transfer_mgr 关联的统一双向传输管理器 (可选，允许为 NULL)
 * @return 0 成功，-1 参数非法
 */
int ecat_transport_registry_init(
    ecat_transport_registry_t *reg,
    ecat_transfer_t *transfer_mgr);

/**
 * @brief 注册一个传输通道插件
 * @param reg 注册表上下文指针
 * @param desc 插件描述符
 * @return 0 成功，-1 参数非法、ID/名称重复或槽位已满
 */
int ecat_transport_registry_register(
    ecat_transport_registry_t *reg,
    const ecat_transport_plugin_desc_t *desc);

/**
 * @brief 注销一个已注册的传输通道插件
 * @note 若该插件当前处于活动会话状态 (is_active == true)，拒绝注销并返回 -1，防止孤儿会话。
 * @param reg 注册表上下文指针
 * @param transport_id 待注销通道 ID
 * @return 0 成功，-1 未找到、参数非法或通道正忙
 */
int ecat_transport_registry_unregister(
    ecat_transport_registry_t *reg,
    uint16_t transport_id);

/**
 * @brief 按通道 ID 查找已注册插件条目
 */
const ecat_transport_plugin_entry_t *ecat_transport_registry_find(
    const ecat_transport_registry_t *reg,
    uint16_t transport_id);

/**
 * @brief 按通道名称查找已注册插件条目
 */
const ecat_transport_plugin_entry_t *ecat_transport_registry_find_by_name(
    const ecat_transport_registry_t *reg,
    const char *name);

/**
 * @brief 获取已注册插件数量
 */
uint8_t ecat_transport_registry_count(const ecat_transport_registry_t *reg);

/**
 * @brief 获取当前处于活动状态的传输通道插件条目 (无活动会话时返回 NULL)
 */
const ecat_transport_plugin_entry_t *ecat_transport_registry_get_active(
    const ecat_transport_registry_t *reg);

/**
 * @brief 检查指定通道是否支持某组能力位掩码
 */
bool ecat_transport_registry_supports(
    const ecat_transport_registry_t *reg,
    uint16_t transport_id,
    uint32_t cap_mask);

/**
 * @brief 统一轮询当前活动通道插件 (适合主循环周期性调用)
 * @param reg 注册表上下文指针
 * @param out_transport_id 可选，传出本次被轮询的活动通道 ID (无活动通道时置 0)
 * @return 活动通道 poll 的结果码；若当前无活动通道或活动通道未提供 poll 回调，返回 ECAT_TRANSFER_INVALID
 */
ecat_transfer_result_t ecat_transport_registry_poll_active(
    ecat_transport_registry_t *reg,
    uint16_t *out_transport_id);

/**
 * @brief 统一中止所有当前处于活动状态的通道插件 (用于紧急停机或状态切换清理)
 */
void ecat_transport_registry_abort_all(ecat_transport_registry_t *reg);

#ifdef __cplusplus
}
#endif

#endif /* ECAT_TRANSPORT_PLUGIN_H */
