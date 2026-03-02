#!/bin/bash
# Integration test suite for liveraid.
#
# Prerequisites:
#   - liveraid binary built:  make
#   - fusermount3 available:  apt install fuse3
#   - /tmp with ~500 MiB free
#
# Usage (from repo root):
#   bash tests/integration.sh
#
# Tests cover:
#   1. parity_threads=4 parallel drain + scrub (0 mismatches)
#   2. rmdir (empty and non-empty)
#   3. rename across directories + remount persistence
#   4. 2 parity levels, simultaneous 2-drive failure transparent recovery
#   5. Offline rebuild of 2 drives from 2-level parity
#   6. Live rebuild (socket) with busy-file skip + subsequent rebuild after close
#   7. Crash recovery: bitmap_interval 3, bitmap saved before drain + persists after kill -9
#   8. Multiple content paths: both written on save, secondary used when primary missing
#   9. Empty files: size=0 created and survives remount
#  10. Directory metadata: chmod and utimens persist across remount
#  11. Control socket: scrub and scrub repair return 0 mismatches
#  12. Position reuse: parity position freed by unlink is reused by next alloc
#  13. chown: uid/gid on files and dirs, immediate + remount persistence
#  14. Placement policies: mostfree, lfs, pfrd smoke test (8 files + parity clean)
#  15. Symlinks: create, readlink, getattr S_IFLNK, readdir, persistence, unlink
#
# Tests 1-9 use 4 drives + 2 parity levels + parity_threads=4.
# Tests 10-15 use various configs as noted inline.
# Drives and parity are created under /tmp/lrt/ and cleaned up after each test.

set -euo pipefail

MNT=/tmp/lrt/mount
CONF=/tmp/lrt/liveraid.conf
# Resolve the binary relative to the repo root (script lives in tests/)
REPO=$(cd "$(dirname "$0")/.." && pwd)
BIN=$REPO/liveraid
PASS=0; FAIL=0

pass() { echo "  PASS  $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL  $1: ${2:-}"; FAIL=$((FAIL+1)); }

# Check a repair/scrub result line for zero mismatches and zero errors.
# Handles both signal format ("0 mismatches, 0 fixed, 0 read errors")
# and socket format ("0 mismatches, fixed=0 errors=0").
check_clean_parity() {
    local line="$1"
    echo "$line" | grep -qP "0 mismatches" || return 1
    echo "$line" | grep -qP "(0 fixed|fixed=0)" || return 1
    echo "$line" | grep -qP "(0 read errors|errors=0)" || return 1
    return 0
}

mount_fs() {
    $BIN -c $CONF -f $MNT >>/tmp/lrt/liveraid.log 2>&1 &
    sleep 1.5
    if ! grep -q lrt /proc/mounts; then
        echo "ERROR: mount failed"; tail -5 /tmp/lrt/liveraid.log; exit 1
    fi
}

unmount_fs() {
    fusermount3 -u $MNT 2>/dev/null || true
    sleep 0.3
}

wipe_data() {
    unmount_fs
    rm -rf /tmp/lrt/d1/* /tmp/lrt/d2/* /tmp/lrt/d3/* /tmp/lrt/d4/* \
            /tmp/lrt/parity1/* /tmp/lrt/parity2/* /tmp/lrt/content/* 2>/dev/null || true
    > /tmp/lrt/liveraid.log
}

# ===================================================================
echo ""
echo "=== parity_threads=4: parallel drain + scrub ==="
wipe_data
mount_fs

for i in $(seq 1 20); do
    dd if=/dev/urandom of=$MNT/file${i}.bin bs=65536 count=3 2>/dev/null
done
sleep 11   # two drain cycles

kill -USR2 $(pidof liveraid)
sleep 4
unmount_fs

result=$(grep "repair:" /tmp/lrt/liveraid.log | tail -1 || true)
echo "  $result"
if [ -n "$result" ] && check_clean_parity "$result"; then
    pass "parity_threads=4: 20 files, parallel drain, 0 mismatches"
else
    fail "parity_threads=4" "$result"
fi

# ===================================================================
echo ""
echo "=== rmdir ==="
wipe_data
mount_fs

mkdir $MNT/toremove
[ -d $MNT/toremove ] && pass "rmdir: mkdir succeeds" || fail "rmdir" "mkdir failed"
rmdir $MNT/toremove
[ ! -e $MNT/toremove ] && pass "rmdir: dir removed" || fail "rmdir" "dir still present"

mkdir $MNT/nonempty
echo "x" > $MNT/nonempty/file.txt
rmdir $MNT/nonempty 2>/dev/null && fail "rmdir" "non-empty should fail" || pass "rmdir: non-empty dir correctly refused"
unmount_fs

# ===================================================================
echo ""
echo "=== rename across directories ==="
wipe_data
mount_fs

mkdir $MNT/src_dir
mkdir $MNT/dst_dir
echo "cross-dir rename" > $MNT/src_dir/file.txt
mv $MNT/src_dir/file.txt $MNT/dst_dir/file.txt

[ ! -e $MNT/src_dir/file.txt ] && [ "$(cat $MNT/dst_dir/file.txt)" = "cross-dir rename" ] \
    && pass "rename across dirs" || fail "rename across dirs"
unmount_fs; mount_fs
[ ! -e $MNT/src_dir/file.txt ] && [ "$(cat $MNT/dst_dir/file.txt)" = "cross-dir rename" ] \
    && pass "cross-dir rename survives remount" || fail "cross-dir rename lost on remount"
unmount_fs

# ===================================================================
echo ""
echo "=== 2 parity levels: simultaneous 2-drive failure recovery ==="
wipe_data
mount_fs

for i in $(seq 1 8); do
    printf "content of file %d" "$i" > $MNT/f${i}.txt
done

d1_files=$(ls /tmp/lrt/d1/*.txt 2>/dev/null | xargs -I{} basename {} || true)
d2_files=$(ls /tmp/lrt/d2/*.txt 2>/dev/null | xargs -I{} basename {} || true)
echo "  d1: $(echo $d1_files)   d2: $(echo $d2_files)"

sleep 11   # parity drain

rm -f /tmp/lrt/d1/*.txt /tmp/lrt/d2/*.txt
echo "  d1 and d2 wiped"

all_ok=1
for f in $d1_files $d2_files; do
    num=$(echo "$f" | grep -oP '\d+')
    actual=$(cat $MNT/$f 2>&1)
    expected="content of file $num"
    if [ "$actual" = "$expected" ]; then echo "    $f: OK"
    else echo "    $f: FAIL got='$actual'"; all_ok=0; fi
done
[ "$all_ok" = "1" ] \
    && pass "2-drive failure: all files recovered from 2-level parity" \
    || fail "2-drive failure" "some files not recovered"
unmount_fs

# ===================================================================
echo ""
echo "=== Offline rebuild: restore d1 and d2 ==="
wipe_data
mount_fs

for i in $(seq 1 8); do printf "rebuild file %d" "$i" > $MNT/r${i}.txt; done
d1_files=$(ls /tmp/lrt/d1/*.txt 2>/dev/null | xargs -I{} basename {} || true)
d2_files=$(ls /tmp/lrt/d2/*.txt 2>/dev/null | xargs -I{} basename {} || true)
unmount_fs

rm -f /tmp/lrt/d1/*.txt /tmp/lrt/d2/*.txt

$BIN rebuild -c $CONF -d d1 2>&1 | grep "^rebuild:"
$BIN rebuild -c $CONF -d d2 2>&1 | grep "^rebuild:"

all_ok=1
for f in $d1_files; do
    num=$(echo "$f" | grep -oP '\d+')
    got=$(cat /tmp/lrt/d1/$f 2>/dev/null || echo "MISSING")
    [ "$got" = "rebuild file $num" ] || { echo "  d1/$f: got='$got'"; all_ok=0; }
done
for f in $d2_files; do
    num=$(echo "$f" | grep -oP '\d+')
    got=$(cat /tmp/lrt/d2/$f 2>/dev/null || echo "MISSING")
    [ "$got" = "rebuild file $num" ] || { echo "  d2/$f: got='$got'"; all_ok=0; }
done
[ "$all_ok" = "1" ] && pass "offline rebuild of d1 and d2" || fail "offline rebuild" "content mismatch"

# ===================================================================
echo ""
echo "=== Live rebuild: busy-file skip ==="
wipe_data
mount_fs

# roundrobin: files 1,5 → d1;  2,6 → d2;  3,7 → d3;  4,8 → d4
for i in 1 2 3 4 5; do printf "live file %d" "$i" > $MNT/live${i}.txt; done

d1_files=$(ls /tmp/lrt/d1/live*.txt 2>/dev/null | xargs -I{} basename {} || true)
echo "  d1 files: $d1_files"

sleep 11   # parity drain

# Hold live5.txt open (it's on d1, second file there)
exec 5< $MNT/live5.txt

rm -f /tmp/lrt/d1/live*.txt   # simulate d1 data loss

rebuild_out=$($BIN rebuild -c $CONF -d d1 2>&1)
echo "$rebuild_out"

if echo "$rebuild_out" | grep -q "skip.*live5.txt.*busy"; then
    pass "live rebuild: open file skipped (busy)"
else
    fail "live rebuild" "expected busy skip for live5.txt"
fi

rebuilt=$(echo "$rebuild_out" | grep -c "^ok " || true)
[ "$rebuilt" -ge 1 ] && pass "live rebuild: $rebuilt non-busy file(s) rebuilt" \
                       || fail "live rebuild" "no files rebuilt"

# Close and rebuild again
exec 5<&-
rebuild_out2=$($BIN rebuild -c $CONF -d d1 2>&1)
echo "$rebuild_out2"
echo "$rebuild_out2" | grep -q "ok.*live5.txt" \
    && pass "live rebuild: busy file rebuilt after fd closed" \
    || fail "live rebuild" "live5.txt not rebuilt after close"

unmount_fs

# ===================================================================
echo ""
echo "=== Crash recovery: bitmap persistence + kill -9 ==="
wipe_data

# Use a short bitmap_interval so we don't wait 5 minutes
cat > /tmp/lrt/crash_recovery.conf << 'CONFEOF'
data d1 /tmp/lrt/d1
data d2 /tmp/lrt/d2
data d3 /tmp/lrt/d3
data d4 /tmp/lrt/d4
parity 1 /tmp/lrt/parity1/liveraid.parity
parity 2 /tmp/lrt/parity2/liveraid.parity
content /tmp/lrt/content/liveraid.content
mountpoint /tmp/lrt/mount
blocksize 64
placement roundrobin
parity_threads 4
bitmap_interval 3
CONFEOF

$BIN -c /tmp/lrt/crash_recovery.conf -f $MNT >>/tmp/lrt/liveraid.log 2>&1 &
sleep 1.5
if ! grep -q lrt /proc/mounts; then
    echo "ERROR: mount failed"; tail -5 /tmp/lrt/liveraid.log; exit 1
fi

# Phase 1: write data via an open fd (keeping fd open prevents lr_flush from
# calling journal_flush, so dirty positions stay in the bitmap).  The background
# worker saves the bitmap BEFORE draining it when save_interval_s (3s) elapses.
# Sleep 4s: past the first 3s save+drain cycle, before the second (at 6s).
exec 7> $MNT/crash_open.dat
for i in $(seq 1 8); do
    dd if=/dev/urandom bs=65536 count=2 >&7 2>/dev/null
done

sleep 4   # > bitmap_interval=3; first timer fires at ~3s, saves bitmap BEFORE drain

bitmap_path=/tmp/lrt/content/liveraid.content.bitmap
[ -f "$bitmap_path" ] \
    && pass "crash recovery: bitmap saved before drain (bitmap_interval=3)" \
    || fail "crash recovery" "bitmap not present after 4s"

# kill -9 while bitmap is on disk (before next 3s cycle overwrites with empty bitmap)
kill -9 $(pidof liveraid) 2>/dev/null || true
exec 7>&- 2>/dev/null || true   # close fd (daemon already dead, ignore errors)
sleep 0.5
fusermount3 -u $MNT 2>/dev/null || true   # detach orphaned FUSE endpoint
sleep 0.3

[ -f "$bitmap_path" ] \
    && pass "crash recovery: bitmap persists after kill -9" \
    || fail "crash recovery" "bitmap gone after kill -9"

# Phase 2: remount with standard config, drain, repair
> /tmp/lrt/liveraid.log
mount_fs
sleep 11   # drain dirty blocks loaded from bitmap

kill -USR2 $(pidof liveraid)
sleep 4
unmount_fs

result=$(grep "repair:" /tmp/lrt/liveraid.log | tail -1 || true)
echo "  $result"
if [ -n "$result" ] && check_clean_parity "$result"; then
    pass "crash recovery: parity clean after remount + drain"
else
    fail "crash recovery" "$result"
fi

# Phase 3: verify pre-crash data is accessible (16 blocks × 64 KiB = 1 MiB)
mount_fs
expected_sz=$((8 * 2 * 65536))   # 8 batches × 2 blocks × 65536 bytes = 1048576
sz=$(stat -c '%s' $MNT/crash_open.dat 2>/dev/null || echo 0)
[ "$sz" = "$expected_sz" ] \
    && pass "crash recovery: pre-crash data file intact (size=$sz)" \
    || fail "crash recovery" "crash_open.dat size=$sz (expected $expected_sz)"
unmount_fs

# ===================================================================
echo ""
echo "=== Multiple content paths ==="
wipe_data
mkdir -p /tmp/lrt/content2

cat > /tmp/lrt/liveraid2.conf << 'EOF'
data d1 /tmp/lrt/d1
data d2 /tmp/lrt/d2
data d3 /tmp/lrt/d3
data d4 /tmp/lrt/d4
parity 1 /tmp/lrt/parity1/liveraid.parity
parity 2 /tmp/lrt/parity2/liveraid.parity
content /tmp/lrt/content/liveraid.content
content /tmp/lrt/content2/liveraid.content
mountpoint /tmp/lrt/mount
blocksize 64
placement roundrobin
parity_threads 4
EOF

$BIN -c /tmp/lrt/liveraid2.conf -f $MNT >>/tmp/lrt/liveraid.log 2>&1 &
sleep 1.5
echo "multi-content file" > $MNT/mctest.txt
fusermount3 -u $MNT; sleep 0.3

[ -f /tmp/lrt/content/liveraid.content ]  && pass "multi-content: primary written"  || fail "multi-content" "primary missing"
[ -f /tmp/lrt/content2/liveraid.content ] && pass "multi-content: secondary written" || fail "multi-content" "secondary missing"

rm /tmp/lrt/content/liveraid.content
$BIN -c /tmp/lrt/liveraid2.conf -f $MNT >>/tmp/lrt/liveraid.log 2>&1 &
sleep 1.5
val=$(cat $MNT/mctest.txt)
[ "$val" = "multi-content file" ] \
    && pass "multi-content: loads from secondary when primary missing" \
    || fail "multi-content" "got '$val'"
fusermount3 -u $MNT; sleep 0.3
rm -rf /tmp/lrt/content2

# ===================================================================
echo ""
echo "=== Empty files ==="
wipe_data
mount_fs

touch $MNT/empty.txt
sz=$(stat -c '%s' $MNT/empty.txt)
[ "$sz" = "0" ] && pass "empty file: size=0 after create" || fail "empty file" "size=$sz"
unmount_fs; mount_fs
sz=$(stat -c '%s' $MNT/empty.txt)
[ "$sz" = "0" ] && pass "empty file: size=0 after remount" || fail "empty file" "size=$sz after remount"
unmount_fs

# ===================================================================
echo ""
echo "=== Directory metadata: chmod and utimens ==="
wipe_data
mount_fs

mkdir $MNT/mdtest
chmod 750 $MNT/mdtest
mode=$(stat -c '%a' $MNT/mdtest)
[ "$mode" = "750" ] && pass "dir metadata: mode=750 after chmod" || fail "dir metadata" "mode=$mode"

touch -d "2024-06-01 00:00:00 UTC" $MNT/mdtest
mtime_set=$(stat -c '%Y' $MNT/mdtest)
unmount_fs; mount_fs

mode=$(stat -c '%a' $MNT/mdtest)
[ "$mode" = "750" ] && pass "dir metadata: chmod survives remount" || fail "dir metadata" "mode=$mode after remount"

mtime_got=$(stat -c '%Y' $MNT/mdtest)
[ "$mtime_set" = "$mtime_got" ] \
    && pass "dir metadata: utimens survives remount" \
    || fail "dir metadata" "set=$mtime_set got=$mtime_got"
unmount_fs

# ===================================================================
echo ""
echo "=== Socket: scrub and scrub repair ==="
wipe_data
mount_fs

for i in $(seq 1 5); do printf "scrub data %d" "$i" > $MNT/sd${i}.txt; done
sleep 11  # drain parity

ctrl=/tmp/lrt/content/liveraid.content.ctrl
scrub_result=$(python3 -c "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('$ctrl')
s.sendall(b'scrub\n')
data = b''
while True:
    chunk = s.recv(4096)
    if not chunk: break
    data += chunk
print(data.decode().strip())
")
echo "  scrub: $scrub_result"
echo "$scrub_result" | grep -qP "^done \d+ 0 errors=0$" \
    && pass "socket scrub: 0 mismatches" \
    || fail "socket scrub" "$scrub_result"

repair_result=$(python3 -c "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('$ctrl')
s.sendall(b'scrub repair\n')
data = b''
while True:
    chunk = s.recv(4096)
    if not chunk: break
    data += chunk
print(data.decode().strip())
")
echo "  scrub repair: $repair_result"
echo "$repair_result" | grep -qP "^done \d+ 0 fixed=0 errors=0$" \
    && pass "socket scrub repair: 0 mismatches, 0 fixed" \
    || fail "socket scrub repair" "$repair_result"
unmount_fs

# ===================================================================
echo ""
echo "=== Position reuse after delete ==="
wipe_data
# Single-drive config so both files share the same per-drive position allocator
cat > /tmp/lrt/single.conf << 'CONFEOF'
data d1 /tmp/lrt/d1
parity 1 /tmp/lrt/parity1/liveraid.parity
content /tmp/lrt/content/liveraid.content
mountpoint /tmp/lrt/mount
blocksize 64
placement roundrobin
parity_threads 1
CONFEOF

$BIN -c /tmp/lrt/single.conf -f $MNT >>/tmp/lrt/liveraid.log 2>&1 &
sleep 1.5
if ! grep -q lrt /proc/mounts; then
    echo "ERROR: mount failed"; tail -5 /tmp/lrt/liveraid.log; exit 1
fi

printf "x" > $MNT/pos_a.txt   # allocates 1 block at position 0 on d1
rm $MNT/pos_a.txt              # frees position 0 back to d1 allocator (first-fit)
printf "x" > $MNT/pos_b.txt   # should reuse position 0
unmount_fs

pos=$(grep "|/pos_b.txt|" /tmp/lrt/content/liveraid.content | cut -d'|' -f5)
echo "  pos_b.txt at parity position $pos"
[ "$pos" = "0" ] \
    && pass "position reuse: freed position reused by next allocation" \
    || fail "position reuse" "pos=$pos (expected 0)"

# ===================================================================
echo ""
echo "=== chown: uid/gid on files and dirs ==="
wipe_data
# Single-drive config (keeps it simple)
cat > /tmp/lrt/chown_test.conf << 'CONFEOF'
data d1 /tmp/lrt/d1
parity 1 /tmp/lrt/parity1/liveraid.parity
content /tmp/lrt/content/liveraid.content
mountpoint /tmp/lrt/mount
blocksize 64
placement roundrobin
parity_threads 1
CONFEOF

$BIN -c /tmp/lrt/chown_test.conf -f $MNT >>/tmp/lrt/liveraid.log 2>&1 &
sleep 1.5
if ! grep -q lrt /proc/mounts; then
    echo "ERROR: mount failed"; tail -5 /tmp/lrt/liveraid.log; exit 1
fi

myuid=$(id -u)
mygid=$(id -g)
# Use a secondary group if available; otherwise fall back to primary gid
alt_gid=$(id -G | tr ' ' '\n' | grep -v "^${mygid}$" | head -1 || true)
[ -z "$alt_gid" ] && alt_gid=$mygid

echo "chown_file.txt" > $MNT/chown_file.txt
chown ${myuid}:${alt_gid} $MNT/chown_file.txt
got_uid=$(stat -c '%u' $MNT/chown_file.txt)
got_gid=$(stat -c '%g' $MNT/chown_file.txt)
[ "$got_uid" = "$myuid" ] && [ "$got_gid" = "$alt_gid" ] \
    && pass "chown file: uid/gid set immediately" \
    || fail "chown file" "uid=$got_uid gid=$got_gid (expected $myuid:$alt_gid)"

mkdir $MNT/chown_dir
chown ${myuid}:${alt_gid} $MNT/chown_dir
got_uid=$(stat -c '%u' $MNT/chown_dir)
got_gid=$(stat -c '%g' $MNT/chown_dir)
[ "$got_uid" = "$myuid" ] && [ "$got_gid" = "$alt_gid" ] \
    && pass "chown dir: uid/gid set immediately" \
    || fail "chown dir" "uid=$got_uid gid=$got_gid (expected $myuid:$alt_gid)"

# Remount and verify persistence
fusermount3 -u $MNT; sleep 0.3
$BIN -c /tmp/lrt/chown_test.conf -f $MNT >>/tmp/lrt/liveraid.log 2>&1 &
sleep 1.5
if ! grep -q lrt /proc/mounts; then
    echo "ERROR: remount failed"; tail -5 /tmp/lrt/liveraid.log; exit 1
fi

got_uid=$(stat -c '%u' $MNT/chown_file.txt)
got_gid=$(stat -c '%g' $MNT/chown_file.txt)
[ "$got_uid" = "$myuid" ] && [ "$got_gid" = "$alt_gid" ] \
    && pass "chown file: uid/gid persists after remount" \
    || fail "chown file remount" "uid=$got_uid gid=$got_gid (expected $myuid:$alt_gid)"

got_uid=$(stat -c '%u' $MNT/chown_dir)
got_gid=$(stat -c '%g' $MNT/chown_dir)
[ "$got_uid" = "$myuid" ] && [ "$got_gid" = "$alt_gid" ] \
    && pass "chown dir: uid/gid persists after remount" \
    || fail "chown dir remount" "uid=$got_uid gid=$got_gid (expected $myuid:$alt_gid)"

unmount_fs

# ===================================================================
echo ""
echo "=== Symlinks ==="
wipe_data
# Use a simple single-drive, no-parity config
cat > /tmp/lrt/symlink_test.conf << CONFEOF
data d1 /tmp/lrt/d1
content /tmp/lrt/content/liveraid.content
mountpoint /tmp/lrt/mount
blocksize 64
placement roundrobin
parity_threads 1
CONFEOF

$BIN -c /tmp/lrt/symlink_test.conf -f $MNT >>/tmp/lrt/liveraid.log 2>&1 &
sleep 1.5
if ! grep -q lrt /proc/mounts; then
    echo "ERROR: mount failed"; tail -5 /tmp/lrt/liveraid.log; exit 1
fi

# Create a regular file for the symlink to target
echo "symlink_target_content" > $MNT/target.txt

# Create a symlink pointing to a file
ln -s /target.txt $MNT/link_to_file.txt

# Create a directory and a symlink pointing to a directory
mkdir -p $MNT/real_dir
ln -s /real_dir $MNT/link_to_dir

# readlink returns the correct target
val=$(readlink $MNT/link_to_file.txt)
[ "$val" = "/target.txt" ] && pass "symlink: readlink file target" \
                            || fail "symlink: readlink file target" "got '$val'"

val=$(readlink $MNT/link_to_dir)
[ "$val" = "/real_dir" ] && pass "symlink: readlink dir target" \
                          || fail "symlink: readlink dir target" "got '$val'"

# getattr reports S_IFLNK (file type nibble = 0xa)
mode=$(stat -c '%f' $MNT/link_to_file.txt)
[ "$((16#$mode & 16#f000))" = "$((16#a000))" ] \
    && pass "symlink: getattr S_IFLNK" \
    || fail "symlink: getattr S_IFLNK" "mode=$mode"

# readdir lists the symlink
ls $MNT | grep -q "link_to_file.txt" \
    && pass "symlink: readdir lists symlink" \
    || fail "symlink: readdir lists symlink"

# Remount and verify persistence
fusermount3 -u $MNT; sleep 0.3
$BIN -c /tmp/lrt/symlink_test.conf -f $MNT >>/tmp/lrt/liveraid.log 2>&1 &
sleep 1.5
if ! grep -q lrt /proc/mounts; then
    echo "ERROR: remount failed"; tail -5 /tmp/lrt/liveraid.log; exit 1
fi

val=$(readlink $MNT/link_to_file.txt)
[ "$val" = "/target.txt" ] && pass "symlink: readlink persists after remount" \
                            || fail "symlink: readlink persists after remount" "got '$val'"

# unlink removes the symlink but leaves the target intact
rm $MNT/link_to_file.txt
[ ! -L $MNT/link_to_file.txt ] && pass "symlink: unlink removes symlink" \
                                 || fail "symlink: unlink removes symlink"
[ -f $MNT/target.txt ] && pass "symlink: target intact after unlink" \
                         || fail "symlink: target intact after unlink"

unmount_fs

# ===================================================================
echo ""
echo "=== Placement policy smoke tests (mostfree, lfs, pfrd) ==="
for placement in mostfree lfs pfrd; do
    wipe_data
    > /tmp/lrt/liveraid.log
    cat > /tmp/lrt/pl_${placement}.conf << EOF
data d1 /tmp/lrt/d1
data d2 /tmp/lrt/d2
data d3 /tmp/lrt/d3
data d4 /tmp/lrt/d4
parity 1 /tmp/lrt/parity1/liveraid.parity
parity 2 /tmp/lrt/parity2/liveraid.parity
content /tmp/lrt/content/liveraid.content
mountpoint /tmp/lrt/mount
blocksize 64
placement ${placement}
parity_threads 4
EOF

    $BIN -c /tmp/lrt/pl_${placement}.conf -f $MNT >>/tmp/lrt/liveraid.log 2>&1 &
    sleep 1.5
    if ! grep -q lrt /proc/mounts; then
        fail "placement $placement" "mount failed"; continue
    fi

    all_ok=1
    for i in $(seq 1 8); do
        printf "placement %s file %d" "$placement" "$i" > $MNT/pl${i}.txt
        val=$(cat $MNT/pl${i}.txt)
        [ "$val" = "placement $placement file $i" ] || { all_ok=0; break; }
    done
    [ "$all_ok" = "1" ] && pass "placement $placement: 8 files written and read back" \
                         || fail "placement $placement" "file content mismatch"

    sleep 11  # drain parity
    kill -USR2 $(pidof liveraid) 2>/dev/null || true
    sleep 4
    unmount_fs

    result=$(grep "repair:" /tmp/lrt/liveraid.log | tail -1 || true)
    echo "  $placement: $result"
    if [ -n "$result" ] && check_clean_parity "$result"; then
        pass "placement $placement: parity clean"
    else
        fail "placement $placement" "$result"
    fi
done

# ===================================================================
echo ""
echo "========================================"
echo "  $PASS passed,  $FAIL failed"
echo "========================================"
[ "$FAIL" = "0" ]
