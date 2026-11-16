/**
 * @file test_ldr_framework.c
 * @brief 统一中性 ldr_ Loader 框架 (去 FoE 化核心 + 3 类插件层 + 格式驱动刷写流水线) 单元测试
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ldr_loader.h"
#include "ldr_format_efw.h"
#include "ldr_fw_pipeline.h"
#include "ldr_crc32.h"

/* -------------------------------------------------------------------------- */
/*          自定义格式插件示例: "MYFW" (20 字节头部，含 payload CRC32)        */
/* -------------------------------------------------------------------------- */

#define CUSTOM_MYFW_FORMAT_ID (LDR_FORMAT_ID_USER_BASE + 1U)
#define CUSTOM_MYFW_HDR_LEN   20U

static uint32_t read_le32(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) |
           ((uint32_t)p[3] << 24U);
}

static void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8U) & 0xFFU);
}

static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8U) & 0xFFU);
    p[2] = (uint8_t)((v >> 16U) & 0xFFU);
    p[3] = (uint8_t)((v >> 24U) & 0xFFU);
}

static ldr_format_probe_result_t myfw_probe(
    const uint8_t *buf,
    size_t len,
    uint16_t *out_header_size,
    void *plugin_ctx)
{
    (void)plugin_ctx;
    if (len < 4U) {
        return LDR_FORMAT_NEED_MORE;
    }
    if (memcmp(buf, "MYFW", 4U) != 0) {
        return LDR_FORMAT_NO_MATCH;
    }
    *out_header_size = CUSTOM_MYFW_HDR_LEN;
    return LDR_FORMAT_MATCH;
}

static int myfw_parse_and_verify(
    const uint8_t *header_buf,
    size_t header_len,
    ldr_format_metadata_t *out_meta,
    void *plugin_ctx)
{
    (void)plugin_ctx;
    if (header_len < CUSTOM_MYFW_HDR_LEN) {
        return -1;
    }
    out_meta->target_id = (uint16_t)read_le32(&header_buf[4]);
    out_meta->fw_version = read_le32(&header_buf[8]);
    out_meta->payload_size = read_le32(&header_buf[12]);
    out_meta->payload_crc32 = read_le32(&header_buf[16]);
    out_meta->is_signed = false;
    return (out_meta->payload_size > 0U) ? 0 : -1;
}

static const ldr_format_plugin_ops_t s_myfw_ops = {
    myfw_probe,
    myfw_parse_and_verify
};

static void build_myfw_header(uint8_t out_hdr[CUSTOM_MYFW_HDR_LEN],
                              uint16_t target_id,
                              uint32_t fw_version,
                              uint32_t payload_size,
                              uint32_t payload_crc32)
{
    (void)memcpy(&out_hdr[0], "MYFW", 4U);
    write_le32(&out_hdr[4], (uint32_t)target_id);
    write_le32(&out_hdr[8], fw_version);
    write_le32(&out_hdr[12], payload_size);
    write_le32(&out_hdr[16], payload_crc32);
}

static void build_valid_efw_header(uint8_t out_hdr[ECAT_FOE_EFW_HEADER_MIN_LEN],
                                   uint16_t target_id,
                                   uint32_t fw_version,
                                   uint32_t payload_size,
                                   uint32_t payload_crc32)
{
    (void)memset(out_hdr, 0, ECAT_FOE_EFW_HEADER_MIN_LEN);
    write_le32(&out_hdr[0x00], ECAT_FOE_EFW_MAGIC);
    write_le16(&out_hdr[0x04], ECAT_FOE_EFW_FORMAT_VERSION);
    write_le16(&out_hdr[0x06], ECAT_FOE_EFW_HEADER_MIN_LEN);
    write_le32(&out_hdr[0x08], 0x10010001U);
    write_le32(&out_hdr[0x0C], 0x00000001U);
    write_le16(&out_hdr[0x10], 1U);
    write_le16(&out_hdr[0x12], 2U);
    write_le16(&out_hdr[0x14], target_id);
    write_le16(&out_hdr[0x16], 0U);
    write_le32(&out_hdr[0x18], fw_version);
    write_le32(&out_hdr[0x1C], 0x01000000U);
    write_le32(&out_hdr[0x20], payload_size);
    write_le32(&out_hdr[0x24], payload_crc32);
    out_hdr[60] = (uint8_t)'S';
    out_hdr[61] = (uint8_t)'I';
    out_hdr[62] = (uint8_t)'G';
    out_hdr[63] = (uint8_t)'2';
    write_le16(&out_hdr[64], 1U);
    uint32_t hdr_crc = ecat_foe_crc32_calc(out_hdr, ECAT_FOE_EFW_HEADER_MIN_LEN - 4U);
    write_le32(&out_hdr[ECAT_FOE_EFW_HEADER_MIN_LEN - 4U], hdr_crc);
}

/* -------------------------------------------------------------------------- */
/*                     测试用内存 Flash / 数据存储驱动                        */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t storage[512];
    uint32_t data_size;
    uint32_t erased_end;
    uint32_t init_calls;
    uint32_t invalidate_calls;
    uint32_t erase_calls;
    uint32_t write_calls;
    uint32_t read_calls;
    uint32_t finalize_calls;
    uint32_t finalize_floor_calls;
    uint32_t abort_calls;
    uint32_t persisted_floor;
    bool inject_readback_corrupt;
    bool inject_floor_commit_fail;
} mock_flash_drv_t;

static void mock_flash_reset_stats(mock_flash_drv_t *d)
{
    d->init_calls = 0U;
    d->invalidate_calls = 0U;
    d->erase_calls = 0U;
    d->write_calls = 0U;
    d->read_calls = 0U;
    d->finalize_calls = 0U;
    d->finalize_floor_calls = 0U;
    d->abort_calls = 0U;
    d->inject_readback_corrupt = false;
    d->inject_floor_commit_fail = false;
}

static int drv_init(void *ctx)
{
    mock_flash_drv_t *d = (mock_flash_drv_t *)ctx;
    d->init_calls++;
    d->erased_end = 0U;
    d->data_size = 0U;
    return 0;
}

static int drv_invalidate(void *ctx)
{
    mock_flash_drv_t *d = (mock_flash_drv_t *)ctx;
    d->invalidate_calls++;
    return 0;
}

static int drv_erase_to(uint32_t need_end, uint32_t *erased_end, void *ctx)
{
    mock_flash_drv_t *d = (mock_flash_drv_t *)ctx;
    d->erase_calls++;
    if (need_end > sizeof(d->storage)) {
        return -1;
    }
    if (need_end > d->erased_end) {
        (void)memset(&d->storage[d->erased_end], 0xFF, need_end - d->erased_end);
        d->erased_end = need_end;
    }
    *erased_end = d->erased_end;
    return 0;
}

static int drv_write(uint32_t offset, const uint8_t *data, uint32_t len, void *ctx)
{
    mock_flash_drv_t *d = (mock_flash_drv_t *)ctx;
    d->write_calls++;
    if ((offset + len) > sizeof(d->storage)) {
        return -1;
    }
    (void)memcpy(&d->storage[offset], data, len);
    if ((offset + len) > d->data_size) {
        d->data_size = offset + len;
    }
    return 0;
}

static int drv_read(uint32_t offset, uint8_t *buf, uint32_t len, void *ctx)
{
    mock_flash_drv_t *d = (mock_flash_drv_t *)ctx;
    d->read_calls++;
    if ((offset + len) > sizeof(d->storage)) {
        return -1;
    }
    (void)memcpy(buf, &d->storage[offset], len);
    if (d->inject_readback_corrupt && (offset == 0U) && (len > 0U)) {
        buf[0] ^= 0x01U;
    }
    return 0;
}

static int drv_finalize(void *ctx)
{
    mock_flash_drv_t *d = (mock_flash_drv_t *)ctx;
    d->finalize_calls++;
    return 0;
}

static void drv_abort(void *ctx)
{
    mock_flash_drv_t *d = (mock_flash_drv_t *)ctx;
    d->abort_calls++;
}

static int drv_finalize_with_floor(uint16_t target_id, uint32_t fw_version,
                                   uint32_t min_allow_version, void *ctx)
{
    mock_flash_drv_t *d = (mock_flash_drv_t *)ctx;
    (void)target_id;
    d->finalize_floor_calls++;
    if (d->inject_floor_commit_fail || (fw_version < d->persisted_floor)) {
        return -1;
    }
    if (min_allow_version > d->persisted_floor) {
        d->persisted_floor = min_allow_version;
    }
    return 0;
}

static int drv_read_open(void *ctx)
{
    (void)ctx;
    return 0;
}

static void drv_read_close(void *ctx)
{
    (void)ctx;
}

static uint32_t drv_get_data_size(void *ctx)
{
    const mock_flash_drv_t *d = (const mock_flash_drv_t *)ctx;
    return d->data_size;
}

/* -------------------------------------------------------------------------- */
/*                                 测试用例                                   */
/* -------------------------------------------------------------------------- */

static void test_format_plugin_registry_and_autodetect(void)
{
    ldr_loader_t loader;
    ldr_format_efw_ctx_t efw_ctx;
    ldr_format_plugin_desc_t efw_desc;
    ldr_format_plugin_desc_t myfw_desc;
    ldr_format_metadata_t meta;
    uint16_t hdr_size = 0U;

    assert(ldr_loader_init(&loader) == 0);

    (void)memset(&efw_ctx, 0, sizeof(efw_ctx));
    efw_ctx.enforce_policy = true;
    efw_ctx.policy_cfg.project_id = 0x10010001U;
    efw_ctx.policy_cfg.hardware_id = 0x00000001U;
    efw_ctx.policy_cfg.local_hw_ver = 1U;
    efw_ctx.expected_target_id = 1U;
    efw_ctx.target_cfg.enabled = true;
    efw_ctx.target_cfg.max_payload_size = 4096U;

    assert(ldr_format_efw_make_plugin_desc(&efw_ctx, &efw_desc) == 0);
    assert(ldr_loader_register_format(&loader, &efw_desc) == 0);

    myfw_desc.format_id = CUSTOM_MYFW_FORMAT_ID;
    myfw_desc.name = "MYFW_V1";
    myfw_desc.ops = &s_myfw_ops;
    myfw_desc.plugin_ctx = NULL;
    assert(ldr_loader_register_format(&loader, &myfw_desc) == 0);
    assert(ldr_format_registry_count(ldr_loader_get_formats(&loader)) == 2U);

    /* 1. 自动识别并验证合法 EFW v2 头部 */
    uint8_t efw_hdr[ECAT_FOE_EFW_HEADER_MIN_LEN];
    build_valid_efw_header(efw_hdr, 1U, 0x01020304U, 1024U, 0x12345678U);
    assert(ldr_loader_probe_format(&loader, efw_hdr, 4U, &hdr_size, &meta) == LDR_FORMAT_NEED_MORE);
    assert(ldr_loader_probe_format(&loader, efw_hdr, 32U, &hdr_size, &meta) == LDR_FORMAT_NEED_MORE);
    assert(hdr_size == ECAT_FOE_EFW_HEADER_MIN_LEN);

    assert(ldr_loader_probe_format(&loader, efw_hdr, sizeof(efw_hdr), &hdr_size, &meta) == LDR_FORMAT_MATCH);
    assert(meta.format_id == LDR_FORMAT_ID_EFW);
    assert(meta.target_id == 1U);
    assert(meta.fw_version == 0x01020304U);
    assert(meta.payload_size == 1024U);
    assert(meta.payload_crc32 == 0x12345678U);

    /* 损坏 CRC32 -> LDR_FORMAT_INVALID */
    efw_hdr[10] ^= 0xFFU;
    assert(ldr_loader_probe_format(&loader, efw_hdr, sizeof(efw_hdr), &hdr_size, &meta) == LDR_FORMAT_INVALID);

    /* 2. 自动识别自定义 "MYFW" 格式 */
    uint8_t myfw_hdr[CUSTOM_MYFW_HDR_LEN];
    build_myfw_header(myfw_hdr, 20U, 0x00020001U, 512U, 0xAABBCCDDU);
    assert(ldr_loader_probe_format(&loader, myfw_hdr, sizeof(myfw_hdr), &hdr_size, &meta) == LDR_FORMAT_MATCH);
    assert(meta.format_id == CUSTOM_MYFW_FORMAT_ID);
    assert(meta.header_size == CUSTOM_MYFW_HDR_LEN);
    assert(meta.target_id == 20U);
    assert(meta.fw_version == 0x00020001U);
    assert(meta.payload_size == 512U);
    assert(meta.payload_crc32 == 0xAABBCCDDU);

    /* 3. 未知魔数 -> LDR_FORMAT_NO_MATCH */
    uint8_t unknown_hdr[20] = { 'X', 'Y', 'Z', '1' };
    assert(ldr_loader_probe_format(&loader, unknown_hdr, sizeof(unknown_hdr), &hdr_size, &meta) == LDR_FORMAT_NO_MATCH);

    /* 4. 注销自定义格式后，再次探测 MYFW 返回 NO_MATCH */
    assert(ldr_loader_unregister_format(&loader, CUSTOM_MYFW_FORMAT_ID) == 0);
    assert(ldr_loader_probe_format(&loader, myfw_hdr, sizeof(myfw_hdr), &hdr_size, &meta) == LDR_FORMAT_NO_MATCH);

    printf("[TEST 1] 格式插件注册表 (EFW + 自定义 MYFW) 自动探测、CRC 拦截与注销... [PASS]\n");
}

static void test_foe_free_unified_loader_bidirectional(void)
{
    ldr_loader_t loader;
    mock_flash_drv_t drv;
    ldr_transport_endpoint_t can_ep;
    ldr_transport_plugin_desc_t ep_desc;

    (void)memset(&drv, 0, sizeof(drv));

    assert(ldr_loader_init(&loader) == 0);
    assert(!ldr_loader_is_busy(&loader));

    ldr_target_desc_t tdesc;
    (void)memset(&tdesc, 0, sizeof(tdesc));
    tdesc.target_id = 30U;
    (void)strcpy(tdesc.name, "PARAM_CAN");
    tdesc.type = LDR_TARGET_TYPE_DATA;
    tdesc.max_size = sizeof(drv.storage);
    tdesc.write_unit = 1U;
    tdesc.permissions = LDR_PERM_RW;
    tdesc.enabled = true;
    tdesc.driver.io.init = drv_init;
    tdesc.driver.io.invalidate = drv_invalidate;
    tdesc.driver.io.erase_to = drv_erase_to;
    tdesc.driver.io.write = drv_write;
    tdesc.driver.io.read = drv_read;
    tdesc.driver.io.finalize = drv_finalize;
    tdesc.driver.io.abort = drv_abort;
    tdesc.driver.read_open = drv_read_open;
    tdesc.driver.read_close = drv_read_close;
    tdesc.driver.get_data_size = drv_get_data_size;
    tdesc.user_ctx = &drv;
    assert(ldr_loader_register_target(&loader, &tdesc) == 0);

    ldr_loader_set_enforce_acl(&loader, true);
    assert(ldr_loader_add_acl(&loader, LDR_TRANSPORT_ID_CAN, 30U, LDR_PERM_RW) == 0);

    assert(ldr_transport_endpoint_init(&can_ep, LDR_TRANSPORT_ID_CAN, "CAN_FD",
                                       ldr_loader_get_transfer(&loader), NULL) == 0);
    assert(ldr_transport_endpoint_make_plugin_desc(&can_ep, LDR_TRANSPORT_CAP_BIDIRECTIONAL, &ep_desc) == 0);
    assert(ldr_loader_register_transport(&loader, &ep_desc) == 0);

    uint8_t tx_data[64];
    uint32_t i;
    for (i = 0U; i < sizeof(tx_data); ++i) {
        tx_data[i] = (uint8_t)(0x50U + i);
    }
    assert(ldr_transport_endpoint_begin_import(&can_ep, 30U, sizeof(tx_data), 0U) == LDR_RES_COMPLETE);
    assert(ldr_loader_is_busy(&loader));
    assert(ldr_loader_get_active_transport(&loader)->transport_id == LDR_TRANSPORT_ID_CAN);
    assert(ldr_transport_endpoint_write_chunk(&can_ep, tx_data, sizeof(tx_data)) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_finish_import(&can_ep) == LDR_RES_COMPLETE);
    assert(!ldr_loader_is_busy(&loader));

    uint32_t export_total = 0U;
    uint8_t rx_data[64];
    size_t actual = 0U;
    assert(ldr_transport_endpoint_begin_export(&can_ep, 30U, &export_total) == LDR_RES_COMPLETE);
    assert(export_total == sizeof(tx_data));
    assert(ldr_transport_endpoint_read_chunk(&can_ep, rx_data, sizeof(rx_data), &actual) == LDR_RES_COMPLETE);
    assert(actual == sizeof(tx_data));
    assert(memcmp(rx_data, tx_data, sizeof(tx_data)) == 0);
    assert(ldr_transport_endpoint_finish_export(&can_ep) == LDR_RES_COMPLETE);
    assert(!ldr_loader_is_busy(&loader));

    assert(ldr_transport_endpoint_begin_export(&can_ep, 30U, &export_total) == LDR_RES_COMPLETE);
    assert(ldr_loader_is_busy(&loader));
    ldr_loader_abort_all(&loader);
    assert(!ldr_loader_is_busy(&loader));

    printf("[TEST 2] 完全去 FoE 化 ldr_loader_t 双向 IMPORT/EXPORT、ACL 与 abort_all... [PASS]\n");
}

static void test_format_driven_firmware_pipeline_multiformat_and_security(void)
{
    ldr_loader_t loader;
    ldr_fw_pipeline_t pipe;
    mock_flash_drv_t fw_drv;
    ldr_transport_endpoint_t can_ep;
    ldr_transport_plugin_desc_t ep_desc;
    ldr_format_plugin_desc_t efw_desc;
    ldr_format_plugin_desc_t myfw_desc;
    uint8_t header_buf[256];
    uint8_t residual_buf[32];
    uint32_t i;

    (void)memset(&fw_drv, 0, sizeof(fw_drv));
    assert(ldr_loader_init(&loader) == 0);

    /* 同时注册内置 EFW v2 与自定义 MYFW 两种格式插件 */
    assert(ldr_format_efw_make_plugin_desc(NULL, &efw_desc) == 0);
    assert(ldr_loader_register_format(&loader, &efw_desc) == 0);
    myfw_desc.format_id = CUSTOM_MYFW_FORMAT_ID;
    myfw_desc.name = "MYFW_V1";
    myfw_desc.ops = &s_myfw_ops;
    myfw_desc.plugin_ctx = NULL;
    assert(ldr_loader_register_format(&loader, &myfw_desc) == 0);

    /* 绑定纯中性格式驱动固件刷写流水线 */
    assert(ldr_fw_pipeline_init(&pipe, &loader, header_buf, sizeof(header_buf),
                                residual_buf, sizeof(residual_buf)) == 0);

    /* 注册 FIRMWARE 目标 (write_unit = 16, 支持双向 IMPORT 刷写与 EXPORT 回读) */
    ldr_target_desc_t fw_target;
    (void)memset(&fw_target, 0, sizeof(fw_target));
    fw_target.target_id = 1U;
    (void)strcpy(fw_target.name, "MCU_APP");
    fw_target.type = LDR_TARGET_TYPE_FIRMWARE;
    fw_target.max_size = sizeof(fw_drv.storage);
    fw_target.write_unit = 16U;
    fw_target.permissions = LDR_PERM_RW;
    fw_target.enabled = true;
    fw_target.driver.io.init = drv_init;
    fw_target.driver.io.invalidate = drv_invalidate;
    fw_target.driver.io.erase_to = drv_erase_to;
    fw_target.driver.io.write = drv_write;
    fw_target.driver.io.read = drv_read;
    fw_target.driver.io.finalize = drv_finalize;
    fw_target.driver.io.abort = drv_abort;
    fw_target.driver.read_open = drv_read_open;
    fw_target.driver.read_close = drv_read_close;
    fw_target.driver.get_data_size = drv_get_data_size;
    fw_target.user_ctx = &fw_drv;
    assert(ldr_loader_register_target(&loader, &fw_target) == 0);

    assert(ldr_transport_endpoint_init(&can_ep, LDR_TRANSPORT_ID_CAN, "CAN_FW",
                                       ldr_loader_get_transfer(&loader), NULL) == 0);
    assert(ldr_transport_endpoint_make_plugin_desc(&can_ep, LDR_TRANSPORT_CAP_BIDIRECTIONAL, &ep_desc) == 0);
    assert(ldr_loader_register_transport(&loader, &ep_desc) == 0);

    /* TEST 3: 自定义 MYFW (20B 头 + 50B 载荷, 非 16B 对齐) 跨随机分片刷写 + EXPORT 回读 */
    uint8_t myfw_img[CUSTOM_MYFW_HDR_LEN + 50U];
    uint8_t myfw_payload[50];
    for (i = 0U; i < sizeof(myfw_payload); ++i) {
        myfw_payload[i] = (uint8_t)(0x11U + i);
    }
    uint32_t myfw_crc = ldr_crc32_calc(myfw_payload, sizeof(myfw_payload));
    build_myfw_header(myfw_img, 1U, 0x02010000U, (uint32_t)sizeof(myfw_payload), myfw_crc);
    (void)memcpy(&myfw_img[CUSTOM_MYFW_HDR_LEN], myfw_payload, sizeof(myfw_payload));

    mock_flash_reset_stats(&fw_drv);
    assert(ldr_transport_endpoint_begin_import(&can_ep, 1U, (uint32_t)sizeof(myfw_img), 0x02010000U) == LDR_RES_COMPLETE);
    /* 故意以非对齐碎片 (7, 13, 19, 31 = 70B) 切分头部与载荷 */
    assert(ldr_transport_endpoint_write_chunk(&can_ep, &myfw_img[0], 7U) == LDR_RES_COMPLETE);
    assert(fw_drv.init_calls == 0U); /* 头部未齐，绝不触碰驱动 */
    assert(ldr_transport_endpoint_write_chunk(&can_ep, &myfw_img[7], 13U) == LDR_RES_COMPLETE);
    assert(fw_drv.init_calls == 1U);
    assert(fw_drv.invalidate_calls == 1U);
    assert(ldr_transport_endpoint_write_chunk(&can_ep, &myfw_img[20], 19U) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_write_chunk(&can_ep, &myfw_img[39], 31U) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_finish_import(&can_ep) == LDR_RES_COMPLETE);
    assert(fw_drv.finalize_calls == 1U);
    assert(fw_drv.abort_calls == 0U);
    assert(ldr_fw_pipeline_get_last_metadata(&pipe)->format_id == CUSTOM_MYFW_FORMAT_ID);
    /* 验证 50B 有效载荷 + 14B 0xFF 尾块填充已写入 */
    assert(memcmp(fw_drv.storage, myfw_payload, sizeof(myfw_payload)) == 0);
    for (i = 50U; i < 64U; ++i) {
        assert(fw_drv.storage[i] == 0xFFU);
    }

    /* 导出刚刷入的固件并逐字节核对前 50B */
    uint32_t exp_sz = 0U;
    uint8_t exp_buf[64];
    size_t exp_actual = 0U;
    assert(ldr_transport_endpoint_begin_export(&can_ep, 1U, &exp_sz) == LDR_RES_COMPLETE);
    assert(exp_sz == 64U);
    assert(ldr_transport_endpoint_read_chunk(&can_ep, exp_buf, sizeof(exp_buf), &exp_actual) == LDR_RES_COMPLETE);
    assert(exp_actual == 64U);
    assert(memcmp(exp_buf, myfw_payload, sizeof(myfw_payload)) == 0);
    assert(ldr_transport_endpoint_finish_export(&can_ep) == LDR_RES_COMPLETE);
    printf("[TEST 3] 格式插件驱动自定义 MYFW 跨片刷写 (0xFF 对齐填充 + 回读核验 + EXPORT)... [PASS]\n");

    /* TEST 4: 标准 EFW v2 (148B 头 + 64B 载荷) 在同一流水线上共存刷写 */
    uint8_t efw_img[ECAT_FOE_EFW_HEADER_MIN_LEN + 64U];
    uint8_t efw_payload[64];
    for (i = 0U; i < sizeof(efw_payload); ++i) {
        efw_payload[i] = (uint8_t)(0x80U + i);
    }
    uint32_t efw_crc = ldr_crc32_calc(efw_payload, sizeof(efw_payload));
    build_valid_efw_header(efw_img, 1U, 0x03000001U, (uint32_t)sizeof(efw_payload), efw_crc);
    (void)memcpy(&efw_img[ECAT_FOE_EFW_HEADER_MIN_LEN], efw_payload, sizeof(efw_payload));

    mock_flash_reset_stats(&fw_drv);
    assert(ldr_transport_endpoint_begin_import(&can_ep, 1U, (uint32_t)sizeof(efw_img), 0U) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_write_chunk(&can_ep, efw_img, sizeof(efw_img)) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_finish_import(&can_ep) == LDR_RES_COMPLETE);
    assert(fw_drv.finalize_calls == 1U);
    assert(fw_drv.abort_calls == 0U);
    assert(ldr_fw_pipeline_get_last_metadata(&pipe)->format_id == LDR_FORMAT_ID_EFW);
    assert(memcmp(fw_drv.storage, efw_payload, sizeof(efw_payload)) == 0);
    printf("[TEST 4] 同一流水线自动切换识别并刷写标准 EFW v2 镜像... [PASS]\n");

    /* TEST 5A: 未知魔数 -> 0 次驱动调用 */
    uint8_t bad_magic[24] = { 'B', 'A', 'D', '!', 1, 2, 3, 4 };
    mock_flash_reset_stats(&fw_drv);
    assert(ldr_transport_endpoint_begin_import(&can_ep, 1U, sizeof(bad_magic), 0U) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_write_chunk(&can_ep, bad_magic, sizeof(bad_magic)) == LDR_RES_FAILED);
    assert(ldr_fw_pipeline_get_last_error(&pipe) == LDR_FW_PIPE_ERR_FORMAT_REJECTED);
    assert(fw_drv.init_calls == 0U);
    assert(fw_drv.abort_calls == 0U);

    /* TEST 5B: 载荷 CRC 篡改 -> 单次 abort 且禁止 finalize */
    myfw_img[CUSTOM_MYFW_HDR_LEN + 5U] ^= 0xFFU;
    mock_flash_reset_stats(&fw_drv);
    assert(ldr_transport_endpoint_begin_import(&can_ep, 1U, (uint32_t)sizeof(myfw_img), 0U) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_write_chunk(&can_ep, myfw_img, sizeof(myfw_img)) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_finish_import(&can_ep) == LDR_RES_FAILED);
    assert(ldr_fw_pipeline_get_last_error(&pipe) == LDR_FW_PIPE_ERR_STREAM_CRC);
    assert(fw_drv.finalize_calls == 0U);
    assert(fw_drv.abort_calls == 1U);
    myfw_img[CUSTOM_MYFW_HDR_LEN + 5U] ^= 0xFFU; /* 恢复 */

    /* TEST 5C: 物理回读位翻转 -> 单次 abort 且禁止 finalize */
    mock_flash_reset_stats(&fw_drv);
    fw_drv.inject_readback_corrupt = true;
    assert(ldr_transport_endpoint_begin_import(&can_ep, 1U, (uint32_t)sizeof(myfw_img), 0U) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_write_chunk(&can_ep, myfw_img, sizeof(myfw_img)) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_finish_import(&can_ep) == LDR_RES_FAILED);
    assert(ldr_fw_pipeline_get_last_error(&pipe) == LDR_FW_PIPE_ERR_READBACK_CRC);
    assert(fw_drv.finalize_calls == 0U);
    assert(fw_drv.abort_calls == 1U);

    /* TEST 5D: 欠收 -> 单次 abort */
    mock_flash_reset_stats(&fw_drv);
    assert(ldr_transport_endpoint_begin_import(&can_ep, 1U, 0U, 0U) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_write_chunk(&can_ep, myfw_img, sizeof(myfw_img) - 4U) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_finish_import(&can_ep) == LDR_RES_FAILED);
    assert(ldr_fw_pipeline_get_last_error(&pipe) == LDR_FW_PIPE_ERR_UNDER_RECEIVE);
    assert(fw_drv.finalize_calls == 0U);
    assert(fw_drv.abort_calls == 1U);

    /* TEST 5E: 超收 -> 单次 abort */
    uint8_t extra_bytes[4] = { 1U, 2U, 3U, 4U };
    mock_flash_reset_stats(&fw_drv);
    assert(ldr_transport_endpoint_begin_import(&can_ep, 1U, 0U, 0U) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_write_chunk(&can_ep, myfw_img, sizeof(myfw_img)) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_write_chunk(&can_ep, extra_bytes, sizeof(extra_bytes)) == LDR_RES_FAILED);
    assert(ldr_fw_pipeline_get_last_error(&pipe) == LDR_FW_PIPE_ERR_OVER_RECEIVE);
    assert(fw_drv.finalize_calls == 0U);
    assert(fw_drv.abort_calls == 1U);

    printf("[TEST 5] 格式驱动刷写流水线安全负向校验 (0 驱动拦截/流 CRC/回读翻转/欠收/超收)... [PASS]\n");
}

/* -------------------------------------------------------------------------- */
/*       TEST 6: 流水线可选流式验签 + 防回滚版本下限原子事务提交验证          */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t running_crc;
    uint32_t expected_sig_tag;
    uint32_t begin_calls;
    uint32_t update_calls;
    uint32_t finish_calls;
    uint32_t abort_calls;
} mock_sig_ctx_t;

static int mock_sig_begin(const uint8_t *key_id, size_t key_id_len,
                          const uint8_t *signature, size_t signature_len, void *user_ctx)
{
    mock_sig_ctx_t *s = (mock_sig_ctx_t *)user_ctx;
    s->begin_calls++;
    if ((key_id == NULL) || (key_id_len != 8U) || (signature == NULL) || (signature_len != 64U)) {
        return -1;
    }
    ldr_crc32_init(&s->running_crc);
    s->expected_sig_tag = read_le32(signature);
    return 0;
}

static int mock_sig_update(const uint8_t *data, size_t len, void *user_ctx)
{
    mock_sig_ctx_t *s = (mock_sig_ctx_t *)user_ctx;
    s->update_calls++;
    ldr_crc32_update(&s->running_crc, data, len);
    return 0;
}

static int mock_sig_finish(void *user_ctx)
{
    mock_sig_ctx_t *s = (mock_sig_ctx_t *)user_ctx;
    s->finish_calls++;
    uint32_t actual = ldr_crc32_final(&s->running_crc);
    return (actual == s->expected_sig_tag) ? 0 : -1;
}

static void mock_sig_abort(void *user_ctx)
{
    mock_sig_ctx_t *s = (mock_sig_ctx_t *)user_ctx;
    s->abort_calls++;
}

typedef struct {
    mock_flash_drv_t *drv;
    bool inject_get_fail;
} mock_floor_ctx_t;

static int mock_get_floor(uint16_t target_id, uint32_t *out_floor, void *user_ctx)
{
    mock_floor_ctx_t *fc = (mock_floor_ctx_t *)user_ctx;
    (void)target_id;
    if ((fc == NULL) || fc->inject_get_fail) {
        return -1;
    }
    *out_floor = fc->drv->persisted_floor;
    return 0;
}

static void build_signed_efw_header(uint8_t out_hdr[ECAT_FOE_EFW_HEADER_MIN_LEN],
                                    uint16_t target_id,
                                    uint32_t fw_version,
                                    uint32_t min_allow_version,
                                    uint32_t payload_size,
                                    uint32_t payload_crc32,
                                    uint32_t sig_tag)
{
    (void)memset(out_hdr, 0, ECAT_FOE_EFW_HEADER_MIN_LEN);
    write_le32(&out_hdr[0x00], ECAT_FOE_EFW_MAGIC);
    write_le16(&out_hdr[0x04], ECAT_FOE_EFW_FORMAT_VERSION);
    write_le16(&out_hdr[0x06], ECAT_FOE_EFW_HEADER_MIN_LEN);
    write_le32(&out_hdr[0x08], 0x10010001U);
    write_le32(&out_hdr[0x0C], 0x00000001U);
    write_le16(&out_hdr[0x10], 1U);
    write_le16(&out_hdr[0x12], 2U);
    write_le16(&out_hdr[0x14], target_id);
    write_le16(&out_hdr[0x16], 0U);
    write_le32(&out_hdr[0x18], fw_version);
    write_le32(&out_hdr[0x1C], min_allow_version);
    write_le32(&out_hdr[0x20], payload_size);
    write_le32(&out_hdr[0x24], payload_crc32);
    out_hdr[60] = (uint8_t)'S';
    out_hdr[61] = (uint8_t)'I';
    out_hdr[62] = (uint8_t)'G';
    out_hdr[63] = (uint8_t)'2';
    write_le16(&out_hdr[64], 1U);
    write_le16(&out_hdr[66], 1U);
    write_le16(&out_hdr[68], ECAT_FOE_EFW_SIGNATURE_LEN);
    write_le16(&out_hdr[70], ECAT_FOE_EFW_KEY_ID_LEN);
    (void)memcpy(&out_hdr[72], "KEYID001", 8U);
    write_le32(&out_hdr[80], sig_tag);
    uint32_t hdr_crc = ecat_foe_crc32_calc(out_hdr, ECAT_FOE_EFW_HEADER_MIN_LEN - 4U);
    write_le32(&out_hdr[ECAT_FOE_EFW_HEADER_MIN_LEN - 4U], hdr_crc);
}

static void test_pipeline_signature_and_anti_rollback(void)
{
    ldr_loader_t loader;
    ldr_fw_pipeline_t pipe;
    mock_flash_drv_t fw_drv;
    mock_sig_ctx_t sig_ctx;
    mock_floor_ctx_t floor_ctx;
    ldr_transport_endpoint_t can_ep;
    ldr_transport_plugin_desc_t ep_desc;
    ldr_format_plugin_desc_t efw_desc;
    uint8_t header_buf[256];
    uint8_t residual_buf[32];
    uint32_t i;

    (void)memset(&fw_drv, 0, sizeof(fw_drv));
    (void)memset(&sig_ctx, 0, sizeof(sig_ctx));
    fw_drv.persisted_floor = 0x02000000U;
    floor_ctx.drv = &fw_drv;
    floor_ctx.inject_get_fail = false;

    assert(ldr_loader_init(&loader) == 0);
    assert(ldr_format_efw_make_plugin_desc(NULL, &efw_desc) == 0);
    assert(ldr_loader_register_format(&loader, &efw_desc) == 0);
    assert(ldr_fw_pipeline_init(&pipe, &loader, header_buf, sizeof(header_buf),
                                residual_buf, sizeof(residual_buf)) == 0);

    ldr_fw_security_cfg_t sec;
    (void)memset(&sec, 0, sizeof(sec));
    sec.require_signature = true;
    sec.verifier.begin = mock_sig_begin;
    sec.verifier.update = mock_sig_update;
    sec.verifier.finish = mock_sig_finish;
    sec.verifier.abort = mock_sig_abort;
    sec.verifier.user_ctx = &sig_ctx;
    sec.enable_anti_rollback = true;
    sec.get_version_floor = mock_get_floor;
    sec.anti_rollback_ctx = &floor_ctx;
    assert(ldr_fw_pipeline_set_security(&pipe, &sec) == 0);

    ldr_target_desc_t fw_target;
    (void)memset(&fw_target, 0, sizeof(fw_target));
    fw_target.target_id = 1U;
    (void)strcpy(fw_target.name, "MCU_SEC");
    fw_target.type = LDR_TARGET_TYPE_FIRMWARE;
    fw_target.max_size = sizeof(fw_drv.storage);
    fw_target.write_unit = 16U;
    fw_target.permissions = LDR_PERM_RW;
    fw_target.enabled = true;
    fw_target.driver.io.init = drv_init;
    fw_target.driver.io.invalidate = drv_invalidate;
    fw_target.driver.io.erase_to = drv_erase_to;
    fw_target.driver.io.write = drv_write;
    fw_target.driver.io.read = drv_read;
    fw_target.driver.io.finalize = drv_finalize;
    fw_target.driver.io.abort = drv_abort;
    fw_target.driver.io.finalize_with_version_floor = drv_finalize_with_floor;
    fw_target.user_ctx = &fw_drv;
    assert(ldr_loader_register_target(&loader, &fw_target) == 0);

    assert(ldr_transport_endpoint_init(&can_ep, LDR_TRANSPORT_ID_CAN, "CAN_SEC",
                                       ldr_loader_get_transfer(&loader), NULL) == 0);
    assert(ldr_transport_endpoint_make_plugin_desc(&can_ep, LDR_TRANSPORT_CAP_BIDIRECTIONAL, &ep_desc) == 0);
    assert(ldr_loader_register_transport(&loader, &ep_desc) == 0);

    uint8_t payload[48];
    for (i = 0U; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)(0x40U + i);
    }
    uint32_t payload_crc = ldr_crc32_calc(payload, sizeof(payload));
    uint8_t img[ECAT_FOE_EFW_HEADER_MIN_LEN + sizeof(payload)];
    (void)memcpy(&img[ECAT_FOE_EFW_HEADER_MIN_LEN], payload, sizeof(payload));

    /* 6A: 未签名镜像在强制验签模式下被拦截 (0 驱动调用) */
    build_valid_efw_header(img, 1U, 0x02010000U, (uint32_t)sizeof(payload), payload_crc);
    mock_flash_reset_stats(&fw_drv);
    assert(ldr_transport_endpoint_begin_import(&can_ep, 1U, (uint32_t)sizeof(img), 0U) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_write_chunk(&can_ep, img, sizeof(img)) == LDR_RES_FAILED);
    assert(ldr_fw_pipeline_get_last_error(&pipe) == LDR_FW_PIPE_ERR_SIGNATURE_REQUIRED);
    assert(fw_drv.init_calls == 0U);
    assert(fw_drv.abort_calls == 0U);

    /* 6B: 版本低于防回滚下限 (0x01090000 < 0x02000000) -> 0 驱动调用 */
    build_signed_efw_header(img, 1U, 0x01090000U, 0x01090000U, (uint32_t)sizeof(payload), payload_crc, payload_crc);
    mock_flash_reset_stats(&fw_drv);
    assert(ldr_transport_endpoint_begin_import(&can_ep, 1U, (uint32_t)sizeof(img), 0U) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_write_chunk(&can_ep, img, sizeof(img)) == LDR_RES_FAILED);
    assert(ldr_fw_pipeline_get_last_error(&pipe) == LDR_FW_PIPE_ERR_ANTI_ROLLBACK_REJECTED);
    assert(fw_drv.init_calls == 0U);

    /* 6C: 防回滚下限查询失败 -> 0 驱动调用 */
    floor_ctx.inject_get_fail = true;
    build_signed_efw_header(img, 1U, 0x02050000U, 0x02020000U, (uint32_t)sizeof(payload), payload_crc, payload_crc);
    mock_flash_reset_stats(&fw_drv);
    assert(ldr_transport_endpoint_begin_import(&can_ep, 1U, (uint32_t)sizeof(img), 0U) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_write_chunk(&can_ep, img, sizeof(img)) == LDR_RES_FAILED);
    assert(ldr_fw_pipeline_get_last_error(&pipe) == LDR_FW_PIPE_ERR_ANTI_ROLLBACK_GET);
    assert(fw_drv.init_calls == 0U);
    floor_ctx.inject_get_fail = false;

    /* 6D: 签名无效 (sig_tag 错误) -> finish 阶段拒绝, 单次 abort, 禁止提交 */
    build_signed_efw_header(img, 1U, 0x02050000U, 0x02020000U, (uint32_t)sizeof(payload), payload_crc, payload_crc ^ 0xDEADU);
    mock_flash_reset_stats(&fw_drv);
    assert(ldr_transport_endpoint_begin_import(&can_ep, 1U, (uint32_t)sizeof(img), 0U) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_write_chunk(&can_ep, img, sizeof(img)) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_finish_import(&can_ep) == LDR_RES_FAILED);
    assert(ldr_fw_pipeline_get_last_error(&pipe) == LDR_FW_PIPE_ERR_SIGNATURE_INVALID);
    assert(fw_drv.finalize_calls == 0U);
    assert(fw_drv.finalize_floor_calls == 0U);
    assert(fw_drv.abort_calls == 1U);
    assert(fw_drv.persisted_floor == 0x02000000U);

    /* 6E: 合法签名 + 防回滚原子事务提交成功并抬升版本下限 */
    build_signed_efw_header(img, 1U, 0x02050000U, 0x02020000U, (uint32_t)sizeof(payload), payload_crc, payload_crc);
    mock_flash_reset_stats(&fw_drv);
    assert(ldr_transport_endpoint_begin_import(&can_ep, 1U, (uint32_t)sizeof(img), 0U) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_write_chunk(&can_ep, img, sizeof(img)) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_finish_import(&can_ep) == LDR_RES_COMPLETE);
    assert(fw_drv.finalize_calls == 0U);
    assert(fw_drv.finalize_floor_calls == 1U);
    assert(fw_drv.abort_calls == 0U);
    assert(fw_drv.persisted_floor == 0x02020000U);
    assert(memcmp(fw_drv.storage, payload, sizeof(payload)) == 0);

    /* 6F: 原子提交失败 -> 单次 abort 且不抬升版本下限 */
    build_signed_efw_header(img, 1U, 0x03000000U, 0x02090000U, (uint32_t)sizeof(payload), payload_crc, payload_crc);
    mock_flash_reset_stats(&fw_drv);
    fw_drv.inject_floor_commit_fail = true;
    assert(ldr_transport_endpoint_begin_import(&can_ep, 1U, (uint32_t)sizeof(img), 0U) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_write_chunk(&can_ep, img, sizeof(img)) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_finish_import(&can_ep) == LDR_RES_FAILED);
    assert(ldr_fw_pipeline_get_last_error(&pipe) == LDR_FW_PIPE_ERR_ANTI_ROLLBACK_COMMIT);
    assert(fw_drv.abort_calls == 1U);
    assert(fw_drv.persisted_floor == 0x02020000U);

    printf("[TEST 6] 流水线可选流式验签与防回滚版本下限原子事务提交 (6 项安全路径)... [PASS]\n");
}

/* -------------------------------------------------------------------------- */
/*          TEST 7: 非阻塞异步固件刷写状态机与异步导出回读闭环验证            */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t storage[512];
    uint32_t data_size;
    uint32_t erased_end;
    uint32_t pending_ticks;
    uint32_t current_ticks;
    uint32_t init_calls;
    uint32_t invalidate_calls;
    uint32_t erase_calls;
    uint32_t write_calls;
    uint32_t read_calls;
    uint32_t finalize_calls;
    uint32_t read_open_calls;
    uint32_t read_close_calls;
    uint32_t abort_calls;
    bool inject_readback_corrupt;
} mock_async_flash_drv_t;

static void mock_async_reset_stats(mock_async_flash_drv_t *d)
{
    d->current_ticks = 0U;
    d->init_calls = 0U;
    d->invalidate_calls = 0U;
    d->erase_calls = 0U;
    d->write_calls = 0U;
    d->read_calls = 0U;
    d->finalize_calls = 0U;
    d->read_open_calls = 0U;
    d->read_close_calls = 0U;
    d->abort_calls = 0U;
    d->inject_readback_corrupt = false;
}

static ecat_async_op_result_t mock_async_exec_op(mock_async_flash_drv_t *d, ecat_async_data_op_t *op)
{
    switch (op->kind) {
    case ECAT_ASYNC_DATA_OP_INIT:
        d->init_calls++;
        d->erased_end = 0U;
        d->data_size = 0U;
        return ECAT_ASYNC_OP_COMPLETE;
    case ECAT_ASYNC_DATA_OP_INVALIDATE:
        d->invalidate_calls++;
        return ECAT_ASYNC_OP_COMPLETE;
    case ECAT_ASYNC_DATA_OP_ERASE_TO:
        d->erase_calls++;
        if (op->need_end > sizeof(d->storage)) {
            return ECAT_ASYNC_OP_FAILED;
        }
        if (op->need_end > d->erased_end) {
            (void)memset(&d->storage[d->erased_end], 0xFF, op->need_end - d->erased_end);
            d->erased_end = op->need_end;
        }
        op->erased_end = d->erased_end;
        return ECAT_ASYNC_OP_COMPLETE;
    case ECAT_ASYNC_DATA_OP_WRITE:
        d->write_calls++;
        if ((op->offset + op->length) > sizeof(d->storage)) {
            return ECAT_ASYNC_OP_FAILED;
        }
        (void)memcpy(&d->storage[op->offset], op->buffer, op->length);
        if ((op->offset + op->length) > d->data_size) {
            d->data_size = op->offset + op->length;
        }
        op->actual_length = op->length;
        return ECAT_ASYNC_OP_COMPLETE;
    case ECAT_ASYNC_DATA_OP_READ:
        d->read_calls++;
        if ((op->offset + op->length) > sizeof(d->storage)) {
            return ECAT_ASYNC_OP_FAILED;
        }
        (void)memcpy(op->buffer, &d->storage[op->offset], op->length);
        if (d->inject_readback_corrupt && (op->offset == 0U) && (op->length > 0U)) {
            op->buffer[0] ^= 0x01U;
        }
        op->actual_length = op->length;
        return ECAT_ASYNC_OP_COMPLETE;
    case ECAT_ASYNC_DATA_OP_FINALIZE:
        d->finalize_calls++;
        return ECAT_ASYNC_OP_COMPLETE;
    case ECAT_ASYNC_DATA_OP_READ_OPEN:
        d->read_open_calls++;
        return ECAT_ASYNC_OP_COMPLETE;
    case ECAT_ASYNC_DATA_OP_READ_CLOSE:
        d->read_close_calls++;
        return ECAT_ASYNC_OP_COMPLETE;
    default:
        return ECAT_ASYNC_OP_FAILED;
    }
}

static ecat_async_op_result_t mock_async_start(void *ctx, ecat_async_data_op_t *op)
{
    mock_async_flash_drv_t *d = (mock_async_flash_drv_t *)ctx;
    if ((d == NULL) || (op == NULL)) {
        return ECAT_ASYNC_OP_FAILED;
    }
    d->current_ticks = 0U;
    if (d->pending_ticks == 0U) {
        return mock_async_exec_op(d, op);
    }
    return ECAT_ASYNC_OP_PENDING;
}

static ecat_async_op_result_t mock_async_poll(void *ctx, ecat_async_data_op_t *op)
{
    mock_async_flash_drv_t *d = (mock_async_flash_drv_t *)ctx;
    if ((d == NULL) || (op == NULL)) {
        return ECAT_ASYNC_OP_FAILED;
    }
    d->current_ticks++;
    if (d->current_ticks < d->pending_ticks) {
        return ECAT_ASYNC_OP_PENDING;
    }
    return mock_async_exec_op(d, op);
}

static void mock_async_abort(void *ctx)
{
    mock_async_flash_drv_t *d = (mock_async_flash_drv_t *)ctx;
    if (d != NULL) {
        d->abort_calls++;
    }
}

static uint32_t mock_async_get_data_size(void *ctx)
{
    const mock_async_flash_drv_t *d = (const mock_async_flash_drv_t *)ctx;
    return (d != NULL) ? d->data_size : 0U;
}

static const ecat_async_data_ops_t s_mock_async_ops = {
    mock_async_start,
    mock_async_poll,
    mock_async_abort
};

static ldr_transfer_result_t poll_loader_until_done(ldr_loader_t *loader, uint32_t max_iters)
{
    ldr_transfer_result_t res = LDR_RES_PENDING;
    uint32_t iter;
    for (iter = 0U; iter < max_iters; ++iter) {
        uint16_t polled_id = 0U;
        res = ldr_loader_poll(loader, &polled_id);
        if (res != LDR_RES_PENDING) {
            return res;
        }
    }
    assert(0 && "async loader poll exceeded max_iters");
    return LDR_RES_FAILED;
}

static void test_async_firmware_pipeline(void)
{
    ldr_loader_t loader;
    ldr_fw_pipeline_t pipe;
    mock_async_flash_drv_t adrv;
    ldr_transport_endpoint_t uart_ep;
    ldr_transport_plugin_desc_t ep_desc;
    ldr_format_plugin_desc_t myfw_desc;
    uint8_t header_buf[256];
    uint8_t residual_buf[32];
    uint8_t staging_buf[128];
    uint32_t i;

    (void)memset(&adrv, 0, sizeof(adrv));
    adrv.pending_ticks = 2U;

    assert(ldr_loader_init(&loader) == 0);
    assert(ldr_loader_set_staging_buffer(&loader, staging_buf, sizeof(staging_buf)) == 0);

    myfw_desc.format_id = CUSTOM_MYFW_FORMAT_ID;
    myfw_desc.name = "MYFW_V1";
    myfw_desc.ops = &s_myfw_ops;
    myfw_desc.plugin_ctx = NULL;
    assert(ldr_loader_register_format(&loader, &myfw_desc) == 0);

    assert(ldr_fw_pipeline_init(&pipe, &loader, header_buf, sizeof(header_buf),
                                residual_buf, sizeof(residual_buf)) == 0);

    ldr_target_desc_t fw_target;
    (void)memset(&fw_target, 0, sizeof(fw_target));
    fw_target.target_id = 2U;
    (void)strcpy(fw_target.name, "MCU_ASYNC");
    fw_target.type = LDR_TARGET_TYPE_FIRMWARE;
    fw_target.max_size = sizeof(adrv.storage);
    fw_target.write_unit = 16U;
    fw_target.permissions = LDR_PERM_RW;
    fw_target.enabled = true;
    fw_target.asynchronous = true;
    fw_target.driver.async_ops = &s_mock_async_ops;
    fw_target.driver.get_data_size = mock_async_get_data_size;
    fw_target.user_ctx = &adrv;
    assert(ldr_loader_register_target(&loader, &fw_target) == 0);

    assert(ldr_transport_endpoint_init(&uart_ep, LDR_TRANSPORT_ID_UART, "UART_ASYNC",
                                       ldr_loader_get_transfer(&loader), NULL) == 0);
    assert(ldr_transport_endpoint_make_plugin_desc(
               &uart_ep, LDR_TRANSPORT_CAP_BIDIRECTIONAL | LDR_TRANSPORT_CAP_ASYNC, &ep_desc) == 0);
    assert(ldr_loader_register_transport(&loader, &ep_desc) == 0);

    /* 构造 20B MYFW 头 + 50B 载荷 (非 16B 对齐尾块) */
    uint8_t myfw_img[CUSTOM_MYFW_HDR_LEN + 50U];
    uint8_t myfw_payload[50];
    for (i = 0U; i < sizeof(myfw_payload); ++i) {
        myfw_payload[i] = (uint8_t)(0xA0U + i);
    }
    uint32_t myfw_crc = ldr_crc32_calc(myfw_payload, sizeof(myfw_payload));
    build_myfw_header(myfw_img, 2U, 0x04010000U, (uint32_t)sizeof(myfw_payload), myfw_crc);
    (void)memcpy(&myfw_img[CUSTOM_MYFW_HDR_LEN], myfw_payload, sizeof(myfw_payload));

    /* 7A: 多分片非阻塞异步固件刷写 + 异步 EXPORT 回读核验 */
    mock_async_reset_stats(&adrv);
    assert(ldr_transport_endpoint_begin_import(&uart_ep, 2U, (uint32_t)sizeof(myfw_img), 0x04010000U) == LDR_RES_COMPLETE);

    /* 分片 1: 10B (头部未齐 -> 立即 COMPLETE，不触碰异步驱动) */
    assert(ldr_transport_endpoint_write_chunk(&uart_ep, &myfw_img[0], 10U) == LDR_RES_COMPLETE);
    assert(adrv.init_calls == 0U);

    /* 分片 2: 26B (凑齐 20B 头部 + 16B 首块载荷 -> 触发异步 INIT/INVALIDATE/ERASE/WRITE) */
    assert(ldr_transport_endpoint_write_chunk(&uart_ep, &myfw_img[10], 26U) == LDR_RES_PENDING);
    assert(poll_loader_until_done(&loader, 200U) == LDR_RES_COMPLETE);
    assert(adrv.init_calls == 1U);
    assert(adrv.invalidate_calls == 1U);
    assert(adrv.write_calls >= 1U);

    /* 分片 3: 剩余 34B 载荷 -> 异步写入 32B，保留 2B 尾块 */
    assert(ldr_transport_endpoint_write_chunk(&uart_ep, &myfw_img[36], 34U) == LDR_RES_PENDING);
    assert(poll_loader_until_done(&loader, 200U) == LDR_RES_COMPLETE);

    /* 异步终结: 0xFF 填充尾块写入 + 异步回读 CRC 核验 + 异步 FINALIZE */
    assert(ldr_transport_endpoint_finish_import(&uart_ep) == LDR_RES_PENDING);
    assert(poll_loader_until_done(&loader, 200U) == LDR_RES_COMPLETE);
    assert(adrv.finalize_calls == 1U);
    assert(adrv.abort_calls == 0U);
    assert(adrv.read_calls >= 1U);
    assert(!ldr_loader_is_busy(&loader));
    assert(memcmp(adrv.storage, myfw_payload, sizeof(myfw_payload)) == 0);
    for (i = 50U; i < 64U; ++i) {
        assert(adrv.storage[i] == 0xFFU);
    }

    /* 异步 EXPORT 回读刚刷入的固件 */
    uint32_t exp_sz = 0U;
    uint8_t exp_buf[64];
    size_t exp_actual = 0U;
    assert(ldr_transport_endpoint_begin_export(&uart_ep, 2U, &exp_sz) == LDR_RES_PENDING);
    assert(poll_loader_until_done(&loader, 200U) == LDR_RES_COMPLETE);
    assert(ecat_transport_endpoint_get_total_export_size(&uart_ep) == 64U);
    assert(ldr_transport_endpoint_read_chunk(&uart_ep, exp_buf, sizeof(exp_buf), &exp_actual) == LDR_RES_PENDING);
    assert(poll_loader_until_done(&loader, 200U) == LDR_RES_COMPLETE);
    assert(exp_actual == 64U);
    assert(memcmp(exp_buf, myfw_payload, sizeof(myfw_payload)) == 0);
    assert(ldr_transport_endpoint_finish_export(&uart_ep) == LDR_RES_PENDING);
    assert(poll_loader_until_done(&loader, 200U) == LDR_RES_COMPLETE);
    assert(!ldr_loader_is_busy(&loader));

    /* 7B: 异步写入中途触发 ldr_loader_abort_all() -> 单次 abort 且干净释放锁 */
    mock_async_reset_stats(&adrv);
    assert(ldr_transport_endpoint_begin_import(&uart_ep, 2U, (uint32_t)sizeof(myfw_img), 0U) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_write_chunk(&uart_ep, myfw_img, 40U) == LDR_RES_PENDING);
    assert(ldr_loader_is_busy(&loader));
    ldr_loader_abort_all(&loader);
    assert(!ldr_loader_is_busy(&loader));
    assert(adrv.abort_calls == 1U);

    /* 7C: 异步物理回读位翻转 -> LDR_FW_PIPE_ERR_READBACK_CRC, finalize == 0, abort == 1 */
    mock_async_reset_stats(&adrv);
    adrv.inject_readback_corrupt = true;
    assert(ldr_transport_endpoint_begin_import(&uart_ep, 2U, (uint32_t)sizeof(myfw_img), 0U) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_write_chunk(&uart_ep, myfw_img, sizeof(myfw_img)) == LDR_RES_PENDING);
    assert(poll_loader_until_done(&loader, 200U) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_finish_import(&uart_ep) == LDR_RES_PENDING);
    assert(poll_loader_until_done(&loader, 200U) == LDR_RES_FAILED);
    assert(ldr_fw_pipeline_get_last_error(&pipe) == LDR_FW_PIPE_ERR_READBACK_CRC);
    assert(adrv.finalize_calls == 0U);
    assert(adrv.abort_calls == 1U);
    assert(!ldr_loader_is_busy(&loader));

    printf("[TEST 7] 非阻塞异步固件刷写状态机 (跨片异步刷写/回读/EXPORT/中途 abort/翻转拦截)... [PASS]\n");
}

int main(void)
{
    printf("=== 运行统一中性 ldr_ Loader 框架单元测试 ===\n");
    test_format_plugin_registry_and_autodetect();
    test_foe_free_unified_loader_bidirectional();
    test_format_driven_firmware_pipeline_multiformat_and_security();
    test_pipeline_signature_and_anti_rollback();
    test_async_firmware_pipeline();
    printf(">>> 统一中性 ldr_ Loader 框架单元测试全部通过！<<<\n");
    return 0;
}
