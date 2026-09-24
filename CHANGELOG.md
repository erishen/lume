# Changelog

All notable changes to Lume are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and versions aim for
[SemVer](https://semver.org/).

## [Unreleased]

### Added

- `lock_file(path, wait_ms?)` / `unlock_file()` built-ins: flock-based
  advisory lock (auto-released on process death), used by invest to serialize
  ledger read-modify-write across workers.
- `write_file` is now atomic: payload goes to a sibling `.tmp.<pid>` file
  renamed over the target, so a crash mid-write never truncates the ledger /
  `.env` / reports; created files are `fchmod`'d 0600 (private even when
  `IQUEST_REPORTS_DIR` points outside `.data`).
- invest weekly reports get a `YYYYMMDD_HHMM` timestamp — same-day
  regenerations no longer overwrite each other; pricing line states how many
  positions were valued at cost when prices only partially cover holdings.
- Kubernetes manifests (`docker/k8s/`): invest / hub deployments for the local
  k3s loop, with Basic Auth password taken from a Kubernetes Secret.

### Security

- Same-origin guard now covers the **GET** product endpoints too
  (`/api/reports`, `/api/reports/*`, `/api/settings`): cross-origin browser
  reads are rejected 403 (DNS-rebinding style theft), `Origin: null` rejected,
  origin-less curl / local scripts still work.
- invest server binds `127.0.0.1` by default (loopback only); dead `write_file`
  entry removed from the invest tool allow-list; cross-border data disclosure
  banner + privacy policy added to the invest settings page, including the
  pse-review model chain.
- Portfolio ledger load failure now prints an explicit corruption warning
  instead of silently continuing with an empty ledger.

### Changed

- `examples/invest.lume` and the frontend report/date handling accept the new
  timestamped report names.

## [0.1.0] - 2026-09-24

Initial release — a self-contained agent DSL server (one C11 binary serving
static sites, JSON APIs, SSE chat, agent tools and SSR pages).

### Added

- Lume DSL: lexer/parser/type-checker/tree-walking interpreter with `el()`
  and `html()` page builders, agent-httpd bridge (HTTP, chat SSE, MCP, tools).
- Product API (`iquest`): weekly report archive + approval settings endpoint
  with whitelisted keys, same-origin CSRF guard, atomic .env write-back.
- Five examples: demo / hello / invest / hub / lang-basics.
- Tooling: `make dev|invest|hub|test|check|clean`, `make asan`
  (ASan/UBSan in a separate build dir), `make vsix` (VS Code extension pack),
  `make ui` (esbuild/Tailwind frontend bundles).
- CI: GitHub Actions (test + asan) with agent-httpd vendored as a git
  submodule pinned by gitlink.
- Docker: two-stage image rebuilding the C binary inside a Linux container;
  compose stack for invest / hub behind Basic Auth.

### Security

- Settings endpoint redacts `env_file` and upstream URLs (`configured`/`null`),
  no hardcoded host paths, provider control characters rejected (400),
  write-back refused when `IQUEST_ENV_FILE` is unset.
- Private data permissions: `.data` dirs 0700 via `mkdir`, mcp config written
  with `fchmod 0600`, session files 0600.
- Frontend has zero third-party calls; `read_file` jailed to web root;
  MCP children get LLM keys unset; sessions TTL 30 days.
