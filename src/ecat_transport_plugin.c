/**
 * @file ecat_transport_plugin.c
 * @brief 统一传输通道插件注册与轮询中心实现
 */

#define LDR_NEUTRAL_BUILD 1
#include "ecat_transport_plugin.h"
#include <string.h>

static bool is_valid_plugin_name(const char *name)
{
    if ((name == NULL) || (name[0] == '\0')) {
        return false;
    }
    return (memchr(name, '\0', ECAT_TRANSPORT_PLUGIN_NAME_MAX_LEN) != NULL);
}

int ecat_transport_registry_init(
    ecat_transport_registry_t *reg,
    ecat_transfer_t *transfer_mgr)
{
    if (reg == NULL) {
        return -1;
    }

    (void)memset(reg, 0, sizeof(*reg));
    reg->transfer_mgr = transfer_mgr;
    return 0;
}

int ecat_transport_registry_register(
    ecat_transport_registry_t *reg,
    const ecat_transport_plugin_desc_t *desc)
{
    uint32_t idx;
    int32_t free_idx = -1;

    if ((reg == NULL) || (desc == NULL)) {
        return -1;
    }
    if ((desc->transport_id == 0U) || !is_valid_plugin_name(desc->name)) {
        return -1;
    }
    if ((desc->ops == NULL) || (desc->ops->is_active == NULL) || (desc->plugin_ctx == NULL)) {
        return -1;
    }
    if (((desc->capabilities & ECAT_TRANSPORT_CAP_BIDIRECTIONAL) == 0UL) ||
        ((desc->capabilities & ~ECAT_TRANSPORT_CAP_ALL_MASK) != 0UL)) {
        return -1;
    }
    if (((desc->capabilities & ECAT_TRANSPORT_CAP_ASYNC) != 0UL) &&
        (desc->ops->poll == NULL)) {
        return -1;
    }
    if (reg->count >= (uint8_t)ECAT_TRANSPORT_REGISTRY_MAX_PLUGINS) {
        return -1;
    }

    for (idx = 0U; idx < ECAT_TRANSPORT_REGISTRY_MAX_PLUGINS; ++idx) {
        if (reg->entries[idx].in_use) {
            if ((reg->entries[idx].transport_id == desc->transport_id) ||
                (strcmp(reg->entries[idx].name, desc->name) == 0)) {
                return -1;
            }
        } else if (free_idx < 0) {
            free_idx = (int32_t)idx;
        }
    }

    if (free_idx < 0) {
        return -1;
    }

    ecat_transport_plugin_entry_t *slot = &reg->entries[(uint32_t)free_idx];
    (void)memset(slot, 0, sizeof(*slot));
    slot->transport_id = desc->transport_id;
    (void)strncpy(slot->name, desc->name, ECAT_TRANSPORT_PLUGIN_NAME_MAX_LEN - 1U);
    slot->name[ECAT_TRANSPORT_PLUGIN_NAME_MAX_LEN - 1U] = '\0';
    slot->capabilities = desc->capabilities;
    slot->ops = desc->ops;
    slot->plugin_ctx = desc->plugin_ctx;
    slot->in_use = true;
    reg->count++;

    return 0;
}

int ecat_transport_registry_unregister(
    ecat_transport_registry_t *reg,
    uint16_t transport_id)
{
    uint32_t idx;

    if (reg == NULL) {
        return -1;
    }

    for (idx = 0U; idx < ECAT_TRANSPORT_REGISTRY_MAX_PLUGINS; ++idx) {
        ecat_transport_plugin_entry_t *slot = &reg->entries[idx];
        if (slot->in_use && (slot->transport_id == transport_id)) {
            if ((slot->ops != NULL) &&
                (slot->ops->is_active != NULL) &&
                slot->ops->is_active(slot->plugin_ctx)) {
                return -1;
            }
            (void)memset(slot, 0, sizeof(*slot));
            if (reg->count > 0U) {
                reg->count--;
            }
            return 0;
        }
    }

    return -1;
}

const ecat_transport_plugin_entry_t *ecat_transport_registry_find(
    const ecat_transport_registry_t *reg,
    uint16_t transport_id)
{
    uint32_t idx;

    if (reg == NULL) {
        return NULL;
    }

    for (idx = 0U; idx < ECAT_TRANSPORT_REGISTRY_MAX_PLUGINS; ++idx) {
        if (reg->entries[idx].in_use && (reg->entries[idx].transport_id == transport_id)) {
            return &reg->entries[idx];
        }
    }

    return NULL;
}

const ecat_transport_plugin_entry_t *ecat_transport_registry_find_by_name(
    const ecat_transport_registry_t *reg,
    const char *name)
{
    uint32_t idx;

    if ((reg == NULL) || !is_valid_plugin_name(name)) {
        return NULL;
    }

    for (idx = 0U; idx < ECAT_TRANSPORT_REGISTRY_MAX_PLUGINS; ++idx) {
        if (reg->entries[idx].in_use && (strcmp(reg->entries[idx].name, name) == 0)) {
            return &reg->entries[idx];
        }
    }

    return NULL;
}

uint8_t ecat_transport_registry_count(const ecat_transport_registry_t *reg)
{
    if (reg == NULL) {
        return 0U;
    }
    return reg->count;
}

const ecat_transport_plugin_entry_t *ecat_transport_registry_get_active(
    const ecat_transport_registry_t *reg)
{
    uint32_t idx;

    if (reg == NULL) {
        return NULL;
    }

    for (idx = 0U; idx < ECAT_TRANSPORT_REGISTRY_MAX_PLUGINS; ++idx) {
        const ecat_transport_plugin_entry_t *slot = &reg->entries[idx];
        if (slot->in_use &&
            (slot->ops != NULL) &&
            (slot->ops->is_active != NULL) &&
            slot->ops->is_active(slot->plugin_ctx)) {
            return slot;
        }
    }

    return NULL;
}

bool ecat_transport_registry_supports(
    const ecat_transport_registry_t *reg,
    uint16_t transport_id,
    uint32_t cap_mask)
{
    const ecat_transport_plugin_entry_t *entry;

    if ((reg == NULL) || (cap_mask == 0UL)) {
        return false;
    }

    entry = ecat_transport_registry_find(reg, transport_id);
    if (entry == NULL) {
        return false;
    }

    return ((entry->capabilities & cap_mask) == cap_mask);
}

ecat_transfer_result_t ecat_transport_registry_poll_active(
    ecat_transport_registry_t *reg,
    uint16_t *out_transport_id)
{
    const ecat_transport_plugin_entry_t *active;

    if (out_transport_id != NULL) {
        *out_transport_id = 0U;
    }
    if (reg == NULL) {
        return ECAT_TRANSFER_INVALID;
    }

    active = ecat_transport_registry_get_active(reg);
    if (active == NULL) {
        return ECAT_TRANSFER_INVALID;
    }

    if (out_transport_id != NULL) {
        *out_transport_id = active->transport_id;
    }

    if ((active->ops == NULL) || (active->ops->poll == NULL)) {
        return ECAT_TRANSFER_INVALID;
    }

    return active->ops->poll(active->plugin_ctx);
}

void ecat_transport_registry_abort_all(ecat_transport_registry_t *reg)
{
    uint32_t idx;

    if (reg == NULL) {
        return;
    }

    for (idx = 0U; idx < ECAT_TRANSPORT_REGISTRY_MAX_PLUGINS; ++idx) {
        const ecat_transport_plugin_entry_t *slot = &reg->entries[idx];
        if (slot->in_use &&
            (slot->ops != NULL) &&
            (slot->ops->is_active != NULL) &&
            slot->ops->is_active(slot->plugin_ctx) &&
            (slot->ops->abort != NULL)) {
            slot->ops->abort(slot->plugin_ctx);
        }
    }
}
