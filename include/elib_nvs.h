/* elib_nvs.h - NVS Wear Leveling (single-umbrella public API)
 *
 * Append-only wear leveling over flash/EEPROM-style media: every record
 * occupies one fixed-size data unit, writes march forward one unit at a
 * time and the whole managed region is erased (lazily, at the next write
 * that can no longer find a slot) when exhausted. Erase wear is spread
 * across every managed page without a wear table.
 *
 * Rules of the game:
 *  - zero dynamic memory: the user allocates the context, the library
 *    allocates nothing (not even a scratch buffer - emptiness checks are
 *    delegated to the user's is_empty op)
 *  - the library only ever stores "the newest record"; data integrity
 *    (torn writes, CRC) is out of scope here - pair with elib-safe-storage
 *  - modifying the managed region outside this library is undefined, but
 *    the window-based relocation heals it at runtime */
#ifndef ELIB_NVS_H
#define ELIB_NVS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- error codes ---- */

typedef enum {
    ELIB_NVS_OK = 0,
    ELIB_NVS_ERR_INVALID_PARAM,   /* init rejected the configuration */
    ELIB_NVS_ERR_NOT_INITIALIZED, /* API called before a successful init */
    ELIB_NVS_ERR_IO,              /* a user op returned non-zero */
    ELIB_NVS_ERR_EMPTY            /* nothing written to the region yet */
} elib_nvs_err_t;

/* ---- user media operations ----
 *
 * read/write/erase return 0 on success and any non-zero value of the
 * user's own choosing on failure (the library maps that to
 * ELIB_NVS_ERR_IO). erase is only ever invoked on page-aligned regions.
 *
 * is_empty must report 1 exactly when [addr, addr+len) is empty by the
 * user's own definition (typically: every byte equals the erased value
 * the media produces). The library never inspects record bytes itself,
 * so it takes no configuration for this. Implement it the way that
 * suits the media best (memory-mapped compare, word-wise sweep,
 * driver-assisted check...). */
typedef struct {
    void *user_data; /* forwarded to every op, untouched by the library */
    int     (*read)    (void *ud, uint32_t addr, uint8_t *buf, uint32_t len);
    int     (*write)   (void *ud, uint32_t addr, const uint8_t *buf, uint32_t len);
    int     (*erase)   (void *ud, uint32_t addr, uint32_t len);
    uint8_t (*is_empty)(void *ud, uint32_t addr, uint32_t len);
} elib_nvs_ops_t;

/* ---- context (statically allocated by the user) ---- */
typedef struct {
    /* configuration (copied at init, kept for reset/erase) */
    elib_nvs_ops_t ops;
    uint32_t start_addr;      /* physical start of the managed region */
    uint32_t managed_size;    /* page-aligned size of the managed region */
    uint32_t erase_page_size; /* smallest erase unit of the media */
    uint32_t unit_size;       /* fixed record size in bytes */

    /* runtime state */
    uint32_t unit_count;      /* managed_size / unit_size (>= 1) */
    uint32_t read_unit;       /* unit index of the newest record */
    uint32_t write_unit;      /* unit index of the next write slot; always
                                   sits on a verified-empty unit (or head) */
    struct {
        uint8_t initialized : 1;
        uint8_t has_data    : 1; /* read_unit holds a valid record */
    } bit_flags;
} elib_nvs_ctx_t;

/* ---- API ---- */

/**
 * @brief 校验配置、定位当前位置并初始化上下文
 * @param ctx 用户静态分配的上下文
 * @param ops 四个介质操作（read/write/erase/is_empty，均不可为 NULL）
 * @param start_addr 管理区物理起始地址（须按擦除页对齐）
 * @param managed_size 管理区大小（须为擦除页整数倍，且 >= 擦除页）
 * @param erase_page_size 介质最小擦除页大小（须非 0）
 * @param unit_size 数据单元大小（1 <= unit_size <= managed_size，可跨页）
 * @return ELIB_NVS_OK，或 ELIB_NVS_ERR_INVALID_PARAM */
elib_nvs_err_t elib_nvs_init(elib_nvs_ctx_t *ctx, const elib_nvs_ops_t *ops,
                             uint32_t start_addr, uint32_t managed_size,
                             uint32_t erase_page_size, uint32_t unit_size);

/**
 * @brief 读取最新一条记录（恒定 unit_size 字节）
 * @return ELIB_NVS_OK / ELIB_NVS_ERR_EMPTY / ELIB_NVS_ERR_IO / ...
 *         ELIB_NVS_ERR_NOT_INITIALIZED */
elib_nvs_err_t elib_nvs_read(elib_nvs_ctx_t *ctx, uint8_t *buf);

/**
 * @brief 追加一条记录；找不到合法槽位时先惰性全域擦除再从头写
 * @param buf 定长 unit_size 字节的记录内容
 * @return ELIB_NVS_OK / ELIB_NVS_ERR_IO / ELIB_NVS_ERR_NOT_INITIALIZED */
elib_nvs_err_t elib_nvs_write(elib_nvs_ctx_t *ctx, const uint8_t *buf);

/**
 * @brief 重新定位（重扫描）读/写位置，不做任何物理擦除
 * @return ELIB_NVS_OK / ELIB_NVS_ERR_NOT_INITIALIZED */
elib_nvs_err_t elib_nvs_reset(elib_nvs_ctx_t *ctx);

/**
 * @brief 物理擦除整个管理区，回到全新空介质状态
 * @return ELIB_NVS_OK / ELIB_NVS_ERR_IO / ELIB_NVS_ERR_NOT_INITIALIZED */
elib_nvs_err_t elib_nvs_erase_all(elib_nvs_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ELIB_NVS_H */
