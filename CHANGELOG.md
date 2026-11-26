# Changelog

All notable changes to Lume are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and versions aim for
[SemVer](https://semver.org/).

## [0.15.0] - 2026-11-26

### Added

- **梯度提升回归内建：`gbdt_fit` / `gbdt_predict`。** 零依赖的平方误差 GBDT 回归树，
  `.lume` 管线无需 sklearn 即可训练/打分信号模型。`gbdt_fit(X, y, rounds, depth,
  lr, min_leaf)` 返回 GC 持有的 model map（前序扁平树列表），`gbdt_predict(model, X)`
  对行矩阵打分。以自包含 `lang/builtins_math_gbdt.inc` 实现（gb_ 前缀 helper，
  不依赖任一树的本地 helper 名），四处登记（builtins.h / builtins_math.c /
  interp.c / typecheck.c）；与 lume-core 双树镜像，两份字节一致。解释器专用
  （同 `server{}`），本版无原生/LLVM 后端 parity、无分类、无缺失值处理。
  两棵树的二进制均在 `y = 2*x1 - x2` 冒烟测试上验证通过。
- **同步规则更新**：`lang/PIN.lag` 接受 GBDT 引入的 4 个 host-owned 文件 delta
  （builtins.h / builtins_math.c / interp.c / typecheck.c），`make check-sync`
  恢复通过。

## [0.14.2] - 2026-11-21

### Fixed

- **`stringify()`/`str()`/`print()` 浮点精度截断。** JSON/字符串序列化用
  `%g` 只保留 6 位有效数字，`cost_usd`/`error_rate` 等被截断
  （`2.141205` → `2.1412`）。新增 `lume_fmt_double()`（host-pinned
  `value.c`）：尝试 `%.*g` 从 1 到 17 位，取第一个能 `strtod` round-trip 的
  （Go `strconv 'g' -1` / `json.Marshal` 语义）；整数值走 `%lld`（避免
  `110.0` → `1.1e+02`）。常见值仍紧凑（`0.1` → `0.1`），高精度保留完整
  （`13.979032000000005` 原样）。替换 `json_append_value`/`vdom`/`print`/
  `str` 的 4 处 `%g`。这是 host 有意与 lume-core 的差异（`PIN.lag`
  accept-lag 记录）。

## [0.14.1] - 2026-11-21

### Added

- **`proxy_stream` 流式透传信封（HTTP 路由）。** 路由返回
  `{ proxy_stream: { url, method?, headers?, body?, timeout?, allow_private? } }`
  时，把上游 HTTP 响应**逐块**转发给客户端（读一块写一块，不缓冲整响应）：
  `builtins_http.c` 新增 `lume_http_relay()`（复用连接逻辑，按
  Content-Length / chunked / EOF 转发），`bridge.c` 识别信封并装
  `stream_fn`。响应透传状态码 + Content-Type + 安全头，不跟随重定向。
  与 `{ sse: [...] }` 预生成数组不同，这是**真流式**（SSE/长响应首字节
  更快、内存 O(块)）。见 `docs/LUME.md` 路由信封一节。

### Fixed

- **Built-in visitor stats: retention was never enforced.** `cleanup_visits`
  set `visit_last_cleanup` only *after* a successful delete, while the
  trigger guarded on `visit_last_cleanup > 0` — so on a fresh process the
  condition was false and the cleanup call was unreachable. `VISIT_RETENTION_DAYS`
  (default 90) therefore never deleted anything and the DB only grew. The
  timestamp is now stamped before the delete (a failed attempt is rate-limited
  to one per 24h instead of retrying on every request), `0` = "never cleaned"
  triggers one pass on the first visit, and `VISIT_RETENTION_DAYS<=0` skips it
  entirely.

- **Built-in visitor stats: 172.20–172.29 were counted as public.** The
  private-address check only enumerated `172.16–19` and `172.30–31`, so ten
  `/16`s inside `172.16.0.0/12` were treated as real visitors and inflated
  `total`/`uv`. Detection is now a numeric octet test over the whole `/12`
  (`is_172_private()`), shared with the new `ip_is_local()` helper.

- **Built-in visitor stats: `X-Forwarded-For` was trusted unconditionally.**
  The header is client-controlled, so anyone could spoof an IP to slip past
  `VISIT_EXCLUDE_IPS` (their own visits counted again) or inflate `uv`. New
  `VISIT_TRUST_XFF` policy: `auto` (default) honours XFF only when the peer
  itself is loopback/private — i.e. the request really came from a local
  reverse proxy; `1`/`0` force it on/off. Deployments exposed directly should
  set `0`.

- **Built-in visitor stats: duplicate routes shadow silently.** The module
  registers before the entry script and `agenthttpd_route()` does not check
  for duplicates (dispatch is first-registered-wins), so a project that keeps
  its own `/api/visit` is not rejected — the built-in handler quietly takes
  over and the project's own fields/dedup rules stop running. Documented as
  a hard migration step in `builtin_visits.c` and the config header.

### Changed

Privacy hardening for the built-in visitor stats (audited against the
minimum-necessity principle; IP + UA + path are personal information):

- **`path` no longer stores query strings and is capped at 200 chars.**
  Previously `path` was written verbatim, so a site sending `location.href`
  persisted `?email=…&q=…` into the analytics DB (verified against a live
  server). It is now cut at `?`/`#`, must be an in-site path (`/`-prefixed),
  and truncated to 200; `visitor` is capped at 64. The summary endpoint
  re-sanitizes on read so rows written before this change don't keep leaking
  query strings. `referrer` was already stripped.

- **`POST /api/visit` rejects malformed bodies instead of storing a phantom
  row.** A body that failed `json()` (or lacked `path`) used to fall back to
  defaults, insert `anon|/`, and answer `recorded:true` — a public, unauthenticated
  endpoint that anyone could use to inflate the table. Now it returns
  `{ok:null,err:…}` and writes nothing.

- **Loopback visits are dropped at record time.** The header claimed
  "self-visits never land on disk", but with no exclude list a `127.0.0.1`
  visit was stored (it was merely filtered during aggregation). Recording now
  drops loopback alongside `VISIT_EXCLUDE_IPS`. Note the interaction documented
  in the header: `VISIT_TRUST_XFF=0` plus a local reverse proxy leaves only
  the loopback peer, so stats would be empty — keep the default `auto` there.

- **`recent` masks client IPs by default** (IPv4 keeps its /24, IPv6 its first
  two groups) and only returns raw addresses when `VISIT_SHOW_RAW_IP=1`. The
  PIN is the only gate on this endpoint, and a PIN in a query string lands in
  upstream nginx/CDN access logs, so the blast radius of a leaked PIN is now
  bounded.

- **Per-IP throttle on the public beacon**: `VISIT_RATE_LIMIT_PER_MIN`
  (default 20, `0` disables). `visitor` is client-supplied, so the existing
  60s dedupe could be bypassed by changing it; the IP cap closes that path.

- **`Authorization: Bearer` is the documented way to pass `VISIT_PIN`.** The
  `?pin=` form still works for backward compatibility with existing dashboards
  (the contract is shared with nsbp / specpulse / tnsbp / lume-crm /
  lume-fastapi-web), but new callers should use the header.

- **Default retention lowered from 90 to 30 days.** Site analytics does not
  need a quarter of IP-level history. This applies to deployments that never
  set `VISIT_RETENTION_DAYS`: on their first beacon after upgrading, the
  cleanup prunes everything older than 30 days. Back up `visits.db` first if
  that history matters; set the variable explicitly to keep a longer window.

### Added

- **Optional IP de-identification: `VISIT_HASH_IP=1` + `VISIT_IP_SALT`.**
  With a salt configured, the table stores `sha256(salt + ":" + ip)` truncated
  to 16 hex chars instead of the address, and `uv` counts distinct hashes.
  The salt is mandatory on purpose — IPv4 is small enough that an unsalted
  digest is reversible by enumeration, i.e. fake anonymisation; without a salt
  the switch stays off and storage remains plaintext (documented fallback).
  Consequences, also documented: private/loopback peers are dropped **before**
  hashing (they can no longer be classified in the summary), and neither
  `VISIT_EXCLUDE_IPS` matching nor raw-IP lookback works on hashed rows.

## [0.13.1] - 2026-11-15

### Added

- **Startup `.env` loader.** `main.c` now reads `$LUME_ENV_FILE` (or
  `<script-dir>/.env` when unset) before the script runs and seeds `env()`
  with it, so per-deployment configuration (PINs, exclude lists, retention)
  can live in a gitignored file instead of scattered `run_args`. Set
  `LUME_NO_ENV=1` to disable; existing process environment variables are
  never overwritten (`setenv(..., 0)`). Plain `KEY=value`, quoted values,
  `export`, inline comments, `#` inside quotes, blank lines, and CRLF
  line endings are all handled.

- **Built-in visitor stats (`LUME_VISITS=1`).** The engine now embeds a
  `builtin://visits` DSL module (SQLite-backed) and auto-registers it before
  the entry script when `LUME_VISITS` is set:
  - `POST /api/visit` — record a visit (body `{path,referrer,visitor,ua,wd}`);
    drops the request's own IP / exclude-list IPs up front (privacy), and
    dedupes the same visitor within 60s.
  - `GET /api/visits?pin=` — `{total,today,uv,days,recent}` summary, gated
    by PIN (fail-closed: unset PIN or wrong PIN returns 403). Filters bots,
    private/loopback IPs and exclude-list IPs during aggregation.

  Configuration via `env()`/`.env`: `VISIT_PIN`, `VISIT_EXCLUDE_IPS`
  (comma-separated exact IPs or CIDRs), `VISIT_RETENTION_DAYS` (default 90),
  `VISIT_DB` (default `.data/visits.db`). The contract matches the
  per-project implementations (nsbp / specpulse / tnsbp / lume-crm /
  lume-fastapi-web), so an opted-in project can drop its own visits routes
  and gain the built-in ones. Unset `LUME_VISITS` leaves existing projects
  untouched (routes not registered).

  Internally `loader.c` grew an in-memory source loader
  (`load_module_source()`) and a pre-entry injection point; `loader.c` is
  now host-pinned in `scripts/sync-lang.py` (the hook calls host-only
  `builtin_visits.c` symbols).

## [0.13.0] - 2026-11-14

### Added

- **`http_*` returns `headers`.** The request builtins used to hand back only
  `{ok, status, body, err}`, so a script could send a `Cookie` header but never
  read the `Set-Cookie` that issued it. That made a whole class of protocols
  impossible to write in `.lume` — the common case being a router admin panel
  that hands out a session cookie on `GET /html/index.html` and then requires it
  for login and every request after. Such integrations had to stay in a
  sidecar in another language.

  Keys are lowercased because header names are case-insensitive while map keys
  are not; without normalisation the caller has to guess `"Set-Cookie"` vs
  `"set-cookie"`, and guessing wrong looks like "the header isn't there".
  Repeated headers are joined with `", "` (matching curl's rendering) rather
  than keeping only the last, which would silently drop a session cookie.

## [0.12.2] - 2026-11-01

### Fixed

- **Restored the linux-x64 release artifact.** v0.12.1 shipped without
  `lume-linux-x64-static.tar.gz`: the release workflow's `ubuntu-latest` job
  failed at `make`, while `ubuntu-24.04-arm` and `macos-14` both succeeded.
  Two causes, both fixed after the tag was cut:

  1. `agent-httpd` submodule pointer was at `f4d1cab`, where
     `parse_range` assigns `char *` from `strchr(const char *)` — GCC 15
     (the `ubuntu-latest` image) rejects that. Fixed in `e62c346`.
  2. Static link order: compression / jitterentropy libs must follow
     `-lcrypto`, and the jitterentropy dev package name differs per runner
     image (`24.04-arm` has none at all).

  Why this mattered beyond one platform: v0.12.1 is what every sibling project
  pins. Anything built with `--platform linux/amd64` — which includes every
  `docker buildx build` for a x64 target — got a 404 on the download and could
  not build at all.

### Changed

- `Dockerfile.scanner` runtime stage uses `alpine:3.20` instead of
  `debian:bookworm-slim` (116 MB → 19 MB). Only `curl`, CA certs and a
  non-root user are needed; the rest was Debian's base system.

## [0.12.0] - 2026-10-31

### Changed

- `LUME_VERSION` 0.11.0 → 0.12.0 (binary self-report 单一来源).
- Bumped vendored `agent-httpd` gitlink to v0.12.0 (`f4d1cab`) — 含 MCP
  tool-registry HTTP 端点、路由级 SSE、Referrer-Policy 放宽、以及
  `tools/call` 的 fail-closed 防御. **部署注意:** 改版本宏后须
  `rm -f build/*.o bin/lume` 再重建, 否则二进制自报旧 0.11.0.

## [0.11.0] - 2026-10-25

### Added

- Request map (`lang/bridge.c`): `origin` and `referer` fields, plus a
  generic `headers` map built from agent-httpd's new unknown-header
  capture (submodule `08884f0`) — node `serve.mts`-style Origin checks
  are now portable.
- Response envelope: optional `headers` map (RFC 9110 token names,
  CR/LF-free values, enforced with a loud 500) appended as response
  headers; a name matching a framework default security header
  suppresses that default (case-insensitive), so per-route
  `X-Frame-Options` / CSP overrides are expressible.
- `server { csp = "..." }`: server-wide Content-Security-Policy
  replacing the built-in default (CR/LF rejected at ingest). Backed by
  `agenthttpd_config.csp` (submodule `08884f0`) and mirrored on the
  lume-core native serve path, so both serve paths behave alike.

### Changed

- Outbound HTTP timeout cap 60s → 600s (`lang/builtins_http.c`,
  mirrored from lume-core `80ac91a`); `opts.timeout` still tightens
  per call. Slow upstreams (300s refresh proxies) no longer 502.

### Fixed

- `lang/` synced to lume-core `v0.10.1` (`95abfbb`): `lang/loader.c` gains
  the per-run latch reset (vm->error / error_msg / stack_count cleared at
  `loader_run` entry) — an embedded host calling `loader_run` twice on one
  VM handle used to silently no-op the second exec after an aborted one.
  PIN bumped `v0.9.3-53` → `v0.10.1`.
- agent-httpd gitlink bumped to `3ff62f9` (ubuntu-smoke CI job).
- CI: `check-sync` upgraded to real drift detection — the job now checks
  out `erishen/lume-core` next to the host checkout and runs
  `sync-lang.py check` against the pinned commit, catching upstream
  changes to synced files (the loader.c drift above) that the committed-md5
  leg alone could not.

- Hand-merged from lume-core: cast accessors answer in the cast's own
  type (`lang/builtins.c` — the fallback default converts over the same
  bridge the direct cast builds, so `int(m, "k", true)` answers `1`, not
  `true`), and `/` on two integers types as float in the checker
  (`lang/typecheck_expr.c`) — the interpreter's long-standing behavior
  the static type used to contradict. Closes the five-run CI red on
  lume-core `ci.yml` (accessor compile refusal + the integer-division
  bit-pattern print it was hiding).

- **Family version alignment to lume-core `v0.11.0`.** `LUME_VERSION`
  bumped `0.8.1 → 0.11.0` so the host's self-reported version matches the
  coordinated lume-core `v0.11.0` release. The agent-httpd submodule pin
  (`3ff62f9`) is exactly the agent-httpd `v0.11.0` commit, keeping the
  mirror debt clean. `lang/PIN` is intentionally left at lume-core
  `06dcfec` (accept-lag discipline — no `sync-lang`); the vendored
  `lang/` tree tracks core one commit behind `v0.11.0`.

## [0.8.0] - 2026-10-22

### Added

- Compile-time builtin arity contract hand-merged from core (`1a2aa80` +
  `172fe01` current tree): the `LUME_BUILTIN_SIG` signature table
  (exact / variadic / min-max) validates builtin call shapes at compile
  time — `bool`/`json`/`stringify` are 1-arg, the `str/int/float(map, key)`
  three-argument accessor forms included. `type_func` split into
  `type_func_arity`; the host smoke suite covers the exact/variadic/
  mismatch rejections.
- `make deepcheck` privacy gate (`check-tree-privacy.sh --history`):
  batch-all-objects blob set vs `rev-list --objects --all` reachability —
  unreachable (loose) blobs must be zero, HEAD must survive. Not part of
  the `make test` aggregate; run on demand before releases.
- SSR relay `react_prefix` (`87e284f`): configurable relay prefix +
  reserved-path exclusion, closing the `bridge.c` config-field contract
  with agent-httpd `6bab252` — a fresh `clone --recurse-submodules` now
  reproduces the React SSR relay end to end.

### Fixed

- P9 for-in inline-temp GC root loss, hand-merged from core `475e099`
  (`effa2b2`): an inline iterable was popped off the VM stack before the
  loop body ran, so GC only rooted the stack below it and any body
  allocation past the 2 MiB threshold collected the iterable mid-iteration
  (use-after-free — the `chinese/server.lume` crash chain). The iterable
  now stays on the stack across the whole loop. Regression legs:
  `tests/for-in-gc.lume` plus `examples/minimal-server-gc.lume` with
  `run_all.sh` 4e (HTTP bracket side asserts `1799970000`); the HTTP leg
  runs on Linux CI (Windows builds have no HTTP bridge).

### Changed

- `lang/` PIN bumped to lume-core `5bd5f04` (`v0.9.3-53`); statistical
  builtins hand-merged (`dfdacf0`); `value.c` pinned host-owned with
  upstream lag tracked (core's OBJ_SQLTX sql-tx mark+release must not
  overwrite the host's sql surface — `fb9bff1`); abac `respond()` unified
  to an anonymous 3-field map so native backends compile (`2b3b624`).
- agent-httpd submodule gitlink bumped `b998e7f` → `23b1c4e` (react_prefix
  /SSR relay on main, remote-reachable; clone verified).
- `LUME_VERSION` bumped `0.7.3` → `0.8.0` (Makefile line 29, single source
  for `lume --version`); the machine-readable version now self-reports
  `0.8.0`.

## [0.7.3] - 2026-10-19

### Changed

- `lang/` synced to lume-core `v0.9.3` (`sha 4f24037`) — `lang/lexer.c`
  now supports scientific notation (`e`/`E` exponent) in numeric literals,
  matching `1e5` / `2.3E-4` style constants. `lang/PIN` and
  `lang/PIN.manifest` refreshed accordingly.
- `LUME_VERSION` bumped `0.7.2` → `0.7.3` (Makefile line 29, single source
  for `lume --version`); the machine-readable version now self-reports
  `0.7.3`.

## [0.7.2] - 2026-10-19

### Changed

- `agent-httpd` submodule pinned to `ee18006` — adds a `.gitattributes`
  (`* text=auto eol=lf` plus an explicit binary list: `*.gz *.class
  *.jar *.png *.jpg *.jpeg *.gif *.ico *.woff *.woff2 *.ttf *.eot *.db
  *.sqlite *.sqlite3 *.pem *.crt *.key *.pdf`). The vendored agent service
  now keeps LF line endings on every platform; the Windows default
  `core.autocrlf=true` would otherwise CRLF the sources on clone. Pure
  cross-platform hygiene for the embedded tree — no runtime or binary
  change.

## [0.7.1] - 2026-10-19

### Fixed — fold in the two upstream memory-safety fixes (lume-core 59ac8a4)

- `lang/typecheck.c` (host-owned, hand-merged from core `de00906`): the
  Pass-C `calloc`'d param scratch was orphaned after `type_func()` started
  copying params (P2 Stage 1) — leaked 40 bytes per module that declares
  functions, caught by CI LeakSanitizer. Now arena-owned, matching
  `N_FUNC_LIT`.
- `lang/lexer.c` (synced from core `59ac8a4`): `lex_istr`'s `EXPR_STR`
  escape skip `p += 2` is now guarded so a trailing backslash at EOF no
  longer reads one byte past the buffer (ASan heap-buffer-overflow).

## [0.7.0] - 2026-10-19

### Added

- `sha256()` / `hmac_sha256()` / `base64_encode()` / `base64_decode()`
  builtins, with the SHA-256 implementation vendored from lume-core
  `compat/` and compiled into the host build — the DSL can now sign
  requests and emit HTTP Basic credentials (accepted upstream from
  lume-core v0.8.0; covered by `tests/test-crypto.lume`).
- Type annotations for `fn` values (`fn(int, string) -> bool`) with
  funclit capture fields, hand-merged from upstream.
- Route table capacity raised 32 → 128 (`MAX_AL_ROUTES`, both definition
  sites changed together).
- `rea-demo` (reverse-engineering MCP service, with homepage examples and
  agent steering away from the 170-tool guessing spiral) and
  `serena-demo` (source-analysis MCP service, `lang/` mounted as the
  analysis target; MCP responses anchored on the JSON-RPC frame).

### Fixed

- The typechecker validated only the LEFT operand of an arithmetic
  expression, so `1 + "x"` passed `--check` and then blew up at runtime —
  a `watch` hot-reload then "recovered" by restarting the child, killing
  the served app. Both operands must now be numeric (`type_compat(FLOAT)`).
- `ty_str`'s diagnostic spelling sized its `snprintf` calls with the ring
  pointer (8 bytes) instead of the ring slot (128), truncating type names
  longer than 7 characters in checker messages. Hand-merged from
  lume-core @ `98ceb48`.
- The interpreter's JSON parse errors now carry the same shape as the
  native emitter's (5xx body matches what `--compile` runs produce).
- `native_split` wraps its root list so the GC can no longer collect it
  mid-call (use-after-free).
- Windows build: duplicate http stubs dropped, the POSIX regex builtins
  vendored (no more `-lgnurx` dependency).

### Security

- Privacy gate: `check-tree-privacy.sh` ported and wired as
  `make treecheck`, with a CI leg that fails the build on any leak at
  HEAD; the 2026-10-18 history scrub is documented in the script, and a
  Dockerfile comment carrying a real build-machine path was replaced with
  a `/Users/<user>` placeholder.

### Sync

- Pinned lume-core v0.8.0 (`436b744`) → v0.9.0 lineage (`c9271a6`): the
  P2 bump-arena regime (parser + Asg/arm-scope strings) rides the synced
  `lang/` files; pinned drift converged byte-for-byte along the way.
- `agent-httpd` submodule bumped to `f1c0047` (v0.4.0): sqlite guard
  mirror + portfolio-escape fix, auth hot-reload, MCP frame fixes.

## [0.6.3] - 2026-10-11

### Added

- `lang/` hand-merged upstream lume-core @ `1435930` (pin `6e7a691` →
  `1435930`): the DSL gains `sort(list)` (in-place, homogeneous
  int/float/string, exact i64 ordering) and `str_match(pattern, text)` /
  `regex` (POSIX ERE findall → `[{text, index, groups}]`), and the outbound
  HTTP builtins now honor `NO_PROXY` / `no_proxy` (`.domain` suffix, exact
  host/IP, `*`, optional `:port`; `LUME_NO_PROXY` wins over
  `LUME_HTTP_PROXY`). Host-only parts kept: `HTTP_DBG` debug logging,
  proxy-failure direct-connect fallback, `sql_*` registrations.

### Fixed

- READMEs (en/zh) no longer claim the DSL has no outbound HTTP builtin —
  `http_get`/`http_post`/`http_put`/`http_patch`/`http_delete` are compiled
  in (via the pinned lume-core `lang/builtins_http.c`), so an untrusted
  script can exfiltrate data to a public endpoint. The security notes now
  state the real surface: private/reserved addresses are refused by
  default, scripts must be audited, and `--no-net` / `LUME_NO_NET=1` kills
  all outbound sockets. `lang/` layout stats refreshed (36 files /
  ~11.3k lines; 21 synced / 14 host-owned / 6 host-only per
  `make check-sync`).

### Build

- `LUME_VERSION` bumped to 0.6.3 (landed just after the tag, together
  with compiling the vendored SHA-256 — the binary now self-reports the
  release version).

## [0.6.2] - 2026-10-09

### Added

- `lume --version` / `-V` now prints the build version (`LUME_VERSION`,
  injected by the Makefile as a single source of truth) so an installed
  binary self-reports the same version as the GitHub Release tag it shipped
  from; the flag is listed in `--help`.
- Response envelopes can now carry `cache_control`: a map returned by a route
  handler (or by `respond`-style helpers) may set the `cache_control` key, and
  it is forwarded verbatim as the `Cache-Control` response header. The field is
  absent by default (each request memsets the response, so no header is
  emitted unless set) — pair it with `PURGE_CLIENT_CACHE` on the HTTP engine
  when a route serves staleable content. Daoye-signal's dashboard uses
  `cache_control: "no-store"` on every JSON endpoint.

- Route handlers now see `req.query` (raw query string without the `?`;
  null when absent) and `req.query_params` (URL-decoded map; a segment
  without `=` gets an empty value, repeated keys last-wins, `+` decodes
  to space).

- `examples/react-ssr.lume` (:8085, `make react-ssr`): a React SSR content
  page driven by a resident node backend — the `server` block sets
  `react_socket` (`./.data/react-ssr.sock`) and the built-in HTTP engine
  FastCGI-relays `/react/*` to `bin/react-ssr-server` (built from
  `frontend/react-ssr/` TSX by `pnpm install` + `scripts/build-react-ssr.sh`;
  `make react-ssr` builds, starts the backend, then runs lume). React components render with `react-dom/server`
  `renderToString` + `StaticRouter` — `/react`, `/react/about`,
  `/react/counter` all server-render; query strings (`?name=Ada&message=hi`)
  are relayed and echoed. No CGI fork, no Lume vdom. Without the backend,
  `/react` degrades to the static `www/react/home.html` shell.

### Fixed

- Route handler lookup now matches the path with the query string stripped
  (the framework already dispatched on the stripped path, but the DSL shim
  compared the full URI, so any `/echo?a=1` request fell through to a 404).

- Call-argument lists now accept a trailing comma (`f(a, b,)`), matching
  list and map literals; previously the extra comma made the parser try to
  parse the closing paren as another expression.

## [0.2.0] - 2026-09-25

### Security

- `env()` masks credential-named environment variables (`*API_KEY`, `*TOKEN`,
  `*SECRET`, `*PASSWORD`, `*CREDENTIAL`, case-insensitive, word-boundary
  match) from `.lume` scripts — they read as `null`. The runtime still reads
  them itself; config-like names (`HTPASSWD_FILE`, ...) are unaffected.
- Startup prints a one-time stderr WARNING when the server binds a
  non-loopback address with Basic Auth off, noting that /chat, /dsl and the
  SQL data behind them are reachable by any host that can reach the port.
- Session retention is configurable via `SESSION_TTL_DAYS` (default 30,
  `0` disables the sweep; memory.json is never pruned).
- README/README.zh.md document the privacy posture: chat + SQLite schema ride
  along to the configured `LLM_API_URL` endpoint; `.lume` files are trusted
  code (only run authored/audited scripts); data backup/deletion is
  user-managed.

### Changed

- /dsl page UI overhaul: sections now render as cards, a status badge
  shows data readiness / row count / errors, tables get styled headers,
  zebra striping, right-aligned numeric columns and monospace symbol
  cells, and the code sample sits in a proper code panel. The page's
  Tailwind utilities were previously never emitted because `dsl.tsx`
  was missing from `app.css` `@source` — tables and the bare `<pre>`
  had no styles at all; the source list now includes it.

## [0.1.2] - 2026-09-25

### Added

- `for` loops in both forms: C-style `for (init; cond; incr)` (all three
  optional) and iteration `for (x in xs)` / `for (let x in xs)` over a
  list's items or a map's keys (sorted). `for (;;)` is an infinite loop.
- `break` / `continue` in `for` and `while`; both are statically rejected
  outside a loop (typechecker tracks loop nesting, function-local).
- Collection builtins: `range(stop)` / `range(start, stop, step)`,
  `map(fn, list)`, `filter(fn, list)`, `reduce(fn, list, init)` — the fn
  forms accept named functions and lambdas and run through the shared call
  machinery (GC-rooted, `return`/`?` unwinding intact).
- Adjacent string literals merge JS-style: `"a" "b"` is `"ab"` (merged at
  parse time pre-escape, so `\n` / `\"` inside either part keep meaning
  across the join). Non-string neighbours still error as before.
- SQL builtins: `sql_query(sql[, params])` / `sql_query(path, sql[, params])`
  (read-only SELECT returning a list of row maps) and `sql_write(sql[,
  params])` / `sql_write(path, sql[, params])` (guarded INSERT / UPDATE /
  DELETE with mandatory WHERE, or CREATE TABLE for a new table; returns
  affected rows, 0 for DDL). `params` binds `?` placeholders via
  sqlite3_bind_* — values never enter the SQL text, so the guardrails see
  only the statement skeleton and injection through a parameter value is
  impossible (quotes, `;`, `--`, DDL keywords in a value are inert). The
  single-statement checks, SELECT-only read and write allow-list are shared
  with the agent chat tools via agent-httpd's exported `sqlite_query_json`
  / `sqlite_write_exec` — one guardrail implementation, no duplication. The
  default database comes from env `SQLITE_DB`; an explicit path opens any
  SQLite file (read-only for queries).
- Typecheck: un-annotated list literals may mix element types (element
  type falls back to `any`), needed for heterogeneous `?` parameter lists
  like `[3, "c"]`; annotated lists stay homogeneous.
- `sqlite-write` example grows a /dsl demo page (chat UI gains a DSL entry
  in the shared nav) and the page migrated from Lume SSR to a React
  client that fetches the `/dsl/data` JSON API.

### Changed

- Release assets are now tarballs (`lume-<os>-<arch>.tar.gz`) containing
  `bin/lume` plus the web UI (`www`, including the esbuild bundles that
  are gitignored in the repo), `examples/`, `docs/` and the READMEs —
  a bare binary alone cannot serve /chat /dsl because the docroot
  resolves `./www` from the working directory. Each tarball ships with a
  `.sha256` sidecar; `install.sh` downloads the tarball, puts the binary
  in `~/.local/bin` and the web UI/examples/docs in `~/.local/share/lume`.
- `install.sh`: installs from the tarball (needs `tar`), still honors
  `LUME_VERSION` / `LUME_PREFIX` / `LUME_SHA256` (checksum now applies to
  the tarball).

## [0.1.1] - 2026-09-25

### Added

- `--help` / `-h` CLI flag: prints usage and exits 0 (previously fell into
  the unknown-flag error path and exited 2).
- One-command installer `install.sh`
  (`curl -sSfL https://raw.githubusercontent.com/erishen/lume/main/install.sh | sh`)
  — downloads the platform binary from GitHub Releases to `~/.local/bin/lume`;
  `LUME_VERSION` / `LUME_PREFIX` / `LUME_SHA256` overrides.
- Prebuilt binaries for all four platforms (linux-x64/arm64, darwin-x64/arm64)
  as `lume-<os>-<arch>`, built by a GitHub Actions matrix and attached to the
  release on tag push; the same matrix runs on push/PR to catch cross-platform
  build failures before a tag is cut. (darwin-x64 was delayed on 0.1.1 by a
  starved macos-13 runner and shipped with 0.1.2 via cross-compilation.)

## [0.1.0] - 2026-09-24

Initial release — a self-contained agent DSL server (one C11 binary serving
static sites, JSON APIs, SSE chat, agent tools and SSR pages).

### Added

- Lume DSL: lexer/parser/type-checker/tree-walking interpreter with `el()`
  and `html()` page builders, agent-httpd bridge (HTTP, chat SSE, MCP, tools).
- Product API (`iquest`): weekly report archive + approval settings endpoint
  with whitelisted keys, same-origin CSRF guard, atomic .env write-back.
- Native SQLite: `sql_query` (read-only, physically enforced via
  `SQLITE_OPEN_READONLY`), opt-in guarded `sql_write`, `sql_tables`,
  `sql_schema` — no Python, no MCP stdio process.
- Text2SQL: live schema + data discipline injected into the chat system
  prompt (DataPulse-style describe), cached by db mtime.
- `lock_file(path, wait_ms?)` / `unlock_file()` built-ins: flock-based
  advisory lock (auto-released on process death), serializing invest ledger
  read-modify-write across workers.
- `write_file` is atomic: payload goes to a sibling `.tmp.<pid>` file renamed
  over the target; created files are `fchmod`'d 0600.
- invest weekly reports get a `YYYYMMDD_HHMM` timestamp — same-day
  regenerations no longer overwrite each other.
- Kubernetes manifests (`docker/k8s/`): invest / hub deployments with Basic
  Auth from a Kubernetes Secret.
- Six examples: demo / hello / invest / hub / lang-basics / sqlite-write.
- Tooling: `make dev|invest|hub|demo-sqlite|test|check|clean`, `make asan`
  (ASan/UBSan), `make vsix` (VS Code extension pack), `make ui` (esbuild /
  Tailwind frontend bundles).
- CI: GitHub Actions (test + asan) with agent-httpd vendored as a git
  submodule pinned by gitlink.
- Docker: two-stage image rebuilding the C binary inside a Linux container;
  compose stack for invest / hub behind Basic Auth.

### Changed

- `examples/invest.lume` and the frontend report/date handling accept the new
  timestamped report names.
- invest default tool whitelist is read-only; `sql_write` is opt-in via the
  whitelist (`HARNESS_TOOLS_ALLOW`).

### Security

- Same-origin guard covers **GET and POST** product endpoints
  (`/api/reports`, `/api/reports/*`, `/api/settings`): cross-origin browser
  reads are rejected 403 (DNS-rebinding style theft), `Origin: null`
  rejected, origin-less curl / local scripts still work.
- invest server binds `127.0.0.1` by default (loopback only); dead
  `write_file` entry removed from the invest tool allow-list; cross-border
  data disclosure banner + privacy policy added to the settings page.
- Settings endpoint redacts `env_file` and upstream URLs (`configured`/`null`),
  provider control characters rejected (400), write-back refused when
  `IQUEST_ENV_FILE` is unset.
- Portfolio ledger write path: per-process flock + atomic rename; load
  failure prints an explicit corruption warning instead of silently
  continuing with an empty ledger.
- Private data permissions: `.data` 0700 via `mkdir`, mcp config written with
  `fchmod 0600`, session files 0600, access logs never record query strings.
- Frontend has zero third-party calls; `read_file` jailed to web root; MCP
  children get LLM keys unset; sessions TTL 30 days.
