# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

liveraid is a FUSE filesystem that merges multiple data drives into a single namespace with live erasure-coded parity. Files are stored whole on one drive (no striping). Parity is maintained via a write-back journal: writes go immediately to the data drive, and a background thread computes and writes parity asynchronously. Up to 6 parity levels using ISA-L Cauchy-matrix GF(2⁸) erasure coding with AVX2 acceleration.

## Build Commands

```bash
make          # Compile binary → ./liveraid
make clean    # Remove objects and binary
```

Dependencies: `libfuse3-dev`, `libisal-dev`, `gcc`, `make`, `pkg-config`.

## Architecture

### Directory Layout

```
./
├── Makefile
├── liveraid.conf.example
└── src/
    ├── main.c          # Entry point: arg parse, rebuild dispatch, SIGUSR1 handler, fuse_main
    ├── config.h/c      # INI-style config parser
    ├── state.h/c       # In-memory state, file table (lr_hash), file list (lr_list),
    │                   # drive selection, per-drive position index
    ├── lr_hash.h/c     # Intrusive separate-chaining hash map (FNV-1a)
    ├── lr_list.h/c     # Intrusive doubly-linked list
    ├── alloc.h/c       # Global parity-position bump allocator + free list
    ├── metadata.h/c    # Content-file load/save (atomic write, CRC32)
    ├── fuse_ops.h/c    # FUSE3 high-level operation callbacks
    ├── parity.h/c      # Parity file I/O, ISA-L encode/recover wrappers
    ├── journal.h/c     # Dirty-position bitmap + background worker thread
    ├── rebuild.h/c     # Drive rebuild from parity (live via socket; offline fallback)
    └── ctrl.h/c        # Unix domain socket control server for live rebuild
```

### Core Concepts

**State Management**: `src/state.h` + `src/state.c` — `lr_state` owns all filesystem metadata: file table (`tommy_hashdyn`-style `lr_hash`), ordered file list (`lr_list`), drive array, parity handle, position allocator, rwlock.

**File Model**: Each `lr_file` stores vpath, drive index, real path on disk, size, parity position range `[pos_start, pos_start+block_count)`, mtime, mode, uid, gid, and open_count.

**Parity Engine** (`src/parity.c`):
- Uses Intel ISA-L: `gf_gen_cauchy1_matrix`, `ec_init_tables`, `ec_encode_data`, `gf_invert_matrix`
- `parity_update_position` — reads all drive blocks at a position, encodes, writes parity
- `parity_recover_block` — multi-drive recovery via matrix inversion
- `parity_scrub` — full scan comparing stored vs recomputed parity

**Write-Back Journal** (`src/journal.c`): Dirty-position bitmap. Background worker drains it every 5 s or on signal. Bitmap is persisted to `<content_path>.bitmap` for crash recovery.

**Control Server** (`src/ctrl.c`): Unix domain socket at `<content_path>.ctrl`. Accepts `rebuild DRIVE\n`, streams progress, handles live rebuilds without unmounting.

### Key Data Structures

- `lr_state` (`state.h`) — root state object
- `lr_file` — file metadata: vpath, drive, size, parity positions, mtime, mode, uid, gid, open_count
- `lr_drive` — drive name + directory path
- `lr_parity_handle` — open parity file descriptors + ISA-L encoding tables
- `lr_pos_allocator` — bump allocator for global parity position namespace

### Limits

- `LR_DRIVE_MAX = 250` — derived from GF(2⁸): `nd + np ≤ 256`, with `LR_LEV_MAX = 6` parity levels reserved
- `LR_LEV_MAX = 6` — maximum parity levels (Cauchy matrix over GF(2⁸))
- Block size: 256 KiB default, configurable, must be multiple of 64 bytes

## Configuration

Runtime configuration in `liveraid.conf` (example: `liveraid.conf.example`). Key directives:

```ini
data NAME DIR          # Register a data drive
parity PATH            # Level-1 parity file
2-parity PATH          # Level-2 parity (up to 6-parity)
content PATH           # Metadata file (list multiple for redundancy)
mountpoint PATH        # FUSE mount point
blocksize 256          # Block size in KiB (default 256)
placement mostfree     # mostfree | roundrobin
```

## Usage

```bash
./liveraid -c liveraid.conf /srv/array        # Mount (background)
./liveraid -c liveraid.conf -f /srv/array     # Mount (foreground)
fusermount3 -u /srv/array                     # Unmount
kill -USR1 $(pidof liveraid)                  # Trigger scrub
./liveraid rebuild -c liveraid.conf -d d1     # Rebuild drive d1 from parity
```
