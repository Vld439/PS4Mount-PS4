#!/bin/bash
# The log analyzer (tools/logcheck.py) on logs whose problems are known: its report must
# match test/data/logs/<name>.<lang>.expected, and it must exit 1 when it finds an error.
#   UPDATE=1 bash test/logcheck_test.sh   writes the reports again (review the diff!)
cd "$(dirname "$0")/.." || exit 1
fail=0 checks=0
check() { # name lang want_exit
    local log=test/data/logs/$1.log want=test/data/logs/$1.$2.expected got rc
    got=$(python3 -I tools/logcheck.py --lang "$2" "$log")
    rc=$?
    if [ "${UPDATE:-0}" = 1 ]; then
        printf '%s\n' "$got" > "$want"
        return
    fi
    checks=$((checks + 1))
    if [ "$got" != "$(cat "$want")" ]; then
        echo "  FAIL $1 ($2): the report changed"
        diff <(printf '%s\n' "$got") "$want" | head -20
        fail=1
    fi
    checks=$((checks + 1))
    if [ "$rc" != "$3" ]; then
        echo "  FAIL $1 ($2): exit $rc, wanted $3"
        fail=1
    fi
}
check healthy en 0
check healthy es 0
check problems en 1
check problems es 1
# the log read from the standard input (curl .../api/log | ...) gives the same report
got=$(python3 -I tools/logcheck.py --lang en - < test/data/logs/healthy.log | sed 's/^File: .*$//')
want=$(sed 's/^File: .*$//' test/data/logs/healthy.en.expected)
checks=$((checks + 1))
[ "$got" = "$want" ] || { echo "  FAIL standard input: another report"; fail=1; }
[ "${UPDATE:-0}" = 1 ] || echo "$checks checks, $fail failed"
exit "$fail"
