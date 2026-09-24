# syntax=docker/dockerfile:1
# ============================================================================
# lume —— 自足 agent DSL 服务器镜像.
#
# lume 是**单二进制完整 HTTP 服务器**(静态 www/docroot + DSL 路由 + JSON
# API + /react/api/chat SSE 聊天), 一个容器就是整站 —— 不需要 nginx /
# FastCGI / 独立 React 后端. 四个示例 (demo/hello/invest/hub) 共用同一棵
# docroot, compose 里每个 service 只换 examples/<name>.lume + 端口 + 白名单 env.
#
# 为什么 build context 是 lume/ (仓库根):
#   lume 静态链 libagenthttpd.a (仓库内 agent-httpd submodule). 宿主机那份
#   .a 是 Mach-O (macOS 上 make 编的), Linux 容器不能链接 —— 必须在容器内把
#   lib 和 bin/lume 都重编, 故 context 含整棵仓库树 (含 agent-httpd/).
#
# 为什么没有前端阶段:
#   www/ 是 esbuild --splitting 分包 + Tailwind 的**纯静态产物**, 平台无关,
#   直接 COPY 宿主已构建的 docroot (宿主机 `make ui` 产出) 即可; 要容器内
#   重编的只有 C 二进制.
#
# 三阶段:
#   1. lume-build  debian 内重编 agent-httpd lib + bin/lume, **-static 静态链**,
#                  --gc-sections 裁段 -> bin/lume 约 1.7M, 不依赖任何动态库;
#   2. rtlayer     装配层: busybox(动态 musl, 连 ld-musl loader 一起带) +
#                  CA 证书 + www/skills/examples + 手写 passwd/group/nsswitch
#                  + 预建 /app/{.data,reports}(归 lume uid 10001);
#   3. runtime     FROM scratch —— 不含 OS/编译链/包管理器.
#
# 体积: 116M(debian-slim + curl/htpasswd/openssl) -> ~10M.
# 这 ~10M 里 scratch 只装 lume(静态 ~1.7M)+busybox(~0.3M)+CA 证书;
# 额外约 7M 是真 curl(动态 musl + 13 个 musl so): /react/api/chat 的 llm.c 用
# curl 子进程流式转发上游 SSE, 心跳/流式语义都靠 -w "\n%{http_code}\n" 尾部
# 行与 busybox 缺位互斥, 换 busybox wget 会丢掉这两者(401 解析后 HTTP 上游
# 直达 llm.c 说 "HTTP/1.1 401 Unauthorized" 会撞 -f 把它当硬错), 故不能替代.
# 安全: 镜像里没有 shell/包管理器/编译器, 凭据仍只走 compose env_file 注入.
# ============================================================================

# ---------- 阶段 1: bin/lume (静态 glibc) ----------
FROM debian:bookworm-slim AS lume-build
RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc libc6-dev libcrypt-dev make m4 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build/lume
COPY agent-httpd/ agent-httpd/
COPY . .
# 静态链:bin/lume 内嵌 glibc, runtime(scratch) 不需要 /lib 动态库;
# --gc-sections 裁掉未引用段 (libagenthttpd.a 里大量被 DSL 用到的 RTTI/DB 之外
# 的全任务死代码).
# 命令行 CFLAGS_EXTRA=/LDFLAGS_EXTRA= 会覆盖两处 Makefile 的 += 分支(子 make
# 继承 MAKEFLAGS 的 override), 所以必须把两家 Makefile 依赖的 feature 显式
# 放全——否则兄弟项目的 -DHAVE_CRYPT(有则 auth 走 crypt 强校验, 无则永久 401)
# 与 -lcrypt -lm 一起丢, Basic Auth 全线拒绝。此前漏掉时症状: 任何凭据恒 401。
RUN rm -rf agent-httpd/bin agent-httpd/build && make all \
    CFLAGS_EXTRA="-D_GNU_SOURCE -DHAVE_CRYPT -DHAVE_CRYPT_H -ffunction-sections -fdata-sections -Wno-format-truncation -Wno-stringop-truncation" \
    LDFLAGS_EXTRA="-lcrypt -lm -static -Wl,--gc-sections"

# ---------- 阶段 2: 运行时装配层 ----------
FROM alpine:3.20 AS rtlayer
# busybox 是 alpine 的动态 musl 二进制 (interp /lib/ld-musl-*.so.1), 需带 loader;
# lume 本体是静态 glibc, 不依赖它。assembly 层 COPY 全由 rootfs 带出。
RUN mkdir -p /out/bin /out/lib /out/etc /out/etc/ssl/certs \
             /out/app/bin /out/app/www /out/app/skills /out/app/examples \
             /out/app/.data /out/app/reports /out/home/lume /out/tmp
COPY --from=lume-build /build/lume/www       /out/app/www
COPY --from=lume-build /build/lume/skills    /out/app/skills
COPY --from=lume-build /build/lume/examples  /out/app/examples
COPY --from=lume-build /build/lume/bin/lume  /out/app/bin/lume
COPY docker/entrypoint.sh /out/app/entrypoint.sh
# curl: agent.c 的聊天/工具调上游走 fork+execlp("curl", …)(curl -sS --http1.1
# -N -f … -w "\n%{http_code}\n" --data-binary @-), 是硬依赖; busybox wget
# 无法在保留 SSE 流式 + 心跳的同时复刻 -w 尾部状态行, 也没有 stdin body。
# 用 alpine 动态 curl + 拷齐 ldd 全链 (musl/libcurl/ssl/crypto/zlib/zstd/
# cares/nghttp2/idn2/psl/brotli/unistring), 净增约 7M。ldd 清单:
#   /lib/ld-musl-*, /usr/lib/libcurl.so.4, /lib/libz.so.1, /usr/lib/libcares.so.2,
#   /usr/lib/libnghttp2.so.14, /usr/lib/libidn2.so.0, /usr/lib/libpsl.so.5,
#   /lib/libssl.so.3, /lib/libcrypto.so.3, /usr/lib/libzstd.so.1,
#   /usr/lib/libbrotlidec.so.1, /usr/lib/libbrotlicommon.so.1,
#   /usr/lib/libunistring.so.5
RUN chmod 755 /out/app/entrypoint.sh \
 && chown -R 10001:10001 /out/app /out/home/lume \
 && apk add --no-cache curl \
 && cp /usr/bin/curl /out/bin/curl \
 && cp /lib/ld-musl-*.so.1 /lib/libz.so.1 /lib/libssl.so.3 /lib/libcrypto.so.3 /out/lib/ \
 && for a in sh ash mkdir chmod printf cat ls cp wget sleep grep sed seq; do ln -s /bin/busybox /out/bin/$a; done \
 && cp /bin/busybox /out/bin/busybox \
 && cp /etc/ssl/certs/ca-certificates.crt /out/etc/ssl/certs/ \
 && cp /usr/lib/libcurl.so.4* /usr/lib/libcares.so.2* /usr/lib/libnghttp2.so.14* \
        /usr/lib/libidn2.so.0* /usr/lib/libpsl.so.5* /usr/lib/libzstd.so.1* \
        /usr/lib/libbrotlidec.so.1* /usr/lib/libbrotlicommon.so.1* \
        /usr/lib/libunistring.so.5* /out/lib/ \
 && printf 'lume:x:10001:10001:lume user:/home/lume:/bin/sh\n' > /out/etc/passwd \
 && printf 'lume:x:10001:\n' > /out/etc/group \
 && printf 'hosts: files dns\n' > /out/etc/nsswitch.conf

# ---------- 阶段 3: runtime (scratch) ----------
FROM scratch
COPY --from=rtlayer /out/ /
USER 10001
WORKDIR /app
EXPOSE 8081 8082 8083
# entrypoint 用 busybox cryptpw 生成 $6$ htpasswd 后 exec 主程序, PID 1 仍
# 是 lume 本体, 信号行为不变; healthcheck 用 busybox wget(见 compose)。
ENTRYPOINT ["/app/entrypoint.sh"]
CMD ["./bin/lume", "examples/invest.lume"]