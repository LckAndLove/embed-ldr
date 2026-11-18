/**
 * @file ldr_uart_transport_template.h
 * @brief 自定义双向传输通道插件参考模板 (UART / RS-485 帧通道示例)
 *
 * 适用场景:
 * - 演示如何基于统一中性 ldr_loader.h 接口，零动态分配接入任意自定义物理总线
 *   (UART / RS-485 / CAN / USB / BLE / SPI 等)，同时支持双向 IMPORT 与 EXPORT。
 * - 完全不依赖 EtherCAT / FoE 或 stdio.h，可在 ENABLE_FOE=0 ENABLE_FILE=0 裁剪模式下独立编译。
 */

#ifndef LDR_UART_TRANSPORT_TEMPLATE_H
#define LDR_UART_TRANSPORT_TEMPLATE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "ldr_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 底层物理串口发送回调 (用户按硬件 HAL 实现) */
typedef int (*ldr_uart_tx_fn_t)(const uint8_t *data, size_t len, void *hal_ctx);

typedef struct {
    ldr_transport_endpoint_t endpoint;   /* 通用双向端点状态机 */
    ldr_uart_tx_fn_t         tx_fn;      /* 底层物理发送回调 (用于 EXPORT 推流) */
    void                    *hal_ctx;    /* 硬件外设上下文 */
    uint8_t                 *tx_buf;     /* 调用方静态提供的单帧导出缓冲 */
    size_t                   tx_buf_cap; /* 导出缓冲容量 (字节) */
    size_t                   pending_read_len;      /* 持久化读长度槽位 (防止异步读栈指针悬空) */
    bool                     export_read_in_flight; /* 异步导出读取挂起标志 */
    bool                     export_finishing;      /* 异步导出终结关闭挂起标志 */
    uint32_t                 events_started;
    uint32_t                 events_progress;
    uint32_t                 events_completed;
    uint32_t                 events_aborted;
    uint32_t                 last_crc32;
} ldr_uart_transport_t;

/**
 * @brief 初始化自定义 UART 双向通道插件实例
 * @param uart 实例指针
 * @param loader 统一 Loader 框架句柄
 * @param transport_id 通道唯一 ID (建议使用 LDR_TRANSPORT_ID_UART 或 LDR_TRANSPORT_ID_USER_BASE + N)
 * @param name 通道名称 (<= 15 字符)
 * @param tx_fn 物理发送函数指针 (EXPORT 时调用)
 * @param hal_ctx 硬件 HAL 上下文
 * @param tx_buf 静态导出分块缓冲
 * @param tx_buf_cap 缓冲容量 (必须 > 0)
 * @return 0 成功，-1 参数非法
 */
int ldr_uart_transport_init(
    ldr_uart_transport_t *uart,
    ldr_loader_t *loader,
    uint16_t transport_id,
    const char *name,
    ldr_uart_tx_fn_t tx_fn,
    void *hal_ctx,
    uint8_t *tx_buf,
    size_t tx_buf_cap);

/**
 * @brief 将本通道插件注册到统一 Loader 框架的通道注册中心
 */
int ldr_uart_transport_register(
    ldr_uart_transport_t *uart,
    ldr_loader_t *loader);

/* ---- IMPORT (主机 -> 设备) 协议帧处理入口 ---- */

ldr_transfer_result_t ldr_uart_transport_on_import_begin(
    ldr_uart_transport_t *uart,
    uint16_t target_id,
    uint32_t expected_size,
    uint32_t fw_version);

ldr_transfer_result_t ldr_uart_transport_on_import_data(
    ldr_uart_transport_t *uart,
    const uint8_t *frame_data,
    size_t frame_len);

ldr_transfer_result_t ldr_uart_transport_on_import_end(
    ldr_uart_transport_t *uart);

/* ---- EXPORT (设备 -> 主机) 协议帧处理入口 ---- */

ldr_transfer_result_t ldr_uart_transport_on_export_begin(
    ldr_uart_transport_t *uart,
    uint16_t target_id,
    uint32_t *out_total_size);

/**
 * @brief 拉取下一帧导出数据并通过 tx_fn 发送到物理总线
 * @return LDR_RES_CONTINUE 仍有后续帧；LDR_RES_COMPLETE 全部导出并完成提交；其他为错误或 PENDING
 */
ldr_transfer_result_t ldr_uart_transport_pump_export_frame(
    ldr_uart_transport_t *uart);

void ldr_uart_transport_abort(ldr_uart_transport_t *uart);

#ifdef __cplusplus
}
#endif

#endif /* LDR_UART_TRANSPORT_TEMPLATE_H */
