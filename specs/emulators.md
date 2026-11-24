# Emulators spec

Governs: `src/emu.h`, `emu_sync` and its helpers in `src/main.c`, and the skips of
`<drive>/PS4Mount/emulators` in `src/catalog.c` and `add_subdirs` (`src/main.c`).
Sources: PS4SX2's README (it reads ISO/BIN/CHD only from `/data/PS4SX2/roms`, and BIOS, memory cards and
covers from folders next to it), user (2026-11-24: "tambien quiero poder usar emuladores", and "no volverme
loco con settings innecesarias").

## Purpose

An emulator app on the console looks for its ROMs inside `/data/<App>/`, which is the console's own
storage. ROMs are gigabytes. PS4Mount shows a folder of a drive there, so the ROMs stay on the drive.
There is no setting: the folder names are the setting.

```
<drive>/PS4Mount/emulators/<App>/<Folder>   is shown as   /data/<App>/<Folder>
e.g. /mnt/usb0/PS4Mount/emulators/PS4SX2/roms  ->  /data/PS4SX2/roms
```

Nothing here is tested on a console: whether an app sees a mount made by the payload is not known until
someone tries it. The log says what was mounted and what was not, and why.

## Rules

### EMU-1: only plain names are mounted
`<App>` and `<Folder>` are 1 to 48 bytes of letters, digits, space, `.`, `-`, `_`; they never start with a dot
(so never `.` or `..`) and never hold a slash. A name from a drive is input from outside, and the result is a
mount over a folder of the console.
Check: test (keep bar 2: security, a name from outside becomes a path). Test: `test/unit.c` › `test_emu`.
Source: this change.

### EMU-2: the folders PS4Mount and the system use are never an app's
`<App>` is never `PS4Mount`, `ps4`, `pkg`, `GoldHEN` or `payloads` (any case).
Check: test (keep bar 2). Test: `test/unit.c` › `test_emu`. Source: this change.

### EMU-3: only an empty folder of the console is covered
`/data/<App>` must already exist and be a real folder (not a link), so an app that was never started is not
given a folder. `/data/<App>/<Folder>` is mounted over when it does not exist (it is made), or exists, is a
folder and is empty. When it already has files, or something else is mounted there, it is left as it is and
the log says so once. Files of the console are never hidden, moved or deleted.
Check: read. Source: this change. `emu_mount_one`.

### EMU-4: a mount whose drive is gone is taken off
Mounts of PS4Mount (a nullfs mount on `/data/...` whose source is under `/mnt/` in a `PS4Mount/emulators/`
folder) are taken off when the source folder is gone or its drive was disconnected, and made again when it
is back. It is done at every scan; repeating it does nothing.
Check: read. Source: this change. `emu_sync`.

### EMU-5: the games searches leave the emulators folder alone
The search for pkgs (`catalog_walk`) and the tidying of the scan folder (`adopt_dir_pkgs`) skip
`<drive>/PS4Mount/emulators`: it has thousands of ROMs and no games, and tidying must never remove an empty
ROM folder as if it were a game folder left empty.
Check: test (keep bar 3: data loss, and a walk that must not be slowed down). Test: `test/unit.c` › `test_emu`
(`emu_is_dir`), `test/catalog_test.c` › `test_skip_dir`. Source: this change.

### EMU-6: everything it does is in the log
Each mount, each mount taken off and each folder not mounted writes an `[emu]` line (the last, once).
`tools/logcheck.py` explains them.
Check: read. Source: AGENTS.md (logs).
