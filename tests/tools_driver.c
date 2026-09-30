#include "lume.h"
#include "minijson.h"
#include "tools.h"

/* Bridge integration: proves a `tool` declared in the DSL is visible to the
 * embedded agent registry with the right name/desc/params and that a tool
 * dispatch through tools_dispatch() runs the DSL function and JSON-encodes
 * its return value. Registration happens once per process (the embed library
 * snapshots the tool table before forks), so this is a one-shot harness. */

int main(void) {
    const char *src =
        "tool \"greet\", \"Say hello to a name\", { name: \"string\" }, func(arg) {\n"
        "  return { greeting: \"hi, \" + arg.name + \"!\" };\n"
        "};\n"
        "tool \"echo\", \"return what you got\", {}, func(a) {\n"
        "  return { got: a };\n"
        "};\n"
        "tool \"echo_sid\", \"echo the agent session id\", {}, func(a) {\n"
        "  return { sid: str(get(a, \"session\")) };\n"
        "};\n"
        "tool \"math\", \"int math\", { a: int, b: int }, func(arg) {\n"
        "  return { sum: arg.a + arg.b };\n"
        "};\n"
        "tool \"shape\", \"zero defaults\", { label: string, ok: bool, ratio: float }, func(arg) {\n"
        "  return { label: arg.label, ok: arg.ok, ratio: arg.ratio };\n"
        "};\n"
        "tool \"full\", \"already-schema params\", { n: { type: \"int\" } }, func(arg) {\n"
        "  return { n: arg.n };\n"
        "};\n";
    char err[512] = {0};
    Node *prog = parse_program(src, err, sizeof(err));
    if (!prog) {
        fprintf(stderr, "parse error: %s\n", err);
        return 1;
    }
    if (!type_check_program(prog, err, sizeof(err))) {
        fprintf(stderr, "type error: %s\n", err);
        return 1;
    }
    VM vm;
    vm_init(&vm);
    bridge_init(&vm);
    exec_program(&vm, prog);
    if (vm.error) {
        fprintf(stderr, "exec error: %s\n", vm.error_msg);
        return 2;
    }

    if (tools_count() != 6) {
        fprintf(stderr, "expected 6 registered tools, saw %d\n", tools_count());
        return 3;
    }

    /* the { name: "string" } sugar must reach the upstream as a schema object */
    const char *schema = tools_schema_json();
    if (!schema || !strstr(schema, "{\"name\":{\"type\":\"string\"}}")) {
        fprintf(stderr, "tool schema not upgraded: %s\n", schema ? schema : "(null)");
        return 11;
    }

    const ToolDef *greet = NULL;
    for (int i = 0; i < tools_count(); i++) {
        const ToolDef *t = tools_get(i);
        if (strcmp(t->name, "greet") == 0) greet = t;
    }
    if (!greet || strcmp(greet->desc, "Say hello to a name") != 0 ||
        strstr(greet->params, "\"name\"") == NULL) {
        fprintf(stderr, "greet tool metadata mismatch\n");
        return 4;
    }

    /* bare type keywords in params must serialise like the quoted sugar:
     * { a: int } -> {"a":{"type":"int"}} */
    if (!schema || !strstr(schema, "\"a\":{\"type\":\"int\"}") ||
        !strstr(schema, "\"b\":{\"type\":\"int\"}")) {
        fprintf(stderr, "bare-keyword params not upgraded: %s\n",
                schema ? schema : "(null)");
        return 12;
    }
    if (!schema || !strstr(schema, "\"label\":{\"type\":\"string\"}") ||
        !strstr(schema, "\"ratio\":{\"type\":\"float\"}") ||
        !strstr(schema, "\"ok\":{\"type\":\"bool\"}")) {
        fprintf(stderr, "scalar keyword params not upgraded: %s\n",
                schema ? schema : "(null)");
        return 19;
    }

    sbuf out = {0};
    if (tools_dispatch("greet", "{\"name\":\"Eri\"}", NULL, &out) != 0) {
        fprintf(stderr, "dispatch failed\n");
        return 5;
    }
    if (strcmp(out.p, "{\"greeting\":\"hi, Eri!\"}") != 0) {
        fprintf(stderr, "greet result mismatch: %s\n", out.p ? out.p : "(null)");
        return 6;
    }

    sbuf out2 = {0};
    if (tools_dispatch("echo", "{\"deep\":{\"k\":1}}", NULL, &out2) != 0) {
        fprintf(stderr, "echo dispatch failed\n");
        return 7;
    }
    if (strcmp(out2.p, "{\"got\":{\"deep\":{\"k\":1}}}") != 0) {
        fprintf(stderr, "echo result mismatch: %s\n", out2.p ? out2.p : "(null)");
        return 8;
    }

    /* the agent loop's conversation id must reach the DSL tool as `session` */
    sbuf out3 = {0};
    if (tools_dispatch("echo_sid", "{}", "sess-agent-42", &out3) != 0) {
        fprintf(stderr, "echo_sid dispatch failed\n");
        return 9;
    }
    if (strcmp(out3.p, "{\"sid\":\"sess-agent-42\"}") != 0) {
        fprintf(stderr, "session id mismatch: %s\n", out3.p ? out3.p : "(null)");
        return 10;
    }

    /* typed member access + schema-derived zero default: `b` is absent, so
     * `arg.b` reads as 0 and the sum is still valid */
    sbuf out4 = {0};
    if (tools_dispatch("math", "{\"a\":4,\"b\":5}", NULL, &out4) != 0) {
        fprintf(stderr, "math dispatch failed\n");
        return 13;
    }
    if (strcmp(out4.p, "{\"sum\":9}") != 0) {
        fprintf(stderr, "math result mismatch: %s\n", out4.p ? out4.p : "(null)");
        return 14;
    }
    sbuf out5 = {0};
    if (tools_dispatch("math", "{\"a\":4}", NULL, &out5) != 0) {
        fprintf(stderr, "math (missing b) dispatch failed\n");
        return 15;
    }
    if (strcmp(out5.p, "{\"sum\":4}") != 0) {
        fprintf(stderr, "math zero-default mismatch: %s\n",
                out5.p ? out5.p : "(null)");
        return 16;
    }

    /* string / bool / float zero values flow through untouched */
    sbuf out6 = {0};
    if (tools_dispatch("shape", "{}", NULL, &out6) != 0) {
        fprintf(stderr, "shape dispatch failed\n");
        return 17;
    }
    if (strcmp(out6.p, "{\"label\":\"\",\"ok\":false,\"ratio\":0}") != 0) {
        fprintf(stderr, "shape zero-default mismatch: %s\n",
                out6.p ? out6.p : "(null)");
        return 18;
    }

    /* already-schema param objects are passed through and still type `arg` */
    if (!schema || !strstr(schema, "\"n\":{\"type\":\"int\"}")) {
        fprintf(stderr, "full-schema params not passed through: %s\n",
                schema ? schema : "(null)");
        return 20;
    }
    sbuf out7 = {0};
    if (tools_dispatch("full", "{}", NULL, &out7) != 0) {
        fprintf(stderr, "full dispatch failed\n");
        return 21;
    }
    if (strcmp(out7.p, "{\"n\":0}") != 0) {
        fprintf(stderr, "full zero-default mismatch: %s\n",
                out7.p ? out7.p : "(null)");
        return 22;
    }

    /* quoted sugar { name: "string" } types the arg too: missing name -> "" */
    sbuf out8 = {0};
    if (tools_dispatch("greet", "{}", NULL, &out8) != 0) {
        fprintf(stderr, "greet (missing name) dispatch failed\n");
        return 23;
    }
    if (strcmp(out8.p, "{\"greeting\":\"hi, !\"}") != 0) {
        fprintf(stderr, "quoted-sugar zero-default mismatch: %s\n",
                out8.p ? out8.p : "(null)");
        return 24;
    }

    printf("tools registered, schemas ok, dispatch JSON round-trips ok\n");
    return 0;
}