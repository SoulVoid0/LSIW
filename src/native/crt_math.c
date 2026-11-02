/* C runtime math: math.h (double, float, and long double, which is a plain
 * double on Windows x64), float.h control words, fenv.h and the ucrt
 * complex.h subset. The host libm does the arithmetic; the MS-ABI wrappers
 * keep xmm6-xmm15 intact as Windows code expects.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "crt.h"
#include <complex.h>
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <string.h>

/* ---- one-argument and two-argument wrappers ---- */

#define M1(n)  API double ms_##n(double x) { return n(x); } \
               API float ms_##n##f(float x) { return n##f(x); }
#define M2(n)  API double ms_##n(double x, double y) { return n(x, y); } \
               API float ms_##n##f(float x, float y) { return n##f(x, y); }

M1(acos) M1(asin) M1(atan) M1(cos) M1(sin) M1(tan) M1(cosh) M1(sinh) M1(tanh)
M1(acosh) M1(asinh) M1(atanh) M1(exp) M1(exp2) M1(expm1) M1(log) M1(log10) M1(log1p) M1(log2) M1(logb)
M1(sqrt) M1(cbrt) M1(ceil) M1(floor) M1(fabs) M1(round) M1(trunc) M1(rint) M1(nearbyint)
M1(erf) M1(erfc) M1(lgamma) M1(tgamma)
M2(atan2) M2(pow) M2(fmod) M2(hypot) M2(remainder) M2(fmin) M2(fmax) M2(fdim) M2(copysign) M2(nextafter)

API double ms_fma(double a, double b, double c) { return fma(a, b, c); }
API float ms_fmaf(float a, float b, float c) { return fmaf(a, b, c); }
API double ms_frexp(double x, int *e) { return frexp(x, e); }
API float ms_frexpf(float x, int *e) { return frexpf(x, e); }
API double ms_ldexp(double x, int e) { return ldexp(x, e); }
API float ms_ldexpf(float x, int e) { return ldexpf(x, e); }
API double ms_modf(double x, double *i) { return modf(x, i); }
API float ms_modff(float x, float *i) { return modff(x, i); }
API double ms_remquo(double x, double y, int *q) { return remquo(x, y, q); }
API float ms_remquof(float x, float y, int *q) { return remquof(x, y, q); }
API double ms_scalbn(double x, int n) { return scalbn(x, n); }
API float ms_scalbnf(float x, int n) { return scalbnf(x, n); }
API double ms_scalbln(double x, int32_t n) { return scalbln(x, n); }
API float ms_scalblnf(float x, int32_t n) { return scalblnf(x, n); }
API int ms_ilogb(double x) { return ilogb(x); }
API int ms_ilogbf(float x) { return ilogbf(x); }
API double ms_nexttoward(double x, double y) { return nextafter(x, y); }
API float ms_nexttowardf(float x, double y) { return nexttowardf(x, (long double)y); }
API double ms_nan(const char *s) { (void)s; return NAN; }
API float ms_nanf(const char *s) { (void)s; return NAN; }

/* long results are 32-bit on Windows */
static int32_t clamp_l(double v)
{
    if (isnan(v) || v >= 2147483648.0 || v < -2147483648.0) { feraiseexcept(FE_INVALID); return INT32_MIN; }
    return (int32_t)v;
}
API int32_t ms_lround(double x) { return clamp_l(round(x)); }
API int32_t ms_lroundf(float x) { return clamp_l(roundf(x)); }
API int32_t ms_lrint(double x) { return clamp_l(rint(x)); }
API int32_t ms_lrintf(float x) { return clamp_l(rintf(x)); }
API long long ms_llround(double x) { return llround(x); }
API long long ms_llroundf(float x) { return llroundf(x); }
API long long ms_llrint(double x) { return llrint(x); }
API long long ms_llrintf(float x) { return llrintf(x); }

/* ---- Microsoft extensions ---- */

API int _finite(double x) { return isfinite(x) != 0; }
API int _finitef(float x) { return isfinite(x) != 0; }
API int _isnan(double x) { return isnan(x) != 0; }
API int _isnanf(float x) { return isnan(x) != 0; }
API double _copysign(double x, double y) { return copysign(x, y); }
API float _copysignf(float x, float y) { return copysignf(x, y); }
API double _chgsign(double x) { return -x; }
API float _chgsignf(float x) { return -x; }
API double _logb(double x) { return logb(x); }
API float _logbf(float x) { return logbf(x); }
API double _scalb(double x, int32_t e) { return scalbn(x, e); }
API float _scalbf(float x, int32_t e) { return scalbnf(x, e); }
API double _nextafter(double x, double y) { return nextafter(x, y); }
API float _nextafterf(float x, float y) { return nextafterf(x, y); }
API double _hypot(double x, double y) { return hypot(x, y); }
API float _hypotf(float x, float y) { return hypotf(x, y); }
API double _j0(double x) { return j0(x); }
API double _j1(double x) { return j1(x); }
API double _jn(int n, double x) { return jn(n, x); }
API double _y0(double x) { return y0(x); }
API double _y1(double x) { return y1(x); }
API double _yn(int n, double x) { return yn(n, x); }
struct ms_complex_d { double x, y; };
API double _cabs(struct ms_complex_d z) { return hypot(z.x, z.y); }

/* _fpclass result bits */
#define FPCLASS_SNAN 0x0001
#define FPCLASS_QNAN 0x0002
#define FPCLASS_NINF 0x0004
#define FPCLASS_NN   0x0008
#define FPCLASS_ND   0x0010
#define FPCLASS_NZ   0x0020
#define FPCLASS_PZ   0x0040
#define FPCLASS_PD   0x0080
#define FPCLASS_PN   0x0100
#define FPCLASS_PINF 0x0200
static int fpclass(double x)
{
    int neg = signbit(x) != 0;
    switch (fpclassify(x)) {
    case FP_NAN: { uint64_t b; memcpy(&b, &x, 8); return (b & 0x0008000000000000ull) ? FPCLASS_QNAN : FPCLASS_SNAN; }
    case FP_INFINITE: return neg ? FPCLASS_NINF : FPCLASS_PINF;
    case FP_ZERO: return neg ? FPCLASS_NZ : FPCLASS_PZ;
    case FP_SUBNORMAL: return neg ? FPCLASS_ND : FPCLASS_PD;
    default: return neg ? FPCLASS_NN : FPCLASS_PN;
    }
}
API int _fpclass(double x) { return fpclass(x); }
API int _fpclassf(float x) { return fpclass(x); }

/* ucrt classification helpers (_dclass returns FP_* with Windows values) */
#define MS_FP_INFINITE 1
#define MS_FP_NAN      2
#define MS_FP_NORMAL  (-1)
#define MS_FP_SUBNORMAL (-2)
#define MS_FP_ZERO     0
static short ms_fpclassify(double x)
{
    switch (fpclassify(x)) {
    case FP_NAN: return MS_FP_NAN;
    case FP_INFINITE: return MS_FP_INFINITE;
    case FP_ZERO: return MS_FP_ZERO;
    case FP_SUBNORMAL: return MS_FP_SUBNORMAL;
    default: return MS_FP_NORMAL;
    }
}
API short _dclass(double x) { return ms_fpclassify(x); }
API short _fdclass(float x) { return ms_fpclassify(x); }
API short _ldclass(double x) { return ms_fpclassify(x); }
API int _dsign(double x) { return signbit(x) ? 0x8000 : 0; }
API int _fdsign(float x) { return signbit(x) ? 0x8000 : 0; }
API int _ldsign(double x) { return signbit(x) ? 0x8000 : 0; }
API short _dtest(double *x) { return ms_fpclassify(*x); }
API short _fdtest(float *x) { return ms_fpclassify(*x); }
API short _ldtest(double *x) { return ms_fpclassify(*x); }
API int _dpcomp(double x, double y) { return isunordered(x, y) ? 0 : (x < y ? 1 : x == y ? 2 : 4); }
API int _fdpcomp(float x, float y) { return isunordered(x, y) ? 0 : (x < y ? 1 : x == y ? 2 : 4); }

/* ---- floating-point control (MXCSR and the x87 control word) ---- */

#define EM_INEXACT    0x00000001
#define EM_UNDERFLOW  0x00000002
#define EM_OVERFLOW   0x00000004
#define EM_ZERODIVIDE 0x00000008
#define EM_INVALID    0x00000010
#define EM_DENORMAL   0x00080000
#define MCW_EM        0x0008001f
#define MCW_RC        0x00000300
#define MCW_DN        0x03000000
#define DN_FLUSH      0x01000000

static unsigned get_mxcsr(void) { unsigned m; __asm__ volatile("stmxcsr %0" : "=m"(m)); return m; }
static void set_mxcsr(unsigned m) { __asm__ volatile("ldmxcsr %0" : : "m"(m)); }

static unsigned cw_from_mxcsr(unsigned m)
{
    unsigned cw = 0;
    if (m & 0x0080) cw |= EM_INVALID;
    if (m & 0x0100) cw |= EM_DENORMAL;
    if (m & 0x0200) cw |= EM_ZERODIVIDE;
    if (m & 0x0400) cw |= EM_OVERFLOW;
    if (m & 0x0800) cw |= EM_UNDERFLOW;
    if (m & 0x1000) cw |= EM_INEXACT;
    switch ((m >> 13) & 3) { case 1: cw |= 0x100; break; case 2: cw |= 0x200; break; case 3: cw |= 0x300; break; }
    if (m & 0x8040) cw |= DN_FLUSH;
    return cw;
}
static unsigned mxcsr_from_cw(unsigned cw, unsigned m)
{
    m &= ~0xFFC0u;
    if (cw & EM_INVALID) m |= 0x0080;
    if (cw & EM_DENORMAL) m |= 0x0100;
    if (cw & EM_ZERODIVIDE) m |= 0x0200;
    if (cw & EM_OVERFLOW) m |= 0x0400;
    if (cw & EM_UNDERFLOW) m |= 0x0800;
    if (cw & EM_INEXACT) m |= 0x1000;
    switch (cw & MCW_RC) { case 0x100: m |= 1 << 13; break; case 0x200: m |= 2 << 13; break; case 0x300: m |= 3 << 13; break; }
    if ((cw & MCW_DN) == DN_FLUSH) m |= 0x8040;
    return m;
}
static void set_x87(unsigned cw)
{
    unsigned short x;
    __asm__ volatile("fnstcw %0" : "=m"(x));
    x &= (unsigned short)~0x0C3F;
    if (cw & EM_INVALID) x |= 0x01;
    if (cw & EM_DENORMAL) x |= 0x02;
    if (cw & EM_ZERODIVIDE) x |= 0x04;
    if (cw & EM_OVERFLOW) x |= 0x08;
    if (cw & EM_UNDERFLOW) x |= 0x10;
    if (cw & EM_INEXACT) x |= 0x20;
    x |= (unsigned short)((cw & MCW_RC) << 2);
    __asm__ volatile("fldcw %0" : : "m"(x));
}

API unsigned _controlfp(unsigned nw, unsigned mask)
{
    mask &= MCW_EM | MCW_RC | MCW_DN;
    unsigned m = get_mxcsr();
    unsigned cw = cw_from_mxcsr(m);
    if (mask) {
        cw = (cw & ~mask) | (nw & mask);
        set_mxcsr(mxcsr_from_cw(cw, m));
        set_x87(cw);
    }
    return cw;
}
API int _controlfp_s(unsigned *cur, unsigned nw, unsigned mask)
{
    unsigned r = _controlfp(nw, mask);
    if (cur) *cur = r;
    return 0;
}
API unsigned _control87(unsigned nw, unsigned mask) { return _controlfp(nw, mask); }
API void _set_controlfp(unsigned nw, unsigned mask) { _controlfp(nw, mask); }
API int __control87_2(unsigned nw, unsigned mask, unsigned *x87, unsigned *sse)
{
    unsigned r = _controlfp(nw, mask);
    if (x87) *x87 = r;
    if (sse) *sse = r;
    return 1;
}

static unsigned status_from_mxcsr(unsigned m)
{
    unsigned s = 0;
    if (m & 0x01) s |= EM_INVALID;
    if (m & 0x02) s |= EM_DENORMAL;
    if (m & 0x04) s |= EM_ZERODIVIDE;
    if (m & 0x08) s |= EM_OVERFLOW;
    if (m & 0x10) s |= EM_UNDERFLOW;
    if (m & 0x20) s |= EM_INEXACT;
    return s;
}
API unsigned _statusfp(void)
{
    unsigned short sw;
    __asm__ volatile("fnstsw %0" : "=m"(sw));
    return status_from_mxcsr(get_mxcsr() | (sw & 0x3f));
}
API void _statusfp2(unsigned *x87, unsigned *sse)
{
    unsigned short sw;
    __asm__ volatile("fnstsw %0" : "=m"(sw));
    if (x87) *x87 = status_from_mxcsr(sw & 0x3f);
    if (sse) *sse = status_from_mxcsr(get_mxcsr());
}
API unsigned _clearfp(void)
{
    unsigned s = _statusfp();
    set_mxcsr(get_mxcsr() & ~0x3Fu);
    __asm__ volatile("fnclex");
    return s;
}
API void _fpreset(void) { set_mxcsr(0x1F80); __asm__ volatile("fninit"); }
API int _set_SSE2_enable(int f) { return f; }
API int _get_FMA3_enable(void) { return 0; }
API int _set_FMA3_enable(int f) { (void)f; return 0; }

/* ---- fenv.h (Microsoft constants equal the _EM_ / _RC_ values) ---- */

typedef struct { uint32_t ctl, stat; } ms_fenv_t;

API int ms_fegetround(void) { return (int)(_controlfp(0, 0) & MCW_RC); }
API int ms_fesetround(int r)
{
    if (r & ~MCW_RC) return 1;
    _controlfp((unsigned)r, MCW_RC);
    return 0;
}
API int ms_feclearexcept(int e)
{
    unsigned m = get_mxcsr();
    if (e & EM_INVALID) m &= ~0x01u;
    if (e & EM_ZERODIVIDE) m &= ~0x04u;
    if (e & EM_OVERFLOW) m &= ~0x08u;
    if (e & EM_UNDERFLOW) m &= ~0x10u;
    if (e & EM_INEXACT) m &= ~0x20u;
    set_mxcsr(m);
    __asm__ volatile("fnclex");
    return 0;
}
API int ms_fetestexcept(int e) { return (int)(_statusfp() & (unsigned)e & 0x1f); }
API int ms_feraiseexcept(int e)
{
    int h = 0;
    if (e & EM_INVALID) h |= FE_INVALID;
    if (e & EM_ZERODIVIDE) h |= FE_DIVBYZERO;
    if (e & EM_OVERFLOW) h |= FE_OVERFLOW;
    if (e & EM_UNDERFLOW) h |= FE_UNDERFLOW;
    if (e & EM_INEXACT) h |= FE_INEXACT;
    return feraiseexcept(h);
}
API int ms_fegetenv(ms_fenv_t *env) { env->ctl = _controlfp(0, 0); env->stat = _statusfp(); return 0; }
API int ms_fesetenv(const ms_fenv_t *env)
{
    _controlfp(env->ctl, MCW_EM | MCW_RC | MCW_DN);
    ms_feclearexcept(0x1f);
    if (env->stat & 0x1f) {
        unsigned m = get_mxcsr();
        if (env->stat & EM_INVALID) m |= 0x01;
        if (env->stat & EM_ZERODIVIDE) m |= 0x04;
        if (env->stat & EM_OVERFLOW) m |= 0x08;
        if (env->stat & EM_UNDERFLOW) m |= 0x10;
        if (env->stat & EM_INEXACT) m |= 0x20;
        set_mxcsr(m);
    }
    return 0;
}
API int ms_feholdexcept(ms_fenv_t *env)
{
    ms_fegetenv(env);
    ms_feclearexcept(0x1f);
    _controlfp(MCW_EM, MCW_EM);
    return 0;
}
API int ms_feupdateenv(const ms_fenv_t *env)
{
    unsigned s = _statusfp() & 0x1f;
    ms_fesetenv(env);
    ms_feraiseexcept((int)s);
    return 0;
}
API int ms_fegetexceptflag(uint32_t *f, int e) { *f = _statusfp() & (unsigned)e & 0x1f; return 0; }
API int ms_fesetexceptflag(const uint32_t *f, int e)
{
    ms_feclearexcept(e);
    unsigned m = get_mxcsr(), v = *f & (unsigned)e;
    if (v & EM_INVALID) m |= 0x01;
    if (v & EM_ZERODIVIDE) m |= 0x04;
    if (v & EM_OVERFLOW) m |= 0x08;
    if (v & EM_UNDERFLOW) m |= 0x10;
    if (v & EM_INEXACT) m |= 0x20;
    set_mxcsr(m);
    return 0;
}

/* ---- complex.h (ucrt _Dcomplex / _Fcomplex) ---- */

typedef struct { double re, im; } dcx;
typedef struct { float re, im; } fcx;
static double complex to_c(dcx z) { return CMPLX(z.re, z.im); }
static dcx from_c(double complex z) { dcx r = { creal(z), cimag(z) }; return r; }
static float complex to_cf(fcx z) { return CMPLXF(z.re, z.im); }
static fcx from_cf(float complex z) { fcx r = { crealf(z), cimagf(z) }; return r; }

API dcx _Cbuild(double re, double im) { dcx r = { re, im }; return r; }
API fcx _FCbuild(float re, float im) { fcx r = { re, im }; return r; }
API dcx _LCbuild(double re, double im) { dcx r = { re, im }; return r; }
API double ms_creal(dcx z) { return z.re; }
API double ms_cimag(dcx z) { return z.im; }
API float ms_crealf(fcx z) { return z.re; }
API float ms_cimagf(fcx z) { return z.im; }
API double ms_cabs(dcx z) { return cabs(to_c(z)); }
API float ms_cabsf(fcx z) { return cabsf(to_cf(z)); }
API double ms_carg(dcx z) { return carg(to_c(z)); }
API float ms_cargf(fcx z) { return cargf(to_cf(z)); }
#define CX1(n) API dcx ms_##n(dcx z) { return from_c(n(to_c(z))); } \
               API fcx ms_##n##f(fcx z) { return from_cf(n##f(to_cf(z))); }
CX1(conj) CX1(cproj) CX1(csqrt) CX1(cexp) CX1(clog) CX1(csin) CX1(ccos) CX1(ctan) CX1(casin) CX1(cacos) CX1(catan)
CX1(csinh) CX1(ccosh) CX1(ctanh) CX1(casinh) CX1(cacosh) CX1(catanh)
API dcx ms_cpow(dcx a, dcx b) { return from_c(cpow(to_c(a), to_c(b))); }
API fcx ms_cpowf(fcx a, fcx b) { return from_cf(cpowf(to_cf(a), to_cf(b))); }
API dcx _Cmulcc(dcx a, dcx b) { return from_c(to_c(a) * to_c(b)); }
API dcx _Cmulcr(dcx a, double b) { return from_c(to_c(a) * b); }
API fcx _FCmulcc(fcx a, fcx b) { return from_cf(to_cf(a) * to_cf(b)); }
API fcx _FCmulcr(fcx a, float b) { return from_cf(to_cf(a) * b); }

double crt_huge = HUGE_VAL;

#define EMF(n) EM(n), EM(n##f), EX(#n "l", ms_##n)
const struct lsiw_export lsiw_crt_math_exports[] = {
    EMF(acos), EMF(asin), EMF(atan), EMF(cos), EMF(sin), EMF(tan), EMF(cosh), EMF(sinh), EMF(tanh),
    EMF(acosh), EMF(asinh), EMF(atanh), EMF(exp), EMF(exp2), EMF(expm1), EMF(log), EMF(log10), EMF(log1p), EMF(log2), EMF(logb),
    EMF(sqrt), EMF(cbrt), EMF(ceil), EMF(floor), EMF(fabs), EMF(round), EMF(trunc), EMF(rint), EMF(nearbyint),
    EMF(erf), EMF(erfc), EMF(lgamma), EMF(tgamma),
    EMF(atan2), EMF(pow), EMF(fmod), EMF(hypot), EMF(remainder), EMF(fmin), EMF(fmax), EMF(fdim), EMF(copysign), EMF(nextafter),
    EMF(fma), EMF(frexp), EMF(ldexp), EMF(modf), EMF(remquo), EMF(scalbn), EMF(scalbln), EMF(ilogb), EMF(nexttoward), EMF(nan),
    EMF(lround), EMF(lrint), EMF(llround), EMF(llrint),
    E(_finite), E(_finitef), E(_isnan), E(_isnanf), E(_copysign), E(_copysignf), E(_chgsign), E(_chgsignf),
    EX("_chgsignl", _chgsign), EX("_copysignl", _copysign),
    E(_logb), E(_logbf), E(_scalb), E(_scalbf), E(_nextafter), E(_nextafterf), E(_hypot), E(_hypotf),
    E(_j0), E(_j1), E(_jn), E(_y0), E(_y1), E(_yn), E(_cabs), E(_fpclass), E(_fpclassf),
    E(_dclass), E(_fdclass), E(_ldclass), E(_dsign), E(_fdsign), E(_ldsign), E(_dtest), E(_fdtest), E(_ldtest), E(_dpcomp), E(_fdpcomp),
    EX("j0", _j0), EX("j1", _j1), EX("jn", _jn), EX("y0", _y0), EX("y1", _y1), EX("yn", _yn), EX("hypot", ms_hypot), EX("cabs", _cabs),
    E(_controlfp), E(_controlfp_s), E(_control87), E(_set_controlfp), E(__control87_2), E(_statusfp), E(_statusfp2), E(_clearfp),
    E(_fpreset), E(_set_SSE2_enable), E(_get_FMA3_enable), E(_set_FMA3_enable),
    EM(fegetround), EM(fesetround), EM(feclearexcept), EM(fetestexcept), EM(feraiseexcept), EM(fegetenv), EM(fesetenv),
    EM(feholdexcept), EM(feupdateenv), EM(fegetexceptflag), EM(fesetexceptflag),
    E(_Cbuild), E(_FCbuild), E(_LCbuild), EM(creal), EM(cimag), EM(crealf), EM(cimagf), EX("creall", ms_creal), EX("cimagl", ms_cimag),
    EX("cabs", ms_cabs), EM(cabsf), EX("cabsl", ms_cabs), EM(carg), EM(cargf), EX("cargl", ms_carg),
    EM(conj), EM(conjf), EM(cproj), EM(cprojf), EM(csqrt), EM(csqrtf), EM(cexp), EM(cexpf), EM(clog), EM(clogf),
    EM(csin), EM(csinf), EM(ccos), EM(ccosf), EM(ctan), EM(ctanf), EM(casin), EM(casinf), EM(cacos), EM(cacosf), EM(catan), EM(catanf),
    EM(csinh), EM(csinhf), EM(ccosh), EM(ccoshf), EM(ctanh), EM(ctanhf), EM(casinh), EM(casinhf), EM(cacosh), EM(cacoshf),
    EM(catanh), EM(catanhf), EM(cpow), EM(cpowf), E(_Cmulcc), E(_Cmulcr), E(_FCmulcc), E(_FCmulcr),
    EX("_HUGE", &crt_huge),
    { NULL, NULL }
};
