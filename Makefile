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

# --- Windows(MSYS2/mingw-w64)分支 ---------------------------------------
# agent-httpd 自身没有 Windows 支持(30 个 .c 全是 POSIX socket 零 _WIN32 守卫),
# 而 bin/lume 硬链 libagenthttpd.a。Windows 构建因此:
#   1) 不构建也不链接 libagenthttpd.a;
#   2) 用 lang/bridge_stub.c 替掉 lang/bridge.c(route/tool 记进 VM 自己的表,
#      run() 明确报错退出, 不假装起服务);
#   3) 排除依赖 AH 库的 iquest.c / builtins_sql.c / builtins_http.c;
#   4) --watch 与出站 http_get/post/... 在 Windows 关闭(前者要 fork/pipe,
#      后者要裸 socket + TLS),由 main.c / builtins_internal.h 的守卫兜住。
# 参照已在 lume-core CI 验证过的同一模式(sbuf.h + bridge_stub.c)。
# UNAME_S 复用下面 platform feature-test 段的那次赋值, 故把检测放在其之后。
CC       ?= cc
CFLAGS   ?= -std=c11 -Wall -Wextra -O2 -g
CFLAGS   += -I lang $(addprefix -I, $(AH_INC))

# 版本号: lume --version 读这个宏(单一来源)。默认值取最近一次发布 tag;
# 打 release 前在此递增值, 保证二进制自报与 GitHub Release tag 一致。
LUME_VERSION ?= 0.6.2
CFLAGS   += -DLUME_VERSION=\"$(LUME_VERSION)\"
# LUME_HAS_HTTP: 出站 HTTP 内建(http_get/post/put/patch/delete)的开关。
# 实现已在 lang/builtins_http.c(2026-10-05 从 lume-core 移植, 裸 socket +
# 可选 libssl, 私有地址默认拒绝, --no-net / LUME_NO_NET=1 整体关掉)。
# 缺 libssl 时 https:// 报 "needs libssl", 不静默降明文; 置 0 可整体摘除。
LUME_HAS_HTTP ?= 1
CFLAGS   += -DLUME_HAS_HTTP=$(LUME_HAS_HTTP)
# 原生 SQLite 工具在 libagenthttpd.a 里(sqlite_tool.o), 链接 bin/lume 也要
# 带 -lsqlite3; 容器构建的 -static 则拉 libsqlite3.a(Dockerfile 已装 dev 包)。
LDFLAGS  += -lsqlite3

# 数据库驱动插件开关(与 agent-httpd 子模块一致;默认关闭零依赖):
# WITH_PG=1 / WITH_MYSQL=1 时 bin/lume 同样链接对应客户端库
WITH_PG ?= 0
WITH_MYSQL ?= 0
ifeq ($(WITH_PG),1)
    PQ_LIB := $(shell pg_config --libdir 2>/dev/null)
    LDFLAGS += -L$(PQ_LIB) -lpq
endif
ifeq ($(WITH_MYSQL),1)
    MYSQL_LIBS := $(shell mysql_config --libs 2>/dev/null) -L$(shell brew --prefix 2>/dev/null)/lib
    LDFLAGS += $(MYSQL_LIBS)
endif

# --- 平台 feature-test: 与 agent-httpd/Makefile:8-22 逐字同款 ---
# main.c 用 sigaction/sigemptyset (--watch 热重载的信号处理), 它们是 POSIX
# 199309 定义; glibc 不会默认放行, 要 -D_GNU_SOURCE 才显; macOS clang 则
# 默认全量 BSD 声明 (宿主 make 从不需要)。行为两侧必须一致 —— 宿主 make
# 编出的 bin/lume 与容器内重编的 bin/lume 都要能编, 所以两家都带, 而不是
# 只在镜像侧补 (镜像侧补 = macOS 宿主永远测不到这条平台差异)。
ifeq ($(OS),Windows_NT)
UNAME_S := Windows_NT
else
UNAME_S := $(shell uname -s)
endif
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

# --- Windows(MSYS2/mingw-w64)检测 ----------------------------------------
# agent-httpd 自身没有 Windows 支持(30 个 .c 全是 POSIX socket、零 _WIN32 守卫),
# 而 bin/lume 硬链 libagenthttpd.a。Windows 构建因此:
#   1) 不构建也不链接 libagenthttpd.a;
#   2) 用 lang/bridge_stub.c 替掉 lang/bridge.c(route/tool 记进 VM 自己的表,
#      run() 明确报错退出, 不假装起服务);
#   3) 排除依赖 AH 库的 iquest.c / builtins_sql.c / builtins_http.c, 出站 http_* 关掉;
#   4) --watch 关掉(要 fork/pipe/waitpid), 由 lang/main.c 的守卫兜住。
# 与 lume-core 已验证的同一模式(sbuf.h + bridge_stub.c), 见文件头 Windows 段。
ifeq ($(OS),Windows_NT)
    IS_WINDOWS := 1
else ifneq (,$(findstring MINGW,$(UNAME_S))$(findstring MSYS,$(UNAME_S))$(findstring CYGWIN,$(UNAME_S)))
    IS_WINDOWS := 1
endif
ifneq ($(strip $(IS_WINDOWS)),)
    TARGET_SUFFIX := .exe
    # Windows 下没有 agent-httpd 可探测, 也没有出站 HTTP 实现。
    LUME_HAS_HTTP := 0
    AH_LIB :=
    # 原生 SQLite 工具在 libagenthttpd.a 里(sqlite_tool.o), 这个库 Windows 下
    # 根本不链接, 相应地也不能要 -lsqlite3(mingw 的 sysroot 没有它)。
    LDFLAGS := $(filter-out -lsqlite3,$(LDFLAGS))
    # -lcrypt 只由上面 Linux 分支带入。真实 MSYS2 的 uname -s 是
    # MINGW64_NT-<ver>(不匹配 Linux), 但不把正确性押在这个假设上: mingw 的
    # sysroot 也没有 libcrypt, 一旦命中就链接失败。
    LDFLAGS := $(filter-out -lcrypt,$(LDFLAGS))
endif

# --- 出站 TLS(可选): http_get() 的 https:// (builtins_http.c, 源自 lume-core) ---
# 与 libLLVM 同款「有就用、没有只是缺」的口径: 探测到 openssl 就把 builtins_http.c
# 里 TLS 那段编进去(-DHAVE_OPENSSL=1 + -lssl -lcrypto); 探测不到时照样能编,
# https:// 直接报 "needs libssl", 不静默降明文。裸 socket 传输层, libssl 只管 TLS。
# macOS 上 pkg-config 常缺, 退 brew openssl 前缀; 再不行按常见安装前缀
# (Apple/Intel Homebrew、/usr) 落盘查头文件; 都查不到则 HAVE_OPENSSL=0。
ifeq ($(strip $(IS_WINDOWS)),)
OPENSSL_PKG := $(shell command -v pkg-config 2>/dev/null)
ifneq ($(strip $(OPENSSL_PKG)),)
    OPENSSL_PREFIX := $(shell pkg-config --variable=prefix openssl 2>/dev/null)
endif
ifeq ($(strip $(OPENSSL_PREFIX)),)
    OPENSSL_PREFIX := $(shell brew --prefix openssl 2>/dev/null)
endif
ifeq ($(strip $(OPENSSL_PREFIX)),)
    OPENSSL_PREFIX := $(shell brew --prefix openssl@3 2>/dev/null)
endif
endif
# 常见安装前缀落盘兜底(brew/pkg-config 都可能不在 PATH): Apple Silicon 与
# Intel Homebrew、系统 /usr。
ifneq ($(wildcard /opt/homebrew/opt/openssl@3/include/openssl/ssl.h),)
    OPENSSL_PREFIX := /opt/homebrew/opt/openssl@3
else ifneq ($(wildcard /opt/homebrew/include/openssl/ssl.h),)
    OPENSSL_PREFIX := /opt/homebrew
else ifneq ($(wildcard /usr/local/opt/openssl@3/include/openssl/ssl.h),)
    OPENSSL_PREFIX := /usr/local/opt/openssl@3
else ifneq ($(wildcard /usr/local/include/openssl/ssl.h),)
    OPENSSL_PREFIX := /usr/local
else ifneq ($(wildcard /usr/include/openssl/ssl.h),)
    OPENSSL_PREFIX := /usr
endif
# Windows 下 openssl 强制 0: 出站 http_* 整体不编(lume_has_http=0), 且 mingw
# 的 pkg-config/brew 探测无意义 —— 留着它会把 -DHAVE_OPENSSL=1 挂到编译行上。
HAVE_OPENSSL := $(if $(IS_WINDOWS),0,$(if $(and $(strip $(OPENSSL_PREFIX)),\
    $(wildcard $(OPENSSL_PREFIX)/include/openssl/ssl.h)),1,0))
ifeq ($(HAVE_OPENSSL),1)
    CFLAGS  += -I$(OPENSSL_PREFIX)/include -DHAVE_OPENSSL=1
    LDFLAGS += -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
    LDFLAGS += -Wl,-rpath,$(OPENSSL_PREFIX)/lib
endif

TARGET   := bin/lume$(TARGET_SUFFIX)
DEMO     := examples/demo.lume
HELLO    := examples/hello.lume
INVEST   := examples/invest.lume
HUB      := examples/hub.lume
EXAMPLES := $(DEMO) examples/lang-basics.lume $(HELLO) $(INVEST) $(HUB) examples/sqlite-write.lume examples/query-demo.lume examples/react-ssr.lume examples/abac.lume examples/modules/app.lume examples/modules-server.lume
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
SRCS     := lang/main.c lang/lexer.c lang/parser.c lang/parser_stmt.c lang/parser_expr.c \
            lang/value.c lang/typecheck.c lang/typecheck_expr.c lang/typecheck_stmt.c \
            lang/interp.c lang/builtins.c lang/builtins_sql.c lang/builtins_fs.c \
            lang/builtins_catalog.c lang/builtins_hof.c lang/builtins_str.c lang/builtins_math.c lang/builtins_crypt.c lang/loader.c lang/vdom.c \
            lang/builtins_http.c lang/bridge.c lang/token.c lang/iquest.c
# Windows: 去掉吃 agent-httpd 的三片(iquest 走 minijson 读取半 + agenthttpd_route,
# builtins_sql 走 AH 里的 db_query_json/db_write_exec, builtins_http 是裸 socket +
# 可选 TLS), 换入 bridge_stub.c; 另加 os_win32.c 供 builtins_fs.c 的
# lume_mkdir / lume_flock 垫片(必须独立 TU: <windows.h> 的 TokenType 枚举与
# lume.h 的 TokenType 类型撞名)。
ifneq ($(strip $(IS_WINDOWS)),)
    SRCS := lang/main.c lang/lexer.c lang/parser.c lang/parser_stmt.c lang/parser_expr.c \
            lang/value.c lang/typecheck.c lang/typecheck_expr.c lang/typecheck_stmt.c \
            lang/interp.c lang/builtins.c lang/builtins_fs.c \
            lang/builtins_catalog.c lang/builtins_hof.c lang/builtins_str.c lang/builtins_math.c lang/builtins_crypt.c lang/loader.c lang/vdom.c \
            lang/bridge_stub.c lang/token.c lang/os_win32.c \
            lang/builtins_stub.c lang/catalog_stub.c
endif
OBJS     := $(SRCS:lang/%.c=build/%.o)

# 内部头:任一 * 片的共享声明变化,所有依赖它的 .o 都要重建
INT_HDRS := $(wildcard lang/*_internal.h)

all: bin $(TARGET)

# Build the embedding library first if it is missing.
# 子模块源文件变化即触发 lib 重建（否则 llm.c 等改动不会带进 bin/lume）。
# Windows:AH_LIB 已置空(见 IS_WINDOWS 段), 整条规则连同它的 order-only 前置
# 一起退化为空目标 —— 否则 make 会去 agent-httpd 里编 POSIX 代码。
ifeq ($(strip $(IS_WINDOWS)),)
AH_DEPS := $(shell find $(AH)/src -name '*.c' -o -name '*.h' 2>/dev/null)
$(AH_LIB): $(AH_DEPS)
	$(MAKE) -C $(AH) lib WITH_PG=$(WITH_PG) WITH_MYSQL=$(WITH_MYSQL)
else
$(AH_LIB):
	@:
endif

# Directory targets: on non-Windows (sh) `mkdir -p` is idempotent and stays
# in the recipe. On Windows, mingw32-make may run recipes under cmd.exe where
# mkdir has no -p and errors on existing directories (and the SHELL env var
# is unreliable - Git Bash injects sh paths even when cmd runs the recipe),
# so the Windows branch creates build/ and bin/ at parse time via a plain
# `$(shell mkdir -p build bin 2>/dev/null)`. The previous form used
# `$(shell cmd /c "if not exist build mkdir build & if not exist bin mkdir
# bin")`, whose `cmd`/`&`/quoting confused the CI make/SHELL into
# "/bin/sh: --: invalid option" + "missing separator" at parse time. CI's
# $(shell) runs /bin/sh, so a bare POSIX `mkdir -p` is robust and needs no
# quoting. The `build: ;` / `bin: ;` empty recipes keep `make -B` from
# failing on directories that already exist.
ifeq ($(strip $(IS_WINDOWS)),)
build:
	mkdir -p build
bin:
	mkdir -p bin
else
$(shell mkdir -p build bin 2>/dev/null)
build: ;
bin: ;
endif

build/%.o: lang/%.c lang/lume.h $(INT_HDRS) | build $(AH_LIB)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS) $(AH_LIB) | bin
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(AH_LIB) $(LDFLAGS) -lm

check: all crypt-test
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

# URL 查询参数示例: examples/query-demo.lume on :$(QUERY_DEMO_PORT)(默认 8087,
# 独立于 demo-sqlite 的 8084 / hub 的 8083)。纯 API 无前端页面:
#   /echo?name=Ada&tag=hello%20world&flag   — 原始 query 串 + 解码 params 对照
#   /api/hello?name=Lume&style=polite       — 参数读取 + get() 缺省值
#   make query-demo           # 构建 + 检查 + 清端口 + 前台启动(Ctrl-C 停)
#   make query-demo-watch     # 热更新:改 examples/query-demo.lume 自动重起
QUERY_DEMO_PORT ?= 8087
query-demo: all check
	$(call KILL_SERVER,$(QUERY_DEMO_PORT),[q]uery-demo.lume)
	@echo "==> lume examples/query-demo.lume on :$(QUERY_DEMO_PORT) (URL query params demo)"; \
	./$(TARGET) examples/query-demo.lume

query-demo-watch:
	$(call KILL_SERVER,$(QUERY_DEMO_PORT),[q]uery-demo.lume)
	@echo "==> lume --watch examples/query-demo.lume on :$(QUERY_DEMO_PORT) (URL query params demo)"; \
	./$(TARGET) --watch examples/query-demo.lume

# 多文件模块演示: examples/modules/app.lume (import/export)。
# 纯计算示例(tax.lume 策略库 + app.lume 入口),不绑定端口,跑完即退;
# 演示命名空间导入、显式导出、模块顶层只执行一次。已在 make check 内。
#   make modules            # 构建 + 检查 + 运行
#   make modules-watch      # --watch:改 modules 下的 .lume 自动重校验重跑
#                          (非 server 脚本,child 跑完即退,watcher 等下次编辑)
modules: all check
	@echo "==> lume examples/modules/app.lume (import/export demo)"; \
	./$(TARGET) examples/modules/app.lume

modules-watch: all check
	@echo "==> lume --watch examples/modules/app.lume (import/export demo)"; \
	./$(TARGET) --watch examples/modules/app.lume

# 多文件模块的 UI 展示: examples/modules-server.lume on :$(MODULES_UI_PORT)
# (默认 8090)。import examples/modules/tax.lume 库,路由调用模块导出函数;
# React 前端(frontend/src/modules/app.tsx -> www/modules/app.js)提供
# 税额计算器 + 模块导出清单。
#   make modules-ui          # 构建 + 检查 + 清端口 + 前台启动(Ctrl-C 停)
#   make modules-ui-watch    # 热更新:改 examples/modules-server.lume 自动重起
MODULES_UI_PORT ?= 8090
modules-ui: all check ui
	$(call KILL_SERVER,$(MODULES_UI_PORT),[m]odules-server.lume)
	@echo "==> lume examples/modules-server.lume on :$(MODULES_UI_PORT) (import/export UI demo)"; \
	./$(TARGET) examples/modules-server.lume

modules-ui-watch: all check ui
	$(call KILL_SERVER,$(MODULES_UI_PORT),[m]odules-server.lume)
	@echo "==> lume --watch examples/modules-server.lume on :$(MODULES_UI_PORT) (import/export UI demo)"; \
	./$(TARGET) --watch examples/modules-server.lume

# ABAC 属性访问控制示例: examples/abac.lume on :$(ABAC_PORT)(默认 8086)。
# 生成 .data/abac.htpasswd(4 个 demo 账号,htpasswd bcrypt),构建 React 前端
# (frontend/src/abac/app.tsx -> www/abac/app.js),再起服务。
ABAC_PORT ?= 8086
ABAC_HTPASSWD ?= .data/abac.htpasswd

abac: all check ui
	@mkdir -p .data; \
	htpasswd -B -b -c $(ABAC_HTPASSWD) admin admin123 && \
	htpasswd -B -b $(ABAC_HTPASSWD) carol carol123 && \
	htpasswd -B -b $(ABAC_HTPASSWD) alice alice123 && \
	htpasswd -B -b $(ABAC_HTPASSWD) bob bob123
	$(call KILL_SERVER,$(ABAC_PORT),[a]bac.lume)
	@echo "==> lume examples/abac.lume on :$(ABAC_PORT) (ABAC: attributes -> PERMIT/DENY)"; \
	@echo "     cold start ~10s (MCP init) - wait, then http://localhost:$(ABAC_PORT)/ (admin/admin123)"; \
	./$(TARGET) examples/abac.lume

abac-watch:
	@mkdir -p .data; \
	htpasswd -B -b -c $(ABAC_HTPASSWD) admin admin123 && \
	htpasswd -B -b $(ABAC_HTPASSWD) carol carol123 && \
	htpasswd -B -b $(ABAC_HTPASSWD) alice alice123 && \
	htpasswd -B -b $(ABAC_HTPASSWD) bob bob123
	$(call KILL_SERVER,$(ABAC_PORT),[a]bac.lume)
	@echo "==> lume --watch examples/abac.lume on :$(ABAC_PORT) (ABAC demo)"; \
	@echo "     cold start ~10s (MCP init) - wait, then http://localhost:$(ABAC_PORT)/ (admin/admin123)"; \
	./$(TARGET) --watch examples/abac.lume

# --- React SSR 常驻后端(node bin/react-ssr-server,经 FastCGI relay) ---
# server{} 的 react_socket 指向 $(REACT_SSR_SOCK),内嵌 agent-httpd 把
# /react/* FastCGI relay 过去;node 进程常驻,无 CGI fork。构建链在
# agent-httpd submodule 里(pnpm install + scripts/build-ssr.sh)。
REACT_SSR_SERVER ?= bin/react-ssr-server
REACT_SSR_SOCK   ?= .data/react-ssr.sock
# React SSR 页面源码在 frontend/react-ssr/(lume 仓库内,可直接改 pages/*.tsx),
# 由 scripts/build-react-ssr.sh 构建常驻后端与 hydration bundle。
REACT_SSR_DEPS   ?= frontend/react-ssr/node_modules/.bin/esbuild

# 杀掉常驻 node React SSR 后端(按完整命令行匹配),并清掉 stale socket。
define KILL_REACT_SSR
	@pgrep -f '$(REACT_SSR_SERVER) $(REACT_SSR_SOCK)' 2>/dev/null | xargs kill -9 2>/dev/null || true; \
	rm -f $(REACT_SSR_SOCK)
endef

$(REACT_SSR_DEPS):
	@echo "==> pnpm install React SSR deps (frontend/react-ssr)"; \
	cd frontend/react-ssr && pnpm install

$(REACT_SSR_SERVER): $(REACT_SSR_DEPS)
	@echo "==> building React SSR resident backend (frontend/react-ssr)"; \
	sh scripts/build-react-ssr.sh

# React SSR 内容页示例: examples/react-ssr.lume on :$(SSR_CONTENT_PORT)(默认 8085)。
# 链路: 常驻 node 后端(React 组件经 react-dom/server renderToString)→
# server{} 的 react_socket → /react/* FastCGI relay;无后端时降级伺服
# www/react/home.html。等价于内置预设内容页。
#   make react-ssr             # 构建后端 + 检查 + 起后端 + 前台启动(Ctrl-C 停)
#   make react-ssr-watch       # 同前,lume 用 --watch 热更新
SSR_CONTENT_PORT ?= 8085
react-ssr: all check $(REACT_SSR_SERVER)
	$(call KILL_REACT_SSR)
	$(call KILL_SERVER,$(SSR_CONTENT_PORT),[r]eact-ssr.lume)
	@echo "==> starting node React SSR backend: $(REACT_SSR_SERVER) $(REACT_SSR_SOCK)"; \
	mkdir -p .data; \
	$(REACT_SSR_SERVER) $(REACT_SSR_SOCK) >/tmp/lume-react-ssr.log 2>&1 & \
	sleep 1; \
	@echo "==> lume examples/react-ssr.lume on :$(SSR_CONTENT_PORT) (React SSR via FastCGI relay)"; \
	./$(TARGET) examples/react-ssr.lume

react-ssr-watch: all check $(REACT_SSR_SERVER)
	$(call KILL_REACT_SSR)
	$(call KILL_SERVER,$(SSR_CONTENT_PORT),[r]eact-ssr.lume)
	@echo "==> starting node React SSR backend: $(REACT_SSR_SERVER) $(REACT_SSR_SOCK)"; \
	mkdir -p .data; \
	$(REACT_SSR_SERVER) $(REACT_SSR_SOCK) >/tmp/lume-react-ssr.log 2>&1 & \
	sleep 1; \
	@echo "==> lume --watch examples/react-ssr.lume on :$(SSR_CONTENT_PORT) (React SSR via FastCGI relay)"; \
	./$(TARGET) --watch examples/react-ssr.lume

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
ASAN_OBJS     := $(SRCS:lang/%.c=build-asan/%.o)
ASAN_CORE_OBJS := $(filter-out build-asan/main.o, $(ASAN_OBJS))

build-asan:
	mkdir -p build-asan

build-asan/tests:
	mkdir -p build-asan/tests

build-asan/%.o: lang/%.c lang/lume.h $(INT_HDRS) | build-asan $(AH_LIB)
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

.PHONY: all check dump dev dev-minimal invest hub demo-sqlite demo-sqlite-watch invest-watch hub-watch run ui ui-items vsix image image-push test clean asan sync-lang sync-lang-force check-sync
# lang/ is a pinned copy of the lume-core language tree (see lang/PIN).
# Bump the pin with `make sync-lang`; watch for drift with `make check-sync`.
# Files listed as host_owned in lang/PIN are never overwritten.
#
# check-sync does two things.  With a lume-core tree next door it diffs every
# synced file against the pinned upstream commit.  Without one - that is the
# CI runner, and erishen/lume-core is an empty repo as of v0.1.0 - it falls
# back to lang/PIN.manifest, the md5 baseline `make sync-lang` writes, so a
# hand edit to a synced file still fails there instead of passing silently.
sync-lang:
	@python3 scripts/sync-lang.py sync

# Refuses by default: a synced lang/ file with uncommitted edits is left
# alone instead of being replaced by the upstream copy, because those edits
# live only in the worktree.  --force is the way past that, and it does drop
# them, so reach for it only when they are known to be disposable.
sync-lang-force:
	@python3 scripts/sync-lang.py sync --force

check-sync:
	@python3 scripts/sync-lang.py check

# crypt_sha512 内建单测（glibc 生成 $6$ / macOS 平台报错 都算 PASS）。
crypt-test: all
	@./$(TARGET) tests/test-crypt.lume > /tmp/lume-crypt-test.out 2>&1; \
	rc=$$?; cat /tmp/lume-crypt-test.out; \
	if [ $$rc -ne 0 ] || grep -q FAIL /tmp/lume-crypt-test.out; then \
		echo "==> crypt 单测失败 (rc=$$rc)"; exit 1; fi; \
	echo "==> crypt 单测全部通过"
