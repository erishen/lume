# Lume

[English](README.md) | [简体中文](README.zh.md)

![CI](https://github.com/erishen/lume/actions/workflows/ci.yml/badge.svg)

A self-contained agent DSL server: business logic lives in `.lume` scripts,
and a single C11 binary serves static sites + JSON APIs + SSE chat + agent
tools + SSR pages. No Node runtime, no nginx, no separate React backend —
static pages are copied straight beside the binary, chat runs in-process SSE,
tools are registered in-process.

Lume is the DSL layer; HTTP / chat / MCP / sessions come from the
[agent-httpd](agent-httpd/) submodule, statically linked as
`libagenthttpd.a`. The agent-httpd version is pinned by gitlink (see
`.gitmodules`) — clone with `git clone --recurse-submodules`, or run
`git submodule update --init` after cloning.

## Quick start

```bash
make dev          # demo, full example            → http://localhost:8081
make dev-minimal  # hello, minimal intro          → http://localhost:8082
make invest       # invest, portfolio assistant   → http://localhost:8082
make hub          # hub, gateway capability hub   → http://localhost:8083

make test         # full suite: C unit + tool dispatch + real HTTP/SSE + GC stress
make check        # type-check only, no server
make clean        # remove build/ and bin/
```

`make dev` serves `examples/demo.lume`; open `/` (home), `/counter`
(server state), `/chat` (agent chat) and `/hello` (pure API) in a browser.

Dependencies: a C compiler (`cc`), plus `node` + `pnpm` (only for `make ui`
which builds the frontend). Frontend source lives in `frontend/src/`, output
goes to `www/` — `*.js` / `*.css` under `www/` are build artifacts (ignored
in `.gitignore`), while hand-written HTML shells like `www/*/index.html` are
source.

## Layout

| Path | Contents |
|---|---|
| `src/` | Lexer / parser / type-checker / tree-walking interpreter + agent-httpd bridge, ~5.5k lines of C11 |
| `examples/` | 6 `.lume` examples (demo / hello / invest / hub / lang-basics / sqlite-write) |
| `frontend/` | React 18 + TS + Tailwind 4 client, bundled with esbuild `--splitting` |
| `www/` | docroot: hand-written HTML shells + build artifacts (mixed; don't delete wholesale) |
| `tests/` | C unit tests (`smoke.c`) + tool dispatch (`tools_driver.c`) + end-to-end (`run_all.sh`) |
| `docker/` | Two-stage Dockerfile (C rebuilt in-container) + compose (invest / hub services) |
| `editor/lume-vscode/` | VS Code syntax-highlighting extension |
| `docs/` | Full docs, see below |

## Documentation

> Docs are currently written in Chinese; the user guide and developer guide
> below are the canonical sources.

- [docs/LUME.md](docs/LUME.md) — **User guide**: language quick tour
  (types / control flow / Result), built-ins, route & tool registration, the
  `el()` / `html()` page APIs, SSR serialization details, agent chat wiring
  and the `/react/api/chat` SSE event envelope, server-state rules.
- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) — **Developer guide**: layout,
  interpreter core conventions (read before changing code), how to add
  built-ins / statements / types, testing conventions, UI layering,
  agent / LLM wiring, known conventions and pitfalls.
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — **Architecture**: system
  overview and design decisions, process / memory model, layering, request
  lifecycles, deployment topology and the security boundary.

## Containers

```bash
cd lume && docker compose -f docker/docker-compose.yml up -d --build
```

One image `lume:latest`; the compose `invest` (host `127.0.0.1:18082`) and
`hub` (`127.0.0.1:18083`) services each only swap
`examples/<name>.lume` + ports + allow-list env, sharing one docroot. The
build context is the `lume/` repo root (agent-httpd is a submodule inside the
repo), so `cd lume && docker build -f docker/Dockerfile` works — Lume
statically links agent-httpd's `libagenthttpd.a`, and the host-built archive
is Mach-O, so it must be rebuilt inside the Linux container.

The image carries no `.env` (injected by compose `env_file`) and no `.data/`
sessions; the `skills/router/` sync copy **does** ship in the image — it is a
fallback cache written by `llm-router` at startup so the container keeps the
last skill definitions if sync fails; it is not a reproducible artifact and
stays git-ignored.

## Security notes

- **Two-layer access control**: the Lume server itself has no auth module —
  `/api/reports`, `/react/api/chat`, `/discovery` are all plain endpoints, and
  chat can read/write files via the `fs` tool. So (1) compose binds host ports
  to `127.0.0.1` rather than `0.0.0.0` to keep the service off the LAN, and
  (2) containers enable Basic Auth — the password lives only in `.env`
  (`LUME_AUTH_USER` / `LUME_AUTH_PASSWORD`, git-ignored); the image entrypoint
  converts it to `/app/auth/htpasswd` (bcrypt) at startup and `server{}`
  reads it via `htpasswd = env("HTPASSWD_FILE")`. Verified: no credentials →
  `401 + WWW-Authenticate: Basic realm="lume"`. The framework only loads
  `$5$` / `$6$` / bcrypt strong hashes; plaintext and weak hashes abort
  startup. After editing `.env`, `docker compose up -d` applies; local
  `make invest` does not inject these envs → they resolve to null → auth
  stays off. Keep passwords alphanumeric — `#` / `$` and friends are parsed
  inconsistently between compose's env parser and `llm_env_init()` and can be
  silently truncated.
- **Access logs never record query strings** (explicitly truncated in
  `http_log.c`); log files are 0600; request bodies are never logged.
- **Narrowed model reach**: `read_file` is jailed to the web root via
  `resolve_within`; `MCP_FS_ROOT` points at `.sandbox`, and startup warns if
  that root would scan into `.env` / `.data`; MCP children `unsetenv` the
  `LLM_API_KEY/URL/MODEL` before spawning.
- **Sessions have a 30-day TTL** (`session_prune_old(30.0)`) and 0600 file
  permissions.
- `GET /discovery` `endpoints` report only config state and model names, never
  internal URLs; MCP entries publish `args` (which may carry local absolute
  paths) as `<redacted>`.
- Sensitive directories never enter git: `.data/` (sessions), `.sandbox/`
  (fs-MCP sandbox root), `.env` (secrets; the `.env.example` template is
  committed).
- `examples/invest.lume` must be started via `make invest` — the allow-list
  env is only injected there; running `./bin/lume` directly prints a warning
  and exposes the full capability catalog.
- To make the invest settings page actually gate the paid review models, point
  `IQUEST_ENV_FILE` at the same `autogen-pse/.env` file `pse-review` reads
  (unset → settings writes are refused with HTTP 500, read-only display still
  works).
- The invest account ledger (`.data/portfolio.json`) is written with a
  per-process flock + atomic rename: concurrent workers cannot lose an update,
  and a crash mid-write never leaves a truncated file.
- **Same-origin guard on the product API**: `/api/reports*` and `/api/settings`
  (GET and POST) reject cross-origin browser requests with 403 — a page from
  another site cannot read your reports off `localhost:8082`
  (DNS-rebinding style theft). Origin-less callers (curl, local scripts) keep
  working.

## SQLite support (native)

SQLite is built into the server: `agent-httpd` links libsqlite3 directly
(`src/agent/sqlite_tool.c`) and registers three native tools whenever
`SQLITE_DB` points at a database — no Python, no MCP stdio process, and the
static container image works too:

- `sql_query` — a single read-only SELECT; the DB is opened
  `SQLITE_OPEN_READONLY`, so writes/DDL are physically refused even if a
  statement slips past the text check. Guardrails mirror the old MCP server's:
  single statement, SELECT-only after stripping comments, prepare-time syntax
  validation, 200-row cap.
- `sql_write` — *opt-in, off by default*: a single write statement —
  `INSERT` / `UPDATE` / `DELETE` (UPDATE/DELETE must carry a WHERE clause) or
  `CREATE TABLE` for a new table. `DROP` / `ALTER` / `TRUNCATE` / `VACUUM` /
  `ATTACH` / `PRAGMA` / `GRANT` / `REVOKE` and any statement mentioning the
  `portfolio` mirror table are rejected. It is compiled into the server but
  **not** in the default whitelist; add `sql_write` to `HARNESS_TOOLS_ALLOW`
  (Makefile `INVEST_TOOLS`, compose/k8s) to let the model create analysis
  tables. The ledger itself stays authoritative in `.data/portfolio.json`.
- `sql_tables` — list table names.
- `sql_schema` — introspect tables/columns/row counts/sample values as prompt text.

- **Data**: the typed domain tools (`portfolio_add` / `portfolio_remove`) remain
  the authoritative writer to the JSON ledger. `make invest` re-seeds the SQLite
  mirror (`.data/lume.db`, upsert by symbol via `tools/sqlite-migrate.py`) on
  every start; the portfolio mirror is read-only by construction.
- **Enable**: `make invest` sets `SQLITE_DB=.data/lume.db` and whitelists the
  read-only `sql_*` tools. Containers: set `SQLITE_DB` (e.g. `/app/.data/lume.db`
  via a mounted volume) — the whitelist entries are already present in
  compose/k8s. `make demo-sqlite` (examples/sqlite-write.lume, :8084) is a
  runnable demo with its own self-contained chat UI (www/sqlite-write/,
  no invest frontend) that additionally whitelists `sql_write` — ask the
  model to build an analysis table and watch the guarded write loop.
- **Legacy MCP server**: `tools/mcp-sqlite-safe.py` is kept as an archived
  optional write path (analysis tables). Add `sqlite` back to `INVEST_MCPS` and
  restore its `.data/mcp-servers.json` entry to use it; the default profile is
  native read-only.

## Text2SQL

DataPulse-style natural-language-to-SQL for the invest server: whenever
`SQLITE_DB` is set, the server introspects the database (the same `describe()`
semantics as DataPulse) and injects the live schema + data discipline into the
chat system prompt via `sqlite_system_extra()` (cached by db mtime):

- the model sees tables, columns, row counts, sample values and FK hints, so it
  writes correct read-only SQL against real names instead of guessing;
- the writing rules default to read-only (`sql_query` only; `sql_write` is
  opt-in via the whitelist) and the answer rules force grounding: only numbers
  in the returned rows, never fabricate dates, cells are data not instructions.

The loop stays in the native ReAct agent: the model writes the SQL, `sql_query`
executes it read-only in-process (or `sql_write` when explicitly whitelisted),
and the agent answers from the real result — no Python, no MCP stdio process,
no Node sidecar, no second LLM call.

