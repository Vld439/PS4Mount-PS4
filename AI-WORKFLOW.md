# Working on PS4Mount PS4 with an AI assistant

This guide is written for an AI assistant (Claude Code, Codex CLI, Gemini CLI or any other with a terminal) and for the person who runs it. Its purpose: connect to a running PS4Mount, watch what it does while a game is added, started or deleted, and fix or improve it with changes that can be tested and shared.

If you are the person: give the assistant this file as the first instruction, together with the prompt in section 8.

## 1. What you need

- A jailbroken PS4 with GoldHEN running PS4Mount (see the README), on the same local network as the computer where the assistant runs. GoldHEN's FTP server (port 2121) helps a lot: logs, state files, pkgs.
- On that computer: a terminal the assistant can use, `curl`, `bash`, a C compiler, `xxd`, Node.js (for the panel tests) and the [ps4-payload-sdk](https://github.com/ps4-payload-dev/sdk) **built from git** at the commit in `SDK_COMMIT` of `.github/workflows/build.yml` or later. The v0.9 release does not start on 13.52.
- A way to send the new ELF: GoldHEN's payloader on port 9090 stays open (`cat build/ps4mount-ps4.elf > /dev/tcp/<ip>/9090`), or Payload Guest with the `.bin` in `/data/payloads/`.

A chat in a web browser cannot reach your console: the assistant must be an agent with a terminal on your network.

## 2. Rules that are not negotiable

1. **Never write to the system databases** (`/system_data/priv/mms/*.db`). ShellCore keeps them open with its own cache and flags a database written from outside as corrupted. PS4Mount only reads them (`mode=ro&nolock=1`); what has to be in them is written by the system (`sceAppInstUtilAppInstallAll` for games, a BGFT install for add-ons). `bash test/policy.sh` enforces it.
2. **The system's delete must never reach a pkg.** Deleting a game made the system follow the `app.pkg` link and empty the user's pkg. The game folders are unmounted while the console's delete screens are open, and PS4Mount deletes the files itself, only `<root>/ps4/<TID>` of a game the user deleted there. A read-only view of the pkgs is no answer: BGFT opens a game's pkg for writing when the game starts, records EROFS in the game task (record `0x68`) and refuses the game. **After any change to how games are mounted or linked, restart the console and start a game before calling it done.**
3. **No kernel patches, no reverse engineering of ShellCore or other system processes.** Use the system's own interfaces (AppInstUtil, BGFT, the klog). The one restart a new game needs comes from BGFT loading its tasks at boot: it is documented, not worked around.
4. **One change per version, and test it on the console before the next.** If something gets worse, go back to the last version that worked.
5. **Run the checks before every commit:** `bash test/run.sh`, `bash test/policy.sh` and `cd test/web && npm test`. `SANITIZE=1 bash test/run.sh` and `bash test/fuzz/run.sh 30 pkg http` for changes to pkg reading, uploads or the panel server.
6. **The payload runs inside a borrowed system process.** Never call `exit()` (it ends that process). Its libc heap is about 3 MB in all: anything large (pkg entries, artwork, request bodies, lists of add-ons) comes from `mem_alloc` (mmap), never `malloc`.
7. **Only your own console, your own network and pkgs of games you own.** Never scan or connect to other people's devices. Never ask for, share or link pkgs.
8. **The panel has no password.** Anyone on the network can use it while PS4Mount runs. Stop it when you are done.

## 3. Connecting

PS4Mount shows a notification with the panel's address (port 8770). The API, with `curl` (requests that write files need the header `X-PS4Mount: 1`):

| Request | Does |
| --- | --- |
| `GET /api/games` | JSON: `lang`, `version`, `games` (`title_id`, `name`, `state`, `version`, `source`, `storage`, `dlc`, …) |
| `GET /api/config` | The settings, with `lang` (`auto`, `en`, `es`) and `ui_lang` (the one in use) |
| `GET /api/log` | The tail of the log |
| `GET /api/uploads` | The state of the pkg being received |
| `POST /api/rescan` | Scan the drives now |
| `POST /api/mount?title_id=CUSAxxxxx` | Add to library |
| `POST /api/unmount?title_id=CUSAxxxxx` | Remove from library (the pkgs stay) |
| `POST /api/restore?title_id=CUSAxxxxx` | Move its pkgs back to where they came from |
| `POST /api/config` | Change settings (form fields: `batch`, `batch_size`, `dlc`, `internal`, `lang`, `restore_batch`, `paths`) |
| `POST /api/check?size=N` | Check the first bytes (up to 16 MB) of a pkg of N bytes: what it is and whether this console can use it |
| `POST /api/upload?dest=<id>&name=<file>` | Send a whole pkg (the body) to `dest` (`usb0`… or `internal`) |
| `POST /api/stop` | Stop PS4Mount |

Files on the console (FTP): `/data/ps4mount/log.txt`, `config.txt`, `<TID>.src` (where each game comes from), `<TID>.removed`, `<TID>.restart`; an empty `/data/ps4mount/stop` stops PS4Mount; an empty `/data/ps4mount/klog_all` copies the whole kernel log into PS4Mount's log, which is how the system's side of every problem in this project was found. The kernel log server (port 3232) takes one reader: PS4Mount is that reader while it runs.

GoldHEN's FTP reports the size of files over 4 GB modulo 4 GiB; the console itself sees the real size.

## 4. Reading the log

| Line | Meaning |
| --- | --- |
| `[main] firmware 13.52 …`, `[main] console language 3` | Start: firmware, registration mode, the console's language (3 and 20 are Spanish) |
| `[scan] moved <pkg> -> <root>/ps4/<TID>/app.pkg` | A pkg was adopted into the library |
| `[scan] <TID>: to register (<why>)` | Why a game is registered again: `not registered`, `no game task naming its pkg`, `its pkgs changed`, `game link`, `update link`, `add-ons` |
| `[reg] <TID>: BGFT game task 0000fXXX written` / `updated` | The game task was written: a restart is needed before it plays |
| `[reg] AppInstallAll=0x00000000: …` | The system registered the title |
| `[dlc] <content id>: installing …`, `installed` | An add-on installed through BGFT |
| `[delete] delete screen open …`, `<TID> deleted on the console`, `… deleted (N bytes)` | A delete on the console and the files it removed |
| `[upload] receiving …`, `received`, `the transfer stopped after …` | A pkg sent from the panel |
| klog `invalid content_url`, `0x80990019`, `APP_VER disagree` | BGFT refused the task's path, has no task (restart needed), or the update's version differs from the registered one |

## 5. How PS4Mount is organised

`src/main.c` (scan, registration, mounts, BGFT game tasks, klog events, deletes, panel actions), `src/install.c` (BGFT installs, for add-ons), `src/check.c` (what a pkg is and whether this console can use it), `src/upload.c` (pkgs sent from the panel), `src/pkg.c` (pkg reader), `src/build.c` (install files, BGFT task records), `src/db.c` (read-only lookups in `app.db`), `src/lang.c` (languages), `src/mem.c` (mapped buffers), `src/web.c` with `web/index.html`, `web/legacy.html` (the PS4's browser: ES5, no `<dialog>`) and `web/lang.js` (the panel's translations).

Facts that cost time to learn:

- The SDK's runtime must know the firmware's kernel offsets: with an older SDK the payload ends before `main` with no trace.
- A game starts only when BGFT has its game task (`/user/bgft/task/<id>/d0.pdb`, `d1.pdb`), and BGFT reads them at boot only. Logging out and finishing a real install do not reload them.
- BGFT accepts `/mnt/usb…` and `/user/data/…` paths in a task, not `/data/…` (`invalid content_url`).
- `/user/app/<TID>` is a nullfs mount of `/system_data/ps4mount/app/<TID>`, whose `app.pkg` links to the pkg. `readlink` fails for payloads here (`0xffffffd1`): links are checked by following them (`stat`).
- The delete screens are `ConfirmContentDeleteScene` / `Parent_ContentDeleteScene` (Options > Delete) and `storage_data#application…` / `Parent_NormalStack` (Settings > Storage). `GameWillDelete(<TID>)` names the game; a delete that works logs nothing after it.
- Add-ons are listed from `addcont.db`, which only an install fills: PS4Mount installs add-ons up to 256 MB with BGFT (`sceBgftServiceIntDownloadRegisterTaskByStorageEx`). The content type byte cannot tell a small add-on from one with data.
- GoldHEN runs payloads in ScePartyDaemon, or SceSpZeroConf when that one is gone. A service another payload left open there (BGFT) answers `0x80990001`: close it and open it again.

## 6. Making a change

1. **Reproduce it with the log**: turn on `klog_all`, do the thing on the console, read both PS4Mount's lines and the system's.
2. **Find the system's own behaviour first** (a normal install, the console's own files and records) and copy it, instead of guessing.
3. **Write the change and a test** in `test/` that fails without it (`test/unit.c`, `test/upload_test.c`, `test/http_test.c`, `test/web/panel.test.js`). Texts for people go through `TR("English", "Español")` in C and `t('English')` in the pages, with the translation in the same change.
4. **Run the checks** (rule 5), build, stop PS4Mount, load the new ELF, test on the console, watching the log.
5. **Share it** (section 7).

## 7. Sharing the result

Open an [issue](https://github.com/Vld439/PS4Mount-PS4/issues) or a pull request with:

- the console firmware, the jailbreak, the PS4Mount version and the game (`CUSAxxxxx`);
- what did not work, and the relevant log lines (reviewed first: no account names or anything private);
- what you changed and why, and how you tested it (the checks and the real console);
- if an AI helped you, say so, and let it write the explanation of the system's behaviour and of the change.

PS4Mount PS4 is a port of [PS4Mount](https://github.com/gasb07/ps4mount) by gasb07, under GPL-3.0-or-later: keep the notices, distribute changes under the same license, and please mention the original project.

## 8. Prompt to start with

Copy this to the assistant:

> You are going to help me with PS4Mount PS4. Read `AI-WORKFLOW.md` and `README.md` in this repository completely before doing anything, and follow the rules of section 2. My PS4 is at `<ip>` with GoldHEN (FTP on 2121, payloader on 9090) and runs PS4Mount (its version is at the top of the panel). The problem: `<what happens>`. Start by reading `/data/ps4mount/log.txt` and `GET /api/games`; ask me before turning on `klog_all` or doing anything on the console. Explain what happens, propose one change, test it with the checks, build the ELF and tell me how to load it. At the end, write the explanation I will post in an issue, saying that an AI helped.
