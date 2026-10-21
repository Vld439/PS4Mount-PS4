#!/bin/bash
# Builds the fuzz targets with libFuzzer (clang) and runs each for SECONDS (default 60):
#   bash test/fuzz/run.sh [SECONDS] [target...]
# Crashes are kept in build/fuzz/<target>/ (crash-*, leak-*, timeout-*).
set -e
cd "$(dirname "$0")/../.."
SECONDS_EACH=${1:-60}
shift || true
TARGETS=${*:-"sfo pkg origin http rar5"}
B=build/fuzz
mkdir -p "$B"
CC=${CC:-clang}
FLAGS="-g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -Isrc -Itest -I$B"
{
    (cd tools && xxd -i -n k_pdb_template pdb_app_template.bin)
    (cd tools && xxd -i -n k_pdb_patch_template pdb_patch_template.bin)
    (cd tools && xxd -i -n k_pdb_addcont_template pdb_addcont_template.bin)
} | sed 's/^unsigned/static const unsigned/' > "$B/pdb_template.h"
{
    (cd web && xxd -i -n index_html index.html)
    (cd web && xxd -i -n legacy_html legacy.html)
} > "$B/page_data.h"

declare -A SRC=(
    [sfo]="src/pkg.c src/mem.c src/crypto.c"
    [pkg]="src/pkg.c src/mem.c src/lang.c src/check.c src/crypto.c"
    [origin]="src/origin.c"
    [http]="src/web.c src/lang.c src/build.c src/mem.c src/crypto.c test/stubs.c"
    # BearSSL calls through its vtables with generic types, which -fsanitize=function flags
    [rar5]="-DRAR5DEC_FUZZ -fno-sanitize=function -Ithird_party/bearssl/inc -Ithird_party/bearssl/src src/rar5dec.c src/mem.c $(echo third_party/bearssl/src/{hash/*.c,mac/hmac.c,symcipher/aes_*.c,codec/*.c,settings.c})"
)
fail=0
for t in $TARGETS; do
    mkdir -p "$B/$t/corpus"
    cp -n test/fuzz/seeds/$t/* "$B/$t/corpus/" 2>/dev/null || true
    $CC $FLAGS -o "$B/$t/fuzz_$t" "test/fuzz/fuzz_$t.c" ${SRC[$t]}
    echo "== fuzz $t for ${SECONDS_EACH}s"
    if ! (cd "$B/$t" && ASAN_OPTIONS=detect_leaks=0 ./fuzz_$t -max_total_time="$SECONDS_EACH" -timeout=10 \
            -rss_limit_mb=2048 -print_final_stats=1 corpus); then
        echo "::error::fuzz target $t found a problem (see build/fuzz/$t)"
        fail=1
    fi
done
exit $fail
