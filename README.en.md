[简体中文](README.md) | **English**

# elib-nvs-wear-leveling

A **lightweight wear-leveling library** for flash / EEPROM-like media: it does exactly
one thing—**append "the latest record" to the next slot of the managed area, and wrap
around to overwrite the oldest slot when the end is reached**—so write wear is naturally
spread across every slot of the managed area.

The library treats **every medium as a byte-writable EEPROM**: only the `read` / `write`
medium ops exist, and it **never erases, never checks emptiness and knows nothing about
pages**. A medium that must erase before programming (NOR flash) hides that inside the
user's `write` as a read-modify-write, presenting an EEPROM-style write to the library.

## Features

- **Zero dynamic memory**: the context is allocated by the user; the library has no static / heap buffer at all
- **C99**, single header + single implementation file, drop it into the project and use it
- **Implicit wear leveling**: rotation + cyclic overwrite; no wear table, no bad-block management
- **Minimal medium surface**: only `read` / `write`; erase and emptiness checks live entirely inside the user's `write`
- **Sequence-number location**: every record carries a 32-bit seq; a full scan at init picks the greatest one
- **Erase-polarity independent**: `magic = 0xA5` marks a written slot, so media that erase to 0x00 or 0xFF both read as "not written"
- **Single-record model**: only "the latest record" is stored; history is reclaimed by rotation

## Risk Disclosure and Recommended Pairing

This library does wear leveling only and **does not guarantee data integrity**. Know these
risks before use:

1. **No CRC / torn-write recovery / bit-flip protection**: whatever is written to the medium
   is read back as-is; a record that is torn by a power loss may be read back as if valid
   after a restart.
2. **Power-loss risk depends on the medium**:
   - **Byte-writable (EEPROM / FeRAM / MRAM)**: overwrite affects only "the cell being
     written", so **the history (the previous record and anything older) is intact**; low risk;
   - **flash (block erase)**: an erase / write-back may **lose a whole page**, taking the
     history in that page with it; high risk → **must be paired with elib-safe-storage's
     dual-region backup** as a backstop.
3. **The managed area may only be accessed by this library**: do not read or write it from
   outside the library (external tampering = undefined behavior).
4. **Concurrency safety of the medium ops (including ISRs) is the user's responsibility**:
   the library does no locking.
5. The configuration must pass `elib_nvs_init` validation; see §Validation Checklist.

**Recommendation**: flash media **must** be paired with
[elib-safe-storage](https://gitee.com/chenyananee/elib-safe-storage) (dual region + CRC);
byte-writable media **should** be paired as well (at least add CRC / torn-write checking so
a torn record is never mistaken for a valid one).

## Directory Layout

```
include/elib_nvs.h      the only header (error codes + ops + ctx + 4 APIs)
src/elib_nvs_core.c     the only implementation file
test/test_elib_nvs.c    RAM-simulated medium cases (compiled with the library as a check)
docs/superpowers/specs/ the v3 design document
```

There is no driver template file: your medium driver is the 2 functions under §Quick Start
below; write them accordingly.

## Quick Start

### Step 1: Implement Your 2 Medium Operations

```c
#include "elib_nvs.h"

/* Library convention: return 0 = success; non-0 = your own error code (mapped to ELIB_NVS_ERR_IO) */

static int my_read(void *ud, uint32_t addr, uint8_t *buf, uint32_t len) {
    /* Read len bytes from addr into buf; memcpy / DMA / driver call are all fine */
    return 0;                                   /* 0 = success */
}

static int my_write(void *ud, uint32_t addr, const uint8_t *buf, uint32_t len) {
    /* Write len bytes to addr.
     * EEPROM: write directly.
     * NOR flash: do the read-modify-write here—read the containing page, merge the
     *   new bytes, erase that page, write the whole page back. The library never erases. */
    return 0;
}

const elib_nvs_ops_t my_ops = {
    .user_data = &my_hw_ctx,   /* forwarded as-is to each of your ops */
    .read      = my_read,
    .write     = my_write,
};
```

### Step 2: Allocate the Context and Initialize

```c
static elib_nvs_ctx_t nvs_ctx;   /* statically allocated by the user; the library never allocates */

#define USER_SIZE    64u     /* user payload bytes */
#define SLOT_SIZE    80u     /* slot size: 5-byte header + 64 user + 11 padding */
#define SLOT_COUNT   8u
#define MANAGED_SIZE (SLOT_SIZE * SLOT_COUNT)

void app_setup(void) {
    elib_nvs_err_t err = elib_nvs_init(&nvs_ctx, &my_ops,
                                       0x00100000,    /* start_addr, no page alignment needed */
                                       MANAGED_SIZE,
                                       SLOT_SIZE,
                                       USER_SIZE);
    /* if err is not ELIB_NVS_OK, troubleshoot with §Validation Checklist */
}
```

### Step 3: Read and Write

```c
static uint8_t latest[USER_SIZE];

/* read the latest record (ELIB_NVS_ERR_EMPTY when nothing has been written yet) */
err = elib_nvs_read(&nvs_ctx, latest);

/* append a new record: lands on the next slot, wraps to overwrite the oldest at the end */
err = elib_nvs_write(&nvs_ctx, new_record);

/* re-scan and relocate (no medium write; rarely needed) */
err = elib_nvs_reset(&nvs_ctx);
```

## How It Works

### Slot Layout and Header

The managed area is divided into `N = managed_size / slot_size` **slots**, each
`slot_size` bytes:

```
┌──────────────┬────────────┬──────────────────────┬──────────┐
│ seq (4B)     │ magic (1B) │ user data user_size  │ padding  │
└──────────────┴────────────┴──────────────────────┴──────────┘
  └── library header (5 bytes, at the slot head) ──┘
  constraint: 5 + user_size <= slot_size
```

- `seq`: a 32-bit monotonic counter used to decide "who is newer";
- `magic`: `0xA5` means the slot was written; media that erase to all-0x00 or all-0xFF
  never read back `0xA5`, so validity is **independent of erase polarity**;
- padding is never written or read and keeps its erased value.

### Write Flow

```
target slot = (latest slot + 1) mod N
1) write user data (slot offset 5..)
2) write header array [seq][magic] (slot offset 0..4), one write, magic last
```

`magic` is the **commit point**: if power fails halfway through the header, `magic` has not
landed yet, so the slot reads as "not written" and the scan skips it.

### Locate (init / reset, full scan)

Scan every slot: only slots with `magic == 0xA5` are candidates; pick the candidate with
the greatest `seq` (wrap-safe modular compare); no candidate at all → empty area.

**The scan must cover all slots and must not stop at the first invalid header**: after a
wrap, a torn write can leave slot 0 invalid while the slots behind it still hold valid
records—stopping early would wrongly report an empty area.

### Rotation and Wear

The write pointer advances monotonically and wraps at the end; each write overwrites the
oldest slot, so write wear is spread across all N slots with no wear table.

### Power-Loss Safety (by medium)

- **Byte-writable (EEPROM / FeRAM / MRAM)**: the write is a byte-level overwrite, so a
  power loss only damages "the slot being written"; the other slots (including the previous
  record) are intact. The library's "data first, header last" protocol itself guarantees
  that after a power loss you read either the fully written new record or the fully
  written previous one.
- **flash (block erase)**: the read-modify-write inside the user's `write` may lose a whole
  page, taking the history in that page with it; that must be backstopped by safe-storage's
  dual region. The library only guarantees that it can still locate a record.

### Sequence Wrap-Around

The 32-bit seq uses the modular compare `(int32_t)(a - b) > 0` for "newer"; the live slot
count is far below 2^31, so wrap-around is naturally correct.

## Validation Checklist (any init rejection is `ELIB_NVS_ERR_INVALID_PARAM`)

- `ctx`, `ops` non-NULL, and both `read` / `write` function pointers inside `ops` non-NULL
- `user_size != 0`
- `5 + user_size <= slot_size` (header + user data fit in one slot)
- `N = managed_size / slot_size >= 2`
- `start_addr + managed_size` must not cross the 32-bit address space (wrap-around is rejected)

## API Overview

| Function | Behavior |
|---|---|
| `elib_nvs_init` | Validate the configuration and locate the latest record with a full scan |
| `elib_nvs_read` | Read the latest record (always user_size bytes) |
| `elib_nvs_write` | Append one record; lands on the next slot, wraps to overwrite the oldest |
| `elib_nvs_reset` | Re-scan and relocate, no medium write |

Return values: `ELIB_NVS_OK` / `ELIB_NVS_ERR_INVALID_PARAM` / `ELIB_NVS_ERR_NOT_INITIALIZED`
/ `ELIB_NVS_ERR_IO` (your op returned a non-0 value) / `ELIB_NVS_ERR_EMPTY` (no record in
the whole area yet).

## Memory Usage

- **flash (code)**: the implementation bodies of the two files, extremely small
- **RAM**: only `elib_nvs_ctx_t` (about 40 bytes on a 32-bit platform: 2 ops pointers +
  user_data + 4 config fields + runtime state)—allocated by **you**; the library uses zero memory
- **Stack**: no deep recursion, no large buffers, single-function stack depth

## Layering with elib-safe-storage

This library provides no integrity: whatever content is written to the medium is what is
read back—**torn writes, bit flips, and CRC checking all live outside this layer**. Wrap any
"record" in a safe-storage envelope and persist it with this library, and you get a storage
stack that has both integrity and persistence:

```
Application layer
  | write record
  v
elib-safe-storage  <- torn write / CRC / recovery
  | safe payload
  v
elib-nvs-wear-leveling  <- sequential write / wear leveling / cyclic overwrite
  |
  v
your medium driver (the two ops read / write)
```

## Integration Notes

1. Copy `include/` and `src/` into the project (or reference them as a submodule).
2. Implement the 2 ops as described above (this library provides no driver template; the method is fully described in this document).
3. Example compiler options: `-std=c99 -Wall` (compiles cleanly under `-Werror -Wextra`);
   MSVC needs `/utf-8` when the sources contain non-ASCII characters.
4. `test/test_elib_nvs.c` simulates the medium with a RAM array—compile it together with the library using whatever host/target toolchain is convenient to verify correctness in a real runtime.
