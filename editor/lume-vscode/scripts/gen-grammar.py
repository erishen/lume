#!/usr/bin/env python3
"""生成 editor/lume-vscode/syntaxes/lume.tmLanguage.json 与 lume-core.tmLanguage.json。

关键字与内建函数一律**从注册表抽取**,不在 JSON 里维护手写快照:
  - src/lexer.c 的 {"name", TOK_*} 字面量表                  → 关键字
  - src/interp.c 的 bridge_seed_builtins() 注册表             → 内建函数

两棵树各自抽,所以 host 与 lume-core 的方言差异自动落到各自的语法里:
  - host-only : sql_query / sql_write(驱动 agent-httpd 的 db 层)
  - core-only : http_get(2026-10-04, builtins_http.c)
别再手改 syntaxes/*.tmLanguage.json 的 types/statements/builtins 段——
下次跑本脚本就被覆盖回去。

用法(在 lume 仓库根目录):
    python3 editor/lume-vscode/scripts/gen-grammar.py [--core ../lume-core]
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CORE = os.path.normpath(os.path.join(HERE, "..", "..", "..", "..", "lume-core"))

# lexer.c 字面量表里的 token -> 语法段。token 名取自源码,分类是一次性映射。
CATEGORY = {
    # 类型
    "type": "types", "int": "types", "float": "types",
    "string": "types", "bool": "types", "Result": "types",
    # 字面量
    "true": "literals", "false": "literals", "null": "literals",
    # 控制流
    "if": "control", "else": "control", "while": "control", "for": "control",
    "return": "control", "break": "control", "continue": "control",
    # 声明
    "let": "declaration", "func": "declaration",
    "import": "declaration", "export": "declaration",
    # 单词运算符
    "and": "operator", "or": "operator", "not": "operator",
    # 顶层 DSL / HTTP 动词(保留字,不是内建函数)
    "server": "dsl", "route": "dsl", "tool": "dsl", "verbs": "dsl",
    "get": "dsl", "head": "dsl", "post": "dsl", "put": "dsl",
    "patch": "dsl", "delete": "dsl", "options": "dsl",
}

SEGMENT_NAME = {
    "types": "storage.type.lume",
    "control": "keyword.control.lume",
    "declaration": "keyword.declaration.lume",
    "dsl": "keyword.statement.lume",
    "operator": "keyword.other.lume",
}


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def lexer_keywords(lexer_c):
    """从 src/lexer.c 抽 {"name", TOK_*} 字面量表(到 {"verbs", TOK_VERBS} 为止)。"""
    src = read(lexer_c)
    start = src.find('{"server"')
    if start < 0:
        sys.exit("lexer.c 里找不到 {\"server\"…} 字面量表")
    anchor = '{"verbs", TOK_VERBS}'
    end = src.find(anchor)
    if end < 0:
        sys.exit("lexer.c 里找不到 {\"verbs\", TOK_VERBS}")
    block = src[start:end + len(anchor)]
    names = re.findall(r'\{"([A-Za-z_][A-Za-z0-9_]*)"\s*,\s*TOK_[A-Z_0-9]+\}', block)
    if not names:
        sys.exit("lexer.c 字面量表抽取为空")
    return names


def registry_builtins(interp_c):
    """从 src/interp.c 的 bridge_seed_builtins() 抽 {"name", b_*} 注册项。"""
    src = read(interp_c)
    m = re.search(r"void bridge_seed_builtins\(VM \*vm\) \{(.*?)\n    \};", src, re.S)
    if not m:
        sys.exit("interp.c 注册表 bridge_seed_builtins() 未找到")
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    names = re.findall(r'\{"([A-Za-z_][A-Za-z0-9_]*)",\s*b_[A-Za-z0-9_]*', body)
    if not names:
        sys.exit("interp.c 内建注册表抽取为空")
    return names


def word(name, words):
    return {"name": name, "match": r"\b(?:%s)\b" % "|".join(words)}


def build(template, builtins, keywords):
    """在模板骨架上覆盖 types/statements/builtins 三段。"""
    repo = json.loads(json.dumps(template["repository"]))  # 深拷贝,别动模板
    repo["types"] = {"patterns": [
        word(SEGMENT_NAME["types"], [w for w in keywords if CATEGORY.get(w) == "types"])
    ]}
    repo["statements"] = {
        "patterns": [
            word(SEGMENT_NAME[seg], [w for w in keywords if CATEGORY.get(w) == seg])
            for seg in ("control", "declaration", "dsl", "operator")
        ]
    }
    repo["builtins"] = {
        "patterns": [{"name": "support.function.builtin.lume",
                      "match": r"\b(?:%s)\b" % "|".join(sorted(builtins))}]
    }
    grammar = {"name": template["name"], "scopeName": template["scopeName"],
               "patterns": template["patterns"], "repository": repo}
    return grammar


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--core", default=DEFAULT_CORE,
                    help="lume-core 树根(默认 ../lume-core)")
    args = ap.parse_args()

    host_root = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
    host_syntax = os.path.join(HERE, "..", "syntaxes")
    template = json.load(open(os.path.join(host_syntax, "lume.tmLanguage.json"), encoding="utf-8"))

    def emit(path, tmpl, builtins, keywords, name, scope):
        g = build(tmpl, builtins, keywords)
        g["name"] = name
        g["scopeName"] = scope
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(g, fh, indent=2, ensure_ascii=False)
            fh.write("\n")
        print("%-52s builtins=%-3d keywords=%d"
              % (os.path.relpath(path, host_root), len(builtins), len(keywords)))

    # --- host 方言:本树注册表 ---
    hk = lexer_keywords(os.path.join(host_root, "src", "lexer.c"))
    hb = registry_builtins(os.path.join(host_root, "src", "interp.c"))
    emit(os.path.join(host_syntax, "lume.tmLanguage.json"), template, hb, hk,
         "Lume", "source.lume")

    # --- core 方言:lume-core 树注册表(可选,缺席就跳过) ---
    # 落在 editor/ 下、但**在扩展目录之外**,这样 `make vsix` 不会把它打进 vsix,
    # 也就不需要新建 .vscodeignore(那会顶掉 vsce 的默认忽略规则)。
    core_root = os.path.abspath(args.core)
    if not os.path.isfile(os.path.join(core_root, "src", "interp.c")):
        print("skip lume-core.tmLanguage.json: %s 没有 src/interp.c" % core_root)
        return
    core_syntax = os.path.join(host_root, "editor", "lume-core-grammar")
    os.makedirs(core_syntax, exist_ok=True)
    ck = lexer_keywords(os.path.join(core_root, "src", "lexer.c"))
    cb = registry_builtins(os.path.join(core_root, "src", "interp.c"))
    emit(os.path.join(core_syntax, "lume-core.tmLanguage.json"), template, cb, ck,
         "Lume Core", "source.lume.core")


if __name__ == "__main__":
    main()
