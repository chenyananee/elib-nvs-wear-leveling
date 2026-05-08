/* elib_nvs_types.h - NVS Wear Leveling Type Definitions */
#ifndef ELIB_NVS_TYPES_H
#define ELIB_NVS_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include "elib_nvs_err.h"

/* 逻辑块 ID 类型 */
typedef uint32_t elib_nvs_block_id_t;

/* 物理地址类型 */
typedef uint32_t elib_nvs_addr_t;

/* 磨损计数器类型 */
typedef uint32_t elib_nvs_wear_count_t;

/* 配置结构 */
typedef struct {
    uint32_t total_blocks;
    uint32_t block_size;
    uint32_t max_key_len;
    uint32_t max_value_len;
    uint32_t wear_threshold;
} elib_nvs_config_t;

/* HAL 驱动接口 */
typedef struct {
    void *user_data;
    elib_nvs_err_t (*read)(void *user_data, elib_nvs_addr_t addr, void *buf, size_t len);
    elib_nvs_err_t (*write)(void *user_data, elib_nvs_addr_t addr, const void *buf, size_t len);
    elib_nvs_err_t (*erase)(void *user_data, elib_nvs_addr_t addr, size_t size);
    elib_nvs_err_t (*get_info)(void *user_data, uint32_t *total_size, uint32_t *erase_size);
} elib_nvs_hal_driver_t;

/* 上下文结构体 - 用户分配实例 */
typedef struct elib_nvs_ctx elib_nvs_ctx_t;

/* Full struct definition (internal but visible for static allocation) */
struct elib_nvs_ctx {
    elib_nvs_hal_driver_t driver;

    /* 用户配置参数 (初始化时传入) */
    uint32_t start_addr;        /* 管理区域起始物理地址 */
    uint32_t total_size;        /* 总管理空间大小 */
    uint32_t block_size;        /* 扇区/块大小 */
    uint32_t data_unit_size;    /* 固定写入数据大小 */

    /* 运行时状态 */
    uint32_t next_write_offset; /* 下次写入位置 (相对于 start_addr 的偏移) */
    uint32_t last_write_offset; /* 最近一次写入位置 (新增，用于读取定位) */

    int initialized;
};

#endif /* ELIB_NVS_TYPES_H */
