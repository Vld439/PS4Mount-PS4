#!/bin/bash
# Builds build/ps4mount-ps4.elf (and the same file as .bin) with the PS4 Payload SDK
# (https://github.com/ps4-payload-dev/sdk): a build with the 13.52 offsets (the v0.9 release
# stops before main on 13.52; see SDK_COMMIT in .github/workflows/build.yml).
# Set PS4_PAYLOAD_SDK when the SDK is not in /opt/ps4-payload-sdk.
set -e
cd "$(dirname "$0")"
B=${PS4_PAYLOAD_SDK:-/opt/ps4-payload-sdk}/bin
mkdir -p build
# Embedded data: panel page, Media tab icon, playgo record templates.
{
    (cd web && xxd -i -n index_html index.html)
    (cd web && xxd -i -n legacy_html legacy.html)
} > build/page_data.h
(cd assets && xxd -i -n icon0_png icon0.png) > build/icon_data.h
{
    (cd tools && xxd -i -n k_pdb_template pdb_app_template.bin)
    (cd tools && xxd -i -n k_pdb_patch_template pdb_patch_template.bin)
    (cd tools && xxd -i -n k_pdb_addcont_template pdb_addcont_template.bin)
} > build/pdb_template.h
[ -f build/sqlite3.o ] || $B/orbis-clang -O2 -w -c third_party/sqlite/sqlite3.c -o build/sqlite3.o \
    -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0
$B/orbis-clang -O2 -Wall -Wextra -Ibuild -Ithird_party/sqlite -o build/ps4mount-ps4.elf \
    src/main.c src/fw.c src/logts.c src/web.c src/pkg.c src/build.c src/check.c src/upload.c src/db.c src/origin.c src/crypto.c build/sqlite3.o \
    -lSceUserService -lSceAppInstUtil -lSceSystemService -lkernel_sys
# Payload Guest only lists .bin payloads (and hands them to GoldHEN's BinLoader,
# which takes the ELF as it is).
cp build/ps4mount-ps4.elf build/ps4mount-ps4.bin
ls -la build/ps4mount-ps4.elf
