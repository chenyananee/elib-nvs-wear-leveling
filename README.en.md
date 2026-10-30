[简体中文](README.md) | **English**

# elib-nvs-wear-leveling

A **lightweight wear-leveling library** for flash / EEPROM-like media: it does not rely on CRC, keeps no wear table, and builds no directory tree—it does exactly one thing: **append "the latest record" to the next cell of the managed area, and after the whole area is full, erase it and start over from the beginning**, so erase wear is naturally spread across every page of the managed area.

Designed to layer with [elib-safe-storage](https://gitee.com/chenyananee/elib-safe-storage) (if released): this library only guarantees "locate + sequential write + cyclic erase"; **data integrity (torn writes, CRC) is out of scope**—a torn write reads back whatever it is, and integrity is backstopped by the safe-storage layer above.

## Features

- **Zero dynamic memory**: the context is allocated by the user; the library has no static / heap buffer at all (not even an emptiness-check buffer)
- **C99**, single header + single implementation file, drop it into the project and use it
- **Implicit wear leveling**: append write + whole-area cyclic erase; no wear table, no bad-block management
- **Emptiness check externalized**: whether the medium is empty is decided by the user op (`is_empty`)—flash users can compare via MMIO, EEPROM users can choose polling or background scanning; zero RAM cost on the library side
- **Lazy erase**: whole-area erase happens only when "the next write cannot find a legal slot"; the data in the last cell stays readable until the next write replaces it, **zero data waste**
- **Single-record model**: only "the latest record" is stored; history is reclaimed naturally by erase

## Directory Layout

```
include/elib_nvs.h     the only header (error codes + ops + ctx + 5 APIs)
src/elib_nvs_core.c    the only implementation file
test/test_elib_nvs.c   RAM-simulated medium cases (compiled with the library as a check)
```

There is no driver template file: your medium driver is the 4 functions under §Integration below; write them accordingly.

## Quick Start

### Step 1: Implement Your 4 Medium Operations

```c
#include "elib_nvs.h"

/* Library convention for every op: return 0 = success; non-0 = your own error code (the library reports it as ELIB_NVS_ERR_IO) */

static int my_read(void *ud, uint32_t addr, uint8_t *buf, uint32_t len) {
    /* Read len bytes from addr into buf; memcpy / DMA / driver call are all fine */
    return 0;                                   /* 0 = success */
}

static int my_write(void *ud, uint32_t addr, const uint8_t *buf, uint32_t len) {
    /* Write len bytes to addr (the library only writes to cells "confirmed empty", so flash needs no prior erase) */
    return 0;
}

static int my_erase(void *ud, uint32_t addr, uint32_t len) {
    /* Erase [addr, addr+len): there are only two call sites—
     *   a) one whole-area erase (addr = start, len = managed_size)
     *   b) elib_nvs_erase_all is likewise always a whole-area erase
     * So the minimal implementation only needs to support "erase the whole area";
     * erase_page_size here is for init alignment validation and does not drive erase calls separately */
    return 0;
}

static uint8_t my_is_empty(void *ud, uint32_t addr, uint32_t len) {
    /* Key convention: decide whether [addr, addr+len) is empty by your own definition—
     * typical implementation: return 1 only if every byte equals the post-erase value (e.g. 0xFF), otherwise 0.
     * The library never reads record contents, so the definition of "empty" is entirely up to you;
     * but the baseline must match what your my_erase erases to—otherwise power-loss relocation will pin the wrong position */
    return 1;  /* implement it the way that is optimal for your medium: direct MMIO compare / word-wide scan / driver helper */
}

const elib_nvs_ops_t my_ops = {
    .user_data = &my_hw_ctx,   /* forwarded as-is to each of your ops */
    .read      = my_read,
    .write     = my_write,
    .erase     = my_erase,
    .is_empty  = my_is_empty,
};
```

### Step 2: Allocate the Context and Initialize

```c
static elib_nvs_ctx_t nvs_ctx;   /* statically allocated by the user; the library never allocates any more memory */

#define RECORD_SIZE       64u      /* fixed byte count per record (cell size) */
#define ERASE_PAGE_SIZE   4096u    /* minimum erase page of the medium */
#define MANAGED_SIZE      (ERASE_PAGE_SIZE * 16u)  /* managed area size */

void app_setup(void) {
    elib_nvs_err_t err = elib_nvs_init(&nvs_ctx, &my_ops,
                                       0x00100000,    /* start_addr, aligned to the erase page */
                                       MANAGED_SIZE,  /* managed area size, integer multiple of the page */
                                       ERASE_PAGE_SIZE,
                                       RECORD_SIZE);
    /* if err is not ELIB_NVS_OK, troubleshoot with §Validation Checklist */
}
```

### Step 3: Read and Write

```c
static uint8_t latest[RECORD_SIZE];

/* read the latest record (returns ELIB_NVS_ERR_EMPTY when nothing has been written in the whole area) */
err = elib_nvs_read(&nvs_ctx, latest);

/* append a new record (advances one cell automatically; when full, lazily erases the whole area first and writes from the beginning) */
err = elib_nvs_write(&nvs_ctx, new_record);

/* re-scan the read/write position (does no physical erase; rarely needed, mainly when you want the library to "re-read the situation" after the medium contents were touched by an external tool) */
err = elib_nvs_reset(&nvs_ctx);

/* actively erase the whole managed area physically, returning to the empty-medium state */
err = elib_nvs_erase_all(&nvs_ctx);
```

## How It Works

### Data Cells and Append Writes

The managed area is divided into `N = managed_size / unit_size` **data cells** (unit_size may span erase pages and need not divide evenly; the remainder of `managed_size / unit_size` is always idle). Each `elib_nvs_write` writes one fixed-size cell at the "write pointer", then:

- if the adjacent cell after it is confirmed all empty → the write pointer points to it;
- if only the remainder or a non-empty neighbor follows it → the write pointer wraps back to the **head (unit 0)**.

Thus one erase-write cycle fills `0, 1, 2, …, N-1` in order, using every page of the area each round, and **erase wear is naturally spread**, with no wear table needed.

### Three-Branch Seek (the same seek predicate shared by init / reset / write)

seek searches forward from the given cell for the first "legal slot":

1. **Normal state** `(k, k+1)` two consecutive all-empty cells (2×unit window) → slot = k; scan variant: the first such k > 0 → `read = k-1` (latest data), `write = k`
2. **Trailing single slot**: k is the last cell and is empty → slot = k ("one is legal when there is not room for two", so the last cell is not wasted)
3. **Full state**: still no slot at the end of the sweep → `read = N-1` (the last cell at the tail = latest data), `write = head`; the next write's prediction triggers the whole-area erase here

An all-empty new medium → no read position (`elib_nvs_read` returns `ELIB_NVS_ERR_EMPTY`), write pointer at the head.

### Why "Two Consecutive All-Empty Cells" Is the Lifeline

Requiring `(k, k+1)` both empty rather than looking at `k` alone is what makes **a power-loss restart at any moment pin the correct position**: every window crossed before the write pointer's landing spot either became the landing spot as a double-empty window or contains a non-empty cell (orphan data from external tampering counts too) and is "sealed off". The restart scan slides the same predicate, the window containing an orphan is necessarily judged illegal and bypassed—**the scan always pins just after the real latest data**, with no on-disk marker and no dirty data written back.

**Directly tampering with the managed area contents outside the library = undefined behavior**. The runtime state self-heals through the window seek (illegal windows are bypassed → the full state converges to "read the tail cell"), but it does not promise to recover "the correct record".

### Degenerate Form n == 1

The area holds only one cell: `lib_nvs_write` always goes "full → lazy whole-area erase → write from the head", equivalent to a rolling single slot.

Lifetime model: total write lifetime ≈ `N × single-page erase lifetime` (each whole-area erase wears every page once).

## Validation Checklist (any init rejection is `ELIB_NVS_ERR_INVALID_PARAM`)

- `ctx`, `ops` non-NULL, and all four function pointers `read/write/erase/is_empty` inside `ops` non-NULL
- `erase_page_size != 0`, `unit_size != 0`
- `start_addr % erase_page_size == 0` (start address aligned to the erase page)
- `managed_size % erase_page_size == 0`, and `managed_size >= erase_page_size`
- `1 <= unit_size <= managed_size`
- `start_addr + managed_size` must not cross the 32-bit address space (wrap-around is rejected)

## API Overview

| Function | Behavior |
|---|---|
| `elib_nvs_init` | Validate the configuration, scan and locate the read/write positions |
| `elib_nvs_read` | Read the latest record (always unit_size bytes) |
| `elib_nvs_write` | Append one record; when full, lazily erase the whole area first and write from the beginning |
| `elib_nvs_reset` | Re-scan and locate the read/write positions, no physical erase |
| `elib_nvs_erase_all` | Physically erase the entire managed area, returning to the empty-medium state |

Return values: `ELIB_NVS_OK` / `ELIB_NVS_ERR_INVALID_PARAM` / `ELIB_NVS_ERR_NOT_INITIALIZED` / `ELIB_NVS_ERR_IO` (your op returned a non-0 value) / `ELIB_NVS_ERR_EMPTY` (no record in the whole area yet).

## Memory Usage

- **flash (code)**: the implementation bodies of the two files, extremely small
- **RAM**: only `elib_nvs_ctx_t` (about 40 bytes on a 32-bit platform: 4 ops pointers + user_data + 5 config fields + runtime state)—allocated by **you**; the library uses zero memory
- **Stack**: no deep recursion, no large buffers, single-function stack depth

## Layering with elib-safe-storage

This library provides no integrity: whatever content is written to the medium is what is read back—**torn writes, bit flips, and CRC checking all live outside this layer**. Wrap any "record" in a safe-storage envelope and persist it with this library, and you get a storage stack that has both integrity and persistence:

```
Application layer
  │  write record
  ▼
elib-safe-storage  ← torn write / CRC / recovery
  │  safe payload
  ▼
elib-nvs-wear-leveling  ← append write / wear leveling / cyclic erase
  │
  ▼
your medium driver (the four ops read / write / erase / is_empty)
```

## Integration Notes

1. Copy `include/` and `src/` into the project (or reference them as a submodule).
2. Implement the 4 ops as described above (this library provides no driver template; the method is fully described in this document).
3. Example compiler options: `-std=c99 -Wall` (compiles cleanly under `-Werror -Wextra`).
4. `test/test_elib_nvs.c` simulates the medium with a RAM array—compile it together with the library using whatever host/target toolchain is convenient to verify correctness in a real runtime.
