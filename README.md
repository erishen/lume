# Lume

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
| `examples/` | 5 个 `.lume` 示例(demo / hello / invest / hub / lang-basics) |
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
