/* elib_nvs_hal.h - NVS Hardware Abstraction Layer */
#ifndef ELIB_NVS_HAL_H
#define ELIB_NVS_HAL_H

#include "elib_nvs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注册 HAL 驱动
 *
 * @param ctx NVS 上下文
 * @param driver HAL 驱动结构
 * @return elib_nvs_err_t 错误码
 */
void elib_nvs_hal_register(elib_nvs_ctx_t *ctx, const elib_nvs_hal_driver_t *driver);

/**
 * @brief 从 HAL 读取数据
 */
elib_nvs_err_t elib_nvs_hal_read(elib_nvs_ctx_t *ctx, elib_nvs_addr_t addr, void *buf, size_t len);

/**
 * @brief 向 HAL 写入数据
 */
elib_nvs_err_t elib_nvs_hal_write(elib_nvs_ctx_t *ctx, elib_nvs_addr_t addr, const void *buf, size_t len);

/**
 * @brief 擦除 HAL 区域
 */
elib_nvs_err_t elib_nvs_hal_erase(elib_nvs_ctx_t *ctx, elib_nvs_addr_t addr, size_t size);

/**
 * @brief 获取存储介质信息
 */
elib_nvs_err_t elib_nvs_hal_get_info(elib_nvs_ctx_t *ctx, uint32_t *total_size, uint32_t *erase_size);

#ifdef __cplusplus
}
#endif

#endif /* ELIB_NVS_HAL_H */
