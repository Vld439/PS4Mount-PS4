# Log and log analyzer spec

Governs: `src/logts.c`, `src/logts.h`, `tools/logcheck.py`, `test/data/logs/`.
Sources: AI-WORKFLOW.md sections 4 and 5, user (2026-11-24: "a good testing and logs system").

## Purpose

Everything PS4Mount does is a line in `/data/ps4mount/log.txt`, and the system's own reasons come
through `[klog]` lines. Whoever fixes a problem (usually from a copy of that file) reads it first.
The analyzer turns a copy of it into plain words for someone who does not know the PS4.

## Rules

### LOG-1: every line starts with the console's local date and time
`YYYY-MM-DD HH:MM:SS.mmm `, also for lines written in pieces.
Check: test (keep bar 6: the format people and the analyzer read). Test: `test/log_test.c` (lines, time).
Source: AI-WORKFLOW.md section 4.

### LOG-2: nothing is lost at the end
Stopping PS4Mount writes what is still pending.
Check: test (keep bar 2: the only record of a failure). Test: `test/log_test.c` (`last`).
Source: user (2026-11-24).

### LOG-3: the file stays near 2 MiB, and the one before it is kept as `log.txt.old`
Past 2 MiB, at the next line, the file moves to `log.txt.old` (replacing an older one) and a new one starts;
the newest line is always in `log.txt`.
When rotation fails, PS4Mount keeps writing to the open log without truncating it.
Check: test (keep bar 2: storage). Test: `test/log_test.c` (log rotation). Source: user (2026-11-24: internal storage).

### LOG-4: the log never keeps PS4Mount from stopping
Stopping waits at most 2 seconds for the log thread.
Check: read. Source: AI-WORKFLOW.md (the payload keeps its ports until it exits).

### LOG-5: the analyzer explains each known failure in plain words and exits 1 on an error
It names what each known signature means (from AI-WORKFLOW.md section 4), counts it, shows its first line,
lists unexplained warnings, and sums up what PS4Mount deleted (`[clean]`). Spanish by default, English with
`--lang en`. It reads a file or the standard input.
Check: test (keep bar 6: the report people act on). Test: `test/logcheck_test.sh`. Source: user (2026-11-24).

### LOG-6: the analyzer only reads
It changes no file and uses no network.
Check: read. Source: AI-WORKFLOW.md rule 8 (nothing private leaves the console's owner).

### LOG-7: no Google credential is ever written to the log
Temporary sign-in codes can be shown on the console and in the active Drive sign-in screen. They are
excluded from the log and the panel's notification history.
Check: test for Drive's notification routing (`test/drive_test.c`), read for the console notification.
Source: AI-WORKFLOW.md rule 8.

## Non-goals

- Log levels or a log server: the tag in brackets is the convention.
- Sending logs anywhere.

## Open questions

- The explanations in the analyzer state what AI-WORKFLOW.md documents. Signatures for failures not yet
  documented are added when one is understood, never guessed. (2026-11-24)
