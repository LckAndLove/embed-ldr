/**
 * @file ldr_uart_transport_template.c
 * @brief 自定义双向传输通道插件参考模板实现 (纯 ldr_ 命名, BARR-C:2018 / MISRA-C 友好)
 */

#include "ldr_uart_transport_template.h"
#include <string.h>

static void uart_on_session_start(void *user_ctx, ldr_transfer_dir_t dir, uint16_t target_id)
{
    ldr_uart_transport_t *uart = (ldr_uart_transport_t *)user_ctx;
    (void)dir;
    (void)target_id;
    if (uart != NULL) {
        uart->events_started++;
    }
}

static void uart_on_progress(void *user_ctx, uint32_t transferred, uint32_t total)
{
    ldr_uart_transport_t *uart = (ldr_uart_transport_t *)user_ctx;
    (void)transferred;
    (void)total;
    if (uart != NULL) {
        uart->events_progress++;
    }
}

static void uart_on_session_complete(void *user_ctx, uint32_t total_transferred, uint32_t crc32)
{
    ldr_uart_transport_t *uart = (ldr_uart_transport_t *)user_ctx;
    (void)total_transferred;
    if (uart != NULL) {
        uart->events_completed++;
        uart->last_crc32 = crc32;
    }
}

static void uart_on_session_abort(void *user_ctx, ldr_transfer_err_t err)
{
    ldr_uart_transport_t *uart = (ldr_uart_transport_t *)user_ctx;
    (void)err;
    if (uart != NULL) {
        uart->events_aborted++;
    }
}

int ldr_uart_transport_init(
    ldr_uart_transport_t *uart,
    ldr_loader_t *loader,
    uint16_t transport_id,
    const char *name,
    ldr_uart_tx_fn_t tx_fn,
    void *hal_ctx,
    uint8_t *tx_buf,
    size_t tx_buf_cap)
{
    ldr_transfer_t *transfer;
    ldr_transport_callbacks_t cbs;

    if ((uart == NULL) || (loader == NULL) || (tx_fn == NULL) ||
        (tx_buf == NULL) || (tx_buf_cap == 0U)) {
        return -1;
    }

    transfer = ldr_loader_get_transfer(loader);
    if (transfer == NULL) {
        return -1;
    }

    (void)memset(uart, 0, sizeof(*uart));
    if (ldr_transport_endpoint_init(&uart->endpoint, transport_id, name, transfer, uart) != 0) {
        return -1;
    }

    uart->tx_fn = tx_fn;
    uart->hal_ctx = hal_ctx;
    uart->tx_buf = tx_buf;
    uart->tx_buf_cap = tx_buf_cap;

    cbs.on_session_start = uart_on_session_start;
    cbs.on_progress = uart_on_progress;
    cbs.on_session_complete = uart_on_session_complete;
    cbs.on_session_abort = uart_on_session_abort;
    ldr_transport_endpoint_set_callbacks(&uart->endpoint, &cbs);

    return 0;
}

int ldr_uart_transport_register(
    ldr_uart_transport_t *uart,
    ldr_loader_t *loader)
{
    ldr_transport_plugin_desc_t desc;

    if ((uart == NULL) || (loader == NULL)) {
        return -1;
    }

    if (ldr_transport_endpoint_make_plugin_desc(
            &uart->endpoint,
            LDR_TRANSPORT_CAP_BIDIRECTIONAL | LDR_TRANSPORT_CAP_ASYNC,
            &desc) != 0) {
        return -1;
    }

    return ldr_loader_register_transport(loader, &desc);
}

ldr_transfer_result_t ldr_uart_transport_on_import_begin(
    ldr_uart_transport_t *uart,
    uint16_t target_id,
    uint32_t expected_size,
    uint32_t fw_version)
{
    if (uart == NULL) {
        return LDR_RES_INVALID;
    }
    return ldr_transport_endpoint_begin_import(
        &uart->endpoint, target_id, expected_size, fw_version);
}

ldr_transfer_result_t ldr_uart_transport_on_import_data(
    ldr_uart_transport_t *uart,
    const uint8_t *frame_data,
    size_t frame_len)
{
    if (uart == NULL) {
        return LDR_RES_INVALID;
    }
    return ldr_transport_endpoint_write_chunk(&uart->endpoint, frame_data, frame_len);
}

ldr_transfer_result_t ldr_uart_transport_on_import_end(
    ldr_uart_transport_t *uart)
{
    if (uart == NULL) {
        return LDR_RES_INVALID;
    }
    return ldr_transport_endpoint_finish_import(&uart->endpoint);
}

ldr_transfer_result_t ldr_uart_transport_on_export_begin(
    ldr_uart_transport_t *uart,
    uint16_t target_id,
    uint32_t *out_total_size)
{
    if (uart == NULL) {
        return LDR_RES_INVALID;
    }
    uart->pending_read_len = 0U;
    uart->export_read_in_flight = false;
    return ldr_transport_endpoint_begin_export(&uart->endpoint, target_id, out_total_size);
}

ldr_transfer_result_t ldr_uart_transport_pump_export_frame(
    ldr_uart_transport_t *uart)
{
    ldr_transfer_result_t res;

    if ((uart == NULL) || (uart->tx_fn == NULL) || (uart->tx_buf == NULL)) {
        return LDR_RES_INVALID;
    }

    if (uart->export_read_in_flight) {
        res = ldr_transport_endpoint_poll(&uart->endpoint);
    } else {
        uart->pending_read_len = 0U;
        res = ldr_transport_endpoint_read_chunk(
            &uart->endpoint, uart->tx_buf, uart->tx_buf_cap, &uart->pending_read_len);
    }

    if (res == LDR_RES_PENDING) {
        uart->export_read_in_flight = true;
        return LDR_RES_PENDING;
    }
    uart->export_read_in_flight = false;

    if ((res != LDR_RES_CONTINUE) && (res != LDR_RES_COMPLETE)) {
        uart->pending_read_len = 0U;
        return res;
    }

    if (uart->pending_read_len > 0U) {
        size_t tx_len = uart->pending_read_len;
        uart->pending_read_len = 0U;
        if (uart->tx_fn(uart->tx_buf, tx_len, uart->hal_ctx) != 0) {
            ldr_transport_endpoint_abort(&uart->endpoint);
            return LDR_RES_FAILED;
        }
    }

    if (res == LDR_RES_COMPLETE) {
        return ldr_transport_endpoint_finish_export(&uart->endpoint);
    }
    return LDR_RES_CONTINUE;
}

void ldr_uart_transport_abort(ldr_uart_transport_t *uart)
{
    if (uart == NULL) {
        return;
    }
    uart->pending_read_len = 0U;
    uart->export_read_in_flight = false;
    ldr_transport_endpoint_abort(&uart->endpoint);
}
