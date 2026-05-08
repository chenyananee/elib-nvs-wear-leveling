/* elib_nvs_mtd.c - NVS MTD Implementation */
#include "elib_nvs_mtd.h"
#include "elib_nvs_core.h"
#include "elib_nvs_scan.h"
#include <string.h>

elib_nvs_err_t elib_nvs_mtd_write(elib_nvs_ctx_t *ctx, const void *data, size_t len) {
    if (ctx == NULL || data == NULL || len == 0) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }

    if (!ctx->initialized) {
        return ELIB_NVS_ERR_NOT_INITIALIZED;
    }

    /* 检查数据大小是否超过配置的写入单位 */
    if (len > ctx->data_unit_size) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }

    /* 1. 首先检查剩余空间是否足够 */
    if (ctx->total_size - ctx->next_write_offset < ctx->data_unit_size) {
        /* 空间不足，需要擦除所有扇区 */
        elib_nvs_err_t err = elib_nvs_mtd_erase(ctx, 0, ctx->total_size);
        if (err != ELIB_NVS_OK) {
            return err;
        }
        ctx->next_write_offset = 0;
    }

    /* 2. 查找下一个可用的对齐位置 */
    int32_t offset = elib_nvs_find_free_offset(ctx, (uint32_t)len);
    if (offset < 0) {
        /* 未找到可用空间，擦除后重试 */
        elib_nvs_err_t err = elib_nvs_mtd_erase(ctx, 0, ctx->total_size);
        if (err != ELIB_NVS_OK) {
            return err;
        }
        ctx->next_write_offset = 0;
        offset = 0;  /* 从 0 开始 */
    }

    /* 3. 写入数据 */
    elib_nvs_addr_t phys_addr = ctx->start_addr + (uint32_t)offset;
    elib_nvs_err_t err = ctx->driver.write(ctx->driver.user_data, phys_addr, data, len);
    if (err != ELIB_NVS_OK) {
        return err;
    }

    /* 4. 更新写入位置 */
    ctx->next_write_offset = (uint32_t)offset + ctx->data_unit_size;

    /* 记录最后写入位置 */
    ctx->last_write_offset = (uint32_t)offset;

    return ELIB_NVS_OK;
}

elib_nvs_err_t elib_nvs_mtd_read(elib_nvs_ctx_t *ctx, void *out, size_t len) {
    if (ctx == NULL || out == NULL || len == 0) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }

    if (!ctx->initialized) {
        return ELIB_NVS_ERR_NOT_INITIALIZED;
    }

    /* 检查数据长度 */
    if (len > ctx->data_unit_size) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }

    /* 检查是否有已写入的数据 */
    if (ctx->last_write_offset == 0xFFFFFFFF) {
        return ELIB_NVS_ERR_NOT_FOUND;
    }

    /* 使用 last_write_offset 作为读取位置 */
    elib_nvs_addr_t phys_addr = ctx->start_addr + ctx->last_write_offset;
    return ctx->driver.read(ctx->driver.user_data, phys_addr, out, len);
}

elib_nvs_err_t elib_nvs_mtd_erase(elib_nvs_ctx_t *ctx, elib_nvs_addr_t addr, size_t size) {
    if (ctx == NULL) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }

    if (!ctx->initialized) {
        return ELIB_NVS_ERR_NOT_INITIALIZED;
    }

    /* addr 现在是相对于 start_addr 的偏移 */
    if (addr + size > ctx->total_size) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }

    /* 对齐到块边界 */
    uint32_t start_block = addr / ctx->block_size;
    uint32_t num_blocks = (size + ctx->block_size - 1) / ctx->block_size;

    for (uint32_t i = 0; i < num_blocks; i++) {
        elib_nvs_addr_t block_addr = ctx->start_addr + (start_block + i) * ctx->block_size;
        elib_nvs_err_t err = ctx->driver.erase(ctx->driver.user_data, block_addr, ctx->block_size);
        if (err != ELIB_NVS_OK) {
            return err;
        }
        /* 注意：移除了磨损计数更新 */
    }

    return ELIB_NVS_OK;
}
