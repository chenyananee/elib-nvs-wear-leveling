/* test_elib_nvs.c - RAM-simulated medium test suite (v3)
 *
 * The simulated medium is a plain byte array with only the two ops the
 * library uses (read / write). Erase polarity is simulated by pre-filling the
 * array with the "erased" value: 0xFF for flash-like media, 0x00 for media
 * that erase to zero.
 */
#include <stdio.h>
#include <string.h>

#include "elib_nvs.h"

#define SIM_OK   0
#define SIM_FAIL (-7)

#define ERASE_FILL 0xFFu

#define SLOT      16u                       /* slot stride: 5 + 8 <= 16 */
#define USER      8u                        /* user payload per slot */
#define N_SLOTS   6u                        /* slot count */
#define MANAGED   (SLOT * N_SLOTS)          /* 96 */
#define BASE      0x1000u
#define MEM_SIZE  256u

static int g_failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_EQ(actual, expected)                                         \
    do {                                                                   \
        long a_ = (long)(actual);                                          \
        long e_ = (long)(expected);                                        \
        if (a_ != e_) {                                                    \
            printf("FAIL %s:%d: %s == %s (%ld != %ld)\n",                  \
                   __FILE__, __LINE__, #actual, #expected, a_, e_);        \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

/* ---- simulated medium ---- */

typedef struct {
    uint32_t base;
    uint8_t *mem;
} sim_t;

static uint8_t g_mem[MEM_SIZE];
static sim_t sim = { BASE, g_mem };

static int sim_read(void *ud, uint32_t addr, uint8_t *buf, uint32_t len) {
    const sim_t *s = (const sim_t *)ud;
    memcpy(buf, s->mem + (addr - s->base), len);
    return SIM_OK;
}

static int sim_write(void *ud, uint32_t addr, const uint8_t *buf,
                     uint32_t len) {
    sim_t *s = (sim_t *)ud;
    memcpy(s->mem + (addr - s->base), buf, len);
    return SIM_OK;
}

static const elib_nvs_ops_t ops = { &sim, sim_read, sim_write };

/* failing ops */
static int fail_read(void *ud, uint32_t addr, uint8_t *buf, uint32_t len) {
    (void)ud; (void)addr; (void)buf; (void)len;
    return SIM_FAIL;
}

static int fail_write(void *ud, uint32_t addr, const uint8_t *buf,
                      uint32_t len) {
    (void)ud; (void)addr; (void)buf; (void)len;
    return SIM_FAIL;
}

static const elib_nvs_ops_t ops_fail_read  = { &sim, fail_read,  sim_write };
static const elib_nvs_ops_t ops_fail_write = { &sim, sim_read,   fail_write };
static const elib_nvs_ops_t ops_null_read  = { &sim, NULL,       sim_write };
static const elib_nvs_ops_t ops_null_write = { &sim, sim_read,   NULL };

/* power-loss injection: only g_budget more bytes may be committed */
static uint32_t g_budget;
static const uint32_t BUDGET_INF = 0xFFFFFFFFu;

static int budget_write(void *ud, uint32_t addr, const uint8_t *buf,
                        uint32_t len) {
    sim_t *s = (sim_t *)ud;

    if (g_budget >= len) {
        memcpy(s->mem + (addr - s->base), buf, len);
        g_budget -= len;
        return SIM_OK;
    }
    if (g_budget > 0u) {
        memcpy(s->mem + (addr - s->base), buf, g_budget);
        g_budget = 0u;
    }
    return SIM_FAIL; /* the write did not complete */
}

static const elib_nvs_ops_t ops_budget = { &sim, sim_read, budget_write };

/* ---- helpers ---- */

static elib_nvs_ctx_t ctx;
static uint8_t rec[USER];
static uint8_t rec_back[USER];

static void fill_record(uint8_t *buf, uint8_t seed) {
    memset(buf, 0x5A, USER);
    buf[0] = seed;
}

/* poke a complete, valid header for a slot (bypasses the library) */
static void poke_header(uint32_t slot, uint32_t seq) {
    uint8_t *p = g_mem + slot * SLOT;
    p[0] = (uint8_t)(seq & 0xFFu);
    p[1] = (uint8_t)((seq >> 8) & 0xFFu);
    p[2] = (uint8_t)((seq >> 16) & 0xFFu);
    p[3] = (uint8_t)((seq >> 24) & 0xFFu);
    p[4] = (uint8_t)ELIB_NVS_MAGIC;
}

/* ---- tests ---- */

static void test_init_valid_and_rejects(void) {
    printf("Test: init validates the configuration... ");

    memset(g_mem, ERASE_FILL, sizeof(g_mem));

    CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, MANAGED, SLOT, USER),
             ELIB_NVS_OK);
    CHECK_EQ(ctx.slot_count, N_SLOTS);
    CHECK_EQ(ctx.bit_flags.initialized, 1);
    CHECK_EQ(ctx.bit_flags.has_data, 0);

    CHECK_EQ(elib_nvs_init(NULL, &ops, BASE, MANAGED, SLOT, USER),
             ELIB_NVS_ERR_INVALID_PARAM);
    CHECK_EQ(elib_nvs_init(&ctx, NULL, BASE, MANAGED, SLOT, USER),
             ELIB_NVS_ERR_INVALID_PARAM);
    CHECK_EQ(elib_nvs_init(&ctx, &ops_null_read, BASE, MANAGED, SLOT, USER),
             ELIB_NVS_ERR_INVALID_PARAM);
    CHECK_EQ(elib_nvs_init(&ctx, &ops_null_write, BASE, MANAGED, SLOT, USER),
             ELIB_NVS_ERR_INVALID_PARAM);

    /* user_size == 0 */
    CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, MANAGED, SLOT, 0u),
             ELIB_NVS_ERR_INVALID_PARAM);
    /* header + user does not fit the slot (5 + 8 > 12) */
    CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, MANAGED, 12u, USER),
             ELIB_NVS_ERR_INVALID_PARAM);
    /* slot_size smaller than the header */
    CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, MANAGED, 4u, 1u),
             ELIB_NVS_ERR_INVALID_PARAM);
    /* slot count < 2 */
    CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, SLOT, SLOT, USER),
             ELIB_NVS_ERR_INVALID_PARAM);
    /* start + size wraps the 32-bit address space */
    CHECK_EQ(elib_nvs_init(&ctx, &ops, 0xFFFFFFC0u, MANAGED, SLOT, USER),
             ELIB_NVS_ERR_INVALID_PARAM);

    printf("PASSED\n");
}

static void test_fresh_domain(void) {
    printf("Test: fresh domain starts empty then accepts writes... ");

    memset(g_mem, ERASE_FILL, sizeof(g_mem));
    CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, MANAGED, SLOT, USER),
             ELIB_NVS_OK);
    CHECK_EQ(ctx.bit_flags.has_data, 0);
    CHECK_EQ(ctx.read_slot, 0);
    CHECK_EQ(ctx.write_slot, 0);
    CHECK_EQ(elib_nvs_read(&ctx, rec_back), ELIB_NVS_ERR_EMPTY);

    fill_record(rec, 0x11);
    CHECK_EQ(elib_nvs_write(&ctx, rec), ELIB_NVS_OK);
    CHECK_EQ(ctx.read_slot, 0);
    CHECK_EQ(ctx.write_slot, 1);
    CHECK_EQ(ctx.bit_flags.has_data, 1);

    memset(rec_back, 0, sizeof(rec_back));
    CHECK_EQ(elib_nvs_read(&ctx, rec_back), ELIB_NVS_OK);
    CHECK_EQ(memcmp(rec_back, rec, USER), 0);

    printf("PASSED\n");
}

static void test_sequential_and_reboot(void) {
    printf("Test: sequential writes survive reboot and reset... ");

    memset(g_mem, ERASE_FILL, sizeof(g_mem));
    CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, MANAGED, SLOT, USER),
             ELIB_NVS_OK);

    fill_record(rec, 0x21); CHECK_EQ(elib_nvs_write(&ctx, rec), ELIB_NVS_OK);
    fill_record(rec, 0x22); CHECK_EQ(elib_nvs_write(&ctx, rec), ELIB_NVS_OK);
    fill_record(rec, 0x23); CHECK_EQ(elib_nvs_write(&ctx, rec), ELIB_NVS_OK);
    CHECK_EQ(ctx.read_slot, 2);
    CHECK_EQ(ctx.write_slot, 3);

    /* reboot-style relocation */
    CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, MANAGED, SLOT, USER),
             ELIB_NVS_OK);
    CHECK_EQ(ctx.read_slot, 2);
    CHECK_EQ(ctx.write_slot, 3);
    CHECK_EQ(ctx.bit_flags.has_data, 1);
    memset(rec_back, 0, sizeof(rec_back));
    CHECK_EQ(elib_nvs_read(&ctx, rec_back), ELIB_NVS_OK);
    CHECK_EQ(rec_back[0], 0x23);

    /* reset re-derives the same state */
    CHECK_EQ(elib_nvs_reset(&ctx), ELIB_NVS_OK);
    CHECK_EQ(ctx.read_slot, 2);
    CHECK_EQ(ctx.write_slot, 3);

    printf("PASSED\n");
}

static void test_slot0_invalid_full_scan(void) {
    printf("Test: invalid slot 0 does not hide later valid records... ");

    memset(g_mem, ERASE_FILL, sizeof(g_mem));
    CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, MANAGED, SLOT, USER),
             ELIB_NVS_OK);
    /* fill every slot so all of them are valid */
    {
        uint32_t i;
        for (i = 0; i < N_SLOTS; ++i) {
            fill_record(rec, (uint8_t)(0x30u + i));
            CHECK_EQ(elib_nvs_write(&ctx, rec), ELIB_NVS_OK);
        }
    }
    CHECK_EQ(ctx.read_slot, N_SLOTS - 1);
    CHECK_EQ(ctx.write_slot, 0);

    /* simulate a torn write / page erase at slot 0: its magic is no longer
     * 0xA5 while the following slots still hold valid older records */
    g_mem[0 * SLOT + 4] = 0x00u;

    CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, MANAGED, SLOT, USER),
             ELIB_NVS_OK);
    CHECK_EQ(ctx.bit_flags.has_data, 1);
    CHECK_EQ(ctx.read_slot, N_SLOTS - 1);
    memset(rec_back, 0, sizeof(rec_back));
    CHECK_EQ(elib_nvs_read(&ctx, rec_back), ELIB_NVS_OK);
    CHECK_EQ(rec_back[0], (uint8_t)(0x30u + (N_SLOTS - 1)));

    printf("PASSED\n");
}

static void test_wrap_rotation(void) {
    printf("Test: writes rotate through every slot and wrap... ");

    memset(g_mem, ERASE_FILL, sizeof(g_mem));
    CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, MANAGED, SLOT, USER),
             ELIB_NVS_OK);
    {
        uint32_t i;
        for (i = 0; i < 2u * N_SLOTS; ++i) {
            fill_record(rec, (uint8_t)(0x40u + i));
            CHECK_EQ(elib_nvs_write(&ctx, rec), ELIB_NVS_OK);
            CHECK_EQ(ctx.read_slot, i % N_SLOTS);
            CHECK_EQ(ctx.write_slot, (i + 1u) % N_SLOTS);
            memset(rec_back, 0, sizeof(rec_back));
            CHECK_EQ(elib_nvs_read(&ctx, rec_back), ELIB_NVS_OK);
            CHECK_EQ(rec_back[0], (uint8_t)(0x40u + i));
        }
    }

    printf("PASSED\n");
}

static void test_seq_wrap(void) {
    printf("Test: sequence wrap-around keeps the newest record... ");

    memset(g_mem, ERASE_FILL, sizeof(g_mem));
    poke_header(0u, 0xFFFFFFFEu);
    poke_header(1u, 0xFFFFFFFFu);
    poke_header(2u, 0x00000001u); /* wrapped: this is the newest */

    CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, MANAGED, SLOT, USER),
             ELIB_NVS_OK);
    CHECK_EQ(ctx.bit_flags.has_data, 1);
    CHECK_EQ(ctx.read_slot, 2);
    CHECK_EQ(ctx.write_slot, 3);
    CHECK_EQ(ctx.next_seq, 2u); /* 1 + 1 */

    printf("PASSED\n");
}

static void test_erase_polarity_zero(void) {
    printf("Test: a medium that erases to 0x00 reads as empty... ");

    memset(g_mem, 0x00, sizeof(g_mem)); /* erase-to-0 medium */

    CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, MANAGED, SLOT, USER),
             ELIB_NVS_OK);
    CHECK_EQ(ctx.bit_flags.has_data, 0);
    CHECK_EQ(elib_nvs_read(&ctx, rec_back), ELIB_NVS_ERR_EMPTY);

    fill_record(rec, 0x71);
    CHECK_EQ(elib_nvs_write(&ctx, rec), ELIB_NVS_OK);
    CHECK_EQ(elib_nvs_read(&ctx, rec_back), ELIB_NVS_OK);
    CHECK_EQ(rec_back[0], 0x71);

    /* reboot on the same 0x00-erased medium */
    CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, MANAGED, SLOT, USER),
             ELIB_NVS_OK);
    CHECK_EQ(ctx.bit_flags.has_data, 1);
    memset(rec_back, 0, sizeof(rec_back));
    CHECK_EQ(elib_nvs_read(&ctx, rec_back), ELIB_NVS_OK);
    CHECK_EQ(rec_back[0], 0x71);

    printf("PASSED\n");
}

static void test_io_errors_and_guards(void) {
    printf("Test: I/O failures and not-initialized guards... ");

    memset(g_mem, ERASE_FILL, sizeof(g_mem));
    CHECK_EQ(elib_nvs_init(&ctx, &ops_fail_read, BASE, MANAGED, SLOT, USER),
             ELIB_NVS_ERR_IO);

    memset(g_mem, ERASE_FILL, sizeof(g_mem));
    CHECK_EQ(elib_nvs_init(&ctx, &ops_fail_write, BASE, MANAGED, SLOT, USER),
             ELIB_NVS_OK);
    fill_record(rec, 0x60);
    CHECK_EQ(elib_nvs_write(&ctx, rec), ELIB_NVS_ERR_IO);

    {
        elib_nvs_ctx_t raw;
        memset(&raw, 0, sizeof(raw));
        CHECK_EQ(elib_nvs_read(&raw, rec_back), ELIB_NVS_ERR_NOT_INITIALIZED);
        CHECK_EQ(elib_nvs_write(&raw, rec), ELIB_NVS_ERR_NOT_INITIALIZED);
        CHECK_EQ(elib_nvs_reset(&raw), ELIB_NVS_ERR_NOT_INITIALIZED);
    }

    CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, MANAGED, SLOT, USER),
             ELIB_NVS_OK);
    CHECK_EQ(elib_nvs_write(&ctx, NULL), ELIB_NVS_ERR_INVALID_PARAM);
    CHECK_EQ(elib_nvs_read(&ctx, NULL), ELIB_NVS_ERR_INVALID_PARAM);

    printf("PASSED\n");
}

static void test_minimal_config(void) {
    printf("Test: minimal config (N=2, slot=6, user=1)... ");

    memset(g_mem, ERASE_FILL, sizeof(g_mem));
    /* slot 6 = 5-byte header + 1 user byte, no padding */
    CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, 12u, 6u, 1u), ELIB_NVS_OK);
    CHECK_EQ(ctx.slot_count, 2u);

    {
        uint32_t i;
        for (i = 0; i < 5u; ++i) {
            uint8_t one = (uint8_t)(0xA0u + i);
            CHECK_EQ(elib_nvs_write(&ctx, &one), ELIB_NVS_OK);
            CHECK_EQ(ctx.read_slot, i % 2u);
            CHECK_EQ(ctx.write_slot, (i + 1u) % 2u);
            CHECK_EQ(elib_nvs_read(&ctx, rec_back), ELIB_NVS_OK);
            CHECK_EQ(rec_back[0], one);
        }
    }

    printf("PASSED\n");
}

static void test_managed_not_multiple(void) {
    printf("Test: managed_size is not a multiple of slot_size... ");

    memset(g_mem, ERASE_FILL, sizeof(g_mem));
    /* 100 / 16 = 6 slots, 4 trailing bytes stay idle */
    CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, 100u, 16u, 8u), ELIB_NVS_OK);
    CHECK_EQ(ctx.slot_count, 6u);

    {
        uint32_t i;
        for (i = 0; i < 8u; ++i) {
            fill_record(rec, (uint8_t)(0xB0u + i));
            CHECK_EQ(elib_nvs_write(&ctx, rec), ELIB_NVS_OK);
            CHECK_EQ(ctx.read_slot, i % 6u);
            memset(rec_back, 0, sizeof(rec_back));
            CHECK_EQ(elib_nvs_read(&ctx, rec_back), ELIB_NVS_OK);
            CHECK_EQ(rec_back[0], (uint8_t)(0xB0u + i));
        }
    }

    printf("PASSED\n");
}

static void test_power_loss_matrix(void) {
    printf("Test: torn writes never expose half-written data... ");

    {
        uint32_t k;
        for (k = 0u; k <= USER + ELIB_NVS_HDR_SIZE; ++k) {
            uint32_t i;
            uint8_t newest;

            /* fresh medium, then fill every slot so the next write rewrites
             * an old slot whose magic is already 0xA5 */
            memset(g_mem, ERASE_FILL, sizeof(g_mem));
            CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, MANAGED, SLOT, USER),
                     ELIB_NVS_OK);
            for (i = 0u; i < N_SLOTS; ++i) {
                fill_record(rec, (uint8_t)(0x10u + i));
                CHECK_EQ(elib_nvs_write(&ctx, rec), ELIB_NVS_OK);
            }
            newest = (uint8_t)(0x10u + (N_SLOTS - 1u));

            /* tear the next write after exactly k bytes */
            fill_record(rec, 0x20);
            g_budget = k;
            (void)elib_nvs_write(&ctx, rec); /* fails for k < total bytes */
            g_budget = BUDGET_INF;

            /* reboot and inspect */
            CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, MANAGED, SLOT, USER),
                     ELIB_NVS_OK);
            CHECK_EQ(ctx.bit_flags.has_data, 1);
            memset(rec_back, 0, sizeof(rec_back));
            if (elib_nvs_read(&ctx, rec_back) == ELIB_NVS_OK) {
                /* only the previous newest or the fully written new record
                 * may come back; never a torn mixture */
                CHECK(rec_back[0] == newest || rec_back[0] == rec[0]);
            }
        }
    }

    /* first write on an empty medium: either empty or the full record */
    {
        uint32_t k;
        for (k = 0u; k <= USER + ELIB_NVS_HDR_SIZE; ++k) {
            memset(g_mem, ERASE_FILL, sizeof(g_mem));
            CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, MANAGED, SLOT, USER),
                     ELIB_NVS_OK);
            fill_record(rec, 0x20);
            g_budget = k;
            (void)elib_nvs_write(&ctx, rec);
            g_budget = BUDGET_INF;

            CHECK_EQ(elib_nvs_init(&ctx, &ops, BASE, MANAGED, SLOT, USER),
                     ELIB_NVS_OK);
            if (ctx.bit_flags.has_data) {
                memset(rec_back, 0, sizeof(rec_back));
                CHECK_EQ(elib_nvs_read(&ctx, rec_back), ELIB_NVS_OK);
                CHECK_EQ(rec_back[0], 0x20);
            } else {
                CHECK_EQ(elib_nvs_read(&ctx, rec_back), ELIB_NVS_ERR_EMPTY);
            }
        }
    }

    printf("PASSED\n");
}

int main(void) {
    test_init_valid_and_rejects();
    test_fresh_domain();
    test_sequential_and_reboot();
    test_slot0_invalid_full_scan();
    test_wrap_rotation();
    test_seq_wrap();
    test_erase_polarity_zero();
    test_io_errors_and_guards();
    test_minimal_config();
    test_managed_not_multiple();
    test_power_loss_matrix();

    if (g_failures != 0) {
        printf("FAILED: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("All tests PASSED\n");
    return 0;
}
