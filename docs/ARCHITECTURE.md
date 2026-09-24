# Lume 架构文档

本文从**系统构成与设计决策**的视角解释 Lume：它是怎么被拆成几层的、进程
和内存模型长什么样、一次 HTTP 请求从连接到响应的完整路径、配置与部署拓扑、
以及当前的安全边界。语言用法见 [LUME.md](LUME.md)（用户指南），代码级约定
与"怎么加功能"见 [DEVELOPMENT.md](DEVELOPMENT.md)（开发文档）；本文与
两者的关系见文末[文档地图](#文档地图)。

---

## 1. 系统全景

Lume 是**一个 C11 单二进制**：内嵌一门可强类型脚本 DSL（`src/`），静态链接
兄弟项目 agent-httpd 的嵌入库 `libagenthttpd.a`（git submodule，见
[3.3](#33-嵌入运行时-agent-httpd)），成为一个自带静态前端 docroot 的完整
HTTP 服务器。一个容器 = 整站：静态资源 + DSL 路由 + JSON API +
`/react/api/chat` SSE 聊天，不需要 nginx / FastCGI / 独立 React 后端。

```
┌──────────────────────────── Lume 单二进制 (bin/lume) ────────────────────────────┐
│                                                                                   │
│  语言前端 (src/，宿主 C11)         运行时代理           嵌入运行时 (libagenthttpd.a) │
│                                                                                   │
│  .lume 源码                                                                        │
│    │  lexer.c  ──>  Token[]                                                       │
│    │  parser.c ──>  AST (递归下降)                                                │
│    │  typecheck.c ─> 编译期强类型 (首错即停)                                       │
│    │  interp.c  ──>  树遍历解释器 (VM: 值栈+GC+jmp_buf)                            │
│    │                   │                                                          │
│    ▼                   ▼                                                          │
│  bridge.c  ──  DSL 世界 <-> agent-httpd 世界 (注册表快照 + shim)                    │
│    │  agenthttpd_route / agenthttpd_tool / agenthttpd_run                          │
│    ▼                                                                              │
│  agenthttpd_run(&cfg) ── fork 后开始伺服 ──────────────────────────────┐          │
│                                                                        │          │
│  ┌───────────────── 每 worker 进程 (fork 继承的 VM 副本) ─────────────┐ │          │
│  │ HTTP 连接 ─> 路由匹配 ─> shim 调用 DSL handler ─> 序列化响应       │ │          │
│  └───────────────────────────────────────────────────────────────────┘ │          │
└───────────────────────────────────────────────────────────────────────────────────┘
                                     │
                www/ docroot (esbuild 分包产物 + Tailwind CSS，纯静态)
```

### 1.1 关键设计决策

| 决策 | 理由 |
|---|---|
| **C11 单二进制** | 无运行时依赖、启动快、部署即一个文件；`--check` 可离线静态校验 |
| **DSL 而非 YAML/JSON 配置** | 路由/工具/服务器参数需要表达式、类型与复用能力；强类型在编译期拦截错误 |
| **静态链接 libagenthttpd.a** | HTTP 服务器、agent 工具、LLM/会话、安全层全部复用，Lume 只写 DSL 语义 |
| **submodule 锁定版本** | agent-httpd 的 gitlink 固定到具体 commit（当前 `a99d492`），构建可复现；升级需显式改 gitlink |
| **VM 每进程一份，fork 前初始化** | worker 只读共享不可变 AST/Type，请求间复用 VM，无锁共享 |

---

## 2. 进程与运行时模型

### 2.1 生命周期

```
main.c: parse_program ──> type_check_program ──> 解释执行 .lume 顶层
                                                    │ (route/tool/run 注册)
                                                    ▼
                                    run() ──> bridge: agenthttpd_run(&cfg)
                                                    │ fork 前注册窗口关闭
                                                    ▼
                            agent-httpd 接管：fork-per-connection 伺服
```

- `agenthttpd_run` 之前是**注册窗口**：DSL 声明的路由、工具、`server{}` 参数
  经 bridge 转成 `agenthttpd_route / agenthttpd_tool / agenthttpd_config`；
  一旦 run 启动，注册关闭（运行时只读表快照）。
- `server{workers=N}` 决定 worker 数；默认 fork-per-connection。
- 每个 worker **继承一份 VM 副本**（`bridge.c` 假设初始化发生在 fork 前）；
  shim 处理完一个请求后调用 `vm_after_request` 清 `error/stack/call_result`，
  使 VM 可在下一请求复用。

### 2.2 CLI 形态（main.c）

| 参数 | 行为 |
|---|---|
| `bin/lume x.lume` | 解析 → 类型检查（永远执行）→ 解释执行 |
| `--check` | 类型检查后即退出，零副作用（`make check` 遍历全部示例） |
| `--dump` | 打印 AST 后退出 |
| `--watch` | 开发热重载：校验新编辑有效才重启服务子进程；无效编辑保留旧服务（父子进程 + SIGUSR1/SIGINT/SIGTERM 信号驱动） |

所有启动型 make 目标第一步执行 `KILL_SERVER` 宏：按端口 + 进程名杀掉旧
`bin/lume`（含 `--watch` 形态），轮询直到端口释放再绑定，避免
"Address already in use"。

### 2.3 内存模型（VM 侧）

- **值栈即 GC 根**：每个 `eval_expr` 结束正好留一个值在 `vm->stack`；GC
  （mark-sweep）在分配前按 `vm->gc_threshold`（默认 2MB）触发，临时值因此
  都是活跃根。改代码时不得让值"悬空"（先 pop 再分配同类对象）。
- **AST/Type 生命周期 = 进程**：一次性解析、永不 free，worker 共享不可变树。
- **返回展开**：用户函数调用前 push `jmp_buf`，`return`/`?` 传播填
  `vm->call_result` 后 `longjmp` 回 `jump_depth-1`；正常/长跳两条路径都恢复
  `jump_depth` 并停用 frame。

---

## 3. 分层架构

### 3.1 语言前端管线（src/）

| 文件 | 职责 |
|---|---|
| `lume.h` | 全部公共头：TokenType、Type、Node、Value/Obj/GC、VM、桥接原型 |
| `token.c` | 枚举 → 名字表（错误信息用） |
| `lexer.c` | 源码 → Token 数组 |
| `parser.c` | Token → AST（递归下降），含类型标注/`type` 声明/`?` |
| `typecheck.c` | 静态类型检查器 + Type 类型对象/构造器 |
| `value.c` | Value/Obj、GC、map/env/string、JSON 编解码 |
| `interp.c` | 树遍历解释器（值栈 + jmp_buf 返回展开）+ 内建函数种子 |
| `bridge.c` | DSL ↔ libagenthttpd 翻译层（route/tool/run shim、内置函数注册） |
| `main.c` | CLI：`--check` / `--dump` / `--watch` / 直接执行 |
| `iquest.c` / `iquest.h` | 投资助手产品 API 层（见 3.4） |

类型系统要点：基本类型 `int/float/string/bool/null/Result` **全部可空**；
`int→float` 隐式加宽，其余无隐式转换（无 JS 式 truthiness）；具名结构体
字面量字段必须齐全且不多余；类型检查**第一条错误即停**。

### 3.2 桥接层（bridge.c）

DSL 世界（Value/Node/VM）与 agent-httpd 世界（C 路由/工具）的翻译：

- 路由注册：`route/get/post/.../verbs` 声明 → `agenthttpd_route(method, path, handler-shim)`；
  `write "/p"` 的 handler 缺省用 `vm->default_handler`（`native_default_route`）。
- 工具注册：`tool "name", "desc", params, func` → `agenthttpd_tool(...)`；
  参数 schema 经 `upgrade_tool_params` 把裸类型关键字升级为 `{"a":{"type":"int"}}`。
- 内建函数种子：`run/print/str/int/float/bool/string/len/keys/get/json/stringify/now`
  以及发现类 `env/files/read_file/tools/skills/mcps`（后三者枚举 agent-httpd
  的注册表与 `.data/mcp-servers-router.json`）。
- shim 守则：返回 0 = 已处理（框架负责序列化）；不设 `res->handled`
  （该标志跳过响应序列化，只用于自流式 handler）。

### 3.3 嵌入运行时 agent-httpd

git submodule（`agent-httpd/`，gitlink 锁定），编译为 `bin/libagenthttpd.a`，
Lume 静态链接。嵌入 API（`src/agenthttpd.h`）：

- `agenthttpd_config`：零值 = 默认（NULL/0 字段走框架默认）；
- `agenthttpd_route(method, path, handler)`：注册自定义路由；
- `agenthttpd_tool / agenthttpd_tool_exec`：注册进程内 C 工具 / 外部进程工具；
- `agenthttpd_tool_allowed`：白名单过滤（供 Lume 侧 `HARNESS_TOOLS_ALLOW` 收敛）；
- `agenthttpd_run(&cfg)`：阻塞至进程生命周期结束。

内部模块（`src/{core,http,agent,security,cgi}`）：HTTP 解析/连接、路由核心
（router.c，写 `.data/mcp-servers-router.json` 的 MCP 同步）、agent/工具/技能/
会话/LLM、安全与 CGI 宿主。

### 3.4 产品层 iquest.c

invest 示例的产品 API，注册成本地 HTTP 端点（`iquest_register()` 必须在
`agenthttpd_run()` 前调用）：

| 端点 | 作用 |
|---|---|
| `GET /api/reports` | 周报归档列表（大小/时间/摘要预览） |
| `GET /api/reports/<name>` | 单篇周报正文 |
| `GET /api/settings` | 审批开关 + provider 当前值（**敏感字段脱敏后返回**） |
| `POST /api/settings` | 写 frameworks 的 `.env`（**限白名单键**） |

数据落点由环境变量覆盖：`IQUEST_REPORTS_DIR`（缺省 `.data/reports`）、
`IQUEST_ENV_FILE`（缺省 NULL = 拒绝写回，见[安全边界](#6-安全边界)）。

### 3.5 前端资产层（frontend/ → www/）

- `frontend/src/<example>/`：每示例一个 React（TypeScript + Tailwind）入口；
  共享层 `theme.css` / `app.css`。
- 一次 esbuild `--splitting` 跑出：`www/<example>/*.js`（入口 bundle）+
  `www/chunk-*.js`（共享 React chunk，全站一份）+ `www/*.css`（Tailwind）。
- `www/` 即 docroot：静态伺服规则为 `/` → `index.html`，**无扩展名路径回退
  到同名 `.html`**（`GET /items` → `www/hello/items.html`）；带扩展名或没有
  同名文件则 404。
- 依赖用 pnpm 管理（`frontend/pnpm-lock.yaml`）；`make ui` 构建全部，
  `make ui-items` 只构建 hello 示例。

---

## 4. 关键路径时序

### 4.1 一次普通请求（如 `GET /hello`）

```
连接进入 ──> fork worker（继承 VM 副本）
          ──> agent-httpd 路由匹配 "GET /hello"
          ──> bridge shim：构造 req map（method/path/label/...）
          ──> interp 求值 DSL handler（return { status, type, body }）
          ──> bridge 把 Value map 翻译成响应结构
          ──> vm_after_request 清 VM 状态
          ──> 序列化响应，连接回收
```

### 4.2 工具 / 技能 / MCP 调用路径

```
DSL handler 调 tool("name", arg) / skill-run / MCP
  ──> interp 内建 → bridge → agent-httpd 工具注册表（进程内 C / 外部进程）
  ──> 白名单过滤：HARNESS_TOOLS_ALLOW / HARNESS_SKILLS_ALLOW / MCP_ALLOW
      （未列出的工具/技能/MCP 不注册、不 spawn，见 Makefile invest/hub 目标）
  ──> MCP 服务器的同步配置写入 .data/mcp-servers-router.json（0600）
```

### 4.3 `/react/api/chat` SSE

聊天走 SSE 事件信封（事件格式细节见 LUME.md）；handler 通过 agent-httpd 的
LLM/会话层接真实模型，流式事件逐条推送。

---

## 5. 配置与部署拓扑

### 5.1 配置优先级

```
.lume 字面量 > .env / 环境变量 > 框架默认（端口 18080、fork-per-connection、docroot ./www）
```

想由环境变量接管端口/worker，就**别在 `server{}` 里写该字段**（字面量优先
无法覆盖）。`.env` 在 CWD 读取，和 LLM 配置共用同一文件。

### 5.2 环境变量族

| 变量 | 用途 |
|---|---|
| `HARNESS_SKILLS_ALLOW` | 技能白名单（逗号分隔，空=全部） |
| `HARNESS_TOOLS_ALLOW` | Agent 工具白名单（空=全部） |
| `MCP_ALLOW` | MCP 服务器白名单（未列出不 spawn 不注册） |
| `IQUEST_REPORTS_DIR` | invest 周报归档目录（缺省 `.data/reports`） |
| `IQUEST_ENV_FILE` | invest settings 写回的目标 env 文件（缺省 NULL=拒绝写回） |
| `PORT` / `HUB_PORT` | dev / invest / hub 启动端口 |

### 5.3 产品档（product profile）约定

每个示例 = 一个 `examples/xxx.lume` + 一个 make 目标，用环境变量把运行时
收敛到该任务所需的一小撮能力：

- `make invest`（:8082）：技能 `weekly-investment`；工具 11 个（
  `skill-run,read_file,write_file,get_time,query_exchange_rate,fetch_url,recall,
  remember,portfolio_get,portfolio_add,portfolio_remove,report_generate`）；
  MCP 5 个（`portfolio-check,pse-review,fs,think,memory`）。
- `make hub`（:8083）：网关能力台，整本网关目录全开（5 技能 / 10 工具 / 5 MCP）。
- `make dev`（:8081）：demo 试验台；`make dev-minimal`（:8082）：hello 最小入门。

### 5.4 容器部署

- build context 是 **lume/ 仓库根**（含 agent-httpd submodule）：宿主那份
  `libagenthttpd.a` 是 Mach-O（macOS 上 make 编的），Linux 容器不能链接，
  必须在容器内把 lib 和 `bin/lume` 都重编。
- compose 每个示例一个 service，共用同一棵 docroot，只换
  `examples/<name>.lume` + 端口 + 白名单 env。

---

## 6. 安全边界

当前实现的安全约定（隐私审查修复后）：

| 层 | 措施 |
|---|---|
| 文件权限 | `.data/` 700；`.data/mcp-servers*.json` 600（router.c `fchmod`）；`native_mkdir` 0700 |
| settings API | `GET /api/settings` 对 `env_file`/`LLM_API_URL`/`ROUTER_API_URL` **redact**（"configured"/null，`LLM_MODEL` 保留原值）；`POST` provider 含控制字符（<0x20/0x7f）→ 400；无 `IQUEST_ENV_FILE` → 写回 500 拒绝 |
| 环境变量 | `IQUEST_ENV_FILE` 缺省 NULL——现有 `.env` 未显式配置时 invest 设置页写回会被拒绝（属预期行为） |
| 能力收敛 | 白名单三件套（skills/tools/MCP）把运行时收敛到示例所需能力 |

完整风险分层与合规待办见隐私审查报告（`skills/` 下或开发文档相关段落）。

---

## 7. 跨模块一致性约束

以下约定横切多个文件，改任一侧前必读（详细版见 DEVELOPMENT.md
「解释器核心约定」）：

1. 值栈即 GC 根；`map_set/env_set` 内部自根。
2. 函数调用契约：先压 callee 再压 argc 个实参 → `call_function` → 单结果。
3. 返回展开 / `?` 传播共用 `jmp_buf` 机制。
4. AST/Type 进程级不可变共享，永不 free（typecheck 直接借用 `Type**`）。
5. bridge 假设初始化在 fork 前；shim 处理完必须清 VM 状态供复用。
6. 平台 feature-test（`-D_GNU_SOURCE` 等）与 agent-httpd/Makefile 逐字同款，
   宿主 make 与容器内重编行为必须一致。

---

## 8. 文档地图

| 文档 | 视角 | 读者 |
|---|---|---|
| [LUME.md](LUME.md) | 语言与业务开发：怎么写 `.lume`、路由/工具/页面/聊天 | 业务开发者 |
| [DEVELOPMENT.md](DEVELOPMENT.md) | 代码级开发：目录结构、解释器核心约定、怎么加功能、测试、已知坑 | 维护者 |
| **ARCHITECTURE.md（本文）** | 系统构成与设计决策：分层、进程/内存模型、关键路径、部署、安全边界 | 架构评审 / 新成员入门 |
| README / README.zh | 项目入口：快速开始、目录、容器、安全约定 | 所有人 |

数据流口诀：`lexer → parser(AST) → typecheck(compile-time) → tree-walk
interpreter → bridge → agenthttpd`。
