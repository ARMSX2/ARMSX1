#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -ne 3 ]; then
    echo "Usage: $0 <binary> <dest-dir> <search-dir-1:search-dir-2:...>" >&2
    exit 1
fi

binary="$1"
dest_dir="$2"
search_dirs="$3"

mkdir -p "$dest_dir"

declare -A seen=()
queue=("$binary")

resolve_library() {
    local lib_name="$1"
    local dir=
    local old_ifs="$IFS"
    IFS=':'
    for dir in $search_dirs; do
        if [ -f "$dir/$lib_name" ]; then
            printf '%s\n' "$dir/$lib_name"
            IFS="$old_ifs"
            return 0
        fi
        if [ -d "$dir" ]; then
            local recursive_match
            recursive_match="$(find "$dir" -type f -name "$lib_name" -print -quit 2>/dev/null || true)"
            if [ -n "$recursive_match" ]; then
                printf '%s\n' "$recursive_match"
                IFS="$old_ifs"
                return 0
            fi
        fi
    done
    IFS="$old_ifs"
    return 1
}

while [ "${#queue[@]}" -gt 0 ]; do
    current="${queue[0]}"
    queue=("${queue[@]:1}")

    while IFS= read -r needed; do
        [ -n "$needed" ] || continue

        case "$needed" in
            linux-vdso.so.*|ld-linux*.so.*)
                continue
                ;;
        esac

        if [ -n "${seen[$needed]:-}" ]; then
            continue
        fi

        resolved="$(resolve_library "$needed" || true)"
        if [ -z "$resolved" ]; then
            echo "Unable to resolve runtime dependency '$needed' for '$current'." >&2
            exit 1
        fi

        cp -L "$resolved" "$dest_dir/$needed"
        seen["$needed"]=1
        queue+=("$resolved")
    done < <(readelf -d "$current" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')
done
