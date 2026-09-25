# Lume — VS Code Syntax Highlighting

[English](README.md) | [简体中文](README.zh.md)

A syntax-highlighting extension for `.lume` files, published on the VS Code
Marketplace as [Lume DSL](https://marketplace.visualstudio.com/items?itemName=erishen.lume)
(extension ID `erishen.lume`). It also works fully locally — no network
download needed, no vsce packaging required for local use.

## Structure

```
editor/lume-vscode/
├── package.json                    # language id "lume", .lume extension, grammar registration
├── language-configuration.json     # // and /* */ comments, {}()[] pairs
└── syntaxes/lume.tmLanguage.json   # TextMate grammar (highlighting rules)
```

## Install

### Marketplace (easiest): search "Lume DSL"

In VS Code, open the Extensions view (`Cmd/Ctrl+Shift+X`), search
**"Lume DSL"** (ID `erishen.lume`), and install.

### Option A: symlink into the extensions dir (edits take effect on reload)

```bash
VSCODE_EXT=~/.vscode/extensions
ln -s ../work/research/lume/editor/lume-vscode "$VSCODE_EXT/cnb.lume-0.1.0"
```

Then reload VS Code (`Cmd+Shift+P` → "Developer: Reload Window"). After
editing `syntaxes/*.json`, reload — no reinstall needed.

> For Cursor / VS Code Insiders / Remote-SSH, replace `~/.vscode/extensions`
> with `~/.cursor/extensions`, `~/.vscode-insiders/extensions`, or
> `~/.vscode-server/extensions`.

### Option B: package a .vsix (distributable)

```bash
make vsix        # from the repo root
# → editor/lume-vscode/lume-0.1.0.vsix
```

Install the vsix via VS Code "Extensions: Install from VSIX...". Bumping a
version? Increment `version` in `package.json` first.

## Highlight coverage

- Keywords: `server route tool func let return if else while`
- Types: `type int float string bool Result` (storage.type, TS-style)
- Literals: `true false null`, numbers, strings (with `\n \t \\ \"` escapes)
- Built-ins: `print str len keys get json stringify now`
- Logic / comparison / assignment operators, `?`
- Comments and auto-pairing

## Known trade-offs

- Type keywords (`type/int/...`) can also be ordinary identifiers (field
  names, `int("42")`); they are always highlighted as type keywords — purely
  cosmetic, no functional impact.
- Type keywords are not highlighted as built-ins; `int` inside `int(...)`
  renders as a type.
