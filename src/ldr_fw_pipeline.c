/**
 * @file ldr_fw_pipeline.c
 * @brief 格式插件驱动的中性固件刷写流水线实现 (同步 + 非阻塞异步 + 验签 + 防回滚)
 */

#define LDR_NEUTRAL_BUILD 1
#include "ldr_fw_pipeline.h"
#include "ldr_crc32.h"
#include <string.h>

#define LDR_FW_PIPE_HEADER_MIN_LEN 4U
#define LDR_FW_PIPE_HEADER_MAX_LEN 512U

static const ecat_upgrade_async_ops_t s_ldr_fw_pipeline_async_ops;

static void reset_session_fields(ldr_fw_pipeline_t *pipe)
{
    pipe->active_target = NULL;
    pipe->req_target_id = 0U;
    pipe->req_fw_version = 0U;
    pipe->header_rx_len = 0U;
    pipe->expected_header_len = 0U;
    pipe->header_complete = false;
    (void)memset(&pipe->meta, 0, sizeof(pipe->meta));
    pipe->payload_rx_bytes = 0U;
    pipe->erased_watermark = 0U;
    pipe->written_offset = 0U;
    pipe->stream_crc32 = 0U;
    pipe->residual_len = 0U;
    pipe->session_active = false;
    pipe->driver_initialized = false;
    pipe->driver_aborted = false;
    pipe->driver_finalized = false;
    pipe->signature_active = false;
    pipe->is_async = false;
    pipe->async_phase = LDR_FW_ASYNC_PHASE_NONE;
    (void)memset(&pipe->async_op, 0, sizeof(pipe->async_op));
    pipe->async_chunk_ptr = NULL;
    pipe->async_chunk_rem = 0U;
    pipe->async_ending = false;
    pipe->async_readback_started = false;
    pipe->async_stream_crc32_final = 0U;
    pipe->async_readback_offset = 0U;
    pipe->async_readback_crc32 = 0U;
}

static void abort_verifier_once(ldr_fw_pipeline_t *pipe)
{
    if ((pipe != NULL) && pipe->signature_active) {
        if (pipe->security.verifier.abort != NULL) {
            pipe->security.verifier.abort(pipe->security.verifier.user_ctx);
        }
        pipe->signature_active = false;
    }
}

static void abort_driver_once(ldr_fw_pipeline_t *pipe)
{
    if (pipe == NULL) {
        return;
    }
    abort_verifier_once(pipe);
    if (pipe->driver_initialized && !pipe->driver_finalized && !pipe->driver_aborted) {
        if (pipe->active_target != NULL) {
            if (pipe->is_async) {
                if ((pipe->active_target->driver.async_ops != NULL) &&
                    (pipe->active_target->driver.async_ops->abort != NULL)) {
                    pipe->active_target->driver.async_ops->abort(pipe->active_target->user_ctx);
                }
            } else if (pipe->active_target->driver.io.abort != NULL) {
                pipe->active_target->driver.io.abort(pipe->active_target->user_ctx);
            }
        }
        pipe->driver_aborted = true;
    }
    pipe->async_phase = LDR_FW_ASYNC_PHASE_NONE;
    pipe->async_chunk_ptr = NULL;
    pipe->async_chunk_rem = 0U;
    pipe->async_ending = false;
    pipe->session_active = false;
}

static int flush_aligned_block_sync(ldr_fw_pipeline_t *pipe, const ldr_target_desc_t *target)
{
    uint32_t write_unit = target->write_unit;
    uint32_t need_end;

    if ((write_unit == 0U) || (pipe->residual_len != (size_t)write_unit)) {
        pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_WRITE;
        return -1;
    }
    if (pipe->written_offset > (UINT32_MAX - write_unit)) {
        pipe->last_error = LDR_FW_PIPE_ERR_PAYLOAD_LIMIT;
        return -1;
    }

    need_end = pipe->written_offset + write_unit;
    if (need_end > pipe->erased_watermark) {
        uint32_t actual_erased = 0U;
        if ((target->driver.io.erase_to(need_end, &actual_erased, target->user_ctx) != 0) ||
            (actual_erased < need_end)) {
            pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_ERASE;
            return -1;
        }
        pipe->erased_watermark = actual_erased;
    }

    if (target->driver.io.write(
            pipe->written_offset, pipe->residual_buf, write_unit, target->user_ctx) != 0) {
        pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_WRITE;
        return -1;
    }

    pipe->written_offset = need_end;
    pipe->residual_len = 0U;
    return 0;
}

static int pipe_precheck_security(ldr_fw_pipeline_t *pipe, const ldr_format_metadata_t *meta)
{
    if (pipe->security.enable_anti_rollback) {
        uint32_t current_floor = 0U;

        if (meta->min_allow_version > meta->fw_version) {
            pipe->last_error = LDR_FW_PIPE_ERR_ANTI_ROLLBACK_REJECTED;
            return -1;
        }
        if ((pipe->security.get_version_floor == NULL) ||
            (pipe->security.get_version_floor(
                 pipe->req_target_id, &current_floor, pipe->security.anti_rollback_ctx) != 0)) {
            pipe->last_error = LDR_FW_PIPE_ERR_ANTI_ROLLBACK_GET;
            return -1;
        }
        if (meta->fw_version < current_floor) {
            pipe->last_error = LDR_FW_PIPE_ERR_ANTI_ROLLBACK_REJECTED;
            return -1;
        }
    }

    if (pipe->security.require_signature && !meta->is_signed) {
        pipe->last_error = LDR_FW_PIPE_ERR_SIGNATURE_REQUIRED;
        return -1;
    }

    if (pipe->security.require_signature ||
        (meta->is_signed && (pipe->security.verifier.begin != NULL))) {
        if ((pipe->security.verifier.begin == NULL) ||
            (pipe->security.verifier.update == NULL) ||
            (pipe->security.verifier.finish == NULL) ||
            (pipe->security.verifier.abort == NULL)) {
            pipe->last_error = LDR_FW_PIPE_ERR_SIGNATURE_INVALID;
            return -1;
        }
        if (pipe->security.verifier.begin(
                meta->key_id,
                (size_t)meta->key_id_len,
                meta->signature,
                (size_t)meta->signature_len,
                pipe->security.verifier.user_ctx) != 0) {
            pipe->security.verifier.abort(pipe->security.verifier.user_ctx);
            pipe->last_error = LDR_FW_PIPE_ERR_SIGNATURE_INVALID;
            return -1;
        }
        pipe->signature_active = true;
    }

    return 0;
}

/* 公共头部装配与元数据/安全预检: 返回 1 表示头部已就绪，0 表示 NEED_MORE，-1 表示失败 */
static int pipe_consume_header_chunk(
    ldr_fw_pipeline_t *pipe,
    const ldr_target_desc_t *target,
    const uint8_t **io_cursor,
    size_t *io_remaining,
    ecat_transfer_err_t *out_err)
{
    size_t prev_hdr_len = pipe->header_rx_len;
    size_t space_left = pipe->header_cap - prev_hdr_len;
    size_t to_copy = (*io_remaining < space_left) ? *io_remaining : space_left;
    uint16_t detected_hdr_len = 0U;
    ldr_format_metadata_t parsed_meta;
    ldr_format_probe_result_t pres;

    if (to_copy > 0U) {
        (void)memcpy(&pipe->header_buf[prev_hdr_len], *io_cursor, to_copy);
        pipe->header_rx_len = prev_hdr_len + to_copy;
    }

    (void)memset(&parsed_meta, 0, sizeof(parsed_meta));
    pres = ldr_format_registry_probe_and_parse(
        pipe->formats, pipe->header_buf, pipe->header_rx_len,
        &detected_hdr_len, &parsed_meta);

    if (pres == LDR_FORMAT_NEED_MORE) {
        if (((detected_hdr_len > 0U) && ((size_t)detected_hdr_len > pipe->header_cap)) ||
            (pipe->header_rx_len >= pipe->header_cap)) {
            pipe->last_error = LDR_FW_PIPE_ERR_HEADER_TOO_LARGE;
            if (out_err != NULL) {
                *out_err = ECAT_TRANSFER_ERR_CORE_UPGRADE;
            }
            return -1;
        }
        pipe->expected_header_len = detected_hdr_len;
        *io_cursor += to_copy;
        *io_remaining -= to_copy;
        return 0;
    }

    if (pres != LDR_FORMAT_MATCH) {
        pipe->last_error = LDR_FW_PIPE_ERR_FORMAT_REJECTED;
        if (out_err != NULL) {
            *out_err = ECAT_TRANSFER_ERR_CORE_UPGRADE;
        }
        return -1;
    }

    if ((parsed_meta.header_size == 0U) ||
        ((size_t)parsed_meta.header_size > pipe->header_rx_len) ||
        ((size_t)parsed_meta.header_size < prev_hdr_len)) {
        pipe->last_error = LDR_FW_PIPE_ERR_FORMAT_REJECTED;
        if (out_err != NULL) {
            *out_err = ECAT_TRANSFER_ERR_CORE_UPGRADE;
        }
        return -1;
    }

    if ((parsed_meta.target_id != 0U) && (parsed_meta.target_id != pipe->req_target_id)) {
        pipe->last_error = LDR_FW_PIPE_ERR_TARGET_MISMATCH;
        if (out_err != NULL) {
            *out_err = ECAT_TRANSFER_ERR_CORE_UPGRADE;
        }
        return -1;
    }

    if ((pipe->req_fw_version != 0U) && (parsed_meta.fw_version != pipe->req_fw_version)) {
        pipe->last_error = LDR_FW_PIPE_ERR_VERSION_MISMATCH;
        if (out_err != NULL) {
            *out_err = ECAT_TRANSFER_ERR_CORE_UPGRADE;
        }
        return -1;
    }

    if ((parsed_meta.payload_size == 0U) || (parsed_meta.payload_size > target->max_size)) {
        pipe->last_error = LDR_FW_PIPE_ERR_PAYLOAD_LIMIT;
        if (out_err != NULL) {
            *out_err = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
        }
        return -1;
    }

    if (pipe_precheck_security(pipe, &parsed_meta) != 0) {
        if (out_err != NULL) {
            *out_err = ECAT_TRANSFER_ERR_CORE_UPGRADE;
        }
        return -1;
    }

    pipe->meta = parsed_meta;
    size_t consumed = (size_t)parsed_meta.header_size - prev_hdr_len;
    *io_cursor += consumed;
    *io_remaining -= consumed;
    return 1;
}

static bool pipe_is_binding_valid(
    const void *fw_ctx,
    const ecat_target_registry_t *reg,
    const void *shared_session)
{
    const ldr_fw_pipeline_t *pipe = (const ldr_fw_pipeline_t *)fw_ctx;
    (void)shared_session;
    if (pipe == NULL) {
        return false;
    }
    return pipe->initialized && (pipe->targets == reg) && (pipe->formats != NULL);
}

static bool pipe_is_busy(const void *fw_ctx)
{
    const ldr_fw_pipeline_t *pipe = (const ldr_fw_pipeline_t *)fw_ctx;
    if (pipe == NULL) {
        return false;
    }
    return pipe->session_active;
}

static bool pipe_validate_async_binding(
    const void *fw_ctx,
    const ecat_upgrade_async_ops_t *async_ops,
    const void *async_ctx)
{
    const ldr_fw_pipeline_t *pipe = (const ldr_fw_pipeline_t *)fw_ctx;
    if ((pipe == NULL) || !pipe->initialized) {
        return false;
    }
    return (async_ops == &s_ldr_fw_pipeline_async_ops) && (async_ctx == pipe);
}

static int pipe_begin_target(
    void *fw_ctx,
    const ecat_target_desc_t *target,
    uint16_t target_id,
    uint32_t fw_version,
    ecat_transfer_err_t *out_err)
{
    ldr_fw_pipeline_t *pipe = (ldr_fw_pipeline_t *)fw_ctx;

    if ((pipe == NULL) || !pipe->initialized || (target == NULL)) {
        if (out_err != NULL) {
            *out_err = ECAT_TRANSFER_ERR_CORE_UPGRADE;
        }
        return -1;
    }

    if ((target->type != LDR_TARGET_TYPE_FIRMWARE) ||
        (target->max_size == 0U) ||
        (target->write_unit == 0U) ||
        ((size_t)target->write_unit > pipe->residual_cap)) {
        pipe->last_error = LDR_FW_PIPE_ERR_TARGET_INVALID;
        if (out_err != NULL) {
            *out_err = ECAT_TRANSFER_ERR_DRIVER_WRITE;
        }
        return -1;
    }

    if (target->asynchronous) {
        if ((target->driver.async_ops == NULL) ||
            (target->driver.async_ops->start == NULL) ||
            (target->driver.async_ops->poll == NULL) ||
            (target->driver.async_ops->abort == NULL)) {
            pipe->last_error = LDR_FW_PIPE_ERR_TARGET_INVALID;
            if (out_err != NULL) {
                *out_err = ECAT_TRANSFER_ERR_CORE_UPGRADE;
            }
            return -1;
        }
    } else {
        if ((target->driver.io.init == NULL) ||
            (target->driver.io.invalidate == NULL) ||
            (target->driver.io.erase_to == NULL) ||
            (target->driver.io.write == NULL) ||
            (target->driver.io.read == NULL) ||
            (target->driver.io.finalize == NULL) ||
            (target->driver.io.abort == NULL) ||
            (pipe->security.enable_anti_rollback &&
             (target->driver.io.finalize_with_version_floor == NULL))) {
            pipe->last_error = LDR_FW_PIPE_ERR_TARGET_INVALID;
            if (out_err != NULL) {
                *out_err = ECAT_TRANSFER_ERR_DRIVER_WRITE;
            }
            return -1;
        }
    }

    reset_session_fields(pipe);
    pipe->active_target = target;
    pipe->req_target_id = target_id;
    pipe->req_fw_version = fw_version;
    pipe->is_async = target->asynchronous;
    pipe->session_active = true;
    pipe->last_error = LDR_FW_PIPE_OK;
    return 0;
}

static int pipe_on_data(
    void *fw_ctx,
    const ecat_target_desc_t *target,
    const uint8_t *data,
    uint32_t len,
    ecat_transfer_err_t *out_err)
{
    ldr_fw_pipeline_t *pipe = (ldr_fw_pipeline_t *)fw_ctx;
    const uint8_t *cursor = data;
    size_t remaining = (size_t)len;

    if ((pipe == NULL) || !pipe->session_active || pipe->is_async || (target == NULL) ||
        (pipe->active_target != target) || ((remaining > 0U) && (cursor == NULL))) {
        if (out_err != NULL) {
            *out_err = ECAT_TRANSFER_ERR_CORE_UPGRADE;
        }
        return -1;
    }

    if (!pipe->header_complete) {
        int hrc = pipe_consume_header_chunk(pipe, target, &cursor, &remaining, out_err);
        if (hrc < 0) {
            return -1;
        }
        if (hrc == 0) {
            return 0;
        }

        if (target->driver.io.init(target->user_ctx) != 0) {
            abort_verifier_once(pipe);
            pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_INIT;
            if (out_err != NULL) {
                *out_err = ECAT_TRANSFER_ERR_DRIVER_INIT;
            }
            return -1;
        }
        pipe->driver_initialized = true;

        if (target->driver.io.invalidate(target->user_ctx) != 0) {
            pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_INVALIDATE;
            if (out_err != NULL) {
                *out_err = ECAT_TRANSFER_ERR_DRIVER_INVALIDATE;
            }
            return -1;
        }

        pipe->header_complete = true;
        ldr_crc32_init(&pipe->stream_crc32);
    }

    if (remaining == 0U) {
        return 0;
    }

    if (remaining > (size_t)(pipe->meta.payload_size - pipe->payload_rx_bytes)) {
        pipe->last_error = LDR_FW_PIPE_ERR_OVER_RECEIVE;
        if (out_err != NULL) {
            *out_err = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
        }
        return -1;
    }

    if (pipe->signature_active) {
        if (pipe->security.verifier.update(
                cursor, remaining, pipe->security.verifier.user_ctx) != 0) {
            pipe->last_error = LDR_FW_PIPE_ERR_SIGNATURE_INVALID;
            if (out_err != NULL) {
                *out_err = ECAT_TRANSFER_ERR_CORE_UPGRADE;
            }
            return -1;
        }
    }

    ldr_crc32_update(&pipe->stream_crc32, cursor, remaining);
    pipe->payload_rx_bytes += (uint32_t)remaining;

    while (remaining > 0U) {
        size_t unit = (size_t)target->write_unit;
        size_t need = unit - pipe->residual_len;
        size_t take = (remaining < need) ? remaining : need;

        (void)memcpy(&pipe->residual_buf[pipe->residual_len], cursor, take);
        pipe->residual_len += take;
        cursor += take;
        remaining -= take;

        if (pipe->residual_len == unit) {
            if (flush_aligned_block_sync(pipe, target) != 0) {
                if (out_err != NULL) {
                    *out_err = (pipe->last_error == LDR_FW_PIPE_ERR_DRIVER_ERASE)
                        ? ECAT_TRANSFER_ERR_DRIVER_ERASE
                        : ECAT_TRANSFER_ERR_DRIVER_WRITE;
                }
                return -1;
            }
        }
    }

    return 0;
}

static int pipe_on_end(void *fw_ctx)
{
    ldr_fw_pipeline_t *pipe = (ldr_fw_pipeline_t *)fw_ctx;
    const ldr_target_desc_t *target;
    uint32_t stream_crc;
    uint32_t readback_crc;
    uint32_t offset = 0U;
    uint8_t rb_buf[LDR_FW_PIPE_READBACK_CHUNK_LEN];

    if ((pipe == NULL) || !pipe->session_active || pipe->is_async || (pipe->active_target == NULL)) {
        return -1;
    }
    target = pipe->active_target;

    if (!pipe->header_complete || (pipe->payload_rx_bytes != pipe->meta.payload_size)) {
        pipe->last_error = LDR_FW_PIPE_ERR_UNDER_RECEIVE;
        return -1;
    }

    if (pipe->residual_len > 0U) {
        size_t unit = (size_t)target->write_unit;
        (void)memset(&pipe->residual_buf[pipe->residual_len], 0xFF, unit - pipe->residual_len);
        pipe->residual_len = unit;
        if (flush_aligned_block_sync(pipe, target) != 0) {
            return -1;
        }
    }

    stream_crc = ldr_crc32_final(&pipe->stream_crc32);
    if (pipe->meta.has_payload_crc32 && (stream_crc != pipe->meta.payload_crc32)) {
        pipe->last_error = LDR_FW_PIPE_ERR_STREAM_CRC;
        return -1;
    }

    ldr_crc32_init(&readback_crc);
    while (offset < pipe->meta.payload_size) {
        uint32_t left = pipe->meta.payload_size - offset;
        uint32_t chunk = (left < LDR_FW_PIPE_READBACK_CHUNK_LEN)
            ? left
            : LDR_FW_PIPE_READBACK_CHUNK_LEN;
        if (target->driver.io.read(offset, rb_buf, chunk, target->user_ctx) != 0) {
            pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_READ;
            return -1;
        }
        ldr_crc32_update(&readback_crc, rb_buf, (size_t)chunk);
        offset += chunk;
    }

    if (ldr_crc32_final(&readback_crc) != stream_crc) {
        pipe->last_error = LDR_FW_PIPE_ERR_READBACK_CRC;
        return -1;
    }

    if (pipe->signature_active) {
        int vrc = pipe->security.verifier.finish(pipe->security.verifier.user_ctx);
        pipe->signature_active = false;
        if (vrc != 0) {
            pipe->last_error = LDR_FW_PIPE_ERR_SIGNATURE_INVALID;
            return -1;
        }
    }

    if (pipe->security.enable_anti_rollback) {
        if (target->driver.io.finalize_with_version_floor(
                pipe->req_target_id,
                pipe->meta.fw_version,
                pipe->meta.min_allow_version,
                target->user_ctx) != 0) {
            pipe->last_error = LDR_FW_PIPE_ERR_ANTI_ROLLBACK_COMMIT;
            return -1;
        }
    } else {
        if (target->driver.io.finalize(target->user_ctx) != 0) {
            pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_FINALIZE;
            return -1;
        }
    }

    pipe->driver_finalized = true;
    pipe->session_active = false;
    return 0;
}

/* -------------------------------------------------------------------------- */
/*                     非阻塞异步固件刷写状态机实现                           */
/* -------------------------------------------------------------------------- */

static ecat_upgrade_async_result_t pipe_async_start_write_block(
    ldr_fw_pipeline_t *pipe,
    const ldr_target_desc_t *target)
{
    uint32_t write_unit = target->write_unit;
    ecat_async_op_result_t ares;

    (void)memset(&pipe->async_op, 0, sizeof(pipe->async_op));
    pipe->async_op.kind = ECAT_ASYNC_DATA_OP_WRITE;
    pipe->async_op.offset = pipe->written_offset;
    pipe->async_op.length = write_unit;
    pipe->async_op.buffer = pipe->residual_buf;

    ares = target->driver.async_ops->start(target->user_ctx, &pipe->async_op);
    if (ares == ECAT_ASYNC_OP_PENDING) {
        pipe->async_phase = LDR_FW_ASYNC_PHASE_WRITE;
        return ECAT_UPGRADE_ASYNC_PENDING;
    }
    if ((ares != ECAT_ASYNC_OP_COMPLETE) || (pipe->async_op.actual_length != (size_t)write_unit)) {
        pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_WRITE;
        abort_driver_once(pipe);
        return ECAT_UPGRADE_ASYNC_FAILED;
    }
    pipe->written_offset += write_unit;
    pipe->residual_len = 0U;
    pipe->async_phase = LDR_FW_ASYNC_PHASE_NONE;
    return ECAT_UPGRADE_ASYNC_COMPLETE;
}

static ecat_upgrade_async_result_t pipe_async_flush_residual(
    ldr_fw_pipeline_t *pipe,
    const ldr_target_desc_t *target)
{
    uint32_t write_unit = target->write_unit;
    uint32_t need_end;

    if (pipe->written_offset > (UINT32_MAX - write_unit)) {
        pipe->last_error = LDR_FW_PIPE_ERR_PAYLOAD_LIMIT;
        abort_driver_once(pipe);
        return ECAT_UPGRADE_ASYNC_FAILED;
    }

    need_end = pipe->written_offset + write_unit;
    if (need_end > pipe->erased_watermark) {
        ecat_async_op_result_t eres;
        (void)memset(&pipe->async_op, 0, sizeof(pipe->async_op));
        pipe->async_op.kind = ECAT_ASYNC_DATA_OP_ERASE_TO;
        pipe->async_op.need_end = need_end;
        eres = target->driver.async_ops->start(target->user_ctx, &pipe->async_op);
        if (eres == ECAT_ASYNC_OP_PENDING) {
            pipe->async_phase = LDR_FW_ASYNC_PHASE_ERASE;
            return ECAT_UPGRADE_ASYNC_PENDING;
        }
        if ((eres != ECAT_ASYNC_OP_COMPLETE) || (pipe->async_op.erased_end < need_end)) {
            pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_ERASE;
            abort_driver_once(pipe);
            return ECAT_UPGRADE_ASYNC_FAILED;
        }
        pipe->erased_watermark = pipe->async_op.erased_end;
    }

    return pipe_async_start_write_block(pipe, target);
}

/* 推进当前挂起中的底层异步驱动操作: 返回 1 继续主循环，0 最终完成，-1 PENDING/FAILED */ 
static int pipe_async_step_poll(
    ldr_fw_pipeline_t *pipe,
    const ldr_target_desc_t *target,
    ecat_upgrade_async_result_t *out_res)
{
    const ecat_async_data_ops_t *aops = target->driver.async_ops;
    ecat_async_op_result_t pres = aops->poll(target->user_ctx, &pipe->async_op);

    if (pres == ECAT_ASYNC_OP_PENDING) {
        *out_res = ECAT_UPGRADE_ASYNC_PENDING;
        return -1;
    }
    if (pres != ECAT_ASYNC_OP_COMPLETE) {
        switch (pipe->async_phase) {
        case LDR_FW_ASYNC_PHASE_INIT:
            pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_INIT;
            break;
        case LDR_FW_ASYNC_PHASE_INVALIDATE:
            pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_INVALIDATE;
            break;
        case LDR_FW_ASYNC_PHASE_ERASE:
            pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_ERASE;
            break;
        case LDR_FW_ASYNC_PHASE_READBACK:
            pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_READ;
            break;
        case LDR_FW_ASYNC_PHASE_FINALIZE:
            pipe->last_error = pipe->security.enable_anti_rollback
                ? LDR_FW_PIPE_ERR_ANTI_ROLLBACK_COMMIT
                : LDR_FW_PIPE_ERR_DRIVER_FINALIZE;
            break;
        case LDR_FW_ASYNC_PHASE_WRITE:
        default:
            pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_WRITE;
            break;
        }
        abort_driver_once(pipe);
        *out_res = ECAT_UPGRADE_ASYNC_FAILED;
        return -1;
    }

    switch (pipe->async_phase) {
    case LDR_FW_ASYNC_PHASE_INIT: {
        ecat_async_op_result_t ires;
        pipe->driver_initialized = true;
        (void)memset(&pipe->async_op, 0, sizeof(pipe->async_op));
        pipe->async_op.kind = ECAT_ASYNC_DATA_OP_INVALIDATE;
        ires = aops->start(target->user_ctx, &pipe->async_op);
        if (ires == ECAT_ASYNC_OP_PENDING) {
            pipe->async_phase = LDR_FW_ASYNC_PHASE_INVALIDATE;
            *out_res = ECAT_UPGRADE_ASYNC_PENDING;
            return -1;
        }
        if (ires != ECAT_ASYNC_OP_COMPLETE) {
            pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_INVALIDATE;
            abort_driver_once(pipe);
            *out_res = ECAT_UPGRADE_ASYNC_FAILED;
            return -1;
        }
        pipe->header_complete = true;
        ldr_crc32_init(&pipe->stream_crc32);
        pipe->async_phase = LDR_FW_ASYNC_PHASE_NONE;
        return 1;
    }
    case LDR_FW_ASYNC_PHASE_INVALIDATE:
        pipe->header_complete = true;
        ldr_crc32_init(&pipe->stream_crc32);
        pipe->async_phase = LDR_FW_ASYNC_PHASE_NONE;
        return 1;
    case LDR_FW_ASYNC_PHASE_ERASE: {
        uint32_t need_end = pipe->written_offset + target->write_unit;
        ecat_upgrade_async_result_t wres;
        if (pipe->async_op.erased_end < need_end) {
            pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_ERASE;
            abort_driver_once(pipe);
            *out_res = ECAT_UPGRADE_ASYNC_FAILED;
            return -1;
        }
        pipe->erased_watermark = pipe->async_op.erased_end;
        wres = pipe_async_start_write_block(pipe, target);
        if (wres != ECAT_UPGRADE_ASYNC_COMPLETE) {
            *out_res = wres;
            return -1;
        }
        return 1;
    }
    case LDR_FW_ASYNC_PHASE_WRITE:
        if (pipe->async_op.actual_length != (size_t)target->write_unit) {
            pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_WRITE;
            abort_driver_once(pipe);
            *out_res = ECAT_UPGRADE_ASYNC_FAILED;
            return -1;
        }
        pipe->written_offset += target->write_unit;
        pipe->residual_len = 0U;
        pipe->async_phase = LDR_FW_ASYNC_PHASE_NONE;
        return 1;
    case LDR_FW_ASYNC_PHASE_READBACK:
        if (pipe->async_op.actual_length != (size_t)pipe->async_op.length) {
            pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_READ;
            abort_driver_once(pipe);
            *out_res = ECAT_UPGRADE_ASYNC_FAILED;
            return -1;
        }
        ldr_crc32_update(&pipe->async_readback_crc32,
                         pipe->async_readback_buf,
                         pipe->async_op.actual_length);
        pipe->async_readback_offset += pipe->async_op.length;
        pipe->async_phase = LDR_FW_ASYNC_PHASE_NONE;
        return 1;
    case LDR_FW_ASYNC_PHASE_FINALIZE:
        pipe->driver_finalized = true;
        pipe->async_phase = LDR_FW_ASYNC_PHASE_NONE;
        pipe->session_active = false;
        *out_res = ECAT_UPGRADE_ASYNC_COMPLETE;
        return 0;
    case LDR_FW_ASYNC_PHASE_NONE:
    default:
        abort_driver_once(pipe);
        *out_res = ECAT_UPGRADE_ASYNC_FAILED;
        return -1;
    }
}

/* 异步终结阶段推进: 尾块 0xFF 补齐 -> 流 CRC -> 异步回读 CRC -> 验签 -> 异步 FINALIZE */
static int pipe_async_step_finalize(
    ldr_fw_pipeline_t *pipe,
    const ldr_target_desc_t *target,
    ecat_upgrade_async_result_t *out_res)
{
    const ecat_async_data_ops_t *aops = target->driver.async_ops;
    ecat_async_op_result_t fin_res;

    if (pipe->payload_rx_bytes != pipe->meta.payload_size) {
        pipe->last_error = LDR_FW_PIPE_ERR_UNDER_RECEIVE;
        abort_driver_once(pipe);
        *out_res = ECAT_UPGRADE_ASYNC_FAILED;
        return -1;
    }

    if (pipe->residual_len > 0U) {
        size_t unit = (size_t)target->write_unit;
        ecat_upgrade_async_result_t fres;
        (void)memset(&pipe->residual_buf[pipe->residual_len], 0xFF, unit - pipe->residual_len);
        pipe->residual_len = unit;
        fres = pipe_async_flush_residual(pipe, target);
        if (fres != ECAT_UPGRADE_ASYNC_COMPLETE) {
            *out_res = fres;
            return -1;
        }
        return 1;
    }

    if (!pipe->async_readback_started) {
        pipe->async_stream_crc32_final = ldr_crc32_final(&pipe->stream_crc32);
        if (pipe->meta.has_payload_crc32 &&
            (pipe->async_stream_crc32_final != pipe->meta.payload_crc32)) {
            pipe->last_error = LDR_FW_PIPE_ERR_STREAM_CRC;
            abort_driver_once(pipe);
            *out_res = ECAT_UPGRADE_ASYNC_FAILED;
            return -1;
        }
        pipe->async_readback_started = true;
        pipe->async_readback_offset = 0U;
        ldr_crc32_init(&pipe->async_readback_crc32);
    }

    if (pipe->async_readback_offset < pipe->meta.payload_size) {
        uint32_t left = pipe->meta.payload_size - pipe->async_readback_offset;
        uint32_t chunk = (left < LDR_FW_PIPE_READBACK_CHUNK_LEN)
            ? left
            : LDR_FW_PIPE_READBACK_CHUNK_LEN;
        ecat_async_op_result_t rres;
        (void)memset(&pipe->async_op, 0, sizeof(pipe->async_op));
        pipe->async_op.kind = ECAT_ASYNC_DATA_OP_READ;
        pipe->async_op.offset = pipe->async_readback_offset;
        pipe->async_op.length = chunk;
        pipe->async_op.buffer = pipe->async_readback_buf;
        rres = aops->start(target->user_ctx, &pipe->async_op);
        if (rres == ECAT_ASYNC_OP_PENDING) {
            pipe->async_phase = LDR_FW_ASYNC_PHASE_READBACK;
            *out_res = ECAT_UPGRADE_ASYNC_PENDING;
            return -1;
        }
        if ((rres != ECAT_ASYNC_OP_COMPLETE) || (pipe->async_op.actual_length != (size_t)chunk)) {
            pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_READ;
            abort_driver_once(pipe);
            *out_res = ECAT_UPGRADE_ASYNC_FAILED;
            return -1;
        }
        ldr_crc32_update(&pipe->async_readback_crc32, pipe->async_readback_buf, (size_t)chunk);
        pipe->async_readback_offset += chunk;
        return 1;
    }

    if (ldr_crc32_final(&pipe->async_readback_crc32) != pipe->async_stream_crc32_final) {
        pipe->last_error = LDR_FW_PIPE_ERR_READBACK_CRC;
        abort_driver_once(pipe);
        *out_res = ECAT_UPGRADE_ASYNC_FAILED;
        return -1;
    }

    if (pipe->signature_active) {
        int vrc = pipe->security.verifier.finish(pipe->security.verifier.user_ctx);
        pipe->signature_active = false;
        if (vrc != 0) {
            pipe->last_error = LDR_FW_PIPE_ERR_SIGNATURE_INVALID;
            abort_driver_once(pipe);
            *out_res = ECAT_UPGRADE_ASYNC_FAILED;
            return -1;
        }
    }

    (void)memset(&pipe->async_op, 0, sizeof(pipe->async_op));
    if (pipe->security.enable_anti_rollback) {
        pipe->async_op.kind = ECAT_ASYNC_DATA_OP_FINALIZE_WITH_FLOOR;
        pipe->async_op.target_id = pipe->req_target_id;
        pipe->async_op.fw_version = pipe->meta.fw_version;
        pipe->async_op.min_allow_version = pipe->meta.min_allow_version;
    } else {
        pipe->async_op.kind = ECAT_ASYNC_DATA_OP_FINALIZE;
    }
    fin_res = aops->start(target->user_ctx, &pipe->async_op);
    if (fin_res == ECAT_ASYNC_OP_PENDING) {
        pipe->async_phase = LDR_FW_ASYNC_PHASE_FINALIZE;
        *out_res = ECAT_UPGRADE_ASYNC_PENDING;
        return -1;
    }
    if (fin_res != ECAT_ASYNC_OP_COMPLETE) {
        pipe->last_error = pipe->security.enable_anti_rollback
            ? LDR_FW_PIPE_ERR_ANTI_ROLLBACK_COMMIT
            : LDR_FW_PIPE_ERR_DRIVER_FINALIZE;
        abort_driver_once(pipe);
        *out_res = ECAT_UPGRADE_ASYNC_FAILED;
        return -1;
    }

    pipe->driver_finalized = true;
    pipe->session_active = false;
    *out_res = ECAT_UPGRADE_ASYNC_COMPLETE;
    return 0;
}

static ecat_upgrade_async_result_t pipe_async_drive(ldr_fw_pipeline_t *pipe)
{
    const ldr_target_desc_t *target = pipe->active_target;
    const ecat_async_data_ops_t *aops = target->driver.async_ops;

    for (;;) {
        if (pipe->async_phase != LDR_FW_ASYNC_PHASE_NONE) {
            ecat_upgrade_async_result_t step_res = ECAT_UPGRADE_ASYNC_FAILED;
            int prc = pipe_async_step_poll(pipe, target, &step_res);
            if (prc <= 0) {
                return step_res;
            }
            continue;
        }

        /* 1. 头部装配阶段 */
        if (!pipe->header_complete) {
            ecat_async_op_result_t init_res;
            ecat_async_op_result_t inv_res;
            int hrc;

            if (pipe->async_ending) {
                pipe->last_error = LDR_FW_PIPE_ERR_UNDER_RECEIVE;
                abort_driver_once(pipe);
                return ECAT_UPGRADE_ASYNC_FAILED;
            }
            if (pipe->async_chunk_rem == 0U) {
                return ECAT_UPGRADE_ASYNC_COMPLETE;
            }
            hrc = pipe_consume_header_chunk(
                pipe, target, &pipe->async_chunk_ptr, &pipe->async_chunk_rem, NULL);
            if (hrc < 0) {
                abort_driver_once(pipe);
                return ECAT_UPGRADE_ASYNC_FAILED;
            }
            if (hrc == 0) {
                return ECAT_UPGRADE_ASYNC_COMPLETE;
            }

            (void)memset(&pipe->async_op, 0, sizeof(pipe->async_op));
            pipe->async_op.kind = ECAT_ASYNC_DATA_OP_INIT;
            init_res = aops->start(target->user_ctx, &pipe->async_op);
            if (init_res == ECAT_ASYNC_OP_PENDING) {
                pipe->driver_initialized = true;
                pipe->async_phase = LDR_FW_ASYNC_PHASE_INIT;
                return ECAT_UPGRADE_ASYNC_PENDING;
            }
            if (init_res != ECAT_ASYNC_OP_COMPLETE) {
                pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_INIT;
                abort_driver_once(pipe);
                return ECAT_UPGRADE_ASYNC_FAILED;
            }
            pipe->driver_initialized = true;

            (void)memset(&pipe->async_op, 0, sizeof(pipe->async_op));
            pipe->async_op.kind = ECAT_ASYNC_DATA_OP_INVALIDATE;
            inv_res = aops->start(target->user_ctx, &pipe->async_op);
            if (inv_res == ECAT_ASYNC_OP_PENDING) {
                pipe->async_phase = LDR_FW_ASYNC_PHASE_INVALIDATE;
                return ECAT_UPGRADE_ASYNC_PENDING;
            }
            if (inv_res != ECAT_ASYNC_OP_COMPLETE) {
                pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_INVALIDATE;
                abort_driver_once(pipe);
                return ECAT_UPGRADE_ASYNC_FAILED;
            }
            pipe->header_complete = true;
            ldr_crc32_init(&pipe->stream_crc32);
            continue;
        }

        /* 2. 载荷流式接收与异步对齐写入 */
        if (pipe->async_chunk_rem > 0U) {
            size_t unit = (size_t)target->write_unit;
            size_t need = unit - pipe->residual_len;
            size_t take = (pipe->async_chunk_rem < need) ? pipe->async_chunk_rem : need;

            if (pipe->async_chunk_rem > (size_t)(pipe->meta.payload_size - pipe->payload_rx_bytes)) {
                pipe->last_error = LDR_FW_PIPE_ERR_OVER_RECEIVE;
                abort_driver_once(pipe);
                return ECAT_UPGRADE_ASYNC_FAILED;
            }

            if (pipe->signature_active) {
                if (pipe->security.verifier.update(
                        pipe->async_chunk_ptr, take, pipe->security.verifier.user_ctx) != 0) {
                    pipe->last_error = LDR_FW_PIPE_ERR_SIGNATURE_INVALID;
                    abort_driver_once(pipe);
                    return ECAT_UPGRADE_ASYNC_FAILED;
                }
            }
            ldr_crc32_update(&pipe->stream_crc32, pipe->async_chunk_ptr, take);
            pipe->payload_rx_bytes += (uint32_t)take;

            (void)memcpy(&pipe->residual_buf[pipe->residual_len], pipe->async_chunk_ptr, take);
            pipe->residual_len += take;
            pipe->async_chunk_ptr += take;
            pipe->async_chunk_rem -= take;

            if (pipe->residual_len == unit) {
                ecat_upgrade_async_result_t fres = pipe_async_flush_residual(pipe, target);
                if (fres != ECAT_UPGRADE_ASYNC_COMPLETE) {
                    return fres;
                }
            }
            continue;
        }

        if (!pipe->async_ending) {
            return ECAT_UPGRADE_ASYNC_COMPLETE;
        }

        /* 3. 异步终结阶段 */
        {
            ecat_upgrade_async_result_t fin_step_res = ECAT_UPGRADE_ASYNC_FAILED;
            int frc = pipe_async_step_finalize(pipe, target, &fin_step_res);
            if (frc <= 0) {
                return fin_step_res;
            }
        }
    }
}

static ecat_upgrade_async_result_t pipe_async_start(
    void *ctx,
    const uint8_t *data,
    size_t length,
    bool more_data)
{
    ldr_fw_pipeline_t *pipe = (ldr_fw_pipeline_t *)ctx;

    if ((pipe == NULL) || !pipe->session_active || !pipe->is_async ||
        (pipe->active_target == NULL) || (pipe->async_phase != LDR_FW_ASYNC_PHASE_NONE)) {
        return ECAT_UPGRADE_ASYNC_FAILED;
    }

    pipe->async_chunk_ptr = data;
    pipe->async_chunk_rem = length;
    pipe->async_ending = !more_data;
    return pipe_async_drive(pipe);
}

static ecat_upgrade_async_result_t pipe_async_poll(void *ctx)
{
    ldr_fw_pipeline_t *pipe = (ldr_fw_pipeline_t *)ctx;

    if ((pipe == NULL) || !pipe->session_active || !pipe->is_async ||
        (pipe->active_target == NULL) || (pipe->async_phase == LDR_FW_ASYNC_PHASE_NONE)) {
        return ECAT_UPGRADE_ASYNC_FAILED;
    }

    return pipe_async_drive(pipe);
}

static void pipe_async_abort(void *ctx)
{
    abort_driver_once((ldr_fw_pipeline_t *)ctx);
}

static const ecat_upgrade_async_ops_t s_ldr_fw_pipeline_async_ops = {
    pipe_async_start,
    pipe_async_poll,
    pipe_async_abort
};

static void pipe_on_abort(void *fw_ctx)
{
    abort_driver_once((ldr_fw_pipeline_t *)fw_ctx);
}

static void pipe_fail_with_reason(void *fw_ctx, ldr_fw_fail_reason_t reason)
{
    ldr_fw_pipeline_t *pipe = (ldr_fw_pipeline_t *)fw_ctx;
    if (pipe == NULL) {
        return;
    }

    if (pipe->last_error == LDR_FW_PIPE_OK) {
        switch (reason) {
        case LDR_FW_FAIL_OVER_RECEIVE:
            pipe->last_error = LDR_FW_PIPE_ERR_OVER_RECEIVE;
            break;
        case LDR_FW_FAIL_UNDER_RECEIVE:
            pipe->last_error = LDR_FW_PIPE_ERR_UNDER_RECEIVE;
            break;
        case LDR_FW_FAIL_PAYLOAD_LIMIT:
            pipe->last_error = LDR_FW_PIPE_ERR_PAYLOAD_LIMIT;
            break;
        case LDR_FW_FAIL_INVALID_ARG:
            pipe->last_error = LDR_FW_PIPE_ERR_INVALID_ARG;
            break;
        case LDR_FW_FAIL_PRESERVE:
        default:
            break;
        }
    }

    abort_driver_once(pipe);
}

static const ldr_fw_backend_ops_t s_ldr_fw_pipeline_ops = {
    LDR_FW_PIPE_HEADER_MIN_LEN,
    LDR_FW_PIPE_HEADER_MAX_LEN,
    pipe_is_binding_valid,
    pipe_is_busy,
    pipe_validate_async_binding,
    pipe_begin_target,
    pipe_on_data,
    pipe_on_end,
    pipe_on_abort,
    pipe_fail_with_reason
};

int ldr_fw_pipeline_init(
    ldr_fw_pipeline_t *pipe,
    ldr_loader_t *loader,
    uint8_t *header_buf,
    size_t header_cap,
    uint8_t *residual_buf,
    size_t residual_cap)
{
    ldr_transfer_t *xfer;
    void *prev_backend_ctx;
    const ldr_fw_backend_ops_t *prev_backend_ops;
    const ecat_upgrade_async_ops_t *prev_async_ops;
    void *prev_async_ctx;

    if ((pipe == NULL) || (loader == NULL) || !loader->initialized ||
        ldr_loader_is_busy(loader) ||
        (header_buf == NULL) || (header_cap < LDR_FW_PIPE_HEADER_MIN_LEN) ||
        (residual_buf == NULL) || (residual_cap == 0U)) {
        return -1;
    }

    xfer = ldr_loader_get_transfer(loader);
    prev_backend_ctx = xfer->fw_backend_ctx;
    prev_backend_ops = xfer->fw_backend_ops;
    prev_async_ops = xfer->fw_async_ops;
    prev_async_ctx = xfer->fw_async_ctx;

    (void)memset(pipe, 0, sizeof(*pipe));
    pipe->formats = ldr_loader_get_formats(loader);
    pipe->targets = ldr_loader_get_targets(loader);
    pipe->header_buf = header_buf;
    pipe->header_cap = header_cap;
    pipe->residual_buf = residual_buf;
    pipe->residual_cap = residual_cap;
    pipe->initialized = true;

    if ((ldr_loader_bind_fw_backend(loader, pipe, &s_ldr_fw_pipeline_ops, NULL) != 0) ||
        (ldr_loader_bind_fw_async_ops(loader, &s_ldr_fw_pipeline_async_ops, pipe) != 0)) {
        xfer->fw_backend_ctx = prev_backend_ctx;
        xfer->fw_backend_ops = prev_backend_ops;
        xfer->fw_async_ops = prev_async_ops;
        xfer->fw_async_ctx = prev_async_ctx;
        pipe->initialized = false;
        return -1;
    }

    return 0;
}

int ldr_fw_pipeline_set_security(
    ldr_fw_pipeline_t *pipe,
    const ldr_fw_security_cfg_t *cfg)
{
    if ((pipe == NULL) || !pipe->initialized || pipe->session_active) {
        return -1;
    }

    if (cfg == NULL) {
        (void)memset(&pipe->security, 0, sizeof(pipe->security));
        return 0;
    }

    if (cfg->require_signature) {
        if ((cfg->verifier.begin == NULL) ||
            (cfg->verifier.update == NULL) ||
            (cfg->verifier.finish == NULL) ||
            (cfg->verifier.abort == NULL)) {
            return -1;
        }
    }

    if (cfg->enable_anti_rollback && (cfg->get_version_floor == NULL)) {
        return -1;
    }

    pipe->security = *cfg;
    return 0;
}

ldr_fw_pipeline_err_t ldr_fw_pipeline_get_last_error(const ldr_fw_pipeline_t *pipe)
{
    if (pipe == NULL) {
        return LDR_FW_PIPE_ERR_INVALID_ARG;
    }
    return pipe->last_error;
}

const ldr_format_metadata_t *ldr_fw_pipeline_get_last_metadata(const ldr_fw_pipeline_t *pipe)
{
    if ((pipe == NULL) || !pipe->header_complete) {
        return NULL;
    }
    return &pipe->meta;
}
