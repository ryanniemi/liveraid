# AGENTS.md

This file provides guidance to AI coding agents working with this repository.
For user-facing documentation see [README.md](README.md); for implementation
details see [INTERNALS.md](INTERNALS.md).

## Project Overview

liveraid is a FUSE filesystem that merges multiple data drives into a single namespace with live erasure-coded parity. Files are stored whole on one drive (no striping). Parity is maintained via a write-back journal: writes go immediately to the data drive, and a background thread computes and writes parity asynchronously. Up to 6 parity levels using ISA-L Cauchy-matrix GF(2⁸) erasure coding with AVX2 acceleration.

## Build Commands

```bash
make          # Compile binary → ./liveraid
make test     # Build and run all unit tests (no extra dependencies)
make clean    # Remove objects, binary, and test binaries
```

Dependencies: `libfuse3-dev`, `libisal-dev`, `gcc`, `make`, `pkg-config`.

## Tests

Unit tests live in `tests/`. Each module has its own binary; all are built and
run by `make test`. Tests use a minimal header-only harness (`tests/test_harness.h`)
with no external test framework required.

| Binary | Source module(s) | What is tested |
|--------|-----------------|----------------|
| `tests/test_alloc` | `src/alloc.c` | `alloc_positions` / `free_positions`: sequential alloc, free with neighbor merging, extent reuse, first-fit skip, bump fallback |
| `tests/test_hash` | `src/lr_hash.c` | Insert, find, remove; bucket growth; same-bucket chain removal; FNV-1a stability |
| `tests/test_list` | `src/lr_list.c` | Insert-tail; remove from head, tail, middle, and sole element |
| `tests/test_state` | `src/state.c` + support | File and dir CRUD; `blocks_for_size`; position-index binary search; round-robin drive selection |
| `tests/test_metadata` | `src/metadata.c` + support | Save/load roundtrip (all 11 file fields, all dir fields); fresh-start with no content file; old 8-field format compatibility; allocator state persistence across save/load |
| `tests/test_config` | `src/config.c` | Valid configs; default values; all placement policies; parity levels and gap detection; error paths (no drives, no content, no mountpoint, bad blocksize, bad parity_threads); unknown directives (non-fatal); comments and blank lines |

### Unit test conventions

- Tests are self-contained: they use `/tmp` for any files they create and clean
  up after themselves. No real drives, parity files, or FUSE mounts are needed.
- `make_config` helpers in each test file use `LR_PLACE_ROUNDROBIN` so
  drive-selection tests never call `statvfs` on real paths.
- Error-path tests (config validation failures, etc.) produce expected messages
  on stderr — this is normal and not a test failure.
- When adding a new source module, add a corresponding `tests/test_<module>.c`
  and a rule in the `Makefile` test section following the existing pattern.

### Integration tests

`tests/integration.sh` mounts a real liveraid filesystem under `/tmp/lrt/` and
exercises end-to-end behaviour. Prerequisites: `fusermount3` (`apt install fuse3`),
~500 MiB free in `/tmp`, and the binary built with `make`.

```bash
bash tests/integration.sh   # run from repo root
```

The script creates 4 data drives + 2 parity levels, runs 13 test sections, and
prints a `PASS`/`FAIL` summary. Each section calls `wipe_data` to start clean.

| # | Section | What is exercised |
|---|---------|-------------------|
| 1 | parity_threads=4 drain + scrub | Parallel bitmap drain, SIGUSR2 repair, 0 mismatches |
| 2 | rmdir | Empty-dir removal; ENOTEMPTY on non-empty |
| 3 | Rename across directories | Cross-dir rename; persistence across remount |
| 4 | 2-drive failure recovery | Simultaneous loss of 2 drives; transparent read via 2-level parity |
| 5 | Offline rebuild | `liveraid rebuild` reconstructs 2 drives from parity (no mount) |
| 6 | Live rebuild + busy-file skip | Socket rebuild skips open files; rebuilds after close |
| 7 | Crash recovery (kill -9) | Content file survives SIGKILL; parity clean after remount |
| 8 | Multiple content paths | All paths written on save; secondary used when primary missing |
| 9 | Empty files | size=0 after create and after remount |
| 10 | Directory metadata | `chmod` + `utimens` on dirs persist across remount |
| 11 | Socket scrub/repair | `scrub` and `scrub repair` commands return 0 mismatches via ctrl socket |
| 12 | Position reuse | Parity position freed by `unlink` is reused by next allocation |
| 13 | Placement policy smoke | `mostfree`, `lfs`, `pfrd`: 8 files readable + parity clean each |

## Architecture

### Directory Layout

```
./
├── Makefile
├── liveraid.conf.example
├── tests/
│   ├── test_harness.h      # Minimal ASSERT / RUN / REPORT macros
│   ├── test_alloc.c
│   ├── test_hash.c
│   ├── test_list.c
│   ├── test_state.c
│   ├── test_metadata.c
│   └── test_config.c
└── src/
    ├── main.c          # Entry point: arg parse, rebuild dispatch,
    │                   # SIGUSR1/USR2 handlers, fuse_main
    ├── config.h/c      # INI-style config parser
    ├── state.h/c       # In-memory state, file table (lr_hash), file list (lr_list),
    │                   # dir table/list (lr_dir), drive selection, per-drive position index
    ├── lr_hash.h/c     # Intrusive separate-chaining hash map (FNV-1a)
    ├── lr_list.h/c     # Intrusive doubly-linked list
    ├── alloc.h/c       # Per-drive parity-position allocator + free list
    ├── metadata.h/c    # Content-file load/save (atomic write, CRC32)
    ├── fuse_ops.h/c    # FUSE3 high-level operation callbacks
    ├── parity.h/c      # Parity file I/O, ISA-L encode/recover/scrub/repair wrappers
    ├── journal.h/c     # Dirty-position bitmap + background worker thread
    │                   # (parallel drain, periodic save, crash journal, scrub/repair)
    ├── rebuild.h/c     # Drive rebuild from parity (live via socket; offline fallback)
    └── ctrl.h/c        # Unix domain socket control server (rebuild, scrub, repair)
```

### Core Concepts

**State Management**: `src/state.h` + `src/state.c` — `lr_state` owns all filesystem metadata: file table (`lr_hash`), ordered file list (`lr_list`), directory table and list (`lr_dir`), drive array (each `lr_drive` holds its own `lr_pos_allocator`), parity handle, rwlock.

**File Model**: Each `lr_file` stores vpath, drive index, real path on disk, size, parity position range `[pos_start, pos_start+block_count)`, mtime, mode, uid, gid, and open_count.

**Directory Model**: Each `lr_dir` stores vpath, mode, uid, gid, and mtime. Persisted in the content file. Only directories that have been explicitly created or had a metadata operation applied are tracked; synthetic ancestor directories are not.

**Parity Engine** (`src/parity.c`):
- Uses Intel ISA-L: `gf_gen_cauchy1_matrix`, `ec_init_tables`, `ec_encode_data`, `gf_invert_matrix`
- `parity_update_position` — reads all drive blocks at a position, encodes, writes parity; takes rdlock so safe to call from multiple threads concurrently
- `parity_recover_block` — multi-drive recovery via matrix inversion
- `parity_scrub(s, result, repair)` — full scan; if repair=1, rewrites mismatched blocks

**Write-Back Journal** (`src/journal.c`): Dirty-position bitmap. Background worker drains it every 5 s or on signal. When `parity_threads > 1`, positions are collected into an array and divided across N threads (each with its own scratch vector). Bitmap is persisted to `<content_path>.bitmap` for crash recovery.

**Control Server** (`src/ctrl.c`): Unix domain socket at `<content_path>.ctrl`. Commands: `rebuild DRIVE\n`, `scrub\n`, `scrub repair\n`.

### Key Data Structures

- `lr_state` (`state.h`) — root state object
- `lr_file` — file metadata: vpath, drive, size, parity positions, mtime, mode, uid, gid, open_count
- `lr_dir` — directory metadata: vpath, mode, uid, gid, mtime
- `lr_drive` — drive name, directory path, and per-drive `lr_pos_allocator`
- `lr_parity_handle` — open parity file descriptors + ISA-L encoding tables
- `lr_pos_allocator` — sorted free-extent allocator; one per drive (embedded in `lr_drive`)

### Limits

- `LR_DRIVE_MAX = 250` — derived from GF(2⁸): `nd + np ≤ 256`, with `LR_LEV_MAX = 6` parity levels reserved
- `LR_LEV_MAX = 6` — maximum parity levels (Cauchy matrix over GF(2⁸))
- Block size: 256 KiB default, configurable, must be multiple of 64 bytes

## Configuration

Runtime configuration in `liveraid.conf` (example: `liveraid.conf.example`). Key directives:

```ini
data NAME DIR          # Register a data drive
parity 1 PATH          # Level-1 parity file (can recover 1 drive failure)
parity 2 PATH          # Level-2 parity (up to level 6; must be contiguous from 1)
content PATH           # Metadata file (list multiple for redundancy)
mountpoint PATH        # FUSE mount point
blocksize 256          # Block size in KiB (default 256)
placement mostfree     # mostfree | lfs | pfrd | roundrobin
parity_threads 4       # Parallel threads for parity drain (default 1, max 64)
```

## Usage

```bash
./liveraid -c liveraid.conf /srv/array        # Mount (background)
./liveraid -c liveraid.conf -f /srv/array     # Mount (foreground)
fusermount3 -u /srv/array                     # Unmount
kill -USR1 $(pidof liveraid)                  # Trigger scrub (verify parity)
kill -USR2 $(pidof liveraid)                  # Trigger repair (fix mismatched parity)
./liveraid rebuild -c liveraid.conf -d 1      # Rebuild drive 1 from parity
```
