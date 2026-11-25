/* elib_nvs.h - NVS Wear Leveling (single-umbrella public API)
 *
 * Pure wear leveling over an EEPROM-like address space: every write lands on
 * the next fixed-size slot and the pointer wraps around, so write wear is
 * spread evenly across the whole managed region.
 *
 * The library owns a 5-byte header at the head of every slot:
 *
 *     [ seq: 4 bytes ][ magic: 1 byte ][ user data: user_size ][ padding ]
 *
 *   seq   monotonic counter used to locate "the latest record"
 *   magic 0xA5 once written; erased media (all-0x00 or all-0xFF) never read
 *         back as 0xA5, so validity is independent of erase polarity
 *
 * Layout / commit rules:
 *  - a write stores the user data first and the header array last, so a
 *    slot only counts as valid once its data is already in place
 *  - magic is the last byte of the header array and therefore the commit point
 *  - locating "the latest" is a full scan: every slot with magic == 0xA5 is a
 *    candidate, and the one with the greatest seq (modular compare) wins
 *
 * The library treats every medium as a byte-writable EEPROM: it never erases,
 * never checks emptiness and knows nothing about pages. Media that must erase
 * before programming (NOR flash) have to hide that inside the user's write
 * op (read-modify-write of the containing page).
 *
 * Data integrity is NOT guaranteed: a torn write may be read back, and on
 * block-erase media a whole page can be lost. Pair with elib-safe-storage
 * (dual region + CRC) whenever that matters.
 */
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

/* Library-owned header layout. ELIB_NVS_HDR_SIZE is the byte count stored at
 * the head of every slot; ELIB_NVS_MAGIC marks a slot as written. */
#define ELIB_NVS_HDR_SIZE 5u
#define ELIB_NVS_MAGIC    0xA5u

/* ---- user media operations ----
 *
 * The library sees a byte-writable EEPROM: only read and write, no erase and
 * no emptiness probe. Both return 0 on success and any non-zero value of the
 * user's own choosing on failure (mapped to ELIB_NVS_ERR_IO).
 *
 * Media that need an erase before a write (NOR flash) must implement that
 * inside write: read the containing page, merge, erase, write back. That
 * costs erase wear and latency and is the user's responsibility.
 */
typedef struct {
    void *user_data; /* forwarded to every op, untouched by the library */
    int (*read) (void *ud, uint32_t addr, uint8_t *buf, uint32_t len);
    int (*write)(void *ud, uint32_t addr, const uint8_t *buf, uint32_t len);
} elib_nvs_ops_t;

/* ---- context (statically allocated by the user) ---- */
typedef struct {
    /* configuration (copied at init) */
    elib_nvs_ops_t ops;
    uint32_t start_addr;   /* physical start of the managed region */
    uint32_t managed_size; /* size of the managed region */
    uint32_t slot_size;    /* slot stride; the whole slot, header included */
    uint32_t user_size;    /* user payload bytes inside one slot */

    /* runtime state */
    uint32_t slot_count;   /* managed_size / slot_size (>= 2) */
    uint32_t read_slot;    /* slot index of the newest record */
    uint32_t write_slot;   /* slot index of the next write; (read_slot+1)%N */
    uint32_t next_seq;     /* seq value the next write will use */
    struct {
        uint8_t initialized : 1;
        uint8_t has_data    : 1; /* read_slot holds a valid record */
    } bit_flags;
} elib_nvs_ctx_t;

/* ---- API ---- */

/**
 * @brief 校验配置、扫描定位最新记录并初始化上下文
 * @param ctx 用户静态分配的上下文
 * @param ops read/write 两个介质操作（均不可为 NULL）
 * @param start_addr 管理区物理起始地址（无页对齐要求）
 * @param managed_size 管理区大小（>= 2 * slot_size）
 * @param slot_size 槽大小（遍历步长，含 5 字节头部与填充）
 * @param user_size 用户数据大小（ELIB_NVS_HDR_SIZE + user_size <= slot_size）
 * @return ELIB_NVS_OK / ELIB_NVS_ERR_INVALID_PARAM / ELIB_NVS_ERR_IO
 */
elib_nvs_err_t elib_nvs_init(elib_nvs_ctx_t *ctx, const elib_nvs_ops_t *ops,
                             uint32_t start_addr, uint32_t managed_size,
                             uint32_t slot_size, uint32_t user_size);

/**
 * @brief 读取最新一条记录（恒定 user_size 字节）
 * @return ELIB_NVS_OK / ELIB_NVS_ERR_EMPTY / ELIB_NVS_ERR_IO /
 *         ELIB_NVS_ERR_INVALID_PARAM / ELIB_NVS_ERR_NOT_INITIALIZED
 */
elib_nvs_err_t elib_nvs_read(elib_nvs_ctx_t *ctx, uint8_t *buf);

/**
 * @brief 追加一条记录；写到下一个槽，写到末尾自动回绕覆盖最旧槽
 * @param buf 定长 user_size 字节的记录内容
 * @return ELIB_NVS_OK / ELIB_NVS_ERR_IO / ELIB_NVS_ERR_INVALID_PARAM /
 *         ELIB_NVS_ERR_NOT_INITIALIZED
 */
elib_nvs_err_t elib_nvs_write(elib_nvs_ctx_t *ctx, const uint8_t *buf);

/**
 * @brief 重新扫描定位读/写位置，不做任何介质写
 * @return ELIB_NVS_OK / ELIB_NVS_ERR_IO / ELIB_NVS_ERR_NOT_INITIALIZED
 */
elib_nvs_err_t elib_nvs_reset(elib_nvs_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ELIB_NVS_H */
