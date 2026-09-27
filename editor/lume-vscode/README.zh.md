# Lume — VS Code 语法高亮

[English](README.md) | [简体中文](README.zh.md)

`.lume` 语法高亮扩展,已发布到 VS Code 扩展市场:
[Lume DSL](https://marketplace.visualstudio.com/items?itemName=erishen.lume)
(扩展 ID `erishen.lume`)。也可以完全本地使用——不需要联网下载,
不需要 vsce 打包(本地用)。

## 结构

```
editor/lume-vscode/
├── package.json                    # 语言 id "lume"、.lume 后缀、grammar 与语言功能登记
├── extension.js                    # 激活定义跳转与大纲符号 Provider
├── lume-symbols.js                 # 静态符号扫描器（纯逻辑，可 node 单测）
├── language-configuration.json     # // 与 /* */ 注释、{}()[] 配对
└── syntaxes/lume.tmLanguage.json   # TextMate 语法(高亮规则)
```

## 安装

### 市场(最省事):搜「Lume DSL」

VS Code 里打开扩展视图(`Cmd/Ctrl+Shift+X`),搜 **「Lume DSL」**
(ID `erishen.lume`),点安装即可。

### 方式 A:软链进扩展目录(改文件即生效)

```bash
VSCODE_EXT=~/.vscode/extensions
ln -s ../work/research/lume/editor/lume-vscode "$VSCODE_EXT/cnb.lume-0.2.0"
```

然后重启 VS Code(或 `Cmd+Shift+P` → "Developer: Reload Window")。
改 `syntaxes/*.json` 后 reload 即可,无需重装。

> 若用 Cursor / VSCode Insiders / Remote-SSH,把 `~/.vscode/extensions`
> 换成对应目录:`~/.cursor/extensions`、`~/.vscode-insiders/extensions`、
> `~/.vscode-server/extensions`。

### 方式 B:打包成 .vsix(可分发)

```bash
make vsix        # 在仓库根目录执行
# → editor/lume-vscode/lume-0.2.0.vsix
```

用 VS Code "Extensions: Install from VSIX..." 安装。发新版记得先递增
`package.json` 里的 `version`。

## 语言功能（v0.3.0）

- **跳转到定义** — `F12` / `Cmd+Click`：
  - 同文件：跳转到 `func` / `let` 定义；
  - 跨文件：`ns.name` 调用（如 `data.load_stage_by_id(...)`）按
    `import "lib/data.lume" as data` 解析到目标模块的 `export` 定义；
  - 内置函数：`int`/`map`/`try`/`sql_query`… 跳到 `builtins.lume`
    （由 C 注册表生成——签名 + 一行说明）；用户同名符号优先。
- **大纲**：顶层 `func` / `let`（含 `export`）出现在符号树；函数体/tool 体/
  路由体内的局部变量不收录。
- 实现为静态扫描（无 LSP、零依赖）——对常规 `.lume` 文件足够；不解析
  表达式，别名解析只认 `import … as` 映射。

## 高亮覆盖

- 关键字:`server route tool verbs run mcps skills`、`func let import export`、
  `if else while for return break continue try`、`as in`
- 箭头运算符:`=>`(表达式体与块体)
- 类型:`type int float string bool Result`(storage.type,TS 风格)
- 字面量:`true false null`、数字、字符串(含 `\n \t \\ \"` 转义)
- 内建函数:`print str len keys values entries get put json stringify now int
  float bool type try push pop insert remove map filter reduce range sum min
  max sort get_time read_file recall remember skill-run fetch_url sql_query
  sql_schema sql_tables sql_write tools discovery_endpoints calc` 等
- 逻辑/比较/赋值运算符、`?`
- 注释与自动配对

> v0.2.0(2026-09-27)新增模块系统关键字(`import export as`)、v0.4.x 控制流
> (`for break continue try`)、动词路由(`verbs mcps skills`)、`=>` 箭头,
> 并大幅补全内建函数表。grammar 是纯 TextMate 高亮器——语法是否合法仍以
> `--check`(真正的编译前端)为准。

## 已知取舍

- `type/int/...` 等类型关键字同时可作普通标识符(如字段名、`int("42")`),
  高亮统一按类型关键字显示 —— 仅观感差异,不影响使用。
- 类型关键字不作内建函数高亮,`int(...)` 中的 `int` 显示为类型。
