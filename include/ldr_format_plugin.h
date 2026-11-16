/**
 * @file ldr_format_plugin.h
 * @brief 统一格式与安全校验插件抽象层 (Format & Security Plugin Registry)
 *
 * 设计原则 (BARR-C:2018 / MISRA-C 友好, C99 零动态分配):
 * 1. 容器格式解耦: 核心框架不硬编码 EFW 或单一镜像格式；用户可按需注册 EFW、
 *    原始二进制或自定义加密/签名镜像容器插件。
 * 2. 零动态分配与无自引用指针: 固定槽位静态存储，支持安全探测、解析与注销。
 * 3. 零 FoE 依赖: 本模块仅依赖标准 C99 头文件，可在无 EtherCAT/FoE 的项目中独立裁剪使用。
 */

#ifndef LDR_FORMAT_PLUGIN_H
#define LDR_FORMAT_PLUGIN_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LDR_FORMAT_REGISTRY_MAX_PLUGINS
#define LDR_FORMAT_REGISTRY_MAX_PLUGINS 4U
#endif

#define LDR_FORMAT_PLUGIN_NAME_MAX_LEN 16U

/* 预留知名镜像格式插件 ID */
#define LDR_FORMAT_ID_RAW       1U
#define LDR_FORMAT_ID_EFW       2U
#define LDR_FORMAT_ID_USER_BASE 100U

/* 格式探测结果 */
typedef enum {
    LDR_FORMAT_MATCH = 0,       /* 魔数与基本头结构匹配，已确定头部总长 */
    LDR_FORMAT_NEED_MORE,       /* 字节数尚不足以判定，需要更多输入数据 */
    LDR_FORMAT_NO_MATCH,        /* 非本格式 (继续尝试其他已注册格式插件) */
    LDR_FORMAT_INVALID          /* 魔数归属本格式，但头部字段/校验非法 */
} ldr_format_probe_result_t;

#define LDR_FORMAT_KEY_ID_MAX_LEN     16U
#define LDR_FORMAT_SIGNATURE_MAX_LEN  64U

/* 解析后的中性固件元数据 */
typedef struct {
    uint16_t format_id;         /* 命中的格式插件 ID */
    uint16_t header_size;       /* 头部总长度 (字节) */
    uint16_t target_id;         /* 目标存储区块 ID (0 表示由会话层决定) */
    uint32_t fw_version;        /* 固件版本号 */
    uint32_t min_allow_version; /* 防回滚最低允许版本 */
    uint32_t payload_size;      /* 有效负载长度 (字节) */
    uint32_t payload_crc32;     /* 有效负载 CRC32 (当 has_payload_crc32 为 true 时有效) */
    bool     has_payload_crc32; /* 是否携带有效负载 CRC32 校验字段 */
    bool     is_signed;         /* 是否包含数字签名 */
    uint8_t  key_id[LDR_FORMAT_KEY_ID_MAX_LEN];       /* 签名密钥 ID (可选) */
    uint8_t  key_id_len;                              /* 有效密钥 ID 长度 */
    uint8_t  signature[LDR_FORMAT_SIGNATURE_MAX_LEN]; /* 数字签名原始字节 (可选) */
    uint8_t  signature_len;                           /* 有效签名长度 */
} ldr_format_metadata_t;

/* 格式与安全插件操作虚表 */
typedef struct {
    /**
     * @brief 轻量探测前导字节流是否属于本格式
     * @param buf 输入字节流
     * @param len 输入长度
     * @param out_header_size 当返回 LDR_FORMAT_MATCH 时输出完整头部长度
     * @param plugin_ctx 插件上下文
     */
    ldr_format_probe_result_t (*probe)(
        const uint8_t *buf,
        size_t len,
        uint16_t *out_header_size,
        void *plugin_ctx);

    /**
     * @brief 解析并校验完整头部，填充中性元数据
     * @param header_buf 完整头部缓冲区
     * @param header_len 头部长度
     * @param out_meta 输出中性元数据
     * @param plugin_ctx 插件上下文
     * @return 0 校验通过，-1 头部非法或策略校验失败
     */
    int (*parse_and_verify)(
        const uint8_t *header_buf,
        size_t header_len,
        ldr_format_metadata_t *out_meta,
        void *plugin_ctx);
} ldr_format_plugin_ops_t;

/* 格式插件注册描述符 (注册入参) */
typedef struct {
    uint16_t format_id;                      /* 格式插件唯一 ID (非 0) */
    const char *name;                        /* 格式名称 (非空，长度 < 16) */
    const ldr_format_plugin_ops_t *ops;      /* 操作虚表 (probe 与 parse_and_verify 均必填) */
    void *plugin_ctx;                        /* 可选上下文指针 */
} ldr_format_plugin_desc_t;

/* 格式插件静态存储槽位 (无自引用指针) */
typedef struct {
    uint16_t format_id;
    char name[LDR_FORMAT_PLUGIN_NAME_MAX_LEN];
    const ldr_format_plugin_ops_t *ops;
    void *plugin_ctx;
    bool in_use;
} ldr_format_plugin_entry_t;

/* 格式插件注册表 */
typedef struct {
    ldr_format_plugin_entry_t entries[LDR_FORMAT_REGISTRY_MAX_PLUGINS];
    uint8_t count;
} ldr_format_registry_t;

/* -------------------------------------------------------------------------- */
/*                                API 接口声明                                */
/* -------------------------------------------------------------------------- */

int ldr_format_registry_init(ldr_format_registry_t *reg);

int ldr_format_registry_register(
    ldr_format_registry_t *reg,
    const ldr_format_plugin_desc_t *desc);

int ldr_format_registry_unregister(
    ldr_format_registry_t *reg,
    uint16_t format_id);

const ldr_format_plugin_entry_t *ldr_format_registry_find(
    const ldr_format_registry_t *reg,
    uint16_t format_id);

const ldr_format_plugin_entry_t *ldr_format_registry_find_by_name(
    const ldr_format_registry_t *reg,
    const char *name);

uint8_t ldr_format_registry_count(const ldr_format_registry_t *reg);

/**
 * @brief 自动探测并解析输入头部缓冲区
 *
 * 按注册顺序依次调用各格式插件的 probe():
 * - 若命中 MATCH 且 len >= header_size，立即调用 parse_and_verify() 并填充 out_meta；
 * - 若命中 MATCH 但 len < header_size，返回 LDR_FORMAT_NEED_MORE 并通过 out_header_size 告知所需总长；
 * - 若所有插件均返回 NO_MATCH，返回 LDR_FORMAT_NO_MATCH；
 * - 若存在 NEED_MORE 且无 MATCH/INVALID，返回 LDR_FORMAT_NEED_MORE。
 *
 * @param reg 格式注册表
 * @param buf 输入数据缓冲区
 * @param len 输入长度
 * @param out_header_size 可选，传出命中格式所需的头部长度
 * @param out_meta 传出解析验证后的中性元数据 (返回 MATCH 时有效)
 * @return 探测与解析结果
 */
ldr_format_probe_result_t ldr_format_registry_probe_and_parse(
    const ldr_format_registry_t *reg,
    const uint8_t *buf,
    size_t len,
    uint16_t *out_header_size,
    ldr_format_metadata_t *out_meta);

#ifdef __cplusplus
}
#endif

#endif /* LDR_FORMAT_PLUGIN_H */
