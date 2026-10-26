/* Native math builtins for Lume — 数值内建片 (2026-09-28).
 *
 * 给 DSL 层补基础数学能力：abs/sqrt/exp/log/ln/pow/floor/ceil/round/
 * min/max；常量 e、pi 由 interp.c bridge_seed_builtins() 直接 env_set。
 * 此前语言层只有四则运算，算法演示(lib/algo.lume)无法计算 sigmoid /
 * 距离 / softmax；本文件补齐后用标准 math.h 实现，精度交给 C double。
 *
 * 约定（与其他 builtins_*.c 一致）：
 *   - native_* 为本片实现（static，仅 b_* 包装使用）；
 *   - b_* 包装注册进 interp.c 的注册表；
 *   - 参数错误 / 域错误一律 vm_set_error（不写 out，返回 null）。
 */

#include "builtins_internal.h"
#include <math.h>

/* 取一个数值参数；非数字 → error。 */
static bool arg_num(VM *vm, Value v, double *out) {
    if (!IS_NUM(v)) {
        vm_set_error(vm, "expected number argument");
        return false;
    }
    *out = AS_NUM(v);
    return true;
}

static void native_abs(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(fabs(x));
}

static void native_sqrt(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    if (x < 0.0) {
        vm_set_error(vm, "sqrt(): negative argument");
        return;
    }
    *out = val_num(sqrt(x));
}

static void native_exp(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(exp(x));
}

static void native_log(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    if (x <= 0.0) {
        vm_set_error(vm, "log(): argument must be positive");
        return;
    }
    *out = val_num(log(x));
}

static void native_pow(VM *vm, int argc, Value *args, Value *out) {
    double x, y;
    if (argc < 2 || !arg_num(vm, args[0], &x) || !arg_num(vm, args[1], &y)) return;
    double r = pow(x, y);
    if (!isfinite(r)) {
        vm_set_error(vm, "pow(): result not finite (domain error)");
        return;
    }
    *out = val_num(r);
}

static void native_floor(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(floor(x));
}

static void native_ceil(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(ceil(x));
}

static void native_round(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(round(x));
}

/* min/max 支持可变参数（≥1 个数值），逐个比较。 */
static void native_min(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) {
        vm_set_error(vm, "min() expects at least one number");
        return;
    }
    double best = 0.0;
    for (int i = 0; i < argc; i++) {
        double x;
        if (!arg_num(vm, args[i], &x)) return;
        if (i == 0 || x < best) best = x;
    }
    *out = val_num(best);
}

static void native_max(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) {
        vm_set_error(vm, "max() expects at least one number");
        return;
    }
    double best = 0.0;
    for (int i = 0; i < argc; i++) {
        double x;
        if (!arg_num(vm, args[i], &x)) return;
        if (i == 0 || x > best) best = x;
    }
    *out = val_num(best);
}

Value b_abs(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_abs); }
Value b_sqrt(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_sqrt); }
Value b_exp(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_exp); }
Value b_log(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_log); }
Value b_ln(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_log); }
Value b_pow(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_pow); }
Value b_floor(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_floor); }
Value b_ceil(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_ceil); }
Value b_round(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_round); }
Value b_min(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_min); }
Value b_max(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_max); }

/* ------------------------------------------------------------------ *
 * Statistical distribution builtins (P4, 2026-10-20).
 *
 * The numerical core is duplicated from rt.c (the native backends' twin,
 * which --compile links separately) exactly like abs/sqrt/... already are:
 * this file must link into the interpreter binary, which does not carry
 * rt.c. Keep the series / continued fractions byte-for-byte in step with
 * rt.c or the three backends drift; the wrappers here add argument
 * validation and turn the NaN domain signal into a vm_set_error.
 * ------------------------------------------------------------------ */

static double igamma_series(double a, double x)
{
    double lnf = -x + a * log(x) - lgamma(a);
    if (lnf < -745.0) return 0.0;
    double term = 1.0 / a, sum = term;
    for (int n = 1; n < 2000; n++) {
        term *= x / (a + n);
        sum += term;
        if (term < sum * 1e-15) break;
    }
    return exp(lnf) * sum;
}

static double igamma_cf(double a, double x)
{
    double lnf = -x + a * log(x) - lgamma(a);
    if (lnf < -745.0) return 0.0;
    double b = x + 1.0 - a;
    double c = 1.0e30, d = 1.0 / b, h = d;   /* c ~ 1/FPMIN (Lentz) */
    for (int i = 1; i < 2000; i++) {
        double an = -(double)i * (i - a);
        b += 2.0;
        d = an * d + b;
        if (fabs(d) < 1e-30) d = 1e-30;
        c = b + an / c;
        if (fabs(c) < 1e-30) c = 1e-30;
        d = 1.0 / d;
        double del = d * c;
        h *= del;
        if (fabs(del - 1.0) < 1e-15) break;
    }
    return exp(lnf) * h;
}

static double ibeta_cf(double a, double b, double x)
{
    double qab = a + b, qap = a + 1.0, qam = a - 1.0;
    double c = 1.0, d = 1.0 - qab * x / qap;
    if (fabs(d) < 1e-30) d = 1e-30;
    d = 1.0 / d;
    double h = d;
    for (int m = 1; m <= 2000; m++) {
        int m2 = 2 * m;
        double aa = (double)m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1.0 + aa * d;
        if (fabs(d) < 1e-30) d = 1e-30;
        c = 1.0 + aa / c;
        if (fabs(c) < 1e-30) c = 1e-30;
        d = 1.0 / d;
        h *= d * c;
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1.0 + aa * d;
        if (fabs(d) < 1e-30) d = 1e-30;
        c = 1.0 + aa / c;
        if (fabs(c) < 1e-30) c = 1e-30;
        d = 1.0 / d;
        double del = d * c;
        h *= del;
        if (fabs(del - 1.0) < 1e-15) break;
    }
    return h;
}

static double bi_erf(double x)     { return erf(x); }
static double bi_normcdf(double x) { return 0.5 * (1.0 + erf(x * 0.70710678118654752440)); }
static double bi_lgamma(double x)  { return lgamma(x); }
static double bi_gamma(double x)   { return exp(lgamma(x)); }

static double bi_igamma(double a, double x)
{
    if (a <= 0.0 || x < 0.0 || !isfinite(a)) return NAN;
    if (x == 0.0) return 0.0;
    if (!isfinite(x)) return 1.0;
    return x < a + 1.0 ? igamma_series(a, x) : 1.0 - igamma_cf(a, x);
}

static double bi_ibeta(double a, double b, double x)
{
    if (a <= 0.0 || b <= 0.0 || x < 0.0 || x > 1.0) return NAN;
    if (x == 0.0) return 0.0;
    if (x == 1.0) return 1.0;
    double lnbt = lgamma(a) + lgamma(b) - lgamma(a + b);
    double lfront = a * log(x) + b * log1p(-x) - lnbt;
    if (x < (a + 1.0) / (a + b + 2.0)) {
        if (lfront < -745.0) return 0.0;
        return exp(lfront) * ibeta_cf(a, b, x) / a;
    }
    double lr = b * log(1.0 - x) + a * log(x) - lnbt;
    if (lr < -745.0) return 1.0;
    return 1.0 - exp(lr) * ibeta_cf(b, a, 1.0 - x) / b;
}

static double bi_tdist_cdf(double t, double df)
{
    if (df <= 0.0 || !isfinite(df)) return NAN;
    if (!isfinite(t)) return t < 0.0 ? 0.0 : 1.0;
    double x = df / (df + t * t);
    double ib = bi_ibeta(df / 2.0, 0.5, x);
    return t >= 0.0 ? 1.0 - 0.5 * ib : 0.5 * ib;
}

static double bi_chisq_cdf(double x, double df)
{
    if (df <= 0.0 || !isfinite(df)) return NAN;
    if (x <= 0.0) return 0.0;
    if (!isfinite(x)) return 1.0;
    return bi_igamma(df / 2.0, x / 2.0);
}

static void native_erf(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(bi_erf(x));
}

static void native_normcdf(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(bi_normcdf(x));
}

static void native_gamma(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    if (x <= 0.0) {
        vm_set_error(vm, "gamma(): argument must be positive");
        return;
    }
    *out = val_num(bi_gamma(x));
}

static void native_lgamma(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    if (x <= 0.0) {
        vm_set_error(vm, "lgamma(): argument must be positive");
        return;
    }
    *out = val_num(bi_lgamma(x));
}

static void native_igamma(VM *vm, int argc, Value *args, Value *out) {
    double a, x;
    if (argc < 2) { vm_set_error(vm, "igamma() expects two numbers"); return; }
    if (!arg_num(vm, args[0], &a) || !arg_num(vm, args[1], &x)) return;
    double r = bi_igamma(a, x);
    if (isnan(r)) { vm_set_error(vm, "igamma(): bad arguments (a > 0, x >= 0)"); return; }
    *out = val_num(r);
}

static void native_ibeta(VM *vm, int argc, Value *args, Value *out) {
    double a, b, x;
    if (argc < 3) { vm_set_error(vm, "ibeta() expects three numbers"); return; }
    if (!arg_num(vm, args[0], &a) || !arg_num(vm, args[1], &b) ||
        !arg_num(vm, args[2], &x)) return;
    double r = bi_ibeta(a, b, x);
    if (isnan(r)) {
        vm_set_error(vm, "ibeta(): bad arguments (a > 0, b > 0, 0 <= x <= 1)");
        return;
    }
    *out = val_num(r);
}

static void native_tdist_cdf(VM *vm, int argc, Value *args, Value *out) {
    double t, df;
    if (argc < 2) { vm_set_error(vm, "tdist_cdf() expects two numbers"); return; }
    if (!arg_num(vm, args[0], &t) || !arg_num(vm, args[1], &df)) return;
    double r = bi_tdist_cdf(t, df);
    if (isnan(r)) { vm_set_error(vm, "tdist_cdf(): bad arguments (df > 0)"); return; }
    *out = val_num(r);
}

static void native_chisq_cdf(VM *vm, int argc, Value *args, Value *out) {
    double x, df;
    if (argc < 2) { vm_set_error(vm, "chisq_cdf() expects two numbers"); return; }
    if (!arg_num(vm, args[0], &x) || !arg_num(vm, args[1], &df)) return;
    double r = bi_chisq_cdf(x, df);
    if (isnan(r)) { vm_set_error(vm, "chisq_cdf(): bad arguments (df > 0)"); return; }
    *out = val_num(r);
}

Value b_normcdf(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_normcdf); }
Value b_erf(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_erf); }
Value b_gamma(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_gamma); }
Value b_lgamma(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_lgamma); }
Value b_igamma(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_igamma); }
Value b_ibeta(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_ibeta); }
Value b_tdist_cdf(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_tdist_cdf); }
Value b_chisq_cdf(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_chisq_cdf); }

/* ============================================================================
 * Technical-indicator sequence builtins (#54, 2026-10-25) — host-only port.
 *
 * The host tree is the interpreter-only leg: there is no rt.c / native code
 * generator here, so only the interpreter native_* need to exist. The six
 * routines below mirror core's builtins_math_quant.inc byte-for-byte in
 * their window / leading-NAN / 1/3-EWM seeding so a script that runs under
 * the host `bin/lume` and under core's three backends prints the same
 * numbers. The list helpers are self-contained statics (each translation
 * unit keeps its own copy, exactly like core's .inc fragments).
 *
 *   rolling_max(list, window) / rolling_min(list, window) -> float list
 *   kdj(high, low, close, period) -> {rsv, k, d, j} float lists
 *   williams_r(high, low, close, period) -> float list
 *   atr(high, low, close, period)      -> float list
 *   obv(close, volume)                 -> float list
 * ==========================================================================*/

static bool m54_arg_list(VM *vm, Value v, ObjList **out) {
    if (!IS_OBJ(v) || AS_OBJ(v)->type != OBJ_LIST) {
        vm_set_error(vm, "expected list argument");
        return false;
    }
    *out = &AS_OBJ(v)->as.list;
    return true;
}

static bool m54_list_num(VM *vm, ObjList *L, int i, double *out) {
    Value v = L->items[i];
    if (IS_INT(v))   { *out = (double)AS_INT(v); return true; }
    if (IS_FLOAT(v)) { *out = v.as.n;           return true; }
    vm_set_error(vm, "quant builtin needs a list of numbers");
    return false;
}

static double *m54_list_to_doubles(VM *vm, ObjList *L, int *n) {
    *n = L->count;
    double *xs = (double *)malloc((size_t)(*n ? *n : 1) * sizeof(double));
    if (!xs) { vm_set_error(vm, "out of memory"); return NULL; }
    for (int i = 0; i < *n; i++)
        if (!m54_list_num(vm, L, i, &xs[i])) { free(xs); return NULL; }
    return xs;
}

static void native_rolling_max(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double wd;
    if (argc < 2 || !m54_arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &wd)) return;
    int64_t w = (int64_t)wd;
    if (w < 1) { vm_set_error(vm, "rolling_max() needs a positive window"); return; }
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (xs) {
        for (int i = 0; i < n; i++) {
            if (i + 1 < w) { list_push(vm, res, val_num(NAN)); continue; }
            double m = xs[i + 1 - w];
            for (int64_t j = i + 2 - w; j <= i; j++) if (xs[j] > m) m = xs[j];
            list_push(vm, res, val_num(m));
        }
        free(xs);
    }
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_rolling_min(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double wd;
    if (argc < 2 || !m54_arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &wd)) return;
    int64_t w = (int64_t)wd;
    if (w < 1) { vm_set_error(vm, "rolling_min() needs a positive window"); return; }
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (xs) {
        for (int i = 0; i < n; i++) {
            if (i + 1 < w) { list_push(vm, res, val_num(NAN)); continue; }
            double m = xs[i + 1 - w];
            for (int64_t j = i + 2 - w; j <= i; j++) if (xs[j] < m) m = xs[j];
            list_push(vm, res, val_num(m));
        }
        free(xs);
    }
    vm_pop(vm);
    *out = val_obj(res);
}

/* build the four KDJ series as a map {rsv,k,d,j} of full-length float lists */
static Obj *m54_kdj_lists(VM *vm, double *hx, double *lx, double *cx, int n,
                          int64_t w, double **rsvp, double **kp, double **dp, double **jp) {
    *rsvp = malloc((size_t)n * sizeof(double));
    *kp = malloc((size_t)n * sizeof(double));
    *dp = malloc((size_t)n * sizeof(double));
    *jp = malloc((size_t)n * sizeof(double));
    if (!*rsvp || !*kp || !*dp || !*jp) {
        free(*rsvp); free(*kp); free(*dp); free(*jp);
        vm_set_error(vm, "out of memory");
        return NULL;
    }
    double alpha = 1.0 / 3.0;
    for (int i = 0; i < n; i++) {
        if (i + 1 < w) {
            (*rsvp)[i] = (*kp)[i] = (*dp)[i] = (*jp)[i] = NAN;
            continue;
        }
        int64_t s = i + 1 - w;
        double hi = hx[s], lo = lx[s];
        for (int64_t j = s + 1; j <= i; j++) {
            if (hx[j] > hi) hi = hx[j];
            if (lx[j] < lo) lo = lx[j];
        }
        double den = hi - lo;
        (*rsvp)[i] = (den == 0.0) ? 50.0 : (cx[i] - lo) / den * 100.0;
    }
    int64_t seed = (w - 1 < n) ? (w - 1) : -1;
    double ek = 0.0, ed = 0.0;
    for (int i = 0; i < n; i++) {
        if (i < seed || (*rsvp)[i] != (*rsvp)[i]) {
            (*kp)[i] = (*dp)[i] = NAN;
            continue;
        }
        ek = (i == seed) ? (*rsvp)[i] : alpha * (*rsvp)[i] + (1.0 - alpha) * ek;
        ed = (i == seed) ? ek : alpha * ek + (1.0 - alpha) * ed;
        (*kp)[i] = ek;
        (*dp)[i] = ed;
        (*jp)[i] = 3.0 * ek - 2.0 * ed;
    }
    Obj *maps[4];
    int ni;
    for (ni = 0; ni < 4; ni++) {
        double *col = (ni == 0 ? *rsvp : ni == 1 ? *kp : ni == 2 ? *dp : *jp);
        Obj *lst = AS_OBJ(make_list(vm));
        vm_push(vm, val_obj(lst));
        for (int i = 0; i < n; i++) list_push(vm, lst, val_num(col[i]));
        vm_pop(vm);
        maps[ni] = lst;
    }
    free(*rsvp); free(*kp); free(*dp); free(*jp);
    Obj *map = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj(map));
    map_set(vm, map, "rsv", val_obj(maps[0]));
    map_set(vm, map, "k", val_obj(maps[1]));
    map_set(vm, map, "d", val_obj(maps[2]));
    map_set(vm, map, "j", val_obj(maps[3]));
    vm_pop(vm);
    return map;
}

static void native_kdj(VM *vm, int argc, Value *args, Value *out) {
    ObjList *H, *Lo, *C; double pd;
    if (argc < 4 || !m54_arg_list(vm, args[0], &H) || !m54_arg_list(vm, args[1], &Lo) ||
        !m54_arg_list(vm, args[2], &C) || !arg_num(vm, args[3], &pd)) return;
    int64_t w = (int64_t)pd;
    int64_t nh = H->count, nl = Lo->count, nc = C->count;
    if (nh != nl || nh != nc || nh < 2 || w < 2) {
        vm_set_error(vm, "kdj() needs (high, low, close, period) with equal lists and period >= 2");
        return;
    }
    int n; double *hx = m54_list_to_doubles(vm, H, &n);
    double *lx = hx ? m54_list_to_doubles(vm, Lo, &n) : NULL;
    double *cx = lx ? m54_list_to_doubles(vm, C, &n) : NULL;
    if (!cx) { free(hx); free(lx); return; }
    double *rsvp, *kp, *dp, *jp;
    Obj *map = m54_kdj_lists(vm, hx, lx, cx, n, w, &rsvp, &kp, &dp, &jp);
    free(hx); free(lx); free(cx);
    if (!map) return;
    *out = val_obj(map);
}

static void native_williams_r(VM *vm, int argc, Value *args, Value *out) {
    ObjList *H, *Lo, *C; double pd;
    if (argc < 4 || !m54_arg_list(vm, args[0], &H) || !m54_arg_list(vm, args[1], &Lo) ||
        !m54_arg_list(vm, args[2], &C) || !arg_num(vm, args[3], &pd)) return;
    int64_t w = (int64_t)pd;
    int64_t nh = H->count, nl = Lo->count, nc = C->count;
    if (nh != nl || nh != nc || nh < 2 || w < 2) {
        vm_set_error(vm, "williams_r() needs (high, low, close, period) with equal lists and period >= 2");
        return;
    }
    int n; double *hx = m54_list_to_doubles(vm, H, &n);
    double *lx = hx ? m54_list_to_doubles(vm, Lo, &n) : NULL;
    double *cx = lx ? m54_list_to_doubles(vm, C, &n) : NULL;
    if (!cx) { free(hx); free(lx); return; }
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    for (int i = 0; i < n; i++) {
        if (i + 1 < w) { list_push(vm, res, val_num(NAN)); continue; }
        int64_t s = i + 1 - w;
        double hi = hx[s], lo = lx[s];
        for (int64_t j = s + 1; j <= i; j++) {
            if (hx[j] > hi) hi = hx[j];
            if (lx[j] < lo) lo = lx[j];
        }
        double den = hi - lo;
        double v = (den == 0.0) ? 0.0 : (hi - cx[i]) / den * -100.0;
        list_push(vm, res, val_num(v));
    }
    free(hx); free(lx); free(cx);
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_atr(VM *vm, int argc, Value *args, Value *out) {
    ObjList *H, *Lo, *C; double pd;
    if (argc < 4 || !m54_arg_list(vm, args[0], &H) || !m54_arg_list(vm, args[1], &Lo) ||
        !m54_arg_list(vm, args[2], &C) || !arg_num(vm, args[3], &pd)) return;
    int64_t w = (int64_t)pd;
    int64_t nh = H->count, nl = Lo->count, nc = C->count;
    if (nh != nl || nh != nc || nh < 2 || w < 1) {
        vm_set_error(vm, "atr() needs (high, low, close, period) with equal lists and period >= 1");
        return;
    }
    int n; double *hx = m54_list_to_doubles(vm, H, &n);
    double *lx = hx ? m54_list_to_doubles(vm, Lo, &n) : NULL;
    double *cx = lx ? m54_list_to_doubles(vm, C, &n) : NULL;
    if (!cx) { free(hx); free(lx); return; }
    double *tr = malloc((size_t)n * sizeof(double));
    if (!tr) { free(hx); free(lx); free(cx); vm_set_error(vm, "out of memory"); return; }
    for (int i = 0; i < n; i++) {
        double h = hx[i] - lx[i];
        if (i > 0) {
            double t2 = hx[i] - cx[i - 1]; if (t2 < 0) t2 = -t2;
            double t3 = lx[i] - cx[i - 1]; if (t3 < 0) t3 = -t3;
            double t = t2 > t3 ? t2 : t3;
            tr[i] = t > h ? t : h;
        } else {
            tr[i] = h;
        }
    }
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    for (int i = 0; i < n; i++) {
        if (i + 1 < w) { list_push(vm, res, val_num(NAN)); continue; }
        double s = 0.0;
        for (int64_t j = i + 1 - w; j <= i; j++) s += tr[j];
        list_push(vm, res, val_num(s / (double)w));
    }
    free(tr); free(hx); free(lx); free(cx);
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_obv(VM *vm, int argc, Value *args, Value *out) {
    ObjList *Cl, *Vl;
    if (argc < 2 || !m54_arg_list(vm, args[0], &Cl) || !m54_arg_list(vm, args[1], &Vl)) return;
    if (Cl->count != Vl->count || Cl->count < 2) {
        vm_set_error(vm, "obv() needs two equal-length lists (n >= 2)");
        return;
    }
    int n; double *cx = m54_list_to_doubles(vm, Cl, &n);
    double *vx = cx ? m54_list_to_doubles(vm, Vl, &n) : NULL;
    if (!vx) { free(cx); return; }
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    double obv = 0.0;
    list_push(vm, res, val_num(obv));
    for (int i = 1; i < n; i++) {
        double d = cx[i] - cx[i - 1];
        if (d > 0.0) obv += vx[i];
        else if (d < 0.0) obv -= vx[i];
        list_push(vm, res, val_num(obv));
    }
    free(cx); free(vx);
    vm_pop(vm);
    *out = val_obj(res);
}

Value b_rolling_max(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_rolling_max); }
Value b_rolling_min(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_rolling_min); }
Value b_kdj(VM *vm, int argc, Value *args)          { return vm_native(vm, argc, args, native_kdj); }
Value b_williams_r(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_williams_r); }
Value b_atr(VM *vm, int argc, Value *args)          { return vm_native(vm, argc, args, native_atr); }
Value b_obv(VM *vm, int argc, Value *args)          { return vm_native(vm, argc, args, native_obv); }

/* ============================================================================
 * Quant indicators & risk builtins (#55, 2026-10-26) — host-only port.
 *
 * Second batch of the core's builtins_math_quant.inc surface, ported so the
 * host interpreter can serve scanner performance/risk analytics from a single
 * process. Formulas mirror core line-for-line (Welford means, the same
 * BI_ANNUAL_SCALE = sqrt(252) constant, Wilder RSI, 1/3-seeded KDJ-free EMA
 * recurrence, interpolated quantiles) so core's three backends and the host
 * keep printing identical numbers. Reuses the m54_* list helpers from the
 * #54 section above.
 *
 *   rolling_mean / rolling_stddev / ewma / pct_change / max_drawdown
 *   rsi (Wilder smoothing)
 *   macd -> {macd, signal, hist} ; bollinger -> {mid, upper, lower}
 *   sharpe_ratio / sortino_ratio / sharpe_annualized (returns list + rf)
 *   annualized_return / annualized_volatility / cumulative_return (prices/returns)
 *   semivariance / downside_deviation
 *   value_at_risk / expected_shortfall / param_var (quantile-based VaR)
 *   calmar_ratio (mean daily pct / max drawdown, both in percent units)
 * ==========================================================================*/

#define M55_ANNUAL_SCALE 15.874507866387544  /* sqrt(252), core's BI_ANNUAL_SCALE_I */

/* inverse standard-normal CDF — clone of core's bi_norminv (Acklam 2011),
 * identical constants/branches so param_var stays byte-identical to core. */
static double m55_norminv(double p)
{
    static const double a[6] = { -3.969683028665376e+01, 2.209460984245205e+02,
        -2.759285104469687e+02, 1.383577518672690e+02, -3.066479806614716e+01,
        2.506628277459239e+00 };
    static const double b[5] = { -5.447609879822406e+01, 1.615858368580409e+02,
        -1.556989798598866e+02, 6.680131188771972e+01, -1.328068155288572e+01 };
    static const double c[6] = { -7.784894002430293e-03, -3.223964580411365e-01,
        -2.400758277161838e+00, -2.549732539343734e+00, 4.374664141464968e+00,
        2.938163982698783e+00 };
    static const double d[4] = { 7.784695709041462e-03, 3.224671290700398e-01,
        2.445134137142996e+00, 3.754408661907416e+00 };
    static const double plow = 0.02425, phigh = 0.97575;
    double q, r;
    if (p <= 0.0 || p >= 1.0) return NAN;
    if (p < plow) {
        q = sqrt(-2.0 * log(p));
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    if (p > phigh) {
        q = sqrt(-2.0 * log(1.0 - p));
        return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
                ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    q = p - 0.5;
    r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
}

/* EMA with alpha = 2/(period+1), seeded at row 0 (the macd/bollinger helper,
 * core's interp_ema twin). */
static Obj *m55_ema(VM *vm, ObjList *L, int64_t period) {
    if (L->count < 1 || period < 1) return NULL;
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (!xs) return NULL;
    double a = 2.0 / (double)(period + 1);
    Obj *out = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(out));
    double e = xs[0];
    list_push(vm, out, val_num(e));
    for (int i = 1; i < n; i++) {
        e = a * xs[i] + (1.0 - a) * e;
        list_push(vm, out, val_num(e));
    }
    free(xs);
    vm_pop(vm);
    return out;
}

static int m55_cmp_d(const void *pa, const void *pb) {
    double x = *(const double *)pa, y = *(const double *)pb;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* interpolated percentile over a sorted array (core's boot_pctl_sorted). */
static double m55_pctl_sorted(const double *v, int64_t m, double p100) {
    double r = p100 / 100.0 * (double)(m - 1);
    int64_t lo = (int64_t)r;
    double frac = r - (double)lo;
    return lo + 1 < m ? v[lo] + frac * (v[lo + 1] - v[lo]) : v[lo];
}

static void native_rolling_mean(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double wd;
    if (argc < 2 || !m54_arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &wd)) return;
    int64_t w = (int64_t)wd;
    if (w < 1) { vm_set_error(vm, "rolling_mean() needs a positive window"); return; }
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (xs) {
        double s = 0.0;
        for (int i = 0; i < n; i++) {
            s += xs[i];
            if (i >= w) s -= xs[i - w];
            list_push(vm, res, val_num(i + 1 >= w ? s / (double)w : NAN));
        }
        free(xs);
    }
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_rolling_stddev(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double wd;
    if (argc < 2 || !m54_arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &wd)) return;
    int64_t w = (int64_t)wd;
    if (w < 1) { vm_set_error(vm, "rolling_stddev() needs a positive window"); return; }
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (xs) {
        for (int i = 0; i < n; i++) {
            if (i + 1 < w || w < 2) { list_push(vm, res, val_num(NAN)); continue; }
            int64_t lo = (int64_t)i + 1 - w;
            double m = 0.0;
            for (int64_t j = lo; j <= i; j++) m += xs[j];
            m /= (double)w;
            double s = 0.0;
            for (int64_t j = lo; j <= i; j++) { double d = xs[j] - m; s += d * d; }
            list_push(vm, res, val_num(sqrt(s / (double)(w - 1))));
        }
        free(xs);
    }
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_ewma(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double alpha;
    if (argc < 2 || !m54_arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &alpha)) return;
    if (alpha <= 0.0 || alpha > 1.0) {
        vm_set_error(vm, "ewma() needs alpha in (0,1]");
        return;
    }
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (xs) {
        if (n > 0) {
            double e = xs[0];
            list_push(vm, res, val_num(e));
            for (int i = 1; i < n; i++) {
                e = alpha * xs[i] + (1.0 - alpha) * e;
                list_push(vm, res, val_num(e));
            }
        }
        free(xs);
    }
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_pct_change(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !m54_arg_list(vm, args[0], &L)) return;
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    if (L->count >= 2) {
        int n; double *xs = m54_list_to_doubles(vm, L, &n);
        if (xs) {
            for (int i = 1; i < n; i++) {
                double prev = xs[i - 1];
                double pc = prev == 0.0 ? NAN : 100.0 * (xs[i] - prev) / prev;
                list_push(vm, res, val_num(pc));
            }
            free(xs);
        }
    }
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_max_drawdown(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !m54_arg_list(vm, args[0], &L)) return;
    if (L->count < 2) { vm_set_error(vm, "max_drawdown() needs >= 2 elements"); return; }
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (!xs) return;
    double peak = xs[0], mdd = 0.0;
    for (int i = 1; i < n; i++) {
        if (xs[i] > peak) peak = xs[i];
        double dd = peak > 0.0 ? 100.0 * (peak - xs[i]) / peak : 0.0;
        if (dd > mdd) mdd = dd;
    }
    free(xs);
    *out = val_num(mdd);
}

static void native_rsi(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double wnd;
    if (argc < 2 || !m54_arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &wnd)) return;
    int64_t window = (int64_t)wnd;
    if (L->count < 2 || window < 1 || window > (int64_t)L->count - 1) {
        vm_set_error(vm, "rsi() needs (prices, window in [1, len-1])");
        return;
    }
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (!xs) return;
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    double avg_gain = 0.0, avg_loss = 0.0;
    for (int64_t i = 0; i < n; i++) {
        if (i < window) { list_push(vm, res, val_num(NAN)); continue; }
        double ch = xs[i] - xs[i - 1];
        double gain = ch > 0.0 ? ch : 0.0;
        double loss = ch < 0.0 ? -ch : 0.0;
        if (i == window) {
            double sg = 0.0, sl = 0.0;
            for (int64_t j = 1; j <= window; j++) {
                double d = xs[j] - xs[j - 1];
                if (d > 0.0) sg += d; else sl += -d;
            }
            avg_gain = sg / (double)window;
            avg_loss = sl / (double)window;
        } else {
            avg_gain = (avg_gain * (window - 1) + gain) / (double)window;
            avg_loss = (avg_loss * (window - 1) + loss) / (double)window;
        }
        if (!(avg_loss > 0.0)) {
            list_push(vm, res, val_num(avg_gain > 0.0 ? 100.0 : 50.0));
        } else {
            list_push(vm, res, val_num(100.0 - 100.0 / (1.0 + avg_gain / avg_loss)));
        }
    }
    free(xs);
    vm_pop(vm);
    *out = val_obj(res);
}

/* Cutler RSI (etl-consistent): gain/loss 是最近 w 个价变的 rolling(w) 均值,
 * 窗口是「本地」的 → 尾值只依赖最后 w+1 根收盘价, 无 EMA 种子 warm-up, 可
 * 精确复现 stock-analyzer 的 pandas _calculate_rsi(逐字节一致)。边界复刻
 * pandas loss.replace(0, inf): loss 均值==0 → rs=gm/inf=0 → RSI=0(不是 100)。
 * 返回全长度 list([NaN]*w + [w+1] 个值), 与 rsi() 同形, 供 tail_k 取尾值。 */
static void native_rsi_cutler(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double wnd;
    if (argc < 2 || !m54_arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &wnd)) return;
    int64_t window = (int64_t)wnd;
    int64_t count = L->count;
    if (count < 2 || window < 1 || window > count - 1) {
        vm_set_error(vm, "rsi_cutler() needs (prices, window in [1, len-1])");
        return;
    }
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (!xs) return;
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    for (int64_t i = 0; i < n; i++) {
        if (i < window) { list_push(vm, res, val_num(NAN)); continue; }
        double sg = 0.0, sl = 0.0;
        for (int64_t j = i - window + 1; j <= i; j++) {
            double d = xs[j] - xs[j - 1];
            if (d > 0.0) sg += d; else if (d < 0.0) sl += -d;
        }
        double gm = sg / (double)window;
        double lm = sl / (double)window;
        double rsi;
        if (lm > 0.0) {
            double rs = gm / lm;
            rsi = 100.0 - 100.0 / (1.0 + rs);
        } else {
            rsi = 0.0;   /* pandas loss.replace(0, inf) → rs=0 → RSI=0 */
        }
        list_push(vm, res, val_num(rsi));
    }
    free(xs);
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_macd(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double fd, sd, gd;
    if (argc < 4 || !m54_arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &fd) ||
        !arg_num(vm, args[2], &sd) || !arg_num(vm, args[3], &gd)) return;
    int64_t fast = (int64_t)fd, slow = (int64_t)sd, signal = (int64_t)gd;
    if (L->count < 1 || fast < 1 || slow < 1 || signal < 1) {
        vm_set_error(vm, "macd() needs (prices, fast, slow, signal) with periods >= 1");
        return;
    }
    Obj *ef = m55_ema(vm, L, fast);
    Obj *es = m55_ema(vm, L, slow);
    if (!ef || !es) return;
    int n; double *fx = m54_list_to_doubles(vm, &ef->as.list, &n);
    double *sx = fx ? m54_list_to_doubles(vm, &es->as.list, &n) : NULL;
    if (!sx) { free(fx); return; }
    Obj *macd = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(macd));
    for (int i = 0; i < n; i++) list_push(vm, macd, val_num(fx[i] - sx[i]));
    free(fx); free(sx);
    vm_pop(vm);
    Obj *sig = m55_ema(vm, &macd->as.list, signal);
    if (!sig) return;
    int m; double *mx = m54_list_to_doubles(vm, &macd->as.list, &m);
    double *gx = mx ? m54_list_to_doubles(vm, &sig->as.list, &m) : NULL;
    if (!gx) { free(mx); return; }
    Obj *hist = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(hist));
    for (int i = 0; i < m; i++) list_push(vm, hist, val_num(mx[i] - gx[i]));
    free(mx); free(gx);
    vm_pop(vm);
    Obj *map = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj(map));
    map_set(vm, map, "macd", val_obj(macd));
    map_set(vm, map, "signal", val_obj(sig));
    map_set(vm, map, "hist", val_obj(hist));
    vm_pop(vm);
    *out = val_obj(map);
}

static void native_bollinger(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double wnd, k;
    if (argc < 3 || !m54_arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &wnd) ||
        !arg_num(vm, args[2], &k)) return;
    int64_t window = (int64_t)wnd;
    if (L->count < 2 || window < 2 || window > (int64_t)L->count || !isfinite(k)) {
        vm_set_error(vm, "bollinger() needs (prices, window in [2, len], k)");
        return;
    }
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (!xs) return;
    Obj *mid = AS_OBJ(make_list(vm));
    Obj *up = AS_OBJ(make_list(vm));
    Obj *lo = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(mid)); vm_push(vm, val_obj(up)); vm_push(vm, val_obj(lo));
    for (int i = 0; i < n; i++) {
        if (i + 1 < window) {
            list_push(vm, mid, val_num(NAN));
            list_push(vm, up, val_num(NAN));
            list_push(vm, lo, val_num(NAN));
            continue;
        }
        int64_t s = (int64_t)i + 1 - window;
        double m = 0.0, m2 = 0.0;
        int64_t kk = 0;
        for (int64_t j = s; j <= i; j++, kk++) {
            double d = xs[j] - m;
            m += d / (double)(kk + 1);
            m2 += d * (xs[j] - m);
        }
        double sd = sqrt(m2 / (double)(window - 1));
        list_push(vm, mid, val_num(m));
        list_push(vm, up, val_num(m + k * sd));
        list_push(vm, lo, val_num(m - k * sd));
    }
    free(xs);
    vm_pop(vm); vm_pop(vm); vm_pop(vm);
    Obj *map = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj(map));
    map_set(vm, map, "mid", val_obj(mid));
    map_set(vm, map, "upper", val_obj(up));
    map_set(vm, map, "lower", val_obj(lo));
    vm_pop(vm);
    *out = val_obj(map);
}

/* ---- Performance / risk (#48/#53/#57 contracts, mirrored from core) ---- */

static void native_sharpe_ratio(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double rf;
    if (argc < 2 || !m54_arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &rf)) return;
    if (!isfinite(rf)) { vm_set_error(vm, "sharpe_ratio() needs a finite risk-free rate"); return; }
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (!xs) return;
    if (n < 2) { free(xs); vm_set_error(vm, "sharpe_ratio() needs at least two observations"); return; }
    double m = 0.0, m2 = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - m;
        m += d / (double)(i + 1);
        m2 += d * (xs[i] - m);
    }
    free(xs);
    double sd = sqrt(m2 / (double)(n - 1));
    if (!(sd > 0.0) || !isfinite(m)) { *out = val_num(NAN); return; }
    *out = val_num((m - rf) / sd);
}

static void native_sortino_ratio(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double rf;
    if (argc < 2 || !m54_arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &rf)) return;
    if (!isfinite(rf)) { vm_set_error(vm, "sortino_ratio() needs a finite risk-free rate"); return; }
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (!xs) return;
    if (n < 2) { free(xs); vm_set_error(vm, "sortino_ratio() needs at least two observations"); return; }
    double m = 0.0, dd = 0.0;
    for (int i = 0; i < n; i++) {
        m += xs[i];
        double d = xs[i] - rf;
        if (d < 0.0) dd += d * d;
    }
    free(xs);
    m /= (double)n;
    double ds = sqrt(dd / (double)n);
    if (!(ds > 0.0) || !isfinite(m)) { *out = val_num(NAN); return; }
    *out = val_num((m - rf) / ds);
}

static void native_annualized_return(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !m54_arg_list(vm, args[0], &L)) return;
    if (L->count < 2) { vm_set_error(vm, "annualized_return() needs >= 2 prices"); return; }
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (!xs) return;
    for (int i = 0; i < n; i++) {
        if (!(xs[i] > 0.0)) { free(xs); *out = val_num(NAN); return; }
    }
    double r = pow(xs[n - 1] / xs[0], 1.0 / (double)(n - 1)) - 1.0;
    free(xs);
    *out = val_num(r);
}

static void native_annualized_volatility(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !m54_arg_list(vm, args[0], &L)) return;
    if (L->count < 2) { vm_set_error(vm, "annualized_volatility() needs >= 2 returns"); return; }
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (!xs) return;
    double m = 0.0, m2 = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - m;
        m += d / (double)(i + 1);
        m2 += d * (xs[i] - m);
    }
    free(xs);
    *out = val_num(sqrt(m2 / (double)(n - 1)) * M55_ANNUAL_SCALE);
}

static void native_sharpe_annualized(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double rf;
    if (argc < 2 || !m54_arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &rf)) return;
    if (L->count < 2) { vm_set_error(vm, "sharpe_annualized() needs >= 2 returns"); return; }
    if (!isfinite(rf)) { vm_set_error(vm, "sharpe_annualized(): rf must be finite"); return; }
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (!xs) return;
    double m = 0.0, m2 = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - m;
        m += d / (double)(i + 1);
        m2 += d * (xs[i] - m);
    }
    free(xs);
    double sd = sqrt(m2 / (double)(n - 1));
    if (!(sd > 0.0) || !isfinite(m)) { *out = val_num(NAN); return; }
    *out = val_num(((m - rf) / sd) * M55_ANNUAL_SCALE);
}

static void native_cumulative_return(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !m54_arg_list(vm, args[0], &L)) return;
    if (L->count < 2) { vm_set_error(vm, "cumulative_return() needs >= 2 prices"); return; }
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (!xs) return;
    if (!(xs[0] > 0.0) || !(xs[n - 1] > 0.0)) { free(xs); *out = val_num(NAN); return; }
    double r = xs[n - 1] / xs[0] - 1.0;
    free(xs);
    *out = val_num(r);
}

static void native_semivariance(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !m54_arg_list(vm, args[0], &L)) return;
    if (L->count < 2) { vm_set_error(vm, "semivariance() needs >= 2 returns"); return; }
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (!xs) return;
    double m = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - m;
        m += d / (double)(i + 1);
    }
    double sv = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - m;
        if (d < 0.0) sv += d * d;
    }
    free(xs);
    *out = val_num(sv / (double)n);
}

static void native_downside_deviation(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double t;
    if (argc < 2 || !m54_arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &t)) return;
    if (L->count < 2) { vm_set_error(vm, "downside_deviation() needs >= 2 returns"); return; }
    if (!isfinite(t)) { vm_set_error(vm, "downside_deviation(): threshold must be finite"); return; }
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (!xs) return;
    double dd = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - t;
        if (d < 0.0) dd += d * d;
    }
    free(xs);
    *out = val_num(sqrt(dd / (double)n));
}

static void native_value_at_risk(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double level;
    if (argc < 2 || !m54_arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &level)) return;
    if (!isfinite(level) || level <= 0.0 || level >= 1.0) {
        vm_set_error(vm, "value_at_risk() needs level in (0,1)"); return;
    }
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (!xs) return;
    if (n < 1) { free(xs); vm_set_error(vm, "value_at_risk() needs a non-empty list"); return; }
    qsort(xs, (size_t)n, sizeof(double), m55_cmp_d);
    double q = m55_pctl_sorted(xs, n, level * 100.0);
    free(xs);
    *out = val_num(-q);
}

static void native_expected_shortfall(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double level;
    if (argc < 2 || !m54_arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &level)) return;
    if (!isfinite(level) || level <= 0.0 || level >= 1.0) {
        vm_set_error(vm, "expected_shortfall() needs level in (0,1)"); return;
    }
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (!xs) return;
    if (n < 1) { free(xs); vm_set_error(vm, "expected_shortfall() needs a non-empty list"); return; }
    qsort(xs, (size_t)n, sizeof(double), m55_cmp_d);
    double q = m55_pctl_sorted(xs, n, level * 100.0);
    double s = 0.0; int64_t k = 0;
    while (k < n && xs[k] <= q) { s += xs[k]; k++; }
    free(xs);
    if (k < 1) { vm_set_error(vm, "expected_shortfall() has no tail observations"); return; }
    *out = val_num(-(s / (double)k));
}

static void native_param_var(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double level;
    if (argc < 2 || !m54_arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &level)) return;
    if (L->count < 2) { vm_set_error(vm, "param_var() needs >= 2 returns"); return; }
    if (!(level > 0.0) || !(level < 1.0)) {
        vm_set_error(vm, "param_var(): level must be in (0, 1)"); return;
    }
    int n; double *xs = m54_list_to_doubles(vm, L, &n);
    if (!xs) return;
    /* normal-model: mu + sigma * z(level), z from the regularized normal
     * quantile via the inverse error function (core's closed form). */
    double m = 0.0, m2 = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - m;
        m += d / (double)(i + 1);
        m2 += d * (xs[i] - m);
    }
    free(xs);
    double sd = sqrt(m2 / (double)(n - 1));
    double z = m55_norminv(level);
    *out = val_num(-(m + z * sd));
}

static void native_calmar_ratio(VM *vm, int argc, Value *args, Value *out) {
    ObjList *P;
    if (argc < 1 || !m54_arg_list(vm, args[0], &P)) return;
    if (P->count < 2) { vm_set_error(vm, "calmar_ratio() needs >= 2 elements"); return; }
    int n; double *xs = m54_list_to_doubles(vm, P, &n);
    if (!xs) return;
    double m = 0.0;
    for (int i = 1; i < n; i++) {
        if (xs[i - 1] == 0.0) { free(xs); *out = val_num(NAN); return; }
        m += 100.0 * (xs[i] - xs[i - 1]) / xs[i - 1];
    }
    m /= (double)(n - 1);
    double mdd = 0.0, peak = xs[0];
    for (int i = 1; i < n; i++) {
        if (xs[i] > peak) peak = xs[i];
        double dd = peak > 0.0 ? 100.0 * (peak - xs[i]) / peak : 0.0;
        if (dd > mdd) mdd = dd;
    }
    free(xs);
    *out = val_num(mdd > 0.0 ? m / mdd : NAN);
}

/* ---- #55 wrappers: performance/risk on top of the #54 list helpers ---- */
Value b_rolling_mean(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_rolling_mean); }
Value b_rolling_stddev(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_rolling_stddev); }
Value b_ewma(VM *vm, int argc, Value *args)           { return vm_native(vm, argc, args, native_ewma); }
Value b_pct_change(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_pct_change); }
Value b_max_drawdown(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_max_drawdown); }
Value b_rsi(VM *vm, int argc, Value *args)            { return vm_native(vm, argc, args, native_rsi); }
Value b_rsi_cutler(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_rsi_cutler); }
Value b_macd(VM *vm, int argc, Value *args)           { return vm_native(vm, argc, args, native_macd); }
Value b_bollinger(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_bollinger); }
Value b_sharpe_ratio(VM *vm, int argc, Value *args)         { return vm_native(vm, argc, args, native_sharpe_ratio); }
Value b_sortino_ratio(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_sortino_ratio); }
Value b_annualized_return(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_annualized_return); }
Value b_annualized_volatility(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_annualized_volatility); }
Value b_sharpe_annualized(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_sharpe_annualized); }
Value b_cumulative_return(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_cumulative_return); }
Value b_semivariance(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_semivariance); }
Value b_downside_deviation(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_downside_deviation); }
Value b_value_at_risk(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_value_at_risk); }
Value b_expected_shortfall(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_expected_shortfall); }
Value b_param_var(VM *vm, int argc, Value *args)           { return vm_native(vm, argc, args, native_param_var); }
Value b_calmar_ratio(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_calmar_ratio); }
