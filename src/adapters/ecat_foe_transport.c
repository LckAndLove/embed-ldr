#include "../ecat_upgrade_session_internal.h"
#include "../ecat_foe_core_internal.h"
/* FoE bridge to the unified transfer manager. */
#include "ecat_foe_transport.h"
#include "ecat_transport_endpoint.h"
#include <string.h>

int ecat_foe_transport_init_transfer(ecat_foe_transport_t *t, ecat_transfer_t *transfer)
{
    if (t == NULL || transfer == NULL || transfer->core == NULL) return -1;
    memset(t, 0, sizeof(*t));
    t->transfer = transfer;
    return 0;
}

int ecat_foe_transport_set_staged_buffer(ecat_foe_transport_t *t, uint8_t *buffer, size_t capacity)
{
    if (t == NULL) return -1;
    /* 处于会话活跃或任何挂起状态时，严禁更换缓冲区，杜绝中途悬空与竞态 */
    if (ecat_foe_transport_owns(t) || t->pending || t->begin_pending || t->has_staged_packet) return -1;
    if (buffer == NULL && capacity != 0U) return -1;
    t->staged_buf = buffer;
    t->staged_cap = capacity;
    t->staged_shared = false;
    return 0;
}

int ecat_foe_transport_set_shared_staged_buffer(ecat_foe_transport_t *t, uint8_t *buffer, size_t capacity)
{
    if (t == NULL) return -1;
    if (ecat_foe_transport_owns(t) || t->pending || t->begin_pending || t->has_staged_packet) return -1;
    if (buffer == NULL && capacity != 0U) return -1;
    t->staged_buf = buffer;
    t->staged_cap = capacity;
    t->staged_shared = (buffer != NULL);
    return 0;
}

static ecat_upgrade_session_t *get_transport_session(const ecat_foe_transport_t *t)
{
    return t->transfer != NULL ? t->transfer->session_ptr : NULL;
}

static const void *get_manager_ptr(const ecat_foe_transport_t *t)
{
    return t->transfer;
}

static ecat_foe_core_t *get_transport_core(const ecat_foe_transport_t *t)
{
    return t->transfer != NULL ? t->transfer->core : NULL;
}

bool ecat_foe_transport_owns(const ecat_foe_transport_t *t)
{
    if (t == NULL) return false;
    ecat_upgrade_session_t *sess = get_transport_session(t);
    const void *mgr = get_manager_ptr(t);
    if (sess == NULL || mgr == NULL) return false;
    return ecat_upgrade_session_owns_mgr(sess, mgr, t, t->token);
}

static void notify_session_closed(ecat_foe_transport_t *t)
{
    if (t->on_session_closed != NULL) {
        t->on_session_closed(t->session_closed_ctx);
    }
}

static void mark_committed(ecat_foe_transport_t *t)
{
    t->committed = true;
    notify_session_closed(t);
}

void ecat_foe_transport_abort(void *ctx)
{
    ecat_foe_transport_t *t = (ecat_foe_transport_t *)ctx;
    if (t == NULL) return;
    if (ecat_foe_transport_owns(t)) {
        (void)ecat_transfer_cancel(t->transfer, t, t->token);
    }
    t->direction = (ecat_transfer_dir_t)0;
    t->export_zero_eof = false;
    t->pending = false;
    t->begin_pending = false;
    t->finish_after_write = false;
    t->finishing = false;
    /* 仅当首包暂存区为独立独占缓冲区时清零实际暂存的字节数；
     * 若为外部异步门控共享的 packet_storage，严禁在此清零，
     * 必须保留原始数据供门控进行超时重传比对 (packet_matches)，直到门控握手闭环 (clear_packet) */
    if (t->has_staged_packet && t->staged_buf != NULL && t->staged_length > 0U && !t->staged_shared) {
        memset(t->staged_buf, 0, t->staged_length);
    }
    t->has_staged_packet = false;
    t->staged_length = 0U;
    t->staged_more_data = false;
    notify_session_closed(t);
}

int ecat_foe_transport_begin(ecat_foe_transport_t *t, const char *name, size_t length)
{
    if (t == NULL || t->transfer == NULL || name == NULL) {
        return ECAT_FOE_ERRCODE_ILLEGAL;
    }
    ecat_upgrade_session_t *sess = get_transport_session(t);
    if (sess != NULL && sess->owner != NULL && !ecat_foe_transport_owns(t)) {
        return ECAT_FOE_ERRCODE_NOTDEFINED;
    }
    ecat_foe_transport_abort(t);
    t->committed = false;
    ecat_foe_core_t *core = get_transport_core(t);
    if (core == NULL) return ECAT_FOE_ERRCODE_NOTDEFINED;

    uint16_t target_id = 0;
    uint32_t fw_version = 0;
    int result = ecat_foe_core_validate_wrq(core, name, length, &target_id, &fw_version);
    if (result != 0) return result;

    ecat_transfer_request_t req;
    memset(&req, 0, sizeof(req));
    req.transport_id = ECAT_TRANSPORT_ID_FOE; /* FoE 通道 ID */
    req.target_id = target_id;
    req.direction = ECAT_TRANSFER_DIR_IMPORT;
    req.expected_size = 0U;
    req.fw_version = fw_version;

    ecat_transfer_result_t tres = ecat_transfer_begin(t->transfer, t, &req, &t->token);
    if (tres == ECAT_TRANSFER_PENDING) {
        t->direction = ECAT_TRANSFER_DIR_IMPORT;
        t->export_zero_eof = false;
        t->pending = true;
        t->begin_pending = true;
        return 0;
    } else if (tres == ECAT_TRANSFER_COMPLETE) {
        t->direction = ECAT_TRANSFER_DIR_IMPORT;
        t->export_zero_eof = false;
        t->pending = false;
        t->begin_pending = false;
        return 0;
    } else {
        return ECAT_FOE_ERRCODE_NOTDEFINED;
    }
}

static bool parse_canonical_target_token(const char *tok, size_t tok_len, uint16_t *out_tid)
{
    if (tok == NULL || out_tid == NULL || tok_len == 0U) {
        return false;
    }
    if (tok_len == 3U && memcmp(tok, "MCU", 3U) == 0) {
        *out_tid = ECAT_FOE_TARGET_MCU;
        return true;
    }
    if (tok_len == 4U && memcmp(tok, "FPGA", 4U) == 0) {
        *out_tid = ECAT_FOE_TARGET_FPGA;
        return true;
    }
    if (tok_len == 3U && memcmp(tok, "SII", 3U) == 0) {
        *out_tid = ECAT_FOE_TARGET_SII;
        return true;
    }
    if (tok_len == 6U && memcmp(tok, "EEPROM", 6U) == 0) {
        *out_tid = ECAT_FOE_TARGET_EEPROM;
        return true;
    }
    if (tok_len == 5U && tok[0] == 'T') {
        uint32_t parsed_hex = 0U;
        for (size_t i = 1U; i < 5U; i++) {
            char c = tok[i];
            uint32_t nibble = 0U;
            if (c >= '0' && c <= '9') {
                nibble = (uint32_t)(c - '0');
            } else if (c >= 'A' && c <= 'F') {
                nibble = (uint32_t)(c - 'A') + 10U;
            } else {
                return false;
            }
            parsed_hex = (parsed_hex << 4) | nibble;
        }
        if (parsed_hex >= 5U && parsed_hex <= 0xFFFFU) {
            *out_tid = (uint16_t)parsed_hex;
            return true;
        }
    }
    return false;
}

int ecat_foe_transport_resolve_read_target(const ecat_foe_transport_t *t,
                                           const char *name,
                                           size_t length,
                                           uint16_t *out_target_id)
{
    if (t == NULL || t->transfer == NULL || t->transfer->registry == NULL ||
        name == NULL || out_target_id == NULL) {
        return ECAT_FOE_ERRCODE_ILLEGAL;
    }
    if (length == 0U || length > ECAT_FOE_MAX_FILENAME_LEN ||
        memchr(name, '\0', length) != NULL) {
        return ECAT_FOE_ERRCODE_ILLEGAL;
    }

    char safe_name[ECAT_FOE_MAX_FILENAME_LEN + 1U];
    memcpy(safe_name, name, length);
    safe_name[length] = '\0';

    /* 1. 优先精确匹配注册表 target->name (适用于日志、参数、自定义目标等) */
    const ecat_target_desc_t *desc =
        ecat_target_registry_find_by_name(t->transfer->registry, safe_name);
    if (desc != NULL) {
        *out_target_id = desc->target_id;
        return 0;
    }

    /* 2. 兼容规范前缀 <PROJECT>_<TARGET> (MCU/FPGA/SII/EEPROM/T0005..TFFFF) */
    const ecat_foe_core_t *core = get_transport_core(t);
    if (core != NULL && core->is_configured) {
        const char *proj = ecat_foe_core_get_project_name(core);
        if (proj != NULL) {
            size_t proj_len = strlen(proj);
            if (proj_len > 0U && length > proj_len + 1U &&
                memcmp(safe_name, proj, proj_len) == 0 &&
                safe_name[proj_len] == '_') {
                uint16_t tid = 0U;
                if (parse_canonical_target_token(safe_name + proj_len + 1U,
                                                 length - proj_len - 1U,
                                                 &tid)) {
                    if (ecat_target_registry_find_by_id(t->transfer->registry, tid) != NULL) {
                        *out_target_id = tid;
                        return 0;
                    }
                }
            }
        }
    }

    return ECAT_FOE_ERRCODE_NOTFOUND;
}

int ecat_foe_transport_begin_read(ecat_foe_transport_t *t,
                                  const char *name,
                                  size_t length,
                                  uint32_t *out_total_size)
{
    if (t == NULL || t->transfer == NULL || t->transfer->registry == NULL || name == NULL) {
        return ECAT_FOE_ERRCODE_ILLEGAL;
    }
    ecat_upgrade_session_t *sess = get_transport_session(t);
    if (sess != NULL && sess->owner != NULL && !ecat_foe_transport_owns(t)) {
        return ECAT_FOE_ERRCODE_NOTDEFINED;
    }
    ecat_foe_transport_abort(t);
    t->committed = false;

    uint16_t target_id = 0U;
    int rrc = ecat_foe_transport_resolve_read_target(t, name, length, &target_id);
    if (rrc != 0) {
        return rrc;
    }

    const ecat_target_desc_t *desc =
        ecat_target_registry_find_by_id(t->transfer->registry, target_id);
    if (desc == NULL || !desc->enabled) {
        return ECAT_FOE_ERRCODE_NOTFOUND;
    }
    if (desc->asynchronous) {
        return ECAT_FOE_ERRCODE_ILLEGAL;
    }

    /* 安全红线: 严禁导出处于未闭环升级态 (已 invalidate 但未 finalize) 的固件半成品 */
    const ecat_foe_core_t *core = get_transport_core(t);
    if (desc->type == ECAT_TARGET_TYPE_FIRMWARE && core != NULL &&
        ecat_foe_core_target_upgrade_incomplete(core, target_id)) {
        return ECAT_FOE_ERRCODE_ACCESS;
    }

    ecat_transfer_request_t req;
    memset(&req, 0, sizeof(req));
    req.transport_id = ECAT_TRANSPORT_ID_FOE;
    req.target_id = target_id;
    req.direction = ECAT_TRANSFER_DIR_EXPORT;
    req.expected_size = 0U;
    req.fw_version = 0U;

    ecat_transfer_result_t tres = ecat_transfer_begin(t->transfer, t, &req, &t->token);
    if (tres == ECAT_TRANSFER_COMPLETE) {
        t->direction = ECAT_TRANSFER_DIR_EXPORT;
        t->export_zero_eof = false;
        t->pending = false;
        t->begin_pending = false;
        if (out_total_size != NULL) {
            *out_total_size = ecat_transfer_get_total_export_size(t->transfer);
        }
        return 0;
    }
    if (tres == ECAT_TRANSFER_PENDING) {
        /* 同步 FoE RRQ 不支持异步挂起导出，安全中止释放租约 */
        (void)ecat_transfer_cancel(t->transfer, t, t->token);
        return ECAT_FOE_ERRCODE_ILLEGAL;
    }
    if (tres == ECAT_TRANSFER_DENIED) {
        return ECAT_FOE_ERRCODE_ACCESS;
    }
    if (ecat_transfer_get_last_error(t->transfer) == ECAT_TRANSFER_ERR_TARGET_NOT_FOUND) {
        return ECAT_FOE_ERRCODE_NOTFOUND;
    }
    return ECAT_FOE_ERRCODE_NOTDEFINED;
}

int ecat_foe_transport_read(ecat_foe_transport_t *t,
                            uint8_t *out_buf,
                            size_t max_length,
                            size_t *out_length)
{
    if (t == NULL || out_length == NULL || !ecat_foe_transport_owns(t) ||
        t->direction != ECAT_TRANSFER_DIR_EXPORT) {
        return ECAT_FOE_ERRCODE_ILLEGAL;
    }
    *out_length = 0U;

    /* 整包对齐后的 0 字节 EOF 结束帧 */
    if (t->export_zero_eof) {
        t->export_zero_eof = false;
        ecat_transfer_result_t fres = ecat_transfer_finish(t->transfer, t, t->token);
        if (fres == ECAT_TRANSFER_COMPLETE) {
            t->direction = (ecat_transfer_dir_t)0;
            mark_committed(t);
            return 0;
        }
        ecat_foe_transport_abort(t);
        return ECAT_FOE_ERRCODE_PROGERROR;
    }

    if (out_buf == NULL || max_length == 0U) {
        ecat_foe_transport_abort(t);
        return ECAT_FOE_ERRCODE_ILLEGAL;
    }

    size_t actual = 0U;
    ecat_transfer_result_t rres = ecat_transfer_read(
        t->transfer, t, t->token, out_buf, max_length, &actual);
    if (rres == ECAT_TRANSFER_CONTINUE) {
        *out_length = actual;
        return 0;
    }
    if (rres == ECAT_TRANSFER_COMPLETE) {
        *out_length = actual;
        if (actual == max_length) {
            /* 数据总长恰好为单包整数倍：保留会话待下一次拉取发送 0 字节 EOF 包 */
            t->export_zero_eof = true;
            return 0;
        }
        ecat_transfer_result_t fres = ecat_transfer_finish(t->transfer, t, t->token);
        if (fres == ECAT_TRANSFER_COMPLETE) {
            t->direction = (ecat_transfer_dir_t)0;
            mark_committed(t);
            return 0;
        }
        ecat_foe_transport_abort(t);
        return ECAT_FOE_ERRCODE_PROGERROR;
    }

    ecat_foe_transport_abort(t);
    return ECAT_FOE_ERRCODE_PROGERROR;
}

ecat_upgrade_async_result_t ecat_foe_transport_start(void *ctx, const uint8_t *data,
    size_t length, bool more_data)
{
    ecat_foe_transport_t *t = (ecat_foe_transport_t *)ctx;
    if (t == NULL || !ecat_foe_transport_owns(t)) return ECAT_UPGRADE_ASYNC_FAILED;

    if (t->begin_pending) {

        ecat_transfer_result_t poll_res = ecat_transfer_poll(t->transfer, t, t->token);
        if (poll_res == ECAT_TRANSFER_PENDING) {
            /* begin 阶段尚未完成：必须深拷贝首包至受控暂存区并向协议栈返回 PENDING */
            if (t->staged_buf == NULL || length > t->staged_cap) {
                /* 未配置受控暂存区或单包长度超出暂存容量上限：严格安全失败并 abort */
                ecat_foe_transport_abort(t);
                return ECAT_UPGRADE_ASYNC_FAILED;
            }
            if (length > 0U) {
                if (data == NULL) {
                    ecat_foe_transport_abort(t);
                    return ECAT_UPGRADE_ASYNC_FAILED;
                }
                /* 防范源与目的重叠未定义行为：若相同跳过拷贝，若部分重叠使用 memmove 安全搬移 */
                if (t->staged_buf != data) {
                    memmove(t->staged_buf, data, length);
                }
            }
            t->staged_length = length;
            t->staged_more_data = more_data;
            t->has_staged_packet = true;
            t->pending = true;
            return ECAT_UPGRADE_ASYNC_PENDING;
        } else if (poll_res == ECAT_TRANSFER_COMPLETE) {
            t->begin_pending = false;
            t->pending = false;
            /* begin 已顺利就绪，顺畅进入下方首包数据写入 */
        } else {
            ecat_foe_transport_abort(t);
            return ECAT_UPGRADE_ASYNC_FAILED;
        }

    } else if (t->pending) {
        return ECAT_UPGRADE_ASYNC_FAILED;
    }

    t->finish_after_write = !more_data;
    t->finishing = false;

    ecat_transfer_result_t res = ecat_transfer_write(t->transfer, t, t->token, data, length);
    if (res == ECAT_TRANSFER_PENDING) {
        t->pending = true;
        return ECAT_UPGRADE_ASYNC_PENDING;
    } else if (res == ECAT_TRANSFER_COMPLETE) {
        t->pending = false;
        if (t->finish_after_write) {
            t->finishing = true;
            ecat_transfer_result_t fres = ecat_transfer_finish(t->transfer, t, t->token);
            if (fres == ECAT_TRANSFER_PENDING) {
                t->pending = true;
                return ECAT_UPGRADE_ASYNC_PENDING;
            } else if (fres == ECAT_TRANSFER_COMPLETE) {
                mark_committed(t);
                return ECAT_UPGRADE_ASYNC_COMPLETE;
            } else {
                ecat_foe_transport_abort(t);
                return ECAT_UPGRADE_ASYNC_FAILED;
            }
        }
        return ECAT_UPGRADE_ASYNC_COMPLETE;
    } else {
        ecat_foe_transport_abort(t);
        return ECAT_UPGRADE_ASYNC_FAILED;
    }

}

ecat_upgrade_async_result_t ecat_foe_transport_poll(void *ctx)
{
    ecat_foe_transport_t *t = (ecat_foe_transport_t *)ctx;
    if (t == NULL || !t->pending || !ecat_foe_transport_owns(t)) return ECAT_UPGRADE_ASYNC_FAILED;

    if (t->begin_pending) {

    ecat_transfer_result_t res = ecat_transfer_poll(t->transfer, t, t->token);
    if (res == ECAT_TRANSFER_PENDING) {
        return ECAT_UPGRADE_ASYNC_PENDING;
    } else if (res == ECAT_TRANSFER_COMPLETE) {
        t->begin_pending = false;
        t->pending = false;
        if (!t->has_staged_packet) {
            return ECAT_UPGRADE_ASYNC_COMPLETE;
        }
        /* 存在暂存的首包数据，在 begin 就绪后从稳定受控暂存区触发写入 */
        const uint8_t *sbuf = t->staged_buf;
        size_t length = t->staged_length;
        bool more_data = t->staged_more_data;
        t->has_staged_packet = false;
        t->staged_length = 0U;
        t->staged_more_data = false;
        return ecat_foe_transport_start(t, sbuf, length, more_data);
    } else {
        ecat_foe_transport_abort(t);
        return ECAT_UPGRADE_ASYNC_FAILED;
    }

}

    ecat_transfer_result_t res = ecat_transfer_poll(t->transfer, t, t->token);
    if (res == ECAT_TRANSFER_PENDING) {
        return ECAT_UPGRADE_ASYNC_PENDING;
    } else if (res == ECAT_TRANSFER_COMPLETE) {
        t->pending = false;
        if (t->finish_after_write && !t->finishing) {
            t->finishing = true;
            ecat_transfer_result_t fres = ecat_transfer_finish(t->transfer, t, t->token);
            if (fres == ECAT_TRANSFER_PENDING) {
                t->pending = true;
                return ECAT_UPGRADE_ASYNC_PENDING;
            } else if (fres == ECAT_TRANSFER_COMPLETE) {
                mark_committed(t);
                return ECAT_UPGRADE_ASYNC_COMPLETE;
            } else {
                ecat_foe_transport_abort(t);
                return ECAT_UPGRADE_ASYNC_FAILED;
            }
        }
        if (t->finishing) mark_committed(t);
        return ECAT_UPGRADE_ASYNC_COMPLETE;
    } else {
        ecat_foe_transport_abort(t);
        return ECAT_UPGRADE_ASYNC_FAILED;
    }

}

const ecat_upgrade_async_ops_t *ecat_foe_transport_async_ops(void)
{
    static const ecat_upgrade_async_ops_t ops = {
        ecat_foe_transport_start, ecat_foe_transport_poll, ecat_foe_transport_abort
    };
    return &ops;
}

static ecat_transfer_result_t foe_plugin_poll(void *plugin_ctx)
{
    ecat_foe_transport_t *transport = (ecat_foe_transport_t *)plugin_ctx;
    ecat_upgrade_async_result_t ares;

    if ((transport == NULL) || !ecat_foe_transport_owns(transport) || !transport->pending) {
        return ECAT_TRANSFER_INVALID;
    }

    ares = ecat_foe_transport_poll(transport);
    if (ares == ECAT_UPGRADE_ASYNC_PENDING) {
        return ECAT_TRANSFER_PENDING;
    }
    if (ares == ECAT_UPGRADE_ASYNC_COMPLETE) {
        return ECAT_TRANSFER_COMPLETE;
    }
    return ECAT_TRANSFER_FAILED;
}

static bool foe_plugin_is_active(const void *plugin_ctx)
{
    return ecat_foe_transport_owns((const ecat_foe_transport_t *)plugin_ctx);
}

static bool foe_plugin_is_pending(const void *plugin_ctx)
{
    const ecat_foe_transport_t *transport = (const ecat_foe_transport_t *)plugin_ctx;
    return (transport != NULL) && ecat_foe_transport_owns(transport) && transport->pending;
}

static const ecat_transport_plugin_ops_t s_foe_plugin_ops = {
    foe_plugin_poll,
    ecat_foe_transport_abort,
    foe_plugin_is_active,
    foe_plugin_is_pending
};

int ecat_foe_transport_make_plugin_desc(
    ecat_foe_transport_t *transport,
    const char *name,
    ecat_transport_plugin_desc_t *out_desc)
{
    if ((transport == NULL) || (transport->transfer == NULL) || (out_desc == NULL) ||
        (name == NULL) || (name[0] == '\0')) {
        return -1;
    }

    out_desc->transport_id = ECAT_TRANSPORT_ID_FOE;
    out_desc->name = name;
    out_desc->capabilities = ECAT_TRANSPORT_CAP_BIDIRECTIONAL | ECAT_TRANSPORT_CAP_ASYNC;
    out_desc->ops = &s_foe_plugin_ops;
    out_desc->plugin_ctx = transport;
    return 0;
}
