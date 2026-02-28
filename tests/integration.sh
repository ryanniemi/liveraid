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
#   7. Crash recovery: kill -9, content file persists, parity clean after remount
#   8. Multiple content paths: both written on save, secondary used when primary missing
#   9. Empty files: size=0 created and survives remount
#
# All tests use 4 drives + 2 parity levels + parity_threads=4.
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
echo "=== Crash recovery: kill -9 resilience ==="
wipe_data
mount_fs

# Phase 1: write files, drain parity, clean unmount → saves content file
for i in $(seq 1 8); do
    dd if=/dev/urandom of=$MNT/crash${i}.bin bs=65536 count=2 2>/dev/null
done
sleep 11   # two drain cycles
unmount_fs

content_file=/tmp/lrt/content/liveraid.content
[ -f "$content_file" ] \
    && pass "crash recovery: content file saved on clean unmount" \
    || fail "crash recovery" "content file missing after unmount"

# Phase 2: remount, write more data, drain, then kill -9
> /tmp/lrt/liveraid.log
mount_fs
for i in $(seq 1 4); do
    dd if=/dev/urandom of=$MNT/crash_post${i}.bin bs=65536 count=2 2>/dev/null
done
sleep 11   # drain parity for post-mount writes
kill -9 $(pidof liveraid) 2>/dev/null || true
sleep 0.5
fusermount3 -u $MNT 2>/dev/null || true   # detach orphaned FUSE endpoint
sleep 0.3

[ -f "$content_file" ] \
    && pass "crash recovery: content file persists after kill -9" \
    || fail "crash recovery" "content file gone after kill -9"

# Phase 3: remount (loads content from Phase 1 clean unmount), drain, repair
> /tmp/lrt/liveraid.log
mount_fs
sleep 11   # drain any dirty blocks

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

# Phase 4: verify pre-crash data is accessible
mount_fs
all_ok=1
for i in $(seq 1 8); do
    sz=$(stat -c '%s' $MNT/crash${i}.bin 2>/dev/null || echo 0)
    [ "$sz" = "131072" ] || { echo "  crash${i}.bin: size=$sz"; all_ok=0; }
done
[ "$all_ok" = "1" ] && pass "crash recovery: pre-crash data files intact" \
                     || fail "crash recovery" "data file size wrong"
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
echo "========================================"
echo "  $PASS passed,  $FAIL failed"
echo "========================================"
[ "$FAIL" = "0" ]
