/**
 * @file ldr_fw_pipeline.h
 * @brief 格式插件驱动的中性固件刷写流水线 (Format-Driven Firmware Upgrade Pipeline)
 *
 * 设计原则 (BARR-C:2018 / MISRA-C 友好, C99 零动态分配, 零 FoE 依赖):
 * 1. 多格式统一驱动: 通过 ldr_format_registry_t 自动探测并校验镜像头 (EFW / 自定义格式)，
 *    无需硬编码任何特定容器格式。
 * 2. 可选安全增强: 支持注入流式签名校验器 (如 Ed25519ph) 与防回滚版本下限事务提交
 *    (get_version_floor + finalize_with_version_floor)，所有策略预检均在触碰驱动前完成。
 * 3. 强防变砖契约: 头部、签名启动与防回滚预检通过前 0 次驱动调用；通过后严格执行
 *    init -> invalidate -> 按需 erase_to + write_unit 对齐写 (尾块 0xFF 补齐)
 *    -> 流 CRC32 与物理回读 CRC32 双重核验 -> 验签 finish -> finalize/防回滚事务提交；
 *    任何异常至多调用一次 abort() 并安全中止验签器。
 * 4. 零动态分配: 头部缓冲与写入对齐缓冲全部由调用方静态提供。
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
    LDR_FW_PIPE_ERR_READBACK_CRC,
    LDR_FW_PIPE_ERR_SIGNATURE_REQUIRED,
    LDR_FW_PIPE_ERR_SIGNATURE_INVALID,
    LDR_FW_PIPE_ERR_ANTI_ROLLBACK_GET,
    LDR_FW_PIPE_ERR_ANTI_ROLLBACK_REJECTED,
    LDR_FW_PIPE_ERR_ANTI_ROLLBACK_COMMIT
} ldr_fw_pipeline_err_t;

/* 中性流式数字签名校验器接口 (如 Ed25519ph / ECDSA / HMAC) */
typedef struct {
    int (*begin)(
        const uint8_t *key_id,
        size_t key_id_len,
        const uint8_t *signature,
        size_t signature_len,
        void *user_ctx);
    int (*update)(const uint8_t *data, size_t len, void *user_ctx);
    int (*finish)(void *user_ctx);
    void (*abort)(void *user_ctx);
    void *user_ctx;
} ldr_signature_verifier_t;

/* 防回滚版本下限查询回调 (由平台安全存储实现) */
typedef int (*ldr_get_version_floor_fn_t)(
    uint16_t target_id,
    uint32_t *out_floor,
    void *user_ctx);

/* 固件流水线安全策略配置 */
typedef struct {
    bool require_signature;
    ldr_signature_verifier_t verifier;
    bool enable_anti_rollback;
    ldr_get_version_floor_fn_t get_version_floor;
    void *anti_rollback_ctx;
} ldr_fw_security_cfg_t;

typedef struct {
    const ldr_format_registry_t *formats;
    const ldr_target_registry_t *targets;
    uint8_t *header_buf;
    size_t header_cap;
    uint8_t *residual_buf;
    size_t residual_cap;
    ldr_fw_security_cfg_t security;
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
    bool signature_active;
    ldr_fw_pipeline_err_t last_error;
} ldr_fw_pipeline_t;

/**
 * @brief 初始化格式插件驱动的固件刷写流水线并自动绑定到统一 Loader 框架
 */
int ldr_fw_pipeline_init(
    ldr_fw_pipeline_t *pipe,
    ldr_loader_t *loader,
    uint8_t *header_buf,
    size_t header_cap,
    uint8_t *residual_buf,
    size_t residual_cap);

/**
 * @brief 配置可选安全策略 (流式签名校验与防回滚版本下限事务提交)
 * @note 活跃刷写会话期间严禁修改安全策略；传 NULL 可重置为默认关闭。
 * @return 0 成功，-1 参数非法或当前有活跃会话
 */
int ldr_fw_pipeline_set_security(
    ldr_fw_pipeline_t *pipe,
    const ldr_fw_security_cfg_t *cfg);

/**
 * @brief 获取最近一次固件刷写流水线细分错误码
 */
ldr_fw_pipeline_err_t ldr_fw_pipeline_get_last_error(const ldr_fw_pipeline_t *pipe);

/**
 * @brief 获取最近一次通过格式插件校验解析出的中性元数据
 */
const ldr_format_metadata_t *ldr_fw_pipeline_get_last_metadata(const ldr_fw_pipeline_t *pipe);

#ifdef __cplusplus
}
#endif

#endif /* LDR_FW_PIPELINE_H */
