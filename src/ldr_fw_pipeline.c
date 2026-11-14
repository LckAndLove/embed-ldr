/**
 * @file ldr_fw_pipeline.c
 * @brief 格式插件驱动的中性固件刷写流水线实现 (零 FoE / 零 EFW 硬编码依赖)
 */

#define LDR_NEUTRAL_BUILD 1
#include "ldr_fw_pipeline.h"
#include "ldr_crc32.h"
#include <string.h>

#define LDR_FW_PIPE_READBACK_CHUNK_LEN 64U
#define LDR_FW_PIPE_HEADER_MIN_LEN     4U
#define LDR_FW_PIPE_HEADER_MAX_LEN     512U

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
}

static void abort_driver_once(ldr_fw_pipeline_t *pipe)
{
    if (pipe == NULL) {
        return;
    }
    if (pipe->driver_initialized && !pipe->driver_finalized && !pipe->driver_aborted) {
        if ((pipe->active_target != NULL) && (pipe->active_target->driver.io.abort != NULL)) {
            pipe->active_target->driver.io.abort(pipe->active_target->user_ctx);
        }
        pipe->driver_aborted = true;
    }
    pipe->session_active = false;
}

static int flush_aligned_block(ldr_fw_pipeline_t *pipe, const ldr_target_desc_t *target)
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
    (void)fw_ctx;
    (void)async_ops;
    (void)async_ctx;
    return false;
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
        target->asynchronous ||
        (target->max_size == 0U) ||
        (target->write_unit == 0U) ||
        ((size_t)target->write_unit > pipe->residual_cap) ||
        (target->driver.io.init == NULL) ||
        (target->driver.io.invalidate == NULL) ||
        (target->driver.io.erase_to == NULL) ||
        (target->driver.io.write == NULL) ||
        (target->driver.io.read == NULL) ||
        (target->driver.io.finalize == NULL) ||
        (target->driver.io.abort == NULL)) {
        pipe->last_error = LDR_FW_PIPE_ERR_TARGET_INVALID;
        if (out_err != NULL) {
            *out_err = ECAT_TRANSFER_ERR_DRIVER_WRITE;
        }
        return -1;
    }

    reset_session_fields(pipe);
    pipe->active_target = target;
    pipe->req_target_id = target_id;
    pipe->req_fw_version = fw_version;
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
    uint32_t remaining = len;

    if ((pipe == NULL) || !pipe->session_active || (target == NULL) ||
        (pipe->active_target != target) || ((remaining > 0U) && (cursor == NULL))) {
        if (out_err != NULL) {
            *out_err = ECAT_TRANSFER_ERR_CORE_UPGRADE;
        }
        return -1;
    }

    if (!pipe->header_complete) {
        size_t prev_hdr_len = pipe->header_rx_len;
        size_t space_left = pipe->header_cap - prev_hdr_len;
        size_t to_copy = ((size_t)remaining < space_left) ? (size_t)remaining : space_left;
        uint16_t detected_hdr_len = 0U;
        ldr_format_metadata_t parsed_meta;
        ldr_format_probe_result_t pres;

        if (to_copy > 0U) {
            (void)memcpy(&pipe->header_buf[prev_hdr_len], cursor, to_copy);
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

        /* 头部与元数据全量核验通过，开始物理驱动生命周期 */
        if (target->driver.io.init(target->user_ctx) != 0) {
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

        pipe->meta = parsed_meta;
        pipe->header_complete = true;
        ldr_crc32_init(&pipe->stream_crc32);

        /* 跳过本块中属于头部的字节，余下字节直接进入载荷刷写 */
        size_t consumed = (size_t)parsed_meta.header_size - prev_hdr_len;
        cursor += consumed;
        remaining -= (uint32_t)consumed;
    }

    if (remaining == 0U) {
        return 0;
    }

    if (remaining > (pipe->meta.payload_size - pipe->payload_rx_bytes)) {
        pipe->last_error = LDR_FW_PIPE_ERR_OVER_RECEIVE;
        if (out_err != NULL) {
            *out_err = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
        }
        return -1;
    }

    ldr_crc32_update(&pipe->stream_crc32, cursor, (size_t)remaining);
    pipe->payload_rx_bytes += remaining;

    while (remaining > 0U) {
        size_t unit = (size_t)target->write_unit;
        size_t need = unit - pipe->residual_len;
        size_t take = ((size_t)remaining < need) ? (size_t)remaining : need;

        (void)memcpy(&pipe->residual_buf[pipe->residual_len], cursor, take);
        pipe->residual_len += take;
        cursor += take;
        remaining -= (uint32_t)take;

        if (pipe->residual_len == unit) {
            if (flush_aligned_block(pipe, target) != 0) {
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

    if ((pipe == NULL) || !pipe->session_active || (pipe->active_target == NULL)) {
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
        if (flush_aligned_block(pipe, target) != 0) {
            return -1;
        }
    }

    stream_crc = ldr_crc32_final(&pipe->stream_crc32);
    if ((pipe->meta.payload_crc32 != 0U) && (stream_crc != pipe->meta.payload_crc32)) {
        pipe->last_error = LDR_FW_PIPE_ERR_STREAM_CRC;
        return -1;
    }

    /* 物理介质回读 CRC32 二次核验 (仅覆盖有效载荷字节，不含尾块 0xFF 对齐填充) */
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

    if (target->driver.io.finalize(target->user_ctx) != 0) {
        pipe->last_error = LDR_FW_PIPE_ERR_DRIVER_FINALIZE;
        return -1;
    }

    pipe->driver_finalized = true;
    pipe->session_active = false;
    return 0;
}

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
    if ((pipe == NULL) || (loader == NULL) || !loader->initialized ||
        (header_buf == NULL) || (header_cap < LDR_FW_PIPE_HEADER_MIN_LEN) ||
        (residual_buf == NULL) || (residual_cap == 0U)) {
        return -1;
    }

    (void)memset(pipe, 0, sizeof(*pipe));
    pipe->formats = ldr_loader_get_formats(loader);
    pipe->targets = ldr_loader_get_targets(loader);
    pipe->header_buf = header_buf;
    pipe->header_cap = header_cap;
    pipe->residual_buf = residual_buf;
    pipe->residual_cap = residual_cap;
    pipe->initialized = true;

    if (ldr_loader_bind_fw_backend(loader, pipe, &s_ldr_fw_pipeline_ops, NULL) != 0) {
        pipe->initialized = false;
        return -1;
    }

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
