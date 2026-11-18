/**
 * @file test_uart_async_fw_example.c
 * @brief 自定义 UART 通道 + 异步固件刷写流水线 (RAW .bin + 流式验签 + 防回滚) 端到端参考示例
 *
 * 编译环境: 纯中性裁剪库 (ENABLE_FOE=0 ENABLE_FOE_CORE=0 ENABLE_EFW_PLUGIN=0 ENABLE_FILE=0)
 *           + 内置 ldr_format_raw 插件，零 FoE/EFW 符号依赖，零 malloc。
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ldr_loader.h"
#include "ldr_fw_pipeline.h"
#include "ldr_format_raw.h"
#include "ldr_crc32.h"
#include "ldr_uart_transport_template.h"

/* -------------------------------------------------------------------------- */
/*              模拟非阻塞异步 MCU Flash 驱动 (支持防回滚原子提交)            */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t  flash[256];
    uint32_t data_size;
    uint32_t erased_end;
    uint32_t persisted_floor;
    uint32_t pending_ticks;
    uint32_t tick_count;
    uint32_t finalize_floor_calls;
    uint32_t abort_calls;
} example_async_flash_t;

static ldr_async_op_result_t flash_exec(example_async_flash_t *f, ldr_async_data_op_t *op)
{
    switch (op->kind) {
    case LDR_ASYNC_DATA_OP_INIT:
        f->erased_end = 0U;
        f->data_size = 0U;
        return LDR_ASYNC_OP_COMPLETE;
    case LDR_ASYNC_DATA_OP_INVALIDATE:
    case LDR_ASYNC_DATA_OP_READ_OPEN:
    case LDR_ASYNC_DATA_OP_READ_CLOSE:
        return LDR_ASYNC_OP_COMPLETE;
    case LDR_ASYNC_DATA_OP_ERASE_TO:
        if (op->need_end > sizeof(f->flash)) {
            return LDR_ASYNC_OP_FAILED;
        }
        if (op->need_end > f->erased_end) {
            (void)memset(&f->flash[f->erased_end], 0xFF, op->need_end - f->erased_end);
            f->erased_end = op->need_end;
        }
        op->erased_end = f->erased_end;
        return LDR_ASYNC_OP_COMPLETE;
    case LDR_ASYNC_DATA_OP_WRITE:
        if ((op->offset + op->length) > sizeof(f->flash)) {
            return LDR_ASYNC_OP_FAILED;
        }
        (void)memcpy(&f->flash[op->offset], op->buffer, op->length);
        if ((op->offset + op->length) > f->data_size) {
            f->data_size = op->offset + op->length;
        }
        op->actual_length = op->length;
        return LDR_ASYNC_OP_COMPLETE;
    case LDR_ASYNC_DATA_OP_READ:
        if ((op->offset + op->length) > sizeof(f->flash)) {
            return LDR_ASYNC_OP_FAILED;
        }
        (void)memcpy(op->buffer, &f->flash[op->offset], op->length);
        op->actual_length = op->length;
        return LDR_ASYNC_OP_COMPLETE;
    case LDR_ASYNC_DATA_OP_FINALIZE_WITH_FLOOR:
        f->finalize_floor_calls++;
        if (op->fw_version < f->persisted_floor) {
            return LDR_ASYNC_OP_FAILED;
        }
        if (op->min_allow_version > f->persisted_floor) {
            f->persisted_floor = op->min_allow_version;
        }
        return LDR_ASYNC_OP_COMPLETE;
    default:
        return LDR_ASYNC_OP_FAILED;
    }
}

static ldr_async_op_result_t flash_start(void *ctx, ldr_async_data_op_t *op)
{
    example_async_flash_t *f = (example_async_flash_t *)ctx;
    f->tick_count = 0U;
    return (f->pending_ticks == 0U) ? flash_exec(f, op) : LDR_ASYNC_OP_PENDING;
}

static ldr_async_op_result_t flash_poll(void *ctx, ldr_async_data_op_t *op)
{
    example_async_flash_t *f = (example_async_flash_t *)ctx;
    f->tick_count++;
    if (f->tick_count < f->pending_ticks) {
        return LDR_ASYNC_OP_PENDING;
    }
    return flash_exec(f, op);
}

static void flash_abort(void *ctx)
{
    example_async_flash_t *f = (example_async_flash_t *)ctx;
    f->abort_calls++;
}

static uint32_t flash_get_size(void *ctx)
{
    const example_async_flash_t *f = (const example_async_flash_t *)ctx;
    return f->data_size;
}

static int flash_get_floor(uint16_t target_id, uint32_t *out_floor, void *ctx)
{
    const example_async_flash_t *f = (const example_async_flash_t *)ctx;
    (void)target_id;
    *out_floor = f->persisted_floor;
    return 0;
}

static const ldr_async_data_ops_t s_example_flash_ops = {
    flash_start,
    flash_poll,
    flash_abort
};

/* -------------------------------------------------------------------------- */
/*                     模拟带外流式签名校验器与 UART TX                       */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t running_crc;
    uint32_t expected_tag;
} example_sig_t;

static int sig_begin(const uint8_t *key_id, size_t key_id_len,
                     const uint8_t *sig, size_t sig_len, void *ctx)
{
    example_sig_t *s = (example_sig_t *)ctx;
    if ((key_id == NULL) || (key_id_len != 4U) || (sig == NULL) || (sig_len != 4U)) {
        return -1;
    }
    ldr_crc32_init(&s->running_crc);
    s->expected_tag = ((uint32_t)sig[0]) |
                      ((uint32_t)sig[1] << 8U) |
                      ((uint32_t)sig[2] << 16U) |
                      ((uint32_t)sig[3] << 24U);
    return 0;
}

static int sig_update(const uint8_t *data, size_t len, void *ctx)
{
    example_sig_t *s = (example_sig_t *)ctx;
    ldr_crc32_update(&s->running_crc, data, len);
    return 0;
}

static int sig_finish(void *ctx)
{
    example_sig_t *s = (example_sig_t *)ctx;
    return (ldr_crc32_final(&s->running_crc) == s->expected_tag) ? 0 : -1;
}

static void sig_abort(void *ctx)
{
    (void)ctx;
}

typedef struct {
    uint8_t rx_buf[256];
    size_t  rx_len;
} uart_wire_capture_t;

static int uart_tx_capture(const uint8_t *data, size_t len, void *hal_ctx)
{
    uart_wire_capture_t *cap = (uart_wire_capture_t *)hal_ctx;
    if ((cap->rx_len + len) > sizeof(cap->rx_buf)) {
        return -1;
    }
    (void)memcpy(&cap->rx_buf[cap->rx_len], data, len);
    cap->rx_len += len;
    return 0;
}

static ldr_transfer_result_t poll_until_done(ldr_loader_t *loader)
{
    uint32_t guard;
    for (guard = 0U; guard < 200U; ++guard) {
        uint16_t tid = 0U;
        ldr_transfer_result_t r = ldr_loader_poll(loader, &tid);
        if (r != LDR_RES_PENDING) {
            return r;
        }
    }
    assert(0 && "poll_until_done timeout");
    return LDR_RES_FAILED;
}

int main(void)
{
    ldr_loader_t loader;
    ldr_fw_pipeline_t pipe;
    ldr_uart_transport_t uart;
    example_async_flash_t flash;
    example_sig_t sig;
    uart_wire_capture_t wire;
    ldr_format_raw_ctx_t raw_ctx;
    ldr_format_plugin_desc_t raw_desc;
    uint8_t staging_buf[128];
    uint8_t header_buf[32];
    uint8_t residual_buf[16];
    uint8_t uart_tx_buf[32];
    uint8_t fw_bin[54];
    uint32_t i;

    (void)memset(&flash, 0, sizeof(flash));
    (void)memset(&sig, 0, sizeof(sig));
    (void)memset(&wire, 0, sizeof(wire));
    (void)memset(&raw_ctx, 0, sizeof(raw_ctx));
    flash.pending_ticks = 2U;
    flash.persisted_floor = 0x01000000U;

    for (i = 0U; i < sizeof(fw_bin); ++i) {
        fw_bin[i] = (uint8_t)(0x21U + i);
    }
    uint32_t fw_crc = ldr_crc32_calc(fw_bin, sizeof(fw_bin));

    /* 1. 初始化纯中性 Loader 框架、RAW 格式插件与异步固件流水线 */
    assert(ldr_loader_init(&loader) == 0);
    assert(ldr_loader_set_staging_buffer(&loader, staging_buf, sizeof(staging_buf)) == 0);

    raw_ctx.target_id = 1U;
    raw_ctx.fw_version = 0x01020000U;
    raw_ctx.min_allow_version = 0x01010000U;
    raw_ctx.payload_size = (uint32_t)sizeof(fw_bin);
    raw_ctx.payload_crc32 = fw_crc;
    raw_ctx.has_payload_crc32 = true;
    raw_ctx.is_signed = true;
    (void)memcpy(raw_ctx.key_id, "K001", 4U);
    raw_ctx.key_id_len = 4U;
    raw_ctx.signature[0] = (uint8_t)(fw_crc & 0xFFU);
    raw_ctx.signature[1] = (uint8_t)((fw_crc >> 8U) & 0xFFU);
    raw_ctx.signature[2] = (uint8_t)((fw_crc >> 16U) & 0xFFU);
    raw_ctx.signature[3] = (uint8_t)((fw_crc >> 24U) & 0xFFU);
    raw_ctx.signature_len = 4U;
    assert(ldr_format_raw_make_plugin_desc(&raw_ctx, &raw_desc) == 0);
    assert(ldr_loader_register_format(&loader, &raw_desc) == 0);

    assert(ldr_fw_pipeline_init(&pipe, &loader, header_buf, sizeof(header_buf),
                                residual_buf, sizeof(residual_buf)) == 0);

    ldr_fw_security_cfg_t sec;
    (void)memset(&sec, 0, sizeof(sec));
    sec.require_signature = true;
    sec.verifier.begin = sig_begin;
    sec.verifier.update = sig_update;
    sec.verifier.finish = sig_finish;
    sec.verifier.abort = sig_abort;
    sec.verifier.user_ctx = &sig;
    sec.enable_anti_rollback = true;
    sec.get_version_floor = flash_get_floor;
    sec.anti_rollback_ctx = &flash;
    assert(ldr_fw_pipeline_set_security(&pipe, &sec) == 0);

    /* 2. 注册异步 FIRMWARE 目标 + 自定义 UART 双向通道 */
    ldr_target_desc_t fw_target;
    (void)memset(&fw_target, 0, sizeof(fw_target));
    fw_target.target_id = 1U;
    (void)strcpy(fw_target.name, "APP_RAW");
    fw_target.type = LDR_TARGET_TYPE_FIRMWARE;
    fw_target.max_size = sizeof(flash.flash);
    fw_target.write_unit = 16U;
    fw_target.permissions = LDR_PERM_RW;
    fw_target.enabled = true;
    fw_target.asynchronous = true;
    fw_target.driver.async_ops = &s_example_flash_ops;
    fw_target.driver.get_data_size = flash_get_size;
    fw_target.user_ctx = &flash;
    assert(ldr_loader_register_target(&loader, &fw_target) == 0);

    assert(ldr_uart_transport_init(&uart, &loader, LDR_TRANSPORT_ID_UART, "UART_FW",
                                   uart_tx_capture, &wire, uart_tx_buf, sizeof(uart_tx_buf)) == 0);
    assert(ldr_uart_transport_register(&uart, &loader) == 0);

    /* 3. 异步 RAW .bin 固件 IMPORT 刷写 (分 2 片 + 异步轮询 + 防回滚原子提交) */
    assert(ldr_uart_transport_on_import_begin(&uart, 1U, (uint32_t)sizeof(fw_bin), 0x01020000U) == LDR_RES_COMPLETE);
    assert(ldr_uart_transport_on_import_data(&uart, &fw_bin[0], 30U) == LDR_RES_PENDING);
    assert(poll_until_done(&loader) == LDR_RES_COMPLETE);
    assert(ldr_uart_transport_on_import_data(&uart, &fw_bin[30], sizeof(fw_bin) - 30U) == LDR_RES_PENDING);
    assert(poll_until_done(&loader) == LDR_RES_COMPLETE);
    assert(ldr_uart_transport_on_import_end(&uart) == LDR_RES_PENDING);
    assert(poll_until_done(&loader) == LDR_RES_COMPLETE);

    assert(flash.finalize_floor_calls == 1U);
    assert(flash.abort_calls == 0U);
    assert(flash.persisted_floor == 0x01010000U);
    assert(memcmp(flash.flash, fw_bin, sizeof(fw_bin)) == 0);

    /* 4. 通过同一 UART 通道异步 EXPORT 回读刚刷入的固件并逐字节核对 */
    uint32_t total_export = 0U;
    assert(ldr_uart_transport_on_export_begin(&uart, 1U, &total_export) == LDR_RES_PENDING);
    assert(poll_until_done(&loader) == LDR_RES_COMPLETE);
    assert(ldr_transport_endpoint_get_total_export_size(&uart.endpoint) == 64U);

    ldr_transfer_result_t exp_res;
    uint32_t guard = 0U;
    do {
        exp_res = ldr_uart_transport_pump_export_frame(&uart);
        guard++;
        assert(guard < 200U);
    } while ((exp_res == LDR_RES_PENDING) || (exp_res == LDR_RES_CONTINUE));

    if (exp_res == LDR_RES_PENDING) {
        exp_res = poll_until_done(&loader);
    }
    assert(exp_res == LDR_RES_COMPLETE);
    assert(wire.rx_len == 64U);
    assert(memcmp(wire.rx_buf, fw_bin, sizeof(fw_bin)) == 0);

    printf("PASS: custom UART async firmware pipeline example (RAW .bin + signature + anti-rollback + async EXPORT)\n");
    return 0;
}
