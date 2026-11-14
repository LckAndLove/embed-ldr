/**
 * @file ldr_fw_pipeline.h
 * @brief 格式插件驱动的中性固件刷写流水线 (Format-Driven Firmware Upgrade Pipeline)
 *
 * 设计原则 (BARR-C:2018 / MISRA-C 友好, C99 零动态分配, 零 FoE 依赖):
 * 1. 多格式统一驱动: 通过 ldr_format_registry_t 自动探测并校验镜像头 (EFW / 自定义格式)，
 *    无需硬编码任何特定容器格式。
 * 2. 强防变砖契约: 头部未通过格式插件与容量/目标交叉核验前绝不触发任何驱动回调 (0 次调用)；
 *    校验通过后严格执行 init -> invalidate -> 按需 erase_to + write_unit 对齐写 (尾块 0xFF 补齐)
 *    -> 流 CRC32 与物理回读 CRC32 双重核验 -> finalize；任何异常至多调用一次 abort()。
 * 3. 零动态分配: 头部缓冲与写入对齐缓冲全部由调用方静态提供。
 */

#ifndef LDR_FW_PIPELINE_H
#define LDR_FW_PIPELINE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "ldr_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LDR_FW_PIPE_OK = 0,
    LDR_FW_PIPE_ERR_INVALID_ARG,
    LDR_FW_PIPE_ERR_TARGET_INVALID,
    LDR_FW_PIPE_ERR_HEADER_TOO_LARGE,
    LDR_FW_PIPE_ERR_FORMAT_REJECTED,
    LDR_FW_PIPE_ERR_TARGET_MISMATCH,
    LDR_FW_PIPE_ERR_VERSION_MISMATCH,
    LDR_FW_PIPE_ERR_PAYLOAD_LIMIT,
    LDR_FW_PIPE_ERR_OVER_RECEIVE,
    LDR_FW_PIPE_ERR_UNDER_RECEIVE,
    LDR_FW_PIPE_ERR_DRIVER_INIT,
    LDR_FW_PIPE_ERR_DRIVER_INVALIDATE,
    LDR_FW_PIPE_ERR_DRIVER_ERASE,
    LDR_FW_PIPE_ERR_DRIVER_WRITE,
    LDR_FW_PIPE_ERR_DRIVER_READ,
    LDR_FW_PIPE_ERR_DRIVER_FINALIZE,
    LDR_FW_PIPE_ERR_STREAM_CRC,
    LDR_FW_PIPE_ERR_READBACK_CRC
} ldr_fw_pipeline_err_t;

typedef struct {
    const ldr_format_registry_t *formats;
    const ldr_target_registry_t *targets;
    uint8_t *header_buf;
    size_t header_cap;
    uint8_t *residual_buf;
    size_t residual_cap;
    bool initialized;

    /* 活跃刷写会话状态 */
    const ldr_target_desc_t *active_target;
    uint16_t req_target_id;
    uint32_t req_fw_version;
    size_t header_rx_len;
    uint16_t expected_header_len;
    bool header_complete;
    ldr_format_metadata_t meta;
    uint32_t payload_rx_bytes;
    uint32_t erased_watermark;
    uint32_t written_offset;
    uint32_t stream_crc32;
    size_t residual_len;
    bool session_active;
    bool driver_initialized;
    bool driver_aborted;
    bool driver_finalized;
    ldr_fw_pipeline_err_t last_error;
} ldr_fw_pipeline_t;

/**
 * @brief 初始化格式插件驱动的固件刷写流水线并自动绑定到统一 Loader 框架
 * @param pipe 流水线上下文指针
 * @param loader 已初始化的统一 Loader 框架句柄
 * @param header_buf 调用方静态提供的头部装配缓冲区 (容量需覆盖最大已注册镜像头)
 * @param header_cap 头部缓冲区容量 (字节，建议 >= 256)
 * @param residual_buf 调用方静态提供的对齐写入缓冲 (容量需 >= 最大固件目标 write_unit)
 * @param residual_cap 对齐缓冲区容量 (字节)
 * @return 0 成功，-1 参数非法或绑定失败
 */
int ldr_fw_pipeline_init(
    ldr_fw_pipeline_t *pipe,
    ldr_loader_t *loader,
    uint8_t *header_buf,
    size_t header_cap,
    uint8_t *residual_buf,
    size_t residual_cap);

/**
 * @brief 获取最近一次固件刷写流水线细分错误码
 */
ldr_fw_pipeline_err_t ldr_fw_pipeline_get_last_error(const ldr_fw_pipeline_t *pipe);

/**
 * @brief 获取最近一次通过格式插件校验解析出的中性元数据
 * @return 头部已校验完成时返回元数据指针，否则返回 NULL
 */
const ldr_format_metadata_t *ldr_fw_pipeline_get_last_metadata(const ldr_fw_pipeline_t *pipe);

#ifdef __cplusplus
}
#endif

#endif /* LDR_FW_PIPELINE_H */
