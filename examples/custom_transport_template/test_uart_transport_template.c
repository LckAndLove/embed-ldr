/**
 * @file test_uart_transport_template.c
 * @brief 自定义双向 UART 通道插件参考模板端到端验证 (纯去 FoE/FILE 裁剪模式)
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ldr_loader.h"
#include "ldr_uart_transport_template.h"

int port_get_version_floor(uint16_t target_id, uint32_t *out_floor)
{
    (void)target_id;
    if (out_floor != NULL) {
        *out_floor = 0U;
    }
    return 0;
}

typedef struct {
    uint8_t mem[256];
    uint32_t data_size;
    uint32_t erased_end;
} mock_nvram_t;

static int nvram_init(void *ctx)
{
    mock_nvram_t *nv = (mock_nvram_t *)ctx;
    nv->erased_end = 0U;
    nv->data_size = 0U;
    return 0;
}

static int nvram_invalidate(void *ctx)
{
    (void)ctx;
    return 0;
}

static int nvram_erase_to(uint32_t need_end, uint32_t *erased_end, void *ctx)
{
    mock_nvram_t *nv = (mock_nvram_t *)ctx;
    if (need_end > sizeof(nv->mem)) {
        return -1;
    }
    if (need_end > nv->erased_end) {
        (void)memset(&nv->mem[nv->erased_end], 0xFF, need_end - nv->erased_end);
        nv->erased_end = need_end;
    }
    *erased_end = nv->erased_end;
    return 0;
}

static int nvram_write(uint32_t offset, const uint8_t *data, uint32_t len, void *ctx)
{
    mock_nvram_t *nv = (mock_nvram_t *)ctx;
    if ((offset + len) > sizeof(nv->mem)) {
        return -1;
    }
    (void)memcpy(&nv->mem[offset], data, len);
    if ((offset + len) > nv->data_size) {
        nv->data_size = offset + len;
    }
    return 0;
}

static int nvram_read(uint32_t offset, uint8_t *buf, uint32_t len, void *ctx)
{
    mock_nvram_t *nv = (mock_nvram_t *)ctx;
    if ((offset + len) > sizeof(nv->mem)) {
        return -1;
    }
    (void)memcpy(buf, &nv->mem[offset], len);
    return 0;
}

static int nvram_finalize(void *ctx)
{
    (void)ctx;
    return 0;
}

static void nvram_abort(void *ctx)
{
    (void)ctx;
}

static int nvram_read_open(void *ctx)
{
    (void)ctx;
    return 0;
}

static void nvram_read_close(void *ctx)
{
    (void)ctx;
}

static uint32_t nvram_get_data_size(void *ctx)
{
    const mock_nvram_t *nv = (const mock_nvram_t *)ctx;
    return nv->data_size;
}

typedef struct {
    uint8_t rx_wire[256];
    size_t  rx_len;
    uint32_t frames;
} mock_uart_wire_t;

static int mock_uart_tx(const uint8_t *data, size_t len, void *hal_ctx)
{
    mock_uart_wire_t *wire = (mock_uart_wire_t *)hal_ctx;
    if ((wire == NULL) || ((wire->rx_len + len) > sizeof(wire->rx_wire))) {
        return -1;
    }
    (void)memcpy(&wire->rx_wire[wire->rx_len], data, len);
    wire->rx_len += len;
    wire->frames++;
    return 0;
}

int main(void)
{
    ldr_loader_t loader;
    mock_nvram_t nvram;
    mock_uart_wire_t wire;
    ldr_uart_transport_t uart;
    uint8_t tx_frame_buf[32];

    (void)memset(&nvram, 0, sizeof(nvram));
    (void)memset(&wire, 0, sizeof(wire));

    /* 1. 完全去 FoE / FILE 初始化统一 Loader 框架 */
    assert(ldr_loader_init(&loader, NULL) == 0);

    /* 2. 注册目标存储插件 (配置参数区 ID=50) */
    ldr_target_desc_t target;
    (void)memset(&target, 0, sizeof(target));
    target.target_id = 50U;
    (void)strcpy(target.name, "NVRAM_CFG");
    target.type = LDR_TARGET_TYPE_DATA;
    target.max_size = sizeof(nvram.mem);
    target.write_unit = 1U;
    target.permissions = LDR_PERM_RW;
    target.enabled = true;
    target.driver.io.init = nvram_init;
    target.driver.io.invalidate = nvram_invalidate;
    target.driver.io.erase_to = nvram_erase_to;
    target.driver.io.write = nvram_write;
    target.driver.io.read = nvram_read;
    target.driver.io.finalize = nvram_finalize;
    target.driver.io.abort = nvram_abort;
    target.driver.read_open = nvram_read_open;
    target.driver.read_close = nvram_read_close;
    target.driver.get_data_size = nvram_get_data_size;
    target.user_ctx = &nvram;
    assert(ldr_loader_register_target(&loader, &target) == 0);

    ldr_loader_set_enforce_acl(&loader, true);
    assert(ldr_loader_add_acl(&loader, LDR_TRANSPORT_ID_UART, 50U, LDR_PERM_RW) == 0);

    /* 3. 初始化并注册自定义 UART 双向通道插件 */
    assert(ldr_uart_transport_init(&uart, &loader, LDR_TRANSPORT_ID_UART, "UART0",
                                   mock_uart_tx, &wire, tx_frame_buf, sizeof(tx_frame_buf)) == 0);
    assert(ldr_uart_transport_register(&uart, &loader) == 0);

    /* 4. 双向 IMPORT: 分 3 帧写入 80 字节 */
    uint8_t payload[80];
    uint32_t i;
    for (i = 0U; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)(0x20U + i);
    }
    assert(ldr_uart_transport_on_import_begin(&uart, 50U, (uint32_t)sizeof(payload), 0U) == LDR_RES_COMPLETE);
    assert(ldr_uart_transport_on_import_data(&uart, &payload[0], 32U) == LDR_RES_COMPLETE);
    assert(ldr_uart_transport_on_import_data(&uart, &payload[32], 32U) == LDR_RES_COMPLETE);
    assert(ldr_uart_transport_on_import_data(&uart, &payload[64], 16U) == LDR_RES_COMPLETE);
    assert(ldr_uart_transport_on_import_end(&uart) == LDR_RES_COMPLETE);
    assert(!ldr_loader_is_busy(&loader));
    assert(uart.events_completed == 1U);

    /* 5. 双向 EXPORT: 分帧回读并校验逐字节一致 */
    uint32_t total_export = 0U;
    assert(ldr_uart_transport_on_export_begin(&uart, 50U, &total_export) == LDR_RES_COMPLETE);
    assert(total_export == sizeof(payload));
    ldr_transfer_result_t res = LDR_RES_CONTINUE;
    while (res == LDR_RES_CONTINUE) {
        res = ldr_uart_transport_pump_export_frame(&uart);
    }
    assert(res == LDR_RES_COMPLETE);
    assert(wire.rx_len == sizeof(payload));
    assert(wire.frames == 3U);
    assert(memcmp(wire.rx_wire, payload, sizeof(payload)) == 0);
    assert(uart.events_completed == 2U);

    printf("PASS: custom UART bidirectional transport template (zero-FoE/FILE trim)\n");
    return 0;
}
