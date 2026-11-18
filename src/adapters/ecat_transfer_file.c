#include "ecat_transfer_file.h"
#include <string.h>

int ecat_transfer_file_init(ecat_transfer_file_t *f, ecat_transfer_t *transfer,
                            uint8_t *buffer, size_t capacity)
{
    if ((f == NULL) || (transfer == NULL) || (buffer == NULL) || (capacity == 0U)) {
        return -1;
    }
    (void)memset(f, 0, sizeof(*f));
    f->transfer = transfer;
    f->buffer = buffer;
    f->capacity = capacity;
    f->result = ECAT_TRANSFER_INVALID;
    return 0;
}

ecat_transfer_result_t ecat_transfer_file_begin(ecat_transfer_file_t *f,
    FILE *stream, const ecat_transfer_request_t *request)
{
    ecat_transfer_result_t result;

    if ((f == NULL) || (f->transfer == NULL) || (stream == NULL) || (request == NULL) ||
        ((request->direction != ECAT_TRANSFER_DIR_IMPORT) &&
         (request->direction != ECAT_TRANSFER_DIR_EXPORT))) {
        return ECAT_TRANSFER_INVALID;
    }
    if (f->active) {
        return ECAT_TRANSFER_BUSY;
    }

    result = ecat_transfer_begin(f->transfer, f, request, &f->token);
    if ((result != ECAT_TRANSFER_COMPLETE) && (result != ECAT_TRANSFER_PENDING)) {
        return result;
    }
    f->stream = stream;
    f->direction = request->direction;
    f->pending_read_len = 0U;
    f->active = true;
    f->pending = (result == ECAT_TRANSFER_PENDING);
    f->ending = false;
    f->export_eof = false;
    f->read_in_flight = false;
    f->io_error = false;
    f->result = ECAT_TRANSFER_PENDING;
    return result;
}

ecat_transfer_result_t ecat_transfer_file_cancel(ecat_transfer_file_t *f)
{
    ecat_transfer_result_t result;

    if ((f == NULL) || (f->transfer == NULL)) {
        return ECAT_TRANSFER_INVALID;
    }
    if (!f->active) {
        return ECAT_TRANSFER_STALE;
    }
    result = ecat_transfer_cancel(f->transfer, f, f->token);
    f->active = false;
    f->pending = false;
    f->ending = false;
    f->export_eof = false;
    f->read_in_flight = false;
    f->pending_read_len = 0U;
    f->result = ECAT_TRANSFER_FAILED;
    return result;
}

static ecat_transfer_result_t advance(ecat_transfer_file_t *f, ecat_transfer_result_t result)
{
    f->pending = (result == ECAT_TRANSFER_PENDING);
    if ((result != ECAT_TRANSFER_COMPLETE) && (result != ECAT_TRANSFER_PENDING)) {
        (void)ecat_transfer_file_cancel(f);
        return ECAT_TRANSFER_FAILED;
    }
    if ((result == ECAT_TRANSFER_COMPLETE) && f->ending) {
        f->active = false;
        f->result = ECAT_TRANSFER_COMPLETE;
    }
    return f->result;
}

static ecat_transfer_result_t flush_export_chunk(ecat_transfer_file_t *f, ecat_transfer_result_t result)
{
    if (result == ECAT_TRANSFER_PENDING) {
        f->pending = true;
        return ECAT_TRANSFER_PENDING;
    }
    f->pending = false;
    f->read_in_flight = false;
    if ((result != ECAT_TRANSFER_COMPLETE) && (result != ECAT_TRANSFER_CONTINUE)) {
        (void)ecat_transfer_file_cancel(f);
        return ECAT_TRANSFER_FAILED;
    }
    if (f->pending_read_len > 0U) {
        if ((fwrite(f->buffer, 1U, f->pending_read_len, f->stream) != f->pending_read_len) ||
            (ferror(f->stream) != 0)) {
            f->io_error = true;
            (void)ecat_transfer_file_cancel(f);
            return ECAT_TRANSFER_FAILED;
        }
        f->pending_read_len = 0U;
    }
    if (result == ECAT_TRANSFER_COMPLETE) {
        if (fflush(f->stream) != 0) {
            f->io_error = true;
            (void)ecat_transfer_file_cancel(f);
            return ECAT_TRANSFER_FAILED;
        }
        f->export_eof = true;
    }
    return f->result;
}

ecat_transfer_result_t ecat_transfer_file_step(ecat_transfer_file_t *f)
{
    size_t length;

    if ((f == NULL) || (f->transfer == NULL)) {
        return ECAT_TRANSFER_INVALID;
    }
    if (!f->active) {
        return f->result;
    }

    if (f->direction == ECAT_TRANSFER_DIR_EXPORT) {
        if (f->pending) {
            ecat_transfer_result_t res = ecat_transfer_poll(f->transfer, f, f->token);
            if (f->read_in_flight) {
                return flush_export_chunk(f, res);
            }
            return advance(f, res);
        }
        if (f->export_eof) {
            f->ending = true;
            return advance(f, ecat_transfer_finish(f->transfer, f, f->token));
        }
        f->pending_read_len = 0U;
        f->read_in_flight = true;
        return flush_export_chunk(f, ecat_transfer_read(
            f->transfer, f, f->token, f->buffer, f->capacity, &f->pending_read_len));
    }

    if (f->pending) {
        return advance(f, ecat_transfer_poll(f->transfer, f, f->token));
    }

    length = fread(f->buffer, 1U, f->capacity, f->stream);
    if (ferror(f->stream) != 0) {
        f->io_error = true;
        (void)ecat_transfer_file_cancel(f);
        return ECAT_TRANSFER_FAILED;
    }
    if (length != 0U) {
        return advance(f, ecat_transfer_write(f->transfer, f, f->token, f->buffer, length));
    }
    if (feof(f->stream) == 0) {
        f->io_error = true;
        (void)ecat_transfer_file_cancel(f);
        return ECAT_TRANSFER_FAILED;
    }
    f->ending = true;
    return advance(f, ecat_transfer_finish(f->transfer, f, f->token));
}

static ecat_transfer_result_t file_plugin_poll(void *plugin_ctx)
{
    return ecat_transfer_file_step((ecat_transfer_file_t *)plugin_ctx);
}

static void file_plugin_abort(void *plugin_ctx)
{
    (void)ecat_transfer_file_cancel((ecat_transfer_file_t *)plugin_ctx);
}

static bool file_plugin_is_active(const void *plugin_ctx)
{
    const ecat_transfer_file_t *file = (const ecat_transfer_file_t *)plugin_ctx;
    return (file != NULL) && file->active;
}

static bool file_plugin_is_pending(const void *plugin_ctx)
{
    const ecat_transfer_file_t *file = (const ecat_transfer_file_t *)plugin_ctx;
    return (file != NULL) && file->active && file->pending;
}

static const ecat_transport_plugin_ops_t s_file_plugin_ops = {
    file_plugin_poll,
    file_plugin_abort,
    file_plugin_is_active,
    file_plugin_is_pending
};

int ecat_transfer_file_make_plugin_desc(
    ecat_transfer_file_t *file,
    uint16_t transport_id,
    const char *name,
    ecat_transport_plugin_desc_t *out_desc)
{
    if ((file == NULL) || (file->transfer == NULL) || (out_desc == NULL) ||
        (name == NULL) || (name[0] == '\0')) {
        return -1;
    }

    out_desc->transport_id = transport_id;
    out_desc->name = name;
    out_desc->capabilities = ECAT_TRANSPORT_CAP_BIDIRECTIONAL | ECAT_TRANSPORT_CAP_ASYNC;
    out_desc->ops = &s_file_plugin_ops;
    out_desc->plugin_ctx = file;
    return 0;
}
