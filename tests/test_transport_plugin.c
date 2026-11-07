/**
 * @file test_transport_plugin.c
 * @brief 统一传输通道插件抽象与多通道注册/轮询中心单元测试
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ecat_foe_core.h"
#include "ecat_foe_transport.h"
#include "ecat_target_registry.h"
#include "ecat_transfer.h"
#include "ecat_transfer_file.h"
#include "ecat_transport_endpoint.h"
#include "ecat_transport_plugin.h"

int port_get_version_floor(uint16_t target_id, uint32_t *out_floor)
{
    (void)target_id;
    if (out_floor != NULL) {
        *out_floor = 0U;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/*                      测试用内存目标驱动 (同步与异步)                       */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t storage[512];
    uint32_t data_size;
    uint32_t erased_end;
    uint32_t pending_ticks;
    uint32_t ticks_left;
    uint32_t read_open_calls;
    uint32_t read_close_calls;
    uint32_t abort_calls;
    bool has_op;
} mem_data_drv_t;

static int mem_sync_init(void *ctx)
{
    mem_data_drv_t *drv = (mem_data_drv_t *)ctx;
    if (drv != NULL) {
        drv->erased_end = 0U;
        drv->data_size = 0U;
    }
    return 0;
}

static int mem_sync_invalidate(void *ctx)
{
    (void)ctx;
    return 0;
}

static int mem_sync_erase_to(uint32_t need_end, uint32_t *erased_end, void *ctx)
{
    mem_data_drv_t *drv = (mem_data_drv_t *)ctx;
    if ((drv == NULL) || (erased_end == NULL) || (need_end > sizeof(drv->storage))) {
        return -1;
    }
    if (need_end > drv->erased_end) {
        (void)memset(&drv->storage[drv->erased_end], 0xFF, need_end - drv->erased_end);
        drv->erased_end = need_end;
    }
    *erased_end = drv->erased_end;
    return 0;
}

static int mem_sync_write(uint32_t offset, const uint8_t *data, uint32_t len, void *ctx)
{
    mem_data_drv_t *drv = (mem_data_drv_t *)ctx;
    if ((drv == NULL) || ((offset + len) > sizeof(drv->storage))) {
        return -1;
    }
    (void)memcpy(&drv->storage[offset], data, len);
    if ((offset + len) > drv->data_size) {
        drv->data_size = offset + len;
    }
    return 0;
}

static int mem_sync_read(uint32_t offset, uint8_t *buf, uint32_t len, void *ctx)
{
    mem_data_drv_t *drv = (mem_data_drv_t *)ctx;
    if ((drv == NULL) || ((offset + len) > sizeof(drv->storage))) {
        return -1;
    }
    (void)memcpy(buf, &drv->storage[offset], len);
    return 0;
}

static int mem_sync_finalize(void *ctx)
{
    (void)ctx;
    return 0;
}

static void mem_sync_abort(void *ctx)
{
    mem_data_drv_t *drv = (mem_data_drv_t *)ctx;
    if (drv != NULL) {
        drv->abort_calls++;
    }
}

static int mem_sync_read_open(void *ctx)
{
    mem_data_drv_t *drv = (mem_data_drv_t *)ctx;
    if (drv != NULL) {
        drv->read_open_calls++;
    }
    return 0;
}

static void mem_sync_read_close(void *ctx)
{
    mem_data_drv_t *drv = (mem_data_drv_t *)ctx;
    if (drv != NULL) {
        drv->read_close_calls++;
    }
}

static uint32_t mem_sync_get_data_size(void *ctx)
{
    const mem_data_drv_t *drv = (const mem_data_drv_t *)ctx;
    return (drv != NULL) ? drv->data_size : 0U;
}

static ecat_async_op_result_t mem_async_execute(mem_data_drv_t *drv, ecat_async_data_op_t *op)
{
    if ((drv == NULL) || (op == NULL)) {
        return ECAT_ASYNC_OP_FAILED;
    }
    if ((op->kind == ECAT_ASYNC_DATA_OP_INIT) ||
        (op->kind == ECAT_ASYNC_DATA_OP_INVALIDATE) ||
        (op->kind == ECAT_ASYNC_DATA_OP_FINALIZE)) {
        return ECAT_ASYNC_OP_COMPLETE;
    }
    if (op->kind == ECAT_ASYNC_DATA_OP_ERASE_TO) {
        if (op->need_end > sizeof(drv->storage)) {
            return ECAT_ASYNC_OP_FAILED;
        }
        op->erased_end = op->need_end;
        return ECAT_ASYNC_OP_COMPLETE;
    }
    if (op->kind == ECAT_ASYNC_DATA_OP_READ_OPEN) {
        drv->read_open_calls++;
        return ECAT_ASYNC_OP_COMPLETE;
    }
    if (op->kind == ECAT_ASYNC_DATA_OP_READ_CLOSE) {
        drv->read_close_calls++;
        return ECAT_ASYNC_OP_COMPLETE;
    }
    if (op->kind == ECAT_ASYNC_DATA_OP_WRITE) {
        if ((op->offset + op->length) > sizeof(drv->storage)) {
            return ECAT_ASYNC_OP_FAILED;
        }
        (void)memcpy(&drv->storage[op->offset], op->buffer, op->length);
        op->actual_length = op->length;
        if ((op->offset + op->length) > drv->data_size) {
            drv->data_size = op->offset + op->length;
        }
        return ECAT_ASYNC_OP_COMPLETE;
    }
    if (op->kind == ECAT_ASYNC_DATA_OP_READ) {
        if ((op->offset + op->length) > sizeof(drv->storage)) {
            return ECAT_ASYNC_OP_FAILED;
        }
        (void)memcpy(op->buffer, &drv->storage[op->offset], op->length);
        op->actual_length = op->length;
        return ECAT_ASYNC_OP_COMPLETE;
    }
    return ECAT_ASYNC_OP_FAILED;
}

static ecat_async_op_result_t mem_async_start(void *ctx, ecat_async_data_op_t *op)
{
    mem_data_drv_t *drv = (mem_data_drv_t *)ctx;
    drv->has_op = true;
    drv->ticks_left = drv->pending_ticks;
    if (drv->ticks_left > 0U) {
        return ECAT_ASYNC_OP_PENDING;
    }
    drv->has_op = false;
    return mem_async_execute(drv, op);
}

static ecat_async_op_result_t mem_async_poll(void *ctx, ecat_async_data_op_t *op)
{
    mem_data_drv_t *drv = (mem_data_drv_t *)ctx;
    if (!drv->has_op) {
        return ECAT_ASYNC_OP_FAILED;
    }
    if (drv->ticks_left > 0U) {
        drv->ticks_left--;
        if (drv->ticks_left > 0U) {
            return ECAT_ASYNC_OP_PENDING;
        }
    }
    drv->has_op = false;
    return mem_async_execute(drv, op);
}

static void mem_async_abort(void *ctx)
{
    mem_data_drv_t *drv = (mem_data_drv_t *)ctx;
    if (drv != NULL) {
        drv->has_op = false;
        drv->ticks_left = 0U;
        drv->abort_calls++;
    }
}

static const ecat_async_data_ops_t s_mem_async_ops = {
    mem_async_start,
    mem_async_poll,
    mem_async_abort
};

/* -------------------------------------------------------------------------- */
/*                     Test 1: 注册表校验、容量与查询                         */
/* -------------------------------------------------------------------------- */

static bool dummy_is_active(const void *ctx)
{
    const bool *flag = (const bool *)ctx;
    return (flag != NULL) && (*flag);
}

static const ecat_transport_plugin_ops_t s_dummy_ops = {
    NULL,
    NULL,
    dummy_is_active,
    NULL
};

static void test_registry_validation_and_capacity(void)
{
    ecat_transport_registry_t reg;
    bool flags[ECAT_TRANSPORT_REGISTRY_MAX_PLUGINS + 1U];
    char names[ECAT_TRANSPORT_REGISTRY_MAX_PLUGINS + 1U][16];
    uint32_t i;

    (void)memset(flags, 0, sizeof(flags));
    assert(ecat_transport_registry_init(NULL, NULL) == -1);
    assert(ecat_transport_registry_init(&reg, NULL) == 0);
    assert(ecat_transport_registry_count(&reg) == 0U);
    assert(ecat_transport_registry_get_active(&reg) == NULL);

    /* 非法参数与非法能力掩码拦截 */
    ecat_transport_plugin_desc_t bad_desc;
    (void)memset(&bad_desc, 0, sizeof(bad_desc));
    assert(ecat_transport_registry_register(&reg, NULL) == -1);
    bad_desc.transport_id = 10U;
    bad_desc.name = "BAD_CAPS";
    bad_desc.capabilities = 0UL;
    bad_desc.ops = &s_dummy_ops;
    bad_desc.plugin_ctx = &flags[0];
    assert(ecat_transport_registry_register(&reg, &bad_desc) == -1);

    bad_desc.capabilities = ECAT_TRANSPORT_CAP_ASYNC; /* 缺少 IMPORT/EXPORT */
    assert(ecat_transport_registry_register(&reg, &bad_desc) == -1);

    bad_desc.capabilities = ECAT_TRANSPORT_CAP_IMPORT | (1UL << 31U); /* 未知能力位 */
    assert(ecat_transport_registry_register(&reg, &bad_desc) == -1);

    bad_desc.capabilities = ECAT_TRANSPORT_CAP_BIDIRECTIONAL;
    bad_desc.name = "THIS_NAME_IS_TOO_LONG_FOR_SLOT";
    assert(ecat_transport_registry_register(&reg, &bad_desc) == -1);

    /* 填满所有槽位并校验重复 ID / 重复名称 / 容量上限 */
    for (i = 0U; i < ECAT_TRANSPORT_REGISTRY_MAX_PLUGINS; ++i) {
        (void)snprintf(names[i], sizeof(names[i]), "CH_%u", (unsigned)i);
        ecat_transport_plugin_desc_t d;
        d.transport_id = (uint16_t)(100U + i);
        d.name = names[i];
        d.capabilities = ECAT_TRANSPORT_CAP_BIDIRECTIONAL;
        d.ops = &s_dummy_ops;
        d.plugin_ctx = &flags[i];
        assert(ecat_transport_registry_register(&reg, &d) == 0);
    }
    assert(ecat_transport_registry_count(&reg) == (uint8_t)ECAT_TRANSPORT_REGISTRY_MAX_PLUGINS);

    /* 超出容量拒绝 */
    ecat_transport_plugin_desc_t extra = {
        200U, "EXTRA", ECAT_TRANSPORT_CAP_BIDIRECTIONAL, &s_dummy_ops,
        &flags[ECAT_TRANSPORT_REGISTRY_MAX_PLUGINS]
    };
    assert(ecat_transport_registry_register(&reg, &extra) == -1);

    /* 活跃插件禁止注销，空闲后允许注销并腾出槽位 */
    flags[0] = true;
    assert(ecat_transport_registry_get_active(&reg) != NULL);
    assert(ecat_transport_registry_get_active(&reg)->transport_id == 100U);
    assert(ecat_transport_registry_unregister(&reg, 100U) == -1);

    flags[0] = false;
    assert(ecat_transport_registry_unregister(&reg, 100U) == 0);
    assert(ecat_transport_registry_count(&reg) == (uint8_t)(ECAT_TRANSPORT_REGISTRY_MAX_PLUGINS - 1U));
    assert(ecat_transport_registry_find(&reg, 100U) == NULL);

    /* 重复 ID 与重复名称拒绝 */
    extra.transport_id = 101U;
    extra.name = "UNIQUE_NAME";
    assert(ecat_transport_registry_register(&reg, &extra) == -1);
    extra.transport_id = 200U;
    extra.name = "CH_1";
    assert(ecat_transport_registry_register(&reg, &extra) == -1);

    /* 重新注册成功 */
    extra.name = "UART_CUSTOM";
    assert(ecat_transport_registry_register(&reg, &extra) == 0);
    assert(ecat_transport_registry_find_by_name(&reg, "UART_CUSTOM") != NULL);
    assert(ecat_transport_registry_supports(&reg, 200U, ECAT_TRANSPORT_CAP_BIDIRECTIONAL));
    assert(!ecat_transport_registry_supports(&reg, 200U, ECAT_TRANSPORT_CAP_ASYNC));

    printf("[TEST 1] 注册表参数校验、能力掩码、防活跃注销与容量上限... [PASS]\n");
}

/* -------------------------------------------------------------------------- */
/*       Test 2: 多插件同台注册 (FoE + File + 自定义异步 Endpoint) 与轮询     */
/* -------------------------------------------------------------------------- */

static void test_multi_plugin_bidirectional_and_poll(void)
{
    ecat_target_registry_t target_reg;
    ecat_foe_core_t core;
    ecat_transfer_t transfer;
    ecat_transport_registry_t transport_reg;

    static mem_data_drv_t sync_drv;
    static mem_data_drv_t async_drv;
    static uint8_t staging_buf[128];
    static uint8_t file_buf[32];

    (void)memset(&sync_drv, 0, sizeof(sync_drv));
    (void)memset(&async_drv, 0, sizeof(async_drv));
    async_drv.pending_ticks = 2U;

    assert(ecat_target_registry_init(&target_reg) == 0);

    /* 注册同步 DATA 目标 (ID=10, 供 FoE 与 File 双向导入导出) */
    ecat_target_desc_t sync_target;
    (void)memset(&sync_target, 0, sizeof(sync_target));
    sync_target.target_id = 10U;
    (void)strcpy(sync_target.name, "CFG_SYNC");
    sync_target.type = ECAT_TARGET_TYPE_DATA;
    sync_target.max_size = sizeof(sync_drv.storage);
    sync_target.write_unit = 1U;
    sync_target.permissions = ECAT_PERM_RW;
    sync_target.enabled = true;
    sync_target.asynchronous = false;
    sync_target.driver.io.init = mem_sync_init;
    sync_target.driver.io.invalidate = mem_sync_invalidate;
    sync_target.driver.io.erase_to = mem_sync_erase_to;
    sync_target.driver.io.write = mem_sync_write;
    sync_target.driver.io.read = mem_sync_read;
    sync_target.driver.io.finalize = mem_sync_finalize;
    sync_target.driver.io.abort = mem_sync_abort;
    sync_target.driver.read_open = mem_sync_read_open;
    sync_target.driver.read_close = mem_sync_read_close;
    sync_target.driver.get_data_size = mem_sync_get_data_size;
    sync_target.user_ctx = &sync_drv;
    assert(ecat_target_registry_add(&target_reg, &sync_target) == 0);

    /* 注册异步 DATA 目标 (ID=11, 供自定义 UART Endpoint 异步双向导入导出) */
    ecat_target_desc_t async_target;
    (void)memset(&async_target, 0, sizeof(async_target));
    async_target.target_id = 11U;
    (void)strcpy(async_target.name, "LOG_ASYNC");
    async_target.type = ECAT_TARGET_TYPE_DATA;
    async_target.max_size = sizeof(async_drv.storage);
    async_target.write_unit = 1U;
    async_target.permissions = ECAT_PERM_RW;
    async_target.enabled = true;
    async_target.asynchronous = true;
    async_target.driver.get_data_size = mem_sync_get_data_size;
    async_target.driver.async_ops = &s_mem_async_ops;
    async_target.user_ctx = &async_drv;
    assert(ecat_target_registry_add(&target_reg, &async_target) == 0);

    /* 初始化最小 core + transfer + transport_registry */
    ecat_foe_core_cfg_t cfg;
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.project_id = 0x10010001U;
    cfg.project_name = "LOADER_DEV";
    cfg.hardware_id = 1U;
    cfg.local_hw_ver = 1U;
    cfg.max_header_size = ECAT_FOE_HEADER_BUF_MAX;
    cfg.registry = &target_reg;
    assert(ecat_foe_core_init(&core, &cfg) == 0);
    assert(ecat_transfer_init(&transfer, &target_reg, &core) == 0);
    assert(ecat_transfer_set_staging_buffer(&transfer, staging_buf, sizeof(staging_buf)) == 0);
    assert(ecat_transport_registry_init(&transport_reg, &transfer) == 0);

    /* 实例化 3 类通道插件并统一注册 */
    ecat_foe_transport_t foe_bridge;
    ecat_transfer_file_t file_adapter;
    ecat_transport_endpoint_t uart_ep;
    ecat_transport_plugin_desc_t desc;

    assert(ecat_foe_transport_init_transfer(&foe_bridge, &transfer) == 0);
    assert(ecat_foe_transport_make_plugin_desc(&foe_bridge, "FOE", &desc) == 0);
    assert(ecat_transport_registry_register(&transport_reg, &desc) == 0);

    assert(ecat_transfer_file_init(&file_adapter, &transfer, file_buf, sizeof(file_buf)) == 0);
    assert(ecat_transfer_file_make_plugin_desc(&file_adapter, ECAT_TRANSPORT_ID_FILE, "FILE", &desc) == 0);
    assert(ecat_transport_registry_register(&transport_reg, &desc) == 0);

    assert(ecat_transport_endpoint_init(&uart_ep, ECAT_TRANSPORT_ID_UART, "UART", &transfer, NULL) == 0);
    assert(ecat_transport_endpoint_make_plugin_desc(&uart_ep, 0UL, &desc) == 0);
    assert(ecat_transport_registry_register(&transport_reg, &desc) == 0);

    assert(ecat_transport_registry_count(&transport_reg) == 3U);
    assert(ecat_transport_registry_supports(&transport_reg, ECAT_TRANSPORT_ID_FOE, ECAT_TRANSPORT_CAP_BIDIRECTIONAL));
    assert(ecat_transport_registry_supports(&transport_reg, ECAT_TRANSPORT_ID_FILE, ECAT_TRANSPORT_CAP_BIDIRECTIONAL));
    assert(ecat_transport_registry_supports(&transport_reg, ECAT_TRANSPORT_ID_UART, ECAT_TRANSPORT_CAP_BIDIRECTIONAL));

    /* 场景 A: 通过统一 poll_active 驱动 File 插件完成 IMPORT -> 再完成 EXPORT 闭环 */
    uint8_t src_payload[72];
    uint32_t i;
    for (i = 0U; i < sizeof(src_payload); ++i) {
        src_payload[i] = (uint8_t)(0xA0U + (i & 0x3FU));
    }

    FILE *fin = tmpfile();
    assert(fin != NULL);
    assert(fwrite(src_payload, 1U, sizeof(src_payload), fin) == sizeof(src_payload));
    rewind(fin);

    ecat_transfer_request_t req;
    (void)memset(&req, 0, sizeof(req));
    req.transport_id = ECAT_TRANSPORT_ID_FILE;
    req.target_id = 10U;
    req.direction = ECAT_TRANSFER_DIR_IMPORT;
    req.expected_size = (uint32_t)sizeof(src_payload);
    assert(ecat_transfer_file_begin(&file_adapter, fin, &req) == ECAT_TRANSFER_COMPLETE);

    /* File 活跃时，其他插件 (UART) 发起会话应被互斥仲裁拦截为 BUSY */
    assert(ecat_transport_endpoint_begin_import(&uart_ep, 11U, 16U, 0U) == ECAT_TRANSFER_BUSY);

    uint16_t polled_id = 0U;
    ecat_transfer_result_t res = ECAT_TRANSFER_PENDING;
    uint32_t steps = 0U;
    while (res == ECAT_TRANSFER_PENDING) {
        res = ecat_transport_registry_poll_active(&transport_reg, &polled_id);
        assert(polled_id == ECAT_TRANSPORT_ID_FILE);
        steps++;
        assert(steps < 32U);
    }
    assert(res == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_registry_get_active(&transport_reg) == NULL);
    (void)fclose(fin);

    /* File EXPORT 验证 */
    FILE *fout = tmpfile();
    assert(fout != NULL);
    req.direction = ECAT_TRANSFER_DIR_EXPORT;
    req.expected_size = 0U;
    assert(ecat_transfer_file_begin(&file_adapter, fout, &req) == ECAT_TRANSFER_COMPLETE);
    res = ECAT_TRANSFER_PENDING;
    steps = 0U;
    while (res == ECAT_TRANSFER_PENDING) {
        res = ecat_transport_registry_poll_active(&transport_reg, &polled_id);
        assert(polled_id == ECAT_TRANSPORT_ID_FILE);
        steps++;
        assert(steps < 32U);
    }
    assert(res == ECAT_TRANSFER_COMPLETE);
    uint8_t exported[72];
    rewind(fout);
    assert(fread(exported, 1U, sizeof(exported), fout) == sizeof(exported));
    assert(memcmp(exported, src_payload, sizeof(src_payload)) == 0);
    (void)fclose(fout);

    /* 场景 B: 通过统一 poll_active 驱动自定义 UART Endpoint 异步 IMPORT 与异步 EXPORT */
    assert(ecat_transport_endpoint_begin_import(&uart_ep, 11U, 48U, 0U) == ECAT_TRANSFER_PENDING);
    assert(ecat_transport_registry_get_active(&transport_reg)->transport_id == ECAT_TRANSPORT_ID_UART);
    while ((res = ecat_transport_registry_poll_active(&transport_reg, &polled_id)) == ECAT_TRANSFER_PENDING) {
        assert(polled_id == ECAT_TRANSPORT_ID_UART);
    }
    assert(res == ECAT_TRANSFER_COMPLETE);

    uint8_t uart_tx[48];
    for (i = 0U; i < sizeof(uart_tx); ++i) {
        uart_tx[i] = (uint8_t)(0x30U + i);
    }
    assert(ecat_transport_endpoint_write_chunk(&uart_ep, uart_tx, sizeof(uart_tx)) == ECAT_TRANSFER_PENDING);
    while ((res = ecat_transport_registry_poll_active(&transport_reg, &polled_id)) == ECAT_TRANSFER_PENDING) {
        assert(polled_id == ECAT_TRANSPORT_ID_UART);
    }
    assert(res == ECAT_TRANSFER_COMPLETE);

    assert(ecat_transport_endpoint_finish_import(&uart_ep) == ECAT_TRANSFER_PENDING);
    while ((res = ecat_transport_registry_poll_active(&transport_reg, &polled_id)) == ECAT_TRANSFER_PENDING) {
        assert(polled_id == ECAT_TRANSPORT_ID_UART);
    }
    assert(res == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_registry_get_active(&transport_reg) == NULL);

    /* UART 异步 EXPORT */
    uint32_t total_export = 0U;
    assert(ecat_transport_endpoint_begin_export(&uart_ep, 11U, &total_export) == ECAT_TRANSFER_PENDING);
    while ((res = ecat_transport_registry_poll_active(&transport_reg, &polled_id)) == ECAT_TRANSFER_PENDING) {
        assert(polled_id == ECAT_TRANSPORT_ID_UART);
    }
    assert(res == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_get_total_export_size(&uart_ep) == 48U);

    uint8_t uart_rx[48];
    size_t actual_read = 0U;
    assert(ecat_transport_endpoint_read_chunk(&uart_ep, uart_rx, sizeof(uart_rx), &actual_read) == ECAT_TRANSFER_PENDING);
    while ((res = ecat_transport_registry_poll_active(&transport_reg, &polled_id)) == ECAT_TRANSFER_PENDING) {
        assert(polled_id == ECAT_TRANSPORT_ID_UART);
    }
    assert(res == ECAT_TRANSFER_COMPLETE);
    assert(actual_read == 48U);
    assert(memcmp(uart_rx, uart_tx, sizeof(uart_tx)) == 0);

    assert(ecat_transport_endpoint_finish_export(&uart_ep) == ECAT_TRANSFER_PENDING);
    while ((res = ecat_transport_registry_poll_active(&transport_reg, &polled_id)) == ECAT_TRANSFER_PENDING) {
        assert(polled_id == ECAT_TRANSPORT_ID_UART);
    }
    assert(res == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_registry_get_active(&transport_reg) == NULL);

    /* 场景 C: FoE 发起 RRQ 导出会话，由 abort_all 统一中止并释放总线锁 */
    uint32_t foe_export_sz = 0U;
    assert(ecat_foe_transport_begin_read(&foe_bridge, "CFG_SYNC", 8U, &foe_export_sz) == 0);
    assert(foe_export_sz == sizeof(src_payload));
    assert(ecat_transport_registry_get_active(&transport_reg)->transport_id == ECAT_TRANSPORT_ID_FOE);
    /* FoE 同步导出无需 poll，poll_active 应安全返回 INVALID 且不破坏会话 */
    assert(ecat_transport_registry_poll_active(&transport_reg, &polled_id) == ECAT_TRANSFER_INVALID);
    assert(polled_id == ECAT_TRANSPORT_ID_FOE);
    assert(ecat_foe_transport_owns(&foe_bridge));

    /* 统一 abort_all 强制中止 FoE 会话，立即释放锁给 UART */
    ecat_transport_registry_abort_all(&transport_reg);
    assert(ecat_transport_registry_get_active(&transport_reg) == NULL);
    assert(!ecat_foe_transport_owns(&foe_bridge));
    assert(ecat_transport_endpoint_begin_export(&uart_ep, 10U, &total_export) == ECAT_TRANSFER_COMPLETE);
    ecat_transport_registry_abort_all(&transport_reg);
    assert(ecat_transport_registry_get_active(&transport_reg) == NULL);

    printf("[TEST 2] 多插件同台注册 (FoE/File/UART)、统一 poll_active 双向流与 abort_all 清理... [PASS]\n");
}

int main(void)
{
    printf("=== 运行统一传输通道插件抽象与多通道注册/轮询单元测试 ===\n");
    test_registry_validation_and_capacity();
    test_multi_plugin_bidirectional_and_poll();
    printf(">>> 统一传输通道插件单元测试全部通过！<<<\n");
    return 0;
}
