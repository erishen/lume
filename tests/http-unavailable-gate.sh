#!/bin/sh
# Windows/mingw regression for the host mirror of the 2026-11-09 diagnostic
# fix (lume-core TODO.md P17, "Found during the Windows lane").
#
# The five outbound HTTP verbs are registered in typecheck.c unconditionally,
# but LUME_HAS_HTTP=0 compiles out their implementation. They must still be
# seeded in the interpreter and fail with the clear "unavailable" error rather
# than `undefined variable`. The three inbound SSE names are a PERMANENT
# core-only boundary in the host (TODO.md P17 mirror) and must stay absent.
#
# Wired into `make check` on the Windows lane only: POSIX builds have
# LUME_HAS_HTTP=1 and carry the real implementations.

set -eu

cd "$(dirname "$0")/.."

BIN=./bin/lume
if [ ! -x "${BIN}" ]; then BIN=./bin/lume.exe; fi
if [ ! -x "${BIN}" ]; then
    echo "FAIL http-unavailable-gate: build the binary first (make all)"
    exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' 0
trap 'exit 1' 1 2 3 15

MSG='unavailable: this build was compiled without LUME_HAS_HTTP'

for name in http_get http_post http_put http_patch http_delete; do
    # typecheck.c gives every verb arity 1..-1, so a single argument is enough.
    printf 'let x = %s("http://127.0.0.1:1/");\n' "${name}" > "${tmp}/${name}.lume"

    if ! "${BIN}" --check "${tmp}/${name}.lume" > "${tmp}/${name}.check" 2>&1; then
        echo "FAIL http-unavailable-gate: --check rejected ${name} (typecheck lost the name):"
        cat "${tmp}/${name}.check"
        exit 1
    fi
    if "${BIN}" "${tmp}/${name}.lume" > "${tmp}/${name}.out" 2>&1; then
        echo "FAIL http-unavailable-gate: ${name} unexpectedly succeeded on this build:"
        cat "${tmp}/${name}.out"
        exit 1
    fi
    if grep -Fq 'undefined variable' "${tmp}/${name}.out"; then
        echo "FAIL http-unavailable-gate: ${name} fell back to 'undefined variable':"
        cat "${tmp}/${name}.out"
        exit 1
    fi
    if ! grep -Fq "${MSG}" "${tmp}/${name}.out"; then
        echo "FAIL http-unavailable-gate: ${name} did not report the clear error:"
        cat "${tmp}/${name}.out"
        exit 1
    fi
    echo "ok   http-unavailable-gate: ${name}"
done

# The host must NOT grow the P17 inbound SSE names: compiled binaries have no
# VM policy to carry no_net / host_blocked, so this is a permanent boundary.
printf '%s\n' 'let c = http_stream_open("http://127.0.0.1:1/sse");' > "${tmp}/stream.lume"
if "${BIN}" "${tmp}/stream.lume" > "${tmp}/stream.out" 2>&1; then
    echo "FAIL http-unavailable-gate: http_stream_open unexpectedly available in the host"
    cat "${tmp}/stream.out"
    exit 1
fi
if ! grep -Fq "undefined variable 'http_stream_open'" "${tmp}/stream.out"; then
    echo "FAIL http-unavailable-gate: P17 core-only boundary changed; re-review the decision:"
    cat "${tmp}/stream.out"
    exit 1
fi
echo "ok   http-unavailable-gate: http_stream_* stays core-only"

echo "==> http-unavailable-gate: opted-out HTTP fails clearly, SSE stays absent"
