# Internal storage hygiene spec

Governs: `src/stage.h`, and in `src/main.c` the functions that receive, install and clean up pkgs
sent from the panel (`install_received`, `install_cleanup`) and that remove a game's own folders
(`clean_holds`).
Sources: user (2026-11-24: "optimize internal storage", "do not break my console"),
README (external storage first), AI-WORKFLOW.md rules 2 and 5.

## Purpose

The console's own drive is small and shared with the system. PS4Mount puts a few files there:
copies of pkgs sent with "Send and install", its records for each game, its log. This area says
when PS4Mount may delete such a file. A wrong deletion costs a game or an upload; a missing one
costs gigabytes, so every deletion is bounded, logged, and limited to PS4Mount's own files.

## Rules

### STORE-1: only PS4Mount's own copies are deleted
PS4Mount deletes files in its own folders (`/data/ps4mount`, `/system_data/ps4mount`) and records it
wrote for a game. It never deletes a pkg the user put on a drive, nor anything the system installed
(`/user/app`, `/user/patch`, `/user/addcont`).
Check: read. Source: AI-WORKFLOW.md rule 2.

### STORE-2: a received pkg waits at least 10 minutes before it is looked at
Before that age it is left alone. At 10 minutes the system's side is checked (STORE-3); the file
is deleted only if that check passes.
Check: test (keep bar 2: data deletion). Test: `test/unit.c` › `test_stage`.
Source: user (2026-11-24); the 10 minutes are the value of the version before this PR.

| Age of `game.pkg` | Action |
|---|---|
| 0 s, 599 s | keep |
| 600 s to 3 days minus 1 s | verify (STORE-3) |
| 3 days | delete (STORE-4) |
| -5 s (the clock went back) | keep |

### STORE-3: a received pkg is deleted once the system has the whole app
The title is installed on the internal drive, registered in the system's database, and the system's
`app.pkg` has the same size as the copy.
Check: read. Source: version before this PR (unchanged by it).

### STORE-4: a pkg the system never installed is deleted after 3 days
Whatever else is true. A paused install of a big game can wait days; 3 days is the limit.
Check: test (keep bar 2). Test: `test/unit.c` › `test_stage`. Source: user (2026-11-24), value chosen in this PR.

### STORE-5: an unfinished transfer is deleted after 2 hours, and never while one runs
`<name>.pkg.part` belongs to the transfer in progress.
Check: test (keep bar 2). Test: `test/unit.c` › `test_stage`. Source: user (2026-11-24), value chosen in this PR.

### STORE-6: anything else in the install folder is left alone
A file that is neither `*.pkg` nor `*.part` is not PS4Mount's to delete.
Check: test (keep bar 2). Test: `test/unit.c` › `test_stage`. Source: AI-WORKFLOW.md rule 2.

### STORE-7: a pkg the system's installer refuses is deleted at once
The copy is PS4Mount's (the user still has the original); leaving it would hold up to a game's size
for days.
Check: read. Source: user (2026-11-24: reclaim internal storage).

### STORE-8: every deletion above writes one `[clean]` line to the log
The line says which file and why; for files, how many bytes. This is what lets a person see, in the
log, what PS4Mount removed.
Check: read; the analyzer (LOG-5) reports them. Source: user (2026-11-24: "good logs").

### STORE-9: a removed game's PlayGo records are deleted only if the game is neither installed nor registered
`/system_data/playgo/<TID>` is also written by the system for real installs; PS4Mount removes it only
when the title is not installed on the internal drive and the system's database does not list it.
Check: read (it needs the console; `test/sdkcheck.sh` proves it compiles). Source: AI-WORKFLOW.md rule 2.

### STORE-10: the settings list what PS4Mount keeps on the console's drive, and only measure it
`GET /api/storage` gives the bytes of: pkgs waiting to be installed, the log, PS4Mount's settings and
records, the holds, and, for the games PS4Mount serves, their PlayGo records, icons and sounds, plus the
pkgs in the internal library and the room left. It counts links as links (a pkg on a drive is never
counted or touched) and deletes nothing.
Check: test for the page (`test/web/panel.test.js` › "settings list what PS4Mount keeps"); read for the
measuring in `app_storage_json` (it needs the console; `test/sdkcheck.sh` compiles it).
Source: user (2026-11-24: idea 3, the storage panel). The old page (`/legacy`) does not show it.

### STORE-11: with "light game data" on, a game's heavy appmeta files are not written
The background sound (`snd0.at9`), the DDS copies of the icon and background, and `pic1.png` are skipped;
`icon0.png`, `pic0.png`, `pronunciation.xml`, `shareparam.json` and the change notes are written as always.
Off by default. It applies when a game is registered; files already written stay until the game is registered
again or removed. The setting is `light_meta` in `config.txt`, added last in the config so no other default shifts.
Check: test for the setting in the panel (`test/web/panel.test.js` › "light_meta"); read for `write_meta`.
Source: user (2026-11-24: idea 2). The sizes (3 to 10 MB per game) are the earlier estimate of this audit: unmeasured.

## Non-goals

- Freeing the space of installed games, or choosing where a game is installed.
- Deleting the heavy appmeta files of games already registered (a purge was left out: those files are visible to
  the system; it needs a console to try).
- Deleting anything in `/user/app`, `/user/patch` or `/user/addcont`.
- Keeping a refused copy for a retry (see open questions).

## Open questions

- Does the home screen really do without the sound and DDS files for a game PS4Mount serves? The same absence
  is normal for pkgs that lack those entries, but it was not tried on a console. (2026-11-24)
- The 10 minutes, 3 days and 2 hours are choices made in this PR (the first is older). Are they right
  for the people who use it? (2026-11-24)
- STORE-7 deletes a copy after one refusal. If BGFT can refuse for a passing reason (a busy service),
  the user must send the pkg again. Keep it for a retry instead? (2026-11-24)
- On removing a game, its `<TID>.restart` marker is kept on purpose (the panel's restart notice reads
  it). Does it ever need to go? (2026-11-24)
