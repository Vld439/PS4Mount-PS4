# PS1, PS2 and PSP Classics spec

Governs: `src/pkg.h` (`pkg_cat_is_game`, `is_sony_game_prefix`, `homebrew_small`), and their use in
`src/check.c`, `src/catalog.c` and `src/main.c`.
Sources: psdevwiki.com/ps4/PARAM.SFO (categories), README, user (2026-11-24: "does this work with PS2
games and other ROMs?").

## Purpose

PS4Mount plays a game from its pkg without installing it. This area decides which pkgs count as games
to mount (and which stay "install the usual way"), including the Classics pkgs that Sony and the
community wrap as PS4 apps.

## Rules

### CLS-1: a pkg is a game when its CATEGORY is `gd` or `gdO`
`gp` is an update, `ac` an add-on, everything else (`gdc`, `gda`, `gdd`...) is not a game.
`gdO` is what the wiki lists as "PS2 Classic". The lowercase `gdo` is also accepted: the wiki calls it a
possible spelling of the same value.
Check: test (keep bar 4: a file format). Test: `test/unit.c` › `test_check`; `test/catalog_test.c` ›
`test_classics`. Source: psdevwiki PARAM.SFO.

| CATEGORY | Game? |
|---|---|
| gd, gdO, gdo | yes |
| gp, ac, gdc, gda, gdd | no |

### CLS-2: a small homebrew app is not mounted; a CUSA game always is
Below 1 GB a title whose ID is not an official one is an app: it is installed the usual way. A port that
carries a game's data is 1 GB or more. A `CUSA` game is a game at any size.
Check: test (keep bar 6: public behavior the README documents). Test: `test/unit.c` › `test_check`.
Source: README (homebrew section).

### CLS-3: PS1, PS2 and PSP style IDs below 1 GB count as games
`SLUS`, `SCUS`, `ULUS`, `NPUJ`... prefixes (the list in `is_sony_game_prefix`) are treated like `CUSA`
for CLS-2.
Check: test, Source: current behavior (unconfirmed). Test: `test/unit.c` › `test_check`.

### CLS-4: a catalog saved under older rules is read again
When the rules that decide what is usable change, the saved list of pkgs is discarded and rebuilt.
Check: read. Source: version before this PR.

### CLS-5: the games list says what kind of title each game is, and the panel can filter by it
`GET /api/games` gives each game a `platform`: `ps4` for a `CUSA` ID, `classic` for the other official-style IDs
of CLS-3, `homebrew` for the rest. The panel shows platform chips only when the library has more than one kind.
Check: test (keep bar 6: a field the panel reads). Test: `test/unit.c` › `test_check` (the classification);
`test/web/panel.test.js` › "platform". Source: user (2026-11-24: idea 10, the filter).
The `/legacy` page does not have the chips. Choosing between several updates of a game (the other half of idea 10)
was not done.

## Non-goals

- Running ROMs, ISOs or disc images. PS4Mount mounts `.pkg` files only; an emulator (such as PS4SX2) is a
  separate app that the user installs, and what it loads is up to it.
- Emulating or converting anything.

## Open questions

- Nothing here was tried on a console: does a mounted PS2 Classic pkg start? The rules only say it is
  listed and mounted like any game. (2026-11-24)
- Do PS1/PS2/PSP Classics pkgs really carry `SLUS`, `SCUS`... IDs? The list came from the disc serials those
  consoles use; no source confirms the pkgs do. If they carry `CUSA` IDs, CLS-3 does nothing and is harmless.
  (2026-11-24)
