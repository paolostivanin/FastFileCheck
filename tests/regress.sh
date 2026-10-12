#!/bin/bash
# regression tests for the independent review's findings
source "$(dirname "$0")/lib.sh"
N=rg; D=$T/tree-$N; DB=$T/db-$N
pass=0; fail=0
ok () { if [ "$2" == "$3" ]; then pass=$((pass+1)); echo "  ok   $1 ($2)"; else fail=$((fail+1)); echo "  FAIL $1: got '$2', want '$3'"; fi; }
fresh () { rm -rf $D $DB; mkdir -p $D/a; for i in $(seq 1 20); do head -c 300 /dev/urandom > $D/a/f$i; done; mkconf $N; }
errs () { sed 's/\x1b\[[0-9;]*[mK]//g' $T/err-$N.txt; }
for ST in ssd hdd; do
echo "################ storage=$ST"; export STORAGE=$ST

echo "== 1. over-long path costs only itself, not the batch"
fresh; long=$(python3 -c "print('/'.join(['d'*50]*10))"); mkdir -p "$D/$long"; for i in 1 2 3; do echo x > "$D/$long/file$i"; done
ok "add exits 2 (3 untrackable files)" $(run $N add) 2
ok "the 20 normal files are all in the DB" $($BIN -c $T/$N.conf stats 2>/dev/null | grep -o "entries:  [0-9]*" | grep -o "[0-9]*$") 20
ok "3 errors counted" "$(grep -o 'Errors: [0-9]*' $T/out-$N.txt)" "Errors: 3"
errs | grep -q "database key limit" && ok "clear message" yes yes || ok "clear message" no yes
ok "summary does not claim success" "$(grep -c 'WITH ERRORS' $T/out-$N.txt)" 1

echo "== 2. tracked file replaced by symlink / directory is reported"
fresh; run $N add >/dev/null
\rm -f $D/a/f1; ln -s /etc/hostname $D/a/f1
\rm -f $D/a/f2; mkdir $D/a/f2
ok "check exits 1" $(run $N check) 1
ok "both reported missing from the file system" "$(grep -c -E 'a/f(1|2):' $T/out-$N.txt)" 2
ok "update removes them from the DB" $(run $N update) 0
ok "check is clean afterwards" $(run $N check) 0

echo "== 3. file vanishing between stat and open is reported as missing"
fresh; run $N add >/dev/null
FFC_SHIM_MODE=vanish FFC_SHIM_TARGET=/a/f7 LD_PRELOAD="${PRELOAD_PREFIX:-} $HELPERS/shim.so" $BIN -c $T/$N.conf --full check >$T/out-$N.txt 2>$T/err-$N.txt; rc=$?
ok "check --full exits 1" $rc 1
ok "f7 listed as missing" "$(grep -c 'a/f7:' $T/out-$N.txt)" 1

echo "== 4. file modified after the scan is a modification, not 'silent corruption'"
fresh; run $N add >/dev/null
FFC_SHIM_MODE=append FFC_SHIM_TARGET=/a/f9 LD_PRELOAD="${PRELOAD_PREFIX:-} $HELPERS/shim.so" $BIN -c $T/$N.conf --full update >$T/out-$N.txt 2>$T/err-$N.txt; rc=$?
ok "update exits 0" $rc 0
ok "no corruption warning" "$(errs | grep -c 'silent corruption')" 0
ok "next check is clean (entry was refreshed)" $(run $N check) 0

echo "== 5. scrub timestamp is the START of the run (no drift by run duration)"
fresh; run $N add >/dev/null
$HELPERS/poke meta_back $DB 100            # force a measurable previous timestamp
t0=$(date +%s.%N); $BIN -c $T/$N.conf -V check >/dev/null 2>&1; t1=$(date +%s.%N)
last=$($HELPERS/poke lastrun $DB)
python3 -I -c "
t0,t1,last=$t0,$t1,$last
dur=t1-t0
print('  run lasted %.2fs, stored last_run - start = %+.3fs' % (dur, last-t0))
import sys; sys.exit(0 if abs(last-t0) < 1.0 and dur > 1.5 else 1)" && { pass=$((pass+1)); echo "  ok   last_run == run start"; } || { fail=$((fail+1)); echo "  FAIL last_run is not the run start"; }

echo "== 6. a path swapped for a FIFO cannot hang the run"
fresh; run $N add >/dev/null
FFC_SHIM_MODE=fifo FFC_SHIM_TARGET=/a/f3 LD_PRELOAD="${PRELOAD_PREFIX:-} $HELPERS/shim.so" timeout 30 $BIN -c $T/$N.conf --full check >$T/out-$N.txt 2>$T/err-$N.txt; rc=$?
ok "finishes (no hang), exit 2" $rc 2

echo "== 7. update never purges the entries below a root that is empty/unreadable (unmounted disk)"
rm -rf $T/ga $T/gb $T/db-gd; mkdir -p $T/ga $T/gb
for i in $(seq 1 20); do head -c 200 /dev/urandom > $T/ga/f$i; head -c 200 /dev/urandom > $T/gb/g$i; done
DIRS="$T/ga,$T/gb" mkconf gd; rm -rf $T/db-gd
ok "add both roots" $(run gd add) 0
entries () { $BIN -c $T/gd.conf stats 2>/dev/null | grep -o "entries:  [0-9]*" | grep -o "[0-9]*$"; }
ok "40 entries" $(entries) 40
\rm -f $T/ga/f1; mv $T/gb $T/gb.away; mkdir $T/gb            # B: mount point present but empty
ok "update exits 2 (error), not a silent purge" $(run gd update) 2
ok "A's deleted file purged, all 20 of B's kept" $(entries) 39
sed 's/\x1b\[[0-9;]*[mK]//g' $T/err-gd.txt | grep -q "20 entries below it" && ok "message names the root and the count" yes yes || ok "message names the root and the count" no yes
ok "check still reports B as missing (it is a read-only report)" $(run gd check) 1
rmdir $T/gb
ok "root missing entirely: update exits 2" $(run gd update) 2
ok "...and still keeps B's entries" $(entries) 39
echo "-- escape hatch: drop the root from the configuration"
DIRS="$T/ga" mkconf gd
ok "update without B exits 0" $(run gd update) 0
ok "B's entries are purged now" $(entries) 19
echo "-- a root that was always empty is not an error"
mkdir -p $T/gc; DIRS="$T/ga,$T/gc" mkconf gd
ok "update with an empty, never-tracked root" $(run gd update) 0
mv $T/gb.away $T/gb

if [ -n "${FFC_SANITIZER:-}" ]; then
  echo "(skipping section 8: the sanitizer runtime intercepts read()/mmap() ahead of the LD_PRELOAD shim)"
else
echo "== 8. a file truncated while it is being hashed must not kill the process (mmap used to SIGBUS)"
rm -rf $D $DB; mkdir -p $D/a; for i in 1 2 3; do head -c 300000 /dev/urandom > $D/a/f$i; done; mkconf $N; run $N add >/dev/null
FFC_SHIM_TARGET=/a/f2 LD_PRELOAD="${PRELOAD_PREFIX:-} $HELPERS/shim_trunc.so" $BIN -c $T/$N.conf --full check >$T/out-$N.txt 2>$T/err-$N.txt; rc=$?
ok "the truncation really happened" "$(stat -c %s $D/a/f2)" 10
if [ $rc -eq 0 ] || [ $rc -eq 1 ]; then ok "survives (exit 0 or 1, not a signal)" ok ok; else ok "survives (exit 0 or 1, not a signal)" "exit $rc" ok; fi
ok "the next run reports the truncated file" "$(run $N check)/$(grep -c 'a/f2:' $T/out-$N.txt)" "1/1"
fi

echo "== 9. a database without a format record (older build) is refused, not silently reused"
fresh; mkdir -p $DB; $HELPERS/poke seed $DB
for cmd in check stats add; do ok "$cmd refuses it (exit 2)" $(run $N $cmd) 2; done
sed 's/\x1b\[[0-9;]*[mK]//g' $T/err-$N.txt | grep -q "incompatible" && ok "message explains what to do" yes yes || ok "message explains what to do" no yes
done
echo; echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
