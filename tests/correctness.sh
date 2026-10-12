#!/bin/bash
source "$(dirname "$0")/lib.sh"
N=correct
rm -rf $T/tree-$N $T/db-$N
mkdir -p $T/tree-$N/{a,b,excl,c/deep}
for i in $(seq 1 20); do head -c $((i*1000)) /dev/urandom > $T/tree-$N/a/f$i.bin; done
for i in $(seq 1 10); do head -c 5000 /dev/urandom > $T/tree-$N/b/g$i.bin; done
head -c 3000 /dev/urandom > $T/tree-$N/excl/x.bin
: > $T/tree-$N/c/empty
head -c 777 /dev/urandom > $T/tree-$N/c/deep/d.bin
echo "scratch" > $T/tree-$N/a/skip.tmp
mkconf $N
pass=0; fail=0
expect () { # expect <label> <got> <want>
  if [ "$2" == "$3" ]; then pass=$((pass+1)); echo "  ok   $1 (exit $2)"; else fail=$((fail+1)); echo "  FAIL $1: exit $2, want $3"; fi
}
has () { grep -q -- "$2" $T/out-$N.txt && { pass=$((pass+1)); echo "  ok   $1"; } || { fail=$((fail+1)); echo "  FAIL $1: '$2' not in output"; }; }
hasnt () { grep -q -- "$2" $T/out-$N.txt && { fail=$((fail+1)); echo "  FAIL $1: '$2' unexpectedly in output"; } || { pass=$((pass+1)); echo "  ok   $1"; }; }

echo "== add / stats / unchanged check"
expect add $(run $N add) 0
expect "stats" $(run $N stats) 0; has "stats counts 33 files (meta excluded)" "Number of entries:  33"
expect "check unchanged" $(run $N check) 0; has "no changes" "No changes detected"
has "processed 33" "Total files processed: 33"
expect "check --full unchanged" $(run $N --full check) 0

echo "== same-size content change with mtime restored (ctime betrays it)"
F=$T/tree-$N/a/f5.bin
M=$(stat -c %y $F)
printf 'ZZZZ' | dd of=$F bs=1 seek=100 conv=notrunc 2>/dev/null
touch -d "$M" $F
expect "check detects hash change under quick_check" $(run $N check) 1; has "hash mismatch listed" "a/f5.bin"
expect "update fixes it" $(run $N update) 0
expect "check clean again" $(run $N check) 0

echo "== touch only / chmod only: no reported change"
touch $T/tree-$N/b/g1.bin; chmod 600 $T/tree-$N/b/g2.bin
expect "check after touch+chmod" $(run $N check) 0
expect "update refreshes the stat info" $(run $N update) 0

echo "== hardlink: link count"
ln $T/tree-$N/b/g3.bin $T/tree-$N/b/g3.link
expect "check with new hardlink" $(run $N check) 1
has "link count changed" "Link count changed"
has "new link missing in db" "g3.link"
rm $T/tree-$N/b/g3.link
expect "check after removing hardlink" $(run $N check) 0

echo "== delete + new file"
rm $T/tree-$N/b/g4.bin
head -c 100 /dev/urandom > $T/tree-$N/b/brand-new.bin
expect "check" $(run $N check) 1
has "deleted file reported" "g4.bin"
has "new file reported" "brand-new.bin"
has "missing from fs wording" "missing from the file system"
expect "update" $(run $N update) 0
expect "check after update" $(run $N check) 0
expect "stats: still 33 files" $(run $N stats) 0; has "33 entries" "Number of entries:  33"

echo "== excluded directory with a DB entry is not 'missing'"
EXCL=$T/tree-$N/excl mkconf $N
expect "check with excl dir" $(run $N check) 0
EXCL= mkconf $N

if ! is_root; then   # permission tests cannot work as root
echo "== unreadable file => exit 2, not abort"
chmod 000 $T/tree-$N/b/g5.bin
r=$(run $N check); expect "check with unreadable file" $r 2
has "error count shown" "Errors: 1"
grep -q "Could not read" $T/err-$N.txt && { pass=$((pass+1)); echo "  ok   error on stderr"; } || { fail=$((fail+1)); echo "  FAIL no stderr message"; }
chmod 644 $T/tree-$N/b/g5.bin

echo "== unreadable directory => exit 2"
mkdir -p $T/tree-$N/locked; echo hi > $T/tree-$N/locked/h; chmod 000 $T/tree-$N/locked
expect "check with locked dir" $(run $N check) 2
chmod 755 $T/tree-$N/locked; rm -rf $T/tree-$N/locked
else echo "(skipping the permission tests: running as root)"; fi

echo "== JSON mode stays valid even with -V"
$BIN -c $T/$N.conf -V -j check > $T/json.txt 2> $T/json.err; rc=$?
python3 -I -c "import json,sys; d=json.load(open('$T/json.txt')); print('  ok   json valid, errors=%d, processed=%d' % (d['errors'], d['total_processed']))" || { fail=$((fail+1)); echo "  FAIL json invalid"; }
expect "json exit" $rc 0

echo "== quick_check=false and --full agree"
QUICK=false mkconf $N
expect "check (quick off)" $(run $N check) 0
QUICK=true mkconf $N

echo; echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
