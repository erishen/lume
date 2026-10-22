# TODO — lume (host)

The host repo mirrors `lume-core`'s `lang/` tree via `make sync-lang`;
interpreter/language fixes belong in lume-core and flow here (host-owned
files — currently `typecheck_expr.c`, `typecheck.c`, and the interp.c
minijson include block — must be hand-merged). Server-side fixes belong
to the standalone `agent-httpd` repo (mirrored here as the
`agent-httpd` submodule).

## P9 (mirror) — for-in over an inline temp value: heap corruption under agent-httpd

`for (x in get(load_xxx(), "k", []))` — iterating an inline expression
chain not bound to a `let` — crashes the HTTP server after ~5 repeated
requests (silent SIGSEGV, no panic output; stdout is block-buffered and
lost). Empirically established on this repo's binary serving the
chinese knowledge project (`work/knowledge/chinese`, fix `c10f23d`);
full investigation, evidence, and the ASan triage plan live in
**lume-core `TODO.md` → P9** (the bare-interpreter path does NOT
reproduce, so the bug sits at the interpreter/agent-httpd lifecycle
boundary).

Host-side items:

- [x] After the core P9 fix lands, hand-merge any interpreter-side
      change into host-owned files (if applicable), re-run
      `make sync-lang` discipline (commit content first, then sync to
      refresh the PIN), and re-verify the chinese project's original
      inline-temp handler under ASan + the 14-endpoint smoke. — Closed
      2026-10-21: interp.c (host-owned) hand-merged the core 475e099
      fix (`94b9b03`, iterable kept on the stack across the whole loop,
      popped on every exit), tests/for-in-gc.lume replayed green
      (total 60000 / keys abc / inner 10000, rc=0); `sync-lang.py
      check` is green @ 456485ea6ba5 (30 synced / 16 host-owned / 6
      host-only, 0 DRIFT — the core's later commits touched no synced
      file, so the PIN at 8a5a0f4 stays valid). ASan + 14-endpoint
      smoke on the chinese project needs a Linux container (the
      Windows build has no HTTP bridge); the core-side ASan replay of
      tests/for-in-gc.lume runs in ci.yml's asan job on every push,
      and run_all.sh leg 4e covers the HTTP bracket on Linux CI.
- [x] Containment in dependent code: all 9 inline-temp for-in loops in
      `chinese/server.lume` converted to `let`-before-loop (2026-10-20,
      chinese `c10f23d`); recite ×11 + full smoke green.
- [x] Coding rule for host/lang code and lume programs served by
      agent-httpd: **never iterate an inline expression chain in
      for-in** — bind the iterable to a `let` first. Recorded in the
      chinese project memory and here until P9 is fixed.

## P10 (agent-httpd) — request/response envelope limits surfaced by the life-dash migration

`personal/life-dash` migrated from node `serve.mts` to a lume DSL
service (2026-10-21, life-dash `848cea5`, 165MB → 21.4MB). Four
framework limits were accepted as degradations; each is tracked here
for a future agent-httpd pass:

- [ ] **req map exposes no generic headers / origin**: the request map in
      `lang/bridge.c` carries method/path/host/content_type/user_agent/
      body/query_params/params, but no `headers` field — Origin-based
      cross-site checks (node `serve.mts`'s `isLocalOrigin`) cannot be
      ported 1:1. life-dash now relies on loopback bind (compose
      `127.0.0.1`) + the framework emitting no CORS headers, so browser
      SOP blocks cross-site reads anyway.
- [ ] **route responses cannot set custom security headers**: the DSL
      envelope supports `status`/`type`/`body`/`cache_control` only —
      `X-Frame-Options`, `X-Content-Type-Options`, `Referrer-Policy`
      (all set by the node server) are unreachable from a script.
- [ ] **static serving has no gzip**: `agent-httpd/src/http/static.c`
      serves assets uncompressed — the >1KB gzip path of node
      `serve.mts` ships raw (life-dash SPA JS ~396KB, ~142KB gzipped).
- [ ] **outbound HTTP timeout caps at 60s**: `HTTP_MAX_TIMEOUT_MS`
      (`lang/builtins_http.c`) is 60000ms — life-dash's
      `/__devstats/refresh` reverse proxy to the host trigger needed
      300s; refreshes over 60s now surface a 502. Consider raising the
      cap or making it per-call configurable.
- [ ] **server responses hardcode CSP `default-src 'self'`**: `bridge_serve.c`
      `send_resp()` emits `Content-Security-Policy: default-src 'self'` +
      `X-Frame-Options: DENY` on every response (no per-app config).
      Cross-origin iframes are blocked — life-dash could not embed the
      shared browser-desk noVNC desktop (127.0.0.1:16082) and had to switch
      to open-in-new-tab. Source comment already flags a future
      `server { csp = ... }` option to relax it per-app (e.g. allow
      `frame-src` for a configured noVNC origin).
- [x] **agent-httpd submodule pinned before react_prefix existed**: 
      `lang/bridge.c` reads `cfg.react_prefix` (configurable SSR relay
      prefix — history `server.lume` uses `react_prefix = "/"`) but the
      submodule gitlink pointed at 195cfaf, whose `agenthttpd_config`
      lacks the field → history's SSR Dockerfile build failed
      (`'agenthttpd_config' has no member named 'react_prefix'`). Fixed
      locally by checking out `win-local-chain` (47320af, includes
      0b43aa6) and rebuilding — verified SSR relay works (home + deep
      `/dynasty/:id` paths return 200). **Closed 2026-10-22**: the
      react_prefix commits were cherry-picked onto agent-httpd main and
      pushed (`b998e7f..23b1c4e`); the gitlink was bumped and committed
      (`7e73e91`), host `make all` compiles against it, and a fresh
      `clone --recurse-submodules` checks out 23b1c4e with the field
      present. SSR relay is now reproducible off-machine. (Note: the
      interim gitlink at b998e7f was briefly uncompilable for the host —
      the submodule working copy had been masking the missing field by
      sitting on `win-local-chain`.)


