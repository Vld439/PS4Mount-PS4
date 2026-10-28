#!/bin/bash
# Builds build/ps4mount-ps4.elf and build/ps4mount-ps4.bin (a raw loader + that ELF) with the PS4 Payload SDK
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
# BearSSL (TLS for Google Drive) once, as a library: only what is used is linked.
# For a PlayStation target, clang before 22 declares the AES-NI, PCLMUL and SSSE3
# intrinsics only when those features are on: they are (the PS4's Jaguar has them, and
# BearSSL checks the CPU before using them). The Jaguar has no RDRAND: off.
BR_FLAGS="-O2 -w -maes -mpclmul -mssse3 -msse4.1 -DBR_RDRAND=0"
BR="build/bearssl-$( (echo "$BR_FLAGS"; cat third_party/bearssl/src/*.c third_party/bearssl/src/*/*.c) | md5sum | cut -c1-8)"
if [ ! -f "$BR/libbearssl.a" ]; then
    mkdir -p "$BR"
    for f in third_party/bearssl/src/*.c third_party/bearssl/src/*/*.c; do
        o="$BR/$(echo "${f#third_party/bearssl/src/}" | tr / _).o"
        echo "$B/orbis-clang $BR_FLAGS -Ithird_party/bearssl/inc -Ithird_party/bearssl/src -c $f -o $o"
    done | xargs -P "$(nproc)" -I{} sh -c '{}'
    $B/orbis-ar rcs "$BR/libbearssl.a" "$BR"/*.o
fi
# libarchive (RAR and ZIP of downloads from links) and the zlib it inflates ZIPs with, once,
# as a library. Its L"" texts must be 32-bit like the wchar_t of the SDK's FreeBSD libc
# (the PlayStation target makes them 16-bit), and its memory goes through la_alloc.h.
LA_FLAGS="-O2 -w -fno-short-wchar -include src/la_alloc.h -DHAVE_CONFIG_H"
LA="build/libarchive-$( (echo "$LA_FLAGS"; cat src/la_alloc.h third_party/libarchive/*.[ch] third_party/zlib/*.[ch]) | md5sum | cut -c1-8)"
if [ ! -f "$LA/libarchive.a" ]; then
    mkdir -p "$LA"
    for f in third_party/libarchive/*.c third_party/zlib/*.c; do
        echo "$B/orbis-clang $LA_FLAGS -Ithird_party/libarchive -Ithird_party/zlib -Ithird_party/bearssl/inc -c $f -o $LA/$(basename "$f" .c).o"
    done | xargs -P "$(nproc)" -I{} sh -c '{}'
    $B/orbis-ar rcs "$LA/libarchive.a" "$LA"/*.o
fi
# libnfs (NFS) and libsmb2 (SMB), the clients of a network share, and libfuse (the
# read-only FUSE filesystem that shows it as a folder), once, as libraries. libfuse is
# the port of itemzflow (see third_party/libfuse/NOTICE).
FN_FLAGS="-O2 -w -DHAVE_CONFIG_H -DHAVE_NFS4_2 -D_U_= -include third_party/libnfs/ps4_compat.h"
FN="build/libnfs-$( (echo "$FN_FLAGS"; cat third_party/libnfs/*/*.c third_party/libnfs/config.h third_party/libnfs/ps4_compat.h) | md5sum | cut -c1-8)"
if [ ! -f "$FN/libnfs.a" ]; then
    mkdir -p "$FN"
    for f in third_party/libnfs/{lib,mount,nfs,nfs4,portmap,nlm,nsm,rquota}/*.c; do
        [ "$(basename "$f")" = rdma.c ] && continue # Linux epoll, not built here
        echo "$B/orbis-clang $FN_FLAGS -Ithird_party/libnfs -Ithird_party/libnfs/include -Ithird_party/libnfs/include/nfsc -Ithird_party/libnfs/mount -Ithird_party/libnfs/nfs -Ithird_party/libnfs/nfs4 -Ithird_party/libnfs/portmap -Ithird_party/libnfs/nlm -Ithird_party/libnfs/nsm -Ithird_party/libnfs/rquota -c $f -o $FN/$(basename "$f" .c).o"
    done | xargs -P "$(nproc)" -I{} sh -c '{}'
    $B/orbis-ar rcs "$FN/libnfs.a" "$FN"/*.o
fi
FS_FLAGS="-O2 -w -DHAVE_ARC4RANDOM_BUF -DHAVE_CONFIG_H -DHAVE_DEV_URANDOM -D_FILE_OFFSET_BITS=64 -D_U_="
FS="build/libsmb2-$( (echo "$FS_FLAGS"; cat third_party/libsmb2/lib/*.c third_party/libsmb2/config.h) | md5sum | cut -c1-8)"
if [ ! -f "$FS/libsmb2.a" ]; then
    mkdir -p "$FS/lib"
    for f in third_party/libsmb2/lib/*.c; do
        echo "$B/orbis-clang $FS_FLAGS -Ithird_party/libsmb2 -Ithird_party/libsmb2/include -Ithird_party/libsmb2/include/smb2 -Ithird_party/libsmb2/lib -c $f -o $FS/lib/$(basename "$f" .c).o"
    done | xargs -P "$(nproc)" -I{} sh -c '{}'
    $B/orbis-ar rcs "$FS/libsmb2.a" "$FS"/lib/*.o
fi
FF_FLAGS="-O2 -w -DFUSE_USE_VERSION=26 -D_FILE_OFFSET_BITS=64"
FF="build/libfuse-$( (echo "$FF_FLAGS"; cat third_party/libfuse/source/*.c third_party/libfuse/source/*.h third_party/libfuse/include/*.h third_party/libfuse/include/orbis/*.h) | md5sum | cut -c1-8)"
if [ ! -f "$FF/libfuse.a" ]; then
    mkdir -p "$FF"
    for f in third_party/libfuse/source/*.c; do
        echo "$B/orbis-clang $FF_FLAGS -Ithird_party/libfuse/include -c $f -o $FF/$(basename "$f" .c).o"
    done | xargs -P "$(nproc)" -I{} sh -c '{}'
    $B/orbis-ar rcs "$FF/libfuse.a" "$FF"/*.o
fi
$B/orbis-clang -O2 -Wall -Wextra -DFUSE_USE_VERSION=26 -D_FILE_OFFSET_BITS=64 -Ibuild -Ithird_party/sqlite -Ithird_party/bearssl/inc -Ithird_party/libarchive -Ithird_party/libfuse/include -Ithird_party/libnfs/include -Ithird_party/libnfs/mount -Ithird_party/libsmb2/include -o build/ps4mount-ps4.elf \
    src/main.c src/fw.c src/logts.c src/web.c src/pkg.c src/build.c src/check.c src/upload.c src/install.c src/lang.c src/mem.c src/db.c src/origin.c src/crypto.c \
    src/badpatch.c src/catalog.c src/shortpath.c src/titlecache.c src/drive.c src/https.c src/json.c src/links.c src/rar5dec.c src/la_alloc.c src/netfs.c src/netfs_nfs.c src/netfs_smb.c "$SQ" "$BR/libbearssl.a" "$LA/libarchive.a" "$FN/libnfs.a" "$FS/libsmb2.a" "$FF/libfuse.a" \
    -lSceUserService -lSceAppInstUtil -lSceSystemService -lkernel_sys
# The .bin (what Payload Guest lists) is raw code that loads the ELF appended to it:
# older BinLoaders (GoldHEN 2.4b18) jump to the first byte and crash on an ELF header.
$B/orbis-clang -c -Os -fPIC -ffreestanding -fno-builtin -fno-jump-tables -fno-stack-protector \
    -fno-asynchronous-unwind-tables -mstackrealign -Wall -Wextra -o build/binboot.o tools/binboot/binboot.c
$B/orbis-ld --static -T tools/binboot/binboot.ld -o build/binboot.elf build/binboot.o
$B/llvm-objcopy -O binary --only-section=.text build/binboot.elf build/binboot.bin
cat build/binboot.bin build/ps4mount-ps4.elf > build/ps4mount-ps4.bin
ls -la build/ps4mount-ps4.elf
