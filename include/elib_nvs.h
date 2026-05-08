/* elib_nvs.h - NVS Wear Leveling Main Header */
#ifndef ELIB_NVS_H
#define ELIB_NVS_H

#include "elib_nvs_err.h"
#include "elib_nvs_types.h"
#include "elib_nvs_config.h"
#include "elib_nvs_hal.h"
#include "elib_nvs_core.h"
#include "elib_nvs_mtd.h"

/* Public API functions */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 NVS 子系统 (V3 API - 纯运行时参数化，无全局配置)
 * @param ctx 用户预分配的上下文指针
 * @param driver 硬件抽象层驱动接口（其中包含用户资源数据）
 * @param start_addr 管理区域起始物理地址
 * @param total_size 总管理空间大小
 * @param block_size 扇区/块大小
 * @param data_unit_size 用户数据固定写入大小
 * @return elib_nvs_err_t 错误码
 */
elib_nvs_err_t elib_nvs_init(
    elib_nvs_ctx_t *ctx,
    const elib_nvs_hal_driver_t *driver,
    uint32_t start_addr,
    uint32_t total_size,
    uint32_t block_size,
    uint32_t data_unit_size
);

/**
 * @brief 反初始化 NVS 子系统 (V2 API - 用户管理内存)
 * @param ctx 上下文指针
 */
void elib_nvs_deinit(elib_nvs_ctx_t *ctx);

/**
 * @brief 在指定逻辑地址处写入数据
 * @param ctx 上下文指针
 * @param addr 逻辑地址
 * @param data 要写入的数据缓冲区
 * @param len 数据长度
 * @return elib_nvs_err_t 错误码
 */
elib_nvs_err_t elib_nvs_write(elib_nvs_ctx_t *ctx, uint32_t addr, const void *data, size_t len);

/**
 * @brief 从指定逻辑地址读取数据
 * @param ctx 上下文指针
 * @param addr 逻辑地址
 * @param out 输出缓冲区
 * @param len 要读取的数据字节数
 * @return elib_nvs_err_t 错误码
 */
elib_nvs_err_t elib_nvs_read(elib_nvs_ctx_t *ctx, uint32_t addr, void *out, size_t len);

/**
 * @brief 擦除指定逻辑地址范围
 * @param ctx 上下文指针
 * @param addr 起始逻辑地址
 * @param size 要擦除的字节数
 * @return elib_nvs_err_t 错误码
 */
elib_nvs_err_t elib_nvs_erase(elib_nvs_ctx_t *ctx, uint32_t addr, size_t size);

/**
 * @brief 写入数据到下一个可用位置 (MTD模式)
 * @param ctx 上下文指针
 * @param data 要写入的数据缓冲区
 * @param len 数据长度
 * @return elib_nvs_err_t 错误码
 */
elib_nvs_err_t elib_nvs_mtd_write(elib_nvs_ctx_t *ctx, const void *data, size_t len);

/**
 * @brief 读取最后写入的数据 (MTD模式)
 * @param ctx 上下文指针
 * @param out 输出缓冲区
 * @param len 要读取的数据字节数
 * @return elib_nvs_err_t 错误码
 */
elib_nvs_err_t elib_nvs_mtd_read(elib_nvs_ctx_t *ctx, void *out, size_t len);

/**
 * @brief 擦除指定位置 (MTD模式)
 * @param ctx 上下文指针
 * @param addr 逻辑地址
 * @param size 要擦除的字节数
 * @return elib_nvs_err_t 错误码
 */
elib_nvs_err_t elib_nvs_mtd_erase(elib_nvs_ctx_t *ctx, uint32_t addr, size_t size);

/**
 * @brief 获取与上下文关联的用户数据指针
 * @param ctx 上下文指针
 * @return void* 用户数据指针
 */
void *elib_nvs_get_user_data(elib_nvs_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ELIB_NVS_H */
