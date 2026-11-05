#!/usr/bin/env bash
# End-to-end test for Lume: unit tests, tool integration, then a live
# agent-httpd server driven by the DSL, exercised over real HTTP.
set -uo pipefail
cd "$(dirname "$0")/.."

PORT=8999
SERVER_LOG=$(mktemp)
ACCESS_LOG=$(mktemp -d)

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok   $*"; }

# 1. build + parse checks -------------------------------------------------
make > /dev/null 2>&1 || fail "make"
# --help / -h must print usage and exit 0 (not the unknown-flag error path)
./bin/lume --help > /dev/null 2>&1 || fail "--help should exit 0"
./bin/lume -h > /dev/null 2>&1 || fail "-h should exit 0"
./bin/lume --check examples/demo.lume > /dev/null || fail "parse demo"
./bin/lume --check examples/lang-basics.lume > /dev/null || fail "parse lang-basics"
./bin/lume --check examples/hello.lume > /dev/null || fail "parse hello"
# pure-language script must compute, print and exit 0
OUT=$(./bin/lume examples/lang-basics.lume 2>&1)
[ "$?" -eq 0 ] || fail "run lang-basics"
for token in "sq_dist(3,4) = 25" "strict bools ok" "21 / 7 = 3" "accessor = ACME|D|42|3.5"; do
    printf '%s' "$OUT" | grep -Fq "$token" || fail "lang-basics missing '$token'"
done
pass "build + parse demo/lang-basics"

# 2. interpreter unit tests ------------------------------------------------
./tests/smoke-bin || fail "interpreter unit tests"
pass "interpreter unit tests"

# 2b. bitwise operators end-to-end -----------------------------------------
BITOUT=$(./bin/lume tests/test-bitwise.lume 2>&1)
[ "$?" -eq 0 ] || fail "run test-bitwise"
printf '%s' "$BITOUT" | grep -Fq "bitwise ok 18" || fail "test-bitwise ($BITOUT)"
# typecheck rejects non-int operands
cat > /tmp/lume-bit-bad.lume <<'LUME'
print(1.5 & 1);
LUME
./bin/lume --check /tmp/lume-bit-bad.lume >/dev/null 2>&1 && fail "bitwise float should be rejected"
pass "bitwise operators + int-only typecheck"

# 2c. block scoping (P1-9) end-to-end ---------------------------------------
# 块内 let 遮蔽/恢复/新绑定删除 + GC root(块内分配触发 GC 后外层字符串恢复)。
BLOUT=$(./bin/lume tests/block-scope.lume 2>&1)
[ "$?" -eq 0 ] || fail "run block-scope"
printf '%s' "$BLOUT" | grep -Fq "block-scope ok" || fail "block-scope ($BLOUT)"
pass "block scoping (shadow/restore/GC-root)"

# 2d. for-init scoping (P1-3) ------------------------------------------------
# 经典 for 整体活在子作用域: 兄弟循环可同名(C99), init 变量循环结束不可见;
# for-init 不得遮蔽外层同名函数局部(两编译后端的扁平预扫描 + asg_find
# 位置感知会让嵌套同名循环读错变量)。
FSOUT=$(./bin/lume tests/for-scope.lume 2>&1)
[ "$?" -eq 0 ] || fail "run for-scope"
printf '%s' "$FSOUT" | grep -Fq "for-scope ok" || fail "for-scope ($FSOUT)"
# for-init shadowing an outer local is rejected
cat > /tmp/lume-for-bad.lume <<'LUME'
let i = 1;
for (let i = 0; i < 1; i = i + 1) { }
LUME
./bin/lume --check /tmp/lume-for-bad.lume >/dev/null 2>&1 && fail "for-init shadow should be rejected"
# nested same-name for-init is also rejected (would mis-bind via asg_find)
cat > /tmp/lume-for-bad2.lume <<'LUME'
for (let i = 0; i < 1; i = i + 1) {
  for (let i = 0; i < 1; i = i + 1) { }
}
LUME
./bin/lume --check /tmp/lume-for-bad2.lume >/dev/null 2>&1 && fail "nested for-init shadow should be rejected"
pass "for-init scoping (sibling loops + shadow rejection)"

# 2e. cross-repo semantic baseline -------------------------------------------
# lume has no native emitter, so core's three-backend consistency suite does
# not apply here (see tests/baseline.provenance). Instead we pin observable
# interpreter semantics byte-for-byte against lume-core: the .lume files are
# verbatim core fixtures and the .expected files are core interpreter output.
# This is what catches a partial hand-merge of the HOST_PINNED typecheck.c /
# value.c / interp.c, which the synced-file md5 baseline cannot see.
BASE_OUT=$(mktemp)
for base in baseline-literals-bitops baseline-block-scope; do
    ./bin/lume "tests/$base.lume" > "$BASE_OUT" 2>/dev/null || fail "run $base"
    tr -d '\r' < "$BASE_OUT" > "$BASE_OUT.clean"
    if ! diff -u "tests/$base.expected" "$BASE_OUT.clean"; then
        fail "semantic baseline $base drifted from lume-core (see tests/baseline.provenance)"
    fi
    pass "semantic baseline: $base matches lume-core interpreter"
done
rm -f "$BASE_OUT" "$BASE_OUT.clean"

# 3. tool registry integration (no HTTP) -------------------------------------
./tests/tools-bin || fail "tool integration"
pass "tool registry + dispatch"

# 4. live server over HTTP ---------------------------------------------------
# Redirect the access log to a per-run temp path (access_log in server{}),
# so tests never write into the repo's ./logs/ directory.
# demo.lume 拆库后带 import（相对当前文件目录）: sed 到 /tmp 会使相对路径
# 解析失败（服务起不来），故 import 行同步改写为绝对路径（loader realpath 支持）。
sed -e "s|port = [0-9]*;|port = $PORT; access_log = \"$ACCESS_LOG/access.log\";|" \
    -e "s|import \"demo/ui.lume\" as ui;|import \"$PWD/examples/demo/ui.lume\" as ui;|" \
    examples/demo.lume > /tmp/lume-demo-test.lume
export LLM_API_KEY=   # force the offline agent demo engine: tests never hit a real LLM
./bin/lume /tmp/lume-demo-test.lume > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
# prefork 架构下 kill 父进程会留下孤儿 worker 继续占用端口: 用 pkill 按命令行
# 兜底清掉整个服务实例, 否则下次运行 curl 会打到残留实例(状态被前序测试污染)。
# prefork 架构下 kill 父进程会留下孤儿 worker: 按端口 lsof 兜底清干净
# (pkill/pgrep 依赖 sysmond, 在本机沙箱环境不可用)。
trap 'kill $SERVER_PID 2>/dev/null; lsof -ti tcp:$PORT 2>/dev/null | xargs kill -9 2>/dev/null; rm -rf "$ACCESS_LOG" "$SERVER_LOG"' EXIT

# MCP 冷启动（fs/memory/think resident）在 CI 环境可能超过 5s：探测窗口拉长
# 到 ~30s（连接被拒时 curl 秒回，循环实际耗时按启动速度自适应）。
for _ in $(seq 1 200); do
    curl -s -m 1 http://127.0.0.1:$PORT/sum > /dev/null 2>&1 && break
    sleep 0.15
done

[ "$(curl -s -m 3 http://127.0.0.1:$PORT/hello)" = \
  "hello from lume, you asked for /hello (GET)" ] || fail "GET /hello"
pass "GET /hello"

[ "$(curl -s -m 3 http://127.0.0.1:$PORT/sum)" = "40 + 2 = 42" ] || fail "GET /sum"
pass "GET /sum"

BODY=$(curl -s -m 3 -X POST -d '{"ping":"pong"}' http://127.0.0.1:$PORT/echo)
case "$BODY" in
    *'"method":"POST"'*'"path":"/echo"'*'"got_body":"{\"ping\":\"pong\"}"'*) ;;
    *) fail "POST /echo -> $BODY" ;;
esac
pass "POST /echo"

# method shorthands (get/put/delete "path", handler) dispatch any verb
[ "$(curl -s -m 3 http://127.0.0.1:$PORT/ping)" = "pong" ] || fail "GET /ping (get shorthand)"
pass "GET /ping (get shorthand)"

PUTBODY=$(curl -s -m 3 -X PUT -d '{}' http://127.0.0.1:$PORT/api/things)
case "$PUTBODY" in
    *'"method":"PUT"'*'"updated":true'*) ;;
    *) fail "PUT /api/things -> $PUTBODY" ;;
esac
pass "PUT /api/things (put shorthand)"

DELBODY=$(curl -s -m 3 -X DELETE http://127.0.0.1:$PORT/api/things)
case "$DELBODY" in
    *'"method":"DELETE"'*'"deleted":true'*) ;;
    *) fail "DELETE /api/things -> $DELBODY" ;;
esac
pass "DELETE /api/things (delete shorthand)"

# built-in `write` group: one statement registers POST/PUT/PATCH/DELETE on one
# handler, and req.label carries the conventional label for each method.
for M in POST PUT PATCH DELETE; do
    case "$M" in
        POST)   L=created  ;;
        PUT)    L=replaced ;;
        PATCH)  L=patched  ;;
        DELETE) L=deleted  ;;
    esac
    VBODY=$(curl -s -m 3 -X "$M" -d '{}' http://127.0.0.1:$PORT/api/item)
    case "$VBODY" in
        *"\"method\":\"$M\""*"\"via\":\"$L\""*) ;;
        *) fail "$M /api/item (write group -> $L) -> $VBODY" ;;
    esac
    pass "$M /api/item (write group -> $L)"
done

# handler-less route: `write "/api/ack";` uses the default JSON ack
DBODY=$(curl -s -m 3 -X POST -d '{"k":1}' http://127.0.0.1:$PORT/api/ack)
case "$DBODY" in
    *'"action":"created"'*'"method":"POST"'*'"got":"{\"k\":1}"'*) ;;
    *) fail "POST /api/ack (default handler) -> $DBODY" ;;
esac
pass "POST /api/ack (handler-less default response)"

# 4b. UI + JSON APIs (fast path: stateful GETs run on the single master VM) --
BODY=$(curl -s -m 3 http://127.0.0.1:$PORT/)
for token in "Lume." "data-page='home'" "id=\"clock\"" "class=\"hero\"" "&quot;GET&quot;"; do
    printf '%s' "$BODY" | grep -Fq "$token" || { fail "GET / missing '$token' -> $(printf '%s' "$BODY" | head -c 200)"; exit 1; }
done
pass "GET / (UI shell)"

BODY=$(curl -s -m 3 http://127.0.0.1:$PORT/counter)
for token in "data-page='counter'" "id='inc'" "<big id='count'>0</big>"; do
    printf '%s' "$BODY" | grep -Fq "$token" || { fail "GET /counter missing '$token' -> $(printf '%s' "$BODY" | head -c 200)"; exit 1; }
done
pass "GET /counter (SSR component + initial count)"

BODY=$(curl -s -m 3 http://127.0.0.1:$PORT/chat)
printf '%s' "$BODY" | grep -Fq "data-page='chat'" || fail "GET /chat missing data-page"
printf '%s' "$BODY" | grep -Fq "id='chat-root'" || fail "GET /chat missing chat root"
pass "GET /chat (agent UI shell)"

CT=$(curl -s -m 3 -D - -o /dev/null http://127.0.0.1:$PORT/app.css | grep -i '^content-type' | tr -d '\r')
[ "$CT" = "Content-Type: text/css" ] || fail "GET /app.css ($CT)"
pass "GET /app.css (static)"

CT=$(curl -s -m 3 -D - -o /dev/null http://127.0.0.1:$PORT/invest/app.js | grep -i '^content-type' | tr -d '\r')
[ "$CT" = "Content-Type: application/javascript" ] || fail "GET /invest/app.js ($CT)"
curl -s -m 3 http://127.0.0.1:$PORT/invest/app.js | grep -Fq "__LUME_UI__" || fail "GET /invest/app.js (React bundle marker missing)"
pass "GET /invest/app.js (static, React bundle)"

# extensionless static fallback: the framework's static handler maps /hello/items
# to www/hello/items.html (same idea as / -> www/invest/index.html), so page
# URLs can drop the .html suffix. A real extension or a missing twin must 404.
CT=$(curl -s -m 3 -D - -o /dev/null http://127.0.0.1:$PORT/hello/items | grep -i '^content-type' | tr -d '\r')
[ "$CT" = "Content-Type: text/html" ] || fail "GET /hello/items fallback type ($CT)"
curl -s -m 3 http://127.0.0.1:$PORT/hello/items | grep -Fq 'id="root"' || fail "GET /hello/items (not the items.html shell)"
[ "$(curl -s -m 3 -o /dev/null -w '%{http_code}' http://127.0.0.1:$PORT/hello/items.html)" = "200" ] || fail "GET /hello/items.html"
[ "$(curl -s -m 3 -o /dev/null -w '%{http_code}' http://127.0.0.1:$PORT/hello/items.txt)" = "404" ] || fail "GET /hello/items.txt (should 404, has extension)"
[ "$(curl -s -m 3 -o /dev/null -w '%{http_code}' http://127.0.0.1:$PORT/nope-not-here)" = "404" ] || fail "GET /nope-not-here (no .html twin)"
pass "GET /hello/items (extensionless -> .html static fallback)"

CNT0=$(curl -s -m 3 http://127.0.0.1:$PORT/api/count | grep -o '"count":[0-9]*')
CNT1=$(curl -s -m 3 http://127.0.0.1:$PORT/api/inc | grep -o '"count":[0-9]*')
[ -n "$CNT1" ] && [ "$CNT1" != "$CNT0" ] || fail "api/inc did not move count ($CNT0 -> $CNT1)"
pass "GET /api/count + /api/inc (state across requests)"

# a body-less map is auto-served as application/json (no stringify/type needed)
CT=$(curl -s -m 3 -D - -o /dev/null http://127.0.0.1:$PORT/api/count | grep -i '^content-type' | tr -d '\r')
[ "$CT" = "Content-Type: application/json" ] || fail "GET /api/count default type ($CT)"
pass "GET /api/count (auto JSON default)"

# discovery page: aggregation of env(), tools(), skills(), and the mcps sync
DB=$(curl -s -m 3 http://127.0.0.1:$PORT/discovery)
printf '%s' "$DB" | LC_ALL=C grep -Fq '"endpoints":' || fail "GET /discovery missing endpoints -> $(printf '%s' "$DB" | head -c 160)"
printf '%s' "$DB" | LC_ALL=C grep -Fq '"llm"' || fail "GET /discovery missing llm"
printf '%s' "$DB" | LC_ALL=C grep -Fq '"tools":[' || fail "GET /discovery missing tools -> $(printf '%s' "$DB" | head -c 160)"
printf '%s' "$DB" | LC_ALL=C grep -Fq '"skills":' || fail "GET /discovery missing skills"
pass "GET /discovery (endpoints/tools/skills/mcps)"

# agent chat loop: with LLM_API_KEY empty the framework streams the offline
# demo engine's canned reply over SSE — proves the native /react/api/chat
# endpoint (session + tool loop surface) is wired into the Lume server.
CHAT=$(curl -s -m 6 -N -X POST -d '{"message":"hi"}' http://127.0.0.1:$PORT/react/api/chat)
printf '%s' "$CHAT" | grep -Fq "demo engine" || {
    printf '%s' "$CHAT" | grep -Fq "Hello!" ||
        fail "POST /react/api/chat (demo SSE) -> $(printf '%s' "$CHAT" | head -c 200)"
}
pass "POST /react/api/chat (agent demo SSE)"

# 4c. GC stress: many requests must not crash workers or corrupt state.
for _ in $(seq 1 150); do
    curl -s -m 2 http://127.0.0.1:$PORT/sum > /dev/null
    curl -s -m 2 -X POST -d '{"n":7}' http://127.0.0.1:$PORT/echo > /dev/null
done
kill -0 $SERVER_PID 2>/dev/null || fail "server died under load"
WORKERS=$(ps aux | grep -c "[b]in/lume /tmp/lume-demo-test")
[ "$WORKERS" -ge 1 ] || fail "no server processes"
pass "GC stress (300 requests)"

# 4d. --watch hot reload (main.c run_watch): a valid edit restarts the child
#     and the new route is live; an invalid edit keeps the old child serving.
#     run() blocks, so edits must be inserted *before* the trailing run();
#     perl keeps this portable across macOS BSD sed / Linux GNU sed.
WATCH_PORT=8997
WATCH_PID=0
# watch 段会覆盖上面的 trap: 同时按端口清主服务与 watch 服务。
trap 'kill $SERVER_PID 2>/dev/null; lsof -ti tcp:$PORT 2>/dev/null | xargs kill -9 2>/dev/null; lsof -ti tcp:$WATCH_PORT 2>/dev/null | xargs kill -9 2>/dev/null; rm -rf "$ACCESS_LOG" "$SERVER_LOG"' EXIT
sed -e "s|port = [0-9]*;|port = $WATCH_PORT;|" \
    -e "s|import \"demo/ui.lume\" as ui;|import \"$PWD/examples/demo/ui.lume\" as ui;|" \
    examples/demo.lume > /tmp/lume-watch-test.lume
./bin/lume --watch /tmp/lume-watch-test.lume > "$SERVER_LOG" 2>&1 &
WATCH_PID=$!
# 探测窗口 ~30s（watch 首启同样有 MCP 冷启动）
for _ in $(seq 1 200); do
    curl -s -m 1 http://127.0.0.1:$WATCH_PORT/sum > /dev/null 2>&1 && break
    sleep 0.15
done
[ "$(curl -s -m 3 http://127.0.0.1:$WATCH_PORT/sum)" = "40 + 2 = 42" ] || fail "watch: child not serving after start"
pass "watch: starts and serves"

# valid edit -> watcher restarts the child, new route is live
perl -pi -e 's/^run\(\);$/route "GET", "\/watch-alive", func(req) { return "alive"; };\nrun();/' /tmp/lume-watch-test.lume
# child 重启同样要 MCP 冷启动: 轮询等新路由就绪(最多 ~30s)
for _ in $(seq 1 200); do
    [ "$(curl -s -m 1 http://127.0.0.1:$WATCH_PORT/watch-alive)" = "alive" ] && break
    sleep 0.15
done
[ "$(curl -s -m 3 http://127.0.0.1:$WATCH_PORT/watch-alive)" = "alive" ] || fail "watch: valid edit did not reload"
pass "watch: valid edit reloads new route"

# invalid edit -> validation fails, old child keeps serving
perl -pi -e 's/^run\(\);$/route "GET", "\/bad", func(req) { return 1 + "x"; };\nrun();/' /tmp/lume-watch-test.lume
sleep 2
[ "$(curl -s -m 3 http://127.0.0.1:$WATCH_PORT/watch-alive)" = "alive" ] || fail "watch: invalid edit killed the server"
pass "watch: invalid edit keeps old child"

kill $WATCH_PID 2>/dev/null; WATCH_PID=0
sleep 1
pkill -f "[b]in/lume /tmp/lume-watch-test" 2>/dev/null || true
pass "watch: shutdown"

# 4e. minimal server + for-in inline-temp GC regression ----------------------
# examples/minimal-server-gc.lume pins the server-side shape of the for-in
# GC-root bug (core 475e099): the route body runs for (x in range(60000))
# with an inline-temp iterable (range(60)) allocated every round. Pre-fix
# any allocation past the 2MiB GC threshold freed the iterable under the
# interpreter's cursor (0xC0000005 on Windows, "crashes on a LATER request"
# behind the old chinese/server.lume); the host interpreter carries the fix
# (hand-merged 94b9b03), so the answer must arrive intact over real HTTP —
# this exercises the HTTP-bracket / per-request arena side too.
MIN_GC_PORT=8996
sed -e "s|port = [0-9]*;|port = $MIN_GC_PORT; access_log = \"$ACCESS_LOG/min-gc.log\";|" \
    examples/minimal-server-gc.lume > /tmp/lume-min-gc-test.lume
./bin/lume /tmp/lume-min-gc-test.lume > "$SERVER_LOG" 2>&1 &
MINGC_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; kill $MINGC_PID 2>/dev/null; lsof -ti tcp:$PORT 2>/dev/null | xargs kill -9 2>/dev/null; lsof -ti tcp:$MIN_GC_PORT 2>/dev/null | xargs kill -9 2>/dev/null; rm -rf "$ACCESS_LOG" "$SERVER_LOG"' EXIT
for _ in $(seq 1 200); do
    curl -s -m 1 http://127.0.0.1:$MIN_GC_PORT/gc > /dev/null 2>&1 && break
    sleep 0.15
done
[ "$(curl -s -m 8 http://127.0.0.1:$MIN_GC_PORT/gc)" = "minimal gc ok: 1799970000" ] || fail "minimal-server-gc: route answer"
pass "minimal-server-gc: inline-temp for-in over HTTP"
kill $MINGC_PID 2>/dev/null; MINGC_PID=
# 置空而非置 0: 末尾 EXIT trap 会再 kill $MINGC_PID —— `kill 0` 是向整个
# 进程组发 SIGTERM, 会把 run_all.sh 自身、外层 recipe shell 和 make 一起
# 打死(CI 实证: "all tests passed" 后 recipe Terminated / make wait ECHILD,
# step 以 143 退出; 自 d3bc42f 引入 min-gc 段起必现)。空变量在无引号展开下
# 整个参数消失, kill 只剩 usage 错误且已被 2>/dev/null 吞掉。
pkill -f "[b]in/lume /tmp/lume-min-gc-test" 2>/dev/null || true

# 4f. first-request HTTPS egress (warmup / fork-safety regression) ------------
# 每个进程的**首次** HTTPS 出站是 macOS libobjc fork 闸门的雷区:
# lume_http_warmup() 必须在 master 单线程阶段完成 TLS 初始化, worker 里
# 第一次出站才不崩。这里的 server 启动时不做任何出站(warmup 已无外部探测,
# probe 走本地回环), /probe 就是整个进程的首次 HTTPS —— 预热缺失时 worker
# 首次出站会踩闸门。
#
# Offline skip is fail-closed. `curl -s` alone exits 0 for a proxy's 502 HTML
# page, a captive portal, or any HTTP status, so the old `if curl ...; else
# skip` silently turned this whole regression leg green on a broken network.
# Only unambiguous transport failures may skip; everything else fails. CI sets
# LUME_REQUIRE_EGRESS=1 so the authoritative leg can never be skipped there.
EGRESS_ERR=$(mktemp)
EGRESS_HTTP=$(curl -sS -o /dev/null -m 5 -w '%{http_code}' \
    https://example.com/ 2>"$EGRESS_ERR")
EGRESS_RC=$?
HTTPS_PORT=8995
if [ "$EGRESS_RC" -eq 0 ] && [ "$EGRESS_HTTP" = "200" ]; then
    cat > /tmp/lume-https-first.lume <<LUME
server {
  port = $HTTPS_PORT;
  access_log = "$ACCESS_LOG/https-first.log";
}

get "/probe", (req) => {
  let r = http_get("https://example.com/", { timeout: 5 });
  if (r["ok"]) {
    return "https-first ok";
  }
  return "https-first failed: " + str(r["err"]);
};

run();
LUME
    ./bin/lume /tmp/lume-https-first.lume > "$SERVER_LOG" 2>&1 &
    HTTPS_PID=$!
    trap 'kill $SERVER_PID 2>/dev/null; kill $HTTPS_PID 2>/dev/null; lsof -ti tcp:$PORT 2>/dev/null | xargs kill -9 2>/dev/null; lsof -ti tcp:$HTTPS_PORT 2>/dev/null | xargs kill -9 2>/dev/null; rm -rf "$ACCESS_LOG" "$SERVER_LOG"' EXIT
    READY=""
    for _ in $(seq 1 200); do
        if curl -s -m 1 http://127.0.0.1:$HTTPS_PORT/probe > /dev/null 2>&1; then READY=1; break; fi
        sleep 0.15
    done
    FIRST=$(curl -s -m 10 http://127.0.0.1:$HTTPS_PORT/probe)
    if [ "$FIRST" != "https-first ok" ]; then
        # 服务器进程的 stdout/stderr 是诊断这个 case 的唯一窗口: worker 里
        # 首次 TLS 出站的崩溃只有 lume 自己的日志能看到, run_all 的输出里
        # 只剩一个空 $FIRST。失败时把日志留在 CI 输出里再退出。
        echo "------ https-first server log (failure diagnostics) ------"
        tail -40 "$SERVER_LOG" 2>/dev/null || echo "(no server log)"
        echo "-----------------------------------------------------------"
        [ -n "$READY" ] || echo "(server never became ready within 30s)"
        fail "first-request HTTPS egress ($FIRST)"
    fi
    pass "first-request HTTPS egress (warmup before fork)"
    kill $HTTPS_PID 2>/dev/null; HTTPS_PID=
    lsof -ti tcp:$HTTPS_PORT 2>/dev/null | xargs kill -9 2>/dev/null || true
else
    # curl transport failures that unambiguously mean "no usable outbound
    # path": 6 DNS, 7 connect, 28 timeout, 35 TLS handshake, 52 empty reply,
    # 55 send, 56 recv. Everything else (rc=0 with a non-200 status, proxy
    # misconfig rc=5, bad CA rc=60, missing curl rc=127) is a broken
    # environment, not an offline one — fail instead of hiding the regression.
    EGRESS_MSG=$(tr -d '\r\n' < "$EGRESS_ERR")
    case "$EGRESS_RC" in
        0)
            fail "egress probe: example.com returned HTTP $EGRESS_HTTP (expected 200; proxy or captive portal?)"
            ;;
        6|7|28|35|52|55|56)
            if [ "${LUME_REQUIRE_EGRESS:-0}" = "1" ]; then
                fail "first-request HTTPS egress is required but host is offline (curl rc=$EGRESS_RC: $EGRESS_MSG)"
            fi
            echo "skip   first-request HTTPS egress (offline: curl rc=$EGRESS_RC $EGRESS_MSG)"
            ;;
        *)
            fail "egress probe failed with curl rc=$EGRESS_RC (not an offline condition): $EGRESS_MSG"
            ;;
    esac
fi
rm -f "$EGRESS_ERR"

# 4g. binary download envelope ----------------------------------------------
# examples/download.lume serves file bytes via read_file + the
# { type, headers, body } response envelope. The server caps request bodies
# at 64 KiB (MAX_REQUEST_SIZE), so downloads already work today while
# uploads are a transport project (lume-core TODO, multipart section);
# this pins the download shape and the try()/err 404 branch.
DL_PORT=8998
DL_CSV=/tmp/lume-dl-export.csv
DL_BIN=/tmp/lume-dl-report.bin
printf 'id,name\n1,alice\n2,bob\n' > "$DL_CSV"
printf '\x89PNG\r\n\x1a\nfakepng-download' > "$DL_BIN"
sed -e "s|port = [0-9]*;|port = $DL_PORT; access_log = \"$ACCESS_LOG/dl.log\";|" \
    -e "s|\./data/export\.csv|$DL_CSV|g" \
    -e "s|\./data/report\.bin|$DL_BIN|g" \
    examples/download.lume > /tmp/lume-dl-test.lume
./bin/lume /tmp/lume-dl-test.lume > "$SERVER_LOG" 2>&1 &
DL_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; kill $DL_PID 2>/dev/null; lsof -ti tcp:$PORT 2>/dev/null | xargs kill -9 2>/dev/null; lsof -ti tcp:$DL_PORT 2>/dev/null | xargs kill -9 2>/dev/null; rm -rf "$ACCESS_LOG" "$SERVER_LOG"' EXIT
for _ in $(seq 1 200); do
    curl -s -m 1 http://127.0.0.1:$DL_PORT/export > /dev/null 2>&1 && break
    sleep 0.15
done
[ "$(curl -s -m 3 http://127.0.0.1:$DL_PORT/export)" = "id,name
1,alice
2,bob" ] || fail "download: /export body"
DLCT=$(curl -s -m 3 -D - -o /dev/null http://127.0.0.1:$DL_PORT/export | grep -i '^content-type' | tr -d '\r')
[ "$DLCT" = "Content-Type: text/csv" ] || fail "download: /export content-type ($DLCT)"
DLCT2=$(curl -s -m 3 -D - -o /dev/null http://127.0.0.1:$DL_PORT/report | grep -i '^content-type' | tr -d '\r')
[ "$DLCT2" = "Content-Type: application/octet-stream" ] || fail "download: /report content-type ($DLCT2)"
rm -f "$DL_CSV"
[ "$(curl -s -m 3 -o /dev/null -w '%{http_code}' http://127.0.0.1:$DL_PORT/export)" = "404" ] || fail "download: /export 404 after file removed (try/err branch)"
[ "$(curl -s -m 3 -o /dev/null -w '%{http_code}' http://127.0.0.1:$DL_PORT/nope)" = "404" ] || fail "download: unknown route 404"
pass "download: binary envelope over HTTP"
kill $DL_PID 2>/dev/null; DL_PID=

# 4h. body-to-tempfile spool (multipart item (b)) ---------------------------
# Content-Length bodies above the 64 KiB read buffer are streamed to a temp
# file when they fit MAX_BODY_SPOOL_BYTES; req.body_temp_path names it and
# the handler reads it with read_file(). Bodies over the cap keep the 413 +
# forced close; small bodies stay in memory. The server is started with a
# small spool cap so one instance exercises all three tiers.
UP_PORT=8994
UP_SMALL=/tmp/lume-up-small.bin
UP_BIG=/tmp/lume-up-big.bin
UP_HUGE=/tmp/lume-up-huge.bin
head -c 10240 /dev/zero | tr '\0' 'u' > "$UP_SMALL"
head -c 102400 /dev/zero | tr '\0' 'u' > "$UP_BIG"
head -c 300000 /dev/zero | tr '\0' 'u' > "$UP_HUGE"
sed -e "s|port = [0-9]*;|port = $UP_PORT; access_log = \"$ACCESS_LOG/up.log\";|" \
    examples/upload-spool.lume > /tmp/lume-up-test.lume
MAX_BODY_SPOOL_BYTES=524288 ./bin/lume /tmp/lume-up-test.lume > "$SERVER_LOG" 2>&1 &
UP_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; kill $UP_PID 2>/dev/null; lsof -ti tcp:$PORT 2>/dev/null | xargs kill -9 2>/dev/null; lsof -ti tcp:$UP_PORT 2>/dev/null | xargs kill -9 2>/dev/null; rm -rf "$ACCESS_LOG" "$SERVER_LOG"' EXIT
for _ in $(seq 1 200); do
    curl -s -m 1 http://127.0.0.1:$UP_PORT/upload-buffered > /dev/null 2>&1 && break
    sleep 0.15
done
# small body stays in memory (req.body)
[ "$(curl -s -m 3 --data-binary @$UP_SMALL http://127.0.0.1:$UP_PORT/upload-buffered)" = "buffered 10240 bytes" ] || fail "upload-spool: small body buffered"
# 100 KiB > 64 KiB read buffer -> spooled to temp file, read back intact
[ "$(curl -s -m 3 --data-binary @$UP_BIG http://127.0.0.1:$UP_PORT/upload)" = "spooled 102400 bytes" ] || fail "upload-spool: 100 KiB spooled"
# 300 KiB > MAX_BODY_SPOOL_BYTES (512 KiB here is not enough... 300 KiB fits!)
# -- the cap in this instance is 524288, so 300 KiB still spools: pin that too
[ "$(curl -s -m 3 --data-binary @$UP_HUGE http://127.0.0.1:$UP_PORT/upload)" = "spooled 300000 bytes" ] || fail "upload-spool: 300 KiB spooled under 512 KiB cap"
pass "upload-spool: body-to-tempfile over HTTP"
kill $UP_PID 2>/dev/null; UP_PID=

# 4i. spool cap -> 413 + forced close -----------------------------------------
# Same example, but the cap is dropped to 128 KiB: the 300 KiB upload must be
# rejected 413 (and the connection closed so nothing is desynced).
UP2_PORT=8993
sed -e "s|port = [0-9]*;|port = $UP2_PORT; access_log = \"$ACCESS_LOG/up2.log\";|" \
    examples/upload-spool.lume > /tmp/lume-up2-test.lume
MAX_BODY_SPOOL_BYTES=131072 ./bin/lume /tmp/lume-up2-test.lume > "$SERVER_LOG" 2>&1 &
UP2_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; kill $UP2_PID 2>/dev/null; lsof -ti tcp:$PORT 2>/dev/null | xargs kill -9 2>/dev/null; lsof -ti tcp:$UP2_PORT 2>/dev/null | xargs kill -9 2>/dev/null; rm -rf "$ACCESS_LOG" "$SERVER_LOG"' EXIT
for _ in $(seq 1 200); do
    curl -s -m 1 http://127.0.0.1:$UP2_PORT/upload-buffered > /dev/null 2>&1 && break
    sleep 0.15
done
[ "$(curl -s -m 3 -o /dev/null -w '%{http_code}' --data-binary @$UP_HUGE http://127.0.0.1:$UP2_PORT/upload)" = "413" ] || fail "upload-spool: 300 KiB over 128 KiB cap must 413"
[ "$(curl -s -m 3 --data-binary @$UP_BIG http://127.0.0.1:$UP2_PORT/upload)" = "spooled 102400 bytes" ] || fail "upload-spool: 100 KiB still spools under 128 KiB cap"
pass "upload-spool: over-cap body 413s, within-cap spools"
kill $UP2_PID 2>/dev/null; UP2_PID=
rm -f "$UP_SMALL" "$UP_BIG" "$UP_HUGE"

# 4j. LUME_FS_ROOT allowlist (opt-in; hand-merged from lume-core) -------------
# Default OFF keeps the HTTP host's existing file access. When set, path-taking
# builtins stay inside realpath-checked descendants; LUME_FS_RO=1 adds RO.
FSR=$(mktemp -d)
FSOUT=$(mktemp)
printf 'inside' > "$FSR/ok.txt"
printf 'outside' > "$FSOUT"
cat > /tmp/lume-fs-root-inside.lume <<LUME
print(str(read_file("$FSR/ok.txt") == "inside"));
LUME
cat > /tmp/lume-fs-root-escape.lume <<LUME
print(str(read_file("$FSOUT") == "outside"));
LUME
[ "$(./bin/lume /tmp/lume-fs-root-inside.lume)" = "true" ] || fail "FS_ROOT: off by default"
[ "$(LUME_FS_ROOT=$FSR ./bin/lume /tmp/lume-fs-root-inside.lume)" = "true" ] || fail "FS_ROOT: inside root readable"
ESCAPE=$(LUME_FS_ROOT=$FSR ./bin/lume /tmp/lume-fs-root-escape.lume 2>&1); ESC_RC=$?
[ "$ESC_RC" -ne 0 ] || fail "FS_ROOT: escape must fail"
printf '%s' "$ESCAPE" | grep -Fq "outside LUME_FS_ROOT" || fail "FS_ROOT: escape error message"
cat > /tmp/lume-fs-root-ro.lume <<LUME
print(str(write_file("$FSR/new.txt", "x")));
LUME
RO=$(LUME_FS_ROOT=$FSR LUME_FS_RO=1 ./bin/lume /tmp/lume-fs-root-ro.lume 2>&1); RO_RC=$?
[ "$RO_RC" -ne 0 ] || fail "FS_ROOT: read-only must refuse write"
printf '%s' "$RO" | grep -Fq "read-only" || fail "FS_ROOT: read-only error message"
BAD=$(LUME_FS_ROOT=$FSR/does-not-exist ./bin/lume /tmp/lume-fs-root-inside.lume 2>&1); BAD_RC=$?
[ "$BAD_RC" -eq 2 ] || fail "FS_ROOT: unresolvable root must fail at startup (rc=$BAD_RC)"
printf '%s' "$BAD" | grep -Fq "does not resolve" || fail "FS_ROOT: unresolvable root error message"
pass "LUME_FS_ROOT: off by default, escape/RO/unresolvable enforced"
rm -rf "$FSR" "$FSOUT" /tmp/lume-fs-root-*.lume

echo
echo "all tests passed"
exit 0