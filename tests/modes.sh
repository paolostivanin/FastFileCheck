#!/bin/bash
source "$(dirname "$0")/lib.sh"
N=modes; D=$T/tree-$N; DB=$T/db-$N
rm -rf $D $DB; mkdir -p $D/sub/deeper/deepest
pass=0; fail=0
ok () { if [ "$2" == "$3" ]; then pass=$((pass+1)); echo "  ok   $1 ($2)"; else fail=$((fail+1)); echo "  FAIL $1: got $2, want $3"; fi; }
B=$((4*1024*1024))
for sz in 0 1 4095 $((B-1)) $B $((B+1)) $((3*B)) $((10*B+123)); do head -c $sz /dev/urandom > $D/file_$sz.bin; done
head -c 5000 /dev/urandom > $D/sub/a.bin; head -c 5000 /dev/urandom > $D/sub/deeper/b.bin; head -c 5000 /dev/urandom > $D/sub/deeper/deepest/c.bin

echo "== hashes agree between mmap path (ssd) and large-read path (hdd), both directions"
STORAGE=ssd mkconf $N; ok "add (ssd)" $(run $N add) 0
STORAGE=hdd mkconf $N; ok "full check (hdd) against ssd-made DB" $(run $N --full check) 0
rm -rf $DB
STORAGE=hdd mkconf $N; ok "add (hdd)" $(run $N add) 0
STORAGE=ssd mkconf $N; ok "full check (ssd) against hdd-made DB" $(run $N --full check) 0

echo "== tiny look-ahead budget (1 MB) + 16 threads: gating must not deadlock, big files still pass"
for st in hdd; do
  EXTRA= ; STORAGE=$st PREFETCH=1 mkconf $N; sed -i 's/^threads_count = 4/threads_count = 16/' $T/$N.conf
  ok "full check, budget 1MB" $(timeout 120 $BIN -c $T/$N.conf --full check >/dev/null 2>&1; echo $?) 0
  rm -rf $DB; ok "add, budget 1MB" $(timeout 120 $BIN -c $T/$N.conf add >/dev/null 2>&1; echo $?) 0
done

echo "== symlinks are skipped (as documented), a symlinked ROOT is followed"
STORAGE=ssd mkconf $N
ln -s $D/sub $D/link_to_dir; ln -s $D/file_1.bin $D/link_to_file; ln -s /nonexistent $D/dangling
ok "check ignores symlinks" $(run $N check) 0
mkdir -p $T/rootlink-target && head -c 100 /dev/urandom > $T/rootlink-target/r.bin; ln -sfn $T/rootlink-target $T/root-link
DIRS=$T/root-link STORAGE=ssd mkconf rl; rm -rf $T/db-rl
ok "add via symlinked root" $(run rl add) 0
grep -q "processed: 1\|files processed: 1" $T/out-rl.txt && echo "  ok   root symlink followed, 1 file" || { fail=$((fail+1)); echo "  FAIL root symlink: $(grep processed $T/out-rl.txt)"; }
rm -f $D/link_to_dir $D/link_to_file $D/dangling

echo "== overlapping directories + trailing slashes + blanks are normalised"
DIRS="$D/, $D/sub ,, $D" STORAGE=ssd mkconf ov; rm -rf $T/db-ov
ok "add overlapping dirs" $(run ov add) 0
c=$(grep -o "files processed: [0-9]*" $T/out-ov.txt | grep -o "[0-9]*$")
ok "each file once" "$c" 11
ok "stats entries" $($BIN -c $T/ov.conf stats | grep -o "entries:  [0-9]*" | grep -o "[0-9]*$") 11

echo "== depth limit"
sed -i 's/^max_recursion_depth = 10/max_recursion_depth = 1/' $T/$N.conf
rm -rf $DB; ok "add with depth 1" $(run $N add) 0
ok "only depth<=1 files (8 top + sub/a.bin)" $(grep -o "files processed: [0-9]*" $T/out-$N.txt | grep -o "[0-9]*$") 9
echo; echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
