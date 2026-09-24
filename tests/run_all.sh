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
./bin/lume --check examples/demo.lume > /dev/null || fail "parse demo"
./bin/lume --check examples/lang-basics.lume > /dev/null || fail "parse lang-basics"
./bin/lume --check examples/hello.lume > /dev/null || fail "parse hello"
# pure-language script must compute, print and exit 0
OUT=$(./bin/lume examples/lang-basics.lume 2>&1)
[ "$?" -eq 0 ] || fail "run lang-basics"
for token in "sq_dist(3,4) = 25" "strict bools ok" "21 / 7 = 3"; do
    printf '%s' "$OUT" | grep -Fq "$token" || fail "lang-basics missing '$token'"
done
pass "build + parse demo/lang-basics"

# 2. interpreter unit tests ------------------------------------------------
./tests/smoke-bin || fail "interpreter unit tests"
pass "interpreter unit tests"

# 3. tool registry integration (no HTTP) -------------------------------------
./tests/tools-bin || fail "tool integration"
pass "tool registry + dispatch"

# 4. live server over HTTP ---------------------------------------------------
# Redirect the access log to a per-run temp path (access_log in server{}),
# so tests never write into the repo's ./logs/ directory.
sed "s|port = [0-9]*;|port = $PORT; access_log = \"$ACCESS_LOG/access.log\";|" examples/demo.lume > /tmp/lume-demo-test.lume
export LLM_API_KEY=   # force the offline agent demo engine: tests never hit a real LLM
./bin/lume /tmp/lume-demo-test.lume > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; rm -rf "$ACCESS_LOG" "$SERVER_LOG"' EXIT

for _ in $(seq 1 50); do
    curl -s -m 1 http://127.0.0.1:$PORT/sum > /dev/null 2>&1 && break
    sleep 0.1
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
trap 'kill $SERVER_PID 2>/dev/null; pkill -f "[b]in/lume /tmp/lume-watch-test" 2>/dev/null; rm -rf "$ACCESS_LOG" "$SERVER_LOG"' EXIT
sed "s|port = [0-9]*;|port = $WATCH_PORT;|" examples/demo.lume > /tmp/lume-watch-test.lume
./bin/lume --watch /tmp/lume-watch-test.lume > "$SERVER_LOG" 2>&1 &
WATCH_PID=$!
for _ in $(seq 1 50); do
    curl -s -m 1 http://127.0.0.1:$WATCH_PORT/sum > /dev/null 2>&1 && break
    sleep 0.1
done
[ "$(curl -s -m 3 http://127.0.0.1:$WATCH_PORT/sum)" = "40 + 2 = 42" ] || fail "watch: child not serving after start"
pass "watch: starts and serves"

# valid edit -> watcher restarts the child, new route is live
perl -pi -e 's/^run\(\);$/route "GET", "\/watch-alive", func(req) { return "alive"; };\nrun();/' /tmp/lume-watch-test.lume
sleep 2
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

echo
echo "all tests passed"
exit 0