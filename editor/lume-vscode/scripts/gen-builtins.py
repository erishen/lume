#!/usr/bin/env python3
"""生成 editor/lume-vscode/builtins.lume。

内置函数声明文件：由 lang/interp.c bridge_seed_builtins() 注册表自动提取，
保证与运行时零漂移。每个函数带详细说明（行为/边界/实现位置），供编辑器
“跳转到定义”与大纲参考；文件不会被编译或 import。

用法（在 lume 仓库根目录）:
    python3 editor/lume-vscode/scripts/gen-builtins.py
"""
import re
import sys
import os

ROOT = os.path.dirname(os.path.abspath(__file__)) + "/../../.."
OUT = os.path.join(ROOT, "editor/lume-vscode/builtins.lume")

# ---- 权威来源：interp.c 注册表 ----
src = open(os.path.join(ROOT, "lang/interp.c"), encoding="utf-8").read()
m = re.search(r"void bridge_seed_builtins\(VM \*vm\) \{(.*?)\n    \};", src, re.S)
if not m:
    sys.exit("interp.c 注册表未找到")
names = re.findall(r'\{"([A-Za-z_][A-Za-z0-9_]*)",\s*b_', m.group(1))

# ---- 每个函数的：签名 / 实现文件 / 详细说明 ----
SIG = {
    "run": "run(file)", "print": "print(value)", "str": "str(value)",
    "int": "int(value, fallback)", "float": "float(value, fallback)",
    "bool": "bool(value)", "string": "string(value)", "stringify": "stringify(value)",
    "len": "len(value)", "keys": "keys(map)", "get": "get(map, key, fallback)",
    "replace": "replace(s, from, to)",
    "put": "put(map, key, value)", "push": "push(list, item)",
    "range": "range(start, end)", "map": "map(fn, list_or_map)",
    "filter": "filter(fn, list_or_map)", "reduce": "reduce(fn, list, initial)",
    "json": "json(text)", "now": "now()", "env": "env(name)",
    "strftime": "strftime(fmt, ts)", "files": "files(path)",
    "read_file": "read_file(path)", "write_file": "write_file(path, content)",
    "mkdir": "mkdir(path)", "lock_file": "lock_file(path)",
    "unlock_file": "unlock_file(path)",
    "sql_query": "sql_query(sql, params)", "sql_write": "sql_write(sql, params)",
    "try": "try(fn)", "tools": "tools()", "skills": "skills()", "mcps": "mcps()",
    "discovery_endpoints": "discovery_endpoints()", "catalog": "catalog()",
    "el": "el(tag, attrs, children)", "render": "render(vdom)", "html": "html(text)",
    "crypt_sha512": "crypt_sha512(password)",
    "abs": "abs(x)", "sqrt": "sqrt(x)", "exp": "exp(x)",
    "log": "log(x)", "ln": "ln(x)", "pow": "pow(x, y)",
    "floor": "floor(x)", "ceil": "ceil(x)", "round": "round(x)",
    "min": "min(...)", "max": "max(...)",
}

IMPL = {
    # builtins.c（核心/集合/JSON/时间）
    "print": "lang/builtins.c", "str": "lang/builtins.c", "int": "lang/builtins.c",
    "float": "lang/builtins.c", "bool": "lang/builtins.c", "string": "lang/builtins.c",
    "stringify": "lang/builtins.c", "len": "lang/builtins.c", "keys": "lang/builtins.c",
    "get": "lang/builtins.c",
    "replace": "lang/builtins_str.c", "json": "lang/builtins.c", "push": "lang/builtins.c",
    "try": "lang/builtins.c", "now": "lang/builtins.c",
    # builtins_hof.c（高阶函数）
    "range": "lang/builtins_hof.c", "map": "lang/builtins_hof.c",
    "filter": "lang/builtins_hof.c", "reduce": "lang/builtins_hof.c",
    # builtins_fs.c（文件系统/环境）
    "env": "lang/builtins_fs.c", "files": "lang/builtins_fs.c",
    "read_file": "lang/builtins_fs.c", "write_file": "lang/builtins_fs.c",
    "mkdir": "lang/builtins_fs.c", "lock_file": "lang/builtins_fs.c",
    "unlock_file": "lang/builtins_fs.c", "strftime": "lang/builtins_fs.c",
    "put": "lang/builtins_fs.c",
    # builtins_sql.c（数据库）
    "sql_query": "lang/builtins_sql.c", "sql_write": "lang/builtins_sql.c",
    # builtins_catalog.c（工具/技能/MCP）
    "tools": "lang/builtins_catalog.c", "skills": "lang/builtins_catalog.c",
    "mcps": "lang/builtins_catalog.c", "discovery_endpoints": "lang/builtins_catalog.c",
    "catalog": "lang/builtins_catalog.c",
    # builtins_crypt.c（口令哈希）
    "crypt_sha512": "lang/builtins_crypt.c",
    # builtins_math.c（数值）
    "abs": "lang/builtins_math.c", "sqrt": "lang/builtins_math.c",
    "exp": "lang/builtins_math.c", "log": "lang/builtins_math.c",
    "ln": "lang/builtins_math.c", "pow": "lang/builtins_math.c",
    "floor": "lang/builtins_math.c", "ceil": "lang/builtins_math.c",
    "round": "lang/builtins_math.c", "min": "lang/builtins_math.c",
    "max": "lang/builtins_math.c",
    # 其他
    "run": "lang/interp.c", "el": "lang/vdom.c", "render": "lang/vdom.c", "html": "lang/vdom.c",
}

DOC = {
    "run": ("运行/导入另一份 .lume 文件（在 server 环境外使用）。",
            "参数：file（路径）。实现：lang/interp.c"),
    "print": ("输出到日志，运行时对敏感字段脱敏。",
              "参数：value。无返回值。实现：lang/builtins.c"),
    "str": ("转字符串。参数：value → string。实现：lang/builtins.c"),
    "string": ("转字符串（与 str 同义）。参数：value → string。实现：lang/builtins.c"),
    "int": ("转整数。value 无法解析时回落 fallback（不置 error，2026-09-27 起）。",
            "参数：value, fallback → number。实现：lang/builtins.c"),
    "float": ("转浮点。value 无法解析时回落 fallback（不置 error，2026-09-27 起）。",
              "参数：value, fallback → number。实现：lang/builtins.c"),
    "bool": ("转布尔。参数：value → bool。实现：lang/builtins.c"),
    "stringify": ("值的可读序列化（调试用）。参数：value → string。实现：lang/builtins.c"),
    "len": ("长度（列表/映射/字符串）。参数：value → number。实现：lang/builtins.c"),
    "keys": ("映射的键列表。参数：map → list[string]。实现：lang/builtins.c"),
    "get": ("映射取值；键缺失时回落 fallback。",
            "参数：map, key, fallback → any。实现：lang/builtins.c"),
    "put": ("映射写值（可变）。参数：map, key, value → map。实现：lang/builtins_fs.c"),
    "push": ("列表追加（DSL 层，2026-09-27 起；list 可变）。",
             "参数：list, item → list。实现：lang/builtins.c"),
    "range": ("整数区间 [start, end)。参数：start, end → list[number]。实现：lang/builtins_hof.c"),
    "map": ("映射/列表变换。参数：fn, list_or_map → list。实现：lang/builtins_hof.c"),
    "filter": ("过滤。参数：fn, list_or_map → list。实现：lang/builtins_hof.c"),
    "reduce": ("归约。参数：fn, list, initial → any。实现：lang/builtins_hof.c"),
    "json": ("解析 JSON 文本；解析失败置 VM error（handler 会 500，用 try() 兜底）。",
             "参数：text → any。实现：lang/builtins.c"),
    "now": ("当前时间戳（秒）。参数：无 → number。实现：lang/builtins.c"),
    "env": ("读环境变量；未设置或敏感变量名返回 null（与缺失同形，判断认证开关用 null 比较）。",
            "参数：name → string|null。实现：lang/builtins_fs.c"),
    "strftime": ("时间戳格式化。参数：fmt, ts → string。实现：lang/builtins_fs.c"),
    "files": ("目录/文件清单。参数：path → list[string]。实现：lang/builtins_fs.c"),
    "read_file": ("读文件；不存在/不可读返回 null（文件过大置 VM error）。",
                  "参数：path → string。实现：lang/builtins_fs.c"),
    "write_file": ("写文件（原子写，权限 0600）。参数：path, content → 无。实现：lang/builtins_fs.c"),
    "mkdir": ("建目录（含父目录）。参数：path → 无。实现：lang/builtins_fs.c"),
    "lock_file": ("flock 加锁（按路径）。参数：path → 无。实现：lang/builtins_fs.c"),
    "unlock_file": ("释放 flock 锁。参数：path → 无。实现：lang/builtins_fs.c"),
    "sql_query": ("SQL 查询（sqlite 默认后端；WITH_PG 时为 PG）。参数绑定防注入。",
                  "参数：sql, params → list[map]。实现：lang/builtins_sql.c"),
    "sql_write": ("SQL 写操作（参数绑定防注入）。",
                  "参数：sql, params → 影响行数。实现：lang/builtins_sql.c"),
    "try": ("捕获函数内 VM error，返回 {ok: 结果} 或 {err: 消息}（2026-09-27 起）。",
            "参数：fn（函数引用）。实现：lang/builtins.c"),
    "tools": ("已注册工具名列表（含 DSL tool / MCP，排序）。参数：无 → list[string]。实现：lang/builtins_catalog.c"),
    "skills": ("已索引技能 {name, desc} 列表（排序）。参数：无 → list[map]。实现：lang/builtins_catalog.c"),
    "mcps": ("MCP 条目（args 已脱敏 <redacted>，不泄露路径/凭据）。",
             "参数：无 → list[map]。实现：lang/builtins_catalog.c"),
    "discovery_endpoints": ("发现端点（tools/skills/mcps 的 HTTP 路由信息）。参数：无 → list[map]。实现：lang/builtins_catalog.c"),
    "catalog": ("工具/技能/MCP 目录（聚合）。参数：无 → map。实现：lang/builtins_catalog.c"),
    "el": ("创建 vdom 元素。参数：tag, attrs, children → vdom。实现：lang/vdom.c"),
    "render": ("渲染 vdom 为 HTML。参数：vdom → string。实现：lang/vdom.c"),
    "html": ("HTML 文本（自动 XSS 转义）。参数：text → string。实现：lang/vdom.c"),
    "replace": ("字面量全局替换：replace(s, from, to) 把 s 中所有 from 替换为 to。",
                "from 为空或无匹配 → 返回原串；UTF-8 按字节序列匹配（中文可用）。",
                "实现：lang/builtins_str.c"),
    "crypt_sha512": ("SHA-512 crypt($6$) 口令哈希，salt 由 pid + 秒级时间拼成（$6$xxxxxxxxxxxx$ 格式）。",
                     "参数：password → string。平台差异：glibc/Linux 的 crypt(3) 支持 $6$ 正常生成；",
                     "macOS 的 crypt(3) 只有 legacy DES，拿不到 $6$ 时置 error（不静默降级）；无 crypt() 的",
                     "平台同样置 error。实现：lang/builtins_crypt.c"),
    "abs": ("绝对值。参数：x → number。非数值参数 / 缺参置 VM error。实现：lang/builtins_math.c"),
    "sqrt": ("平方根。参数：x → number。x < 0 置 error（sqrt(): negative argument）。实现：lang/builtins_math.c"),
    "exp": ("e 的 x 次幂。参数：x → number，精度为 C double。实现：lang/builtins_math.c"),
    "log": ("自然对数，与 ln 同一实现。参数：x → number；x <= 0 置 error（log(): argument must be positive）。",
            "注意是 ln 而非 log10。实现：lang/builtins_math.c"),
    "ln": ("自然对数，log() 的别名。参数：x → number；x <= 0 置 error（报错文案仍写作 log()）。",
           "实现：lang/builtins_math.c"),
    "pow": ("幂运算。参数：x, y → number；结果为非有限值（如 0 的负次幂）时置 error（pow(): result not finite）。",
            "实现：lang/builtins_math.c"),
    "floor": ("向下取整（朝 -∞）。参数：x → number。实现：lang/builtins_math.c"),
    "ceil": ("向上取整（朝 +∞）。参数：x → number。实现：lang/builtins_math.c"),
    "round": ("四舍五入，半值远离零（round(0.5)=1、round(-0.5)=-1，与 floor(x+0.5) 不同）。",
              "参数：x → number。实现：lang/builtins_math.c"),
    "min": ("最小值，可变参数（≥1 个数值，逐个比较）。参数：nums... → number；缺参或非数值置 error。",
            "实现：lang/builtins_math.c"),
    "max": ("最大值，可变参数（≥1 个数值，逐个比较）。参数：nums... → number；缺参或非数值置 error。",
            "实现：lang/builtins_math.c"),
}

# 分组（语义聚合；顺序大体跟随 interp.c 注册表，便于对照阅读）
GROUPS = [
    ("模块", ["run"]),
    ("输出与类型转换", ["print", "str", "int", "float", "bool", "string", "stringify"]),
    ("字符串变换", ["replace"]),
    ("密码哈希", ["crypt_sha512"]),
    ("集合操作", ["len", "keys", "get", "put", "push", "range", "map", "filter", "reduce"]),
    ("JSON / 时间 / 环境", ["json", "now", "env", "strftime"]),
    ("文件系统", ["files", "read_file", "write_file", "mkdir", "lock_file", "unlock_file"]),
    ("数据库（sqlite / PG 后端）", ["sql_query", "sql_write"]),
    ("异常捕获", ["try"]),
    ("数值计算", ["abs", "sqrt", "exp", "log", "ln", "pow",
                  "floor", "ceil", "round", "min", "max"]),
    ("Agent 运行时（工具/技能/MCP）", ["tools", "skills", "mcps", "discovery_endpoints", "catalog"]),
    ("DOM / 渲染", ["el", "render", "html"]),
]

missing = [n for n in names if n not in DOC or n not in IMPL or n not in SIG]
extra = [n for n in DOC if n not in names]
if missing or extra:
    sys.exit(f"清单不一致: missing={missing} extra={extra}")

lines = [
    "// Lume 内置函数声明 —— 由 C 运行时提供，非 .lume 源码定义。",
    "// 本文件仅供编辑器“跳转到定义 / 大纲”参考（Ctrl+Click 内置函数跳到这里），",
    "// 不会被编译或 import。每个函数附行为说明与实现位置。",
    f"// 生成来源：interp.c bridge_seed_builtins() 注册表（{len(names)} 个）。",
    f"// 生成脚本：editor/lume-vscode/scripts/gen-builtins.py（lume 加内置后重跑即可）。",
    "",
]
for title, group in GROUPS:
    lines.append(f"// ── {title} ──")
    for n in group:
        doc = DOC[n]
        if isinstance(doc, tuple):
            body = "\n//   ".join(doc)
        else:
            body = doc
        lines.append(f"// {body}")
        lines.append(f"func {SIG[n]} {{}}  // 实现: {IMPL[n]}")
        lines.append("")

open(OUT, "w", encoding="utf-8").write("\n".join(lines).rstrip() + "\n")
print(f"builtins.lume 已生成：{len(names)} 个内置函数（{OUT}）")
