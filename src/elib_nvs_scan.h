/* elib_nvs_scan.h - NVS Scan Helper Functions */
#ifndef ELIB_NVS_SCAN_H
#define ELIB_NVS_SCAN_H

#include "elib_nvs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 检查指定区域是否全为空闲 (0xff)
 * @param ctx NVS 上下文
 * @param offset 相对于 start_addr 的偏移
 * @param len 检查长度
 * @return 1=全空闲，0=已使用，<0=错误
 */
int elib_nvs_is_region_free(
    const elib_nvs_ctx_t *ctx,
    uint32_t offset,
    uint32_t len
);

/**
 * @brief 查找下一个可写入的对齐位置
 * @param ctx NVS 上下文
 * @param len 需要写入的数据长度
 * @return 可写入的偏移量 (相对于 start_addr)，找不到返回 -1
 */
int32_t elib_nvs_find_free_offset(
    const elib_nvs_ctx_t *ctx,
    uint32_t len
);

/**
 * @brief 扫描并恢复写入位置 (掉电恢复用)
 * @param ctx NVS 上下文
 * @return 0=成功，<0=错误
 */
int elib_nvs_recover_write_position(elib_nvs_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ELIB_NVS_SCAN_H */
