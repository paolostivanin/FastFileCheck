#!/bin/bash
source "$(dirname "$0")/lib.sh"
N=scrub; D=$T/tree-$N; DB=$T/db-$N
rm -rf $D $DB; mkdir -p $D
for i in $(seq 1 1000); do mkdir -p $D/d$((i%20)); head -c 300 /dev/urandom > $D/d$((i%20))/f$i.bin; done
mkconf $N
pass=0; fail=0
ok () { if [ "$2" == "$3" ]; then pass=$((pass+1)); echo "  ok   $1 ($2)"; else fail=$((fail+1)); echo "  FAIL $1: got $2, want $3"; fi; }
inrange () { if [ "$2" -ge "$3" ] && [ "$2" -le "$4" ]; then pass=$((pass+1)); echo "  ok   $1 ($2 in $3..$4)"; else fail=$((fail+1)); echo "  FAIL $1: $2 not in $3..$4"; fi; }
# files whose CONTENT was opened during a run (ssd mode: workers open only what they hash)
opened () { strace -f -e trace=openat -o $T/sc.txt $BIN -c $T/$N.conf "$@" >/dev/null 2>&1; grep -o "$D/[^\"]*\.bin" $T/sc.txt | sort -u; }

echo "== setup"; ok add $(run $N add) 0
# a few seconds of elapsed time are ~1e-6 of the ring: 0 files expected, 1 would still be a fluke of the hash
inrange "fresh DB: nothing (or at most a fluke) due, quick check reads no data" $(opened check | wc -l) 0 1

echo "== slice = elapsed/scrub_days, gapless walk over the ring (scrub_days=30)"
$HELPERS/poke meta_back $DB 1296000          # 15 days ago -> half the ring
opened check > $T/s1.txt; n1=$(wc -l < $T/s1.txt); inrange "first half hashed" $n1 430 570
echo "  cursor now $($HELPERS/poke cursor $DB) (want ~0.5)"
inrange "immediate re-run: ~nothing due" $(opened check | wc -l) 0 1   # elapsed ~0s -> slice of ~0 files
$HELPERS/poke meta_back $DB 1296000
opened check > $T/s2.txt; n2=$(wc -l < $T/s2.txt); inrange "second half hashed" $n2 430 570
ok "no overlap between the two slices" $(comm -12 $T/s1.txt $T/s2.txt | wc -l) 0
ok "slices together cover every file" $(cat $T/s1.txt $T/s2.txt | sort -u | wc -l) 1000
echo "  cursor now $($HELPERS/poke cursor $DB) (want ~0.0 after wrapping)"

echo "== older than scrub_days => everything is re-hashed"
$HELPERS/poke meta_back $DB 4000000
ok "all 1000 hashed" $(opened check | wc -l) 1000

echo "== scrub_days=0 disables it"
EXTRA= ; sed -i 's/^quick_check = true/scrub_days = 0\nquick_check = true/' $T/$N.conf
$HELPERS/poke meta_back $DB 99999999
ok "nothing hashed with scrub off" $(opened check | wc -l) 0
sed -i '/^scrub_days = 0$/d' $T/$N.conf

echo "== silent corruption (stored hash != content, metadata unchanged)"
V=$D/d7/f7.bin
$HELPERS/poke hash $DB $V
$HELPERS/poke meta_back $DB 0.001
ok "quick check alone does NOT see it (that is the trade-off)" $(run $N check) 0
ok "--full sees it" $(run $N --full check) 1
grep -q "f7.bin" $T/out-$N.txt && echo "  ok   listed in report" || echo "  FAIL not listed"
$HELPERS/poke meta_back $DB 4000000
ok "scrub (slice covers everything) sees it" $(run $N check) 1
$HELPERS/poke meta_back $DB 4000000
ok "update refuses to bless it: exit 2" $(run $N update) 2
grep -q "Possible silent corruption" $T/err-$N.txt && echo "  ok   warns about silent corruption" || echo "  FAIL no warning"
ok "entry kept: check still flags it" $(run $N --full check) 1
echo; echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
