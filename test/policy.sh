#!/bin/bash
# Rules the PS4Mount sources must keep, checked on every change: bash test/policy.sh
# Each rule comes from a problem seen on consoles (see the comments).
cd "$(dirname "$0")/.." || exit 1
fail=0
rule() { echo "::error::$1"; fail=1; }
SRC=(src/*.c src/*.h)
OTHER=() # every source but main.c
for f in src/*.c; do [ "$f" = src/main.c ] || OTHER+=("$f"); done

# The system databases are written by the system alone: rows written from outside made
# ShellCore flag app.db as corrupted (0.3.3), and read locks made its writes fail (0.3.4).
grep -nE 'sqlite3_(exec|blob_open|backup_init)\b' "${SRC[@]}" && rule "sqlite3 calls that can write: the system databases are read-only for PS4Mount"
grep -nE 'SQLITE_OPEN_(READWRITE|CREATE)' "${SRC[@]}" && rule "a database opened for writing"
grep -nE 'sqlite3_prepare' "${SRC[@]}" | grep -vE 'sqlite3_prepare(_v2)?\([^,]+, "SELECT ' && rule "SQL other than SELECT"
grep -n 'sqlite3_open' "${SRC[@]}" | grep -v 'mode=ro&nolock=1' && rule "every sqlite3_open must use mode=ro&nolock=1"
grep -nE '"/system_data/priv/mms/[^"]*"' "${SRC[@]}" | grep -vE 'APP_DB|db_mtime|static const char \*files|"/system_data/priv/mms/[a-z]+\.db(-journal)?"' &&
    rule "a path into the system database folder outside the read-only uses"
for f in "${SRC[@]}"; do
    awk '/write_file\(|open\([^)]*O_(WRONLY|RDWR)/ && /priv\/mms/ {print FILENAME":"NR": "$0; bad=1} END {exit bad}' "$f" ||
        rule "a write into /system_data/priv/mms"
done

# No shell, no other programs: the payload has no business running commands.
grep -nE '\b(system|popen|execl|execlp|execle|execv|execvp|execve)\s*\(' "${SRC[@]}" && rule "running commands"

# Unbounded string functions on data from files or the network.
grep -nE '\b(strcpy|strcat|sprintf|gets|vsprintf)\s*\(' "${SRC[@]}" && rule "unbounded string function (use snprintf/memcpy with a size)"

# Closing apps is only for moving pkgs back (close_open_games): nowhere else.
n=$(grep -c 'sceSystemServiceKillApp(' src/main.c)
[ "$n" -le 2 ] || rule "sceSystemServiceKillApp used outside close_open_games ($n uses)"
grep -nE 'sceSystemServiceKillApp|sceSystemServiceLaunchApp|sceLncUtil' "${OTHER[@]}" && rule "launching or closing apps outside main.c"

# The web panel changes state only on POST: every app_* call that changes something sits in
# a branch that checks for POST.
awk '/strn?cmp\(path/ {cond=$0} /app_(add|remove|restore|restored_forget|config_set|rescan|stop)\(/ && cond !~ /post/ {print FILENAME":"NR": "$0; bad=1} END {exit bad}' src/web.c ||
    rule "a state change in the web panel reachable without POST"

# The page for older firmwares must stay without <dialog> and modern JavaScript.
grep -nE '</dialog>|showModal|fetch\(|=>|\blet |\bconst ' web/legacy.html && rule "legacy.html uses <dialog> or modern JavaScript"

# Every TR(...) text takes the same arguments in the same order: a translation with
# another % conversion would read the wrong argument.
python3 -I - "${SRC[@]}" <<'PY' || rule "a TR translation whose % conversions differ from the English text"
import re, sys
bad = 0
conv = re.compile(r'%[-+ #0]*[0-9*]*(?:\.[0-9*]+)?(?:hh|h|ll|l|z|j|t)?[diouxXcspf%]')
for path in sys.argv[1:]:
    src = open(path, encoding="utf-8").read()
    for m in re.finditer(r'\bTR\(', src):
        depth, i = 1, m.end()
        while depth and i < len(src):
            c = src[i]
            if c == '"':
                i += 1
                while src[i] != '"':
                    i += 2 if src[i] == "\\" else 1
            elif c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
            i += 1
        body = src[m.end():i - 1]
        if body.strip() == "...":
            continue  # the macro itself
        args = re.findall(r'((?:"(?:[^"\\]|\\.)*"\s*)+)(?:,|$)', body)
        texts = ["".join(re.findall(r'"((?:[^"\\]|\\.)*)"', a)) for a in args]
        convs = [conv.findall(t) for t in texts]
        if len(texts) < 2 or any(c != convs[0] for c in convs[1:]):
            line = src.count("\n", 0, m.start()) + 1
            print(f"{path}:{line}: {texts}")
            bad = 1
sys.exit(bad)
PY

# Notifications are in the console's language: a text given to notify() goes through TR()
# (a bare "%s" passes a text made with TR elsewhere).
grep -nE '\bnotify\("' src/main.c | grep -vE 'notify\("%s(: %s)?"' && rule "a notification without TR(): it would show in English on a Spanish console"

[ $fail = 0 ] && echo "policy: all rules hold"
exit $fail
