# Change Log

All notable changes to the Lume VS Code extension are documented here.

## [0.3.6] - 2026-09-27

### Added

- `replace(s, from, to)` in `builtins.lume` (global literal replacement;
  empty `from` or no match returns the string unchanged). Generator
  `gen-builtins.py` gained the SIG/IMPL/DOC entries and a new "字符串变换"
  group — 39 builtins now (was 38), still verified against the interp.c
  registry.

## [0.3.5] - 2026-09-27

### Fixed

- `builtins.lume` docs for `env()` and `read_file()` were wrong: `env()` returns
  **null** for unset or sensitive variable names (not empty string — auth
  checks compare against null), and `read_file()` returns **null** for missing/
  unreadable files (not empty string; oversized files set a VM error). Docs
  regenerated from the actual C behaviour.

## [0.3.4] - 2026-09-27

### Added

- **Jump to C implementation**: Go to Definition on a built-in now returns the
  actual `native_<name>` / `b_<name>` location in the Lume C sources
  (priority) plus the `builtins.lume` doc declaration. The Lume repo root is
  resolved from the new `lume.sourceRoot` setting, or auto-detected in the
  workspace (BFS, depth 3; a folder named `*lume*` wins). Pure-lookup logic
  (`findSourceRoots` / `findNativeLine`) is in `lume-symbols.js` and unit
  tested against the real repo: int→builtins.c, map→builtins_hof.c,
  run→builtins.c (b_run), el→vdom.c, unknown→null.

## [0.3.3] - 2026-09-27

### Changed

- **Richer built-in jump target**: `builtins.lume` now carries detailed docs
  per function — behaviour, parameter semantics, return type, edge cases and
  the implementing C file (`src/builtins.c` / `_hof` / `_fs` / `_sql` /
  `_catalog` / `vdom.c`), with concrete signatures.
- **Generator checked in**: `scripts/gen-builtins.py` regenerates
  `builtins.lume` from the `bridge_seed_builtins()` registry (run it after
  adding built-ins; verified 38/38, no drift).
- **vsix output moved** to `editor/lume-vscode/dist/` (kept gitignored).

## [0.3.2] - 2026-09-27

### Added

- **Built-in function jump**: Go to Definition now falls back to
  `builtins.lume` (generated from the `bridge_seed_builtins()` registry,
  38 functions with signatures & one-line docs), so `int`/`map`/`try`/
  `sql_query`/`render`… resolve instead of dead-ending. User-defined
  same-name symbols still win.

## [0.3.1] - 2026-09-27

### Changed

- Drop the explicit `activationEvents` entry: VS Code auto-generates
  `onLanguage:lume` from `contributes.languages` (since 1.74), so declaring it
  manually was redundant.

## [0.3.0] - 2026-09-27

### Added

- **Go to Definition** (F12 / Cmd+Click): same-file `func`/`let` jumping, and
  cross-file jumping for `ns.name` calls resolved through `import "…" as ns`
  to the target module's `export` definitions.
- **Outline / Document Symbols**: top-level `func` / `let` (incl. `export`)
  shown in the symbol tree; locals inside function/tool/route bodies are
  excluded via brace-depth tracking.
- Implemented as a static scanner (`extension.js` + `lume-symbols.js`, no LSP,
  no dependencies). Language features activate on `onLanguage:lume`.

## [0.2.0] - 2026-09-27

### Added

- Keywords for the module system: `import`, `export`, `as`.
- v0.4.x control-flow keywords: `for`, `break`, `continue`, `try`.
- HTTP verb routes (`verbs`) plus `mcps`/`skills` statement keywords.
- Arrow `=>` operator; `in` keyword for `for ... in ...`.
- Fuller builtin-function list (`push`, `map`, `filter`, `reduce`, `range`,
  `sum`, `min`, `max`, `sort`, `values`, `entries`, `put`, `int`, `float`,
  `bool`, `type`, `get_time`, `read_file`, `recall`, `remember`, `skill-run`,
  `fetch_url`, `sql_*`, `tools`, `discovery_endpoints`, `calc`, ...).

## [0.1.0] - 2026-09-24

### Added

- Initial release: syntax highlighting (TextMate grammar, `source.lume`) for the
  Lume scripting DSL.
- Language configuration: block/line comments and bracket pairs.
- Packaged as MIT-licensed vsix, repository metadata pointing at
  github.com/erishen/lume.
