/* elib_nvs_scan.c - NVS Scan Helper Implementation */
#include "elib_nvs_scan.h"
#include "elib_nvs_core.h"
#include <string.h>
#include <stdint.h>

/* 最大扫描块大小 - 使用栈缓冲避免 malloc */
#define NVS_SCAN_MAX_BLOCK 256

int elib_nvs_is_region_free(
    const elib_nvs_ctx_t *ctx,
    uint32_t offset,
    uint32_t len
) {
    if (ctx == NULL || !ctx->initialized) {
        return -1;
    }
    if (offset + len > ctx->total_size) {
        return -1;  /* 超出范围 */
    }

    /* 分块扫描，避免大缓冲区 */
    uint8_t buf[NVS_SCAN_MAX_BLOCK];
    uint32_t remaining = len;
    uint32_t current_offset = offset;

    while (remaining > 0) {
        uint32_t chunk_size = (remaining > NVS_SCAN_MAX_BLOCK) ? NVS_SCAN_MAX_BLOCK : remaining;

        /* 读取一块数据 */
        elib_nvs_addr_t phys_addr = ctx->start_addr + current_offset;
        elib_nvs_err_t err = ctx->driver.read(ctx->driver.user_data, phys_addr, buf, chunk_size);
        if (err != ELIB_NVS_OK) {
            return -1;
        }

        /* 检查是否全为 0xff */
        for (uint32_t i = 0; i < chunk_size; i++) {
            if (buf[i] != 0xff) {
                return 0;  /* 已使用 */
            }
        }

        remaining -= chunk_size;
        current_offset += chunk_size;
    }

    return 1;  /* 全空闲 */
}

int32_t elib_nvs_find_free_offset(
    const elib_nvs_ctx_t *ctx,
    uint32_t len
) {
    if (ctx == NULL || !ctx->initialized) {
        return -1;
    }
    if (len == 0) {
        return -1;
    }

    /* 按 data_unit_size 对齐查找 */
    uint32_t align = ctx->data_unit_size;
    for (uint32_t offset = 0; offset + len <= ctx->total_size; offset += align) {
        int result = elib_nvs_is_region_free(ctx, offset, len);
        if (result > 0) {
            return (int32_t)offset;
        } else if (result < 0) {
            return -1;  /* 读取错误 */
        }
        /* result == 0 继续查找 */
    }

    return -1;  /* 未找到 */
}

int elib_nvs_recover_write_position(elib_nvs_ctx_t *ctx) {
    if (ctx == NULL || !ctx->initialized) {
        return -1;
    }

    uint32_t align = ctx->data_unit_size;
    ctx->last_write_offset = 0xFFFFFFFF;  /* 标记无已写入数据 */
    ctx->next_write_offset = 0;

    /* 从前往后扫描，找到第一个空闲位置 */
    for (uint32_t offset = 0; offset + align <= ctx->total_size; offset += align) {
        int result = elib_nvs_is_region_free(ctx, offset, align);
        if (result < 0) {
            return -1;  /* 读取错误 */
        }
        if (result > 0) {
            /* 找到第一个空闲位置 */
            if (offset > 0) {
                /* 前一个位置是最后写入的位置 */
                ctx->last_write_offset = offset - align;
            }
            ctx->next_write_offset = offset;
            return 0;
        }
    }

    /* 全部已写入，需要擦除后才能写入 */
    if (ctx->total_size >= align) {
        ctx->last_write_offset = ctx->total_size - align;
    }
    ctx->next_write_offset = 0;
    return 0;
}
