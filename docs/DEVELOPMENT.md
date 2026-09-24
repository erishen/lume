# Lume 开发文档

Lume 是一门内嵌的可强类型脚本 DSL，宿主语言为 C11，运行时驱动
[agent-httpd](../agent-httpd/) 的静态链接库 `libagenthttpd.a`，用来配置
HTTP 路由、注册 agent 工具、声明服务器参数，然后调用 `run()` 把进程交给
agent-httpd 的嵌入接口。

```
lexer -> parser(AST) -> type checker(compile-time) -> tree-walk interpreter -> bridge -> agenthttpd
```

---

## 目录结构

```
lume/
├── src/
│   ├── al.h          # 全部公共头：tokens、Type、Node、Value/Obj/GC、VM、桥接原型
│   ├── token.c       # 枚举 -> 名字表（错误信息用）
│   ├── lexer.c       # 源码 -> Token 数组
│   ├── parser.c      # Token -> AST（递归下降），含类型标注/`type` 声明/`?`
│   ├── typecheck.c   # 静态类型检查器 + Type 类型对象/构造器
│   ├── value.c       # Value/Obj、GC（mark-sweep）、map/env/string、json 编解码
│   ├── interp.c      # 树遍历解释器（value stack + jmp_buf 返回展开）+ 内建函数
│   ├── bridge.c      # DSL <-> libagenthttpd 翻译层（route/tool/run shim）
│   └── main.c        # CLI：--check / --dump / 直接执行脚本
├── examples/         # 每个是一个完整示例；invest/hub/hello 各带前端
│   ├── demo.lume         # E2E 试验台（SSR + 路由 + 工具），:8081，make dev
│   ├── invest.lume       # 投资助手产品，:8082，make invest
│   ├── hub.lume          # 网关能力台，:8083，make hub
│   ├── hello.lume        # 最小入门（含 /items React 页），:8082，make dev-minimal
│   └── lang-basics.lume  # 纯语言脚本（无 run()），run_all 用它断言输出
├── frontend/src/      # 每 example 一个子目录（src/<name>/ + 共享层）
│   ├── theme.css         # @theme token，共享层（app.css / items.css 都 @import）
│   ├── app.css           # 共享壳样式表（所有 www 壳都链接 /app.css）
│   ├── invest/           # ← invest.lume:Dashboard/Chat/Reports/Settings + markdown/report-utils
│   ├── hub/              # ← hub.lume:hub-home / hub-catalog / hub-discovery 三入口包 + hub-shared
│   └── hello/            # ← hello.lume:items 单页（独立 IIFE 包）+ items.css
├── www/                # docroot 产物；同样按 example 分子目录，URL 由 views 顶成根路径
│   ├── app.css          # 共享壳样式（所有壳都链接 /app.css）
│   ├── chunk-*.js       # esbuild 分包出的共享 React chunk（全站只此一份，缓存复用）
│   ├── invest/          # chat 的聊天 bundle 也住这（app.js），hub 的 /chat 复用
│   ├── hub/  hello/    # 每示例自己的壳 <html> 和 bundle <js>
├── tests/
│   ├── smoke.c        # 解释器 + 类型检查单测（49 check + 11 reject = 60）
│   ├── tools_driver.c # 工具注册 + tools_dispatch JSON 往返
│   └── run_all.sh     # 端到端：构建、单测、live HTTP、300 请求 GC 压测
├── Makefile          # make / check / dump / dev / invest / hub / test / ui
└── bin/lume          # 编译产物
```

---

## 编辑器支持

VS Code 用本地扩展 `editor/lume-vscode/` 提供 `.lume` 语法高亮/注释/括号配对。
安装:软链到扩展目录后重载窗口(安装与维护详见该扩展的 README.md):

```bash
ln -s "$PWD/editor/lume-vscode" ~/.vscode/extensions/cnb.lume-0.1.0
```

## 常规操作

```bash
make                  # 构建 bin/lume（缺 lib 时自动编译 ../agent-httpd）
make check            # 类型检查 demo.lume
make dump             # 打印 AST
make dev              # demo → http://localhost:8081（阻塞, Ctrl-C 停）
make invest           # 投资助手 → http://localhost:8082（白名单见 INVEST_* 变量）
make hub              # 网关能力台 → http://localhost:8083（HUB_* 变量）
make hub-watch        # --watch 热更新（invest-watch 同理）
make test             # 全部测试
make clean            # 清理 build/ 和 bin/
```

> 每个启动目标的第 1 步(`KILL_SERVER` 宏)都会先清场:杀掉当前监听着目标端口的
> 进程 + 仍伺服该示例的旧 `bin/lume`(含 `--watch` 形态),并轮询直到端口真正
> 释放才绑定,因此可反复 `make hub` / `make invest-watch` 切换形态而不报
> "Address already in use"。

示例:`examples/demo.lume`(UI + JSON API 服务器)、`examples/lang-basics.lume`
(纯语言脚本,无 run(),打印后退出,可在 `main.c` 看到它的宽松退出)。

`main.c` 的 CLI 规则：先 `parse_program`，再 `type_check_program`（永远执行），
然后解释执行；`--check` 在类型检查后即退出（不产生副作用）。

---

## 语言速览

```lume
// 结构体声明（编译期类型，运行时就是 map）
type User = { name: string, age: int };

server {
  port = 8081;
  workers = 4;
}

// func 参数/返回可选标注；未标注 = “宽松”(any)，route/tool 回调即如此
func greeting(u: User): string {
  return "hi, " + u.name + " (age " + str(u.age) + ")";
}

route "GET", "/hello", func(req) {
  return { status: 200, type: "text/plain", body: "hi" };
};

// 方法缩写 + 箭头函数:`get "path", fn` ≡ `route "GET", "path", fn`
// (get/head/post/put/patch/delete/options 七个关键字; `(a,b)=>{}` ≡
//  `func(a,b){}`，但箭头不支持返回类型标注、函数体必须是块)
get "/ping", (req) => { return "pong"; };

// `write`/`read` 是内置方法组（在 bridge_seed_builtins 里预置），无需声明：
// `write "/p", fn` 一次注册 POST/PUT/PATCH/DELETE，req.label 为约定标签。
write "/items", (req) => { return { method: req.method, action: req.label }; };

// handler 可省略：`write "/p";` 用 vm->default_handler（native_default_route，
// 返回 { action, method, got }，见 interp.c / value.c 的 GC root）。
write "/ack";

// 自定义方法组用 `verbs` 声明（见 al.h TOK_VERBS/N_VERBS、interp.c N_ROUTE alias
// 分支）：列表 = 纯方法；映射 = 方法 -> 标签，标签经 RouteRec.label 注入 req.label
verbs crud = { POST: "created", DELETE: "removed" };
crud "/things", (req) => { return { method: req.method, action: req.label }; };

tool "greet", "say hi", { name: "string" }, func(arg) {
  return { msg: "hi " + arg.name };
};

// Result 错误处理（无泛型：{ok:..}/{err:..} 单键 map 即 Result）
func maybe(a: int): Result {
  if (a > 0) { return { ok: a }; }
  return { err: "negative" };
}
func use(): Result {
  let v = maybe(7)?;   // `?` 解包 ok；遇到 err 直接向上传播
  print(str(v));
  return { ok: v };
}

run();   // 启动服务器（阻塞）
```

### 类型系统
- 基本类型：`int` / `float` / `string` / `bool` / `null` / `Result`；
  全部**可空**（null 可赋任意类型），无需 `?` 后缀。
- 列表：`int[]`（`T[]`）；map 字面量/匿名结构体推断。
- 结构体：`type Name = { f: T, ... }`；具名结构体的字面量必须**字段齐全且不
  多余**。
- 推断：`let x = 5` 推导为 `int`；`5.5` 为 `float`；标注可省略。
- 规则：
  - `int -> float` 隐式加宽，其余无隐式转换（字面量 `5` 可给 `float` 参数）。
  - `if/while/and/or/not` 要求 `bool`（无 JS 式 truthiness）。
  - 类型关键字（`type/int/float/string/bool/Result`）在表达式里仍可作为普通
    标识符（所以 `int("42")` 内建可用）；也可作 map 键 / 字段名（如响应的
    `type` 字段）。
  - 未标注的参数/返回/handler = `any`，永不约束；内建函数全部 `any`。
  - 类型检查在**第一条错误即停**，停止后不执行。

### 内建函数
`run()` `print(...)` `str(...)` `int(...)` `float(...)` `bool(...)` `string(...)`
`len(x)` `keys(m)` `get(m,k[,def])`
`json(s)` `stringify(v)` `now()`（注册在 `interp.c: bridge_seed_builtins`）。
发现类内建：`env(k)`、`files(dir)`、`read_file(path)`、`tools()`、`skills()`、`mcps()`——
`tools()`/`skills()` 枚举 libagenthttpd.a 的注册表（工具:本地内建 + DSL tool +
MCP + router 代理;技能:SKILL.md 索引）,`files()`/`read_file()` 供目录/目录清单
页使用,`mcps()` 读取 router 同步的 `.data/mcp-servers-router.json`
（实现见 `interp.c` 的 `native_env`/`native_files`/`native_read_file`/
`native_tools`/`native_skills`/`native_mcps`,各有对应 smoke 用例）。

`str`/`int`/`float`/`bool`/`string` 被 seed 成 callable 原生（`int("42")` 一直可以，
`float`/`bool`/`string` 同理新增）。带多参数时 = 取值 + 转换：`int(m,"a")` ≡ `int(get(m,"a"))`，
缺键 → 类型零值（int→0、str→字面 `"null"`），可传第三个参数当缺省（见 `interp.c: value_from_map`）。

`tool` 的参数 map 支持**裸类型关键字**：`{ a: int }` 求值时 `int` 是全局里的 native 值，
`N_TOOL` 求值前会把 map 里 native/函数值 rewrite 成其名字字符串，于是序列化成 `{"a":"int"}`，
再走 `bridge.c: upgrade_tool_params` 升级成 `{"a":{"type":"int"}}`（见 `interp.c` 的 `N_TOOL` 分支）。

**tool handler 首参定型（零值缺省）**：`typecheck.c: N_TOOL` 从 params AST 直接推导一个匿名
struct（`tool_param_struct`，把 `int`/`float`/`string`/`bool` 关键字或字符串映射成标量类型），
经 `Checker.param_hint` 一次性喂给 handler `N_FUNC_LIT` 的第一个参数（仅 arity==1）。于是
`N_MEMBER` 命中该 struct 成员时把结果类型写回 `node->as.member.type`；运行时
`interp.c: N_MEMBER` 遇到缺键且 `member.type` 非空 → `vm_zero_value` 返回类型零值
（int/float→0、string→""、bool→false），未定型则照旧报 `map has no field`。params map 的
`ck_expr` 类型不能用作 schema（裸关键字求值为 `any`、引号形式为 `string`），故必须走 AST 推导。

---

## 解释器核心约定（改代码前必读）

1. **值栈即 GC 根**：每个 `eval_expr` 结束正好留下一个值在
   `vm->stack` 上。GC 在分配前触发（`vm->gc_threshold`，默认 2MB），所有
   临时值因此都是活跃根。**不要让值“悬空”（先 pop 再分配同类对象）**。
2. **map_set/env_set 内部自根**：它们会先把新值 push 上栈再 strdup 键，
   避免中间分配回收入参。
3. **函数调用契约**：调用方先压 `callee`，再压 `argc` 个实参（顺序）
   → `call_function(vm, callee, argc)` → 该区间被替换成**单个结果**。
4. **返回展开**：用户函数调用前 push 一个 `jmp_buf`
   （`vm->jump_bufs[vm->jump_depth]`），`return` 填 `vm->call_result` 后
   `longjmp` 到 `jump_depth-1`；`call_function` 在正常/长跳两条路径都恢复
   `jump_depth` 并停用 frame。
5. **`?` 传播**复用同一机制：`interp.c N_CALL` 中若结果 map 有 `err` 键 →
   `vm->call_result = 该map; longjmp(...)`；有 `ok` 键 → 栈顶换成 `ok` 值。
6. **VM 每进程一份**：`bridge.c` 假设初始化发生在 `agenthttpd_run` **fork
   之前**；worker 各自继承 VM 副本。shim 处理请求后调用 `vm_after_request`
   清 `error/stack/call_result` 以便复用于下一请求。
7. **shim 守则**：返回 0 = 已处理（框架负责序列化）；**不要**设
   `res->handled`（那个标志会跳过响应序列化，只用于自流式 handler）。
8. **AST/Type 生命周期 = 进程**：一次性解析，永不 free（worker 共享不可变
   树）；可直接借用 AST 里的 `Type** param_types` 数组（typecheck.c 就是这么做的）。

---

## 怎么加功能

### 加一个内建函数
1. `interp.c` 写 `static void native_xxx(...)`，再包一层 `b_xxx`（NativeFn）。
2. 在 `bridge_seed_builtins` 的 `built[]` 里登记。
3. typecheck.c 的 `BUILTINS[]` 已默认按 `any` 放行，无需改。

### 加一种新语句/关键字
1. `al.h`：加 `TokenType` 枚举项；token.c 好 `TOKEN_NAMES` 的对应名字。
2. `lexer.c`：`KEYWORDS[]` 或标点分支。
3. `parser.c`：`parse_statement` 分支生成新 `NodeType`；`al.h` 扩 AST union；
   `node_print` 加调试输出。
4. `typecheck.c`：`ck_stmt`/`ck_expr` 处理新节点（编译期强类型就在这保证）。
5. `interp.c`：`exec_statement`/`eval_expr` 给运行时语义。
6. `tests/smoke.c` 补一条 `check(...)`（正常路径）和必要时 `reject(...)`
   （类型错误路径）。

### 加一个新内置类型（如 date）
- `al.h TypeKind` / `typecheck.c` 的 `ck_expr` literal 与 `type_compat`、
  `ty_str/tp_inner` 都要同步加分支。

---

## 测试约定

- `make test` 起一个真服务器（默认 :8999，`tests/run_all.sh` 里 PORT），依次：
  1. `make` + `./bin/lume --check examples/demo.lume`
  2. `tests/smoke-bin`（60 项：解析+类型检查+执行，stdout 逐字节比对）
  3. `tests/tools-bin`（工具表 + `tools_dispatch` JSON 往返 + `session` 透传）
  4. live HTTP：GET /hello、GET /sum、POST /echo、
     `POST /react/api/chat`（agent demo SSE）+ 300 次请求 GC 压测
  - run_all 开头 `export LLM_API_KEY=`：即使仓库里有 `.env`，chat 测试也
    永远走**离线 demo 引擎**，不会真的调用外部模型。
- smoke 的 `capture_begin/End` 会用 `dup` 暂存原 stdout 再恢复；新增测试直接
  复用 `check(name, src, expect)` / `reject(name, src, 含的错误子串)`。

---

## UI 层（Lume SSR 壳 + 真 React 客户端）

UI 分两层：**服务端是 Lume**（SSR 壳 + JSON API），**客户端是标准 React**
（frontend/src，TypeScript + JSX 由 esbuild 打包、Tailwind CSS 生成样式，
产物都落进 docroot）。

1. **服务端两套写法，可互相组合**：
   - `el(tag, props, ...children)`（原生内建）构建 vnode 树
     `{type, props, children}`；`render(tree)`（原生内建）一次性序列化整棵树
     为 HTML。
   - `html("...{0}...{1}...", a, b)`（原生内建）是**模板字符串**，`{N}` 按
     位置替换第 N 个实参。语义（`src/interp.c` 的 `html_slot`）：
     - **独立标量槽**：字符串/数字/bool/null/map → 转义后按文本输出（默认
       安全，用户数据放这里）。
     - **列表槽**：视为子节点序列（children idiom）——vnode 结构性渲染、
       字符串原样输出（它们是模板产物，内部已转义标量）。
     - vnode 槽：结构性渲染、不转义。
     - `{{` / `}}` 输出字面 `{` / `}`。
   - 组件函数**刻意不加返回类型**（loose/any——UI 是动态结构），如 `nav()`
     `card()` 返回 vnode、`page()` 返回 `html()` 字符串；`route "GET", "/",
     home_page` 直接引用。函数参数定长，页面内容用**列表**打包给
     `page(title, id, [...])`。
2. **render()/html() 的序列化规则**（`src/interp.c` 的 `render_value`）：
   - 文本与属性值一律 **HTML 转义**（`& < > "`）。
   - `on*` 事件属性在 SSR **丢弃**（事件只活在客户端）。
   - `data_page` → `data-page`（下划线转连字符，保持 Lume 标识符合法；
     在 `html()` 模板里可以直接写 `data-page='...'`，无需转换）。
   - void 标签（`img/link/meta/br...`）不输出闭合标签；`class:false` 不输出；
     bool true 输出 `k="true"`；裸值（非元素 map）回退成转义 JSON。
3. **客户端运行时（真 React + TypeScript + Tailwind，pnpm 管理依赖）**：
   前端按示例分区：`frontend/src/<example>/` 对应一个 `examples/*.lume`，
   `www/<example>/` 是它落到 docroot 的产物（页面壳 `<html>` + 各页入口 bundle）。
   共享层在 docroot 下：`www/chunk-*.js`（React 分包）、`www/app.css`
   （壳样式）`www/invest/app.js`（聊天 bundle），所有示例的壳都直接引用。
   - **一个例子一张或多张页面，一页一入口包**（invest：`dashboard`/`app`/
     `reports`/`settings` 四个入口；hub：`hub-home`/`hub-catalog`/
     `hub-discovery` 三入口包 + `hub-shared`；hello：`items` 独立单页）：
     一次 esbuild 构建把所有入口 `--bundle --splitting --format=esm` 打进
     `www/`——**React 抽成共享 `chunk-*.js`（约 140kb），每页只剩 3~10kb 的
     入口包**，页面壳 `<script type="module" src="/x.js">` 引用根 URL（静态层
     会把它映射进子目录）。首访任一页面后 React chunk 进浏览器缓存，其它
     页与跨示例的聊天页都复用同一个 chunk，不再每页重复下载。
   - `app.js`（invest 的 Chat）跨示例复用：hub 的 `www/hub/chat.html` 也挂
     `/invest/app.js`，`data-page="chat"` 决定挂哪个组件，所以 hub 聊天不需要
     自己的 Chat 包。
   - 构建：`cd frontend && pnpm install && pnpm run build`（或 `make ui`，缺
     依赖自动 `pnpm install`），产出 `www/chunk-*.js` + `www/app.css` +
     `www/invest/*.js` + `www/hub/*.js`；hello 另有 `pnpm run build:items` /
     `make ui-items`。`pnpm run dev` 现在 watch 四个进程（全量 esbuild 分包 +
     app 壳 CSS + items）改任一件 src 即增量重打。
   - **依赖管理**：pnpm（`packageManager: pnpm@9.15.9` + `frontend/pnpm-lock.yaml`，
     不再有 `package-lock.json`）；命令是 `pnpm run <script>`/`pnpm install`。
   - **Tailwind**：`frontend/src/theme.css` 用 `@theme` 定义色板/字体 token，
     `app.css`/`items.css` 各自 `@import` 它。SSR 壳发出的语义类名
     （`.hero`/`.card`/`.msg`…）不能在 C 字符串里换成 utilities，所以放在
     `app.css` 的 `@layer components`（用 `@apply` 由 token 生成）；React 组件
     可直接写 utilities（`items.tsx` 就是纯 utilities）。`source(none)` 关掉
     Tailwind 的自动扫描，避免它去扫 `node_modules`；`app.css` 的 `@source`
     列出各 `<example>/` 的 TSX 和 `../../www/<example>/*.html`。
   - **类型检查**：`pnpm run typecheck`（`tsc --noEmit`，`strict`），`build` /
     `build:items` 会先跑它，所以类型错误会让 `make ui` 直接失败。
   - 这套目录结构有意把 `frontend/` 放在 docroot 之外，`node_modules` 不会
     被框架 serve 出去。
4. **JSON API**：Lume 路由返回一张**不含 `body` 键的 map** 即自动按
   `200 application/json` + 整张 map JSON 化返回（`bridge.c
   result_to_response`）；需要改码/换 MIME 才写 `{status?, type?, body}`
   （body 非字符串时自动 JSON 化，无需 `stringify`）。返回裸字符串仍走
   `text/html`。客户端 `fetchJSON()` 消费。静态资源（css/js/html）由框架从
   `server{ docroot="./www"; }` 伺服；框架的 static handler 还实现了
   **无扩展名路径回退到 `.html`**（`GET /items` → `www/hello/items.html`，
   `../agent-httpd/src/http/static.c`），所以静态页面 URL 可以省掉 `.html`。
   **`views`(可选)**：`server{ views = "<名字>"; }` 在 docroot 底下再挂一层
   页面子目录。分发顺序固定是 **DSL 路由表 → views 静态根 → docroot 静态根**
   （`../agent-httpd/src/http/http_route.c`：`handle_views_file` 0 已服务 /
   -1 已拒绝 / 1 未中 → 回落 `handle_static_file`）。views 根里的文件以根 URL
伺服：`/dashboard.js` 命中 `www/invest/dashboard.js`、`/hub-home.js` 命中
    `www/hub/hub-home.js`,而 views 里没有的共享资产（`chunk-*.js`、
    `/invest/app.js`、`/app.css`）顺着回落 docroot 根。这正是
    "一示例一子目录 + 全站共享层"布局的实现。注意
    DSL 路由仍在 views 之前,所以 JSON 端点（如 `/discovery`）永远赢。
5. **Agent 聊天页 `/chat`**：页面壳由 Lume 的 `chat_page` SSR（只有一个
    `#chat-root` 挂载点 + 表单），真正的 Agent 是客户端 React `<Chat />`
    （`frontend/src/invest/app.tsx`，hub 的 /chat 复用同一 `www/invest/app.js`）：
   - `POST /react/api/chat`（服务器原生 SSE），`fetch` + `ReadableStream`
     逐块解析 `data: {"t":...}` 信封：`note`（状态条/工具调用行）/`delta`（流式
     token，逐字追加进当前 agent 气泡）/`error`/`done`，忽略 `:` 心跳。
   - **会话记忆**：浏览器 `crypto.getRandomValues` 造 `sess-<hex>` 存
     `localStorage`，随 `sessionId` 上行；服务器按 transcript 重放上下文。
     "New session" 按钮清掉旧 id 重启会话；有真 key 时 agent 的工具循环会回调
     Lume 的 `tool`。
   - 跨域 POST 一律 403（框架 CSRF 守卫）；无 `Origin` 的非浏览器调用放行
     （curl 调试用）。

**跨请求状态的关键约束**:agent-httpd 的 GET/HEAD 走 master 快路径、
POST/带 body 走 prefork worker,每进程各有一份 VM。所以 demo 里的可变状态
(`app_state` 全局 map,靠 N_ASSIGN_MEMBER 原地改对象)必须放在 **GET**
路由上,才能保证读写同属 master 的同一 VM(见 demo.lume 顶部注释)。要跨
进程持久就得接真存储,DSL 层不管。

---

## Agent / LLM 接线（libagenthttpd `src/agent`）

Lume 直接复用 agent-httpd 的 agent 能力,不需要在 DSL 里再造一套:

- **已经接上**:
  - **tools**:`tool "名","描述",{...},fn` → `agenthttpd_tool()` 注册;
    agent 的工具循环(`tools_dispatch`)会回调 DSL 函数。
  - **会话透传**:工具调用时把 `session_id`(agent 循环的多轮对话 id)作为
    `session` 键塞进实参 map,DSL 端 `get(a, "session")` 即可取到
    (`src/bridge.c` 的 `tool_shim`;`tests/tools_driver.c` 的 `echo_sid` 验证)。
  - **chat 端点**:`POST /react/api/chat` 由框架层在路由分发前拦截
    (`src/agent/llm.c`),Lume 起的所有 server 天然带这条路径,无需 DSL 代码。
    配合 `/react/api/pse`(POST)、以及 skills/session/mcp 都在框架侧。
- **server 配置的 env 兜底**:`bridge_run()`(src/bridge.c)先把
  `llm_env_init()` 提前(加载 CWD `.env`),再按 **`server{}` 字面量 >
  `PORT`/`WORKERS`/`DOCROOT` 环境变量 > 框架默认**(18080 /
  fork-per-connection / `./www`)填 `agenthttpd_config`。字面量总是赢,
  要让部署环境覆盖就**别在 `.lume` 里写该字段**。
- **LLM 配置(`.env`,由 `bridge_run()` 里的 `llm_env_init()` 从进程 CWD
  加载)**,见 `.env.example`:
  - `LLM_API_URL`(OpenAI 兼容地址)、`LLM_MODEL`、`LLM_API_KEY`、
    `LLM_TIMEOUT`(默认 60s)。
  - **`LLM_API_KEY` 为空 = 离线 demo 引擎**:不发起任何外部调用,在服务器
    进程内生成 canned 回答并按 token 走 SSE 流式返回 —— 正好用来做;
    `curl -N -X POST -d '{"message":"hi"}' localhost:8081/react/api/chat`。
  - env 里已存在的变量(哪怕空字符串)**优先于 .env**,所以
    `export LLM_API_KEY=` 能强行切回 demo 引擎(测试就是这样保证不花钱)。
- **模型回路**:有 key 时 `agent_run()`(agent.c) 走多轮 ReAct 循环(工具
  调用都落在上面的 DSL `tool` 上),连同 skills 索引 + session 记忆一起拼进
  system prompt;每轮完成后把 transcript 持久化到 session 文件。
- **工具 schema（踩过的坑）**:DSL 里写 `tool ..., { name: "string" }` 是
  语法糖,`bridge_define_tool` 会在注册时自动升级成 OpenAI 要求的
  `{"name":{"type":"string"}}`（`upgrade_tool_params`,src/bridge.c）。
  千万别把裸 `"string"` 直接塞进 `properties`——宽松的中继(sensenova)会
  静默放行,但严格网关(如 llm-router 后端的 agnes)会以
  **HTTP 400 "invalid 'parameters' schema"** 拒收,表现为偶发 `[agent
  error] upstream rejected the request (HTTP 400)`。`tests/tools_driver.c`
  断言 schema 里必须出现 `{"name":{"type":"string"}}` 防止回归。

## 已知约定 / 坑

- 端口一律 < 10000（demo :8081、invest/hello :8082、hub :8083、测试 :8999）。
  agent-httpd Docker 体系用的是 18080/18081/18101，**那是兄弟工程自己的端口，别碰**。
- `route`/`tool` 语句在加载期**立即求值**，被引用的函数必须**先定义再注册**
  （`route "GET", "/chat", chat_page` 写在 `chat_page` 函数体前面会运行时
  报 "undefined variable"，路由不注册——纯 `--check` 不会发现，因为那是
  运行期错误）。demo.lume 里 /chat 路由就放在函数定义之后。
- `start server {}` 里 `type` 等关键字可做字段名，但作为顶层变量名会被当作
  类型关键字（保留字语义）。
- 具名结构体返回类型要求字段齐全：`type P={x,y}` 时 `return {x:1}` 是类型
  错误（缺失 `y`）。
- 类型检查会拒绝 `not 0` / `not ""`（不是 bool），如需 truthiness 语义请
  显式转 bool。