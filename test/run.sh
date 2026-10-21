#!/bin/bash
# Builds and runs the host tests (no PS5 or SDK needed): bash test/run.sh
#   CC=clang              compiler (default: cc)
#   SANITIZE=1            with AddressSanitizer and UndefinedBehaviorSanitizer
set -e
cd "$(dirname "$0")/.."
B=build/test
mkdir -p "$B"
CC=${CC:-cc}
FLAGS="-O1 -g -Wall -Wextra -I$B -Isrc"
if [ "${SANITIZE:-0}" = 1 ]; then
    FLAGS="$FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
    # clang's -fsanitize=function flags BearSSL's calls through its vtables (generic types)
    if $CC --version 2>/dev/null | grep -q clang; then
        FLAGS="$FLAGS -fno-sanitize=function"
    fi
    export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1
fi

# Embedded data, as build.sh makes it
{
    (cd tools && xxd -i -n k_pdb_template pdb_app_template.bin)
    (cd tools && xxd -i -n k_pdb_patch_template pdb_patch_template.bin)
    (cd tools && xxd -i -n k_pdb_addcont_template pdb_addcont_template.bin)
} | sed 's/^unsigned/static const unsigned/' > "$B/pdb_template.h"  # included by build.c and unit.c
{
    (cd web && xxd -i -n index_html index.html)
    (cd web && xxd -i -n legacy_html legacy.html)
} > "$B/page_data.h"

# SQLite once per compiler and flags (it is large)
SQ="$B/sqlite3-$(echo "$CC $FLAGS" | md5sum | cut -c1-8).o"
[ -f "$SQ" ] || $CC $FLAGS -w -c third_party/sqlite/sqlite3.c -o "$SQ" \
    -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_CACHE_SIZE=-128

# BearSSL once per compiler and flags, as a library
BR="$B/bearssl-$(echo "$CC $FLAGS" | md5sum | cut -c1-8)"
if [ ! -f "$BR/libbearssl.a" ]; then
    mkdir -p "$BR"
    for f in third_party/bearssl/src/*.c third_party/bearssl/src/*/*.c; do
        echo "$CC $FLAGS -w -Ithird_party/bearssl/inc -Ithird_party/bearssl/src -c $f -o $BR/$(echo "${f#third_party/bearssl/src/}" | tr / _).o"
    done | xargs -P "$(nproc)" -I{} sh -c '{}'
    ar rcs "$BR/libbearssl.a" "$BR"/*.o
fi
BRF="-Ithird_party/bearssl/inc"

# libarchive and zlib once per compiler and flags, as build.sh makes them
LA="$B/libarchive-$( (echo "$CC $FLAGS"; cat src/la_alloc.h third_party/libarchive/*.[ch] third_party/zlib/*.[ch]) | md5sum | cut -c1-8)"
if [ ! -f "$LA/libarchive.a" ]; then
    mkdir -p "$LA"
    for f in third_party/libarchive/*.c third_party/zlib/*.c; do
        echo "$CC $FLAGS -w -include src/la_alloc.h -DHAVE_CONFIG_H -Ithird_party/libarchive -Ithird_party/zlib -Ithird_party/bearssl/inc -c $f -o $LA/$(basename "$f" .c).o"
    done | xargs -P "$(nproc)" -I{} sh -c '{}'
    ar rcs "$LA/libarchive.a" "$LA"/*.o
fi
LAF="-Ithird_party/libarchive"

echo "== unit"
$CC $FLAGS -o "$B/unit" test/unit.c src/build.c src/mem.c src/lang.c src/check.c src/crypto.c src/pkg.c src/origin.c
"$B/unit"

echo "== firmware simulation"
$CC $FLAGS -o "$B/fw_sim" test/fw_sim.c src/fw.c
"$B/fw_sim"

echo "== system database"
$CC $FLAGS -Ithird_party/sqlite -DAPP_DB="\"$B/app.db\"" -o "$B/db_test" test/db_test.c src/db.c src/crypto.c "$SQ"
"$B/db_test"

echo "== web panel server"
$CC $FLAGS -Itest -o "$B/http_test" test/http_test.c test/stubs.c src/web.c src/lang.c src/build.c src/mem.c src/crypto.c -lpthread
"$B/http_test"

echo "== pkgs sent from the panel"
$CC $FLAGS -Itest -o "$B/upload_test" test/upload_test.c src/upload.c src/lang.c src/mem.c src/check.c src/pkg.c src/build.c src/crypto.c -lpthread
"$B/upload_test"

echo "== log with times"
$CC $FLAGS -o "$B/log_test" test/log_test.c src/logts.c -lpthread
"$B/log_test"

echo "== Google Drive backups"
$CC $FLAGS $BRF -o "$B/drive_test" test/drive_test.c src/drive.c src/https.c src/json.c src/lang.c src/build.c src/mem.c src/crypto.c "$BR/libbearssl.a" -lpthread
"$B/drive_test"

echo "== downloads from links"
$CC $FLAGS $BRF $LAF -Itest -o "$B/links_test" test/links_test.c src/links.c src/rar5dec.c src/https.c src/json.c src/lang.c src/build.c src/mem.c \
    src/crypto.c src/check.c src/pkg.c src/la_alloc.c "$BR/libbearssl.a" "$LA/libarchive.a" -lpthread
"$B/links_test"
