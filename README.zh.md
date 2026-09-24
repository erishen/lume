# Lume

[English](README.md) | [简体中文](README.zh.md)

![CI](https://github.com/erishen/lume/actions/workflows/ci.yml/badge.svg)

自足 agent DSL 服务器:业务逻辑写在 `.lume` 脚本里,一个 C11 二进制直接伺服
静态站点 + JSON API + SSE 聊天 + Agent 工具 + SSR 页面。没有 Node 运行时、
没有 nginx、没有独立 React 后端——静态页直接 COPY 进二进制旁,聊天走进程内
SSE,工具注册在进程内。

Lume 是 DSL 层,HTTP/聊天/MCP/会话由 [agent-httpd](agent-httpd/) submodule
提供,静态链 `libagenthttpd.a`。agent-httpd 版本由 gitlink 锁定(见 `.gitmodules`),
clone 用 `git clone --recurse-submodules`,或 clone 后 `git submodule update --init`。

## 快速开始

```bash
make dev          # demo 完整示例            → http://localhost:8081
make dev-minimal  # hello 最小入门           → http://localhost:8082
make invest       # invest 投资助手产品      → http://localhost:8082
make hub          # hub 网关能力台           → http://localhost:8083

make test         # 全部测试:C 单测 + 工具派发 + 真实 HTTP/SSE + GC 压测
make check        # 只做类型检查,不启动
make clean        # 删 build/ 与 bin/
```

`make dev` 起的是 `examples/demo.lume`,浏览器看 `/`(首页)、`/counter`
(服务器状态)、`/chat`(Agent 聊天)、`/hello`(纯 API)。

依赖:编译器(cc)、`node` + `pnpm`(只用于 `make ui` 打前端产物)。前端源码在
`frontend/src/`,产物进 `www/`——`www/` 里的 `*.js` / `*.css` 是构建产物
(已在 `.gitignore` 排除),手写的 HTML 壳 `www/*/index.html` 等才是源码。

## 目录

| 路径 | 内容 |
|---|---|
| `src/` | 词法/语法/类型检查/树遍历解释器 + agent-httpd 桥接,共约 5.5k 行 C11 |
| `examples/` | 6 个 `.lume` 示例(demo / hello / invest / hub / lang-basics / sqlite-write) |
| `frontend/` | React 18 + TS + Tailwind 4 客户端,esbuild `--splitting` 打包 |
| `www/` | docroot:手写 HTML 壳 + 构建产物(混合,勿整体删) |
| `tests/` | C 单测(`smoke.c`) + 工具派发(`tools_driver.c`) + 端到端(`run_all.sh`) |
| `docker/` | 两阶段 Dockerfile(容器内重编 C)+ compose(invest / hub 两个 service) |
| `editor/lume-vscode/` | 本地 VS Code 语法高亮扩展 |
| `docs/` | 完整文档,见下 |

## 文档

- [docs/LUME.md](docs/LUME.md) —— **用户指南**:语言速览(类型/控制流/Result)、
  内建函数、路由与工具注册、`el()`/`html()` 两套页面写法、SSR 序列化细节、
  Agent 聊天接线与 `/react/api/chat` SSE 事件信封、服务器状态铁律。
- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) —— **开发文档**:目录结构、
  解释器核心约定(改代码前必读)、怎么加内建函数/新语句/新类型、测试约定、
  UI 层分层、Agent/LLM 接线、已知约定与坑。
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) —— **架构文档**:系统全景与
  设计决策、进程/内存模型、分层架构、请求生命周期、部署拓扑与安全边界。

## 容器

```bash
cd lume && docker compose -f docker/docker-compose.yml up -d --build
```

单镜像 `lume:latest`,compose 里 `invest`(宿主 `127.0.0.1:18082`)/ `hub`
(`127.0.0.1:18083`)两个 service 各自只换 `examples/<name>.lume` + 端口 +
白名单 env,共用同一棵 docroot。构建 context 是 `lume/` 仓库根(agent-httpd
是 submodule,在仓库内),context 里 `cd lume && docker build -f docker/Dockerfile`
即可——Lume 要静态链 `agent-httpd` 的 `libagenthttpd.a`,而宿主编出来的是
Mach-O,必须在 Linux 容器内重编。

镜像不带 `.env`(由 compose `env_file` 注入),也不带 `.data/` 会话;
`skills/router/` 的同步副本**会**进镜像——它是启动时 `llm-router` 同步的
降级缓存:同步失败时容器仍能用上一次的技能定义,不是可再生产物。git 里仍
忽略它。

## 安全约定

- **访问控制(两层)**:Lume 服务端本身没有鉴权模块,`/api/reports`、
  `/react/api/chat`、`/discovery` 都是端点,聊天里还能经 `fs` 工具读写文件。
  因此:(1) compose 宿主端口绑 `127.0.0.1` 而非 `0.0.0.0`,服务不进 LAN;
  (2) 容器开 Basic Auth —— 密码只写 `.env`(`LUME_AUTH_USER` /
  `LUME_AUTH_PASSWORD`,`.env` 已 gitignore),镜像 entrypoint 在容器启动时
  把它换成 `/app/auth/htpasswd`(bcrypt),`server{}` 里的
  `htpasswd = env("HTPASSWD_FILE")` 读到。实测无凭证
  `401 + WWW-Authenticate: Basic realm="lume"`。框架只认 `$5$`/`$6$`/bcrypt
  强哈希,明文与弱哈希加载即退出。改 `.env` 后 `docker compose up -d` 生效;
  本机 `make invest` 不注这些 env → 取到 null → 认证保持关闭。密码值请只用
  字母数字——`#`/`$` 等字符在 compose 的 env 解析和 `llm_env_init()` 里的
  处理规则不一致,容易静默截断。
- **访问日志不落 query string**(`http_log.c` 显式截断),日志文件 0600;
  请求体从不写日志。
- **模型可及范围收窄**:`read_file` 用 `resolve_within` 锁在 web root;
  `MCP_FS_ROOT` 指到 `.sandbox`,启动时若该根扫进 `.env`/`.data` 会告警;
  MCP 子进程启动前 `unsetenv` 掉 `LLM_API_KEY/URL/MODEL`。
- **会话有 30 天 TTL**(`session_prune_old(30.0)`)自动清理,文件权限 600。
- `GET /discovery` 的 `endpoints` 只报配置状态与模型名,不返回内网 URL;
  MCP 条目的 `args`(可能含本机绝对路径)统一以 `<redacted>` 发布。
- 敏感目录不进 git:`.data/`(会话)、`.sandbox/`(fs MCP 沙箱根)、
  `.env`(密钥,模板 `.env.example` 入库)。
- `examples/invest.lume` 需要 `make invest` 起——白名单 env 只在那里注入,
  直跑 `./bin/lume` 会打印告警并暴露完整能力目录。
- 要让 invest 设置页的审批开关真正管住付费复盘模型,`IQUEST_ENV_FILE` 需指向
  `pse-review` 读取的同一个 `autogen-pse/.env`(未配置时设置写回会被 500 拒绝,
  只读展示不受影响)。
- invest 账本(`.data/portfolio.json`)写入带每进程 flock 锁 + 原子 rename:
  并发 worker 不会互相覆盖丢更新,写一半崩溃也不会留下半截文件。
- **产品 API 同源守卫**:`/api/reports*` 与 `/api/settings`(GET 与 POST)对
  跨源浏览器请求一律 403——别的站点无法从 `localhost:8082` 窃读你的周报
  (防 DNS-rebinding 型窃读);无 Origin 的调用(curl/本机脚本)照常放行。

## SQLite 支持(原生)

SQLite 直接内建进服务器:`agent-httpd` 静态链 libsqlite3
(`src/agent/sqlite_tool.c`),只要 `SQLITE_DB` 指向一个数据库,就注册三个原生
工具——**不需要 Python、没有 MCP stdio 进程,静态容器镜像同样可用**:

- `sql_query` —— 单条只读 SELECT;数据库以 `SQLITE_OPEN_READONLY` 打开,
  即使语句绕过文本校验,写入/DDL 也被物理拒绝。护栏与旧 MCP server 一致:
  单语句、去注释后必须 SELECT 开头、prepare 语法校验、结果上限 200 行。
- `sql_write` —— **选装,默认不开放**:单条写语句:`INSERT`/`UPDATE`/`DELETE`
  (UPDATE/DELETE 必须带 WHERE)或为新建表执行 `CREATE TABLE`。`DROP`/`ALTER`/
  `TRUNCATE`/`VACUUM`/`ATTACH`/`PRAGMA`/`GRANT`/`REVOKE`,以及任何提及
  `portfolio` 镜像表的语句都会被拒绝。它已编入服务器但**不在默认白名单**;
  需要模型建分析表时,把 `sql_write` 加回 `HARNESS_TOOLS_ALLOW`(Makefile
  `INVEST_TOOLS`、compose/k8s)即可。账本本身仍以 `.data/portfolio.json` 为权威。
- `sql_tables` —— 列出表名。
- `sql_schema` —— introspect 表/列/行数/示例值,输出为提示文本。

- **数据**:类型化领域工具(`portfolio_add`/`portfolio_remove`)仍是 JSON 账本的
  权威写入方。`make invest` 每次启动用 `tools/sqlite-migrate.py` 把账本幂等
  重灌成 SQLite 镜像(`.data/lume.db`);portfolio 镜像表天然只读。
- **启用**:`make invest` 设置 `SQLITE_DB=.data/lume.db` 并白名单放行只读
  `sql_*` 工具。容器:设置 `SQLITE_DB`(如经挂载卷指向 `/app/.data/lume.db`)
  即可——compose/k8s 的白名单条目已就位。`make demo-sqlite`
  (examples/sqlite-write.lume,:8084) 是一个可直接跑的写能力演示——自带
  独立聊天 UI(`www/sqlite-write/`,不依赖 invest 前端),该 profile 额外放行
  `sql_write`,问模型建一张分析表即可看到受检写循环。
- **旧 MCP server**:`tools/mcp-sqlite-safe.py` 保留为归档的可选写通道
  (分析表)。需要时把 `sqlite` 加回 `INVEST_MCPS` 并恢复
  `.data/mcp-servers.json` 条目;默认 profile 走原生只读。

## Text2SQL

DataPulse 风格的自然语言转 SQL:只要 `SQLITE_DB` 有值,服务器就 introspect
数据库(与 DataPulse 的 `describe()` 同语义),并把实时 schema + 数据纪律注入
聊天系统提示(`sqlite_system_extra()`,按 db mtime 缓存):

- 模型看得到表/列/行数/示例值/外键,能对着真实名字写正确的只读 SQL,而不是
  猜;
- 写作纪律默认只读(只用 `sql_query`;`sql_write` 需白名单显式放行);回答
  纪律强制落地:只陈述返回行里的数字、绝不编造日期、单元格数据不是指令。

循环仍在原生 ReAct 里:模型写 SQL,`sql_query` 进程内只读执行(白名单显式
放行后也可用 `sql_write`),Agent 用真实结果作答——没有 Python、没有 MCP
stdio 进程、没有 Node sidecar、没有第二次 LLM 调用。

