# Shared helpers for the integration tests (sourced by the test scripts, see tests/run.sh).
#   FFC_BIN       the FastFileCheck binary under test
#   FFC_TEST_DIR  scratch directory (trees, databases, configs, outputs)
#   FFC_HELPERS   directory with the compiled helpers: poke, shim.so, shim_trunc.so
: "${FFC_BIN:?FFC_BIN is not set (use tests/run.sh)}"
: "${FFC_TEST_DIR:?FFC_TEST_DIR is not set (use tests/run.sh)}"
: "${FFC_HELPERS:?FFC_HELPERS is not set (use tests/run.sh)}"
BIN=$FFC_BIN
T=$FFC_TEST_DIR
HELPERS=$FFC_HELPERS

# mkconf NAME: writes $T/NAME.conf. Knobs (environment): DIRS EXCL QUICK STORAGE PREFETCH DB_MB BATCH EXTRA
mkconf () {
  cat > $T/$1.conf <<CONF
[settings]
threads_count = 4
io_threads = 0
storage = ${STORAGE:-auto}
prefetch_mb = ${PREFETCH:-0}
ram_usage_percent = 30
[database]
db_path = $T/db-$1/
db_size_mb = ${DB_MB:-50}
db_batch_size = ${BATCH:-500}
[logging]
log_to_file_enabled = false
[scanning]
max_recursion_depth = 10
directories = ${DIRS:-$T/tree-$1}
exclude_hidden = true
exclude_directories = ${EXCL:-}
exclude_extensions = .tmp
quick_check = ${QUICK:-true}
${EXTRA:-}
CONF
}

# run NAME ARGS...: runs the binary with $T/NAME.conf, prints its exit code
run () {
  $BIN -c $T/$1.conf "${@:2}" > $T/out-$1.txt 2> $T/err-$1.txt; echo $?
}

# Tests that rely on file permissions cannot work as root
is_root () { [ "$(id -u)" -eq 0 ]; }
