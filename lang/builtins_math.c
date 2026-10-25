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
