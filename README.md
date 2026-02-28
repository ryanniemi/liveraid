# LiveRAID

> [!WARNING]
> **LiveRAID is experimental software. It will probably eat your data.**
> Do not use it for anything you care about. There are known limitations around
> crash recovery, the allocator never compacts freed space, and the codebase has
> not been audited or stress-tested. Use a mature solution (ZFS, mdadm, SnapRAID)
> for real data protection.

A FUSE filesystem that merges multiple data drives of any size into a single
namespace, with live parity computed in the background. Unlike traditional
striped RAID, files are stored whole on a single drive — so if you lose more
drives than you have parity levels, only the files on those specific drives are
gone. Everything on the surviving drives remains fully intact and directly
readable. Parity is kept current via a write-back journal: writes go
immediately to the data drive, and a background thread computes and writes
parity asynchronously.

Because files are placed on individual drives rather than striped across all of
them, drives can be different sizes and a new drive can be added to the array
simply by registering it in the config — no rebalancing or resilvering needed.
Parity protection can also be scaled up or down at any time by adding or
removing parity files and updating the config.

Uses Intel ISA-L (`libisal`) for Cauchy-matrix GF(2⁸) erasure coding.

### The key difference from traditional RAID

Traditional striped RAID (RAID-5, RAID-6, etc.) spreads every file's data
across all drives. This means that if you lose more drives than you have parity
levels, **every file on the entire array becomes unrecoverable** — even files
that had nothing to do with the failed drives.

LiveRAID stores each file whole on a single drive. Parity is still computed
across the array and can recover any drive's contents, but critically: losing
more drives than you have parity levels only affects **the files that were
physically on those specific drives**. All files on the surviving drives remain
fully intact and directly readable — no recovery needed. You lose some files,
not everything.

For example, with 10 data drives and 1 parity drive:
- **RAID-5**: lose any 2 drives → entire array unrecoverable
- **LiveRAID**: lose any 2 drives → files on those 2 drives unrecoverable, files on the other 8 drives untouched

This makes LiveRAID a better fit for large media collections and archival
storage where total array loss is a worse outcome than partial file loss.

## Features

- **Drive merging**: up to 250 data drives under a single mount point (like mergerfs)
- **Any underlying filesystem**: each data drive can use any filesystem (ext4, XFS, btrfs, etc.) — no uniformity required
- **Mixed drive sizes**: drives can be any capacity — no requirement to match sizes
- **Easy expansion**: add a drive by registering it in the config; no rebalancing required
- **Adjustable parity**: scale from 1 to 6 parity levels at any time by adding or removing parity files
- **Erasure coding**: ISA-L Cauchy-matrix GF(2⁸) with AVX2 acceleration
- **Whole-file placement**: each file lives entirely on one drive (like UnRAID)
- **Live parity**: dirty blocks queued in a bitmap; background thread drains it
- **Transparent read recovery**: up to *np* simultaneous drive failures reconstructed from parity
- **Transparent open on dead drive**: read-only opens succeed even when a drive is missing, routing immediately to parity recovery (no user-visible error)
- **Full metadata survival**: file and directory mode, uid, gid, and mtime are stored in the content file and served from stored state when the backing drive is unavailable
- **Offline rebuild**: `./liveraid rebuild -c CONFIG -d DRIVE_NAME` reconstructs all files on a replaced drive from parity, restoring permissions and timestamps
- **Live rebuild**: if the filesystem is mounted, `liveraid rebuild` automatically connects via a Unix domain socket and rebuilds without unmounting; files currently open are skipped and reported
- **Crash-consistent journal**: dirty bitmap saved to disk periodically; restored on unclean remount
- **Scrub**: `kill -USR1 <pid>` verifies parity against data; `kill -USR2 <pid>` repairs any mismatches
- **Persistent metadata**: content file saved atomically on unmount and every 5 min
- **CRC32 integrity**: content file footer detects corruption at load time
- **Drive selection**: `mostfree` (default) or `roundrobin`

## Requirements

- Linux with FUSE3 kernel support
- `libfuse3-dev` (`apt install libfuse3-dev`)
- `libisal-dev` (`apt install libisal-dev`) — Intel ISA-L erasure coding
- gcc, make, pkg-config

## Build

```sh
git clone https://github.com/ryanniemi/liveraid
cd liveraid
make
```

The binary `liveraid` is placed in the current directory.

```sh
make clean   # remove objects and binary
```

## Configuration

Copy `liveraid.conf.example` and edit it:

```ini
# Data drives: "data NAME DIR"
data 1 /mnt/disk1/
data 2 /mnt/disk2/
data 3 /mnt/disk3/

# Parity files: "parity LEVEL PATH" (up to 6 levels, contiguous from 1)
parity 1 /mnt/parity1/liveraid.parity
parity 2 /mnt/parity2/liveraid.parity

# Content file (metadata; list multiple for redundancy)
content /var/lib/liveraid/liveraid.content
content /mnt/disk1/liveraid.content

# Mount point
mountpoint /srv/array

# Block size in KiB (default 256, must be multiple of 64 bytes)
#blocksize 256

# Drive selection policy for new files: mostfree | roundrobin
#placement mostfree
```

**Directives:**

| Directive | Required | Description |
|-----------|----------|-------------|
| `data NAME DIR` | yes (≥1) | Register a data drive. `NAME` is used in the content file; `DIR` is the real path on disk. |
| `parity LEVEL PATH` | no | Parity file for the given level (1–6). Levels must be contiguous starting from 1. Level 1 recovers 1 failed drive; each additional level recovers one more. |
| `content PATH` | yes (≥1) | Where to save file metadata. List multiple paths for redundancy (all are written on save, first found is loaded). |
| `mountpoint PATH` | yes | FUSE mount point. |
| `blocksize KiB` | no | Parity block size in KiB (default 256). Must be a multiple of 64 bytes. |
| `placement POLICY` | no | `mostfree` (default) or `roundrobin`. |

## Usage

```sh
# Mount in the foreground (useful for debugging)
./liveraid -c /etc/liveraid.conf -f /srv/array

# Mount in the background
./liveraid -c /etc/liveraid.conf /srv/array

# Unmount
fusermount3 -u /srv/array

# Verify parity (read-only, reports mismatches to stderr)
kill -USR1 $(pidof liveraid)

# Repair parity (rewrite any mismatched blocks — use after a crash or after
# adding a new parity level)
kill -USR2 $(pidof liveraid)

# Rebuild a replaced drive from parity.
# Automatically runs live (via socket) if mounted, offline otherwise.
./liveraid rebuild -c /etc/liveraid.conf -d 1

# Scrub/repair via control socket (filesystem must be mounted)
echo "scrub"        | nc -U /var/lib/liveraid/liveraid.content.ctrl
echo "scrub repair" | nc -U /var/lib/liveraid/liveraid.content.ctrl
```

Standard FUSE options (`-d`, `-s`, `-o allow_other`, etc.) are passed through.

## How It Works

### Namespace

The virtual filesystem merges all data drives. Files are looked up by their
virtual path (e.g. `/movies/foo.mkv`) in an in-memory hash table. Directories
are synthesized: a virtual directory exists for any path prefix that appears in
the file table.

### File placement

When a file is created, a drive is chosen according to `placement_policy`:
- **mostfree**: pick the drive with the most free bytes (`statvfs`)
- **roundrobin**: cycle through drives in order

The file is stored entirely on that drive. Its real path is
`<drive_dir>/<virtual_path>`.

### Parity positions

All data drives share a single global position namespace. Position *K*
represents block *K* on every drive simultaneously. A file assigned parity
positions [S, S+N) contributes its N blocks at those positions; any drive that
has no file covering position K contributes a zero block at that position.

```
parity[level][K] = ec_encode_data over { drive[0][K], drive[1][K], …, drive[nd-1][K] }
```

Positions are allocated from a sorted free-extent list (first-fit), falling
back to advancing the `next_free` high-water mark when no suitable free range
exists. Freed ranges are returned to the extent list with neighbor merging and
are persisted across remounts in the content file. Because files on different
drives get distinct ranges, a position typically has data on exactly one drive
and zeros on all others — parity at that position equals the single drive's
block. This is still correct and recoverable; it just means the parity file is
proportionally larger than a tightly-packed layout would require.

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
empty one, then drains the old bitmap — calling `parity_update_position` for
each set bit while holding a read lock on the state.

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
configured, `lr_open` stores the sentinel value `LR_DEAD_DRIVE_FH` in the file
handle and returns success. Subsequent `read` calls skip the `pread` entirely
and go straight to the parity recovery path described above. No error is ever
returned to the calling application; it reads the file as if the drive were
healthy.

**Metadata (mode/uid/gid/mtime):** When the real file is not accessible,
`lr_getattr` returns the stored mode, uid, gid, and mtime from the content
file. These values are populated at `create` time via `fstat` and updated when
`chmod`, `chown`, or `utimens` is called on the virtual path.

**Write access:** Writes to a dead drive return `EIO`; the file must be
rebuilt before it can be written again.

### Rebuild

After replacing a failed drive, use `liveraid rebuild` to reconstruct all
files that belong to that drive from parity.

#### Live rebuild (filesystem mounted)

When the filesystem is mounted, `liveraid rebuild` connects to the running
process via a Unix domain socket (`<first_content_path>.ctrl`) and rebuilds
without unmounting. The running process handles the request under its existing
`state_lock`, keeping parity and the file table consistent.

```sh
./liveraid rebuild -c /etc/liveraid.conf -d 1
```

Progress is streamed to stdout:

```
progress 0 3 (starting)
progress 1 3 /movies/foo.mkv
ok /movies/foo.mkv
progress 2 3 /docs/a.pdf
ok /docs/a.pdf
progress 3 3 /photos/img.jpg
skip /photos/img.jpg busy
done 2 0 skipped=1
```

Files that are currently open (`open_count > 0`) are skipped with
`skip PATH busy` and counted in the `skipped=N` summary. They can be rebuilt
in a subsequent run once they are closed.

Exit status is 0 if no files failed, 1 if any failed.

#### Offline rebuild (filesystem unmounted)

If no running process is detected (socket absent), rebuild falls back to an
offline mode that opens the parity files directly:

```sh
fusermount3 -u /srv/array
./liveraid rebuild -c /etc/liveraid.conf -d 1
```

Output is one line per file to stderr:

```
rebuild: drive '1' (/mnt/disk1/) — 3 file(s) to reconstruct
rebuild: [1/3] OK   /movies/foo.mkv
rebuild: [2/3] OK   /docs/a.pdf
rebuild: [3/3] FAIL /photos/img.jpg
rebuild: complete — 2 rebuilt, 1 failed
```

#### Common to both modes

1. Loads the content file to find all files assigned to the named drive.
2. For each file, reconstructs every block via `parity_recover_block`.
3. Creates the real file at `<drive_dir>/<virtual_path>` with recovered data.
4. Restores the file's mode, uid, gid, and mtime from stored metadata.

After a successful rebuild, remount (if needed) — the drive is fully
operational.

### Array management

#### Adding a data drive

1. Register the new drive in the config: `data N /mnt/diskN/`
2. Unmount and remount. New files will be placed on the new drive according to
   the placement policy; existing files are unaffected.

#### Removing a data drive

All files on the drive must be vacated before removing it from the config.
The content file stores drive names, so any file whose drive name is absent
from the config will be silently dropped on load.

There is no built-in drive-drain command. The safest procedure:

1. Copy the files to a location outside the array.
2. Delete them from the array through the FUSE mount (this keeps parity current).
3. Copy them back into the array through the FUSE mount (they will land on
   other drives according to the placement policy).
4. Verify no files remain on the drive.
5. Unmount, remove the `data` line from the config, remount.

If the drive has failed and its data is unrecoverable, use `rebuild` to
reconstruct the files onto remaining drives (see [Rebuild](#rebuild) above),
then remove the dead drive from the config.

#### Adding a parity level

1. Add the new parity line to the config: `parity N /mnt/parityN/liveraid.parity`
   Levels must remain contiguous from 1, so add the next level in sequence.
2. Unmount and remount. The new parity file starts empty (all zeros).
3. Run a repair pass to initialize it from existing data:
   ```sh
   kill -USR2 $(pidof liveraid)
   ```
   Wait for the repair to complete (watch stderr for the `repair: … fixed=…`
   line) before relying on the new parity level for recovery.

#### Removing a parity level

Only the highest-numbered level can be removed (levels must remain contiguous).

1. Unmount.
2. Remove the highest `parity N` line from the config.
3. Remount. The unused parity file can be deleted or left in place; it will not
   be opened or written.

### Metadata

On unmount (FUSE `destroy` callback), after `journal_flush` completes, the
content file is written atomically to every configured `content` path:

```
# liveraid content
# version: 1
# blocksize: 262144
# next_free_pos: 4096
file|1|/movies/foo.mkv|734003200|0|2792|1706745600|0|100644|1000|1000
file|2|/docs/a.pdf|1048576|2792|4|1706745601|0|100600|1000|1000
dir|/movies|40755|1000|1000|1706745600|0
dir|/docs|40700|1000|1000|1706745601|0
# crc32: A3F1CC02
```

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

The `# crc32:` footer is the IEEE 802.3 CRC32 of everything before that line.
A mismatch on load prints a warning to stderr but parsing continues.

On mount, the first readable content file is loaded to restore the file table
and parity position allocator. The background journal worker also saves metadata
every 5 minutes so the file table is not stale after an unclean shutdown.

Alongside each periodic metadata save, the in-memory dirty-position bitmap is
written to `<first_content_path>.bitmap` (binary: `LRBM` magic, word count,
uint64 array). On clean unmount the bitmap file is deleted. On unclean remount,
if the file is present, the stored bits are OR-merged into the fresh bitmap so
stale parity positions are recomputed by the background worker.

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
    ├── alloc.h/c       # Global parity-position allocator (sorted free extents + high-water mark)
    ├── metadata.h/c    # Content-file load/save (atomic write, CRC32)
    │                   # 11-field format with mode/uid/gid; backward-compat load
    ├── fuse_ops.h/c    # FUSE3 high-level operation callbacks
    │                   # Dead-drive sentinel (LR_DEAD_DRIVE_FH) in open/read/write
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

## Limitations

**Parity / recovery**
- Crash recovery is best-effort: the dirty bitmap is saved every 5 minutes
  (hardcoded; not currently a config option). Writes in the window
  between two saves are not recorded; after a crash those positions may have
  stale parity that is not flagged for recomputation. A clean unmount always
  flushes parity and deletes the bitmap file.
- Read recovery requires parity to be current for the affected position. A
  crash before the background sweep can leave parity stale, resulting in
  silently wrong recovered data. A post-crash repair (`kill -USR2`) detects
  and fixes such mismatches.
- Multi-drive recovery is limited to *np* simultaneous failures (one per
  configured parity level). With a single parity file, at most one drive can
  be reconstructed at a time.

**Filesystem**
- No hard link support.
- No xattr support.
- Virtual directory `mtime`/`ctime` is only tracked for directories that have been explicitly created or had a metadata operation applied. Directories that exist implicitly because files were placed in them always report epoch mtime.
- Write access to a file whose drive is missing returns `EIO`; use `rebuild`
  to restore the drive before writing.
- Live rebuild skips files that are currently open; a subsequent `rebuild` run
  (or remount) is needed to recover them.

**Allocator**
- Free extents are persisted in the content file as `# free_extent:` header
  lines and restored on remount, so deleted files' positions are reused across
  sessions. The parity file is never truncated; its size is bounded by the
  high-water mark of positions ever allocated.

## License

MIT — see [LICENSE](LICENSE).
