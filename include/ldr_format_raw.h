/**
 * @file ldr_format_raw.h
 * @brief 内置可选原始二进制 (RAW .bin) 镜像格式插件
 *
 * 适用场景:
 * - 无容器头部的纯二进制固件刷写 (header_size == 0)，有效载荷长度与可选校验信息由外部控制面或本地静态配置提供。
 * - 零 FoE / 零 EFW 依赖，零动态分配，BARR-C:2018 / MISRA-C 友好。
 */

#ifndef LDR_FORMAT_RAW_H
#define LDR_FORMAT_RAW_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "ldr_format_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t target_id;          /* 期望目标区块 ID (0 表示不限制，沿用会话目标) */
    uint32_t fw_version;         /* 固件版本号 */
    uint32_t min_allow_version;  /* 防回滚最低允许版本 */
    uint32_t payload_size;       /* 预期原始二进制总字节数 (必须 > 0) */
    uint32_t payload_crc32;      /* 预期载荷 CRC32 (当 has_payload_crc32 为 true 时生效) */
    bool     has_payload_crc32;  /* 是否强制校验载荷 CRC32 */
    bool     is_signed;          /* 是否携带外部带外签名 */
    uint8_t  key_id[LDR_FORMAT_KEY_ID_MAX_LEN];
    uint8_t  key_id_len;
    uint8_t  signature[LDR_FORMAT_SIGNATURE_MAX_LEN];
    uint8_t  signature_len;
} ldr_format_raw_ctx_t;

/**
 * @brief 构造内置 RAW (.bin) 格式插件描述符 (format_id = LDR_FORMAT_ID_RAW)
 * @param ctx RAW 元数据上下文 (不可为 NULL，刷写前需填入有效 payload_size)
 * @param out_desc 输出格式插件描述符
 * @return 0 成功，-1 参数非法
 */
int ldr_format_raw_make_plugin_desc(
    ldr_format_raw_ctx_t *ctx,
    ldr_format_plugin_desc_t *out_desc);

#ifdef __cplusplus
}
#endif

#endif /* LDR_FORMAT_RAW_H */
