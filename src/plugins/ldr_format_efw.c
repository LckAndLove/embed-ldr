/**
 * @file ldr_format_efw.c
 * @brief 内置可选 EFW (format v2) 镜像容器格式插件实现
 */

#include "ldr_format_efw.h"
#include <string.h>

static ldr_format_probe_result_t efw_plugin_probe(
    const uint8_t *buf,
    size_t len,
    uint16_t *out_header_size,
    void *plugin_ctx)
{
    const ldr_format_efw_ctx_t *ctx = (const ldr_format_efw_ctx_t *)plugin_ctx;
    uint16_t max_hdr = (ctx != NULL) ? ctx->max_header_size : 0U;
    ecat_foe_efw_err_t err;

    if ((buf == NULL) || (out_header_size == NULL)) {
        return LDR_FORMAT_INVALID;
    }
    if (len < ECAT_FOE_EFW_STAGE1_PROBE_LEN) {
        return LDR_FORMAT_NEED_MORE;
    }

    err = ecat_foe_efw_parse_stage1(buf, len, max_hdr, out_header_size);
    if (err == ECAT_FOE_EFW_OK) {
        return LDR_FORMAT_MATCH;
    }
    if (err == ECAT_FOE_EFW_ERR_MAGIC) {
        return LDR_FORMAT_NO_MATCH;
    }
    return LDR_FORMAT_INVALID;
}

static int efw_plugin_parse_and_verify(
    const uint8_t *header_buf,
    size_t header_len,
    ldr_format_metadata_t *out_meta,
    void *plugin_ctx)
{
    const ldr_format_efw_ctx_t *ctx = (const ldr_format_efw_ctx_t *)plugin_ctx;
    ecat_foe_efw_header_t hdr;
    ecat_foe_efw_err_t err;

    if ((header_buf == NULL) || (out_meta == NULL)) {
        return -1;
    }

    (void)memset(&hdr, 0, sizeof(hdr));
    err = ecat_foe_efw_parse_stage2(header_buf, header_len, &hdr);
    if (err != ECAT_FOE_EFW_OK) {
        return -1;
    }

    if ((ctx != NULL) && ctx->enforce_policy) {
        err = ecat_foe_efw_parse_stage3(
            &hdr,
            &ctx->policy_cfg,
            ctx->expected_target_id,
            &ctx->target_cfg);
        if (err != ECAT_FOE_EFW_OK) {
            return -1;
        }
    }

    out_meta->format_id = LDR_FORMAT_ID_EFW;
    out_meta->header_size = hdr.header_length;
    out_meta->target_id = hdr.target_id;
    out_meta->fw_version = hdr.fw_version;
    out_meta->min_allow_version = hdr.min_allow_version;
    out_meta->payload_size = hdr.payload_length;
    out_meta->payload_crc32 = hdr.payload_crc32;
    out_meta->is_signed = hdr.signed_package;
    if (hdr.signed_package) {
        (void)memcpy(out_meta->key_id, hdr.key_id, ECAT_FOE_EFW_KEY_ID_LEN);
        out_meta->key_id_len = (uint8_t)ECAT_FOE_EFW_KEY_ID_LEN;
        (void)memcpy(out_meta->signature, hdr.signature, ECAT_FOE_EFW_SIGNATURE_LEN);
        out_meta->signature_len = (uint8_t)ECAT_FOE_EFW_SIGNATURE_LEN;
    }
    return 0;
}

static const ldr_format_plugin_ops_t s_efw_format_ops = {
    efw_plugin_probe,
    efw_plugin_parse_and_verify
};

int ldr_format_efw_make_plugin_desc(
    ldr_format_efw_ctx_t *ctx,
    ldr_format_plugin_desc_t *out_desc)
{
    if (out_desc == NULL) {
        return -1;
    }

    out_desc->format_id = LDR_FORMAT_ID_EFW;
    out_desc->name = "EFW_V2";
    out_desc->ops = &s_efw_format_ops;
    out_desc->plugin_ctx = ctx;
    return 0;
}
