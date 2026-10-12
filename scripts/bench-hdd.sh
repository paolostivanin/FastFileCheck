#!/bin/bash
# Cold-cache A/B benchmark of FastFileCheck's HDD mode against the unordered/parallel baseline.
#
#   scripts/bench-hdd.sh DIRECTORY [path/to/FastFileCheck]
#
# For each mode (storage = ssd is the baseline: workers read in parallel in scan order;
# storage = hdd is the ordered, prefetched pipeline) it times three cold-cache runs on DIRECTORY:
#   add          reads every file (cold) and writes the database
#   check        quick check, no file content read: measures the metadata walk (inode order)
#   check --full reads every file against the database: measures the data sweep (disk order)
# Caches are dropped before each run, which needs root (sudo will ask). The database and the
# config live in a temporary directory; DIRECTORY is only read. Put the temp dir on another
# disk with TMPDIR=/path if /tmp is on the disk under test.
# (FFC_BENCH_DROP overrides the cache-drop command, e.g. FFC_BENCH_DROP=true to test the script.)
set -euo pipefail

DIR=${1:?usage: $0 DIRECTORY [path/to/FastFileCheck]}
BIN=${2:-$(dirname "$0")/../build/FastFileCheck}
[ -d "$DIR" ] || { echo "not a directory: $DIR" >&2; exit 2; }
[ -x "$BIN" ] || { echo "binary not found: $BIN (build it first, or pass the path)" >&2; exit 2; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/ffc-bench.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
DROP=${FFC_BENCH_DROP:-sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'}
[ -n "${FFC_BENCH_DROP:-}" ] || sudo -v

BYTES=$(du -sb "$DIR" | cut -f1)
FILES=$(find "$DIR" -type f | wc -l)
echo "Benchmarking $DIR: $FILES files, $((BYTES / 1048576)) MiB (temp dir: $WORK)"
printf '%-6s %-14s %10s %12s\n' mode run seconds MiB/s

for MODE in ssd hdd; do
    cat > "$WORK/$MODE.conf" <<CONF
[settings]
storage = $MODE
[database]
db_path = $WORK/db-$MODE/
db_size_mb = 2048
[logging]
log_to_file_enabled = false
[scanning]
directories = $DIR
exclude_hidden = false
exclude_extensions =
max_recursion_depth = 64
quick_check = true
scrub_days = 0
CONF
    mkdir -p "$WORK/db-$MODE"
    # label|arguments (options such as --full come before the command)
    for SPEC in "add|add" "check|check" "check --full|--full check"; do
        LABEL=${SPEC%%|*}
        read -ra ARGS <<< "${SPEC#*|}"
        eval "$DROP"
        START=$(date +%s.%N)
        RC=0
        "$BIN" -c "$WORK/$MODE.conf" "${ARGS[@]}" >/dev/null 2>"$WORK/err.txt" || RC=$?
        END=$(date +%s.%N)
        SECS=$(echo "$END - $START" | bc -l)
        if [ "$LABEL" = "check" ]; then
            RATE="-"   # no data read: this run measures the metadata walk
        else
            RATE=$(echo "$BYTES / 1048576 / $SECS" | bc -l | xargs printf '%.0f')
        fi
        printf '%-6s %-14s %10.1f %12s' "$MODE" "$LABEL" "$SECS" "$RATE"
        if [ "$RC" -ne 0 ]; then
            printf '   <- exit code %s (timing not comparable): %s' "$RC" "$(head -c 200 "$WORK/err.txt" | tr '\n' ' ')"
        fi
        printf '\n'
    done
done
