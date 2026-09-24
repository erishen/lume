# syntax=docker/dockerfile:1
# ============================================================================
# lume —— 自足 agent DSL 服务器镜像.
#
# lume 是**单二进制完整 HTTP 服务器**(静态 www/docroot + DSL 路由 + JSON
# API + /react/api/chat SSE 聊天), 一个容器就是整站 —— 不需要 nginx /
# FastCGI / 独立 React 后端 (那是 agent-httpd 镜像的布局, 与 lume 无关).
# 四个示例 (demo/hello/invest/hub) 共用同一棵 docroot, compose 里每个
# service 只换 examples/<name>.lume + 端口 + 白名单 env.
#
# 为什么 build context 是 lume/ (仓库根):
#   lume 静态链 libagenthttpd.a (submodule agent-httpd/, 见 .gitmodules).
#   宿主机那份 .a 是 Mach-O (macOS 上 make 编的), Linux 容器不能链接 —— 与
#   agent-httpd 镜像同理, 必须在容器内把 lib 和 bin/lume 都重编. lume 的
#   Makefile 里 AH := agent-httpd (仓库内 submodule 目录), 所以:
#       cd lume && docker build -f docker/Dockerfile -t lume:latest .
#
# 为什么没有前端阶段:
#   www/ 是 esbuild --splitting 分包 + Tailwind 的**纯静态产物**, 平台无关,
#   直接 COPY 宿主已构建的 docroot (宿主机 `make ui` 产出) 即可; 要容器内
#   重编的只有 C 二进制.
#
# 两阶段 (只重编 C):
#   1. lib-build   容器内编 debian 的 libagenthttpd.a
#   2. lume-build  容器内编 bin/lume (静态链阶段 1 的 .a)
#   3. runtime     只带 bin/lume + www/ + skills/ + examples, 非 root
# ============================================================================

# ---------- 阶段 1: libagenthttpd.a ----------
FROM debian:bookworm-slim AS lib-build
RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc libc6-dev libcrypt-dev make m4 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY agent-httpd/ agent-httpd/
RUN make -C agent-httpd lib \
    CFLAGS_EXTRA="-Wno-format-truncation -Wno-stringop-truncation"

# ---------- 阶段 2: bin/lume ----------
FROM debian:bookworm-slim AS lume-build
RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc libc6-dev libcrypt-dev make m4 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build/lume
COPY agent-httpd/ agent-httpd/
COPY . .
# lume 的 Makefile AH := agent-httpd —— /build/lume/agent-httpd 原样命中;
# `make all` 先依赖编 agent-httpd/lib 再编 bin/lume, 与 make dev 自举同链。
# 注意: .dockerignore 已排除 *.a 与根级 build/ bin/, 但 agent-httpd 的
# bin/ build/ 是子目录仍会被 COPY 进来(含宿主机 Mach-O 版 .a 等非 *.a
# 文件), make 会误判"已存在且最新"而跳过容器内重编, 于是链接宿主 Mach-O ——
# Linux ld 收不了 (file 实证: "current ar archive random library" = Darwin)。
# 故容器内先清掉宿主产物, 逼 $(AH_LIB) 真触发 make -C agent-httpd lib
# 重编出 ELF .a (其 Makefile:15-20 Linux 分支已豁免 gcc12 glibc 专属警告族),
# 再链接, 才能得到容器可跑的 bin/lume。
RUN rm -rf agent-httpd/bin agent-httpd/build && make all

# ---------- 阶段 3: 运行时 ----------
FROM debian:bookworm-slim AS runtime
# curl: compose healthcheck 打 /health (框架自带, 见 agent-httpd/src/http/
# http_route.c); ca-certificates 供 LLM 上游走 https; apache2-utils 只为了
# entrypoint 里的 htpasswd —— 把 .env 的明文密码在容器内换成 bcrypt。
# .env (LLM_API_URL / 白名单 / 密码) 全部由 compose 注入, 不进镜像。
RUN apt-get update \
    && apt-get install -y --no-install-recommends curl ca-certificates \
       apache2-utils \
    && rm -rf /var/lib/apt/lists/*
# 非 root。docroot/www 与 .data (会话/记忆) 归它写, reports/ 给 iquest 的
# /api/reports + 设置页写周报归档 (compose 注入 IQUEST_REPORTS_DIR)。
# 注意: 这里只建用户, chown 放到全部 COPY 之后 —— 见下。
RUN useradd --system --uid 10001 --create-home --home-dir /home/lume lume
WORKDIR /app
COPY --from=lume-build /build/lume/bin/lume bin/
# www/ 全树 (共享分包 chunk + invest 聊天 bundle + 各 views 子目录) ←
# 四示例共用同一棵 docroot, 平台无关直接 COPY。
COPY www/ www/
COPY skills/ skills/
COPY examples/ examples/
# 启动入口:把 .env 的 LUME_AUTH_PASSWORD 换成 /app/auth/htpasswd, 再 exec 主程序。
# 必须在 USER lume 之前 COPY —— COPY 以 root 执行, 保留宿主可执行位。
COPY docker/entrypoint.sh docker/entrypoint.sh
# chown 必须在全部 COPY 之后: COPY 以 root 运行, 会把 www/ skills/ examples/
# 连同子目录建成 root 属主, 容器里 lume 用户就写不进去。其中 skills/ 必须
# 可写 —— llm-router 同步要把 SKILL.md 落到 skills/router/<name>/ 下, 写不进
# 就同步失败(MCP 目录随之为空), 而且这类失败只在日志里有一行 Permission
# denied, 页面上看不出来。
RUN mkdir -p /app/.data /app/reports \
    && chown -R lume:lume /app /home/lume
USER lume
EXPOSE 8081 8082 8083
# entrypoint 生成 htpasswd 后 exec CMD, PID 1 仍是 lume 本体, 信号行为不变。
ENTRYPOINT ["/app/docker/entrypoint.sh"]
# healthcheck 走 /health; 默认 invest (compose 各 service 覆盖成示例)。
CMD ["./bin/lume", "examples/invest.lume"]
