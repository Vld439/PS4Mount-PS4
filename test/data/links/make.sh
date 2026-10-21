#!/bin/bash
# Makes the archives test/links_test.c downloads (they are kept in the repository: the
# tests' machines have no rar). Needs rar 5 or later and python3; run from the repository
# root after bash test/run.sh has built build/test/links_test:
#   bash test/data/links/make.sh
#   game.partN.rar  RAR5 in volumes of 20 KB: game.pkg (5 MB), update.pkg, readme.txt
#   bundle.zip      the same three, deflated
#   locked.rar      update.pkg with a password (its data encrypted, rar -p)
#   secret.partN.rar game.pkg and update.pkg in volumes of an odd size (a block cut across
#                   two), headers and all encrypted (rar -hp)
#   guess.rar       update.pkg, headers encrypted, with the password guess.site
#   rar4.partN.rar  an older RAR (format 4) in volumes with no pkg inside: libarchive's
#                   test_rar_multivolume_single_file (BSD-2-Clause, libarchive/test)
set -e
D=test/data/links
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
build/test/links_test --make-pkgs "$T"
(cd "$T" && rar a -idq -ma5 -m5 -v20k game.rar game.pkg update.pkg readme.txt &&
    rar a -idq -ma5 -m3 -pps4mount locked.rar update.pkg &&
    rar a -idq -ma5 -m3 -hpps4mount -v20001b secret.rar game.pkg update.pkg &&
    rar a -idq -ma5 -m3 -hpguess.site guess.rar update.pkg)
python3 -I - "$T" <<'PY'
import sys, zipfile
d = sys.argv[1]
with zipfile.ZipFile(d + "/bundle.zip", "w", zipfile.ZIP_DEFLATED) as z:
    for n in ("game.pkg", "update.pkg", "readme.txt"):
        z.write(d + "/" + n, n)
PY
rm -f "$D"/game.part*.rar "$D"/secret.part*.rar
cp "$T"/game.part*.rar "$T"/secret.part*.rar "$T"/locked.rar "$T"/guess.rar "$T"/bundle.zip "$D"/
ls -la "$D"
