# Changelog

All notable changes to Lume are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and versions aim for
[SemVer](https://semver.org/).

## [Unreleased]

- (placeholder)

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
