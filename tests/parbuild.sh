#!/usr/bin/env bash
# Parallel, correctly-cached compile-and-link for the gate's instrumented builds.
#
# WHY: the gate's ASAN build was ONE g++ invocation over 42 translation units, so it compiled them
# serially: 365 seconds, the largest single row in a 44-minute gate, and not a test at all. The
# read-local ownership-invariant build cost another 276s the same way. On a 128-core box that is
# most of the gate's wall clock spent on one core. Measured here: 58s parallel against 365s serial.
#
# CACHE KEY. Reusing an object because it is newer than its .cc is WRONG -- a changed header leaves
# every .o stale and the gate would test code that no longer exists. The key is therefore a hash of
# the source, EVERY header in the tree, and the exact compile flags. Any of those moving is a miss.
# That makes a re-run on an unchanged tree nearly free, which is what makes iterating on the gate
# itself bearable.
#
# Usage: parbuild.sh <output> <objdir> "<compile flags>" "<link flags>" <source>...
set -u
OUT=$1; OBJDIR=$2; CFLAGS=$3; LFLAGS=$4; shift 4
mkdir -p "$OBJDIR" || exit 1
JOBS=${PARBUILD_JOBS:-$(nproc)}

# One hash over all headers + flags; combined per-file with the source's own hash.
HDRS=$( { find src tests -name '*.h' -o -name '*.inc' 2>/dev/null | sort | xargs cat 2>/dev/null; \
          printf '%s' "$CFLAGS"; } | sha256sum | cut -c1-16)

compile_one() {
    local src=$1 obj=$2 stamp=$3
    local key; key=$( { sha256sum "$src" | cut -d' ' -f1; printf '%s' "$HDRS"; } | sha256sum | cut -c1-32)
    if [ -f "$obj" ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$key" ]; then return 0; fi
    g++ $CFLAGS -c "$src" -o "$obj" 2>"$obj.err" || { cat "$obj.err" >&2; return 1; }
    printf '%s' "$key" > "$stamp"
}

objs=(); pids=(); rc=0
for src in "$@"; do
    base=$(echo "$src" | tr '/' '_' | sed 's/\.cc$//')
    obj="$OBJDIR/$base.o"; objs+=("$obj")
    compile_one "$src" "$obj" "$OBJDIR/$base.key" &
    pids+=($!)
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n 2>/dev/null || rc=1; done
done
for p in "${pids[@]}"; do wait "$p" || rc=1; done
[ $rc -eq 0 ] || { echo "parbuild: a translation unit failed to compile" >&2; exit 1; }

g++ $CFLAGS "${objs[@]}" -o "$OUT" $LFLAGS
