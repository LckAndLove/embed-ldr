/**
 * @file ldr_format_raw.c
 * @brief 内置可选原始二进制 (RAW .bin) 镜像格式插件实现
 */

#include "ldr_format_raw.h"
#include <string.h>

static ldr_format_probe_result_t raw_plugin_probe(
    const uint8_t *buf,
    size_t len,
    uint16_t *out_header_size,
    void *plugin_ctx)
{
    const ldr_format_raw_ctx_t *ctx = (const ldr_format_raw_ctx_t *)plugin_ctx;

    if ((buf == NULL) || (out_header_size == NULL) || (ctx == NULL)) {
        return LDR_FORMAT_INVALID;
    }
    if (ctx->payload_size == 0U) {
        return LDR_FORMAT_INVALID;
    }
    if (len == 0U) {
        return LDR_FORMAT_NEED_MORE;
    }

    *out_header_size = 0U;
    return LDR_FORMAT_MATCH;
}

static int raw_plugin_parse_and_verify(
    const uint8_t *header_buf,
    size_t header_len,
    ldr_format_metadata_t *out_meta,
    void *plugin_ctx)
{
    const ldr_format_raw_ctx_t *ctx = (const ldr_format_raw_ctx_t *)plugin_ctx;
    (void)header_buf;

    if ((ctx == NULL) || (out_meta == NULL) || (header_len != 0U) || (ctx->payload_size == 0U)) {
        return -1;
    }

    out_meta->format_id = LDR_FORMAT_ID_RAW;
    out_meta->header_size = 0U;
    out_meta->target_id = ctx->target_id;
    out_meta->fw_version = ctx->fw_version;
    out_meta->min_allow_version = ctx->min_allow_version;
    out_meta->payload_size = ctx->payload_size;
    out_meta->payload_crc32 = ctx->payload_crc32;
    out_meta->has_payload_crc32 = ctx->has_payload_crc32;
    out_meta->is_signed = ctx->is_signed;

    if (ctx->is_signed) {
        if ((ctx->key_id_len > LDR_FORMAT_KEY_ID_MAX_LEN) ||
            (ctx->signature_len > LDR_FORMAT_SIGNATURE_MAX_LEN) ||
            (ctx->signature_len == 0U)) {
            return -1;
        }
        if (ctx->key_id_len > 0U) {
            (void)memcpy(out_meta->key_id, ctx->key_id, (size_t)ctx->key_id_len);
        }
        out_meta->key_id_len = ctx->key_id_len;
        (void)memcpy(out_meta->signature, ctx->signature, (size_t)ctx->signature_len);
        out_meta->signature_len = ctx->signature_len;
    }

    return 0;
}

static const ldr_format_plugin_ops_t s_raw_format_ops = {
    raw_plugin_probe,
    raw_plugin_parse_and_verify
};

int ldr_format_raw_make_plugin_desc(
    ldr_format_raw_ctx_t *ctx,
    ldr_format_plugin_desc_t *out_desc)
{
    if ((ctx == NULL) || (out_desc == NULL)) {
        return -1;
    }

    out_desc->format_id = LDR_FORMAT_ID_RAW;
    out_desc->name = "RAW_BIN";
    out_desc->ops = &s_raw_format_ops;
    out_desc->plugin_ctx = ctx;
    return 0;
}
