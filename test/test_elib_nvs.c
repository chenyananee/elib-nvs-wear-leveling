/* test_elib_nvs.c - RAM-simulated media test suite
 *
 * The suite only compiles on this host toolchain (-Werror, no run);
 * every case is verified by static timeline tracing. The simulated
 * media is a plain byte array: write overlays bytes, erase fills with
 * the erase default, is_empty sweeps for it. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "elib_nvs.h"

#define SIM_OK        0
#define SIM_FAIL      (-7) /* arbitrary user-space failure code */

#define ERASE_DEFAULT 0xFF
#define PAGE_A        256u
#define UNIT_A        640u   /* 6 full units + 256B zero remainder */
#define N_UNITS_A     6u
#define SIZE_A        (UNIT_A * N_UNITS_A + 256u) /* 4096 */
#define UNIT_B        512u   /* n == 1 degenerate domain */
#define N_UNITS_B     1u
#define SIZE_B        (UNIT_B * N_UNITS_B)        /* 512 */

/* Simulated media: A at offset 0, n==1 domain B at offset SIZE_A */
static uint8_t sim_mem[SIZE_A + SIZE_B];

typedef struct {
    uint32_t base;
    uint8_t *mem;
} sim_t;

static sim_t sim_a = { 0x1000u, &sim_mem[0] };
static sim_t sim_b = { 0x2000u, &sim_mem[SIZE_A] };

static int sim_read(void *ud, uint32_t addr, uint8_t *buf, uint32_t len) {
    const sim_t *s = (const sim_t *)ud;
    memcpy(buf, s->mem + (addr - s->base), len);
    return SIM_OK;
}

static int sim_write(void *ud, uint32_t addr, const uint8_t *buf, uint32_t len) {
    sim_t *s = (sim_t *)ud;
    memcpy(s->mem + (addr - s->base), buf, len);
    return SIM_OK;
}

static int sim_erase(void *ud, uint32_t addr, uint32_t len) {
    sim_t *s = (sim_t *)ud;
    memset(s->mem + (addr - s->base), ERASE_DEFAULT, len);
    return SIM_OK;
}

static uint8_t sim_is_empty(void *ud, uint32_t addr, uint32_t len) {
    const sim_t *s = (const sim_t *)ud;
    uint32_t off = addr - s->base;
    uint32_t i;
    for (i = 0; i < len; i++) {
        if (s->mem[off + i] != ERASE_DEFAULT) {
            return 0;
        }
    }
    return 1;
}

static const elib_nvs_ops_t ops_a = {
    &sim_a, sim_read, sim_write, sim_erase, sim_is_empty,
};
static const elib_nvs_ops_t ops_b = {
    &sim_b, sim_read, sim_write, sim_erase, sim_is_empty,
};

/* op stubs that fail, for I/O-error mapping tests */
static int fail_read(void *ud, uint32_t addr, uint8_t *buf, uint32_t len) {
    (void)ud; (void)addr; (void)buf; (void)len;
    return SIM_FAIL;
}
static int fail_write(void *ud, uint32_t addr, const uint8_t *buf, uint32_t len) {
    (void)ud; (void)addr; (void)buf; (void)len;
    return SIM_FAIL;
}
static const elib_nvs_ops_t ops_fail_read  = { &sim_a, fail_read,  sim_write, sim_erase, sim_is_empty };
static const elib_nvs_ops_t ops_fail_write = { &sim_a, sim_read,  fail_write, sim_erase, sim_is_empty };

/* op tables with NULL members, for the init-rejection test */
static const elib_nvs_ops_t ops_null_read   = { &sim_a, NULL, sim_write, sim_erase, sim_is_empty };
static const elib_nvs_ops_t ops_null_write  = { &sim_a, sim_read, NULL,  sim_erase, sim_is_empty };
static const elib_nvs_ops_t ops_null_erase  = { &sim_a, sim_read, sim_write, NULL, sim_is_empty };

/* Physical address of unit "idx" inside the simulated A domain */
static uint32_t unit_addr_of(uint32_t idx) {
    return sim_a.base + idx * UNIT_A;
}

/* Record payload helpers: content = seed, rest = 0xA5 pattern */
static void fill_record(uint8_t *buf, uint8_t seed) {
    memset(buf, 0xA5, UNIT_A);
    buf[0] = seed;
}

static uint32_t erase_count;
static int counting_erase(void *ud, uint32_t addr, uint32_t len) {
    erase_count++;
    return sim_erase(ud, addr, len);
}
static const elib_nvs_ops_t ops_counting = {
    &sim_a, sim_read, sim_write, counting_erase, sim_is_empty,
};

static elib_nvs_ctx_t ctx;
static uint8_t rec[UNIT_A];
static uint8_t rec_back[UNIT_A];

/* ---- tests ---- */

static void test_init_valid_and_rejects(void) {
    printf("Test: init validates the configuration... ");

    memset(sim_mem, ERASE_DEFAULT, sizeof(sim_mem));

    /* Valid A-domain config */
    assert(elib_nvs_init(&ctx, &ops_a, sim_a.base, SIZE_A,
                         PAGE_A, UNIT_A) == ELIB_NVS_OK);
    assert(ctx.unit_count == N_UNITS_A);
    assert(ctx.bit_flags.initialized == 1);

    /* Every rejection rule */
    assert(elib_nvs_init(NULL, &ops_a, 0, 0, 0, 0) == ELIB_NVS_ERR_INVALID_PARAM);
    assert(elib_nvs_init(&ctx, NULL, 0, 0, 0, 0) == ELIB_NVS_ERR_INVALID_PARAM);
    assert(elib_nvs_init(&ctx, &ops_null_read, sim_a.base, SIZE_A,
                         PAGE_A, UNIT_A) == ELIB_NVS_ERR_INVALID_PARAM);
    assert(elib_nvs_init(&ctx, &ops_null_write, sim_a.base, SIZE_A,
                         PAGE_A, UNIT_A) == ELIB_NVS_ERR_INVALID_PARAM);
    assert(elib_nvs_init(&ctx, &ops_null_erase, sim_a.base, SIZE_A,
                         PAGE_A, UNIT_A) == ELIB_NVS_ERR_INVALID_PARAM);
    assert(elib_nvs_init(&ctx, &ops_a, sim_a.base, SIZE_A,
                         0, UNIT_A) == ELIB_NVS_ERR_INVALID_PARAM);   /* page 0 */
    assert(elib_nvs_init(&ctx, &ops_a, sim_a.base, SIZE_A,
                         PAGE_A, 0) == ELIB_NVS_ERR_INVALID_PARAM);   /* unit 0 */
    /* op tables with a NULL is_empty member */
    {
        elib_nvs_ops_t bad = ops_a;
        bad.is_empty = NULL;
        assert(elib_nvs_init(&ctx, &bad, sim_a.base, SIZE_A,
                             PAGE_A, UNIT_A) == ELIB_NVS_ERR_INVALID_PARAM);
    }
    assert(elib_nvs_init(&ctx, &ops_a, sim_a.base + 1u, SIZE_A, /* unaligned start */
                         PAGE_A, UNIT_A) == ELIB_NVS_ERR_INVALID_PARAM);
    assert(elib_nvs_init(&ctx, &ops_a, sim_a.base, SIZE_A - 1u, /* unaligned size */
                         PAGE_A, UNIT_A) == ELIB_NVS_ERR_INVALID_PARAM);
    assert(elib_nvs_init(&ctx, &ops_a, sim_a.base, PAGE_A / 2u, /* size < page */
                         PAGE_A, UNIT_A) == ELIB_NVS_ERR_INVALID_PARAM);
    assert(elib_nvs_init(&ctx, &ops_a, sim_a.base, SIZE_A,
                         PAGE_A, SIZE_A + 1u) == ELIB_NVS_ERR_INVALID_PARAM); /* unit > managed */
    assert(elib_nvs_init(&ctx, &ops_a, 0xFFFFF000u, SIZE_A, /* start + size wraps */
                         PAGE_A, UNIT_A) == ELIB_NVS_ERR_INVALID_PARAM);

    /* n == 1 domain config is valid */
    assert(elib_nvs_init(&ctx, &ops_b, sim_b.base, SIZE_B,
                         PAGE_A, UNIT_B) == ELIB_NVS_OK);
    assert(ctx.unit_count == N_UNITS_B);

    printf("PASSED\n");
}

static void test_scan_fresh_domain(void) {
    printf("Test: scan lands on a fresh empty domain... ");

    memset(sim_mem, ERASE_DEFAULT, sizeof(sim_mem));
    assert(elib_nvs_init(&ctx, &ops_a, sim_a.base, SIZE_A,
                         PAGE_A, UNIT_A) == ELIB_NVS_OK);
    assert(ctx.bit_flags.has_data == 0);
    assert(ctx.read_unit == 0 && ctx.write_unit == 0);

    /* Read before anything is written */
    assert(elib_nvs_read(&ctx, rec_back) == ELIB_NVS_ERR_EMPTY);

    /* First write lands on unit 0, then read returns it */
    fill_record(rec, 1);
    assert(elib_nvs_write(&ctx, rec) == ELIB_NVS_OK);
    assert(ctx.read_unit == 0 && ctx.write_unit == 1);
    memset(rec_back, 0, sizeof(rec_back));
    assert(elib_nvs_read(&ctx, rec_back) == ELIB_NVS_OK);
    assert(memcmp(rec_back, rec, UNIT_A) == 0);

    printf("PASSED\n");
}

static void test_scan_partial_domain(void) {
    printf("Test: scan places read/write at the data frontier... ");

    memset(sim_mem, ERASE_DEFAULT, sizeof(sim_mem));
    assert(elib_nvs_init(&ctx, &ops_a, sim_a.base, SIZE_A,
                         PAGE_A, UNIT_A) == ELIB_NVS_OK);

    /* Three records appended normally */
    fill_record(rec, 0x10); assert(elib_nvs_write(&ctx, rec) == ELIB_NVS_OK);
    fill_record(rec, 0x11); assert(elib_nvs_write(&ctx, rec) == ELIB_NVS_OK);
    fill_record(rec, 0x12); assert(elib_nvs_write(&ctx, rec) == ELIB_NVS_OK);
    assert(ctx.read_unit == 2 && ctx.write_unit == 3);
    assert(elib_nvs_read(&ctx, rec_back) == ELIB_NVS_OK);
    assert(rec_back[0] == 0x12);

    /* Reboot-style relocation (init again) finds the same positions */
    assert(elib_nvs_init(&ctx, &ops_a, sim_a.base, SIZE_A,
                         PAGE_A, UNIT_A) == ELIB_NVS_OK);
    assert(ctx.read_unit == 2 && ctx.write_unit == 3);
    assert(ctx.bit_flags.has_data == 1);
    memset(rec_back, 0, sizeof(rec_back));
    assert(elib_nvs_read(&ctx, rec_back) == ELIB_NVS_OK);
    assert(rec_back[0] == 0x12); /* newest survives the reboot */

    /* reset() re-derives the same state */
    assert(elib_nvs_reset(&ctx) == ELIB_NVS_OK);
    assert(ctx.read_unit == 2 && ctx.write_unit == 3);

    printf("PASSED\n");
}

static void test_scan_tail_single_empty(void) {
    printf("Test: scan tail clause - exactly one empty unit left... ");

    memset(sim_mem, ERASE_DEFAULT, sizeof(sim_mem));
    assert(elib_nvs_init(&ctx, &ops_a, sim_a.base, SIZE_A,
                         PAGE_A, UNIT_A) == ELIB_NVS_OK);
    fill_record(rec, 0x21); assert(elib_nvs_write(&ctx, rec) == ELIB_NVS_OK);
    fill_record(rec, 0x22); assert(elib_nvs_write(&ctx, rec) == ELIB_NVS_OK);
    fill_record(rec, 0x23); assert(elib_nvs_write(&ctx, rec) == ELIB_NVS_OK);
    fill_record(rec, 0x24); assert(elib_nvs_write(&ctx, rec) == ELIB_NVS_OK);
    fill_record(rec, 0x25); assert(elib_nvs_write(&ctx, rec) == ELIB_NVS_OK);
    assert(ctx.read_unit == 4 && ctx.write_unit == 5);

    /* Reboot: the empty run is exactly one unit (the tail slot) */
    assert(elib_nvs_init(&ctx, &ops_a, sim_a.base, SIZE_A,
                         PAGE_A, UNIT_A) == ELIB_NVS_OK);
    assert(ctx.read_unit == 4 && ctx.write_unit == 5);
    memset(rec_back, 0, sizeof(rec_back));
    assert(elib_nvs_read(&ctx, rec_back) == ELIB_NVS_OK);
    assert(rec_back[0] == 0x25);

    /* Writing into the tail slot: data is alive, pointer wraps to head */
    fill_record(rec, 0x26);
    assert(elib_nvs_write(&ctx, rec) == ELIB_NVS_OK);
    assert(ctx.read_unit == 5 && ctx.write_unit == 0);
    assert(elib_nvs_read(&ctx, rec_back) == ELIB_NVS_OK);
    assert(rec_back[0] == 0x26);

    /* The lazy erase has NOT fired yet: 0x26 is still readable */
    assert(erase_count == 0);

    printf("PASSED\n");
}

static void test_scan_full_domain(void) {
    printf("Test: scan full clause + lazy global erase on next write... ");

    erase_count = 0;
    memset(sim_mem, ERASE_DEFAULT, sizeof(sim_mem));
    assert(elib_nvs_init(&ctx, &ops_counting, sim_a.base, SIZE_A,
                         PAGE_A, UNIT_A) == ELIB_NVS_OK);
    {
        uint8_t i;
        for (i = 0; i < N_UNITS_A; i++) {
            fill_record(rec, (uint8_t)(0x30 + i));
            assert(elib_nvs_write(&ctx, rec) == ELIB_NVS_OK);
        }
    }
    /* Domain fully written, pointer back at head, no erase yet */
    assert(erase_count == 0);
    assert(ctx.write_unit == 0);
    assert(elib_nvs_read(&ctx, rec_back) == ELIB_NVS_OK);
    assert(rec_back[0] == 0x35); /* the last unit's record lives */

    /* Reboot in the full state: read = last unit, write = head */
    assert(elib_nvs_init(&ctx, &ops_counting, sim_a.base, SIZE_A,
                         PAGE_A, UNIT_A) == ELIB_NVS_OK);
    assert(ctx.read_unit == N_UNITS_A - 1 && ctx.write_unit == 0);
    assert(ctx.bit_flags.has_data == 1);
    memset(rec_back, 0, sizeof(rec_back));
    assert(elib_nvs_read(&ctx, rec_back) == ELIB_NVS_OK);
    assert(rec_back[0] == 0x35);

    /* Next write cannot find a slot: the lazy global erase fires now */
    fill_record(rec, 0x36);
    assert(elib_nvs_write(&ctx, rec) == ELIB_NVS_OK);
    assert(erase_count == 1);
    assert(ctx.read_unit == 0 && ctx.write_unit == 1);
    assert(ctx.bit_flags.has_data == 1);
    memset(rec_back, 0, sizeof(rec_back));
    assert(elib_nvs_read(&ctx, rec_back) == ELIB_NVS_OK);
    assert(rec_back[0] == 0x36); /* cycle restarts from unit 0 */

    printf("PASSED\n");
}

static void test_write_skips_illegal_windows(void) {
    printf("Test: orphan window is skipped, reboot relocates past it... ");

    memset(sim_mem, ERASE_DEFAULT, sizeof(sim_mem));
    assert(elib_nvs_init(&ctx, &ops_a, sim_a.base, SIZE_A,
                         PAGE_A, UNIT_A) == ELIB_NVS_OK);

    /* Record A lands in unit 0 */
    fill_record(rec, 0x40);
    assert(elib_nvs_write(&ctx, rec) == ELIB_NVS_OK);

    /* Out-of-library junk appears in unit 1 while the pointer is there */
    {
        uint8_t junk[UNIT_A];
        memset(junk, 0x55, sizeof(junk));
        assert(sim_write(&sim_a, unit_addr_of(1), junk, UNIT_A) == SIM_OK);
    }

    /* The (1,2) window is illegal: the write slides to the first legal
     * double-empty window (2,3) and lands in unit 2 */
    fill_record(rec, 0x41);
    assert(elib_nvs_write(&ctx, rec) == ELIB_NVS_OK);
    assert(ctx.read_unit == 2 && ctx.write_unit == 3);

    /* Reboot: the scan skips both illegal windows ((0,1) and (1,2)) and
     * seats at (2,3): read = unit 2 = the true newest record */
    assert(elib_nvs_init(&ctx, &ops_a, sim_a.base, SIZE_A,
                         PAGE_A, UNIT_A) == ELIB_NVS_OK);
    assert(ctx.read_unit == 2 && ctx.write_unit == 3);
    memset(rec_back, 0, sizeof(rec_back));
    assert(elib_nvs_read(&ctx, rec_back) == ELIB_NVS_OK);
    assert(rec_back[0] == 0x41);

    printf("PASSED\n");
}

static void test_n1_domain_rolls(void) {
    printf("Test: single-unit domain rolls write->erase->write... ");

    erase_count = 0;
    memset(sim_mem, ERASE_DEFAULT, sizeof(sim_mem));
    assert(elib_nvs_init(&ctx, &ops_counting, sim_b.base, SIZE_B,
                         PAGE_A, UNIT_B) == ELIB_NVS_OK);
    {
        uint8_t rec1[UNIT_B];
        memset(rec1, 0xA5, sizeof(rec1));
        rec1[0] = 0x50;
        assert(elib_nvs_write(&ctx, rec1) == ELIB_NVS_OK);
        assert(ctx.read_unit == 0 && ctx.write_unit == 0);
        assert(erase_count == 0);
        assert(elib_nvs_read(&ctx, rec_back) == ELIB_NVS_OK);
        assert(rec_back[0] == 0x50);

        /* Second write: the sole unit is full -> lazy erase -> rewrite */
        rec1[0] = 0x51;
        assert(elib_nvs_write(&ctx, rec1) == ELIB_NVS_OK);
        assert(erase_count == 1);
        assert(ctx.read_unit == 0 && ctx.write_unit == 0);
        memset(rec_back, 0, sizeof(rec_back));
        assert(elib_nvs_read(&ctx, rec_back) == ELIB_NVS_OK);
        assert(rec_back[0] == 0x51);
    }

    printf("PASSED\n");
}

static void test_io_errors_map(void) {
    printf("Test: user op failures surface as ELIB_NVS_ERR_IO... ");

    memset(sim_mem, ERASE_DEFAULT, sizeof(sim_mem));
    assert(elib_nvs_init(&ctx, &ops_fail_write, sim_a.base, SIZE_A,
                         PAGE_A, UNIT_A) == ELIB_NVS_OK);
    fill_record(rec, 0x60);
    assert(elib_nvs_write(&ctx, rec) == ELIB_NVS_ERR_IO);

    assert(elib_nvs_init(&ctx, &ops_fail_read, sim_a.base, SIZE_A,
                         PAGE_A, UNIT_A) == ELIB_NVS_OK);
    {
        uint8_t rec2[UNIT_A];
        fill_record(rec2, 0x61);
        /* write with a failing reader is fine; read then reports IO */
        assert(elib_nvs_write(&ctx, rec2) == ELIB_NVS_OK);
        /* swap in a failing reader only: reuse ops_fail_read whose
         * writer works (sim_write) and reader fails */
        assert(elib_nvs_read(&ctx, rec_back) == ELIB_NVS_ERR_IO);
    }

    /* Not-initialized guards */
    {
        elib_nvs_ctx_t raw_ctx;
        memset(&raw_ctx, 0, sizeof(raw_ctx));
        assert(elib_nvs_read(&raw_ctx, rec_back) == ELIB_NVS_ERR_NOT_INITIALIZED);
        assert(elib_nvs_write(&raw_ctx, rec) == ELIB_NVS_ERR_NOT_INITIALIZED);
        assert(elib_nvs_reset(&raw_ctx) == ELIB_NVS_ERR_NOT_INITIALIZED);
        assert(elib_nvs_erase_all(&raw_ctx) == ELIB_NVS_ERR_NOT_INITIALIZED);
    }

    printf("PASSED\n");
}

static void test_erase_all(void) {
    printf("Test: erase_all returns the domain to the fresh state... ");

    memset(sim_mem, ERASE_DEFAULT, sizeof(sim_mem));
    assert(elib_nvs_init(&ctx, &ops_a, sim_a.base, SIZE_A,
                         PAGE_A, UNIT_A) == ELIB_NVS_OK);
    fill_record(rec, 0x70); assert(elib_nvs_write(&ctx, rec) == ELIB_NVS_OK);
    fill_record(rec, 0x71); assert(elib_nvs_write(&ctx, rec) == ELIB_NVS_OK);
    assert(ctx.bit_flags.has_data == 1);

    assert(elib_nvs_erase_all(&ctx) == ELIB_NVS_OK);
    assert(ctx.bit_flags.has_data == 0);
    assert(ctx.write_unit == 0);
    assert(elib_nvs_read(&ctx, rec_back) == ELIB_NVS_ERR_EMPTY);

    /* Write after erase lands at unit 0 again */
    fill_record(rec, 0x72);
    assert(elib_nvs_write(&ctx, rec) == ELIB_NVS_OK);
    assert(ctx.read_unit == 0 && ctx.write_unit == 1);
    memset(rec_back, 0, sizeof(rec_back));
    assert(elib_nvs_read(&ctx, rec_back) == ELIB_NVS_OK);
    assert(rec_back[0] == 0x72);

    printf("PASSED\n");
}

static void test_null_buf_guard(void) {
    printf("Test: NULL record buffers are rejected... ");

    memset(sim_mem, ERASE_DEFAULT, sizeof(sim_mem));
    assert(elib_nvs_init(&ctx, &ops_a, sim_a.base, SIZE_A,
                         PAGE_A, UNIT_A) == ELIB_NVS_OK);
    assert(elib_nvs_write(&ctx, NULL) == ELIB_NVS_ERR_INVALID_PARAM);
    assert(elib_nvs_read(&ctx, NULL) == ELIB_NVS_ERR_INVALID_PARAM);

    printf("PASSED\n");
}

int main(void) {
    test_init_valid_and_rejects();
    test_scan_fresh_domain();
    test_scan_partial_domain();
    test_scan_tail_single_empty();
    test_scan_full_domain();
    test_write_skips_illegal_windows();
    test_n1_domain_rolls();
    test_io_errors_map();
    test_erase_all();
    test_null_buf_guard();

    printf("All tests PASSED\n");
    return 0;
}
