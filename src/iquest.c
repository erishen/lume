/* iquest.c —— 投资助手产品 API(lume 本地路由)。
 *
 * 提供周报归档 / 审批设置两类只读+受控写端点,逻辑全部留在这个产品模块里,
 * 不污染通用 agent-httpd 嵌入库。写操作只有一个:设置页把 PSE_ALLOW_PAID /
 * PSE_REVIEW_PROVIDER 落进 frameworks/autogen-pse/.env(审批闸门的读取源),
 * 采用 白名单键 + 同源 Origin 校验 + 原子替换(临时文件 + rename)。
 *
 * 依赖:libagenthttpd 的 minijson(sbuf / jfind_value / jread_string)与
 * agenthttpd_route 注册。 */
#include "agenthttpd.h"
#include "minijson.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define IQUEST_JSON "application/json; charset=utf-8"

/* 文档注释里说的两个环境变量即这两个默认路径。 */
static const char *default_reports_dir(void) {
    const char *d = getenv("IQUEST_REPORTS_DIR");
    return d ? d : "<PROJECT_ROOT>/work/harness/resolve-studio/sandbox/weekly-investment";
}
static const char *default_env_file(void) {
    const char *d = getenv("IQUEST_ENV_FILE");
    return d ? d : "<PROJECT_ROOT>/frameworks/autogen-pse/.env";
}

#define ROUTE_REPORTS_GET "/api/reports"
#define ROUTE_REPORTS_PREFIX "/api/reports/"
#define ROUTE_SETTINGS "/api/settings"

/* ---------- response helpers ---------- */

static void finish_json(HttpResponse *res, int status, const char *status_text,
                        sbuf *b) {
    res->status_code = status;
    strncpy(res->status_text, status_text, sizeof(res->status_text) - 1);
    strncpy(res->content_type, IQUEST_JSON, sizeof(res->content_type) - 1);
    strncpy(res->cache_control, "no-store", sizeof(res->cache_control) - 1);
    res->body = b->p ? b->p : strdup("");
    res->body_length = (int)(b->p ? b->len : 0);
}

static void json_error(HttpResponse *res, int status, const char *msg) {
    sbuf b = {0};
    sb_str(&b, "{\"error\":");
    sb_json_str(&b, msg);
    sb_str(&b, "}");
    finish_json(res, status, status == 200 ? "OK"
                        : status == 400 ? "Bad Request"
                        : status == 403 ? "Forbidden"
                        : status == 404 ? "Not Found" : "Internal Server Error", &b);
}

/* 同源守卫:浏览器发 POST 必带 Origin。本服务是 http://host[:port],因此
 * Origin 必须精确等于 "http://" + Host 头才算放行;其它站(CSRF)一律拒绝。
 * 无 Origin(Curl/脚本)视为本机可信源。 */
static int origin_ok(const HttpRequest *req) {
    if (!req->origin[0]) return 1;
    if (strcmp(req->origin, "null") == 0) return 0;
    char expected[320];
    int n = snprintf(expected, sizeof expected, "http://%s", req->host);
    return n > 0 && (size_t)n < sizeof expected &&
           strcmp(req->origin, expected) == 0;
}

/* ---------- /api/reports ---------- */

/* 一个周报条目:文件名里的 <model>__weekly_review_<date>.md 拆出字段。 */
typedef struct {
    char name[512];
    char model[160];
    char date[160];
    long size;
    time_t mtime;
    char preview[288];
} ReportEntry;

static void parse_report_name(const char *name, ReportEntry *e) {
    const char *marker = "__weekly_review_";
    e->model[0] = e->date[0] = '\0';
    const char *p = strstr(name, marker);
    if (p && p > name) {
        size_t ml = (size_t)(p - name);
        if (ml >= sizeof(e->model)) ml = sizeof(e->model) - 1;
        memcpy(e->model, name, ml);
        e->model[ml] = '\0';
        const char *q = p + strlen(marker);
        const char *dot = strstr(q, ".md");
        if (dot) {
            size_t dl = (size_t)(dot - q);
            if (dl >= sizeof(e->date)) dl = sizeof(e->date) - 1;
            memcpy(e->date, q, dl);
            e->date[dl] = '\0';
        }
    }
}

static int report_entries(ReportEntry **out, int *out_n) {
    ReportEntry *arr = NULL;
    int n = 0, cap = 0;
    const char *dir = default_reports_dir();
    DIR *d = opendir(dir);
    if (!d) { *out = NULL; *out_n = 0; return 0; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        size_t l = strlen(e->d_name);
        if (l < 4 || strcmp(e->d_name + l - 3, ".md") != 0) continue;
        char full[4096];
        if (snprintf(full, sizeof full, "%s/%s", dir, e->d_name) >=
            (int)sizeof full)
            continue;
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            ReportEntry *na = realloc(arr, (size_t)cap * sizeof *arr);
            if (!na) break;
            arr = na;
            memset(na + n, 0, (size_t)(cap - n) * sizeof *na);
        }
        ReportEntry *r = &arr[n];
        snprintf(r->name, sizeof r->name, "%s", e->d_name);
        parse_report_name(e->d_name, r);
        r->size = (long)st.st_size;
        r->mtime = st.st_mtime;
        /* 预览截断到 280 字节,避免大文件拖慢归档页 */
        FILE *f = fopen(full, "rb");
        if (f) {
            size_t got = fread(r->preview, 1, sizeof(r->preview) - 1, f);
            r->preview[got] = '\0';
            fclose(f);
        } else {
            r->preview[0] = '\0';
        }
        n++;
    }
    closedir(d);
    /* 按修改时间倒序 */
    for (int i = 1; i < n; i++) {
        ReportEntry key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j].mtime < key.mtime) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
    *out = arr;
    *out_n = n;
    return 1;
}

static int h_reports_list(HttpRequest *req, HttpResponse *res) {
    (void)req;
    ReportEntry *arr;
    int n;
    report_entries(&arr, &n);
    sbuf b = {0};
    sb_str(&b, "{\"reports\":[");
    for (int i = 0; i < n; i++) {
        if (i) sb_chr(&b, ',');
        char mt[32];
        struct tm tmv;
        localtime_r(&arr[i].mtime, &tmv);
        strftime(mt, sizeof mt, "%Y-%m-%d %H:%M", &tmv);
        sb_str(&b, "{\"name\":");
        sb_json_str(&b, arr[i].name);
        sb_str(&b, ",\"model\":");
        sb_json_str(&b, arr[i].model);
        sb_str(&b, ",\"date\":");
        sb_json_str(&b, arr[i].date);
        sb_str(&b, ",\"size\":");
        {
            char num[32];
            snprintf(num, sizeof num, "%ld", arr[i].size);
            sb_str(&b, num);
        }
        sb_str(&b, ",\"mtime\":");
        sb_json_str(&b, mt);
        sb_str(&b, ",\"preview\":");
        sb_json_str(&b, arr[i].preview);
        sb_str(&b, "}");
    }
    sb_str(&b, "]}");
    free(arr);
    finish_json(res, 200, "OK", &b);
    return 0;
}

static int h_reports_get(HttpRequest *req, HttpResponse *res) {
    const char *base_prefix = ROUTE_REPORTS_PREFIX;
    size_t plen = strlen(base_prefix);
    const char *name = req->path + plen;
    if (name[0] == '\0' || strchr(name, '/') || strcmp(name, "..") == 0 ||
        strcmp(name, ".") == 0) {
        json_error(res, 400, "bad report name");
        return 0;
    }
    char full[4096];
    if (snprintf(full, sizeof full, "%s/%s", default_reports_dir(), name) >=
        (int)sizeof full) {
        json_error(res, 400, "bad report name");
        return 0;
    }
    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
        json_error(res, 404, "report not found");
        return 0;
    }
    if (st.st_size > (1024u << 20)) {
        json_error(res, 413, "report exceeds 1 MiB cap");
        return 0;
    }
    FILE *f = fopen(full, "rb");
    if (!f) {
        json_error(res, 500, "unreadable report");
        return 0;
    }
    size_t cap = st.st_size > 0 ? (size_t)st.st_size : 1;
    char *body = malloc(cap + 1);
    if (!body) {
        fclose(f);
        json_error(res, 500, "out of memory reading report");
        return 0;
    }
    size_t got = fread(body, 1, cap, f);
    fclose(f);
    body[got] = '\0';
    char mt[32];
    struct tm tmv;
    localtime_r(&st.st_mtime, &tmv);
    strftime(mt, sizeof mt, "%Y-%m-%d %H:%M", &tmv);
    sbuf b = {0};
    sb_str(&b, "{\"name\":");
    sb_json_str(&b, name);
    sb_str(&b, ",\"mtime\":");
    sb_json_str(&b, mt);
    sb_str(&b, ",\"size\":");
    {
        char num[32];
        snprintf(num, sizeof num, "%lld", (long long)st.st_size);
        sb_str(&b, num);
    }
    sb_str(&b, ",\"body\":");
    sb_json_str(&b, body);
    sb_str(&b, "}");
    free(body);
    finish_json(res, 200, "OK", &b);
    return 0;
}

/* ---------- /api/settings ---------- */

/* 扫描 .env 文本里两个白名单键的当前值(顺序无关;无文件/无键 -> 空)。 */
static void read_settings(const char *text, char *allow_paid, size_t ap_len,
                          char *provider, size_t prov_len) {
    allow_paid[0] = provider[0] = '\0';
    if (!text) return;
    const char *p = text;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t ln = nl ? (size_t)(nl - p) : strlen(p);
        if (ln > 0 && p[ln - 1] == '\r') ln--;
        if (strncmp(p, "PSE_ALLOW_PAID=", 15) == 0) {
            size_t vlen = ln - 15;
            if (vlen >= ap_len) vlen = ap_len - 1;
            memcpy(allow_paid, p + 15, vlen);
            allow_paid[vlen] = '\0';
        } else if (strncmp(p, "PSE_REVIEW_PROVIDER=", 20) == 0) {
            size_t vlen = ln - 20;
            if (vlen >= prov_len) vlen = prov_len - 1;
            memcpy(provider, p + 20, vlen);
            provider[vlen] = '\0';
        }
        p = nl ? nl + 1 : NULL;
    }
}

/* 原子重写 .env:替换两个白名单键的行,缺失则追加;其余行逐字保留。 */
static int write_settings_disk(const char *allow_paid, const char *provider,
                               char *err, size_t err_len) {
    const char *path = default_env_file();
    err[0] = '\0';
    sbuf src = {0};
    FILE *f = fopen(path, "rb");
    if (f) {
        char buf[16384];
        size_t got;
        while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
            if (src.len + got > (256u << 10)) {
                fclose(f);
                snprintf(err, err_len, ".env 超过 256KB,拒绝写回");
                return -1;
            }
            sb_mem(&src, buf, got);
        }
        fclose(f);
    }
    char old_allow[64] = {0}, old_prov[64] = {0};
    read_settings(src.p, old_allow, sizeof old_allow, old_prov, sizeof old_prov);
    sbuf out = {0};
    int wrote_allow = 0, wrote_prov = 0;
    const char *p = src.p;
    /* 逐行透传,命中目标键则替换 */
    if (p) {
        const char *cursor = p;
        while (cursor && *cursor) {
            const char *nl = strchr(cursor, '\n');
            size_t ln = nl ? (size_t)(nl - cursor) : strlen(cursor);
            if (strncmp(cursor, "PSE_ALLOW_PAID=", 15) == 0) {
                sb_str(&out, "PSE_ALLOW_PAID=");
                sb_str(&out, allow_paid);
                sb_chr(&out, '\n');
                wrote_allow = 1;
            } else if (strncmp(cursor, "PSE_REVIEW_PROVIDER=", 20) == 0) {
                sb_str(&out, "PSE_REVIEW_PROVIDER=");
                sb_str(&out, provider);
                sb_chr(&out, '\n');
                wrote_prov = 1;
            } else {
                sb_mem(&out, cursor, ln);
                sb_chr(&out, '\n');
            }
            cursor = nl ? nl + 1 : NULL;
        }
    }
    if (!wrote_allow) {
        sb_str(&out, "PSE_ALLOW_PAID=");
        sb_str(&out, allow_paid);
        sb_str(&out, "\n");
    }
    if (!wrote_prov) {
        sb_str(&out, "PSE_REVIEW_PROVIDER=");
        sb_str(&out, provider);
        sb_str(&out, "\n");
    }
    /* 写临时文件后 rename,避免半截文件被正在运行的 worker 读到 */
    char tmp[4600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *tf = fopen(tmp, "wb");
    if (!tf) {
        snprintf(err, err_len, "无法写 %s", tmp);
        return -1;
    }
    size_t written = out.p ? fwrite(out.p, 1, out.len, tf) : fwrite("\n", 1, 1, tf);
    int closed_ok = fclose(tf) == 0;
    if (!closed_ok || written != (out.p ? out.len : 1) || rename(tmp, path) != 0) {
        remove(tmp);
        snprintf(err, err_len, "写回 .env 失败");
        return -1;
    }
    return 0;
}

static int h_settings_get(HttpRequest *req, HttpResponse *res) {
    (void)req;
    sbuf src = {0};
    FILE *f = fopen(default_env_file(), "rb");
    if (f) {
        char buf[16384];
        size_t got;
        while ((got = fread(buf, 1, sizeof buf, f)) > 0 && src.len < (64u << 10))
            sb_mem(&src, buf, got);
        fclose(f);
    }
    char allow[64] = {0}, prov[64] = {0};
    read_settings(src.p, allow, sizeof allow, prov, sizeof prov);
    sbuf b = {0};
    sb_str(&b, "{");
    sb_str(&b, "\"allow_paid\":");
    {
        int on = strcmp(allow, "1") == 0;
        sb_str(&b, on ? "true" : "false");
    }
    sb_str(&b, ",\"provider\":");
    sb_json_str(&b, prov[0] ? prov : "router");
    sb_str(&b, ",\"env_file\":");
    sb_json_str(&b, default_env_file());
    /* 运行时进程环境(agent 实际走的通道)一并展示,便于对照;不吐 .env 原始
     * 内容——它含 AGNES/OPENAI 等密钥,前端设置页不需要。 */
    const char *lp = getenv("LLM_API_URL");
    const char *lm = getenv("LLM_MODEL");
    const char *rp = getenv("ROUTER_API_URL");
    sb_str(&b, ",\"runtime\":{\"LLM_API_URL\":");
    sb_json_str(&b, lp ? lp : "");
    sb_str(&b, ",\"LLM_MODEL\":");
    sb_json_str(&b, lm ? lm : "");
    sb_str(&b, ",\"ROUTER_API_URL\":");
    sb_json_str(&b, rp ? rp : "");
    sb_str(&b, "}}");
    finish_json(res, 200, "OK", &b);
    return 0;
}

static int h_settings_post(HttpRequest *req, HttpResponse *res) {
    if (!origin_ok(req)) {
        json_error(res, 403, "cross-origin write rejected");
        return 0;
    }
    const char *body = req->body ? req->body : "";
    char allow[64] = {0};
    const char *v = jfind_value(body, "allow_paid");
    if (v) {
        const char *cur = v;
        if (*cur == 't' || *cur == 'f') {
            int on = strncmp(cur, "true", 4) == 0;
            strcpy(allow, on ? "1" : "0");
        } else if (*cur == '\"') {
            /* 兼容 "1"/"0"/"on"/"off" 字符串 */
            char buf[64];
            const char *p = cur;
            if (jread_string(&p, buf, sizeof buf))
                strncpy(allow, buf, sizeof allow - 1);
        }
    }
    char prov[64] = {0};
    const char *pv = jfind_value(body, "provider");
    if (pv && *pv == '\"')
        jread_string(&pv, prov, sizeof prov);
    char err[256];
    int rc = write_settings_disk(allow, prov, err, sizeof err);
    if (rc != 0) {
        json_error(res, 500, err[0] ? err : "write failed");
        return 0;
    }
    sbuf b = {0};
    sb_str(&b, "{\"ok\":true,\"allow_paid\":");
    sb_str(&b, strcmp(allow, "1") == 0 ? "true" : "false");
    sb_str(&b, ",\"provider\":");
    sb_json_str(&b, prov[0] ? prov : "router");
    sb_str(&b, "}");
    finish_json(res, 200, "OK", &b);
    return 0;
}

void iquest_register(void) {
    agenthttpd_route("GET", ROUTE_REPORTS_GET, h_reports_list);
    agenthttpd_route("GET", ROUTE_REPORTS_PREFIX "*", h_reports_get);
    agenthttpd_route("GET", ROUTE_SETTINGS, h_settings_get);
    agenthttpd_route("POST", ROUTE_SETTINGS, h_settings_post);
}