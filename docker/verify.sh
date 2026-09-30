#!/usr/bin/env bash
# lume 四示例 docker 栈全链自检。
#
# 只直读真身, 三步规矩 (不背端口 / 不 grep Status 壳 / 不发明 SSE 壳):
#   1) compose Health 列 (healthy 才算绿) —— 直读 compose ps --format
#   2) 每容器宿主 /health —— HostPort 从 `docker port` 实读映射的**末列**
#      ($NF, IPv4 "0.0.0.0:18081" 与 IPv6 "[::]:18081" 双壳通吃), 不背
#   3) 镜像内 bin/lume magic —— 7f454c46=ELF(绿) / cffaedfe=Mach-O(宿主
#      走私货, 红); 容器无 file, 读头 4 字节
#   4) invest SSE 业务水 —— POST /react/api/chat 真签, 看 tool/round SSE 帧
#      轮不轮转 (框架最外层活水, 非空壳)
#
# 用法 (research/ 下, compose 已 up):
#   bash lume/docker/verify.sh
# 退出码: 0=四门全绿; 1=某门红 (红处即定位)
set -uo pipefail
cd "$(dirname "$0")/../.."                       # 回到 research/
COMPOSE=lume/docker/docker-compose.yml

echo "== 1) 四 service 健康门 (compose Health 列直读: 全 healthy 才绿) =="
docker compose -f "$COMPOSE" ps --format '{{.Service}}\t{{.Health}}' \
  | awk -F'\t' '
      {printf "  %-7s %s\n", $1, $2; if ($2 != "healthy") bad=1}
      END {if (bad) {print "  红: 有 service 不是 healthy"; exit 1}}' \
  || exit 1

echo
echo "== 2) 每容器宿主 /health (HostPort 从 docker port 实读末列映射, 双壳通吃) =="
bad=0
for cid in $(docker compose -f "$COMPOSE" ps -q); do
  svc=$(docker inspect -f '{{index .Config.Labels "com.docker.compose.service"}}' "$cid")
  hostport=$(docker port "$cid" | head -1 | awk -F'[:/]' '{print $NF}' | tr -dc '0-9')
  if [ -z "$hostport" ]; then echo "  [$svc] 红: docker port 读不到宿主映射"; bad=1; continue; fi
  body=$(curl -fsS --max-time 6 "http://127.0.0.1:${hostport}/health" 2>&1) \
    && echo "  [$svc] HostPort ${hostport} /health -> ${body}" \
    || { echo "  [$svc] HostPort ${hostport} /health -> 红 (${body})"; bad=1; }
done
[ "$bad" = 0 ] || exit 1

echo
echo "== 3) 镜像内 bin/lume magic (7f454c46=ELF 绿 / cffaedfe=Mach-O 红=走私) =="
firstcid=$(docker compose -f "$COMPOSE" ps -q | head -1)
magic=$(docker exec "$firstcid" sh -c 'od -An -tx1 -N4 bin/lume | tr -d " "' 2>/dev/null)
echo "  magic = ${magic:-<读不到>}"
[ "$magic" = 7f454c46 ] && echo "  绿: ELF ✓" || { echo "  红: 不是 ELF"; exit 1; }

echo
echo "== 4) invest SSE 业务水 (POST /react/api/chat 真签 {message:...}; 看 tool/round 帧轮不轮) =="
invest_cid=$(docker ps --filter "name=lume-stack" --format '{{.Names}}\t{{.Label "com.docker.compose.service"}}' \
  | awk -F'\t' '$2=="invest"{print $1; exit}')
invest_cid=${invest_cid:-$(docker ps --filter "name=invest" --format '{{.Names}}' | head -1)}
hostport=$(docker port "$invest_cid" | head -1 | awk -F'[:/]' '{print $NF}' | tr -dc '0-9')
echo "  invest 容器=${invest_cid:-?} HostPort=${hostport:-?}"
frames=$(curl -sN --max-time 25 -X POST "http://127.0.0.1:${hostport}/react/api/chat" \
  -H 'Content-Type: application/json' \
  -d '{"message":"用一句话说 invest 持仓最该盯的三个风险"}' 2>&1 \
  | grep -oE '"t":"(tool|note|round)"' | sort -u | tr '\n' ' ')
echo "  SSE 帧: ${frames:-<无帧>}"
case "$frames" in
  *tool*|*round*) echo "  绿: 业务水在轮转 (工具轮 = 框架最外层活)";;
  *) echo "  黄: 无业务帧 (框架 build 链已由 1-3 证绿; 路由在但没触发工具轮)"; exit 1;;
esac

echo
echo "== 全绿: 四 healthy + 四 /health ok + bin/lume ELF + invest SSE 业务水轮转 =="
