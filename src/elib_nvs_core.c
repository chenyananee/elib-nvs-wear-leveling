/* elib_nvs_core.c - NVS Wear Leveling Core Implementation (V3 API) */
#include "elib_nvs_core.h"
#include "elib_nvs_scan.h"
#include <string.h>

/**
 * @brief 初始化 NVS 上下文 (V3 API - 纯运行时参数化，无全局配置)
 * 消除 V1/V2 API 中的全局配置文件依赖和 elib_nvs_resources_t 结构
 * 通过驱动接口获取全部硬件抽象层资源指针
 */
elib_nvs_err_t elib_nvs_init(
    elib_nvs_ctx_t *ctx,
    const elib_nvs_hal_driver_t *driver,
    uint32_t start_addr,
    uint32_t total_size,
    uint32_t block_size,
    uint32_t data_unit_size
) {
    /* 参数有效性检查 */
    if (ctx == NULL) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }
    if (driver == NULL) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }
    if (driver->read == NULL || driver->write == NULL || driver->erase == NULL) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }
    if (total_size == 0 || block_size == 0 || data_unit_size == 0) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }
    if (data_unit_size > total_size) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }

    /* 清零并初始化上下文 */
    memset(ctx, 0, sizeof(elib_nvs_ctx_t));

    /* 复制驱动接口（其中已包含用户资源指针），消除独立的 resources 参数 */
    memcpy(&ctx->driver, driver, sizeof(elib_nvs_hal_driver_t));

    /* 保存配置参数 */
    ctx->start_addr = start_addr;
    ctx->total_size = total_size;
    ctx->block_size = block_size;
    ctx->data_unit_size = data_unit_size;
    ctx->next_write_offset = 0;  /* 从起始位置开始 */
    ctx->last_write_offset = 0xFFFFFFFF;  /* 初始化为无效值 */

    /* 标记已初始化 */
    ctx->bit_flags.initialized = 1;

    /* 扫描并恢复写入位置 (掉电恢复) */
    int result = elib_nvs_recover_write_position(ctx);
    if (result < 0) {
        /* 恢复失败，从 0 开始 */
        ctx->next_write_offset = 0;
    }

    return ELIB_NVS_OK;
}

/**
 * @brief 反初始化 NVS 上下文 (V2 API - 用户管理内存)
 * @note 本函数仅标记未初始化，不释放任何内存 (由用户管理)
 */
void elib_nvs_deinit(elib_nvs_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    ctx->bit_flags.initialized = 0;
}

/**
 * @brief 在指定逻辑地址处写入数据
 * @param ctx 上下文指针
 * @param addr 逻辑地址
 * @param data 要写入的数据缓冲区
 * @param len 数据长度
 * @return elib_nvs_err_t 错误码
 */
elib_nvs_err_t elib_nvs_write(elib_nvs_ctx_t *ctx, uint32_t addr, const void *data, size_t len) {
    if (ctx == NULL || data == NULL || len == 0) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }

    if (!ctx->bit_flags.initialized) {
        return ELIB_NVS_ERR_NOT_INITIALIZED;
    }

    if (addr + len > ctx->total_size) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }

    /* 物理地址 = 基地址 + 逻辑地址 */
    elib_nvs_addr_t phys_addr = ctx->start_addr + addr;

    /* 使用驱动接口写入数据 */
    return ctx->driver.write(ctx->driver.user_data, phys_addr, data, len);
}

/**
 * @brief 从指定逻辑地址读取数据
 * @param ctx 上下文指针
 * @param addr 逻辑地址
 * @param out 输出缓冲区
 * @param len 要读取的数据字节数
 * @return elib_nvs_err_t 错误码
 */
elib_nvs_err_t elib_nvs_read(elib_nvs_ctx_t *ctx, uint32_t addr, void *out, size_t len) {
    if (ctx == NULL || out == NULL || len == 0) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }

    if (!ctx->bit_flags.initialized) {
        return ELIB_NVS_ERR_NOT_INITIALIZED;
    }

    if (addr + len > ctx->total_size) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }

    /* 物理地址 = 基地址 + 逻辑地址 */
    elib_nvs_addr_t phys_addr = ctx->start_addr + addr;

    /* 使用驱动接口读取数据 */
    return ctx->driver.read(ctx->driver.user_data, phys_addr, out, len);
}

/**
 * @brief 擦除指定逻辑地址范围
 * @param ctx 上下文指针
 * @param addr 起始逻辑地址
 * @param size 要擦除的字节数
 * @return elib_nvs_err_t 错误码
 */
elib_nvs_err_t elib_nvs_erase(elib_nvs_ctx_t *ctx, uint32_t addr, size_t size) {
    if (ctx == NULL) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }

    if (!ctx->bit_flags.initialized) {
        return ELIB_NVS_ERR_NOT_INITIALIZED;
    }

    if (addr + size > ctx->total_size) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }

    /* 对齐到块边界进行擦除 */
    uint32_t block_start = (addr / ctx->block_size) * ctx->block_size;
    uint32_t block_end = ((addr + size + ctx->block_size - 1) / ctx->block_size) * ctx->block_size;

    for (uint32_t pos = block_start; pos < block_end; pos += ctx->block_size) {
        elib_nvs_addr_t phys_addr = ctx->start_addr + pos;

        /* 使用驱动接口擦除块 */
        elib_nvs_err_t err = ctx->driver.erase(ctx->driver.user_data, phys_addr, ctx->block_size);
        if (err != ELIB_NVS_OK) {
            return err;
        }
    }

    return ELIB_NVS_OK;
}

/**
 * @brief 获取与上下文关联的用户数据指针
 * @param ctx 上下文指针
 * @return void* 用户数据指针
 */
void *elib_nvs_get_user_data(elib_nvs_ctx_t *ctx) {
    if (ctx == NULL) {
        return NULL;
    }
    return ctx->driver.user_data;
}
