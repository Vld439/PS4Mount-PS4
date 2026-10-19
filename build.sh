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
# The pages with their texts in other languages (web/lang.js) where they say <!--LANG-->.
mkdir -p build/web
for page in index legacy; do
    awk -v js=web/lang.js '/^<!--LANG-->$/ { print "<script>"; while ((getline l < js) > 0) print l; close(js);
                                              print "</script>"; next } { print }' web/$page.html > build/web/$page.html
done
{
    (cd build/web && xxd -i -n index_html index.html)
    (cd build/web && xxd -i -n legacy_html legacy.html)
} > build/page_data.h
(cd assets && xxd -i -n icon0_png icon0.png) > build/icon_data.h
{
    (cd tools && xxd -i -n k_pdb_template pdb_app_template.bin)
    (cd tools && xxd -i -n k_pdb_patch_template pdb_patch_template.bin)
    (cd tools && xxd -i -n k_pdb_addcont_template pdb_addcont_template.bin)
} > build/pdb_template.h
# A small page cache (128 KB): the process the payload runs in has a libc heap of a few MB.
SQ_FLAGS="-DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_CACHE_SIZE=-128"
SQ="build/sqlite3-$(echo "$SQ_FLAGS" | md5sum | cut -c1-8).o"
[ -f "$SQ" ] || $B/orbis-clang -O2 -w -c third_party/sqlite/sqlite3.c -o "$SQ" $SQ_FLAGS
$B/orbis-clang -O2 -Wall -Wextra -Ibuild -Ithird_party/sqlite -o build/ps4mount-ps4.elf \
    src/main.c src/fw.c src/logts.c src/web.c src/pkg.c src/build.c src/check.c src/upload.c src/install.c src/lang.c src/mem.c src/db.c src/origin.c src/crypto.c "$SQ" \
    -lSceUserService -lSceAppInstUtil -lSceSystemService -lkernel_sys
# Payload Guest only lists .bin payloads (and hands them to GoldHEN's BinLoader,
# which takes the ELF as it is).
cp build/ps4mount-ps4.elf build/ps4mount-ps4.bin
ls -la build/ps4mount-ps4.elf
