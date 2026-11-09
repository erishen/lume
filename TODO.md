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

- [ ] **File uploads are out of scope until a multipart parser exists.**
      Original 2026-10-30 assessment (kept as the reasoning record):
      `MAX_REQUEST_SIZE` was 64 KiB and the body had to fit one read
      buffer (`agent-httpd/src/httpd.h:8`, `http.c` 413 path force-closes),
      so there was no multipart parser anywhere in either tree.
      **Update 2026-11-09:** the transport half of that redesign landed
      on 2026-11-04 — bodies above the 64 KiB read buffer are now spooled
      to a `mkstemp` temp file up to `MAX_BODY_SPOOL_BYTES` (default
      8 MiB, `BODY_SPOOL_DIR`), `req->body_temp_path` names the file,
      and CGI/FastCGI consume it. The architectural blocker is gone;
      what remains is the `multipart(req.body, req.content_type)` builtin
      (lume-core `TODO.md` P13, item (a)). The download direction works
      today (Lume strings are NUL-safe, so `read_file` + a
      `{type, headers, body}` envelope serves bytes). Decision for
      fastapi-web-style services is unchanged: no uploads, and do not
      bolt a parser on before item (a) is scheduled.
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

### macOS fork-safety TLS warmup (committed 2026-10-31, probe rework 2026-11-04; FIXED 2026-11-07 — see the landed section at the end of this file)

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
whole `make test`. Such an A/B run must happen outside the authoritative
leg; as of 2026-11-06 `tests/run_all.sh` guard rejects any non-loopback
`LUME_TLS_WARMUP_PROBE` with an explicit false-green diagnostic, so the two
can no longer coexist silently.

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

> **DO NOT COPY THIS PATCH — investigation record only.** Everything from
> here down to the "2026-11-07 — LANDED" section records a theory that the
> A/B test **disproved** on macOS: it blames a Keychain read during the TLS
> handshake, and its acceptance text still mentions an external probe and
> `HTTP_DBG`. The shipped fix primes one external hostname resolution before
> `fork()`, calls `ssl_ctx_ready()`, logs through `WARMUP_NOTE`, and never
> reads `LUME_TLS_WARMUP_PROBE`. The 11-07 in-memory BIO-pair handshake was
> itself removed 2026-11-08 (A/B: not load-bearing). See the LANDED section
> and `lang/builtins_http.c` for what actually landed. Kept as the reasoning
> trail, not as a template.

### Reference patch — `lume_http_warmup()` (historical; compiled + landed 2026-11-07)

> **STATUS: reference only.** Written on a Windows host with
> `HAVE_OPENSSL=0`, no `openssl/ssl.h` and no Python 3. This code has
> **not been compiled against real OpenSSL headers, linked or run**. It
> did pass `clang -std=c11 -Wall -Wextra -fsyntax-only` against stub
> declarations (rc=0, no warnings), which rules out C syntax errors only —
> OpenSSL API signatures, linkage and runtime behaviour remain unverified.
> Do not paste it into `lang/builtins_http.c` and call the bug fixed — build
> it on macOS first with `make CFLAGS_EXTRA='-DLUME_HTTP_DEBUG'` and confirm
> the `[http] warmup:` line. It lives here so the macOS pass starts from a
> concrete implementation instead of a prose sketch.
>
> **2026-11-07 update:** this patch has now been compiled with real OpenSSL 3
> headers under `-Werror`, linked, run and landed (plus one extra step the
> patch was missing — see the final section). The original warning stands for
> the *text above as written*; keep it as the historical record, do not read
> it as the current status of `lang/builtins_http.c`.

**Why not an embedded PEM.** The first draft embedded a fixed self-signed
cert/key as string constants — short, and immune to any API split. It was
dropped: committing a private key (even a deliberately-untrusted,
loopback-only, in-process test key) into a public repo is a bad default.
Generate the key at runtime instead; the temp files from that draft were
deleted.

**Avoiding the OpenSSL 1.1 / 3.0 split.** Do **not** use
`RSA_generate_key_ex` (deprecated in 3.0, breaks `-Werror` builds) or
`EVP_RSA_gen` (3.0 only). `EVP_PKEY_CTX_new_id` + `EVP_PKEY_keygen_init` +
`EVP_PKEY_CTX_set_rsa_keygen_bits` + `EVP_PKEY_keygen` is identical on
1.1.0+ and 3.x. Everything else below (`X509_*`, `SSL_CTX_*`,
`BIO_new_bio_pair`, `SSL_set_bio`) is likewise available in both.

**Extra includes**, next to `builtins_http.c:49-50`:

```c
#include <openssl/x509.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>   /* EVP_PKEY_CTX_set_rsa_keygen_bits */
#include <openssl/bio.h>
```

**Replace the entire `#if defined(HAVE_OPENSSL)` arm of
`lume_http_warmup()` (`builtins_http.c:442-487`) with:**

```c
int lume_http_warmup(void) {
#if defined(HAVE_OPENSSL)
    /* The business CTX must still be built in the master. */
    if (ssl_ctx_ready() != 0) return -1;

    int hs_done = 0;
    X509 *cert = NULL;
    EVP_PKEY *key = NULL;
    EVP_PKEY_CTX *pctx = NULL;
    SSL_CTX *sctx = NULL, *cctx = NULL;
    SSL *cli = NULL, *srv = NULL;
    BIO *cbio = NULL, *sbio = NULL;

    /* 1. Ephemeral RSA key (the one API stable across 1.1 and 3.x). */
    pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (!pctx || EVP_PKEY_keygen_init(pctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) <= 0 ||
        EVP_PKEY_keygen(pctx, &key) <= 0) goto done;

    /* 2. A 60-second self-signed cert. It never leaves this process. */
    cert = X509_new();
    if (!cert) goto done;
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 60);
    X509_set_pubkey(cert, key);
    {
        X509_NAME *nm = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
            (const unsigned char *)"lume-tls-warmup.invalid", -1, -1, 0);
        X509_set_issuer_name(cert, nm);
    }
    if (X509_sign(cert, key, EVP_sha256()) <= 0) goto done;

    /* 3. Server side. */
    sctx = SSL_CTX_new(TLS_server_method());
    if (!sctx) goto done;
    if (SSL_CTX_use_certificate(sctx, cert) != 1 ||
        SSL_CTX_use_PrivateKey(sctx, key) != 1 ||
        SSL_CTX_check_private_key(sctx) != 1) goto done;

    /* 4. Client side: a PRIVATE CTX. Never add the self-signed cert to
     *    g_ssl_ctx -- that would make every business request trust it.
     *    set_default_paths() loads the system CA store, which is the path
     *    suspected of touching Keychain / CFNetwork on macOS. */
    cctx = SSL_CTX_new(TLS_client_method());
    if (!cctx) goto done;
    {
        X509_STORE *st = SSL_CTX_get_cert_store(cctx);
        if (!st || X509_STORE_set_default_paths(st) != 1 ||
            X509_STORE_add_cert(st, cert) != 1) goto done;
    }
    SSL_CTX_set_verify(cctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_mode(cctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_AUTO_RETRY);

    /* 5. One in-memory TLS handshake driven by THIS thread only: no socket,
     *    no fork, no pthread. The pre-fork single-thread invariant is the
     *    whole point of this hook. */
    if (BIO_new_bio_pair(&cbio, 0, &sbio, 0) != 1) goto done;
    cli = SSL_new(cctx);
    srv = SSL_new(sctx);
    if (!cli || !srv) goto done;
    /* SSL_set_bio takes ownership; SSL_free releases the BIOs. */
    SSL_set_bio(cli, cbio, cbio); cbio = NULL;
    SSL_set_bio(srv, sbio, sbio); sbio = NULL;
    SSL_set_connect_state(cli);
    SSL_set_accept_state(srv);
    for (int i = 0; i < 64; i++) {
        int cr = SSL_do_handshake(cli);
        int sr = SSL_do_handshake(srv);
        if (cr == 1 && sr == 1) { hs_done = 1; break; }
        /* SSL_get_error() is only meaningful on a non-1 return. */
        int ce = (cr == 1) ? SSL_ERROR_NONE : SSL_get_error(cli, cr);
        int se = (sr == 1) ? SSL_ERROR_NONE : SSL_get_error(srv, sr);
        int cw = (ce == SSL_ERROR_NONE || ce == SSL_ERROR_WANT_READ ||
                  ce == SSL_ERROR_WANT_WRITE);
        int sw = (se == SSL_ERROR_NONE || se == SSL_ERROR_WANT_READ ||
                  se == SSL_ERROR_WANT_WRITE);
        if (!cw && !sw) break;
    }
    HTTP_DBG("warmup: local handshake %s (verify=%ld)\n",
             hs_done ? "completed" : "incomplete",
             hs_done ? (long)SSL_get_verify_result(cli) : -1L);

done:
    if (cli)  SSL_free(cli);
    if (srv)  SSL_free(srv);
    if (cbio) BIO_free(cbio);
    if (sbio) BIO_free(sbio);
    if (cctx) SSL_CTX_free(cctx);
    if (sctx) SSL_CTX_free(sctx);
    if (cert) X509_free(cert);         /* use_certificate up-ref'd it */
    if (key)  EVP_PKEY_free(key);      /* use_PrivateKey up-ref'd it */
    if (pctx) EVP_PKEY_CTX_free(pctx);
    if (!hs_done) ERR_clear_error();   /* never poison the next TLS call */
    return 0;                          /* warmup failures are never fatal */
#else
    return 1;
#endif
}
```

**Landing notes.**

1. Only the **warmup body** changes. `open_conn_impl()` and `tls_attach()`
   stay — the real `http_get()` path still uses both. Delete just the probe
   parsing / `Url` / `Conn` block from the hook.
2. `LUME_TLS_WARMUP_PROBE` should either be removed, or kept strictly under
   `#ifdef LUME_TLS_WARMUP_LEGACY` for throwaway A/B diagnostics. The 4f
   guard added in `fb09217` rejects non-loopback values on the authoritative
   leg, so the two cannot silently coexist.
3. The client verifies against a **private** CTX that trusts only the
   ephemeral cert plus the system CA store. `g_ssl_ctx` is untouched — this
   is the load-bearing security constraint; do not "simplify" by reusing it.
4. Build with `-Werror` on macOS to catch any deprecated-API surprise, and
   check for leaks under ASan (the `done:` path must free every allocation
   on every early exit).
5. The 64-iteration cap is a deadlock guard for the BIO pair. If the debug
   line reports `incomplete`, raise the pair's buffer size (second/fourth
   `BIO_new_bio_pair` args) before anything else — TLS 1.3 handshake records
   can exceed the 17 KB default on some builds.

**Acceptance for this patch** is the same as above: on macOS, 4f passes with
`LUME_REQUIRE_EGRESS=1`, the `[http] warmup: local handshake completed`
line is in the server log, the other ~115 tests stay green, and only then
is `ci.yml:94` allowed to lose `continue-on-error: true`.

### 2026-11-07 — LANDED on macOS, root cause corrected

Everything above this section is kept as the investigation record. The fix
landed today in `lang/builtins_http.c`; `ci.yml` no longer has
`continue-on-error` on the macOS job. **The 11-06 "Keychain read inside the
handshake" theory is wrong on this host.**

**Root cause.** The crash is triggered by the process's *first external
hostname resolution* happening inside a forked worker, not by TLS. Leg 4f's
pure-HTTP control path uses `127.0.0.1`, so nothing resolves a name until
`/probe` calls `http_get("https://example.com/")`. Homebrew OpenSSL 3 reads
CA files from disk, so the in-memory handshake never touches Keychain or
CFNetwork, and `X509_STORE_set_default_paths()` alone does not trip the objc
initialiser either.

**A/B runs** (each on macOS, clean env, `LUME_REQUIRE_EGRESS=1`,
`OBJC_DISABLE_INITIALIZE_FORK_SAFETY` unset):

| warmup body | 4f | objc crashes |
|---|---|---|
| loopback `127.0.0.1:443` probe (old HEAD) | FAIL | 40 |
| reference patch: local BIO-pair handshake only | FAIL | 396 in the focused leg |
| handshake + `getaddrinfo("example.com")` | **PASS** | 0 |
| handshake + `getaddrinfo("localhost")` | FAIL | 396 |
| `getaddrinfo("example.com")` only, no handshake | **PASS** | 0 |

So DNS priming is the load-bearing change; the local handshake is not what
fixes this host. `localhost` is not a substitute — it resolves through
`/etc/hosts` and never initialises the external resolver / system
configuration path.

**What shipped.** `lume_http_warmup()` now, before any fork or thread:

1. One `getaddrinfo()` on an IANA-reserved name, result discarded, nothing
   connected. This is a resolver prime, not an HTTPS egress, so `/probe`
   remains the process's first HTTPS and 4f stays authoritative (unlike the
   old external probe, which made the master the first egress).
2. `ssl_ctx_ready()` for the business CTX — its lazy initialisation also
   touches system frameworks, so it must not first happen in a worker.
3. `LUME_TLS_WARMUP_PROBE` is gone from the C code. `run_all.sh`'s guard
   remains as a tripwire. `WARMUP_NOTE` is a separate one-shot startup
   diagnostic that prints `warmup: dns prime rc=N` by default.

**Simplified 2026-11-08.** The 11-07 build additionally did an ephemeral
RSA key + 60 s self-signed cert + in-memory BIO-pair handshake after the
DNS prime, as insurance for a hypothetical Keychain read path. The A/B
table above already showed `getaddrinfo("example.com")` **alone** passes
with 0 objc crashes, and Homebrew OpenSSL 3 reads CA files from disk, so
that insurance was never load-bearing. Removing it drops ~0.1 s of startup
work (more under ASan), nine short-lived OpenSSL objects and four now-dead
`<openssl/*.h>` includes. `open_conn_impl()` / `tls_attach()` /
`g_ssl_ctx` remain untouched; `builtins_http.c` is `HOST_PINNED`, so no
`make sync-lang` round-trip is involved.

**Verification** (macOS, Apple clang 21, Homebrew OpenSSL 3):

- `make CFLAGS_EXTRA='-D_DARWIN_C_SOURCE -Werror' all` → rc=0, zero
  warnings; the reference patch's OpenSSL 1.1/3.x API choices all compile.
- Full `tests/run_all.sh` with `LUME_REQUIRE_EGRESS=1` and proxies unset →
  **rc=0, 156 legs green, `all tests passed`, 0 objc crashes**, including
  `ok first-request HTTPS egress (warmup before fork)`.
- Startup log contains `warmup: dns prime rc=0` (default build, no debug
  flag). As of 2026-11-08 there is no `local handshake` line — that
  diagnostic went away with the removed BIO-pair handshake.
- No per-call owned allocations remain: the `addrinfo` chain is freed in
  the same block, and the business `SSL_CTX` is static.

**Closed 2026-11-08 — dedicated ASan/LSan harness.** The earlier record
above (macOS `leaks`, 25431 nodes / 0 leaks) only covered the success path
on a live server. `tests/warmup_leakcheck.c` + `make warmup-leakcheck` now
link the same ASan objects but never initialise the interpreter, so LSan can
be enabled in isolation despite `make asan`'s global `detect_leaks=0`
(process-lifetime Type objects). Eight consecutive `lume_http_warmup()`
rounds each get a `__lsan_do_recoverable_leak_check()`. A separate
`-DLUME_WARMUP_SELFTEST_LEAK` negative control must be reported by LSan
before the green result is trusted, so a disabled sanitizer cannot fake a
pass. The target is part of `make asan`, hence CI's ubuntu `asan` job.
LeakSanitizer is Linux-only; on macOS (Apple clang) the same rounds still
run under ASan/UBSan, just without the leak assertions.

**2026-11-08 update.** The `done:` label and its nine owned OpenSSL objects
no longer exist. The harness now checks a smaller surface: the `addrinfo`
chain returned by `getaddrinfo()` must be freed every round, and the static
business `SSL_CTX` must not be rebuilt per round. Eight repetitions make a
per-call leak visible as a growing allocation. The negative control is
unchanged and still the guard against a disabled LSan faking a pass.

Verified: macOS 8 rounds green; Linux Ubuntu 24.04 container with GCC —
negative control reported, 8 rounds `no leaks`, and the full CI-equivalent
`make -j4 asan` passed. All local gates (`check-sync`, `check-parity`,
`check-version`, `check-baseline`, `privacycheck`, `treecheck`, `check`)
stayed green.

**Confirmed on a remote runner (2026-11-07).** The now hard-blocking macOS
job passed on GitHub Actions after push: lume CI run `42381803754` @
`ab90005`, job `macos` (macos-latest) completed `success`, and its
`Build + full test suite` step logged
`ok first-request HTTPS egress (warmup before fork)` followed by
`all tests passed`. The same job checked out the `agent-httpd` submodule at
`cca43ec`, so the bumped gitlink is fetchable from a clean clone.
`continue-on-error` stays removed.

### 2026-11-08 — fatal-warning gate (`-Werror`): deferred in the morning, WIRED later that day

Morning evaluation (kept for the record): for the Ubuntu/GCC CI lane this
was deliberately NOT wired in. On Ubuntu 24.04 GCC with
`-D_FORTIFY_SOURCE=2`, a clean full build turned two classes of diagnostics
into hard failures: `-Wformat-truncation=` on bounded `snprintf` into fixed
`why` buffers, and `-Wdangling-pointer=` in `open_conn_impl()` (then at
`lang/builtins_http.c:721-748`, `chost` pointing at the stack buffer
`phost`). Suppressing them one at a time would have hidden a real
pointer-lifetime question; an opt-in `WERROR=1` prototype was reverted
rather than left half-working.

**Closed later on 2026-11-08 — see the section at the end of this file.**
The dangling pointer was a real use-after-scope, not a noisy diagnostic;
it and every format-truncation site were fixed in code (no `-Wno-*`), and
`WERROR=1` is now a real gate on the Ubuntu/GCC lane.

### 2026-11-08 — CI: `macos-14` label is retired (fixed)

GitHub retired the macOS 14 runner image on 2026-11-02 (changelog
2026-10-01); `macos-14`, `macos-14-large` and `macos-14-xlarge` are gone,
with capacity reductions in the preceding weeks. That explains the
release/ci jobs being `cancelled` within one second, with an empty step
list and no runner name, since 2026-11-06 — they never got a machine.

`actions/runner-images` README confirms `macos-15` is the arm64 GA label
(`macos-15`, `macos-15-xlarge`), while `macos-15-intel` / `-large` are x64,
and that all macOS 14 images retired from GitHub Actions on 2026-11-02.
So switching does not change the `lume-darwin-arm64` architecture: CI's
`macos-latest` job ran on `macos-26-arm64` (CI run `42381803754`), and the
last successful `macos-14` build ran on `macos-14-arm64` (Release run
`42030553700`).

Fixed 2026-11-08 by pinning `macos-15` (not `macos-latest`: the 2026-10
`ubuntu-latest` migration already cost this repo a week of red test jobs):
`lume/.github/workflows/release.yml`, `lume-core/.github/workflows/release.yml`,
`lume-core/.github/workflows/ci.yml`, plus the `lume` `ci.yml` comment that
describes the release lane. CI jobs on `macos-latest` were left alone — they
are the hard-blocking 4f lane and still pass on `macos-26-arm64`.

Verified after push on 2026-11-08: lume CI `42442792829` and Release
`42442792852`, lume-core ci `42442723452` and Release `42442723365` are all
`success`, with no `cancelled` job. `build (macos-15, lume-darwin-arm64)`
had a named runner and 25 real steps, and logged `Image: macos-15-arm64`,
`Darwin arm64`, `bin/lume: Mach-O 64-bit executable arm64` — the product
architecture is unchanged. lume-core's `release-build (macos-15)` really
compiled (`arm64-apple-darwin24.6.0`), not an empty successful shell.

The same CI round also green-lit the guards added in this batch:
`ok   warmup LSan negative control: injected leak reported`,
`ok   warmup leakcheck: 8 rounds, no leaks`, and the privacy guard's
`scope=HEAD (HEAD + index + non-ignored worktree)` with `self-test=4/4`.

### 2026-11-08 — P2: warmup slimmed, submodule fallback fixed

Host `lang/builtins_http.c`: `lume_http_warmup()` dropped the 11-07
ephemeral RSA key + 60 s self-signed cert + in-memory BIO-pair handshake.
The A/B table above already showed `getaddrinfo("example.com")` alone
fixes the libobjc fork guard, and Homebrew OpenSSL 3 reads CA files from
disk, so that insurance was never load-bearing. Warmup is now DNS prime +
`ssl_ctx_ready()` only; four now-dead OpenSSL includes went with it.
`WARMUP_NOTE` still prints `warmup: dns prime rc=N`, and
`local handshake completed` is no longer an acceptance line. The 4f
comment, CI job comment, LANDED section and `warmup_leakcheck.c` rationale
were all updated to match — no stale "we still handshake" text left.

Submodule `agent-httpd/src/core/framework.c`: the weak
`lume_http_warmup()` fallback (used when agent-httpd is built standalone)
used to `fork()` a `curl https://example.com --max-time 3` child. That was
a startup HTTPS egress, could block 3 s, and — decisively — performed the
TLS/resolver initialisation in an `exec`'d child, so the parent had still
never resolved an external name. It now does one `getaddrinfo()` +
`freeaddrinfo()` in the master, no fork, no egress, no timeout. `fcntl.h`
was dead after removing the child's `/dev/null` redirect; `netdb.h` added.

Verification (all local, before push): macOS full `tests/run_all.sh` with
proxies set and `LUME_REQUIRE_EGRESS=1` → rc=0, `all tests passed`,
`ok first-request HTTPS egress`; macOS `make warmup-leakcheck` → 8 ×
`[http] warmup: dns prime rc=0`, `8 rounds, no leaks`, zero
`local handshake` lines; Linux Ubuntu 24.04 / GCC `make -j4 asan` →
rc=0, LSan negative control reported, 8 rounds no leaks, all ASan/UBSan
passed (the 10 `warning:` lines are the known `-Wformat-truncation`
baseline plus the negative control's intentional unused `malloc` — no
new warnings from the removed includes); standalone agent-httpd
`make test` → `PASS=169 FAIL=0`; all seven local gates PASS.

Confirmed on remote runners after push (2026-11-08): lume CI
`42446280766` (5/5 success) — the hard-blocking macOS job logged
`ok first-request HTTPS egress (warmup before fork)` and `all tests
passed`, so removing the handshake did not regress 4f; the ubuntu `asan`
job logged `ok warmup LSan negative control: injected leak reported`,
8 × `[http] warmup: dns prime rc=0`, `8 rounds, no leaks` and no
`local handshake` line. lume Release `42446280780` (including
`build (macos-15, lume-darwin-arm64)`) and Windows `42446280733` are
success. agent-httpd CI `42446203897` is 6/6 success: its macOS job
recompiled `framework.c` under `-Werror`, and the Ubuntu full smoke
passed `PASS=157 FAIL=0`. Gitlink four trees all `dbb1272`.

### 2026-11-08 (noon) — `phost` was a real bug; the fatal-warning gate is now live

A read-only review found that the diagnostic deferred in the morning was not
a false positive. In both `open_conn_impl()` (host `lang/builtins_http.c`)
and `open_conn()` (core `src/builtins_http.c`), `char phost[512]` was declared
*inside* `if (proxy && proxy[0])`, but `chost = phost` was then used after
that block: `getaddrinfo(chost, ...)`, the connect debug path, every `c->why`
failure reason and the CONNECT error branches. That is a read of a dead stack
buffer — GCC's `-Wdangling-pointer` pointed at a genuine use-after-scope,
not a formatting nit. Both trees had the identical shape. Fixed by lifting
`phost` to function scope with a comment saying why; `builtins_http.c` is
host-owned, so the core change was hand-merged and `lang/PIN.lag` refreshed
with `sync-lang.py accept-lag`.

The second class was a host-only lag: core already had `set_why()`, a
`va_list` wrapper that lets libc do the NUL truncation so GCC cannot
statically prove a truncation, while the host still used eight direct
`snprintf(c->why /* 160B */, ...)` calls. Hand-merged `set_why()` and
switched all eight. Three more `-Wformat-truncation` sites surfaced once
that was gone; all fixed in code, no `-Wno-*` suppression:

| site | fix |
|---|---|
| `lang/bridge.c:80` (`creds[512]` into a 64-byte user buffer) | explicit `strlen`/`memcpy` truncation instead of `snprintf`'s implicit one |
| `lang/iquest.c` (4 `err` call sites) | `set_err()`, the same `va_list` shape as `set_why()` |
| `tests/smoke.c:150` (`dir[256]` used twice in `cmd[512]`) | `cmd` widened to 768 so two 255-byte `dir`s plus the literal fit |

`Makefile` now has `WERROR ?= 0` with `CFLAGS += -Werror`, and the
ubuntu-24.04 CI lane runs `make -j4 WERROR=1 test` instead of
`make -j4 test`. Scope is deliberately the GCC lane only: clang reports a
different diagnostic set, and the ASan negative control keeps an intentional
unused `malloc()` result that must not become a build failure. Release,
Windows, ASan and macOS lanes are unchanged.

Verification (local, 2026-11-08): before the last three fixes, a clean
Ubuntu 24.04 / GCC 13.3.0 source copy failed `make -j4 CC="cc -Werror" all`
with exactly `bridge.c:80` and `iquest.c:365`; after them, a clean copy
builds `make -j4 WERROR=1 all tests/smoke-bin tests/tools-bin` with rc=0
and zero diagnostics. macOS clang `make -j4 -B WERROR=1 all` is also rc=0
with zero diagnostics. The full macOS `make test` with proxies set and
`LUME_REQUIRE_EGRESS=1` stays green, and all seven local gates pass. The
CI workflow change itself needs the next push for remote confirmation.

## 2026-11-08 — post-fix verification and compliance closeout

### `WERROR=1 test` is verified end-to-end locally

The note above left the workflow change itself unproven. Re-ran the exact
CI command on a forced full rebuild: `LUME_REQUIRE_EGRESS=1 make -j4 -B
WERROR=1 test` → rc=0, zero compiler diagnostics, `all tests passed`, `ok
first-request HTTPS egress (warmup before fork)`. `tests/run_all.sh`
issues exactly one build call — the bare `make` on line 26, the default
`all` target, already built by the outer `test` prerequisite — so it opens
no compilation path outside `all`, `smoke-bin` and `tools-bin`. `ui` is a
pnpm frontend build and never consumes `CFLAGS`.

Together with the clean Ubuntu 24.04 / GCC 13.3 copy build, the only
untested part of this lane is the GitHub runner — remote confirmation still
needs the next push.

### NOTICE corrected: not every `lang/compat/` tree is vendored

The old text said all the other compatibility implementations "retain their
own upstream copyright and license notices". Their headers disagree:
`sha256/sha256.c` is a fresh public-domain / CC0-style implementation with
no upstream file copied, and `ed25519/ed25519.c` is a from-scratch pure-C
RFC 8032 implementation. `lang/compat/regex/` is the only vendored
third-party tree, and its BSD attribution stays. NOTICE now separates
original code, the `agent-httpd` submodule and system libraries. The
`sha512/` header was not read this round (an environment approval timed
out); under the no-retry / no-inference rule it is not claimed either way,
and its header remains authoritative.

### Deliberately left alone

- `lume_http_warmup()` primes DNS with a blocking `getaddrinfo()` and has no
  timeout. A portable timeout needs a worker thread or SIGALRM around a libc
  resolver call — more machinery than a rare slow-DNS startup warrants.
  Recorded here so it stops resurfacing as an open TODO.
- `agent-httpd/TODO.md` P2 security review re-read: zero unchecked items
  remain (same-origin gate, Secure cookie, login limiter, roles/audit,
  ALLOWED_HOSTS, session entropy — all Done 2026-11-04).
- P14 `spawn()` landed in lume-core (gate + rooted exec), the emitter
  decision is closed, and host integration is deliberately closed as a
  permanent core-only boundary (see the P14 mirror section below). Only
  **streaming / long-lived reads** remains a separate project. Historical
  privacy residue stays out of scope per the user's "不用管历史" instruction.

## 2026-11-08 — remote verification after push

Pushed `lume-core 57b74b8..ce7e590` and `lume 3a271a1..0efad3a`.
`agent-httpd` was unchanged at `dbb1272`, so no gitlink bump was needed;
all four trees still point at `dbb1272`.

| run | conclusion |
|---|---|
| lume CI `42454199642` | 5/5 success (macos, test, asan, privacy, check-sync) |
| lume Release `42454199719` | success, including the macos-15 darwin-arm64 lane |
| lume Windows `42454199658` | success |
| lume-core ci `42454195295` | 10/10 success |
| lume-core Release `42454195304` / Windows `42454195297` | success / success |

The ubuntu-24.04 `test` job (`128649624114`) ran exactly `make -j4 WERROR=1
test`. All 32 host `lang/*.c` compiles carry `-Werror` (located via the
`-DLUME_VERSION` marker in the job log); compiler diagnostics = 0. The only
two `warning:` matches in the whole job come from setup-node's `punycode`
DeprecationWarning, not from the build. `all tests passed` and `ok
first-request HTTPS egress (warmup before fork)` are both present. The asan
job still reports `ok warmup LSan negative control: injected leak reported`
and `8 rounds, no leaks`; privacy reports `self-test=4/4`.

Wording correction to the local claim of "no `-Wno-*` suppression": that
meant **no new suppression was added for these fixes**. The Linux-only
`-Wno-stringop-truncation` at `Makefile:82` predates this work (`b92c2a1`)
and stays, with its own comment: under `_FORTIFY_SOURCE=2` it covers a safe
`strncpy` into a zeroed 64-byte buffer with a guaranteed trailing NUL. It is
unrelated to the two diagnostic classes fixed here (`-Wformat-truncation`
and `-Wdangling-pointer`). The `agent-httpd` submodule carries its own two
exemptions in its own build.

## P14 (mirror) — `spawn` stays a lume-core-only capability (decided 2026-11-08)

`lume-core` P14 landed `spawn` / `proc_read` / `proc_wait` / `proc_kill`
behind an opt-in gate (`--spawn` / `LUME_SPAWN=1` **plus** a mandatory
`LUME_SPAWN_ALLOW`), realpath-checked `LUME_SPAWN_ROOT` directories and a
filtered `env`. This host does **not** adopt them. The decision is recorded
here so a later `make sync-lang` pass does not mistake the accepted
`PIN.lag` delta for an unfinished merge.

**Decision (a): interpreter-only, permanently.** A compiled lume binary has
no `VM` (the gate lives in `VM.spawn_enabled` / `spawn_allow` /
`spawn_root`) and `src/rt.c` has no `getenv` gate of its own.
`codegen_call.c` and `llvm_codegen_internal.h` have no spawn row in their
`BUILTINS[]` tables; `--compile-text` on a `spawn()` call exits 1 with
"call to unknown function 'spawn'". That is the safe default, so the native
emitters stay that way. lume-core's `make check` now runs
`tests/spawn-native-gate.sh` on every CI lane: it fails if a spawn token
appears in `rt.c` or either emitter, asserts the native rejection, and
(POSIX) asserts the interpreter can still spawn when gated. Two replayed
negatives (injected token / blinded matcher) keep it from going falsely
green.

**Why the host does not merge the chain.** `lang/`'s seven host_owned files
(`typecheck.c`, `interp.c`, `builtins.c`, `lume.h`, `main.c`,
`builtins_internal.h`, `builtins.h`) carry upstream-only lines for the spawn
registrations, accepted into `lang/PIN.lag` on 2026-11-08. Hand-merging
them would put a process primitive in a server binary with **no gate** —
the exact failure mode the P14 "gate first" rule exists to prevent. Those
lag entries are intentionally retained, not a TODO.

**Bookkeeping that changed with this decision (no product C edits here):**

- `scripts/sync-lang.py`: `builtins_spawn.c` moved from a transient
  `CORE_ONLY_NAMES.add(...)` with a "REVISIT" note into the permanent set,
  with the conditions for re-admission spelled out.
- `tests/builtin-parity.allow`: the four spawn names are now marked
  PERMANENT rather than TRANSITIONAL; the list is no longer a promise to
  port them.
- `lang/PIN.lag`: left numerically unchanged on purpose. Re-run
  `sync-lang.py accept-lag` only if the accepted upstream-only deltas
  actually move.

**If a future host really needs child processes:** land an equivalent
fail-closed gate in `lang/` first (opt-in + mandatory allowlist + realpath
roots), then sync `builtins_spawn.c` and the registration chain, remove the
`CORE_ONLY_NAMES` entry, drop the four parity-allow names, refresh
`PIN.lag`, and replace this section. Do not start by copying the builtin.

Remaining P14 work (lume-core side, not this repo): wiring the streamed
reads into `task.delegate`'s `observe_delegate` — the `proc_read`
primitive itself landed as `649f2df` (2026-11-08; 8 assertions green on
macOS/clang and a clean gcc:13-bookworm copy), which proved a live child
is tailed rather than collected at exit. P16 (0.12 lib compatibility,
static ETag/304) is **closed** — `33e68e3` and `5a904e8` landed
2026-11-08. This note originally said P16 was "independent and still
open" and P14 streaming was entirely outstanding; corrected 2026-11-09.

## 2026-11-08 — P17 (mirror) — `http_stream` stays a lume-core-only capability (decided 2026-11-08)

`lume-core` P17 landed `http_stream_open` / `http_stream_read` /
`http_stream_close` as an inbound SSE client — the deferred half of P14's
streaming work: a generation-tagged slot handle table plus an incremental
SSE line parser, reusing the bare-socket HTTP/1.1 transport (`url_parse`,
the `host_blocked` SSRF gate, `open_conn` proxy+TLS, `read_head`) and the
shared `vm->no_net` gate. This host does **not** adopt them. The decision
is recorded here so a later `make sync-lang` pass does not mistake the
accepted `PIN.lag` delta for an unfinished merge.

**Decision (a): interpreter-only, permanently.** Like `spawn`, the
registrations live only in the interpreter (`src/interp.c` / `typecheck.c`
/ `builtins.c` / `builtins_internal.h` / `builtins.h`) and never reach
`codegen_call.c` or `llvm_codegen_internal.h`'s `BUILTINS[]`;
`--compile-text` on an `http_stream_open()` call exits 1 with "call to
unknown function 'http_stream_open'". The native emitters stay that way
(and `rt.c` has no stream helper), enforced by lume-core's `make check` ->
`tests/stream-native-gate.sh`. That is the safe default: a compiled lume
binary simply has no inbound-SSE primitive. `http_stream_read` returns one
parsed SSE event per call (`{ok, event, data, id, eof, err}`); v1 does not
auto-reconnect — the last event id is handed back so a script can reconnect
itself.

**Why the host does not merge the chain.** `lang/`'s host_owned files
(`typecheck.c`, `interp.c`, `builtins.c`, `builtins_internal.h`,
`builtins.h`, plus `builtins_http.c` which is `HOST_PINNED`) carry
upstream-only lines for the http_stream registrations, accepted into
`lang/PIN.lag` on 2026-11-08. Hand-merging them would put an UNGATED
network primitive (the `vm->no_net` gate lives in the VM, which a compiled
binary has none of) into a server binary — the exact failure mode the P14
"gate first" rule exists to prevent. Those lag entries are intentionally
retained, not a TODO.

**Bookkeeping that changed with this decision (no product C edits here):**

- `scripts/sync-lang.py`: no new `CORE_ONLY_NAMES` entry was needed —
  `http_stream_*` is appended to `src/builtins_http.c`, which is already
  `HOST_PINNED`, not a separate core-only file like `builtins_spawn.c`.
- `tests/builtin-parity.allow`: the three http_stream names are now in
  `[core-only]`, marked PERMANENT (see the header note) rather than
  TRANSITIONAL; the list is no longer a promise to port them.
- `lang/PIN.lag`: refreshed via `sync-lang.py accept-lag` on 2026-11-08 to
  record the accepted upstream-only delta for the five host_owned files
  that grew (interp.c, typecheck.c, builtins.c, builtins_internal.h,
  builtins.h). Re-run accept-lag only if those deltas actually move again.

**If a future host really needs inbound SSE:** land an equivalent
fail-closed gate in `lang/` first (the `vm->no_net` policy, or its
compiled-binary equivalent), then sync the registration chain, remove the
three parity-allow names, refresh `PIN.lag`, and replace this section. Do
not start by copying the builtin.

Remaining P17 work (lume-core side, this repo-irrelevant): **closed
2026-11-09**. The Windows `LUME_HAS_HTTP=0` lane is green locally
(`make -j4 check`, MinGW gcc 14.2, both native gates) and in CI (run
`42553136470`); the Linux dash `/bin/sh` leg is green in CI (run
`42553136496`, `test (ubuntu-latest)` -> `make test` ->
`==> http_stream 流式单测通过 (stream + locked)`), with macOS green too.
The literal `gcc:13-bookworm` container has no CI lane and could not be
run here (no docker; WSL launch blocked by local security policy) —
recorded in `lume-core/TODO.md` P17 as a coverage note, not a
correctness gap. Both lanes are independent of the host mirror; the
Windows lane did surface one pre-existing diagnostic gap (unreachable
`native_http_unavailable` stubs), filed in that same section.
