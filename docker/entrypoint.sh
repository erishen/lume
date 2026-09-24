#!/bin/sh
# 容器启动入口:把 .env 里的明文密码换成容器内的 htpasswd 文件,然后 exec 主程序。
#
# 为什么在这里生成而不是宿主机:
#   密码只有 .env 这一个来源。宿主侧再生成一份 auth/htpasswd 就要多维护一个
#   派生产物 —— 改了 .env 忘记重跑就会得到一把对不上的钥匙。放在容器启动时
#   生成,`docker compose up` 之后立刻就是 .env 里的当前值。
#
# 语义:
#   LUME_AUTH_PASSWORD 空  -> 不生成文件,服务端 htpasswd 为空 -> 认证关闭
#   LUME_AUTH_PASSWORD 非空 -> 生成 /app/auth/htpasswd(0600),认证开启
#
# 哈希格式必须是 crypt(3) 强哈希:框架的 load_htpasswd 只认 $5$/$6$/bcrypt,
# 明文与弱哈希($1$ MD5 / $apr1$ / DES 13 位)加载即失败退出 —— 弱文件永远
# 不可能变成"放行所有",见 ../agent-httpd/src/security/auth.c 头注释。
# 这里用 htpasswd -B 出 bcrypt($2y$)。

set -eu

if [ -n "${LUME_AUTH_PASSWORD:-}" ]; then
    user="${LUME_AUTH_USER:-admin}"
    mkdir -p /app/auth
    htpasswd -cbB /app/auth/htpasswd "$user" "$LUME_AUTH_PASSWORD" >/dev/null
    chmod 600 /app/auth/htpasswd
fi

exec "$@"
