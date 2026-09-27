# Change Log

All notable changes to the Lume VS Code extension are documented here.

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
