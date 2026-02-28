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
- **Drive selection**: `mostfree` (default), `lfs`, `pfrd`, or `roundrobin`

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

# Block size in KiB (default 256); the resulting byte count must be a multiple of 64
#blocksize 256

# Drive selection policy for new files: mostfree | lfs | pfrd | roundrobin
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
| `placement POLICY` | no | `mostfree` (default) — most free space; `lfs` — least free space (fill fullest drive first); `pfrd` — weighted random by free space; `roundrobin` — cycle in config order. |
| `parity_threads N` | no | Threads used to drain the dirty-parity bitmap in parallel (default 1, max 64). Each thread processes an independent subset of dirty positions. |

## Storage overhead

### Parity file size

Like traditional RAID-5, **each parity level requires capacity roughly equal
to the largest single data drive**. Each drive has its own independent position
namespace, so files on different drives can occupy the same position number.
The parity file needs to cover `max(drive.next_free) × block_size` — dominated
by the drive with the most data, not the total across all drives:

| Array | Largest drive | Parity (1 level) | Parity (2 levels) |
|-------|---------------|------------------|-------------------|
| 5 × 10 TB (balanced) | ~10 TB | ~10 TB | ~20 TB |
| 3 × 8 TB + 2 × 4 TB | 8 TB | ~8 TB | ~16 TB |

Plan your parity drive(s) accordingly — a single parity file needs capacity
equal to the largest data drive, similar to a traditional RAID-5 parity disk.

The parity file is never truncated; its size grows to the high-water mark of
positions ever allocated across all drives. Deleted files' positions are
reclaimed and reused per drive, so it does not grow without bound, but it
does not shrink either.

### Block size

Every file occupies `⌈file_size / block_size⌉` parity positions; the final
block is zero-padded. The average wasted parity space per file is
`block_size / 2`. For a collection of large files this is negligible; for
many small files it adds up.

Each dirty parity position also requires reading one full block from every
data drive to recompute parity (`nd × block_size` bytes of reads per
position update). Smaller blocks reduce this per-update I/O cost.

As a rule of thumb, choose a block size no larger than about one tenth of
your average file size, so last-block rounding waste stays below ~10%:

| Typical file sizes | Suggested block size |
|--------------------|----------------------|
| > 2.5 MiB (movies, ISOs, backups) | 256 KiB (default) |
| 256 KiB – 2.5 MiB | 64 KiB |
| < 256 KiB | 16 KiB or smaller |

The block size is fixed at array-creation time and stored in the content
file. Changing it requires rebuilding the array from scratch.

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

## Rebuild

After replacing a failed drive, use `liveraid rebuild` to reconstruct all
files that belong to that drive from parity.

### Live rebuild (filesystem mounted)

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

### Offline rebuild (filesystem unmounted)

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

### Common to both modes

1. Loads the content file to find all files assigned to the named drive.
2. For each file, reconstructs every block via `parity_recover_block`.
3. Creates the real file at `<drive_dir>/<virtual_path>` with recovered data.
4. Restores the file's mode, uid, gid, and mtime from stored metadata.

After a successful rebuild, remount (if needed) — the drive is fully
operational.

## Array management

### Adding a data drive

1. Register the new drive in the config: `data N /mnt/diskN/`
2. Unmount and remount. New files will be placed on the new drive according to
   the placement policy; existing files are unaffected.

### Removing a data drive

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

### Adding a parity level

1. Add the new parity line to the config: `parity N /mnt/parityN/liveraid.parity`
   Levels must remain contiguous from 1, so add the next level in sequence.
2. Unmount and remount. The new parity file starts empty (all zeros).
3. Run a repair pass to initialize it from existing data:
   ```sh
   kill -USR2 $(pidof liveraid)
   ```
   Wait for the repair to complete (watch stderr for the `repair: … fixed=…`
   line) before relying on the new parity level for recovery.

### Removing a parity level

Only the highest-numbered level can be removed (levels must remain contiguous).

1. Unmount.
2. Remove the highest `parity N` line from the config.
3. Remount. The unused parity file can be deleted or left in place; it will not
   be opened or written.

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
- Free extents are persisted in the content file as `# drive_free_extent:`
  header lines and restored on remount, so deleted files' positions are reused
  across sessions. The parity file is never truncated; its size is bounded by
  the high-water mark of positions ever allocated.

## License

MIT — see [LICENSE](LICENSE).

For a detailed description of the implementation, see [INTERNALS.md](INTERNALS.md).
