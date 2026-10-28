#!/bin/bash
# Builds build/panel-app/IV0000-PSMT00001_00-PS4MOUNTPANEL000.pkg (the panel app) with the
# OpenOrbis toolchain (https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain, v0.5.4): its
# create-fself, create-gp4 and PkgTool.Core, a clang and an ld.lld. PS4Mount downloads it
# from the release (ps4mount-panel.pkg) and installs it.
# Set OO_PS4_TOOLCHAIN to the toolchain root, CC and LD when clang and ld.lld are not in PATH, and
# SSL_LIBS to a folder with OpenSSL 1.1 when the system has only 3 (PkgTool.Core needs it).
set -e
cd "$(dirname "$0")"
O=${OO_PS4_TOOLCHAIN:?set OO_PS4_TOOLCHAIN to the OpenOrbis toolchain}
T=$O/bin/linux
CC=${CC:-clang}
LD=${LD:-ld.lld}
TITLE_ID=PSMT00001 # PANEL_APP_TID in src/main.c
CONTENT_ID=IV0000-${TITLE_ID}_00-PS4MOUNTPANEL000
VER=01.00
W=../build/panel-app
export OO_PS4_TOOLCHAIN=$O DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
rm -rf "$W" && mkdir -p "$W/sce_sys/about" "$W/sce_module"
$CC --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall -Wextra -c -isysroot "$O" -isystem "$O/include" \
    -o "$W/main.o" main.c
$LD "$W/main.o" -o "$W/panel-app.elf" -m elf_x86_64 -pie --script "$O/link.x" --eh-frame-hdr -L"$O/lib" -lc -lkernel \
    -lSceUserService -lSceSystemService -lSceSysmodule -lSceCommonDialog -lSceMsgDialog -lSceWebBrowserDialog "$O/lib/crt1.o"
cd "$W"
"$T/create-fself" -in=panel-app.elf -out=panel-app.oelf --eboot eboot.bin --paid 0x3800000000000011 >/dev/null
cp "$O/samples/hello_world/sce_sys/about/right.sprx" sce_sys/about/
cp "$O/samples/hello_world/sce_module/libc.prx" "$O/samples/hello_world/sce_module/libSceFios2.prx" sce_module/
cp ../../assets/icon0.png sce_sys/
S=sce_sys/param.sfo
"$T/PkgTool.Core" sfo_new $S
sfo() { "$T/PkgTool.Core" sfo_setentry $S "$@" >/dev/null; }
sfo APP_TYPE --type Integer --maxsize 4 --value 1
sfo APP_VER --type Utf8 --maxsize 8 --value $VER
sfo ATTRIBUTE --type Integer --maxsize 4 --value 2 # 2: X confirms in the system dialogs (0: Circle)
sfo CATEGORY --type Utf8 --maxsize 4 --value gd
sfo CONTENT_ID --type Utf8 --maxsize 48 --value $CONTENT_ID
sfo DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
sfo SYSTEM_VER --type Integer --maxsize 4 --value 0
sfo TITLE --type Utf8 --maxsize 128 --value PS4Mount
sfo TITLE_ID --type Utf8 --maxsize 12 --value $TITLE_ID
sfo VERSION --type Utf8 --maxsize 8 --value $VER
"$T/create-gp4" -out pkg.gp4 --content-id=$CONTENT_ID \
    --files "eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_module/libc.prx sce_module/libSceFios2.prx" >/dev/null
LD_LIBRARY_PATH=${SSL_LIBS:+$SSL_LIBS:}$LD_LIBRARY_PATH "$T/PkgTool.Core" pkg_build pkg.gp4 . >/dev/null
ls -l "$CONTENT_ID.pkg"
