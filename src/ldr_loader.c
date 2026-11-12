/**
 * @file ldr_loader.c
 * @brief 统一可扩展双向 Loader 框架中性门面实现
 */

#include "ldr_loader.h"
#include <string.h>

int ldr_loader_init(ldr_loader_t *loader, ecat_foe_core_t *fw_core)
{
    if (loader == NULL) {
        return -1;
    }

    (void)memset(loader, 0, sizeof(*loader));

    if (ecat_target_registry_init(&loader->targets) != 0) {
        return -1;
    }
    if (ldr_format_registry_init(&loader->formats) != 0) {
        return -1;
    }
    if (ecat_transfer_init(&loader->transfer, &loader->targets, fw_core) != 0) {
        return -1;
    }
    if (ecat_transport_registry_init(&loader->transports, &loader->transfer) != 0) {
        return -1;
    }

    loader->initialized = true;
    return 0;
}

int ldr_loader_register_target(
    ldr_loader_t *loader,
    const ldr_target_desc_t *desc)
{
    if ((loader == NULL) || !loader->initialized) {
        return -1;
    }
    return ecat_target_registry_add(&loader->targets, desc);
}

int ldr_loader_add_acl(
    ldr_loader_t *loader,
    uint16_t transport_id,
    uint16_t target_id,
    uint8_t allowed_perms)
{
    if ((loader == NULL) || !loader->initialized) {
        return -1;
    }
    return ecat_target_registry_add_acl(&loader->targets, transport_id, target_id, allowed_perms);
}

void ldr_loader_set_enforce_acl(ldr_loader_t *loader, bool enforce)
{
    if ((loader == NULL) || !loader->initialized) {
        return;
    }
    ecat_target_registry_set_enforce_acl(&loader->targets, enforce);
}

int ldr_loader_register_transport(
    ldr_loader_t *loader,
    const ldr_transport_plugin_desc_t *desc)
{
    if ((loader == NULL) || !loader->initialized) {
        return -1;
    }
    return ecat_transport_registry_register(&loader->transports, desc);
}

int ldr_loader_unregister_transport(
    ldr_loader_t *loader,
    uint16_t transport_id)
{
    if ((loader == NULL) || !loader->initialized) {
        return -1;
    }
    return ecat_transport_registry_unregister(&loader->transports, transport_id);
}

int ldr_loader_register_format(
    ldr_loader_t *loader,
    const ldr_format_plugin_desc_t *desc)
{
    if ((loader == NULL) || !loader->initialized) {
        return -1;
    }
    return ldr_format_registry_register(&loader->formats, desc);
}

int ldr_loader_unregister_format(
    ldr_loader_t *loader,
    uint16_t format_id)
{
    if ((loader == NULL) || !loader->initialized) {
        return -1;
    }
    return ldr_format_registry_unregister(&loader->formats, format_id);
}

ldr_format_probe_result_t ldr_loader_probe_format(
    const ldr_loader_t *loader,
    const uint8_t *buf,
    size_t len,
    uint16_t *out_header_size,
    ldr_format_metadata_t *out_meta)
{
    if ((loader == NULL) || !loader->initialized) {
        if (out_header_size != NULL) {
            *out_header_size = 0U;
        }
        return LDR_FORMAT_INVALID;
    }
    return ldr_format_registry_probe_and_parse(
        &loader->formats, buf, len, out_header_size, out_meta);
}

int ldr_loader_set_staging_buffer(
    ldr_loader_t *loader,
    uint8_t *buf,
    size_t capacity)
{
    if ((loader == NULL) || !loader->initialized) {
        return -1;
    }
    return ecat_transfer_set_staging_buffer(&loader->transfer, buf, capacity);
}

ldr_transfer_result_t ldr_loader_poll(
    ldr_loader_t *loader,
    uint16_t *out_transport_id)
{
    if ((loader == NULL) || !loader->initialized) {
        if (out_transport_id != NULL) {
            *out_transport_id = 0U;
        }
        return LDR_RES_INVALID;
    }
    return ecat_transport_registry_poll_active(&loader->transports, out_transport_id);
}

void ldr_loader_abort_all(ldr_loader_t *loader)
{
    if ((loader == NULL) || !loader->initialized) {
        return;
    }
    ecat_transport_registry_abort_all(&loader->transports);
}

bool ldr_loader_is_busy(const ldr_loader_t *loader)
{
    if ((loader == NULL) || !loader->initialized) {
        return false;
    }
    return ecat_transfer_is_active(&loader->transfer) ||
           (ecat_transport_registry_get_active(&loader->transports) != NULL);
}

const ldr_transport_plugin_entry_t *ldr_loader_get_active_transport(
    const ldr_loader_t *loader)
{
    if ((loader == NULL) || !loader->initialized) {
        return NULL;
    }
    return ecat_transport_registry_get_active(&loader->transports);
}

ldr_transfer_t *ldr_loader_get_transfer(ldr_loader_t *loader)
{
    if ((loader == NULL) || !loader->initialized) {
        return NULL;
    }
    return &loader->transfer;
}

ldr_target_registry_t *ldr_loader_get_targets(ldr_loader_t *loader)
{
    if ((loader == NULL) || !loader->initialized) {
        return NULL;
    }
    return &loader->targets;
}

ldr_transport_registry_t *ldr_loader_get_transports(ldr_loader_t *loader)
{
    if ((loader == NULL) || !loader->initialized) {
        return NULL;
    }
    return &loader->transports;
}

ldr_format_registry_t *ldr_loader_get_formats(ldr_loader_t *loader)
{
    if ((loader == NULL) || !loader->initialized) {
        return NULL;
    }
    return &loader->formats;
}
