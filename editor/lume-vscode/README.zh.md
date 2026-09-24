# Lume — VS Code 语法高亮

[English](README.md) | [简体中文](README.zh.md)

本地扩展,给 `.lume` 文件提供语法高亮、注释切换、括号配对与自动闭合。
不需要联网下载,不需要 vsce 打包(本地用)。

## 结构

```
editor/lume-vscode/
├── package.json                    # 语言 id "lume"、.lume 后缀、grammar 登记
├── language-configuration.json     # // 与 /* */ 注释、{}()[] 配对
└── syntaxes/lume.tmLanguage.json   # TextMate 语法(高亮规则)
```

## 安装(两种方式)

### 方式 A:软链进扩展目录(推荐,改文件即生效)

```bash
VSCODE_EXT=~/.vscode/extensions
ln -s ../work/research/lume/editor/lume-vscode "$VSCODE_EXT/cnb.lume-0.1.0"
```

然后重启 VS Code(或 `Cmd+Shift+P` → "Developer: Reload Window")。
改 `syntaxes/*.json` 后 reload 即可,无需重装。

> 若用 Cursor / VSCode Insiders / Remote-SSH,把 `~/.vscode/extensions`
> 换成对应目录:`~/.cursor/extensions`、`~/.vscode-insiders/extensions`、
> `~/.vscode-server/extensions`。

### 方式 B:打包成 .vsix(可分发)

```bash
make vsix        # 在仓库根目录执行
# → editor/lume-vscode/lume-0.1.0.vsix
```

用 VS Code "Extensions: Install from VSIX..." 安装。发新版记得先递增
`package.json` 里的 `version`。

## 高亮覆盖

- 关键字:`server route tool func let return if else while`
- 类型:`type int float string bool Result`(storage.type,TS 风格)
- 字面量:`true false null`、数字、字符串(含 `\n \t \\ \"` 转义)
- 内建函数:`print str len keys get json stringify now`
- 逻辑/比较/赋值运算符、`?`
- 注释与自动配对

## 已知取舍

- `type/int/...` 等类型关键字同时可作普通标识符(如字段名、`int("42")`),
  高亮统一按类型关键字显示 —— 仅观感差异,不影响使用。
- 类型关键字不作内建函数高亮,`int(...)` 中的 `int` 显示为类型。
