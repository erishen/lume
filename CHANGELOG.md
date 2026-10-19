# Changelog

All notable changes to Lume are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and versions aim for
[SemVer](https://semver.org/).

## [Unreleased]

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
