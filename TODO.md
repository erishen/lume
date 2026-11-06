# TODO — lume (host)

The host repo mirrors `lume-core`'s `lang/` tree via `make sync-lang`;
interpreter/language fixes belong in lume-core and flow here. The files
that must be hand-merged are the 17 entries under `host_owned:` in
`lang/PIN` — that list is the source of truth, `sync-lang.py` never
overwrites them, and `make check-sync` (strict) reports their accepted
lag from `lang/PIN.lag`. Server-side fixes belong to the standalone
`agent-httpd` repo (mirrored here as the `agent-httpd` submodule).

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
- [x] **Builtins land in lume-core first, then `make sync-lang`.** The
      warmup change below is exactly the shape that goes wrong: host-only
      edits to a synced file turn into DRIFT on the next sync. Any new
      builtin adds **five** coordinated edits — `LUME_BUILTIN_NAMES` and
      the arity contract in `typecheck.c`, the `interp.c` builtin table,
      the `b_*` wrapper in `builtins.c`, and the prototype in
      `builtins_internal.h` — plus the host mirror.
      **Followed 2026-11-04** for `crypt_verify` / `const_time_eq`: core
      landed first (lume-core `acbabcb`), then this host mirrored it —
      `lang/builtins_crypt.c` copied (a synced file), and the host-owned
      `interp.c` / `builtins.h` / `typecheck.c` registered by hand (they
      are in `HOST_PINNED`, so `sync-lang` never touches them). At the
      time this box had no python3, so the `PIN.manifest` md5 for
      `builtins_crypt.c` was refreshed by hand. **Resolved**: the pin was
      later re-cut with a real `make sync-lang` (see `lang/PIN`), and
      `check --strict` now fails on any hand-edited synced file whose md5
      no longer matches `PIN.manifest`. Refreshing an md5 by hand is never
      the fix — run `make sync-lang && make check-sync` instead.

### In flight — macOS fork-safety TLS warmup (committed 2026-10-31, probe rework 2026-11-04)

Both halves landed 2026-10-31 (agent-httpd `09050e1c` calls via the
file-end weak fallback right after `agent_init()`, before the first
thread; host `builtins_http.c` provides the strong symbol). A duplicate
late call (`71984e20`) that re-dialed the probe *after*
`pthread_create(session_prune_thread)` was removed 2026-11-04 — it was
both redundant and wrong-ordered.

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

- [x] Commit both halves (core/gitlink discipline: the agent-httpd side
      first, then the host picking up the bumped gitlink), or revert both
      together. A half-landed pair compiles on this machine only.
      **Done 2026-10-31** (`09050e1c` + host side; `71984e20` duplicate
      removed 2026-11-04).
- [x] Warmup currently dials `example.com:443` with a 5 s deadline on
      every server start. That is a network dependency and a third-party
      connection from a local service. Replace the probe with something
      that exercises the same init path without external traffic (connect
      + `SSL_CTX_set_default_verify_paths` against a local socket, or gate
      the dial behind an env opt-in), and make the failure mode a warning
      rather than a 5 s startup stall on offline machines.
      **Done 2026-11-04**: probe now defaults to loopback `127.0.0.1:443`
      with a 500 ms deadline — no third-party connection, no startup
      stall on offline machines (local connect fails instantly), while a
      local https service still completes a real handshake and trips the
      Keychain init chain. Old external behaviour is an explicit opt-in:
      `LUME_TLS_WARMUP_PROBE=example.com:443`.
- [x] Add a regression that runs the prefork server with an outbound
      `https://` route as its **first** request — the bug only reproduces
      on the first HTTPS egress per process, so a test that warms up in
      the parent by accident proves nothing.
      **Done 2026-11-04**: `tests/run_all.sh` leg 4f boots a minimal
      server whose `/probe` handler issues `http_get("https://example.com/")`
      — the process's first HTTPS egress, the server having done no
      outbound work at startup (the probe is loopback-only). Skips with a
      notice when outbound network is unavailable; CI is authoritative.

### 2026-11-06 review — status, constraints, and the converged fix plan

Read-only re-investigation from a Windows host. No product code was changed:
the three verification blockers listed below make an unverified C edit to the
TLS path irresponsible. Everything here is checkable in the trees as of
host `c24178f` / lume-core `5b6fbbb`.

**Where the hook lives (no sync / no agent-httpd change needed).**

- `lang/builtins_http.c` is in `HOST_PINNED` (`scripts/sync-lang.py:97-100`).
  The warmup body is host-owned: it can be edited in place and
  `make check-sync --strict` will neither overwrite it nor report drift. No
  lume-core edit + `make sync-lang` round-trip is required.
- `lume_http_warmup()` (`builtins_http.c:441`) is the *strong* symbol;
  agent-httpd's `src/core/framework.c:836` weak fallback is used only by
  standalone builds (it `fork()`s a `curl https://example.com` child). The
  call site (`framework.c:614`) is already correctly ordered — after
  `agent_init()`, before `pthread_create(session_prune_thread)`, before
  `start_worker_pool()`. Do not move it.

**Why the loopback probe is a no-op.** `ssl_ctx_ready()` builds the CTX and
calls `SSL_CTX_set_default_verify_paths()`, then the warmup dials
`127.0.0.1:443` with a 500 ms deadline. Nothing listens there on a CI
runner, so `open_conn_impl()` fails and `tls_attach()` is never reached —
not one `SSL_do_handshake()` runs. `builtins_http.c:444` already records
that building the CTX is *not* enough: the objc/Security init is reached
inside the handshake. The 2026-11-04 "no third-party connection" change
therefore removed the only action the warmup existed to perform.

**Why the external probe is NOT the fix.** Leg 4f
(`tests/run_all.sh:341-394`) only proves anything if the server does no
outbound work at startup and `/probe` is the process's *first* HTTPS
egress. A real handshake to `example.com:443` in the master would itself
become that first egress, turning 4f green without proving a forked worker
is safe — the same class of mistake as setting
`OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES`. Keep
`LUME_TLS_WARMUP_PROBE` as a throwaway A/B diagnostic only, never as the
shipped default, and do not "fix" the macOS job by exporting it for the
whole `make test`.

**Converged fix: one entirely local, self-contained TLS handshake.**

1. Add a fixed self-signed cert/key as PEM string constants in
   `builtins_http.c` (generated once, offline:
   `openssl req -x509 -newkey rsa:2048 -nodes -days 36500 -subj /CN=lume-warmup.local`).
   Load them with `BIO_new_mem_buf` + `PEM_read_bio_X509` /
   `PEM_read_bio_PrivateKey` into a server `SSL_CTX` (`TLS_server_method`).
   PEM readers are stable across OpenSSL 1.1 and 3.0, so this sidesteps the
   `RSA_generate_key` / `EVP_RSA_gen` key-generation API split entirely.
   The key is loopback-only, never sent to a peer, and the cert is
   deliberately untrusted.
2. Drive client and server **in the same single thread** — `BIO_new_bio_pair()`
   (or `socketpair()`) plus two `SSL*` objects, alternating
   `SSL_do_handshake()` with a bounded iteration cap / deadline. No `fork`,
   no `pthread_create`: the pre-fork single-thread invariant is the whole
   point of the hook.
3. The client keeps using `g_ssl_ctx` (`SSL_VERIFY_PEER` + default verify
   paths). Verification of the self-signed cert will fail — expected, and
   must be ignored. A failing verification still walks `X509_verify_cert`
   through the system CA store / Keychain lookup, which is precisely the
   init chain the warmup needs to trip.
4. Drop `open_conn_impl` from the warmup path; keep
   `LUME_TLS_WARMUP_PROBE` as an explicit diagnostic opt-in. Add an
   `HTTP_DBG` line recording whether the local handshake was attempted and
   completed, so a macOS CI log can distinguish "no handshake" from
   "handshake happened but the worker still crashed".
5. All failures stay non-fatal (`return 0`), matching current semantics.

**Verification blockers on this host (why no C change was committed).**

- `Makefile:153` sets `HAVE_OPENSSL := 0` on Windows; no `fork`.
- `openssl` CLI 3.5.7 (MSYS) exists but there is no `openssl/ssl.h` anywhere
  on this machine, so `-DHAVE_OPENSSL=1 -fsyntax-only` is impossible.
- `make check-sync` cannot run here either: Error 9009 (`python3`/`python`
  are Windows Store stubs; `py -3` reports "No Installed Pythons Found").
  Not blocking for this file (HOST_PINNED), but worth recording.
- macOS lane remains `continue-on-error: true` (`ci.yml:94`) until the
  acceptance below is actually observed on a macOS runner. No result is
  claimed from this host.

**Acceptance.** macOS job's 4f passes with `LUME_REQUIRE_EGRESS=1` (i.e. not
an offline skip), the other ~115 tests stay green, the server log shows the
local-handshake debug line, `OBJC_DISABLE_INITIALIZE_FORK_SAFETY` stays
unset, and only then may `ci.yml:94` be promoted back to hard-blocking.

**If 4f still fails after the local handshake:** the "Keychain read inside
the handshake" theory is wrong. Next suspect is the first
`getaddrinfo("example.com")` — note the pure-HTTP control path in 4f uses
`127.0.0.1`, so *no* control request resolves a hostname either. Warm up
DNS resolution once in the master and retest, using `HTTP_DBG` to report
which branch ran.
