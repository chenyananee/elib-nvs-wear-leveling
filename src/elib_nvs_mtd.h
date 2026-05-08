/* elib_nvs_mtd.h - NVS Memory Technology Device Interface */
#ifndef ELIB_NVS_MTD_H
#define ELIB_NVS_MTD_H

#include "elib_nvs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 写入数据到线性地址空间（自动管理写入位置）
 * @param ctx NVS 上下文
 * @param data 要写入的数据
 * @param len 数据长度
 * @return elib_nvs_err_t 错误码
 */
elib_nvs_err_t elib_nvs_mtd_write(elib_nvs_ctx_t *ctx, const void *data, size_t len);

/**
 * @brief 读取最近一次写入的数据（自动定位）
 * @param ctx NVS 上下文
 * @param out 输出缓冲区
 * @param len 读取长度
 * @return elib_nvs_err_t 错误码
 */
elib_nvs_err_t elib_nvs_mtd_read(elib_nvs_ctx_t *ctx, void *out, size_t len);

/**
 * @brief 擦除线性地址空间
 */
elib_nvs_err_t elib_nvs_mtd_erase(elib_nvs_ctx_t *ctx, elib_nvs_addr_t addr, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* ELIB_NVS_MTD_H */
