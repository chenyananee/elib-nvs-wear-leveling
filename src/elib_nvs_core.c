/* elib_nvs_core.c - NVS Wear Leveling implementation
 *
 * One slot (slot_size bytes):
 *
 *   +----------+----------+----------------------+-----------+
 *   | seq (4B) | magic 1B | user data user_size  | padding   |
 *   +----------+----------+----------------------+-----------+
 *    \________ library header (5B) ________/
 *
 * Write order is "user data first, header array last", so the magic byte (the
 * last byte of the header) is the commit point: a slot only counts as valid
 * once its data is already in place.
 *
 * Locating the newest record is a FULL scan. A slot is a candidate only when
 * its magic byte reads 0xA5; among the candidates the greatest seq wins under
 * a wrap-safe modular comparison. Scanning must never stop at the first
 * invalid header: after a wrap a torn write can leave slot 0 invalid while the
 * slots behind it still hold valid (older) records.
 */
#include <stddef.h>

#include "elib_nvs.h"

/* Physical address of slot index */
static uint32_t slot_addr(const elib_nvs_ctx_t *ctx, uint32_t idx) {
    return ctx->start_addr + idx * ctx->slot_size;
}

/* Wrap-safe "a is newer than b" for the 32-bit sequence numbers. Valid as long
 * as the live records span less than half of the counter range, which always
 * holds here because there are only slot_count of them. */
static int seq_is_greater(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

/* Read the 5-byte header of one slot. Returns 0 on success, -1 on I/O error.
 * Sets *valid to 1 when the magic byte marks a written slot, and *seq to the
 * stored sequence number (meaningful only when *valid is 1). */
static int read_header(const elib_nvs_ctx_t *ctx, uint32_t idx,
                       uint32_t *seq, uint8_t *valid) {
    uint8_t hdr[ELIB_NVS_HDR_SIZE];

    if (ctx->ops.read(ctx->ops.user_data, slot_addr(ctx, idx), hdr,
                      ELIB_NVS_HDR_SIZE) != 0) {
        return -1;
    }
    *valid = (hdr[4] == ELIB_NVS_MAGIC) ? 1u : 0u;
    *seq = (uint32_t)hdr[0] |
           ((uint32_t)hdr[1] << 8) |
           ((uint32_t)hdr[2] << 16) |
           ((uint32_t)hdr[3] << 24);
    return 0;
}

/* Full-scan relocation. Purely read-only, shared by init and reset.
 * Returns 0 on success and -1 when a header read failed (the context is left
 * untouched in that case). */
static int nvs_locate(elib_nvs_ctx_t *ctx) {
    uint32_t i;
    uint32_t best_slot = 0u;
    uint32_t best_seq = 0u;
    uint8_t best_valid = 0u;

    for (i = 0u; i < ctx->slot_count; ++i) {
        uint32_t seq;
        uint8_t valid;

        if (read_header(ctx, i, &seq, &valid) != 0) {
            return -1;
        }
        if (!valid) {
            continue;
        }
        if (!best_valid || seq_is_greater(seq, best_seq)) {
            best_seq = seq;
            best_slot = i;
            best_valid = 1u;
        }
    }

    if (best_valid) {
        ctx->read_slot = best_slot;
        ctx->write_slot = (best_slot + 1u < ctx->slot_count)
                              ? best_slot + 1u : 0u;
        ctx->next_seq = best_seq + 1u;
        ctx->bit_flags.has_data = 1u;
    } else {
        ctx->read_slot = 0u;
        ctx->write_slot = 0u;
        ctx->next_seq = 1u;
        ctx->bit_flags.has_data = 0u;
    }
    return 0;
}

/* ---- API ---- */

elib_nvs_err_t elib_nvs_init(elib_nvs_ctx_t *ctx, const elib_nvs_ops_t *ops,
                             uint32_t start_addr, uint32_t managed_size,
                             uint32_t slot_size, uint32_t user_size) {
    uint32_t slot_count;

    if (ctx == NULL || ops == NULL) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }
    if (ops->read == NULL || ops->write == NULL) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }
    /* Overflow-safe form of "ELIB_NVS_HDR_SIZE + user_size <= slot_size" */
    if (user_size == 0u || slot_size < ELIB_NVS_HDR_SIZE ||
        slot_size - ELIB_NVS_HDR_SIZE < user_size) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }
    if ((uint64_t)start_addr + (uint64_t)managed_size > 0xFFFFFFFFULL) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }

    slot_count = managed_size / slot_size;
    if (slot_count < 2u) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }

    ctx->ops = *ops;
    ctx->start_addr = start_addr;
    ctx->managed_size = managed_size;
    ctx->slot_size = slot_size;
    ctx->user_size = user_size;
    ctx->slot_count = slot_count;
    ctx->bit_flags.initialized = 1u;
    ctx->bit_flags.has_data = 0u;

    if (nvs_locate(ctx) != 0) {
        ctx->bit_flags.initialized = 0u;
        return ELIB_NVS_ERR_IO;
    }
    return ELIB_NVS_OK;
}

elib_nvs_err_t elib_nvs_read(elib_nvs_ctx_t *ctx, uint8_t *buf) {
    if (ctx == NULL || !ctx->bit_flags.initialized) {
        return ELIB_NVS_ERR_NOT_INITIALIZED;
    }
    if (buf == NULL) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }
    if (!ctx->bit_flags.has_data) {
        return ELIB_NVS_ERR_EMPTY;
    }
    if (ctx->ops.read(ctx->ops.user_data,
                      slot_addr(ctx, ctx->read_slot) + ELIB_NVS_HDR_SIZE,
                      buf, ctx->user_size) != 0) {
        return ELIB_NVS_ERR_IO;
    }
    return ELIB_NVS_OK;
}

elib_nvs_err_t elib_nvs_write(elib_nvs_ctx_t *ctx, const uint8_t *buf) {
    uint8_t hdr[ELIB_NVS_HDR_SIZE];
    uint32_t slot;
    uint32_t seq;

    if (ctx == NULL || !ctx->bit_flags.initialized) {
        return ELIB_NVS_ERR_NOT_INITIALIZED;
    }
    if (buf == NULL) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }

    slot = ctx->write_slot;
    seq = ctx->next_seq;

    /* 1) user data: a slot is only marked valid once its data is in place */
    if (ctx->ops.write(ctx->ops.user_data,
                       slot_addr(ctx, slot) + ELIB_NVS_HDR_SIZE,
                       buf, ctx->user_size) != 0) {
        return ELIB_NVS_ERR_IO;
    }

    /* 2) header array [seq][magic]; magic is the last byte = commit point */
    hdr[0] = (uint8_t)(seq & 0xFFu);
    hdr[1] = (uint8_t)((seq >> 8) & 0xFFu);
    hdr[2] = (uint8_t)((seq >> 16) & 0xFFu);
    hdr[3] = (uint8_t)((seq >> 24) & 0xFFu);
    hdr[4] = (uint8_t)ELIB_NVS_MAGIC;
    if (ctx->ops.write(ctx->ops.user_data, slot_addr(ctx, slot), hdr,
                       ELIB_NVS_HDR_SIZE) != 0) {
        return ELIB_NVS_ERR_IO;
    }

    ctx->read_slot = slot;
    ctx->bit_flags.has_data = 1u;
    ctx->write_slot = (slot + 1u < ctx->slot_count) ? slot + 1u : 0u;
    ctx->next_seq = seq + 1u;
    return ELIB_NVS_OK;
}

elib_nvs_err_t elib_nvs_reset(elib_nvs_ctx_t *ctx) {
    if (ctx == NULL || !ctx->bit_flags.initialized) {
        return ELIB_NVS_ERR_NOT_INITIALIZED;
    }
    if (nvs_locate(ctx) != 0) {
        return ELIB_NVS_ERR_IO;
    }
    return ELIB_NVS_OK;
}
