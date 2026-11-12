/**
 * @file ldr_format_efw.h
 * @brief 内置可选 EFW (format v2) 镜像容器格式插件
 *
 * 将现有 .efw 解析校验器封装为标准的 ldr_format_plugin_desc_t，
 * 可按需注册到 ldr_format_registry_t / ldr_loader_t，未使用时可完全裁剪。
 */

#ifndef LDR_FORMAT_EFW_H
#define LDR_FORMAT_EFW_H

#include "ldr_format_plugin.h"
#include "ecat_foe_efw.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t max_header_size;            /* 允许的头部上限 (0 表示使用默认 148 字节) */
    bool     enforce_policy;             /* 是否在 parse_and_verify 阶段附加执行 stage3 策略核验 */
    ecat_foe_efw_config_t policy_cfg;    /* 当 enforce_policy == true 时生效的本地策略配置 */
    uint16_t expected_target_id;         /* 当 enforce_policy == true 时生效的期望目标 ID */
    ecat_foe_efw_target_cfg_t target_cfg;/* 当 enforce_policy == true 时生效的目标限额配置 */
} ldr_format_efw_ctx_t;

/**
 * @brief 构造 EFW 格式插件注册描述符
 * @param ctx EFW 格式插件配置上下文 (可为 NULL，表示仅执行 stage1/stage2 结构与 CRC32 校验)
 * @param out_desc 输出插件描述符
 * @return 0 成功，-1 参数非法
 */
int ldr_format_efw_make_plugin_desc(
    ldr_format_efw_ctx_t *ctx,
    ldr_format_plugin_desc_t *out_desc);

#ifdef __cplusplus
}
#endif

#endif /* LDR_FORMAT_EFW_H */
