#!/bin/bash
# Integration tests for FastFileCheck. No root needed.
#
#   tests/run.sh PATH/TO/FastFileCheck
#
# Needs: bash, python3, a C compiler, liblmdb headers (for the helpers), and strace (the scrub test
# counts which files were opened; it is skipped if strace is missing).
#
# Environment:
#   FFC_SANITIZER=asan|tsan  the binary is a sanitizer build: set the runtime options, and skip the one
#                            test that needs a read()/mmap() LD_PRELOAD shim the sanitizer would bypass
#   FFC_TEST_KEEP=1          keep the scratch directory (trees, databases, outputs) for inspection
set -u

BIN=$(readlink -f "${1:?usage: $0 PATH/TO/FastFileCheck}")
HERE=$(cd "$(dirname "$0")" && pwd)
[ -x "$BIN" ] || { echo "not executable: $BIN" >&2; exit 2; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/ffc-tests.XXXXXX")
[ -n "${FFC_TEST_KEEP:-}" ] && echo "scratch directory kept: $WORK" || trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/helpers" "$WORK/scratch"

# Test helpers: an LMDB poker (simulates bit rot, rewinds the scrub clock, seeds a legacy DB) and two
# LD_PRELOAD race injectors (file vanishes / changes / becomes a FIFO / is truncated while being read)
CC=${CC:-cc}
$CC -O1 -Wall -o "$WORK/helpers/poke" "$HERE/helpers/poke.c" $(pkg-config --cflags --libs lmdb) || exit 2
$CC -shared -fPIC -O1 -o "$WORK/helpers/shim.so" "$HERE/helpers/shim.c" -ldl || exit 2
$CC -shared -fPIC -O1 -o "$WORK/helpers/shim_trunc.so" "$HERE/helpers/shim_trunc.c" -ldl || exit 2

export FFC_BIN="$BIN" FFC_TEST_DIR="$WORK/scratch" FFC_HELPERS="$WORK/helpers"
case "${FFC_SANITIZER:-}" in
    asan)
        export FFC_SANITIZER ASAN_OPTIONS="detect_leaks=1" UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
        # the ASan runtime must come first in the preload list
        export PRELOAD_PREFIX=$($CC -print-file-name=libasan.so) ;;
    tsan)
        # GLib's mutexes are futex based and invisible to TSan: ignore reports that involve GLib itself
        export FFC_SANITIZER TSAN_OPTIONS="suppressions=$HERE/tsan.supp:halt_on_error=0:exitcode=66" ;;
esac

failed=0
suite () {   # suite LABEL COMMAND...
    local label=$1; shift
    echo "=== $label"
    if "$@"; then echo "--- $label: PASS"; else echo "--- $label: FAIL"; failed=$((failed + 1)); fi
    echo
}

for storage in ssd hdd; do
    suite "correctness (storage=$storage)" env STORAGE=$storage bash "$HERE/correctness.sh"
done
if command -v strace >/dev/null 2>&1; then
    for storage in ssd hdd; do
        suite "rolling scrub (storage=$storage)" env STORAGE=$storage bash "$HERE/scrub.sh"
    done
else
    echo "=== rolling scrub: SKIPPED (strace not installed)"; echo
fi
suite "modes, symlinks, depth, look-ahead gating" bash "$HERE/modes.sh"
suite "regressions (both storage modes)" bash "$HERE/regress.sh"

if [ $failed -eq 0 ]; then echo "ALL TEST SUITES PASSED"; else echo "$failed TEST SUITE(S) FAILED"; fi
[ $failed -eq 0 ]
