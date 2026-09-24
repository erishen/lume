#!/bin/sh
# scratch/busybox 版启动入口：把 .env 的明文密码换成 /app/auth/htpasswd，然后 exec 主程序。
#
# 与 apache2-utils 版(htpasswd -cbB, bcrypt)等价：busybox `cryptpw -m sha512`
# 输出 $6$ 格式 —— agent-httpd 的 load_htpasswd 只认 $5$/$6$/bcrypt 强哈希，
# 明文与弱哈希($1$ MD5 / $apr1$ / DES 13 位)加载即失败退出（弱文件永远不可能
# 变成"放行所有"）。$6$ 是 SHA-512 crypt，与 bcrypt 同属强档。
#
# 密码只有 .env 这一个来源。LUME_AUTH_PASSWORD 空 -> 不生成文件，认证关闭。

set -eu

if [ -n "${LUME_AUTH_PASSWORD:-}" ]; then
    user="${LUME_AUTH_USER:-admin}"
    mkdir -p /app/auth
    # cryptpw 从 stdin 读密码、自动生成随机盐；printf 不引入尾换行。
    hash="$(printf '%s' "$LUME_AUTH_PASSWORD" | /bin/busybox cryptpw -m sha512)"
    printf '%s:%s\n' "$user" "$hash" > /app/auth/htpasswd
    chmod 600 /app/auth/htpasswd
fi

exec "$@"