# Notes for AI assistants working on PS4Mount PS4

Read `AI-WORKFLOW.md` first: its section 2 (rules that are not negotiable) and section 6 (how to make a
change) apply to every change. This file adds how tests, specs and logs fit together.

## Before every commit

```sh
bash test/run.sh                  # host tests (about 40 s); SANITIZE=1 for pkg, upload, panel or log changes
bash test/policy.sh               # rules the sources must keep (read-only system databases, no commands...)
bash test/sdkcheck.sh             # compiles every src/*.c against the real PS4 SDK headers (needs network once)
(cd test/web && npm test)         # the panel in simulated browsers
```

`test/sdkcheck.sh` matters: `src/main.c` cannot be built by `test/run.sh` (it needs the SDK's headers), so
without it a change to `main.c` is only compiled by CI. It does not run the payload: only a console does.

## Tests and specs

- Each area's intent lives in `specs/<area>.md` (storage, classics, emulators, logging). Read the one for the area
  before changing it. A rule has an ID (`STORE-4`), a `Check:` level and a `Source:`.
- After a change, check every rule your diff touches. For `Check: read` rules, say in your summary that
  you re-checked them. For `Check: test` rules, find the test with `grep -rn "STORE-4" specs test` and make
  sure it runs: `bash test/run.sh` runs all of them; there is no per-rule filter.
- A new test needs a `Check: test` rule that meets at least one of: external boundary (a file format, the
  system, the network), money/auth/data-loss/security, concurrency/time/ordering, large input space
  (parsers, formatters), a real past regression, or a public contract others depend on. Everything else is
  checked by reading. No tests for private helpers, restated constants, or mock call arguments.
- Code that decides to **delete a file** goes through a function a host test can run (see `src/stage.h`),
  with a table of ages and names in `test/unit.c`. A change to those values changes the spec first.
- A bug-fix regression test must fail on the code before the fix. Break the behavior on purpose, run the
  test, see it fail on an assertion, revert.
- Removing or moving code needs no test that it is gone.
- No sleeps, retries or raised timeouts in tests. Await the operation or its signal, and fake the network
  so unexpected requests fail.
- Behavior changes start in the spec: update the rule, then the code.
- If code and spec disagree, stop and ask. Do not edit the spec to match the code.

## Logs

- Every `printf("[tag] ...")` of the payload goes to `/data/ps4mount/log.txt` with a time (`src/logts.c`),
  capped near 2 MiB with the one before it in `log.txt.old`.
- Anything PS4Mount deletes on the console's drive writes a `[clean]` line saying what and why.
- `python3 tools/logcheck.py log.txt` explains a copy of the log in plain words (Spanish by default,
  `--lang en`). A new failure that gets understood becomes a signature there, with a fixture in
  `test/data/logs/` and its expected report (`UPDATE=1 bash test/logcheck_test.sh`, then review the diff).
