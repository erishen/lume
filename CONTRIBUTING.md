# Contributing

## Toolchain

- C11 compiler with OpenSSL and SQLite development headers
- Node.js 24 + pnpm 9 (only needed for the React UI under `frontend/`)
- Python 3 (test helpers, the privacy guard, the baseline tooling)

`make` builds `bin/lume`. `make check` type-checks every example;
`make test` runs the full end-to-end suite in `tests/run_all.sh` against a
real HTTP server. The test script exempts loopback requests from proxy
environment variables while leaving the external egress probe on the normal
network path, so a local proxy cannot turn the suite into a false red.

## Before opening a PR

Run at minimum:

```
make check
make test
make check-sync check-parity check-version check-baseline
make privacycheck treecheck
```

`make asan` adds ASan/UBSan plus the isolated warmup leak harness. Leak
assertions are Linux-only (LeakSanitizer); macOS runs the same rounds under
ASan/UBSan without them. CI runs the same gates on Ubuntu, macOS and
Windows.

## The `lang/` tree is pinned

`lang/` is a pinned copy of the sibling `lume-core` language tree — see
`lang/PIN` and `PIN.manifest`. Files marked *synced* must stay byte-identical
to upstream: change them in `lume-core` first, then `make sync-lang`.
*Host-owned* files (for example `interp.c` and `builtins_http.c`) are edited
here and hand-merged upstream when needed. `make check-sync` fails on drift
and on a hand-edit of a synced file, and CI runs it. Editing a synced file in
place will be overwritten by the next sync.

After changing a version macro, rebuild from clean object files: the objects
are not keyed on the version value.

## This repository is public

Never commit absolute build-machine paths, developer usernames, tokens or
`.env` files; use placeholders and `.env.example` instead.
`make privacycheck` scans HEAD blobs, staged blobs and non-ignored worktree
files byte-for-byte, so running it locally now catches a leak *before*
`git add`. `make treecheck` is the older shell guard for build-machine paths.
For the whole object database, `make deepcheck` is a manual audit tool and is
deliberately not wired into CI.
