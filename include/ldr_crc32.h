/**
 * @file ldr_crc32.h
 * @brief 中性 IEEE 802.3 CRC32 增量校验接口 (零 FoE / 零 EFW 依赖)
 */

#ifndef LDR_CRC32_H
#define LDR_CRC32_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void ldr_crc32_init(uint32_t *crc);
void ldr_crc32_update(uint32_t *crc, const uint8_t *data, size_t len);
uint32_t ldr_crc32_final(const uint32_t *crc);
uint32_t ldr_crc32_calc(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* LDR_CRC32_H */
