/* elib_nvs_core.c - NVS Wear Leveling implementation
 *
 * Single-file implementation of the append-only wear leveling model.
 * Terminology:
 *   unit        - one fixed logical record slot, unit_size bytes
 *   unit index  - 0-based, relative to the start of the managed region
 *   legal slot  - a unit on which the next payload may land:
 *                 (k, k+1) both empty, or k is the last unit and empty
 *   full        - no legal slot anywhere: lazy global erase is due
 *
 * Positioning invariant (runtime and reboot agree on the same predicate):
 * the write pointer always rests on a verified-empty unit, and there is
 * no empty unit anywhere between the start of the filled prefix and the
 * landing slot - illegal windows (containing a non-empty unit, e.g. an
 * out-of-library orphan) are skipped by the shared seek, which is why no
 * dirt-marking writes are ever needed.
 */
#include <stddef.h>

#include "elib_nvs.h"

/* ---- helpers ---- */

/* Physical address of unit index */
static uint32_t unit_addr(const elib_nvs_ctx_t *ctx, uint32_t idx) {
    return ctx->start_addr + idx * ctx->unit_size;
}

/* Shared slot seeker. Scans unit windows starting at "from":
 *   (k, k+1) both empty                  -> slot = k
 *   k is the last unit and empty         -> single-slot clause, slot = k
 *   slid off the end without a slot      -> full (out_slot = 0)
 * Returns 0 when a slot was found, 1 when the domain is full.
 * Read-only: never performs a media write. */
static uint8_t nvs_seek(const elib_nvs_ctx_t *ctx, uint32_t from,
                        uint32_t *out_slot) {
    const elib_nvs_ops_t *ops = &ctx->ops;

    if (from >= ctx->unit_count) {
        return 1; /* nothing behind the pointer can hold a slot */
    }

    {
        uint32_t k = from;
        for (;;) {
            if (k + 1 < ctx->unit_count) {
                if (ops->is_empty(ops->user_data, unit_addr(ctx, k),
                                  ctx->unit_size) &&
                    ops->is_empty(ops->user_data, unit_addr(ctx, k + 1),
                                  ctx->unit_size)) {
                    *out_slot = k;
                    return 0;
                }
            } else {
                /* Last unit: "space for fewer than two" - one empty
                 * unit still qualifies as a legal slot */
                if (ops->is_empty(ops->user_data, unit_addr(ctx, k),
                                  ctx->unit_size)) {
                    *out_slot = k;
                    return 0;
                }
                /* Last unit holds data and nothing follows: the domain
                 * is full. The caller decides the full-domain outcome */
                *out_slot = 0;
                return 1;
            }
            k++;
        }
    }
}

/* Position the context by scanning. Purely read-only, shared by init
 * and reset. Three entry outcomes:
 *   slot k > 0  -> newest record = unit k-1, next write = unit k
 *   slot k == 0 -> fresh/empty domain: no record, next write = unit 0
 *   full        -> newest record = last unit, next write = head
 *                  (every library fold resets at the head, so a fully
 *                  populated domain was filled 0..N-1 in order) */
static void nvs_locate(elib_nvs_ctx_t *ctx) {
    uint32_t slot;

    if (!nvs_seek(ctx, 0, &slot)) {
        ctx->write_unit = slot;
        if (slot > 0) {
            ctx->read_unit = slot - 1;
            ctx->bit_flags.has_data = 1;
        } else {
            ctx->bit_flags.has_data = 0;
        }
    } else {
        ctx->read_unit = ctx->unit_count - 1;
        ctx->write_unit = 0;
        ctx->bit_flags.has_data = 1;
    }
}

/* ---- API ---- */

elib_nvs_err_t elib_nvs_init(elib_nvs_ctx_t *ctx, const elib_nvs_ops_t *ops,
                             uint32_t start_addr, uint32_t managed_size,
                             uint32_t erase_page_size, uint32_t unit_size,
                             uint8_t erase_default) {
    if (ctx == NULL || ops == NULL) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }
    if (ops->read == NULL || ops->write == NULL || ops->erase == NULL ||
        ops->is_empty == NULL) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }
    if (erase_page_size == 0 || unit_size == 0) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }
    if ((start_addr % erase_page_size) != 0 ||
        (managed_size % erase_page_size) != 0) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }
    if (managed_size < erase_page_size) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }
    if (unit_size > managed_size) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }
    if ((uint64_t)start_addr + (uint64_t)managed_size > 0xFFFFFFFFULL) {
        return ELIB_NVS_ERR_INVALID_PARAM; /* region must not wrap */
    }

    ctx->ops = *ops;
    ctx->start_addr = start_addr;
    ctx->managed_size = managed_size;
    ctx->erase_page_size = erase_page_size;
    ctx->unit_size = unit_size;
    ctx->erase_default = erase_default;
    ctx->unit_count = managed_size / unit_size;
    ctx->bit_flags.initialized = 1;

    nvs_locate(ctx);
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
    if (ctx->ops.read(ctx->ops.user_data, unit_addr(ctx, ctx->read_unit),
                      buf, ctx->unit_size) != 0) {
        return ELIB_NVS_ERR_IO;
    }
    return ELIB_NVS_OK;
}

elib_nvs_err_t elib_nvs_write(elib_nvs_ctx_t *ctx, const uint8_t *buf) {
    uint32_t slot;

    if (ctx == NULL || !ctx->bit_flags.initialized) {
        return ELIB_NVS_ERR_NOT_INITIALIZED;
    }
    if (buf == NULL) {
        return ELIB_NVS_ERR_INVALID_PARAM;
    }

    if (nvs_seek(ctx, ctx->write_unit, &slot)) {
        /* Full: lazy global erase fires only now - the record in the
         * last unit has been readable right up to this replacement */
        if (ctx->ops.erase(ctx->ops.user_data, ctx->start_addr,
                           ctx->managed_size) != 0) {
            return ELIB_NVS_ERR_IO; /* seek re-locates on the next write */
        }
        ctx->bit_flags.has_data = 0; /* old records are gone now */
        slot = 0;
    }

    if (ctx->ops.write(ctx->ops.user_data, unit_addr(ctx, slot),
                       buf, ctx->unit_size) != 0) {
        return ELIB_NVS_ERR_IO; /* previous record (if any) still reads */
    }

    ctx->read_unit = slot;
    ctx->bit_flags.has_data = 1;
    /* The slot's partner unit was verified empty inside the seek (or the
     * domain was just erased), so the advance needs no further check */
    ctx->write_unit = (slot + 1 < ctx->unit_count) ? (slot + 1) : 0;
    return ELIB_NVS_OK;
}

elib_nvs_err_t elib_nvs_reset(elib_nvs_ctx_t *ctx) {
    if (ctx == NULL || !ctx->bit_flags.initialized) {
        return ELIB_NVS_ERR_NOT_INITIALIZED;
    }
    nvs_locate(ctx);
    return ELIB_NVS_OK;
}

elib_nvs_err_t elib_nvs_erase_all(elib_nvs_ctx_t *ctx) {
    if (ctx == NULL || !ctx->bit_flags.initialized) {
        return ELIB_NVS_ERR_NOT_INITIALIZED;
    }
    if (ctx->ops.erase(ctx->ops.user_data, ctx->start_addr,
                       ctx->managed_size) != 0) {
        return ELIB_NVS_ERR_IO;
    }
    ctx->write_unit = 0;
    ctx->bit_flags.has_data = 0;
    return ELIB_NVS_OK;
}
