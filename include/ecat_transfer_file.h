/** Optional host FILE stream import/export adapter; not a hardware protocol. */
#ifndef ECAT_TRANSFER_FILE_H
#define ECAT_TRANSFER_FILE_H
#include "ecat_transfer.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ecat_transfer_t *transfer;
    FILE *stream;
    uint8_t *buffer;
    size_t capacity;
    uint64_t token;
    ecat_transfer_dir_t direction;
    size_t pending_read_len;
    bool active;
    bool pending;
    bool ending;
    bool export_eof;
    bool read_in_flight;
    bool io_error;
    ecat_transfer_result_t result;
} ecat_transfer_file_t;

/* Initialize once; do not copy. Caller owns the stream and buffer lifetime. */
int ecat_transfer_file_init(ecat_transfer_file_t *file, ecat_transfer_t *transfer,
                            uint8_t *buffer, size_t capacity);
/* The caller selects transport ID, target, and direction (IMPORT or EXPORT).
 * expected_size may be zero for a stream of unknown size. Busy never touches it. */
ecat_transfer_result_t ecat_transfer_file_begin(ecat_transfer_file_t *file,
    FILE *stream, const ecat_transfer_request_t *request);
/* One read/write or poll per step. Returns PENDING until EOF commits. fread/fwrite
 * may block; this is a host reference, not an ISR/real-time API. The caller
 * retains ownership of FILE and must keep it and the buffer alive throughout. */
ecat_transfer_result_t ecat_transfer_file_step(ecat_transfer_file_t *file);
ecat_transfer_result_t ecat_transfer_file_cancel(ecat_transfer_file_t *file);

#ifdef __cplusplus
}
#endif
#endif
