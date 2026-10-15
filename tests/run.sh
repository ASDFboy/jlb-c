#!/bin/sh
# Runs every program in tests/cases with an interpreter and compares its output
# (standard output, then standard error) with the matching .out file. A matching
# .in file is fed in as typed input. The Rust interpreter (jlb-rust) uses the same cases.
#
#   tests/run.sh ./jlb-c

exe=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
cases=$(cd "$(dirname "$0")/cases" && pwd)
pass=0
fail=0
for prog in "$cases"/*.jlb; do
    name=$(basename "$prog" .jlb)
    tmp=$(mktemp -d)
    input="$cases/$name.in"
    [ -f "$input" ] || input=/dev/null
    (cd "$tmp" && "$exe" "$prog" < "$input" > "$tmp/stdout" 2> "$tmp/stderr")
    # Compare without carriage returns or trailing blank lines.
    cat "$tmp/stdout" "$tmp/stderr" | tr -d '\r' | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}' > "$tmp/actual"
    tr -d '\r' < "$cases/$name.out" | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}' > "$tmp/expected"
    if cmp -s "$tmp/actual" "$tmp/expected"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "--- $name ---"
        diff "$tmp/expected" "$tmp/actual" | head -20
    fi
    rm -rf "$tmp"
done
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
