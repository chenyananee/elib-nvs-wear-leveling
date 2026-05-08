/* driver_template.c - NVS HAL Driver Template */
#include "elib_nvs_hal.h"
#include <stdint.h>
#include <string.h>

/* ============================================================
 * 用户自定义驱动数据结构
 * ============================================================ */
typedef struct {
    /* Flash/EEPROM 硬件句柄 */
    void *hw_handle;
    /* 存储总大小 */
    uint32_t total_size;
    /* 擦除块大小 */
    uint32_t erase_size;
} my_storage_driver_t;

/* ============================================================
 * 读函数实现
 * ============================================================ */
static elib_nvs_err_t my_storage_read(void *user_data, elib_nvs_addr_t addr, void *buf, size_t len) {
    my_storage_driver_t *drv = (my_storage_driver_t *)user_data;

    /* TODO: 替换为实际的读操作 */
    /* 示例：spi_flash_read(drv->hw_handle, addr, buf, len); */

    memset(buf, 0, len); /* 占位实现 */
    return ELIB_NVS_OK;
}

/* ============================================================
 * 写函数实现
 * ============================================================ */
static elib_nvs_err_t my_storage_write(void *user_data, elib_nvs_addr_t addr, const void *buf, size_t len) {
    my_storage_driver_t *drv = (my_storage_driver_t *)user_data;

    /* TODO: 替换为实际的写操作 */
    /* 示例：spi_flash_write(drv->hw_handle, addr, buf, len); */

    return ELIB_NVS_OK;
}

/* ============================================================
 * 擦除函数实现
 * ============================================================ */
static elib_nvs_err_t my_storage_erase(void *user_data, elib_nvs_addr_t addr, size_t size) {
    my_storage_driver_t *drv = (my_storage_driver_t *)user_data;

    /* TODO: 替换为实际的擦除操作 */
    /* 示例：spi_flash_erase(drv->hw_handle, addr, size); */

    return ELIB_NVS_OK;
}

/* ============================================================
 * 获取信息函数实现 (可选)
 * ============================================================ */
static elib_nvs_err_t my_storage_get_info(void *user_data, uint32_t *total_size, uint32_t *erase_size) {
    my_storage_driver_t *drv = (my_storage_driver_t *)user_data;

    if (total_size != NULL) {
        *total_size = drv->total_size;
    }
    if (erase_size != NULL) {
        *erase_size = drv->erase_size;
    }

    return ELIB_NVS_OK;
}

/* ============================================================
 * 驱动注册 (用户初始化代码)
 * ============================================================ */
void my_storage_init(void) {
    static my_storage_driver_t g_drv = {
        .hw_handle = NULL,      /* 初始化硬件句柄 */
        .total_size = 1024 * 1024,  /* 1MB */
        .erase_size = 4096      /* 4KB per block */
    };

    /* V4 API: 用户分配上下文，所有配置通过参数传入 (去除了多余的 resources 结构) */
    /* 注意：用户数据现在直接存储在 driver 结构中，消除了重复结构 */
    static elib_nvs_ctx_t g_ctx;

    /* 硬件驱动接口 - 用户数据现在直接存储在驱动结构中 */
    elib_nvs_hal_driver_t hal_driver = {
        .user_data = &g_drv,              /* 用户数据存储在驱动结构内 */
        .read = my_storage_read,
        .write = my_storage_write,
        .erase = my_storage_erase,
        .get_info = my_storage_get_info
    };

    /* V4 初始化函数不再接收单独的 resources 参数
     * 所有硬件相关数据统一通过 hal_driver 接口提供 */
    elib_nvs_err_t err = elib_nvs_init(
        &g_ctx,
        &hal_driver,                 /* 驱动接口直接包含用户数据 */
        0x08000000,                 /* start_addr: 起始物理地址 */
        1024 * 1024,               /* total_size: 总空间 1MB */
        4096,                      /* block_size: 区块大小 4KB */
        1024                       /* data_unit_size: 数据单元大小 1KB */
    );
    if (err != ELIB_NVS_OK) {
        /* 处理初始化错误 */
        return;
    }

    /* 使用 NVS (MTD 模式示例 - 不需要地址) */
    uint8_t data[] = "hello";
    elib_nvs_mtd_write(&g_ctx, data, sizeof(data));

    /* 读取最后写入的数据 */
    uint8_t buffer[sizeof(data)];
    err = elib_nvs_mtd_read(&g_ctx, buffer, sizeof(buffer));
    if (err == ELIB_NVS_OK) {
        /* 成功读取到数据 */
    }

    /* 清理资源 */
    elib_nvs_deinit(&g_ctx);
}
