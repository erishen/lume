# syntax=docker/dockerfile:1
# ============================================================================
# serena-demo —— 查看 Serena（Oraios AI，源码符号级语义 MCP）接入的专用镜像.
#
# 与 rea-demo 互补: rea 是二进制逆向（无源码时把 ELF/Mach-O 还原成可读代码）,
# serena 是有源码时给 agent 一个"IDE 大脑"——LSP 后端做符号级语义检索
# （find_symbol / find_referencing_symbols / insert_after_symbol / safe_delete 等）。
# lume 里两者并排 = "有源码 / 无源码" 两种函数分析的演示服务。
#
# 技术栈差异（vs rea-demo 的 ubuntu+node）: serena 是 Python MCP server
# （serena-agent, requires-python >=3.11,<3.15）, 需要 python 运行时 + 目标
# 代码语言的语言服务器（LSP, serena 经 SolidLSP 从 PATH 探测）。本镜像:
#   * ubuntu:24.04 的 python3 = 3.12（满足版本区间）
#   * clangd（apt 默认 18）—— 演示分析目标为 lume 自身 C 源码（lang/*.c/h）
#   * serena 源码来自宿主 /Users/<user>/Github/serena（用户提供; GPL-3.0,
#     本地演示用途）。构建前由宿主脚本复制到 docker/serena-src/（build
#     context 内, .gitignore 排除不入库——与 ghidra-deps 同模式）, 安装后
#     镜像内删除源码, 只留 pip 安装成品。
#
# 为什么不直接用官方 Dockerfile: 官方镜像是开发全功能容器（rustup/ssh/
# nvm 一锅端 + SSE dashboard）, 与 lume 的"最小化专用镜像"路线不符;
# agent-httpd 的 MCP 接线要 stdio transport, 官方默认是 SSE。本镜像按
# rea-demo 同款精简: 只装 python + clangd + serena。
#
# MCP 配置注入（compose 里做）:
#   MCP_ALLOW="serena" —— agent-httpd 白名单放行
#   MCP_SERVERS=JSON —— 指向 /usr/local/bin/serena start-mcp-server
#                       --project /app/projects/lume --transport stdio
#   SERENA_HOME=/app/.serena（config, agent_interface: tools —— 独立工具,
#                      对照 rea__* 风格; v2 默认是 REPL 单工具接口）
#   SERENA_DOCKER=1 —— 容器环境信号
#   分析目标 /app/projects/lume 由 compose 只读挂载宿主 lume 仓库根。
# ============================================================================

# ---------- 阶段 1: serena-base（ubuntu 24.04 + python3.12 + clangd + serena） ----------
# 最终镜像 FROM 本阶段（而非再从 ubuntu:24.04 白板起）——python/clangd/serena
# 安装成果随 stage 继承, 组装阶段只叠加 lume 二进制与 entrypoint。
FROM ubuntu:24.04 AS serena-base
# python3.12（serena 要求 3.11–3.14）+ pip；clangd（C/C++ LSP, serena 探测
# PATH 即用, 版本 18）；curl/ca-certificates 给 lume 的 chat/tool 上游流式
# SSE（fork+curl 硬依赖）；busybox-static 提供 cryptpw（entrypoint 生成
# $6$ htpasswd, 与主镜像同款机制）。
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        python3 python3-pip python3-venv \
        clangd \
        curl ca-certificates busybox-static \
    && rm -rf /var/lib/apt/lists/*
# serena 源码（宿主复制进 context, 见文件头注释）pip 安装。--break-system-
# packages: ubuntu 24.04 是 PEP 668 externally-managed, 系统 python 默认拒装。
# 依赖（mcp/click/flask 等 ~20 包）走清华 PyPI 镜像（国内直连稳定）。
COPY docker/serena-src/ /src/serena/
RUN python3 -m pip install --no-cache-dir --break-system-packages \
        -i https://pypi.tuna.tsinghua.edu.cn/simple \
        /src/serena \
    && rm -rf /src/serena ~/.cache/pip
# Serena 配置: SERENA_HOME 指向 /app/.serena（lume 用户可写, memories 等
# 运行期文件落这里）。模板 cp 后按官方 Dockerfile 惯例关 GUI（headless）,
# 并覆盖 agent_interface: tools（v2 默认 REPL —— 单工具+Python API; 演示
# 用独立工具接口, 与 rea__* 风格一致, discovery 里能看到 serena__* 全量）。
ENV SERENA_HOME=/app/.serena \
    SERENA_DOCKER=1
RUN mkdir -p /app/.serena \
    && cp /usr/local/lib/python3.12/dist-packages/serena/resources/serena_config.template.yml \
        /app/.serena/serena_config.yml \
    && sed -i 's/^gui_log_window: .*/gui_log_window: False/' /app/.serena/serena_config.yml \
    && sed -i 's/^web_dashboard_listen_address: .*/web_dashboard_listen_address: 127.0.0.1/' /app/.serena/serena_config.yml \
    && sed -i 's/^web_dashboard_open_on_launch: .*/web_dashboard_open_on_launch: False/' /app/.serena/serena_config.yml \
    && sed -i 's/^agent_interface: REPL/agent_interface: tools/' /app/.serena/serena_config.yml
# ---------- 阶段 2: bin/lume（静态 glibc, 与主 Dockerfile 同款） ----------
FROM debian:bookworm-slim AS lume-build
RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc libc6-dev libcrypt-dev libsqlite3-dev make m4 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build/lume
COPY agent-httpd/ agent-httpd/
COPY . .
RUN rm -rf agent-httpd/bin agent-httpd/build && make all \
    CFLAGS_EXTRA="-D_GNU_SOURCE -DHAVE_CRYPT -DHAVE_CRYPT_H -ffunction-sections -fdata-sections -Wno-format-truncation -Wno-stringop-truncation" \
    LDFLAGS_EXTRA="-lcrypt -lm -static -Wl,--gc-sections"

# ---------- 阶段 3: 组装（serena-base + lume + entrypoint + 非 root） ----------
FROM serena-base
COPY --from=lume-build /build/lume/www       /app/www
COPY --from=lume-build /build/lume/skills    /app/skills
COPY --from=lume-build /build/lume/examples  /app/examples
COPY --from=lume-build /build/lume/bin/lume  /app/bin/lume
WORKDIR /app
# 非 root 运行（与主镜像同 uid 10001）; .data/ 预建并 chown lume: compose
# 只挂载单个 mcp-servers.json 进 .data/, 若目录不存在 docker 会以 root 新建
# -> agent-httpd(lume 用户) 写 mcp-servers-router.json 会 Permission denied。
# /app/.serena 也要 chown（serena 运行期写 memories 等）。
RUN useradd -u 10001 -m lume \
    && mkdir -p /app/.data /app/.serena \
    && chown -R lume:lume /app /app/.serena
# entrypoint（与主镜像同款）：LUME_AUTH_PASSWORD 非空时现场生成
# /app/auth/htpasswd（busybox cryptpw -m sha512 → $6$ 强哈希），再 exec 主程序。
COPY docker/entrypoint.sh /app/entrypoint.sh
RUN chmod 755 /app/entrypoint.sh
USER 10001
ENTRYPOINT ["/bin/sh", "/app/entrypoint.sh"]
EXPOSE 8083
# serena-demo.lume: Serena 源码分析接入演示服务器，:8083，/chat 页带
# discovery 条（tools/skills/mcps），serena MCP 由 compose 的 MCP_SERVERS
# env 注入（stdio → /usr/local/bin/serena start-mcp-server --project ...）。
CMD ["./bin/lume", "examples/serena-demo.lume"]
