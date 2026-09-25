# Lume — a strongly-typed DSL that compiles against the sibling project's
# static embed library (libagenthttpd.a). Everything besides libc comes from
# there.
# 依赖锁定: git submodule agent-httpd (github.com/erishen/agent-httpd),
# gitlink 锁定 cea88bd (2026-09-24, 原生 SQLite 工具 + schema 注入,写默认只读)。
# 升级后同步更新 .gitmodules 的 gitlink;CI 经 submodules: recursive 自动按
# gitlink 拉取。
AH          := agent-httpd
AH_LIB      := $(AH)/bin/libagenthttpd.a
AH_INC      := $(AH)/src $(AH)/src/core $(AH)/src/agent

CC       ?= cc
CFLAGS   ?= -std=c11 -Wall -Wextra -O2 -g
CFLAGS   += -I src $(addprefix -I, $(AH_INC))
# 原生 SQLite 工具在 libagenthttpd.a 里(sqlite_tool.o), 链接 bin/lume 也要
# 带 -lsqlite3; 容器构建的 -static 则拉 libsqlite3.a(Dockerfile 已装 dev 包)。
LDFLAGS  += -lsqlite3

# --- 平台 feature-test: 与 agent-httpd/Makefile:8-22 逐字同款 ---
# main.c 用 sigaction/sigemptyset (--watch 热重载的信号处理), 它们是 POSIX
# 199309 定义; glibc 不会默认放行, 要 -D_GNU_SOURCE 才显; macOS clang 则
# 默认全量 BSD 声明 (宿主 make 从不需要)。行为两侧必须一致 —— 宿主 make
# 编出的 bin/lume 与容器内重编的 bin/lume 都要能编, 所以两家都带, 而不是
# 只在镜像侧补 (镜像侧补 = macOS 宿主永远测不到这条平台差异)。
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    CFLAGS_EXTRA += -D_GNU_SOURCE
    # glibc fortify 与 agent-httpd 同开: Ubuntu 默认注入, 显式开启使本地
    # Linux 构建与 CI 一致(realpath 等 _chk 调用会校验 PATH_MAX)。
    CFLAGS_EXTRA += -D_FORTIFY_SOURCE=2
    # stringop-truncation: fortify 下 strncpy 会报可截断告警(此处用法安全:
    # 64B 零初始化缓冲 + 复制 63B, 尾部 NUL 保底); 与 agent-httpd 同款豁免。
    CFLAGS_EXTRA += -Wno-stringop-truncation
    LDFLAGS_EXTRA += -lcrypt -lm
else ifeq ($(UNAME_S),Darwin)
    CFLAGS_EXTRA += -D_DARWIN_C_SOURCE
endif
CFLAGS  += $(CFLAGS_EXTRA)
LDFLAGS += $(LDFLAGS_EXTRA)

TARGET   := bin/lume
DEMO     := examples/demo.lume
HELLO    := examples/hello.lume
INVEST   := examples/invest.lume
HUB      := examples/hub.lume
EXAMPLES := $(DEMO) examples/lang-basics.lume $(HELLO) $(INVEST) $(HUB) examples/sqlite-write.lume examples/query-demo.lume
# dev / dev-minimal 用的默认端口 (echo 与启动前清端口用)。
PORT ?= 8082
HUB_PORT ?= 8083

# $(call KILL_SERVER,port,bracket-stem) — 启动前清场:杀掉当前监听着 :port 的
# 进程,也按进程名杀掉仍在伺服该示例的旧 lume(bracket-stem 如 [h]ub.lume,
# 使 grep 模式匹配真正的进程,又不会匹配到杀手自己这条 sh)。普通与 --watch
# 两种启动形式都覆盖(模式里 .* 兼容 --watch 标志),然后轮询直到端口确实
# 释放,保证下一行能直接 bind,不会 "Address already in use"。
define KILL_SERVER
	@echo "==> freeing :$(1): killing stale lume ($(2))"; \
	{ lsof -ti tcp:$(1) 2>/dev/null; pgrep -f '$(TARGET).*$(2)' 2>/dev/null; } | sort -nu | xargs kill -9 2>/dev/null || true; \
	i=0; while lsof -ti tcp:$(1) >/dev/null 2>&1; do \
		lsof -ti tcp:$(1) 2>/dev/null | xargs kill -9 2>/dev/null || true; \
		i=$$((i+1)); [ $$i -ge 10 ] && break; sleep 0.3; \
	done
endef
SRCS     := src/main.c src/lexer.c src/parser.c src/value.c \
            src/typecheck.c src/interp.c src/builtins.c src/vdom.c \
            src/bridge.c src/token.c src/iquest.c
OBJS     := $(SRCS:src/%.c=build/%.o)

all: bin $(TARGET)

# Build the embedding library first if it is missing.
# 子模块源文件变化即触发 lib 重建（否则 llm.c 等改动不会带进 bin/lume）。
AH_DEPS := $(shell find $(AH)/src -name '*.c' -o -name '*.h')
$(AH_LIB): $(AH_DEPS)
	$(MAKE) -C $(AH) lib

build:
	mkdir -p build

bin:
	mkdir -p bin

build/%.o: src/%.c src/lume.h | build $(AH_LIB)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS) $(AH_LIB) | bin
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(AH_LIB) $(LDFLAGS) -lm

check: all
	@for f in $(EXAMPLES); do ./$(TARGET) --check $$f || exit 1; done

dump: all
	./$(TARGET) --dump $(DEMO)

# Build, type check, and start the demo server (blocks; Ctrl-C to stop).
#   make dev PORT=8081
# examples/demo.lume binds :8081 in its server{ } — the KILL_SERVER/echo port
# defaults to match unless PORT is forced on the command line / environment
# (origin: PORT ?= 8082 below serves dev-minimal/invest, not this target).
dev: all check ui
	$(eval override PORT := $(if $(filter command line environment,$(origin PORT)),$(PORT),8081))
	$(call KILL_SERVER,$(PORT),[d]emo.lume)
	@echo "==> lume $(DEMO) on :$(PORT)"; \
	./$(TARGET) $(DEMO)

# Minimal getting-started server: examples/hello.lume on :8082.
#   make dev-minimal
dev-minimal: all check ui
	$(call KILL_SERVER,$(PORT),[h]ello.lume)
	@echo "==> lume $(HELLO) on :$(PORT)"; \
	./$(TARGET) $(HELLO)

# 投资助手产品服务器: examples/invest.lume on :8082(首选日常入口;含 iquest
# 产品 API 与前端仪表盘/归档/设置页)。启动前会先清场(KILL_SERVER:按端口 + 按
# 进程名杀掉旧 lume)——若由 supervisor 托管,请改用 supervisor 重启以保留其
# 监管关系。
#
# 产品档(profile)约定:每个示例 = 一个 examples/xxx.lume + 一个好的
# make 目标,用环境变量把运行时收敛到该任务所需的那一小撮能力:
#   HARNESS_SKILLS_ALLOW  技能白名单(逗号分隔,空=全部)
#   HARNESS_TOOLS_ALLOW   Agent 工具白名单(逗号分隔,空=全部)
#   MCP_ALLOW             MCP 服务器白名单(逗号分隔,空=全部;未列出的
#                         server 不 spawn 不注册)
# 后续新示例(媒体/MCP 演示/…)照抄这个目标,替换 example 名与白名单即可。
#   make invest            # 构建 + 前端 + 清端口 + 前台启动(Ctrl-C 停)
#   make invest PORT=8082
INVEST_SKILLS := weekly-investment
# invest 领域工具(portfolio_* / report_generate,见 examples/invest.lume)必须
# 在这一点上 —— 本地 tool 也被 HARNESS_TOOLS_ALLOW 过滤,漏了就像曾经的 add
# 一样被静默丢弃。目录 iquest 的 IQUEST_REPORTS_DIR 指到 .data/reports,让
# report_generate 的产物与仪表盘读的是同一处(容器里 compose 已注 /app/reports)。
INVEST_TOOLS  := skill-run,read_file,get_time,query_exchange_rate,fetch_url,recall,remember,portfolio_get,portfolio_add,portfolio_remove,report_generate,sql_query,sql_tables,sql_schema
# 写能力 sql_write 默认不放行(保守)。需要模型建分析表时手动加回:
#   INVEST_TOOLS := $(INVEST_TOOLS),sql_write   (护栏见 agent-httpd sqlite_tool.c)
# 原生 SQLite 取代 MCP sqlite 的读通道(只读 SELECT,SQLITE_DB 指向镜像库)。
# 若需写分析表等写能力,可手动把 sqlite 加回这里(同时恢复
# mcp-servers.json 的 sqlite 条目),但默认 invest 走原生只读。
INVEST_MCPS   := portfolio-check,pse-review,fs,think,memory
invest: all check ui
	$(call KILL_SERVER,$(PORT),[i]nvest.lume)
	@if [ -x .venv-sqlite/bin/python ]; then echo "==> sync JSON ledger -> SQLite mirror (.data/lume.db)"; .venv-sqlite/bin/python tools/sqlite-migrate.py; fi
	@echo "==> lume $(INVEST) on :$(PORT) (skills=$(INVEST_SKILLS) tools=$(INVEST_TOOLS) mcps=$(INVEST_MCPS))"; \
	HARNESS_SKILLS_ALLOW=$(INVEST_SKILLS) HARNESS_TOOLS_ALLOW=$(INVEST_TOOLS) \
		MCP_ALLOW=$(INVEST_MCPS) SQLITE_DB=.data/lume.db \
		IQUEST_REPORTS_DIR=.data/reports ./$(TARGET) $(INVEST)

# 热更新版:同上但加 --watch,改 examples/xxx.lume 自动重起(无效编辑保旧)。
# 端口会短暂释放重起(约 1s)。
invest-watch:
	$(call KILL_SERVER,$(PORT),[i]nvest.lume)
	@echo "==> lume --watch $(INVEST) on :$(PORT) (skills=$(INVEST_SKILLS) tools=$(INVEST_TOOLS) mcps=$(INVEST_MCPS))"; \
	HARNESS_SKILLS_ALLOW=$(INVEST_SKILLS) HARNESS_TOOLS_ALLOW=$(INVEST_TOOLS) \
		MCP_ALLOW=$(INVEST_MCPS) SQLITE_DB=.data/lume.db \
		IQUEST_REPORTS_DIR=.data/reports ./$(TARGET) --watch $(INVEST)

# SQLite 写能力演示: examples/sqlite-write.lume on :$(DEMO_SQLITE_PORT)(默认
# 8084,与 invest 8082 / hub 8083 并存)。与 invest 的只读默认对照 —— 这个
# profile 显式放行 sql_write,展示"模型建分析表 → 写入 → 查询"完整链路
# (护栏见 agent-httpd src/agent/sqlite_tool.c;portfolio 镜像表只读)。
#   make demo-sqlite        # 构建 + 同步账本镜像 + 清端口 + 前台启动(Ctrl-C 停)
#   make demo-sqlite-watch  # 热更新:改 examples/sqlite-write.lume 自动重起
DEMO_SQLITE_PORT ?= 8084
DEMO_SQLITE_TOOLS := read_file,get_time,sql_query,sql_write,sql_tables,sql_schema
demo-sqlite: all check ui
	$(call KILL_SERVER,$(DEMO_SQLITE_PORT),[s]qlite-write.lume)
	@if [ -x .venv-sqlite/bin/python ]; then echo "==> sync JSON ledger -> SQLite mirror (.data/lume.db)"; .venv-sqlite/bin/python tools/sqlite-migrate.py; fi
	@echo "==> lume examples/sqlite-write.lume on :$(DEMO_SQLITE_PORT) (tools=$(DEMO_SQLITE_TOOLS))"; \
	HARNESS_TOOLS_ALLOW=$(DEMO_SQLITE_TOOLS) MCP_ALLOW=fs,think,memory \
		SQLITE_DB=.data/lume.db ./$(TARGET) examples/sqlite-write.lume

demo-sqlite-watch: all check ui
	$(call KILL_SERVER,$(DEMO_SQLITE_PORT),[s]qlite-write.lume)
	@echo "==> lume --watch examples/sqlite-write.lume on :$(DEMO_SQLITE_PORT) (tools=$(DEMO_SQLITE_TOOLS))"; \
	HARNESS_TOOLS_ALLOW=$(DEMO_SQLITE_TOOLS) MCP_ALLOW=fs,think,memory \
		SQLITE_DB=.data/lume.db ./$(TARGET) --watch examples/sqlite-write.lume

# tsm-hub 网关能力示例: examples/hub.lume on :$(HUB_PORT)(默认 8083,
# 与 invest 的 8082 并存)。不收敛——整本网关目录全开:
#   HARNESS_SKILLS_ALLOW=网关 5 个技能(code-review/rust-review/hot-news-post/
#                         post-comment/weekly-investment)
#   HARNESS_TOOLS_ALLOW = tsm-hub 的 10 个 router 内置工具
#   MCP_ALLOW           = 网关 5 个 MCP(fs/memory/think/portfolio-check/pse-review)
# 开着 /chat 即可直接用这些能力(skill-run 技能、fs 读文件、汇率、计算…)。
#   make hub                 # 构建 + 前端 + 清端口 + 前台启动(Ctrl-C 停)
#   make hub HUB_PORT=8084
#   make hub-watch           # 热更新:改 .lume 自动重起(无效编辑保旧服务)
HUB_SKILLS := code-review,rust-review,hot-news-post,post-comment,weekly-investment
HUB_TOOLS  := calc,echo,fetch_url,get_time,query_exchange_rate,recall,remember,skill-run,system_info,execute_code
HUB_MCPS   := fs,memory,think,portfolio-check,pse-review
hub: all check ui
	$(call KILL_SERVER,$(HUB_PORT),[h]ub.lume)
	@echo "==> lume $(HUB) on :$(HUB_PORT) (skills=$(HUB_SKILLS) tools=$(HUB_TOOLS) mcps=$(HUB_MCPS))"; \
	HARNESS_SKILLS_ALLOW=$(HUB_SKILLS) HARNESS_TOOLS_ALLOW=$(HUB_TOOLS) \
		MCP_ALLOW=$(HUB_MCPS) ./$(TARGET) $(HUB)

hub-watch:
	$(call KILL_SERVER,$(HUB_PORT),[h]ub.lume)
	@echo "==> lume --watch $(HUB) on :$(HUB_PORT) (skills=$(HUB_SKILLS) tools=$(HUB_TOOLS) mcps=$(HUB_MCPS))"; \
	HARNESS_SKILLS_ALLOW=$(HUB_SKILLS) HARNESS_TOOLS_ALLOW=$(HUB_TOOLS) \
		MCP_ALLOW=$(HUB_MCPS) ./$(TARGET) --watch $(HUB)

# Run an arbitrary example:  make run FILE=examples/foo.lume
run: all
	./$(TARGET) $(FILE)

# Bundle the React clients: one esbuild run with --splitting from
# frontend/src/<example>/*.tsx -> www/<example>/*.js entry bundles + a shared
# www/chunk-*.js (React), plus frontend/src/*.css -> www/*.css (Tailwind),
# all into the docroot.
# Dependencies are managed with pnpm (frontend/pnpm-lock.yaml).
ui:
	@cd frontend && { [ -d node_modules ] || pnpm install; } && pnpm run build && pnpm run build:items

# Only the hello example's assets (frontend/src/hello/items.tsx|items.css -> www/hello/items.*).
ui-items:
	@cd frontend && { [ -d node_modules ] || pnpm install; } && pnpm run build:items

# Package the VS Code syntax-highlighting extension (editor/lume-vscode) into a
# redistributable lume-<version>.vsix (lands in editor/lume-vscode/).
# --allow-missing-repository/--skip-license: 内部扩展,无 git remote 与独立
# LICENSE 文件;发新版前记得递增 package.json 的 version。
#   make vsix
vsix:
	@cd editor/lume-vscode && npx -y @vscode/vsce package

# --- 镜像构建/发布 (docker) -----------------------------------------------
# 发布到云机器之前先出容器镜像。镜像从 scratch + 静态 lume + busybox/musl
# curl 依赖拼成 ~17.7MB(比旧 debian-slim 116MB 省 100MB 磁盘/台),仓库与
# 云拉取带宽都省。
#   make image         本地构建(当前架构),产出 IMAGE_REPO:IMAGE_TAG
#   make image-push    buildx 多架构构建 + 直接推送 registry(先 docker login)
# 推送命名:镜像名含 registry 前缀时整体生效,例:
#   make image-push IMAGE_REPO=erishen/lume IMAGE_TAG=v1.0.0      # docker hub
#   make image-push IMAGE_REPO=registry.example.com/lume IMAGE_TAG=v1.0.0
# 构建上下文是仓库根(agent-httpd submodule 与 frontend 都会编进镜像),
# -f docker/Dockerfile 与 compose 一致。
IMAGE_REPO ?= lume
IMAGE_TAG  ?= latest
IMAGE_PLAT ?= linux/amd64,linux/arm64

image:
	@echo "==> docker build -t $(IMAGE_REPO):$(IMAGE_TAG) (local arch)"; \
	docker build -f docker/Dockerfile -t $(IMAGE_REPO):$(IMAGE_TAG) .

image-push:
	@docker buildx create --use --name lume-$(IMAGE_TAG) >/dev/null 2>&1 || true; \
	docker buildx build --platform $(IMAGE_PLAT) \
		-f docker/Dockerfile -t $(IMAGE_REPO):$(IMAGE_TAG) --push .; \
	echo "==> pushed $(IMAGE_REPO):$(IMAGE_TAG) for [$(IMAGE_PLAT)]"

CORE_OBJS := $(filter-out build/main.o, $(OBJS))

build/tests:
	mkdir -p build/tests

tests/smoke-bin: tests/smoke.c $(CORE_OBJS) build/tests | $(AH_LIB)
	$(CC) $(CFLAGS) -o $@ tests/smoke.c $(CORE_OBJS) $(AH_LIB) $(LDFLAGS) -lm

tests/tools-bin: tests/tools_driver.c $(CORE_OBJS) build/tests | $(AH_LIB)
	$(CC) $(CFLAGS) -o $@ tests/tools_driver.c $(CORE_OBJS) $(AH_LIB) $(LDFLAGS) -lm

test: all ui tests/smoke-bin tests/tools-bin
	chmod +x tests/run_all.sh && ./tests/run_all.sh

clean:
	rm -rf build bin build-asan

# --- ASan/UBSan 构建 (make asan) ------------------------------------------
# 独立构建目录 build-asan/,不污染正常 build/。检出 lume 侧代码的
# 堆/栈越界与 UB;agent-httpd 的 lib 不插桩(宿主编译),只能检出 lume 侧。
# macOS clang 与 Linux gcc 均支持 -fsanitize=address,undefined。
ASAN_CFLAGS   := -fsanitize=address,undefined -fno-omit-frame-pointer
ASAN_LDFLAGS  := -fsanitize=address,undefined
ASAN_TARGET   := bin/lume-asan
ASAN_OBJS     := $(SRCS:src/%.c=build-asan/%.o)
ASAN_CORE_OBJS := $(filter-out build-asan/main.o, $(ASAN_OBJS))

build-asan:
	mkdir -p build-asan

build-asan/tests:
	mkdir -p build-asan/tests

build-asan/%.o: src/%.c src/lume.h | build-asan $(AH_LIB)
	$(CC) $(CFLAGS) $(ASAN_CFLAGS) -c $< -o $@

$(ASAN_TARGET): $(ASAN_OBJS) $(AH_LIB) | build-asan bin
	$(CC) $(CFLAGS) $(ASAN_CFLAGS) -o $@ $(ASAN_OBJS) $(AH_LIB) $(LDFLAGS) $(ASAN_LDFLAGS) -lm

tests/smoke-bin-asan: tests/smoke.c $(ASAN_CORE_OBJS) build-asan/tests | $(AH_LIB)
	$(CC) $(CFLAGS) $(ASAN_CFLAGS) -o $@ tests/smoke.c $(ASAN_CORE_OBJS) $(AH_LIB) $(LDFLAGS) $(ASAN_LDFLAGS) -lm

tests/tools-bin-asan: tests/tools_driver.c $(ASAN_CORE_OBJS) build-asan/tests | $(AH_LIB)
	$(CC) $(CFLAGS) $(ASAN_CFLAGS) -o $@ tests/tools_driver.c $(ASAN_CORE_OBJS) $(AH_LIB) $(LDFLAGS) $(ASAN_LDFLAGS) -lm

asan: $(ASAN_TARGET) tests/smoke-bin-asan tests/tools-bin-asan
	@echo "==> ASan/UBSan: --check 全部示例 + lang-basics 直跑 + 单测 + 工具派发"
	# detect_leaks=0: Type 对象生命周期=进程(AST/符号表持有,无释放函数是设计);
	# --watch reload 走 fork+exec 子进程重启, 无累积路径。LSan 会把无主的
	# 临时检查类型(ck_expr 中间结果)当泄漏, 对一次性/exec 隔离进程是误报,
	# 故豁免; ASan/UBSan 的越界/UB 检测保持全开。
	@for f in $(EXAMPLES); do ASAN_OPTIONS=detect_leaks=0 ./$(ASAN_TARGET) --check $$f || exit 1; done
	@ASAN_OPTIONS=detect_leaks=0 ./$(ASAN_TARGET) examples/lang-basics.lume >/dev/null || exit 1
	@ASAN_OPTIONS=detect_leaks=0 ./tests/smoke-bin-asan || exit 1
	@ASAN_OPTIONS=detect_leaks=0 ./tests/tools-bin-asan || exit 1
	@echo "ok   ASan/UBSan all passed"

.PHONY: all check dump dev dev-minimal invest hub demo-sqlite demo-sqlite-watch invest-watch hub-watch run ui ui-items vsix image image-push test clean asan