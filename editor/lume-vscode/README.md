# Lume — VS Code Syntax Highlighting

[English](README.md) | [简体中文](README.zh.md)

A syntax-highlighting extension for `.lume` files, published on the VS Code
Marketplace as [Lume DSL](https://marketplace.visualstudio.com/items?itemName=erishen.lume)
(extension ID `erishen.lume`). It also works fully locally — no network
download needed, no vsce packaging required for local use.

## Structure

```
editor/lume-vscode/
├── package.json                    # language id "lume", .lume extension, grammar + language features
├── extension.js                    # activates Definition & DocumentSymbol providers
├── lume-symbols.js                 # static symbol scanner (pure logic, node-testable)
├── builtins.lume                   # built-in function declarations (generated; jump target)
├── scripts/gen-builtins.py         # regenerates builtins.lume from the C registry
├── language-configuration.json     # // and /* */ comments, {}()[] pairs
├── syntaxes/lume.tmLanguage.json   # TextMate grammar (highlighting rules)
└── dist/                           # packed .vsix artifacts (gitignored)
```

## Building the vsix

```bash
cd editor/lume-vscode
npx --yes @vscode/vsce package --allow-missing-repository -o dist/
```

## Install

### Marketplace (easiest): search "Lume DSL"

In VS Code, open the Extensions view (`Cmd/Ctrl+Shift+X`), search
**"Lume DSL"** (ID `erishen.lume`), and install.

### Option A: symlink into the extensions dir (edits take effect on reload)

```bash
VSCODE_EXT=~/.vscode/extensions
ln -s ../work/research/lume/editor/lume-vscode "$VSCODE_EXT/cnb.lume-0.2.0"
```

Then reload VS Code (`Cmd+Shift+P` → "Developer: Reload Window"). After
editing `syntaxes/*.json`, reload — no reinstall needed.

> For Cursor / VS Code Insiders / Remote-SSH, replace `~/.vscode/extensions`
> with `~/.cursor/extensions`, `~/.vscode-insiders/extensions`, or
> `~/.vscode-server/extensions`.

### Option B: package a .vsix (distributable)

```bash
make vsix        # from the repo root
# → editor/lume-vscode/lume-0.2.0.vsix
```

Install the vsix via VS Code "Extensions: Install from VSIX...". Bumping a
version? Increment `version` in `package.json` first.

## Language features (v0.3.0)

- **Go to Definition** — `F12` / `Cmd+Click`:
  - same-file: jump to `func`/`let` definitions;
  - cross-file: `ns.name` calls (e.g. `data.load_stage_by_id(...)`) resolve
    through `import "lib/data.lume" as data` to the target module's `export`
    definition;
  - built-ins: `int`/`map`/`try`/`sql_query`… jump to `builtins.lume`
    (generated from the C registry — signatures + one-line docs); user
    same-name symbols win.
- **Outline**: top-level `func`/`let` (incl. `export`) appear in the symbol
  tree; locals inside function/tool/route bodies are excluded.
- Implemented as a static scanner (no LSP, zero dependencies) — good enough
  for typical `.lume` files; it does not parse expressions or resolve
  aliases beyond the `import … as` map.

## Highlight coverage

- Keywords: `server route tool verbs run mcps skills`, `func let import export`,
  `if else while for return break continue try`, `as in`
- Arrow operator: `=>` (expression and block bodies)
- Types: `type int float string bool Result` (storage.type, TS-style)
- Literals: `true false null`, numbers, strings (with `\n \t \\ \"` escapes)
- Built-ins: `print str len keys values entries get put json stringify now int
  float bool type try push pop insert remove map filter reduce range sum min
  max sort get_time read_file recall remember skill-run fetch_url sql_query
  sql_schema sql_tables sql_write tools discovery_endpoints calc` and more
- Logic / comparison / assignment operators, `?`
- Comments and auto-pairing

> v0.2.0 (2026-09-27) added the module-system keywords (`import export as`),
> v0.4.x control flow (`for break continue try`), verb routes
> (`verbs mcps skills`), the `=>` arrow, and a much fuller builtin-function
> list. The grammar is a pure TextMate highlighter — `--check` (the real
> compiler front-end) remains the authority for syntax validity.

## Known trade-offs

- Type keywords (`type/int/...`) can also be ordinary identifiers (field
  names, `int("42")`); they are always highlighted as type keywords — purely
  cosmetic, no functional impact.
- Type keywords are not highlighted as built-ins; `int` inside `int(...)`
  renders as a type.
