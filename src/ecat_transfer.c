#define LDR_NEUTRAL_BUILD 1
#include "ecat_upgrade_session_internal.h"
/**
 * @file ecat_transfer.c
 * @brief 嵌入式多通道双向传输管理器实现 (链接级零 FoE / 零 EFW 依赖)
 */

#include "ecat_transfer.h"
#include "ldr_crc32.h"
#include <string.h>

#define LDR_DEFAULT_FW_HEADER_MIN_LEN 148U

static uint32_t get_fw_header_min_len(const ecat_transfer_t *t)
{
    if ((t != NULL) && (t->fw_backend_ops != NULL) && (t->fw_backend_ops->header_min_len > 0U)) {
        return t->fw_backend_ops->header_min_len;
    }
    return LDR_DEFAULT_FW_HEADER_MIN_LEN;
}

static uint32_t get_fw_header_max_len(const ecat_transfer_t *t)
{
    if ((t != NULL) && (t->fw_backend_ops != NULL) && (t->fw_backend_ops->header_max_len > 0U)) {
        return t->fw_backend_ops->header_max_len;
    }
    return get_fw_header_min_len(t);
}

static uint32_t calc_firmware_container_limit(const ecat_transfer_t *t, const ecat_target_desc_t *target)
{
    uint64_t wide_cap = (uint64_t)target->max_size + (uint64_t)get_fw_header_max_len(t);
    return (wide_cap > (uint64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)wide_cap;
}

int ecat_transfer_init_neutral(ecat_transfer_t *t, ecat_target_registry_t *reg)
{
    if ((t == NULL) || (reg == NULL)) {
        return -1;
    }

    (void)memset(t, 0, sizeof(*t));
    t->registry = reg;
    t->core = NULL;
    t->fw_backend_ctx = NULL;
    t->fw_backend_ops = NULL;
    t->session_ptr = &t->session;
    t->last_error = ECAT_TRANSFER_ERR_NONE;
    return 0;
}

int ecat_transfer_bind_fw_backend(
    ecat_transfer_t *t,
    void *fw_ctx,
    const ldr_fw_backend_ops_t *ops,
    ecat_upgrade_session_t *shared_session)
{
    const ecat_upgrade_session_t *cur_lease;

    if ((t == NULL) || (t->registry == NULL) || (fw_ctx == NULL) || (ops == NULL)) {
        return -1;
    }
    if ((ops->begin_target == NULL) || (ops->on_data == NULL) ||
        (ops->on_end == NULL) || (ops->on_abort == NULL)) {
        return -1;
    }
    cur_lease = (t->session_ptr != NULL) ? t->session_ptr : &t->session;
    if (t->active || (cur_lease->owner != NULL)) {
        return -1;
    }
    if ((shared_session != NULL) && (shared_session->owner != NULL)) {
        return -1;
    }

    t->fw_backend_ctx = fw_ctx;
    t->fw_backend_ops = ops;
    /* 切换固件后端时同步清理旧异步操作绑定，防止残留跨后端指针 */
    t->fw_async_ops = NULL;
    t->fw_async_ctx = NULL;
    t->session_ptr = (shared_session != NULL) ? shared_session : &t->session;
    return 0;
}

int ecat_transfer_bind_fw_async_ops(
    ecat_transfer_t *t,
    const ecat_upgrade_async_ops_t *ops,
    void *async_ctx)
{
    const ecat_upgrade_session_t *lease;

    if ((t == NULL) || (t->fw_backend_ctx == NULL) || (t->fw_backend_ops == NULL) ||
        (ops == NULL) || (async_ctx == NULL)) {
        return -1;
    }
    if ((ops->start == NULL) || (ops->poll == NULL) || (ops->abort == NULL)) {
        return -1;
    }
    lease = (t->session_ptr != NULL) ? t->session_ptr : &t->session;
    if (t->active || (lease->owner != NULL)) {
        return -1;
    }
    if ((t->fw_backend_ops->validate_async_binding != NULL) &&
        !t->fw_backend_ops->validate_async_binding(t->fw_backend_ctx, ops, async_ctx)) {
        return -1;
    }
    t->fw_async_ops = ops;
    t->fw_async_ctx = async_ctx;
    return 0;
}

int ecat_transfer_set_staging_buffer(
    ecat_transfer_t *t,
    uint8_t *buf,
    size_t capacity)
{
    if ((t == NULL) || (buf == NULL) || (capacity == 0U)) {
        return -1;
    }
    if (t->pending) {
        return -1;
    }
    t->staging_buf = buf;
    t->staging_capacity = capacity;
    return 0;
}

bool ecat_transfer_is_pending(const ecat_transfer_t *t)
{
    return t != NULL && t->pending;
}

static inline ecat_upgrade_session_t *get_transfer_session(ecat_transfer_t *t)
{
    return (t != NULL && t->session_ptr != NULL) ? t->session_ptr : (t != NULL ? &t->session : NULL);
}

static inline const ecat_upgrade_session_t *get_transfer_session_const(const ecat_transfer_t *t)
{
    return (t != NULL && t->session_ptr != NULL) ? t->session_ptr : (t != NULL ? &t->session : NULL);
}

static void release_session(ecat_transfer_t *t, const void *owner, uint64_t token)
{
    if (t != NULL) {
        t->driver_invalidated = false;
        ecat_upgrade_session_t *s = get_transfer_session(t);
        if (s != NULL) {
            (void)ecat_upgrade_session_release_mgr(s, t, owner, token);
        }
    }
}

static void transfer_abort_data(ecat_transfer_t *t)
{
    if (t == NULL) {
        return;
    }
    if (t->is_async) {
        if (t->target != NULL && t->target->driver.async_ops != NULL &&
            t->target->driver.async_ops->abort != NULL) {
            t->target->driver.async_ops->abort(t->target->user_ctx);
        }
    } else {
        if (t->driver_initialized && t->target != NULL) {
            if (t->direction == ECAT_TRANSFER_DIR_EXPORT && t->target->driver.read_close != NULL) {
                t->target->driver.read_close(t->target->user_ctx);
            } else if (t->target->driver.io.abort != NULL) {
                t->target->driver.io.abort(t->target->user_ctx);
            }
        }
    }
}

static void transfer_abort_firmware(ecat_transfer_t *t)
{
    if (t == NULL) {
        return;
    }
    if (t->direction == ECAT_TRANSFER_DIR_EXPORT) {
        transfer_abort_data(t);
        return;
    }
    if (t->core_started && (t->fw_backend_ctx != NULL) &&
        (t->fw_backend_ops != NULL) && (t->fw_backend_ops->on_abort != NULL)) {
        t->fw_backend_ops->on_abort(t->fw_backend_ctx);
        t->core_started = false;
    } else if ((t->fw_async_ops != NULL) && (t->fw_async_ops->abort != NULL)) {
        t->fw_async_ops->abort(t->fw_async_ctx);
    } else if (t->driver_initialized && (t->target != NULL) && (t->target->driver.io.abort != NULL)) {
        t->target->driver.io.abort(t->target->user_ctx);
    }
}

static void transfer_fail_firmware_core_reason(ecat_transfer_t *t, ldr_fw_fail_reason_t reason)
{
    if ((t == NULL) || (t->fw_backend_ctx == NULL) ||
        (t->fw_backend_ops == NULL) || (t->fw_backend_ops->fail_with_reason == NULL)) {
        return;
    }
    t->fw_backend_ops->fail_with_reason(t->fw_backend_ctx, reason);
    t->core_started = false;
}

static void transfer_fail_firmware_core(ecat_transfer_t *t)
{
    transfer_fail_firmware_core_reason(t, LDR_FW_FAIL_PRESERVE);
}

static bool is_owned(const ecat_transfer_t *t, const void *owner, uint64_t token)
{
    return (t != NULL && ecat_upgrade_session_owns_mgr(get_transfer_session_const(t), t, owner, token));
}

static void update_export_size(ecat_transfer_t *t)
{
    if (t->target->driver.get_data_size != NULL) {
        t->total_size = t->target->driver.get_data_size(t->target->user_ctx);
        if (t->total_size > t->target->max_size) t->total_size = t->target->max_size;
    } else {
        t->total_size = t->expected_size > 0U ? t->expected_size : t->target->max_size;
    }
}

ecat_transfer_result_t ecat_transfer_begin(
    ecat_transfer_t *t,
    const void *owner,
    const ecat_transfer_request_t *request,
    uint64_t *token)
{
    if (t == NULL || owner == NULL || request == NULL || token == NULL) {
        return ECAT_TRANSFER_INVALID;
    }

    if (t->fw_backend_ctx != NULL) {
        if ((t->fw_backend_ops == NULL) ||
            (t->fw_backend_ops->is_binding_valid == NULL) ||
            !t->fw_backend_ops->is_binding_valid(t->fw_backend_ctx, t->registry, get_transfer_session(t))) {
            return ECAT_TRANSFER_INVALID;
        }
    }

    /* 在任何会话获取、驱动调用或副作用前严格校验方向枚举有效性 */
    if ((request->direction != ECAT_TRANSFER_DIR_IMPORT) &&
        (request->direction != ECAT_TRANSFER_DIR_EXPORT)) {
        return ECAT_TRANSFER_INVALID;
    }

    /* 检查固件后端是否已被其它入口占用进行升级 */
    if ((t->fw_backend_ctx != NULL) && (t->fw_backend_ops != NULL) &&
        (t->fw_backend_ops->is_busy != NULL) && t->fw_backend_ops->is_busy(t->fw_backend_ctx)) {
        return ECAT_TRANSFER_BUSY;
    }

    uint64_t acquired_token = 0U;
    ecat_upgrade_session_result_t session_res =
        ecat_upgrade_session_acquire_mgr(get_transfer_session(t), t, owner, &acquired_token);

    if (session_res == ECAT_UPGRADE_SESSION_BUSY) {
        return ECAT_TRANSFER_BUSY;
    }
    if (session_res != ECAT_UPGRADE_SESSION_OK) {
        return ECAT_TRANSFER_FAILED;
    }

    /* 查找目标区块描述符 */
    const ecat_target_desc_t *target =
        ecat_target_registry_find_by_id(t->registry, request->target_id);
    if ((target == NULL) || !target->enabled) {
        t->last_error = ECAT_TRANSFER_ERR_TARGET_NOT_FOUND;
        release_session(t, owner, acquired_token);
        return ECAT_TRANSFER_FAILED;
    }

    /* 权限检查 (ACL 与目标权限) */
    uint8_t required_perm = (request->direction == ECAT_TRANSFER_DIR_IMPORT) ?
        ECAT_PERM_WRITE : ECAT_PERM_READ;

    if (!ecat_target_registry_check_permission(
            t->registry, request->transport_id, request->target_id, required_perm)) {
        t->last_error = ECAT_TRANSFER_ERR_PERMISSION_DENIED;
        release_session(t, owner, acquired_token);
        return ECAT_TRANSFER_DENIED;
    }

    /* 物理边界防越界检查 */
    if ((request->direction == ECAT_TRANSFER_DIR_IMPORT) &&
        (target->type == ECAT_TARGET_TYPE_FIRMWARE)) {
        if (request->expected_size > 0U) {
            uint32_t limit = calc_firmware_container_limit(t, target);
            if ((request->expected_size < get_fw_header_min_len(t)) ||
                (request->expected_size > limit)) {
                t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                release_session(t, owner, acquired_token);
                return ECAT_TRANSFER_FAILED;
            }
        }
    } else {
        if (request->expected_size > target->max_size) {
            t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
            release_session(t, owner, acquired_token);
            return ECAT_TRANSFER_FAILED;
        }
    }

    /* 初始化会话参数 */
    t->target = target;
    t->direction = request->direction;
    t->expected_size = request->expected_size;
    t->current_offset = 0U;
    t->erased_watermark = 0U;
    t->active = true;
    t->driver_initialized = false;
    t->driver_invalidated = false;
    t->last_error = ECAT_TRANSFER_ERR_NONE;
    t->is_async = target->asynchronous;
    t->pending = false;
    t->stage = ECAT_TRANSFER_STAGE_NONE;
    ldr_crc32_init(&t->running_crc32);

    /* 异步驱动能力核验: 严禁假异步，缺少异步后端或 ops 时必须在任何驱动副作用前明确拒绝 */
    if (t->is_async) {
        if ((request->direction == ECAT_TRANSFER_DIR_IMPORT) && (target->type == ECAT_TARGET_TYPE_FIRMWARE)) {
            if ((t->fw_backend_ctx == NULL) || (t->fw_backend_ops == NULL) ||
                (t->fw_backend_ops->validate_async_binding == NULL) ||
                !t->fw_backend_ops->validate_async_binding(t->fw_backend_ctx, t->fw_async_ops, t->fw_async_ctx)) {
                t->last_error = ECAT_TRANSFER_ERR_CORE_UPGRADE;
                t->active = false;
                release_session(t, owner, acquired_token);
                return ECAT_TRANSFER_FAILED;
            }
        } else {
            if (target->driver.async_ops == NULL ||
                target->driver.async_ops->start == NULL ||
                target->driver.async_ops->poll == NULL ||
                target->driver.async_ops->abort == NULL) {
                t->last_error = ECAT_TRANSFER_ERR_DRIVER_INIT;
                t->active = false;
                release_session(t, owner, acquired_token);
                return ECAT_TRANSFER_FAILED;
            }
        }
    }

    /* 区分方向进行底层准备 */
    if (request->direction == ECAT_TRANSFER_DIR_EXPORT) {
        if (target->driver.io.read == NULL && (!t->is_async || target->driver.async_ops == NULL)) {
            t->last_error = ECAT_TRANSFER_ERR_DRIVER_READ;
            t->active = false;
            t->driver_initialized = false;
            release_session(t, owner, acquired_token);
            return ECAT_TRANSFER_FAILED;
        }

        /* Size belongs to the opened read snapshot, not a pre-open view. */
        t->total_size = 0U;

        if (t->is_async) {
            /* 异步 EXPORT 模式: 严格使用 async_ops 进行只读打开生命周期控制 (前置已校验 ops 完整性) */
            memset(&t->async_data_op, 0, sizeof(t->async_data_op));
            t->async_data_op.kind = ECAT_ASYNC_DATA_OP_READ_OPEN;
            ecat_async_op_result_t ares = target->driver.async_ops->start(target->user_ctx, &t->async_data_op);
            if (ares == ECAT_ASYNC_OP_PENDING) {
                t->pending = true;
                t->stage = ECAT_TRANSFER_STAGE_READ_OPEN;
                t->driver_initialized = true;
                *token = acquired_token;
                return ECAT_TRANSFER_PENDING;
            } else if (ares == ECAT_ASYNC_OP_COMPLETE) {
                t->driver_initialized = true;
                update_export_size(t);
                *token = acquired_token;
                return ECAT_TRANSFER_COMPLETE;
            } else {
                t->last_error = ECAT_TRANSFER_ERR_DRIVER_INIT;
                target->driver.async_ops->abort(target->user_ctx);
                t->active = false;
                t->driver_initialized = false;
                release_session(t, owner, acquired_token);
                return ECAT_TRANSFER_FAILED;
            }
        } else {
            /* 同步 EXPORT 模式只读生命周期安全校验:
             * 1. 允许完全无生命周期的纯 read 驱动 (read_open == NULL && read_close == NULL && io.init == NULL)
             * 2. 安全配对的只读驱动 (read_open != NULL && read_close != NULL)
             * 3. 拒绝任何未安全配对或缺少只读关闭的驱动:
             *    - read_open 与 read_close 仅提供其一 (不配对)
             *    - 需要打开资源 (io.init != NULL) 但缺少 safe read_close (防止资源泄漏或误调 abort 损坏镜像)
             */
            bool has_read_open = (target->driver.read_open != NULL);
            bool has_read_close = (target->driver.read_close != NULL);
            bool has_io_init = (target->driver.io.init != NULL);

            if (has_read_open != has_read_close) {
                t->last_error = ECAT_TRANSFER_ERR_DRIVER_INIT;
                t->active = false;
                t->driver_initialized = false;
                release_session(t, owner, acquired_token);
                return ECAT_TRANSFER_FAILED;
            }

            if (has_io_init && !has_read_close) {
                t->last_error = ECAT_TRANSFER_ERR_DRIVER_INIT;
                t->active = false;
                t->driver_initialized = false;
                release_session(t, owner, acquired_token);
                return ECAT_TRANSFER_FAILED;
            }

            if (has_read_open && has_read_close) {
                int init_rc = target->driver.read_open(target->user_ctx);
                if (init_rc != 0) {
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_INIT;
                    t->active = false;
                    t->driver_initialized = false;
                    release_session(t, owner, acquired_token);
                    return ECAT_TRANSFER_FAILED;
                }
                t->driver_initialized = true;
            } else {
                /* 纯 read 驱动，无生命周期开销 */
                t->driver_initialized = false;
            }
            update_export_size(t);
        }
    } else {
        /* IMPORT 模式 */
        if (target->type == ECAT_TARGET_TYPE_FIRMWARE) {
            ecat_transfer_err_t fw_err = ECAT_TRANSFER_ERR_CORE_UPGRADE;
            if ((t->fw_backend_ctx == NULL) || (t->fw_backend_ops == NULL) ||
                (t->fw_backend_ops->begin_target == NULL)) {
                t->last_error = ECAT_TRANSFER_ERR_CORE_UPGRADE;
                t->active = false;
                t->driver_initialized = false;
                release_session(t, owner, acquired_token);
                return ECAT_TRANSFER_FAILED;
            }

            t->total_size = request->expected_size;
            if (t->fw_backend_ops->begin_target(
                    t->fw_backend_ctx, target, request->target_id, request->fw_version, &fw_err) != 0) {
                t->last_error = fw_err;
                t->active = false;
                t->driver_initialized = false;
                release_session(t, owner, acquired_token);
                return ECAT_TRANSFER_FAILED;
            }
            t->core_started = true;
        } else {
            /* DATA 模式 */
            t->total_size = (request->expected_size > 0U) ? request->expected_size : target->max_size;

            if (t->is_async) {
                /* 异步 DATA 模式驱动初始化与介质失效化 (前置已校验 ops 完整性) */
                memset(&t->async_data_op, 0, sizeof(t->async_data_op));
                t->async_data_op.kind = ECAT_ASYNC_DATA_OP_INIT;

                ecat_async_op_result_t ares = target->driver.async_ops->start(target->user_ctx, &t->async_data_op);
                if (ares == ECAT_ASYNC_OP_PENDING) {
                    t->pending = true;
                    t->stage = ECAT_TRANSFER_STAGE_INIT;
                    t->driver_initialized = true;
                    *token = acquired_token;
                    return ECAT_TRANSFER_PENDING;
                } else if (ares == ECAT_ASYNC_OP_COMPLETE) {
                    t->driver_initialized = true;
                    /* 第一阶段 INIT 立即完成，必须立即启动第二阶段 INVALIDATE，绝不能暗中省略 */
                    memset(&t->async_data_op, 0, sizeof(t->async_data_op));
                    t->async_data_op.kind = ECAT_ASYNC_DATA_OP_INVALIDATE;
                    ecat_async_op_result_t inv_res = target->driver.async_ops->start(target->user_ctx, &t->async_data_op);
                    if (inv_res == ECAT_ASYNC_OP_PENDING) {
                        t->pending = true;
                        t->stage = ECAT_TRANSFER_STAGE_INVALIDATE;
                        *token = acquired_token;
                        return ECAT_TRANSFER_PENDING;
                    } else if (inv_res == ECAT_ASYNC_OP_COMPLETE) {
                        t->driver_invalidated = true;
                        *token = acquired_token;
                        return ECAT_TRANSFER_COMPLETE;
                    } else {
                        t->last_error = ECAT_TRANSFER_ERR_DRIVER_INVALIDATE;
                        transfer_abort_data(t);
                        t->active = false;
                        t->driver_initialized = false;
                        release_session(t, owner, acquired_token);
                        return ECAT_TRANSFER_FAILED;
                    }
                } else {
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_INIT;
                    transfer_abort_data(t);
                    t->active = false;
                    t->driver_initialized = false;
                    release_session(t, owner, acquired_token);
                    return ECAT_TRANSFER_FAILED;
                }
            } else {
                /* 同步 DATA 模式驱动初始化与失效化生命周期 */
                if (target->driver.io.init != NULL) {
                    if (target->driver.io.init(target->user_ctx) != 0) {
                        t->last_error = ECAT_TRANSFER_ERR_DRIVER_INIT;
                        t->active = false;
                        t->driver_initialized = false;
                        release_session(t, owner, acquired_token);
                        return ECAT_TRANSFER_FAILED;
                    }
                }
                t->driver_initialized = true;

                if (target->driver.io.invalidate != NULL) {
                    if (target->driver.io.invalidate(target->user_ctx) != 0) {
                        t->last_error = ECAT_TRANSFER_ERR_DRIVER_INVALIDATE;
                        if (target->driver.io.abort != NULL) {
                            target->driver.io.abort(target->user_ctx);
                        }
                        t->active = false;
                        t->driver_initialized = false;
                        release_session(t, owner, acquired_token);
                        return ECAT_TRANSFER_FAILED;
                    }
                    t->driver_invalidated = true;
                }
            }
        }
    }

    *token = acquired_token;
    return ECAT_TRANSFER_COMPLETE;
}

ecat_transfer_result_t ecat_transfer_read(
    ecat_transfer_t *t,
    const void *owner,
    uint64_t token,
    uint8_t *out_buf,
    size_t buf_capacity,
    size_t *actual_read)
{
    if (!is_owned(t, owner, token)) {
        return ECAT_TRANSFER_STALE;
    }

    if (t->pending) {
        return ECAT_TRANSFER_BUSY;
    }

    if (t->direction != ECAT_TRANSFER_DIR_EXPORT || out_buf == NULL ||
        actual_read == NULL || buf_capacity == 0U) {
        return ECAT_TRANSFER_INVALID;
    }

    *actual_read = 0U;

    /* 检查是否已达到导出终点 */
    if (t->current_offset >= t->total_size) {
        return ECAT_TRANSFER_COMPLETE;
    }

    uint32_t remaining = t->total_size - t->current_offset;
    size_t to_read_sz = ((size_t)remaining < buf_capacity) ? (size_t)remaining : buf_capacity;
    uint32_t to_read = (uint32_t)to_read_sz;

    if (t->is_async) {
        if (t->target->driver.async_ops == NULL || t->target->driver.async_ops->start == NULL) {
            t->last_error = ECAT_TRANSFER_ERR_DRIVER_READ;
            return ECAT_TRANSFER_FAILED;
        }

        memset(&t->async_data_op, 0, sizeof(t->async_data_op));
        t->async_data_op.kind = ECAT_ASYNC_DATA_OP_READ;
        t->async_data_op.offset = t->current_offset;
        t->async_data_op.length = to_read;
        t->async_data_op.buffer = out_buf;

        t->user_read_out_buf = out_buf;
        t->user_read_actual = actual_read;
        t->pending_length = to_read;

        ecat_async_op_result_t res = t->target->driver.async_ops->start(t->target->user_ctx, &t->async_data_op);
        if (res == ECAT_ASYNC_OP_PENDING) {
            t->pending = true;
            t->stage = ECAT_TRANSFER_STAGE_READ;
            return ECAT_TRANSFER_PENDING;
        } else if (res == ECAT_ASYNC_OP_COMPLETE) {
            size_t actual = t->async_data_op.actual_length;
            if (actual > (size_t)to_read) {
                t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                transfer_abort_data(t);
                t->driver_initialized = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }
            if (actual == 0U && to_read > 0U) {
                /* 契约防护: 尚有未导出数据但驱动未读取任何字节即宣告完成，判定为读失败防范死循环 */
                t->last_error = ECAT_TRANSFER_ERR_DRIVER_READ;
                transfer_abort_data(t);
                t->driver_initialized = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }
            t->current_offset += (uint32_t)actual;
            *actual_read = actual;
            if (actual > 0U) {
                ldr_crc32_update(&t->running_crc32, out_buf, (size_t)actual);
            }
            if (t->current_offset >= t->total_size) {
                return ECAT_TRANSFER_COMPLETE;
            }
            return ECAT_TRANSFER_CONTINUE;
        } else {
            t->last_error = ECAT_TRANSFER_ERR_DRIVER_READ;
            transfer_abort_data(t);
            t->driver_initialized = false;
            t->active = false;
            release_session(t, owner, token);
            return ECAT_TRANSFER_FAILED;
        }
    }

    if (t->target->driver.io.read == NULL) {
        t->last_error = ECAT_TRANSFER_ERR_DRIVER_READ;
        return ECAT_TRANSFER_FAILED;
    }

    int rc = t->target->driver.io.read(
        t->current_offset, out_buf, to_read, t->target->user_ctx);
    if (rc != 0) {
        t->last_error = ECAT_TRANSFER_ERR_DRIVER_READ;
        transfer_abort_data(t);
        t->driver_initialized = false;
        t->active = false;
        release_session(t, owner, token);
        return ECAT_TRANSFER_FAILED;
    }

    t->current_offset += to_read;
    ldr_crc32_update(&t->running_crc32, out_buf, (size_t)to_read);
    *actual_read = to_read;

    if (t->current_offset >= t->total_size) {
        return ECAT_TRANSFER_COMPLETE;
    }

    return ECAT_TRANSFER_CONTINUE;
}

ecat_transfer_result_t ecat_transfer_write(
    ecat_transfer_t *t,
    const void *owner,
    uint64_t token,
    const uint8_t *data,
    size_t length)
{
    if (!is_owned(t, owner, token)) {
        return ECAT_TRANSFER_STALE;
    }

    if (t->pending) {
        return ECAT_TRANSFER_BUSY;
    }

    if (t->direction != ECAT_TRANSFER_DIR_IMPORT) {
        return ECAT_TRANSFER_INVALID;
    }

    if (length == 0U) {
        return ECAT_TRANSFER_COMPLETE;
    }

    if (data == NULL) {
        return ECAT_TRANSFER_INVALID;
    }

    if (t->target->type == ECAT_TARGET_TYPE_FIRMWARE) {
        if ((t->fw_backend_ctx == NULL) || (t->fw_backend_ops == NULL)) {
            t->last_error = ECAT_TRANSFER_ERR_CORE_UPGRADE;
            return ECAT_TRANSFER_FAILED;
        }

        uint32_t limit = calc_firmware_container_limit(t, t->target);
        uint32_t fw_cap = (t->total_size > 0U) ? t->total_size : limit;

        if ((length > (size_t)UINT32_MAX) ||
            (t->current_offset > fw_cap) ||
            (length > (size_t)(fw_cap - t->current_offset))) {
            ldr_fw_fail_reason_t reason = ((length <= (size_t)UINT32_MAX) &&
                                           (t->current_offset <= limit) &&
                                           (length <= (size_t)(limit - t->current_offset)))
                ? LDR_FW_FAIL_OVER_RECEIVE
                : LDR_FW_FAIL_PAYLOAD_LIMIT;
            t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
            transfer_fail_firmware_core_reason(t, reason);
            t->active = false;
            release_session(t, owner, token);
            return ECAT_TRANSFER_FAILED;
        }

        if (t->is_async && (t->fw_async_ops != NULL)) {
            if ((t->staging_buf == NULL) || (length > t->staging_capacity)) {
                t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                transfer_fail_firmware_core_reason(t, LDR_FW_FAIL_INVALID_ARG);
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }
            if ((t->staging_buf != data) && (length > 0U)) {
                (void)memmove(t->staging_buf, data, length);
            }
            uint32_t candidate_crc = t->running_crc32;
            ldr_crc32_update(&candidate_crc, t->staging_buf, length);

            ecat_upgrade_async_result_t res = t->fw_async_ops->start(t->fw_async_ctx, t->staging_buf, length, true);
            if (res == ECAT_UPGRADE_ASYNC_PENDING) {
                t->pending = true;
                t->stage = ECAT_TRANSFER_STAGE_WRITE;
                t->pending_length = length;
                t->pending_crc32 = candidate_crc;
                return ECAT_TRANSFER_PENDING;
            } else if (res == ECAT_UPGRADE_ASYNC_COMPLETE) {
                t->current_offset += (uint32_t)length;
                t->running_crc32 = candidate_crc;
                return ECAT_TRANSFER_COMPLETE;
            } else {
                t->last_error = ECAT_TRANSFER_ERR_CORE_UPGRADE;
                transfer_fail_firmware_core(t);
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }
        }

        if (t->fw_backend_ops->on_data == NULL) {
            t->last_error = ECAT_TRANSFER_ERR_CORE_UPGRADE;
            t->active = false;
            release_session(t, owner, token);
            return ECAT_TRANSFER_FAILED;
        }

        uint32_t len32 = (uint32_t)length;
        ecat_transfer_err_t fw_err = ECAT_TRANSFER_ERR_CORE_UPGRADE;
        if (t->fw_backend_ops->on_data(t->fw_backend_ctx, t->target, data, len32, &fw_err) != 0) {
            t->last_error = fw_err;
            transfer_fail_firmware_core(t);
            t->active = false;
            release_session(t, owner, token);
            return ECAT_TRANSFER_FAILED;
        }

        t->current_offset += len32;
        ldr_crc32_update(&t->running_crc32, data, (size_t)len32);
    } else {
        /* DATA 普通数据流导入: 先用 size_t 与减法防环绕拒绝超界长度 */
        uint32_t write_limit = (t->expected_size > 0U) ? t->expected_size : t->target->max_size;
        if (length > (size_t)UINT32_MAX ||
            t->current_offset > write_limit ||
            length > (size_t)(write_limit - t->current_offset)) {
            t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
            transfer_abort_data(t);
            t->driver_initialized = false;
            t->active = false;
            release_session(t, owner, token);
            return ECAT_TRANSFER_FAILED;
        }

        if (t->is_async && t->target->type == ECAT_TARGET_TYPE_DATA) {
            if (t->staging_buf == NULL || length > t->staging_capacity) {
                t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                transfer_abort_data(t);
                t->driver_initialized = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }

            /* 保证每一分块仅捕获一次稳定副本至 staging_buf，防范重叠内存未定义行为及二次拷贝覆盖破坏 */
            if (t->staging_buf != data && length > 0U) {
                memmove(t->staging_buf, data, length);
            }

            /* 异步 DATA 写入前按需非阻塞擦除 */
            uint32_t len32 = (uint32_t)length;
            uint32_t need_end = t->current_offset + len32;
            if (need_end > t->erased_watermark) {
                memset(&t->async_data_op, 0, sizeof(t->async_data_op));
                t->async_data_op.kind = ECAT_ASYNC_DATA_OP_ERASE_TO;
                t->async_data_op.need_end = need_end;
                ecat_async_op_result_t eres = t->target->driver.async_ops->start(t->target->user_ctx, &t->async_data_op);
                if (eres == ECAT_ASYNC_OP_PENDING) {
                    t->pending = true;
                    t->stage = ECAT_TRANSFER_STAGE_ERASE;
                    t->pending_length = length;
                    return ECAT_TRANSFER_PENDING;
                } else if (eres == ECAT_ASYNC_OP_COMPLETE) {
                    uint32_t actual_erased = t->async_data_op.erased_end;
                    if (actual_erased < need_end) {
                        t->last_error = ECAT_TRANSFER_ERR_DRIVER_ERASE;
                        transfer_abort_data(t);
                        t->driver_initialized = false;
                        t->active = false;
                        release_session(t, owner, token);
                        return ECAT_TRANSFER_FAILED;
                    }
                    t->erased_watermark = actual_erased;
                } else {
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_ERASE;
                    transfer_abort_data(t);
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
            }

            memset(&t->async_data_op, 0, sizeof(t->async_data_op));
            t->async_data_op.kind = ECAT_ASYNC_DATA_OP_WRITE;
            t->async_data_op.offset = t->current_offset;
            t->async_data_op.length = (uint32_t)length;
            t->async_data_op.buffer = t->staging_buf;

            ecat_async_op_result_t res = t->target->driver.async_ops->start(t->target->user_ctx, &t->async_data_op);
            if (res == ECAT_ASYNC_OP_PENDING) {
                t->pending = true;
                t->stage = ECAT_TRANSFER_STAGE_WRITE;
                t->pending_length = length;
                return ECAT_TRANSFER_PENDING;
            } else if (res == ECAT_ASYNC_OP_COMPLETE) {
                size_t actual = t->async_data_op.actual_length;
                if (actual > length) {
                    t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                    transfer_abort_data(t);
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
                if (actual < length) {
                    /* 原子完整写入契约: 短写或 0 字节写入属于非法驱动行为，必须明确报错以防止数据截断丢失 */
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_WRITE;
                    transfer_abort_data(t);
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
                t->current_offset += (uint32_t)actual;
                ldr_crc32_update(&t->running_crc32, t->staging_buf, actual);
                return ECAT_TRANSFER_COMPLETE;
            } else {
                t->last_error = ECAT_TRANSFER_ERR_DRIVER_WRITE;
                transfer_abort_data(t);
                t->driver_initialized = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }
        }

        uint32_t len32 = (uint32_t)length;
        /* 按需擦除 */
        uint32_t need_end = t->current_offset + len32;
        if (need_end > t->erased_watermark) {
            uint32_t actual_erased = 0U;
            if (t->target->driver.io.erase_to != NULL) {
                int erase_rc = t->target->driver.io.erase_to(
                    need_end, &actual_erased, t->target->user_ctx);
                if (erase_rc != 0 || actual_erased < need_end) {
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_ERASE;
                    transfer_abort_data(t);
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
                t->erased_watermark = actual_erased;
            }
        }

        /* 写入介质 */
        if (t->target->driver.io.write == NULL) {
            t->last_error = ECAT_TRANSFER_ERR_DRIVER_WRITE;
            transfer_abort_data(t);
            t->driver_initialized = false;
            t->active = false;
            release_session(t, owner, token);
            return ECAT_TRANSFER_FAILED;
        }

        int write_rc = t->target->driver.io.write(
            t->current_offset, data, len32, t->target->user_ctx);
        if (write_rc != 0) {
            t->last_error = ECAT_TRANSFER_ERR_DRIVER_WRITE;
            transfer_abort_data(t);
            t->driver_initialized = false;
            t->active = false;
            release_session(t, owner, token);
            return ECAT_TRANSFER_FAILED;
        }

        t->current_offset += len32;
        ldr_crc32_update(&t->running_crc32, data, (size_t)len32);
    }

    return ECAT_TRANSFER_COMPLETE;
}

ecat_transfer_result_t ecat_transfer_finish(
    ecat_transfer_t *t,
    const void *owner,
    uint64_t token)
{
    if (!is_owned(t, owner, token)) {
        return ECAT_TRANSFER_STALE;
    }

    if (t->pending) {
        return ECAT_TRANSFER_BUSY;
    }

    if (t->direction == ECAT_TRANSFER_DIR_EXPORT && t->current_offset < t->total_size) {
        return ECAT_TRANSFER_INVALID;
    }

    if (t->is_async) {
        if (t->direction == ECAT_TRANSFER_DIR_EXPORT) {
            /* EXPORT 模式: 无论 DATA 还是 FIRMWARE 目标，导出均为只读，统一异步安全关闭 */
            if (t->target == NULL || t->target->driver.async_ops == NULL ||
                t->target->driver.async_ops->start == NULL) {
                t->last_error = ECAT_TRANSFER_ERR_DRIVER_FINALIZE;
                t->driver_initialized = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }
            memset(&t->async_data_op, 0, sizeof(t->async_data_op));
            t->async_data_op.kind = ECAT_ASYNC_DATA_OP_READ_CLOSE;
            ecat_async_op_result_t res = t->target->driver.async_ops->start(t->target->user_ctx, &t->async_data_op);
            if (res == ECAT_ASYNC_OP_PENDING) {
                t->pending = true;
                t->stage = ECAT_TRANSFER_STAGE_CLOSING;
                return ECAT_TRANSFER_PENDING;
            } else if (res == ECAT_ASYNC_OP_COMPLETE) {
                t->driver_initialized = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_COMPLETE;
            } else {
                t->last_error = ECAT_TRANSFER_ERR_DRIVER_FINALIZE;
                transfer_abort_data(t);
                t->driver_initialized = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }
        } else if (t->target->type == ECAT_TARGET_TYPE_FIRMWARE) {
            /* 异步固件导入核验 expected_size */
            if ((t->total_size > 0U) && (t->current_offset != t->total_size)) {
                t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                if (t->fw_backend_ctx != NULL) {
                    ldr_fw_fail_reason_t reason = (t->current_offset < t->total_size)
                        ? LDR_FW_FAIL_UNDER_RECEIVE
                        : LDR_FW_FAIL_OVER_RECEIVE;
                    transfer_fail_firmware_core_reason(t, reason);
                } else {
                    transfer_abort_firmware(t);
                }
                t->driver_initialized = false;
                t->driver_invalidated = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }

            if (t->fw_async_ops != NULL) {
                ecat_upgrade_async_result_t res = t->fw_async_ops->start(t->fw_async_ctx, NULL, 0U, false);
                if (res == ECAT_UPGRADE_ASYNC_PENDING) {
                    t->pending = true;
                    t->stage = ECAT_TRANSFER_STAGE_FINALIZE;
                    return ECAT_TRANSFER_PENDING;
                } else if (res == ECAT_UPGRADE_ASYNC_COMPLETE) {
                    t->core_started = false;
                    t->driver_initialized = false;
                    t->driver_invalidated = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_COMPLETE;
                } else {
                    t->last_error = ECAT_TRANSFER_ERR_CORE_UPGRADE;
                    transfer_fail_firmware_core(t);
                    t->driver_initialized = false;
                    t->driver_invalidated = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
            }
        } else {
            /* DATA 模式异步 finish */
            if (t->expected_size > 0U && t->current_offset != t->expected_size) {
                t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                transfer_abort_data(t);
                t->driver_initialized = false;
                t->driver_invalidated = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }

            memset(&t->async_data_op, 0, sizeof(t->async_data_op));
            t->async_data_op.kind = ECAT_ASYNC_DATA_OP_FINALIZE;
            ecat_async_op_result_t res = t->target->driver.async_ops->start(t->target->user_ctx, &t->async_data_op);
            if (res == ECAT_ASYNC_OP_PENDING) {
                t->pending = true;
                t->stage = ECAT_TRANSFER_STAGE_FINALIZE;
                return ECAT_TRANSFER_PENDING;
            } else if (res == ECAT_ASYNC_OP_COMPLETE) {
                /* 异步 FINALIZE 已经完成，绝不重复调用同步 io.finalize */
                t->driver_initialized = false;
                t->driver_invalidated = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_COMPLETE;
            } else {
                t->last_error = ECAT_TRANSFER_ERR_DRIVER_FINALIZE;
                transfer_abort_data(t);
                t->driver_initialized = false;
                t->driver_invalidated = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }
        }
    }

    if (t->direction == ECAT_TRANSFER_DIR_EXPORT) {
        /* EXPORT 模式: 只读传输结束，仅调用 read_close，严禁调用写提交 io.finalize */
        if (t->driver_initialized && t->target != NULL && t->target->driver.read_close != NULL) {
            t->target->driver.read_close(t->target->user_ctx);
        }
    } else {
        /* IMPORT 模式 */
        if (t->target->type == ECAT_TARGET_TYPE_FIRMWARE) {
            if ((t->total_size > 0U) && (t->current_offset != t->total_size)) {
                t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                if (t->fw_backend_ctx != NULL) {
                    ldr_fw_fail_reason_t reason = (t->current_offset < t->total_size)
                        ? LDR_FW_FAIL_UNDER_RECEIVE
                        : LDR_FW_FAIL_OVER_RECEIVE;
                    transfer_fail_firmware_core_reason(t, reason);
                }
                t->core_started = false;
                t->driver_initialized = false;
                t->driver_invalidated = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }
            if ((t->fw_backend_ctx != NULL) && (t->fw_backend_ops != NULL) &&
                (t->fw_backend_ops->on_end != NULL) && (t->fw_backend_ops->on_end(t->fw_backend_ctx) != 0)) {
                t->last_error = ECAT_TRANSFER_ERR_CORE_UPGRADE;
                transfer_fail_firmware_core(t);
                t->driver_initialized = false;
                t->driver_invalidated = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }
        } else {
            /* DATA 模式: 若声明 expected_size 则必须严格完全对齐 */
            if (t->expected_size > 0U && t->current_offset != t->expected_size) {
                t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                if (t->driver_initialized && t->target->driver.io.abort != NULL) {
                    t->target->driver.io.abort(t->target->user_ctx);
                }
                t->core_started = false;
                t->driver_initialized = false;
                t->driver_invalidated = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }

            if (t->target->driver.io.finalize != NULL) {
                if (t->target->driver.io.finalize(t->target->user_ctx) != 0) {
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_FINALIZE;
                    if (t->driver_initialized && t->target->driver.io.abort != NULL) {
                        t->target->driver.io.abort(t->target->user_ctx);
                    }
                    t->core_started = false;
                    t->driver_initialized = false;
                    t->driver_invalidated = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
            }
        }
    }

    t->core_started = false;
    t->driver_initialized = false;
    t->driver_invalidated = false;
    t->active = false;
    release_session(t, owner, token);
    return ECAT_TRANSFER_COMPLETE;
}

ecat_transfer_result_t ecat_transfer_cancel(
    ecat_transfer_t *t,
    const void *owner,
    uint64_t token)
{
    if (!is_owned(t, owner, token)) {
        return ECAT_TRANSFER_STALE;
    }

    if (t->target != NULL && t->target->type == ECAT_TARGET_TYPE_FIRMWARE) {
        transfer_abort_firmware(t);
    } else {
        transfer_abort_data(t);
    }
    t->pending = false;
    t->stage = ECAT_TRANSFER_STAGE_NONE;

    t->core_started = false;
    t->driver_initialized = false;
    t->driver_invalidated = false;
    t->active = false;
    release_session(t, owner, token);
    return ECAT_TRANSFER_COMPLETE;
}

ecat_transfer_result_t ecat_transfer_poll(
    ecat_transfer_t *t,
    const void *owner,
    uint64_t token)
{
    if (!is_owned(t, owner, token)) {
        return ECAT_TRANSFER_STALE;
    }

    if (!t->pending) {
        return ECAT_TRANSFER_INVALID;
    }

    if (t->direction == ECAT_TRANSFER_DIR_IMPORT && t->target->type == ECAT_TARGET_TYPE_FIRMWARE) {
        if (t->fw_async_ops == NULL || t->fw_async_ops->poll == NULL) {
            return ECAT_TRANSFER_INVALID;
        }

        ecat_upgrade_async_result_t res = t->fw_async_ops->poll(t->fw_async_ctx);
        if (res == ECAT_UPGRADE_ASYNC_PENDING) {
            return ECAT_TRANSFER_PENDING;
        } else if (res == ECAT_UPGRADE_ASYNC_COMPLETE) {
            t->pending = false;
            if (t->stage == ECAT_TRANSFER_STAGE_FINALIZE) {
                t->active = false;
                t->core_started = false;
                t->driver_initialized = false;
                t->stage = ECAT_TRANSFER_STAGE_NONE;
                release_session(t, owner, token);
                return ECAT_TRANSFER_COMPLETE;
            } else {
                t->current_offset += (uint32_t)t->pending_length;
                t->running_crc32 = t->pending_crc32;
                t->stage = ECAT_TRANSFER_STAGE_NONE;
                return ECAT_TRANSFER_COMPLETE;
            }
        } else {
            t->pending = false;
            t->stage = ECAT_TRANSFER_STAGE_NONE;
            t->last_error = ECAT_TRANSFER_ERR_CORE_UPGRADE;
            transfer_fail_firmware_core(t);
            t->driver_initialized = false;
            t->active = false;
            release_session(t, owner, token);
            return ECAT_TRANSFER_FAILED;
        }
    } else {
        /* DATA 目标异步轮询 */
        if (t->target->driver.async_ops == NULL || t->target->driver.async_ops->poll == NULL) {
            return ECAT_TRANSFER_INVALID;
        }

        ecat_async_op_result_t res = t->target->driver.async_ops->poll(t->target->user_ctx, &t->async_data_op);
        if (res == ECAT_ASYNC_OP_PENDING) {
            return ECAT_TRANSFER_PENDING;
        } else if (res == ECAT_ASYNC_OP_COMPLETE) {
            if (t->stage == ECAT_TRANSFER_STAGE_INIT) {
                /* INIT 轮询完成，无缝启动第二阶段 INVALIDATE */
                t->driver_initialized = true;
                memset(&t->async_data_op, 0, sizeof(t->async_data_op));
                t->async_data_op.kind = ECAT_ASYNC_DATA_OP_INVALIDATE;
                ecat_async_op_result_t inv_res = t->target->driver.async_ops->start(t->target->user_ctx, &t->async_data_op);
                if (inv_res == ECAT_ASYNC_OP_PENDING) {
                    t->stage = ECAT_TRANSFER_STAGE_INVALIDATE;
                    return ECAT_TRANSFER_PENDING;
                } else if (inv_res == ECAT_ASYNC_OP_COMPLETE) {
                    t->pending = false;
                    t->stage = ECAT_TRANSFER_STAGE_NONE;
                    t->driver_invalidated = true;
                    return ECAT_TRANSFER_COMPLETE;
                } else {
                    t->pending = false;
                    t->stage = ECAT_TRANSFER_STAGE_NONE;
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_INVALIDATE;
                    transfer_abort_data(t);
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
            } else if (t->stage == ECAT_TRANSFER_STAGE_INVALIDATE) {
                /* INVALIDATE 轮询完成 */
                t->pending = false;
                t->stage = ECAT_TRANSFER_STAGE_NONE;
                t->driver_invalidated = true;
                return ECAT_TRANSFER_COMPLETE;
            } else if (t->stage == ECAT_TRANSFER_STAGE_READ_OPEN) {
                /* 异步 READ_OPEN 轮询完成 */
                t->pending = false;
                t->stage = ECAT_TRANSFER_STAGE_NONE;
                t->driver_initialized = true;
                update_export_size(t);
                return ECAT_TRANSFER_COMPLETE;
            } else if (t->stage == ECAT_TRANSFER_STAGE_ERASE) {
                uint32_t actual_erased = t->async_data_op.erased_end;
                uint32_t need = t->current_offset + (uint32_t)t->pending_length;
                if (actual_erased < need) {
                    t->pending = false;
                    t->stage = ECAT_TRANSFER_STAGE_NONE;
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_ERASE;
                    transfer_abort_data(t);
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
                t->erased_watermark = actual_erased;

                /* 擦除已轮询完成，状态机无缝启动第二阶段 WRITE */
                memset(&t->async_data_op, 0, sizeof(t->async_data_op));
                t->async_data_op.kind = ECAT_ASYNC_DATA_OP_WRITE;
                t->async_data_op.offset = t->current_offset;
                t->async_data_op.length = (uint32_t)t->pending_length;
                t->async_data_op.buffer = t->staging_buf;
                ecat_async_op_result_t wres = t->target->driver.async_ops->start(t->target->user_ctx, &t->async_data_op);
                if (wres == ECAT_ASYNC_OP_PENDING) {
                    t->stage = ECAT_TRANSFER_STAGE_WRITE;
                    return ECAT_TRANSFER_PENDING;
                } else if (wres == ECAT_ASYNC_OP_COMPLETE) {
                    t->pending = false;
                    size_t actual = t->async_data_op.actual_length;
                    if (actual > (size_t)t->pending_length) {
                        t->stage = ECAT_TRANSFER_STAGE_NONE;
                        t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                        transfer_abort_data(t);
                        t->driver_initialized = false;
                        t->active = false;
                        release_session(t, owner, token);
                        return ECAT_TRANSFER_FAILED;
                    }
                    if (actual < (size_t)t->pending_length) {
                        t->stage = ECAT_TRANSFER_STAGE_NONE;
                        t->last_error = ECAT_TRANSFER_ERR_DRIVER_WRITE;
                        transfer_abort_data(t);
                        t->driver_initialized = false;
                        t->active = false;
                        release_session(t, owner, token);
                        return ECAT_TRANSFER_FAILED;
                    }
                    t->current_offset += (uint32_t)actual;
                    ldr_crc32_update(&t->running_crc32, t->staging_buf, actual);
                    t->stage = ECAT_TRANSFER_STAGE_NONE;
                    return ECAT_TRANSFER_COMPLETE;
                } else {
                    t->pending = false;
                    t->stage = ECAT_TRANSFER_STAGE_NONE;
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_WRITE;
                    transfer_abort_data(t);
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
            } else if (t->stage == ECAT_TRANSFER_STAGE_WRITE) {
                t->pending = false;
                size_t actual = t->async_data_op.actual_length;
                if (actual > (size_t)t->pending_length) {
                    t->stage = ECAT_TRANSFER_STAGE_NONE;
                    t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                    transfer_abort_data(t);
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
                if (actual < (size_t)t->pending_length) {
                    t->stage = ECAT_TRANSFER_STAGE_NONE;
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_WRITE;
                    transfer_abort_data(t);
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
                t->current_offset += (uint32_t)actual;
                ldr_crc32_update(&t->running_crc32, t->staging_buf, actual);
                t->stage = ECAT_TRANSFER_STAGE_NONE;
                return ECAT_TRANSFER_COMPLETE;
            } else if (t->stage == ECAT_TRANSFER_STAGE_READ) {
                t->pending = false;
                size_t actual = t->async_data_op.actual_length;
                if (actual > (size_t)t->pending_length) {
                    t->stage = ECAT_TRANSFER_STAGE_NONE;
                    t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                    transfer_abort_data(t);
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
                if (actual == 0U && t->pending_length > 0U) {
                    /* 契约防护: 异步 poll 推进完成但读取字节为 0，判定为读失败防范死循环 */
                    t->stage = ECAT_TRANSFER_STAGE_NONE;
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_READ;
                    transfer_abort_data(t);
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
                t->current_offset += (uint32_t)actual;
                if (t->user_read_actual != NULL) {
                    *t->user_read_actual = actual;
                }
                if (actual > 0U && t->user_read_out_buf != NULL) {
                    ldr_crc32_update(&t->running_crc32, t->user_read_out_buf, actual);
                }
                t->stage = ECAT_TRANSFER_STAGE_NONE;
                if (t->current_offset >= t->total_size) {
                    return ECAT_TRANSFER_COMPLETE;
                }
                return ECAT_TRANSFER_CONTINUE;
            } else if (t->stage == ECAT_TRANSFER_STAGE_FINALIZE || t->stage == ECAT_TRANSFER_STAGE_CLOSING) {
                /* 异步 FINALIZE 或 READ_CLOSE 轮询完成，绝不重复调用同步 io.finalize */
                t->pending = false;
                t->stage = ECAT_TRANSFER_STAGE_NONE;
                t->driver_initialized = false;
                t->driver_invalidated = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_COMPLETE;
            } else {
                t->pending = false;
                t->stage = ECAT_TRANSFER_STAGE_NONE;
                return ECAT_TRANSFER_COMPLETE;
            }
        } else {
            t->pending = false;
            ecat_transfer_stage_t failed_stage = t->stage;
            t->stage = ECAT_TRANSFER_STAGE_NONE;
            if (failed_stage == ECAT_TRANSFER_STAGE_INIT || failed_stage == ECAT_TRANSFER_STAGE_READ_OPEN) {
                t->last_error = ECAT_TRANSFER_ERR_DRIVER_INIT;
            } else if (failed_stage == ECAT_TRANSFER_STAGE_INVALIDATE) {
                t->last_error = ECAT_TRANSFER_ERR_DRIVER_INVALIDATE;
            } else if (failed_stage == ECAT_TRANSFER_STAGE_ERASE) {
                t->last_error = ECAT_TRANSFER_ERR_DRIVER_ERASE;
            } else if (failed_stage == ECAT_TRANSFER_STAGE_READ) {
                t->last_error = ECAT_TRANSFER_ERR_DRIVER_READ;
            } else if (failed_stage == ECAT_TRANSFER_STAGE_FINALIZE || failed_stage == ECAT_TRANSFER_STAGE_CLOSING) {
                t->last_error = ECAT_TRANSFER_ERR_DRIVER_FINALIZE;
            } else {
                t->last_error = ECAT_TRANSFER_ERR_DRIVER_WRITE;
            }
            transfer_abort_data(t);
            t->driver_initialized = false;
            t->driver_invalidated = false;
            t->active = false;
            release_session(t, owner, token);
            return ECAT_TRANSFER_FAILED;
        }
    }
}

uint32_t ecat_transfer_get_crc32(const ecat_transfer_t *t)
{
    if (t == NULL) {
        return 0U;
    }
    return ldr_crc32_final(&t->running_crc32);
}

uint32_t ecat_transfer_get_bytes_transferred(const ecat_transfer_t *t)
{
    if (t == NULL) {
        return 0U;
    }
    return t->current_offset;
}

uint32_t ecat_transfer_get_total_export_size(const ecat_transfer_t *t)
{
    if (t == NULL) {
        return 0U;
    }
    return t->total_size;
}

ecat_transfer_err_t ecat_transfer_get_last_error(const ecat_transfer_t *t)
{
    if (t == NULL) {
        return ECAT_TRANSFER_ERR_INVALID_ARG;
    }
    return t->last_error;
}
