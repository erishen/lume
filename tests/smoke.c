#include "lume.h"
#include "tools.h"
#include "skills.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* Headless interpreter/runtime unit tests (no HTTP needed). Each snippet is
 * parsed, type checked, executed against a fresh VM, and its stdout (from
 * print()) is compared byte-for-byte against the expected output. */

static int tests_run = 0;
static int tests_failed = 0;
static int saved_stdout = -1;

static int capture_begin(char *path) {
    snprintf(path, 256, "/tmp/lume-smoke-%d.out", (int)getpid());
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    dup2(fd, STDOUT_FILENO);
    return fd;
}

static void capture_end(int fd, char *path, char *out, size_t cap) {
    fflush(stdout);
    if (saved_stdout >= 0) {  /* restore the original stdout */
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdout);
        saved_stdout = -1;
    }
    close(fd);
    int rfd = open(path, O_RDONLY);
    ssize_t n = rfd >= 0 ? read(rfd, out, cap - 1) : -1;
    if (rfd >= 0) close(rfd);
    out[n < 0 ? 0 : n] = '\0';
}

static void check(const char *name, const char *src, const char *expect) {
    tests_run++;
    char err[512] = {0};
    Node *prog = parse_program(src, err, sizeof(err));
    VM vm;
    if (!prog) {
        fprintf(stderr, "FAIL %-32s parse: %s\n", name, err);
        tests_failed++;
        return;
    }
    if (!type_check_program(prog, err, sizeof(err))) {
        fprintf(stderr, "FAIL %-32s typecheck: %s\n", name, err);
        tests_failed++;
        return;
    }
    vm_init(&vm);
    char path[256];
    int fd = capture_begin(path);
    exec_program(&vm, prog);
    char out[8192];
    capture_end(fd, path, out, sizeof(out));
    unlink(path);

    if (vm.error) {
        fprintf(stderr, "FAIL %-32s runtime: %s\n", name, vm.error_msg);
        tests_failed++;
    } else if (strcmp(out, expect) != 0) {
        fprintf(stderr, "FAIL %-32s output\n  got: %s\n want: %s\n", name, out,
                expect);
        tests_failed++;
    } else {
        printf("ok   %s\n", name);
    }
}

/* The snippet must be REJECTED by the type checker with `want_sub` in the
 * message (proves the strong-typing pass actually catches things). */
static void reject(const char *name, const char *src, const char *want_sub) {
    tests_run++;
    char err[512] = {0};
    Node *prog = parse_program(src, err, sizeof(err));
    bool accepted = prog && type_check_program(prog, err, sizeof(err));
    if (accepted || !strstr(err, want_sub)) {
        fprintf(stderr,
                "FAIL %-32s expected type error containing '%s'; got: %s\n",
                name, want_sub, accepted ? "accepted (no error)" : err);
        tests_failed++;
    } else {
        printf("ok   %s\n", name);
    }
}

int main(void) {
    /* Seed the lib registries so the discovery-builtin tests below are
     * deterministic: tools_init() registers the 7 builtins; skills_init()
     * picks up a scratch skill we drop in $HARNESS_SKILLS_DIR. */
    static const char *SKILLS_SCRATCH = "/tmp/lume-smoke-skills";
    {
        char buf[512];
        snprintf(buf, sizeof buf, "%s/smoke-skill", SKILLS_SCRATCH);
        mkdir(SKILLS_SCRATCH, 0755);
        mkdir(buf, 0755);
        snprintf(buf, sizeof buf, "%s/smoke-skill/SKILL.md", SKILLS_SCRATCH);
        FILE *f = fopen(buf, "wb");
        if (f) {
            fputs("---\nname: smoke-skill\ndescription: A smoke-test skill\n"
                  "---\n# Steps\n1. do a thing\n", f);
            fclose(f);
        }
        setenv("HARNESS_SKILLS_DIR", SKILLS_SCRATCH, 1);
    }
    skills_init();
    tools_init();

    /* mcps() reads <cwd>/.data/mcp-servers-router.json — the artifact a
     * router sync writes. Resetting it here keeps the check deterministic
     * even after manual runs have dropped a real catalog. */
    mkdir(".data", 0755);
    unlink(".data/mcp-servers-router.json");

    check("arithmetic + casts",
          "print(str(1 + 2 * 3)); print(str(int(\"42\") - 2.5));",
          "7\n39.5\n");

    check("string concat",
          "let a = \"hello\"; let b = \" world\"; print(a + b + \"!\");",
          "hello world!\n");

    check("comparisons",
          "print(str(2 < 3)); print(str(2 >= 3)); print(str(1 == 1)); "
          "print(str(1 != 2)); print(str(1 == 2));",
          "true\nfalse\ntrue\ntrue\nfalse\n");

    check("boolean logic",
          "print(str(true and false)); print(str(true or false)); "
          "print(str(not true)); print(str(not false));",
          "false\ntrue\nfalse\ntrue\n");

    check("let reassignment + if/else",
          "let x = 1;\nif (x == 1) { x = x + 10; } else { x = 0; }\n"
          "if (x > 5) { print(\"big\"); } else { print(\"small\"); }\n"
          "while (x < 15) { x = x + 1; }\nprint(str(x));",
          "big\n15\n");

    check("recursive function + return unwinding",
          "func fact(n) { if (n <= 1) { return 1; } return n * fact(n - 1); }\n"
          "print(str(fact(6)));",
          "720\n");

    check("higher-order: function expression + map + keys/len/get",
          "let add = func(a, b) { return a + b; };\n"
          "let m = { first: 1, second: 2 };\n"
          "print(str(add(m.first, m.second)));\n"
          "print(str(len(keys(m))));\n"
          "print(str(get(m, \"first\")));\n"
          "print(str(get(m, \"nope\") == null));",
          "3\n2\n1\ntrue\n");

    check("arrow fn: assignment + call",
          "let f = (a) => { return a * 2; };\nprint(str(f(21)));",
          "42\n");

    check("arrow fn: immediate call in an expression",
          "print(str((x, y) => { return x + y; }(3, 4)));",
          "7\n");

    check("arrow fn: typed params",
          "let g = (n: int) => { return str(n) + \"!\"; };\nprint(g(5));",
          "5!\n");

    check("arrow fn: zero params",
          "let h = () => { return 99; };\nprint(str(h()));",
          "99\n");

    check("contextual keyword: get(m,k) builtin still callable",
          "let m = { a: 7 };\nprint(str(get(m, \"a\")));",
          "7\n");

    check("cast-and-get: int(m,k) is sugar for int(get(m,k))",
          "let m = { a: 2, b: \"9\" };\nprint(str(int(m, \"a\") + int(m, \"b\")));",
          "11\n");

    check("cast-and-get: missing key -> type zero / explicit default",
          "let m = { a: 2 };\nprint(str(int(m, \"z\")));\n"
          "print(str(int(m, \"z\", 7)));\nprint(str(str(m, \"z\")));\n"
          "print(str(m, \"z\", \"empty\"));",
          "0\n7\nnull\nempty\n");

    check("contextual keyword: get/post usable as map key + member",
          "let m = { get: 1, post: 2 };\nprint(str(m.get + m.post));",
          "3\n");

    check("method shorthand: get/post register routes",
          "get \"/p\", (req) => { return \"x\"; };\n"
          "post \"/p\", (req) => { return \"y\"; };\n"
          "print(\"registered\");",
          "registered\n");

    check("method shorthand: bare get(m,k) statement still an expression",
          "let m = { a: 1 };\nget(m, \"a\");\nprint(\"ok\");",
          "ok\n");

    check("verbs group: `w \"path\", h` registers one route per method",
          "verbs w = [\"POST\", \"PUT\"];\n"
          "w \"/p\", (req) => { return \"x\"; };\n"
          "print(\"registered\");",
          "registered\n");

    check("verbs group: alias is also an ordinary list variable",
          "verbs w = [\"POST\", \"PUT\"];\nprint(str(len(w)));\n"
          "print(get(w, 0));",
          "2\nPOST\n");

    check("verbs map: group maps methods to labels (req.label at dispatch)",
          "verbs w = { POST: \"created\", DELETE: \"removed\" };\n"
          "print(get(w, \"POST\"));\n"
          "w \"/p\", (req) => { return req.label; };\n"
          "print(\"registered\");",
          "created\nregistered\n");

    check("built-in write group: predeclared, maps methods to labels",
          "print(str(len(write)));\nprint(get(write, \"POST\"));\n"
          "write \"/p\", (req) => { return req.label; };\n"
          "print(\"registered\");",
          "4\ncreated\nregistered\n");

    check("built-in read group: predeclared GET/HEAD list",
          "print(str(len(read)));\nprint(get(read, 0));",
          "2\nGET\n");

    check("handler-less route: `write \"/p\";` registers with the default handler",
          "write \"/p\";\nread \"/q\";\nprint(\"registered\");",
          "registered\n");

    check("tool params: bare type keyword { a: int } registers",
          "tool \"t\", \"d\", { a: int }, (a) => { return { ok: 1 }; };\n"
          "print(\"registered\");",
          "registered\n");

    check("cast-and-get: float/bool/string callable casts",
          "let m = { f: \"2.5\", on: 1 };\n"
          "print(str(float(m, \"f\")));\nprint(str(bool(m, \"on\")));\nprint(string(42));",
          "2.5\ntrue\n42\n");

    check("list literal + indexing via get",
          "let xs = [10, 20, 30];\nprint(str(len(xs)));\n"
          "print(str(get(xs, 1)));",
          "3\n20\n");

    check("stringify round-trips strings/numbers/bools/null",
          "print(stringify({ a: \"x\", b: 1, c: true, d: null }));",
          "{\"a\":\"x\",\"b\":1,\"c\":true,\"d\":null}\n");

    check("json() parses tool-style input",
          "let m = json(\"{\\\"n\\\":3,\\\"s\\\":\\\"hi\\\"}\");\n"
          "print(str(m.n)); print(m.s);",
          "3\nhi\n");

    check("GC stress: 20k allocations in a loop",
          "func spam(n) { let t = \"\";\n"
          "  let i = 0; while (i < n) { let s = str(i) + \"x\"; t = t + s; i = i + 1; }\n"
          "  return len(t);\n}\nprint(str(spam(20000)));",
          "108890\n");

    check("nested maps + member assignment",
          "let m = { a: { b: 1 } };\nm.a.b = 99;\nprint(str(m.a.b));",
          "99\n");

    /* ---- Lume typed-language coverage ---- */

    check("inference: int stays int, float widens",
          "let i = 5;\nlet f = 5.5;\n"
          "print(str(i + i)); print(str(i + f)); print(str(f + f));",
          "10\n10.5\n11\n");

    check("typed struct decl + func returns struct",
          "type Pt = { x: int, y: int };\n"
          "func add_pt(a: Pt, b: Pt): Pt {\n"
          "  return { x: a.x + b.x, y: a.y + b.y };\n}\n"
          "let p: Pt = add_pt({ x: 1, y: 2 }, { x: 10, y: 20 });\n"
          "print(str(p.x + p.y));",
          "33\n");

    check("typed list",
          "let xs: int[] = [1, 2, 3];\n"
          "let total: int = get(xs, 0) + get(xs, 1) + get(xs, 2);\n"
          "print(str(total)); print(str(len(xs)));",
          "6\n3\n");

    check("Result ?: ok path unwraps",
          "func maybe(a: int): Result {\n"
          "  if (a > 0) { return { ok: a }; }\n"
          "  return { err: \"neg\" };\n}\n"
          "func use(): Result { let v = maybe(7)?; print(str(v)); return { ok: v }; }\n"
          "use();",
          "7\n");

    check("Result ?: err propagates up",
          "func maybe(a: int): Result {\n"
          "  if (a > 0) { return { ok: a }; }\n"
          "  return { err: \"neg\" };\n}\n"
          "func use(): Result { let v = maybe(-1)?; print(\"unreachable\"); return { ok: 1 }; }\n"
          "print(stringify(use()));",
          "{\"err\":\"neg\"}\n");

    check("explicit annotation honored by checker (int->int)",
          "let x: int = 1;\nprint(stringify(x));",
          "1\n");

    check("vdom: render() serializes el() trees (SSR)",
          "print(render(el(\"a\", { href: \"/x\" }, \"Home\")));\n"
          "print(render(el(\"section\", { data_page: \"home\", hidden: true }, \"Hi\")));",
          "<a href=\"/x\">Home</a>\n"
          "<section data-page=\"home\" hidden=\"true\">Hi</section>\n");

    check("vdom: escaping, void tags, on* dropped server-side",
          "print(render(el(\"img\", { src: \"/p.png\", onclick: \"nope\" })));\n"
          "print(render(el(\"p\", { class: false }, \"<script>x</script>\")));",
          "<img src=\"/p.png\">\n"
          "<p>&lt;script&gt;x&lt;/script&gt;</p>\n");

    check("vdom: props map expanded + children flattened",
          "print(render(el(\"div\", { class: \"btn\" }, "
          "el(\"b\", {}, \"x\"), \" and \", 42)));",
          "<div class=\"btn\"><b>x</b> and 42</div>\n");

    check("html template: positional slots, escaping, {{ }}",
          "print(html(\"<a class='{0}' href='{1}'>{2}</a>\", \"btn\", \"/x\", \"Go\"));\n"
          "print(html(\"<b>{0}</b>\", \"<script>x</script>\"));\n"
          "print(html(\"{{ {0} }}\", 7));",
          "<a class='btn' href='/x'>Go</a>\n"
          "<b>&lt;script&gt;x&lt;/script&gt;</b>\n"
          "{ 7 }\n");

    check("html template: vnode slots render structurally",
          "print(html(\"<main>{0}</main>\", el(\"big\", { id: \"c\" }, 7)));\n"
          "print(html(\"{0}\", \"raw text\"));",
          "<main><big id=\"c\">7</big></main>\n"
          "raw text\n");

    /* ---- native discovery builtins (env/files/read_file/tools/skills) ---- */

    check("env: set var -> string, unset -> null",
          "print(str(env(\"LUME_NO_SUCH_VAR_42\")));\n"
          "print(str(len(env(\"PATH\")) > 0));",
          "null\ntrue\n");

    check("files: sorted listing + read_file success/missing path",
          "let fs = files(\"docs\");\n"
          "let found = false;\n"
          "let i = 0;\n"
          "while (i < len(fs)) {\n"
          "  if (get(fs, i) == \"LUME.md\") { found = true; }\n"
          "  i = i + 1;\n"
          "}\n"
          "print(str(found));\n"
          "print(str(len(read_file(\"docs/LUME.md\")) > 1000));\n"
          "print(str(read_file(\"no/such-file.md\") == null));",
          "true\ntrue\ntrue\n");

    check("tools: registry enumerates registered tools, sorted",
          "let ts = tools();\n"
          "print(str(len(ts) >= 7));\n"
          "print(str(get(ts, 0)));",
          "true\ncalc\n");

    check("skills: desc parsed from frontmatter",
          "let ss = skills();\n"
          "let found = false;\n"
          "let i = 0;\n"
          "while (i < len(ss)) {\n"
          "  let e = get(ss, i);\n"
          "  if (get(e, \"name\") == \"smoke-skill\") {\n"
          "    found = (get(e, \"desc\") == \"A smoke-test skill\");\n"
          "  }\n"
          "  i = i + 1;\n"
          "}\n"
          "print(str(found));",
          "true\n");

    /* Missing sync file is "no MCP servers", which is an empty list, not null.
     * Publishing null here made /discovery return "mcps": null and the hub
     * pages threw on data.mcps.length. Real read failures (OOM, oversized,
     * malformed JSON) still yield null. */
    check("mcps: missing catalog -> empty list",
          "print(str(len(mcps()) == 0));",
          "true\n");

    /* Drop the catalog the sync would produce, then verify the builtin
     * parses it end-to-end (list of server maps). */
    {
        FILE *f = fopen(".data/mcp-servers-router.json", "wb");
        if (f) {
            fputs("[{\"id\":\"fs\",\"cmd\":\"mcp-pty\"}]", f);
            fclose(f);
        }
    }

    check("mcps: parses router catalog into a list",
          "let ms = mcps();\n"
          "print(str(len(ms) == 1));\n"
          "print(str(get(get(ms, 0), \"id\")));",
          "true\nfs\n");

check("skills: index enumeration includes the scratch skill",
          "let ss = skills();\n"
          "let found = false;\n"
          "let i = 0;\n"
          "while (i < len(ss)) {\n"
          "  if (get(get(ss, i), \"name\") == \"smoke-skill\") { found = true; }\n"
          "  i = i + 1;\n"
          "}\n"
          "print(str(len(ss) >= 1));\n"
          "print(str(found));",
          "true\ntrue\n");

    /* ---- product-tool builtins (invest.lume portfolio + report) ---- */

    check("strftime: localtime format of a timestamp",
          "print(strftime(\"%Y-%m-%d\", 0));"
          "print(strftime(\"%Y%m%d\", 0));",
          "1970-01-01\n19700101\n");

    check("put: dynamic-key map write + reference mutation",
          "let h = {};\n"
          "put(h, \"aapl\", { name: \"Apple\", units: 10, avg_cost: 150.5 });\n"
          "let cur = get(h, \"aapl\", null);\n"
          "cur.units = cur.units + 2;\n"
          "put(h, \"tsla\", { name: \"Tesla\", units: 5, avg_cost: 250.0 });\n"
          "print(str(len(keys(h)) == 2));\n"
          "print(str(get(h, \"aapl\", null).units));",
          "true\n12\n");

    /* The invest.lume ledger shape in miniature: weighted-average upsert +
     * remove via put(). Mirrors portfolio_add/portfolio_remove. */
    check("portfolio ledger: weighted-average upsert + remove",
          "let h = {};\n"
          "put(h, \"aapl\", { name: \"Apple\", units: 10, avg_cost: 100.0 });\n"
          "let cur = get(h, \"aapl\", null);\n"
          "let nu = cur.units + 10;\n"
          "cur.avg_cost = (cur.units * cur.avg_cost + 10 * 120.0) / nu;\n"
          "cur.units = nu;\n"
          "print(str(get(h, \"aapl\", null).units));\n"
          "print(str(get(h, \"aapl\", null).avg_cost));\n"
          "let out = {};\n"
          "let ks = keys(h);\n"
          "let i = 0;\n"
          "while (i < len(ks)) {\n"
          "  let k = get(ks, i);\n"
          "  if (k != \"aapl\") { put(out, k, get(h, k, null)); }\n"
          "  i = i + 1;\n"
          "}\n"
          "print(str(len(keys(out)) == 0));",
          "20\n110\ntrue\n");

    /* mkdir + write_file round-trip. write_file must read the inline string
     * payload — reading as.str.data (never set) was a latent crash the moment
     * fopen succeeded. */
    check("mkdir + write_file round-trip",
          "mkdir(\"/tmp/lume-smoke-data/a/b\");\n"
          "let ok = write_file(\"/tmp/lume-smoke-data/a/b/x.txt\", \"hibytes\");\n"
          "print(str(ok));\n"
          "print(read_file(\"/tmp/lume-smoke-data/a/b/x.txt\"));",
          "true\nhibytes\n");

    /* ---- type checker rejects ---- */

    reject("let type mismatch", "let x: int = \"hi\";", "assignable");
    reject("strict bool in if",
          "let x = 1;\nif (x) { print(\"y\"); }", "expected bool");
    reject("strict bool in and",
          "let x = 1;\nprint(str(true and x));", "expected bool");
    reject("unknown type name", "type A = { v: Nope };", "unknown type");
    reject("? outside a function",
          "func f(): Result { return { err: \"e\" }; }\nlet r = f()?;",
          "inside a function");
    reject("? on a non-Result call",
          "func f(): int { return 1; }\n"
          "func g(): Result { let x = f()?; return { ok: 1 }; }",
          "does not return Result");
    reject("undefined variable in expr", "print(x);", "undefined variable");
    reject("missing struct field",
          "type P = { x: int, y: int };\nfunc p(): P { return { x: 1 }; }",
          "missing field");
    reject("call arity mismatch", "func f(a: int) { }\nf(1, 2);", "arguments");
    reject("map key not in struct",
          "type P = { x: int };\nfunc p(): P { return { x: 1, z: 2 }; }",
          "no field");

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}