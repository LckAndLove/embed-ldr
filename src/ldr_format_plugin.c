/**
 * @file ldr_format_plugin.c
 * @brief 统一格式与安全校验插件注册表实现
 */

#include "ldr_format_plugin.h"
#include <string.h>

static bool is_valid_format_name(const char *name)
{
    if ((name == NULL) || (name[0] == '\0')) {
        return false;
    }
    return (memchr(name, '\0', LDR_FORMAT_PLUGIN_NAME_MAX_LEN) != NULL);
}

int ldr_format_registry_init(ldr_format_registry_t *reg)
{
    if (reg == NULL) {
        return -1;
    }
    (void)memset(reg, 0, sizeof(*reg));
    return 0;
}

int ldr_format_registry_register(
    ldr_format_registry_t *reg,
    const ldr_format_plugin_desc_t *desc)
{
    uint32_t idx;
    int32_t free_idx = -1;

    if ((reg == NULL) || (desc == NULL)) {
        return -1;
    }
    if ((desc->format_id == 0U) || !is_valid_format_name(desc->name)) {
        return -1;
    }
    if ((desc->ops == NULL) ||
        (desc->ops->probe == NULL) ||
        (desc->ops->parse_and_verify == NULL)) {
        return -1;
    }
    if (reg->count >= (uint8_t)LDR_FORMAT_REGISTRY_MAX_PLUGINS) {
        return -1;
    }

    for (idx = 0U; idx < LDR_FORMAT_REGISTRY_MAX_PLUGINS; ++idx) {
        if (reg->entries[idx].in_use) {
            if ((reg->entries[idx].format_id == desc->format_id) ||
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

    ldr_format_plugin_entry_t *slot = &reg->entries[(uint32_t)free_idx];
    (void)memset(slot, 0, sizeof(*slot));
    slot->format_id = desc->format_id;
    (void)strncpy(slot->name, desc->name, LDR_FORMAT_PLUGIN_NAME_MAX_LEN - 1U);
    slot->name[LDR_FORMAT_PLUGIN_NAME_MAX_LEN - 1U] = '\0';
    slot->ops = desc->ops;
    slot->plugin_ctx = desc->plugin_ctx;
    slot->in_use = true;
    reg->count++;
    return 0;
}

int ldr_format_registry_unregister(
    ldr_format_registry_t *reg,
    uint16_t format_id)
{
    uint32_t idx;

    if ((reg == NULL) || (format_id == 0U)) {
        return -1;
    }

    for (idx = 0U; idx < LDR_FORMAT_REGISTRY_MAX_PLUGINS; ++idx) {
        if (reg->entries[idx].in_use && (reg->entries[idx].format_id == format_id)) {
            (void)memset(&reg->entries[idx], 0, sizeof(reg->entries[idx]));
            if (reg->count > 0U) {
                reg->count--;
            }
            return 0;
        }
    }

    return -1;
}

const ldr_format_plugin_entry_t *ldr_format_registry_find(
    const ldr_format_registry_t *reg,
    uint16_t format_id)
{
    uint32_t idx;

    if ((reg == NULL) || (format_id == 0U)) {
        return NULL;
    }

    for (idx = 0U; idx < LDR_FORMAT_REGISTRY_MAX_PLUGINS; ++idx) {
        if (reg->entries[idx].in_use && (reg->entries[idx].format_id == format_id)) {
            return &reg->entries[idx];
        }
    }

    return NULL;
}

const ldr_format_plugin_entry_t *ldr_format_registry_find_by_name(
    const ldr_format_registry_t *reg,
    const char *name)
{
    uint32_t idx;

    if ((reg == NULL) || !is_valid_format_name(name)) {
        return NULL;
    }

    for (idx = 0U; idx < LDR_FORMAT_REGISTRY_MAX_PLUGINS; ++idx) {
        if (reg->entries[idx].in_use && (strcmp(reg->entries[idx].name, name) == 0)) {
            return &reg->entries[idx];
        }
    }

    return NULL;
}

uint8_t ldr_format_registry_count(const ldr_format_registry_t *reg)
{
    if (reg == NULL) {
        return 0U;
    }
    return reg->count;
}

ldr_format_probe_result_t ldr_format_registry_probe_and_parse(
    const ldr_format_registry_t *reg,
    const uint8_t *buf,
    size_t len,
    uint16_t *out_header_size,
    ldr_format_metadata_t *out_meta)
{
    uint32_t idx;
    bool saw_need_more = false;

    if (out_header_size != NULL) {
        *out_header_size = 0U;
    }
    if ((reg == NULL) || (buf == NULL) || (out_meta == NULL)) {
        return LDR_FORMAT_INVALID;
    }

    for (idx = 0U; idx < LDR_FORMAT_REGISTRY_MAX_PLUGINS; ++idx) {
        const ldr_format_plugin_entry_t *slot = &reg->entries[idx];
        uint16_t hdr_size = 0U;
        ldr_format_probe_result_t pres;

        if (!slot->in_use || (slot->ops == NULL) ||
            (slot->ops->probe == NULL) || (slot->ops->parse_and_verify == NULL)) {
            continue;
        }

        pres = slot->ops->probe(buf, len, &hdr_size, slot->plugin_ctx);
        if (pres == LDR_FORMAT_MATCH) {
            if (out_header_size != NULL) {
                *out_header_size = hdr_size;
            }
            if ((hdr_size == 0U) || (len < (size_t)hdr_size)) {
                return LDR_FORMAT_NEED_MORE;
            }
            (void)memset(out_meta, 0, sizeof(*out_meta));
            if (slot->ops->parse_and_verify(buf, (size_t)hdr_size, out_meta, slot->plugin_ctx) != 0) {
                return LDR_FORMAT_INVALID;
            }
            out_meta->format_id = slot->format_id;
            out_meta->header_size = hdr_size;
            return LDR_FORMAT_MATCH;
        }
        if (pres == LDR_FORMAT_INVALID) {
            return LDR_FORMAT_INVALID;
        }
        if (pres == LDR_FORMAT_NEED_MORE) {
            saw_need_more = true;
        }
    }

    return saw_need_more ? LDR_FORMAT_NEED_MORE : LDR_FORMAT_NO_MATCH;
}
