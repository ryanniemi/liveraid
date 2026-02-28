# LiveRAID Internals

Implementation reference for contributors and advanced users. For installation,
configuration, and operational procedures, see [README.md](README.md).

## How It Works

### Namespace

The virtual filesystem merges all data drives. Files are looked up by their
virtual path (e.g. `/movies/foo.mkv`) in an in-memory hash table. Directories
are synthesized: a virtual directory exists for any path prefix that appears in
the file table.

### File placement

When a file is created, a drive is chosen according to `placement_policy`:
- **mostfree**: pick the drive with the most free bytes (`statvfs`) — default
- **lfs**: least-free-space; fill the fullest drive first
- **pfrd**: probabilistic weighted by free space; a drive with 2× the free space is 2× as likely to be chosen
- **roundrobin**: cycle through drives in config order

The file is stored entirely on that drive. Its real path is
`<drive_dir>/<virtual_path>`.

### Parity positions

Each data drive has its own independent position namespace. Position *K* on
drive *D* represents block *K* of that drive's files; position numbers on
different drives are unrelated and may overlap. A file on drive *D* assigned
parity positions [S, S+N) occupies blocks S through S+N-1 in drive *D*'s
namespace. The parity computation uses all drives' data at the same position *K*:
drives with no file at position *K* contribute a zero block.

```
parity[level][K] = ec_encode_data over { drive[0][K], drive[1][K], …, drive[nd-1][K] }
```

Each drive has its own sorted free-extent allocator. Positions are allocated
first-fit from free extents, falling back to the per-drive `next_free`
high-water mark. Freed ranges are merged with neighbors and persisted in the
content file as `# drive_next_free:` and `# drive_free_extent:` header lines.

Because files on different drives share a position namespace, multiple drives
may have data at the same position K simultaneously. The parity file therefore
needs capacity equal to `max(drive[d].next_free) × block_size` — roughly the
size of the single largest drive, not the total data across all drives. See the
[Storage overhead](README.md#storage-overhead) section in README.md for
guidance on parity capacity planning and block size selection.

### Write-back journal

The following operations mark parity positions dirty:

| Operation | Positions marked dirty |
|-----------|------------------------|
| `write` | Blocks touched by the write, plus any newly allocated blocks |
| `truncate` (grow) | Newly allocated blocks |
| `truncate` (shrink) | Freed blocks (so they are zeroed in parity) |
| `unlink` | All blocks the deleted file occupied |

Dirty positions are recorded in a per-bit bitmap (one bit per position).
A background worker thread wakes either on signal or after a 5-second
interval. It atomically swaps out the current bitmap, replaces it with an
empty one, then drains the old bitmap. If `parity_threads` is 1 (default),
dirty positions are processed serially. If `parity_threads` is greater than 1,
the dirty positions are collected into an array and divided into equal chunks;
each chunk is handled by a separate thread, each with its own scratch vector,
calling `parity_update_position` under a shared read lock on the state.

`parity_update_position` reads one block from each data drive at the given
position (zero-filling when no file covers that position), calls
`ec_encode_data(block_size, nd, np, gftbls, data, parity)` using the
precomputed Cauchy GF tables, and writes the resulting parity blocks to the
parity files.

On unmount, `journal_flush` is called before saving metadata: it kicks the
worker and blocks until both the bitmap is empty **and** the worker has
finished writing the batch it is currently processing. Parity is therefore
always consistent with the data at rest after a clean unmount.

### Crash journal

The in-memory dirty bitmap is saved to `<first_content_path>.bitmap` as part
of each periodic metadata save (every 5 minutes by default). On clean unmount
the file is deleted.

On remount, if the bitmap file is found:

1. The stored dirty bits are OR-ed into the fresh in-memory bitmap.
2. A message is printed to stderr (`journal: restored dirty bitmap …`).
3. The worker drains those positions — recomputing and writing parity — before
   the first parity-sweep cycle completes.

This bounds the stale-parity window after an unclean shutdown to at most one
save interval (5 minutes). Positions written after the last periodic save but
before the crash are not recorded and will have silently stale parity; running
a scrub after remount detects them.

### Scrub and repair

**Scrub** (`SIGUSR1`) verifies parity without modifying anything:

```sh
kill -USR1 $(pidof liveraid)
```

The background worker picks up the request at its next wake-up (within
`interval_ms`, default 5 s). It walks every parity position from 0 to
`next_free`, reads all data blocks, recomputes parity via `ec_encode_data`,
reads the stored parity, and compares them byte-for-byte. Results are
printed to stderr:

```
scrub: 4096 positions checked, 0 parity mismatches, 0 read errors
```

**Repair** (`SIGUSR2`) does the same walk but overwrites any mismatched parity
blocks with the correct values:

```sh
kill -USR2 $(pidof liveraid)
```

```
repair: 4096 positions checked, 12 mismatches, 12 fixed, 0 read errors
```

Use repair after a crash (to fix positions written after the last bitmap save)
or after adding a new parity level (to initialize the new parity file from
existing data).

**Implementation note:** `parity_scrub` reads each data block and the
corresponding parity blocks while holding `state_lock` as a read lock, but
releases it between positions. This means a concurrent write can update a
parity position after it has been checked; such a position will not be
reported as mismatched even if it was stale at the time the scrub began.
This is an intentional design trade-off: holding the lock for the entire scrub
would block all writes.

Both operations are also available via the control socket while mounted.
The socket path is `<first_content_path>.ctrl`:

```sh
echo "scrub"        | nc -U /var/lib/liveraid/liveraid.content.ctrl
echo "scrub repair" | nc -U /var/lib/liveraid/liveraid.content.ctrl
```

Socket response: `done CHECKED MISMATCHES errors=N` (scrub) or
`done CHECKED MISMATCHES fixed=N errors=N` (repair).

### Read recovery

When `pread` on a data file returns `EIO`, `lr_read` attempts to reconstruct
the block from parity:

1. For the known-failed drive, zero-fill its slot as a placeholder.
2. For every other data drive *d*: read the block at the same parity position.
   If drive *d* also returns `EIO`, add it to the failure list (up to *np*
   drives total; if more fail, return `EIO` to the caller).
3. Read the lowest *nfailed* parity levels.
4. Build an *nd×nd* decode matrix from the surviving rows of the Cauchy
   encoding matrix, invert it with `gf_invert_matrix`, and call
   `ec_encode_data` with the resulting decode coefficients to reconstruct all
   failed blocks simultaneously.
5. Copy the recovered block for the originally-requested drive to the read
   buffer, clamped to the file range.

Recovery is attempted block-by-block across the full read range; partial data
already assembled is returned if a later block cannot be recovered.

**Precondition:** parity must be current for the affected positions. Parity is
guaranteed current after a clean unmount; after a crash, run a repair
(`kill -USR2`) to rewrite any stale positions before relying on recovery.

### Drive failure handling

When a drive is physically missing or returns errors, LiveRAID degrades
gracefully at the file level:

**Read-only access (transparent):** If `open(2)` on the real file fails with
`ENOENT`, `EIO`, or `ENXIO`, and the file was opened `O_RDONLY`, and parity is
configured, `lr_open` allocates an `lr_fh_t` with `fd = -1` and stores its
pointer in `fi->fh`, then returns success. Subsequent `read` calls detect
`fh->fd == -1` and go straight to the parity recovery path described above.
No error is ever returned to the calling application; it reads the file as if
the drive were healthy.

**Metadata (mode/uid/gid/mtime):** When the real file is not accessible,
`lr_getattr` returns the stored mode, uid, gid, and mtime from the content
file. These values are populated at `create` time via `fstat` and updated when
`chmod`, `chown`, or `utimens` is called on the virtual path.

**Write access:** Writes to a dead drive return `EIO`; the file must be
rebuilt before it can be written again.

### Metadata

On unmount (FUSE `destroy` callback), after `journal_flush` completes, the
content file is written atomically to every configured `content` path:

```
# liveraid content
# version: 1
# blocksize: 262144
# drive_next_free: 1 2792
# drive_next_free: 2 4
file|1|/movies/foo.mkv|734003200|0|2792|1706745600|0|100644|1000|1000
file|2|/docs/a.pdf|1048576|0|4|1706745601|0|100600|1000|1000
dir|/movies|40755|1000|1000|1706745600|0
dir|/docs|40700|1000|1000|1706745601|0
# crc32: A3F1CC02
```

Header lines (before `file|` and `dir|` records):

- `# drive_next_free: NAME N` — high-water mark of the position allocator for
  drive `NAME`; one line per drive.
- `# drive_free_extent: NAME START COUNT` — a free position extent for drive
  `NAME`; zero or more lines per drive, produced when positions are freed by
  `unlink` or `truncate`. Extents are restored to the per-drive allocator on
  load so freed positions can be reused.

File fields: `file|DRIVE|VPATH|SIZE|PARITY_POS_START|BLOCK_COUNT|MTIME_SEC|MTIME_NSEC|MODE|UID|GID`

Directory fields: `dir|VPATH|MODE|UID|GID|MTIME_SEC|MTIME_NSEC`

- `MODE` is the full `st_mode` value in octal (e.g. `100644` = regular file, `40755` = directory).
- `UID` / `GID` are decimal owner and group IDs.
- Directory records are written for directories that have been explicitly created
  (`mkdir`) or had a metadata operation applied (`chmod`, `chown`, `utimens`).
  Ancestor directories that exist implicitly because files were placed in them
  are not recorded and report mode `0755`, uid/gid `0`, and epoch mtime.
- Content files from older versions that omit the last three `file` fields
  default to mode `100644`, uid `0`, gid `0` on load — backward compatible.
- Old-format `# next_free_pos:` / `# free_extent:` global headers are silently
  ignored on load; per-drive allocator state is derived from file records instead.

The `# crc32:` footer is the IEEE 802.3 CRC32 of everything before that line.
A mismatch on load prints a warning to stderr but parsing continues.

On mount, the first readable content file is loaded to restore the file table
and parity position allocator. The background journal worker also saves metadata
every 5 minutes so the file table is not stale after an unclean shutdown.

Alongside each periodic metadata save, the in-memory dirty-position bitmap is
written to `<first_content_path>.bitmap` (binary: `LRBM` magic, word count,
uint64 array). The array is written in host byte order and is not portable
across machines with different endianness; it is intended only for crash
recovery on the same host. On clean unmount the bitmap file is deleted. On
unclean remount, if the file is present, the stored bits are OR-merged into
the fresh bitmap so stale parity positions are recomputed by the background
worker.

## Source Layout

```
liveraid/
├── Makefile
├── liveraid.conf.example
└── src/
    ├── main.c          # Entry point: arg parse, rebuild dispatch,
    │                   # init sequence, SIGUSR1/USR2 handlers, fuse_main
    ├── config.h/c      # INI-style config parser
    ├── state.h/c       # In-memory state, file table (lr_hash),
    │                   # file list (lr_list), dir table/list,
    │                   # drive selection, per-drive position index
    │                   # lr_file: vpath, real_path, size, parity positions,
    │                   # mtime, mode, uid, gid
    │                   # lr_dir: vpath, mode, uid, gid, mtime
    ├── lr_hash.h/c     # Intrusive separate-chaining hash map (FNV-1a)
    ├── lr_list.h/c     # Intrusive doubly-linked list
    ├── alloc.h/c       # Per-drive parity-position allocator (sorted free extents + high-water mark)
    ├── metadata.h/c    # Content-file load/save (atomic write, CRC32)
    │                   # 11-field format with mode/uid/gid; backward-compat load
    ├── fuse_ops.h/c    # FUSE3 high-level operation callbacks
    │                   # lr_fh_t per-open struct (fd + vpath) in fi->fh
    ├── parity.h/c      # Parity file I/O, ISA-L encode/recover wrappers,
    │                   # lr_alloc_vector, parity_update_position,
    │                   # parity_recover_block (multi-drive), parity_scrub
    ├── journal.h/c     # Dirty-position bitmap + background worker thread
    │                   # (parity sweep, periodic save, crash journal, scrub)
    ├── rebuild.h/c     # Drive rebuild from parity
    │                   # (try_live_rebuild via ctrl socket; offline fallback)
    └── ctrl.h/c        # Unix domain socket control server
                        # (live rebuild, scrub, repair; open_count busy-skip)
```

Runtime dependencies: `libfuse3`, `libisal`. No external source trees required.
