# syntax=docker/dockerfile:1
# ============================================================================
# rea-demo —— 查看 rea（Reverse Engineer Anything，逆向工程 MCP）接入的专用镜像.
#
# 与主 Dockerfile 的 scratch 最小化设计不同：rea 是 Node MCP server
# （rea-agents，要求 Node ^22.19 / ^24.11 / >=26），容器里必须有 node 运行时，
# 所以本镜像基于 ubuntu:24.04（rea 的 host 平台检查只支持 macOS 12+ /
# Ubuntu 24.04+ / Fedora 41+ / Arch 等，Debian bookworm 会被拒），Node 24
# 由 npmmirror 下载二进制放入 /opt/node（rea 要求 Node ^22.19 / ^24.11 / >=26）。
# 只服务一个目的：在 Docker 环境里跑通 rea ↔ agent-httpd（lume 的 MCP client）
# 接线，供查看 rea 的接入使用（resident 常驻 + rea__* 工具注册 + 实际逆向调用）。
#
# 体积: 基础 node:22-noble ~250M（vs 主镜像 scratch ~10M），
#       这是 node 运行时 + rea 包的成本；invest/hub 主服务不受影响。
#
# 为什么不直接改主 Dockerfile:
#   invest/hub 的 scratch 镜像刻意保持无 shell/包管理器/编译器（安全面最小），
#   加 node 会把整个安全面扩大。rea 是查看/实验用途，独立镜像隔离更干净。
#
# MCP 配置注入（compose 里做）:
#   MCP_ALLOW="rea" —— agent-httpd 白名单放行 rea
#   MCP_SERVERS=JSON —— 指向容器内 node + rea.mjs（.dockerignore 已排除 .data/，
#                      镜像内无宿主机绝对路径配置，env 是唯一来源，无冲突）
#   rea 冷启动 40–90s（首次 Playwright 平台 registry 加载）：agent-httpd 的
#   MCP_BOOT_TIMEOUT 已改 120s（源码级改动，容器内重编自动带上）。
# ============================================================================

# ---------- 阶段 1: bin/lume（静态 glibc，与主 Dockerfile 同款） ----------
FROM debian:bookworm-slim AS lume-build
# libsqlite3-dev: 原生 SQLite 工具静态链接需要；-static 链出无动态依赖二进制。
RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc libc6-dev libcrypt-dev libsqlite3-dev make m4 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build/lume
COPY agent-httpd/ agent-httpd/
COPY . .
# 与主 Dockerfile 完全相同的静态链参数（含 HAVE_CRYPT，Basic Auth 强校验）
RUN rm -rf agent-httpd/bin agent-httpd/build && make all \
    CFLAGS_EXTRA="-D_GNU_SOURCE -DHAVE_CRYPT -DHAVE_CRYPT_H -ffunction-sections -fdata-sections -Wno-format-truncation -Wno-stringop-truncation" \
    LDFLAGS_EXTRA="-lcrypt -lm -static -Wl,--gc-sections"

# ---------- 阶段 2: Ghidra 12.1.4 arm64 decompiler（源码构建） ----------
# Ghidra 官方发行包只带 linux_x86_64 / win_x86_64 的 Decompiler 原生二进制，
# 没有 linux_arm_64 —— rea 的 Ghidra provider 探测 native_decompiler_path 会报
# executable_missing（此前 12.1.4 已通过版本检查, 只卡在这里）。本阶段从官方
# 源码 tag Ghidra_12.1.4_build 构建 arm64 decompiler：DevGuide 确认
# `gradle buildNatives` 只构建当前平台（arm64 宿主 -> linux_arm_64），无需交叉
# 工具链。官方 release 没有 Source.zip, 源码需 git clone（走宿主 Clash 代理）。
# 基础镜像用 ubuntu:24.04（noble）: ① rea 的 host 平台检查只认 macOS 12+ /
# Ubuntu 24.04+ / Fedora 41+ / Arch 等 —— Debian bookworm 会被拒
# （GhidraLaunchError: headless launch failed）；② noble 自带 openjdk-21
# （Ghidra 12 要求 JDK 21），无需 Temurin。
FROM ubuntu:24.04 AS ghidra-native
RUN apt-get update \
    && apt-get install -y --no-install-recommends git gcc g++ make ca-certificates openjdk-21-jdk-headless unzip \
    && rm -rf /var/lib/apt/lists/*
# git 与 gradle wrapper 都走宿主 Clash 代理（host.docker.internal:7897, OrbStack
# 映射）。gradle wrapper 的 HTTPS 下载是 JVM 网络栈，需 systemProp 写在 ~/.gradle
# （services.gradle.org 直连会被断, SSL EOF；代理下可下载）。
WORKDIR /src
# Ghidra 官方 release 无 Source.zip, 源码只能 git clone 或 zipball。BuildKit
# 构建容器内 git clone 大仓库（~2 万文件）出现 checkout 产物丢失（层快照里
# /src 为空, exit 0）——改由宿主下载 tag zipball（97MB, codeload）再 COPY +
# unzip, 完全绕开构建容器内 clone。gradle 代理配置保留（后续 buildNatives 的
# fetchDependencies 若需远端 maven 也能走代理, 但 deps 已全部预置）。
COPY docker/ghidra-deps/ghidra-src.zip /tmp/ghidra-src.zip
# 预置 fetchDependencies 的全部依赖（宿主 curl + sha256 校验下载，见
# docker/ghidra-fetch-deps.sh / docker/ghidra-deps/downloads/）。构建容器内的
# curl 走代理对大文件不稳定；宿主下载已验证 sha。fetchDependencies 检测到
# 文件已存在且 sha 匹配会直接跳过下载（见其 deps.each 逻辑）。
COPY docker/ghidra-deps/downloads/ /src/ghidra/dependencies/downloads/
# gradle 发行包同样宿主导下载（services.gradle.org 走代理连接超时/不稳），
# wrapper 的 distributionUrl 改为本地 file:// 直接使用。
COPY docker/ghidra-deps/gradle-9.7.1-bin.zip /tmp/gradle-9.7.1-bin.zip
# BuildKit（OrbStack）跨层快照对大量文件（2 万+）写入存在丢失（clone / unzip
# 单独成层时, 后续层看到空目录）。解压 + 移动 + 替换 distributionUrl + gradle
# 构建全部合并到同一个 RUN：RUN 内文件系统是连续的, 不依赖跨层快照传播。
# fetchDependencies（init script）建 flatRepo（maven 依赖 + 预置文件校验）；
# buildNatives 只构建当前平台 native。实测：Ghidra_12.1.4_build tag,
# gradle 9.7.1, arm64 宿主, ~15 分钟 BUILD SUCCESSFUL,
# 产物 Ghidra/Features/Decompiler/build/os/linux_arm_64/decompile。
RUN mkdir -p ~/.gradle \
    && printf 'systemProp.http.proxyHost=host.docker.internal\nsystemProp.http.proxyPort=7897\nsystemProp.https.proxyHost=host.docker.internal\nsystemProp.https.proxyPort=7897\n' > ~/.gradle/gradle.properties \
    && mkdir -p /src/ghidra /src/ghidra-tmp \
    && (unzip /tmp/ghidra-src.zip -d /src/ghidra-tmp 2>&1 | tail -3) \
    && mv /src/ghidra-tmp/ghidra-Ghidra_12.1.4_build/* /src/ghidra/ \
    && rm -rf /src/ghidra-tmp /tmp/ghidra-src.zip \
    && echo "SRC_FILES=$(ls /src/ghidra | wc -l)" \
    && sed -i "s|distributionUrl=.*|distributionUrl=file:///tmp/gradle-9.7.1-bin.zip|" /src/ghidra/Ghidra/RuntimeScripts/Common/support/gradle/gradle-wrapper.properties \
    && cd /src/ghidra \
    && JAVA_HOME=/usr/lib/jvm/java-21-openjdk-arm64 ./gradlew -I gradle/support/fetchDependencies.gradle buildNatives --no-daemon

# ---------- 阶段 3: runtime（ubuntu 24.04 + node 24 + lume + rea-agents + Ghidra 12） ----------
# ubuntu:24.04（noble）: rea 的 host 平台检查只支持 macOS 12+ / Ubuntu 24.04+ /
# Fedora 41+ / Arch 等（Debian bookworm 会被拒 GhidraLaunchError）。Node 官方
# 镜像的 noble 变体（node:22-noble 等）国内镜像源同步不全/直连被墙，改用
# npmmirror（已验证可用）下载 Node 24 二进制。openjdk-21 来自 apt（Ghidra 12
# 要求 JDK 21）。
FROM ubuntu:24.04
# lume 的 chat/tool 上游走 fork+curl 子进程（流式 SSE + -w 尾部状态行），硬依赖；
# CA 证书给 curl HTTPS 上游；unzip 解压 Ghidra 发行包；xz-utils 解压 node tar.xz；
# busybox-static 提供 /bin/busybox cryptpw（entrypoint 生成 $6$ htpasswd 用，
# 与主镜像同款机制 —— 见 docker/entrypoint.sh）。
RUN apt-get update \
    && apt-get install -y --no-install-recommends curl ca-certificates unzip xz-utils openjdk-21-jdk-headless busybox-static \
    && rm -rf /var/lib/apt/lists/*
# Node 24 LTS（rea 要求 ^22.19 / ^24.11 / >=26），npmmirror 国内镜像直连
RUN curl -fsSL -o /tmp/node.tar.xz \
        https://npmmirror.com/mirrors/node/v24.12.0/node-v24.12.0-linux-arm64.tar.xz \
    && mkdir -p /opt/node \
    && tar -xJf /tmp/node.tar.xz -C /opt/node --strip-components=1 \
    && rm /tmp/node.tar.xz
ENV PATH="/opt/node/bin:${PATH}"
WORKDIR /app
COPY --from=lume-build /build/lume/www       /app/www
COPY --from=lume-build /build/lume/skills    /app/skills
COPY --from=lume-build /build/lume/examples  /app/examples
COPY --from=lume-build /build/lume/bin/lume  /app/bin/lume
# rea-agents 容器内安装（npmmirror 国内源；本地 npmrc 的 127.0.0.1:7897 代理
# 在容器内不可达，不继承）。rea 冷启动时长由 MCP_BOOT_TIMEOUT=120s 兜底。
RUN npm install --prefix /app/rea --registry=https://registry.npmmirror.com \
        --no-audit --no-fund rea-agents@6.4.0 \
    && rm -rf /app/rea/node_modules/.cache /root/.npm
# Ghidra 12.1.4（rea 6.4.0 明确要求: "Install a Ghidra 12.1.x release (verified
# with 12.1.4)"；11.3.2 会被拒 unsupported_version）。zip 由宿主下载置于
# docker/ghidra/（build context）；解压到 /opt/ghidra。rea 通过
# GHIDRA_INSTALL_DIR + JAVA_HOME 探测（GhidraDoctor），bridge .java 由 rea
# 运行时用 javac 编译进 Ghidra extension。
COPY docker/ghidra/ghidra_12.1.4_PUBLIC_20260921.zip /tmp/ghidra.zip
# 瘦身：Ghidra 发行包解压后 ~880MB，headless 分析只用核心 Features/Framework。
# 删除文档、官方扩展、FID 指纹库（195MB，Ghidra 缺库时分析器自动跳过不报错）、
# BSim/服务器/PyGhidra，以及所有非 arm64 平台 native（本机 linux/arm64 只加载
# linux_arm_64 的 decompile，decompile 已由 ghidra-native 阶段提供）。
RUN unzip -q /tmp/ghidra.zip -d /opt/ghidra \
    && rm /tmp/ghidra.zip \
    && rm -rf /opt/ghidra/ghidra_12.1.4_PUBLIC/docs \
    && rm -rf /opt/ghidra/ghidra_12.1.4_PUBLIC/Extensions \
    && rm -rf /opt/ghidra/ghidra_12.1.4_PUBLIC/GPL \
    && rm -rf /opt/ghidra/ghidra_12.1.4_PUBLIC/server \
    && rm -rf /opt/ghidra/ghidra_12.1.4_PUBLIC/Ghidra/Features/FunctionID \
    && rm -rf /opt/ghidra/ghidra_12.1.4_PUBLIC/Ghidra/Features/BSim \
    && rm -rf /opt/ghidra/ghidra_12.1.4_PUBLIC/Ghidra/Features/GhidraServer \
    && rm -rf /opt/ghidra/ghidra_12.1.4_PUBLIC/Ghidra/Features/PyGhidra \
    && find /opt/ghidra/ghidra_12.1.4_PUBLIC/Ghidra/Features -type d \( -name "linux_x86_64" -o -name "win_x86_64" -o -name "mac_x86_64" -o -name "mac_arm_64" \) -exec rm -rf {} +
# arm64 decompiler（阶段 2 源码构建产物）放入发行布局 os/linux_arm_64/，
# rea 的 native_decompiler_path 探测到即可用（发行包只有 linux_x86_64/win_x86_64）
COPY --from=ghidra-native /src/ghidra/Ghidra/Features/Decompiler/build/os/linux_arm_64/decompile \
     /opt/ghidra/ghidra_12.1.4_PUBLIC/Ghidra/Features/Decompiler/os/linux_arm_64/decompile
ENV GHIDRA_INSTALL_DIR=/opt/ghidra/ghidra_12.1.4_PUBLIC \
    JAVA_HOME=/usr/lib/jvm/java-21-openjdk-arm64
# 非 root 运行（与主镜像同 uid 10001），rea spawn 的临时文件都在 /tmp 可写
# .data/ 预建并 chown lume: compose 只挂载单个 mcp-servers.json 进 .data/,
# 若目录不存在 docker 会以 root 新建 -> agent-httpd(lume 用户) 写
# mcp-servers-router.json(router 同步) 会 Permission denied。
# Ghidra/JDK 也在此统一 chown（lume 用户必须先创建，才能 chown 给 lume）；
# JDK 是系统包（/usr/lib/jvm）无需 chown，lume 只读即可。
# 运行期不需要的 JDK/node 文件（rea 的 ghidra bridge 是 .java 源码, 运行时
# 需要 javac 编译 → 保留完整 bin/ 与 lib/, 删 jmods(81MB, 仅 jlink 用) 与
# docs/man/include；npm/npx/corepack 已装完 rea, 运行期不再需要(17MB)。
RUN useradd -u 10001 -m lume \
    && mkdir -p /app/.data \
    && chown -R lume:lume /app /opt/ghidra \
    && rm -rf /usr/lib/jvm/java-21-openjdk-arm64/jmods \
       /usr/lib/jvm/java-21-openjdk-arm64/docs \
       /usr/lib/jvm/java-21-openjdk-arm64/man \
       /usr/lib/jvm/java-21-openjdk-arm64/include \
       /opt/node/bin/npm /opt/node/bin/npx /opt/node/bin/corepack \
       /opt/node/lib/node_modules/npm /opt/node/lib/node_modules/corepack
# entrypoint（与主镜像同款）：LUME_AUTH_PASSWORD 非空时现场生成
# /app/auth/htpasswd（busybox cryptpw -m sha512 → $6$ 强哈希），再 exec 主程序。
# /app 已整树 chown lume，lume 用户可直接写 /app/auth；compose 注入
# HTPASSWD_FILE=/app/auth/htpasswd + 镜像 ENTRYPOINT 即启用 Basic Auth，
# 密码只有 .env 一个来源（不落镜像、不落 compose）。
COPY docker/entrypoint.sh /app/entrypoint.sh
RUN chmod 755 /app/entrypoint.sh
USER 10001
ENTRYPOINT ["/bin/sh", "/app/entrypoint.sh"]
EXPOSE 8082
# rea-demo.lume: REA 逆向接入演示服务器，:8082，/chat 页带 discovery 条
# （tools/skills/mcps），rea MCP 由 compose 的 MCP_SERVERS env 注入。
CMD ["./bin/lume", "examples/rea-demo.lume"]
