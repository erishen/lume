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

- [x] **req map exposes no generic headers / origin**: CLOSED 2026-10-22.
      `request_to_value` now carries `origin` and `referer` (both were
      already dedicated HttpRequest fields), plus a generic `headers` map
      built from the framework's new unknown-header capture
      (agent-httpd `08884f0`: headers without a dedicated field are
      retained verbatim in a bounded 4KB buffer). Origin-based cross-site
      checks (node `serve.mts`'s `isLocalOrigin`) are now portable 1:1.
- [x] **route responses cannot set custom security headers**: CLOSED
      2026-10-22. The DSL envelope grows a `headers` map (string values
      only; name validated as an RFC 9110 token, CR/LF rejected loudly
      with a 500) serialized into `HttpResponse.extra_headers`, which
      agent-httpd emits before the security block — and a default
      security header whose name the handler set is suppressed
      (case-insensitive), so `X-Frame-Options: SAMEORIGIN` overrides
      instead of doubling (unit-tested in agent-httpd
      `tests/test_resp_headers.c`).
- [x] **static serving has no gzip**: already stale when inventoried —
      `static.c` has full gzip-sibling negotiation (`.gz` sidecar served
      with `Content-Encoding: gzip`, RFC 7231 Accept-Encoding q-values,
      shared weak ETag, Range refused on the gzipped representation).
      No code change needed; life-dash ships `.gz` siblings at build.
- [x] **outbound HTTP timeout caps at 60s**: CLOSED 2026-10-22. The cap
      moved 60000ms → 600000ms in both copies (lume-core `80ac91a` +
      this tree's `lang/builtins_http.c`); `opts.timeout` still tightens
      per call. life-dash's 300s reverse proxy fits again.
- [x] **server responses hardcode CSP `default-src 'self'`**: CLOSED
      2026-10-22 on both serve paths. `server { csp = "..."; }` (host
      `bridge.c` → `agenthttpd_config.csp`; lume-core native serve
      `bridge_serve.c` reads the same DSL field) replaces the default
      policy, CR/LF rejected at ingest; a per-response envelope
      `headers` CSP entry still wins over the server-wide one. Verified
      end-to-end: `frame-ancestors http://10.0.0.1:16082` round-trips,
      so life-dash can embed the noVNC desktop again.
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



## P12 (audit mirror) — 2026-10-22 php2shell-class memory-safety review

Status: filed 2026-10-22 after the CVE-2026-109618/109619 review;
audit pass over the lume-core (arena/rt/codegen/interp) and
agent-httpd request path found **no** php2shell-shaped stale
global state or use-after-free (request state is stack-local,
shared globals are fixed-size or PTHREAD_PROCESS_SHARED, OOM
exits cleanly). Details and open hardening items live in:

- `agent-httpd/TODO.md` — "P1 — 2026-10-22 memory-safety audit
  follow-ups": strict-CRLF mode (**done 2026-10-22**: default strict CRLF
  with `AGENTHTTPD_LENIENT_CRLF=1` escape hatch, `tests/test_crlf.c`,
  wired into `make test-unit`), `make test-asan` (**done 2026-10-22**:
  SAN_FLAGS parameterized, whole suite under ASan+LSan, keep-alive leg
  under ASan; in the ubuntu-fuzz CI job),
  `tests/fuzz_http.c` (**done 2026-10-22**: chunked framing factored out
  into a pure `http_chunk.c` state machine + libFuzzer target with a
  regression main in `make test-unit`, `make fuzz-http` + CI
  `ubuntu-fuzz` job), session prune/worker write-contract check
  (**done 2026-10-22**: tmp+rename atomic save verified, prune immune to
  in-flight tmp files; `tests/test_session_prune.c`). P1 fully closed.
  C robustness body-OOM (**done 2026-10-22**: unchecked body malloc now
  answers 500 + close instead of crashing in form_field(); LD_PRELOAD
  fault-injection regression `make test-oom` in the ubuntu-fuzz CI job;
  audit confirms fastcgi/cgi request-path buffers already check).
- `lume-core/TODO.md` — "P11": failed-VM-rerun ASAN test
  (interp GC, done 2026-10-22 → `make vm-reuse-test`), ASAN/LSAN targets
  for codegen + LSP bridge (**done 2026-10-22**: codegen covered by
  `asan-compile` — `--compile-text` / `--compile-llvm` /
  `native_backends.sh` under ASan+LSan; LSP doc-lifetime fixed —
  didClose + exit cleanup in `bridge_lsp.c`, `test`

## P13 (host mirror) — 2026-10-30 React SSR / nsbp-lume dependency puzzle

Filed 2026-10-30 from two real projects in this checkout:
`personal/personal-site/nsbp` (React 19 + react-router-dom + `loadable`
+ webpack) and `personal/personal-site/nsbp-lume` (React 19 +
react-router-dom + `loadable` + esbuild, SSR via `renderToPipeableStream`).
The host repo's job here is the **mirror discipline** and the **dependent-
project disposition**, not to re-write lume-core's TODO — the language-side
item is the `lume-core/TODO.md` P14 block.

### Mirror + host discipline

- [ ] Keep the host `lang/` mirror honest if/when the core lands the React-
  SSR-relevant builtin/server gaps from core P14: any new builtin or server
  behavior that nsbp-lume-style projects would want from a lume-hosted
  React SSR site must land in **lume-core**, not in the host's `lang/` tree,
  otherwise `make sync-lang` turns the host into DRIFT next time the core
  and host diverge (the exact problem the host TODO already warns about).
- [ ] Re-run `make sync-lang` discipline (commit core-mirrored content first,
  then sync to refresh the PIN) if lume-core P14 produces a syncable change;
  re-verify the relevant dependent project (e.g. a lume-hosted variant of
  nsbp-lume) after the sync rather than before.

### Dependent-project disposition

- [ ] Decide, for `personal/personal-site/nsbp-lume`, whether it stays a
  **parallel** React SSR project (Hono/node-server, no lume dependency —
  current state) or becomes a **lume-hosted** variant (lume-core `server{}`
  + the project's React SSR build step). The current code does not answer
  this: `nsbp-lume` adds lume-core to its build/runtime picture only as a
  comparison target, not as a dependency. Writing that decision down here
  keeps the next README/architecture pass from reprising it.
- [ ] If the decision is "lume-hosted variant": add a concrete path item,
  not a vague one — e.g. "wire the nsbp-lume React SSR build step so a
  lume-core `server{}` route serves the prebuilt shell + client chunk +
  hydration boot, with the esbuild/ webpack `react-dom/server` polyfill
  story from core P14 resolved first." Do **not** create a TODO item that
  assumes `lume-core/build/lang/c/` already exists — that path does not
  exist as a source directory today (lume-core emits C on demand, it does
  not mirror `lang/` there).
- [ ] If the decision is "parallel / no lume benefit": record why (e.g.
  the React SSR dependency surface, the node-on-windows footprint, and the
  missing esbuild polyfill already make the React SSR host a Node project;
  lume-core's builtin server does not absorb that), so the next review does
  not re-open it as a "gap."

### node-on-windows ops note (host-side read-only)

Not a lume-core language item, but relevant to anyone running these projects
on Windows: today's working Node toolchain on this Windows box is the locally
installed `node@22.11.0` / `npm@10.9.2`; the system `npm@9.6.x` is a known
poison for npm-linked workspace packages and blocks `npm run build` on
nsbp-lume. If the lume host framework ever targets a Windows developer/deployer
picture that includes a React SSR site, the ops story must include "use a sane
Node 20+ toolchain, not the system npm."s/lsp_smoke.sh` wired
  into `make asan` under LSan). P11 fully closed.

Host action: keep the two sub-repo TODOs in sync when picking up
those items (they file into the sub-repos directly, not the host).

## Direction 2 (2026-10-23) — Windows smoke-bin portability + tre UBSan noise

Closed on this repo:

- [x] `tests/smoke.c` now builds and runs on Windows/mingw
      (`make tests/smoke-bin`): agent-httpd/sqlite/http slices gated
      `#ifndef _WIN32` (Windows build has no agent-httpd/sqlite layer
      and no outbound `http_*`), `mkdir_p`/`rmrf` replace the
      `rm -rf && mkdir -p` shell calls (mingw `system()` is cmd.exe),
      and the 0600 permission assertions only check existence on
      Windows (CRT `stat` maps mode bits differently). Result on this
      machine: **98 tests, 0 failed**.
- [x] `lang/loader.c` (mirrored from core): on Windows `_fullpath()`
      only normalizes — it does not require the path to exist, so a
      missing `import` reported `cannot read …` instead of the
      `cannot resolve import` every other platform gives. Added an
      `access(canon, F_OK)` mirror of `realpath` semantics in the
      `_WIN32` branch.

Known Windows-local limits (Linux CI is authoritative; not treated as
bugs): `make asan`'s `tests/smoke-bin-asan` leg OOMs inside the
allocation-heavy tests (Windows ASan runtime shadow-map exhaustion,
"internal allocator is out of memory trying to allocate 0x70 bytes",
not parameter-fixable) and `tests/lsp_smoke.sh` cannot fork the ASan
binary under MSYS2. The `asan-compile` emitter gate, `--check` over
examples, `lang-basics` and the interp leg are all green on Windows.

## P13 (audit mirror) — 2026-10-30 fastapi-web migration inventory

Filed 2026-10-30 after scoping a FastAPI service
(`personal/web-services/fastapi-web`) onto a lume DSL server. Conclusion:
the framework side is further along than expected — MySQL is a real
backend, static serving has gzip/ETag/Range, htpasswd + bcrypt + session
cookies exist, and the response envelope can already override the
security header block. Four gaps block a 1:1 port; only one of them is a
real engineering item. Per-repo detail lives in the two sub-TODOs:

- `agent-httpd/TODO.md` → **P2** — security review (CSRF/Host allowlist,
  cookie `Secure`, login throttling, `req.user`, audit log).
- `lume-core/TODO.md` → **P13** — language builtins (`crypt_verify`,
  `const_time_eq`, multipart, fs allowlist).

Host-side disposition (what this repo decides for dependent projects):

- [ ] **File uploads are out of scope until the body path is redesigned.**
      `MAX_REQUEST_SIZE` is 64 KiB and the body must fit one read buffer
      (`agent-httpd/src/httpd.h:8`, `http.c` 413 path force-closes), and
      there is no multipart parser anywhere in either tree. The download
      direction works today (Lume strings are NUL-safe, so `read_file` +
      a `{type, headers, body}` envelope serves bytes). Decision for
      fastapi-web-style services: no uploads; if a use case truly needs
      them, schedule the buffer redesign, do not bolt a parser onto a
      64 KiB ceiling.
- [ ] **Redis is not adopted.** The tree has zero Redis support, and the
      DSL gives nothing that a SQLite table cannot do at this scale.
      Porting a `redis_client.py` module should become "cache/queue tables
      in the same DB", not a new builtin.
- [ ] **No ORM, no migrations, no OpenAPI.** `sql_query`/`sql_write` are
      raw SQL with guardrails (`builtins_sql.c`); schema changes stay in
      the project's own init script. Do not file this as a gap again —
      it is the price of the zero-dependency property that also removes
      the whole CVE surface (see the security review).
- [ ] **The middleware chain stays a per-handler convention.** IP filter,
      path protection and anti-spam were FastAPI middleware; here they
      become a `guard(req)` call at the top of each handler. Accepted,
      with the caveat that one forgotten call is a hole — the containment
      for that is the `is_same_origin` gate the agent-httpd P2 item adds,
      which cannot be forgotten because it lives in the framework.
- [ ] **Builtins land in lume-core first, then `make sync-lang`.** The
      warmup change below is exactly the shape that goes wrong: host-only
      edits to a synced file turn into DRIFT on the next sync. Any new
      builtin adds **five** coordinated edits — `LUME_BUILTIN_NAMES` and
      the arity contract in `typecheck.c`, the `interp.c` builtin table,
      the `b_*` wrapper in `builtins.c`, and the prototype in
      `builtins_internal.h` — plus the host mirror.

### In flight — macOS fork-safety TLS warmup (uncommitted 2026-10-30)

Both working trees carry an uncommitted change that must land together:

- this repo: `lang/builtins_http.c` + `lang/builtins_internal.h` add
  `lume_http_warmup()` — builds the SSL CTX **and performs one probe TLS
  handshake** before any thread exists, because macOS libobjc aborts the
  child when Objective-C runtime init (CFNetwork/Security, reached from
  the first cert-store read) is in progress across a `fork()`. Compiles
  clean against the pinned submodule (verified 2026-10-30).
- `agent-httpd/src/core/framework.c` calls it through a **weak symbol**
  right after `agent_init()` and before `pthread_create(session_prune_
  thread)` — the ordering is the whole point, so do not move the call
  below the thread or below `start_worker_pool()`.

- [ ] Commit both halves (core/gitlink discipline: the agent-httpd side
      first, then the host picking up the bumped gitlink), or revert both
      together. A half-landed pair compiles on this machine only.
- [ ] Warmup currently dials `example.com:443` with a 5 s deadline on
      every server start. That is a network dependency and a third-party
      connection from a local service. Replace the probe with something
      that exercises the same init path without external traffic (connect
      + `SSL_CTX_set_default_verify_paths` against a local socket, or gate
      the dial behind an env opt-in), and make the failure mode a warning
      rather than a 5 s startup stall on offline machines.
- [ ] Add a regression that runs the prefork server with an outbound
      `https://` route as its **first** request — the bug only reproduces
      on the first HTTPS egress per process, so a test that warms up in
      the parent by accident proves nothing.
