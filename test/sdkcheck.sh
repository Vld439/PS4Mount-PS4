#!/bin/bash
# Compiles (without linking or running) every source in src/ against the real PS4 SDK headers,
# on any computer with clang: bash test/sdkcheck.sh
#
# The host tests (test/run.sh) cannot build src/main.c: it needs the SDK's headers. This is
# the check that catches a call to something that does not exist, a wrong type or a warning
# in the code that only the console build sees, without installing the SDK. It does not
# prove the payload works (only the console does), and the build of the ELF (build.sh, run
# by CI on every pull request) remains the final word.
#
#   CLANG=clang     compiler (default: clang)
#   SDK_HEADERS=dir an existing checkout of the SDK (default: cloned into build/sdk-headers)
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
B=build/sdkcheck
mkdir -p "$B/gen"

# The SDK version the real build uses (.github/workflows/build.yml)
COMMIT=$(sed -n 's/^ *SDK_COMMIT: *\([0-9a-f]\{40\}\).*/\1/p' .github/workflows/build.yml | head -n 1)
[ -n "$COMMIT" ] || { echo "sdkcheck: SDK_COMMIT not found in .github/workflows/build.yml"; exit 1; }
SDK=${SDK_HEADERS:-build/sdk-headers}
if [ ! -d "$SDK/include/ps4" ]; then
    echo "sdkcheck: getting the SDK headers ($COMMIT) into $SDK"
    rm -rf "$SDK"
    mkdir -p "$SDK"
    git -C "$SDK" init -q
    git -C "$SDK" fetch -q --depth 1 https://github.com/ps4-payload-dev/sdk.git "$COMMIT"
    git -C "$SDK" checkout -q FETCH_HEAD
fi

# Embedded data, as build.sh makes it (the contents do not matter to a compile)
{
    (cd tools && xxd -i -n k_pdb_template pdb_app_template.bin)
    (cd tools && xxd -i -n k_pdb_patch_template pdb_patch_template.bin)
    (cd tools && xxd -i -n k_pdb_addcont_template pdb_addcont_template.bin)
} > "$B/gen/pdb_template.h"
{
    (cd web && xxd -i -n index_html index.html)
    (cd web && xxd -i -n legacy_html legacy.html)
} > "$B/gen/page_data.h"
(cd assets && xxd -i -n icon0_png icon0.png) > "$B/gen/icon_data.h"

fail=0
n=0
for f in src/*.c; do
    n=$((n + 1))
    # -Werror: a warning on the console build is a mistake here too. The flags are
    # build.sh's, with the target and headers of a FreeBSD 9.3 console.
    if ! "$CLANG" -fsyntax-only -std=gnu11 -Wall -Wextra -Werror \
        -DFUSE_USE_VERSION=26 -D_FILE_OFFSET_BITS=64 -DNETFS_ID='"check"' \
        -target x86_64-unknown-freebsd9.3 -nostdinc \
        -isystem "$SDK/include/freebsd-9.3" -isystem "$SDK/include" \
        -Isrc -I"$B/gen" -Ithird_party/sqlite -Ithird_party/bearssl/inc -Ithird_party/libarchive \
        -Ithird_party/libfuse/include -Ithird_party/libnfs/include -Ithird_party/libnfs/mount \
        -Ithird_party/libsmb2/include "$f"; then
        echo "sdkcheck: FAILED $f"
        fail=1
    fi
done
[ "$fail" = 0 ] && echo "sdkcheck: $n sources compile against the SDK headers"
exit "$fail"
