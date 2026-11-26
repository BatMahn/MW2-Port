#!/bin/sh
set -eu
export LC_ALL=C

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
dev_switches=$(grep -hEo 'mw2_development_env\("MW2_[A-Z0-9_]+"' \
    "$root"/src/*.c "$root"/tools/glview.c "$root"/tools/mw2shell.c "$root"/tools/mapview.c |
    cut -d '"' -f 2 | sort -u)
test -n "$dev_switches"

for binary do
    contents=$(strings -a "$binary")
    if matches=$(printf '%s\n' "$contents" | grep -Fx "$dev_switches"); then
        printf '%s contains development switches:\n%s\n' "$binary" "$matches" >&2
        exit 1
    fi
    if matches=$(printf '%s\n' "$contents" | grep -E '^([._]|__)(debug_(info|line|abbrev|str)|zdebug_info)$|^__(asan|ubsan|tsan|msan)_init$'); then
        printf '%s contains debug or sanitizer metadata:\n%s\n' "$binary" "$matches" >&2
        exit 1
    fi
done
