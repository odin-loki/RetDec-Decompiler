/**
 * @file src/llvmir2hll/semantics/semantics/gcc_general_semantics/get_arity_of_func.cpp
 * @brief Implementation of semantics::gcc_general::getArityOfFunc() for
 *        GCCGeneralSemantics.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include "retdec/llvmir2hll/semantics/semantics/gcc_general_semantics/get_arity_of_func.h"

namespace retdec {
namespace llvmir2hll {
namespace semantics {
namespace gcc_general {

namespace {

#define ADD_FUNC_ARITY(name, params, variadic) m.emplace(name, FuncArity{(params), (variadic)})

/**
 * @brief This function is used to initialize FUNC_ARITY_MAP later in the file.
 *
 * Every entry here was MEASURED against the system C headers rather than
 * recalled, by scripts/ci/check_libc_arity.py: for each name it compiles a call
 * with 0..8 arguments and records which counts the real declaration accepts.
 * A single accepted count is a fixed arity; acceptance of everything from N
 * upwards is variadic with N named parameters; anything else -- the function is
 * a macro, or is not declared on the platform -- is left out entirely, because
 * an entry that is only probably right is worse here than no entry.
 *
 * Run that script to re-derive this file. It is also a CI check: it fails if
 * the table and the headers ever disagree.
 */
const FuncArityMap& initFuncArityMap()
{
	static FuncArityMap m;

	// aio.h
	ADD_FUNC_ARITY("aio_cancel", 2, false);
	ADD_FUNC_ARITY("aio_error", 1, false);
	ADD_FUNC_ARITY("aio_fsync", 2, false);
	ADD_FUNC_ARITY("aio_read", 1, false);
	ADD_FUNC_ARITY("aio_return", 1, false);
	ADD_FUNC_ARITY("aio_suspend", 3, false);
	ADD_FUNC_ARITY("aio_write", 1, false);
	ADD_FUNC_ARITY("lio_listio", 4, false);

	// alloca.h
	ADD_FUNC_ARITY("alloca", 1, false);

	// arpa/inet.h
	ADD_FUNC_ARITY("inet_addr", 1, false);
	ADD_FUNC_ARITY("inet_makeaddr", 2, false);
	ADD_FUNC_ARITY("inet_network", 1, false);
	ADD_FUNC_ARITY("inet_ntop", 4, false);
	ADD_FUNC_ARITY("inet_pton", 3, false);

	// assert.h
	ADD_FUNC_ARITY("__assert", 3, false);
	ADD_FUNC_ARITY("__assert_fail", 4, false);
	ADD_FUNC_ARITY("__assert_perror_fail", 4, false);

	// ctype.h
	ADD_FUNC_ARITY("__ctype_b_loc", 0, false);
	ADD_FUNC_ARITY("__ctype_tolower_loc", 0, false);
	ADD_FUNC_ARITY("__ctype_toupper_loc", 0, false);
	ADD_FUNC_ARITY("isalnum", 1, false);
	ADD_FUNC_ARITY("isalpha", 1, false);
	ADD_FUNC_ARITY("isblank", 1, false);
	ADD_FUNC_ARITY("iscntrl", 1, false);
	ADD_FUNC_ARITY("isdigit", 1, false);
	ADD_FUNC_ARITY("isgraph", 1, false);
	ADD_FUNC_ARITY("islower", 1, false);
	ADD_FUNC_ARITY("isprint", 1, false);
	ADD_FUNC_ARITY("ispunct", 1, false);
	ADD_FUNC_ARITY("isspace", 1, false);
	ADD_FUNC_ARITY("isupper", 1, false);
	ADD_FUNC_ARITY("isxdigit", 1, false);
	ADD_FUNC_ARITY("tolower", 1, false);
	ADD_FUNC_ARITY("toupper", 1, false);

	// dirent.h
	ADD_FUNC_ARITY("closedir", 1, false);
	ADD_FUNC_ARITY("opendir", 1, false);
	ADD_FUNC_ARITY("readdir", 1, false);
	ADD_FUNC_ARITY("rewinddir", 1, false);

	// dlfcn.h
	ADD_FUNC_ARITY("dlclose", 1, false);
	ADD_FUNC_ARITY("dlerror", 0, false);
	ADD_FUNC_ARITY("dlopen", 2, false);
	ADD_FUNC_ARITY("dlsym", 2, false);

	// errno.h
	ADD_FUNC_ARITY("__errno_location", 0, false);

	// error.h
	ADD_FUNC_ARITY("__error_alias", 3, true);
	ADD_FUNC_ARITY("__error_at_line_alias", 5, true);
	ADD_FUNC_ARITY("__error_at_line_noreturn", 5, true);
	ADD_FUNC_ARITY("__error_noreturn", 3, true);
	ADD_FUNC_ARITY("error", 3, true);
	ADD_FUNC_ARITY("error_at_line", 5, true);

	// fcntl.h
	ADD_FUNC_ARITY("creat", 2, false);
	ADD_FUNC_ARITY("fcntl", 2, true);
	ADD_FUNC_ARITY("open", 2, true);

	// fenv.h
	ADD_FUNC_ARITY("feclearexcept", 1, false);
	ADD_FUNC_ARITY("fegetenv", 1, false);
	ADD_FUNC_ARITY("fegetexceptflag", 2, false);
	ADD_FUNC_ARITY("fegetround", 0, false);
	ADD_FUNC_ARITY("feholdexcept", 1, false);
	ADD_FUNC_ARITY("feraiseexcept", 1, false);
	ADD_FUNC_ARITY("fesetenv", 1, false);
	ADD_FUNC_ARITY("fesetexceptflag", 2, false);
	ADD_FUNC_ARITY("fesetround", 1, false);
	ADD_FUNC_ARITY("fetestexcept", 1, false);
	ADD_FUNC_ARITY("feupdateenv", 1, false);

	// fmtmsg.h
	ADD_FUNC_ARITY("fmtmsg", 6, false);

	// fnmatch.h
	ADD_FUNC_ARITY("fnmatch", 3, false);

	// ftw.h
	ADD_FUNC_ARITY("ftw", 3, false);

	// gdbm.h
	ADD_FUNC_ARITY("gdbm_close", 1, false);
	ADD_FUNC_ARITY("gdbm_export", 4, false);
	ADD_FUNC_ARITY("gdbm_fdesc", 1, false);
	ADD_FUNC_ARITY("gdbm_firstkey", 1, false);
	ADD_FUNC_ARITY("gdbm_import", 3, false);
	ADD_FUNC_ARITY("gdbm_reorganize", 1, false);
	ADD_FUNC_ARITY("gdbm_setopt", 4, false);
	ADD_FUNC_ARITY("gdbm_strerror", 1, false);
	ADD_FUNC_ARITY("gdbm_sync", 1, false);
	ADD_FUNC_ARITY("gdbm_version_cmp", 2, false);

	// getopt.h
	ADD_FUNC_ARITY("getopt", 3, false);
	ADD_FUNC_ARITY("getopt_long", 5, false);

	// glob.h
	ADD_FUNC_ARITY("globfree", 1, false);

	// grp.h
	ADD_FUNC_ARITY("getgrgid", 1, false);
	ADD_FUNC_ARITY("getgrnam", 1, false);

	// iconv.h
	ADD_FUNC_ARITY("iconv", 5, false);
	ADD_FUNC_ARITY("iconv_close", 1, false);
	ADD_FUNC_ARITY("iconv_open", 2, false);

	// inttypes.h
	ADD_FUNC_ARITY("imaxabs", 1, false);
	ADD_FUNC_ARITY("imaxdiv", 2, false);
	ADD_FUNC_ARITY("strtoimax", 3, false);
	ADD_FUNC_ARITY("strtoumax", 3, false);
	ADD_FUNC_ARITY("wcstoimax", 3, false);
	ADD_FUNC_ARITY("wcstoumax", 3, false);

	// langinfo.h
	ADD_FUNC_ARITY("nl_langinfo", 1, false);

	// libgen.h
	ADD_FUNC_ARITY("__xpg_basename", 1, false);
	ADD_FUNC_ARITY("dirname", 1, false);

	// libintl.h
	ADD_FUNC_ARITY("bindtextdomain", 2, false);
	ADD_FUNC_ARITY("dcgettext", 3, false);
	ADD_FUNC_ARITY("dgettext", 2, false);
	ADD_FUNC_ARITY("gettext", 1, false);
	ADD_FUNC_ARITY("textdomain", 1, false);

	// locale.h
	ADD_FUNC_ARITY("localeconv", 0, false);
	ADD_FUNC_ARITY("setlocale", 2, false);

	// math.h
	ADD_FUNC_ARITY("__acos", 1, false);
	ADD_FUNC_ARITY("__acosf", 1, false);
	ADD_FUNC_ARITY("__acosh", 1, false);
	ADD_FUNC_ARITY("__acoshf", 1, false);
	ADD_FUNC_ARITY("__acoshl", 1, false);
	ADD_FUNC_ARITY("__acosl", 1, false);
	ADD_FUNC_ARITY("__asin", 1, false);
	ADD_FUNC_ARITY("__asinf", 1, false);
	ADD_FUNC_ARITY("__asinh", 1, false);
	ADD_FUNC_ARITY("__asinhf", 1, false);
	ADD_FUNC_ARITY("__asinhl", 1, false);
	ADD_FUNC_ARITY("__asinl", 1, false);
	ADD_FUNC_ARITY("__atan", 1, false);
	ADD_FUNC_ARITY("__atan2", 2, false);
	ADD_FUNC_ARITY("__atan2f", 2, false);
	ADD_FUNC_ARITY("__atan2l", 2, false);
	ADD_FUNC_ARITY("__atanf", 1, false);
	ADD_FUNC_ARITY("__atanh", 1, false);
	ADD_FUNC_ARITY("__atanhf", 1, false);
	ADD_FUNC_ARITY("__atanhl", 1, false);
	ADD_FUNC_ARITY("__atanl", 1, false);
	ADD_FUNC_ARITY("__cbrt", 1, false);
	ADD_FUNC_ARITY("__cbrtf", 1, false);
	ADD_FUNC_ARITY("__cbrtl", 1, false);
	ADD_FUNC_ARITY("__ceil", 1, false);
	ADD_FUNC_ARITY("__ceilf", 1, false);
	ADD_FUNC_ARITY("__ceill", 1, false);
	ADD_FUNC_ARITY("__copysign", 2, false);
	ADD_FUNC_ARITY("__copysignf", 2, false);
	ADD_FUNC_ARITY("__copysignl", 2, false);
	ADD_FUNC_ARITY("__cos", 1, false);
	ADD_FUNC_ARITY("__cosf", 1, false);
	ADD_FUNC_ARITY("__cosh", 1, false);
	ADD_FUNC_ARITY("__coshf", 1, false);
	ADD_FUNC_ARITY("__coshl", 1, false);
	ADD_FUNC_ARITY("__cosl", 1, false);
	ADD_FUNC_ARITY("__erf", 1, false);
	ADD_FUNC_ARITY("__erfc", 1, false);
	ADD_FUNC_ARITY("__erfcf", 1, false);
	ADD_FUNC_ARITY("__erfcl", 1, false);
	ADD_FUNC_ARITY("__erff", 1, false);
	ADD_FUNC_ARITY("__erfl", 1, false);
	ADD_FUNC_ARITY("__exp", 1, false);
	ADD_FUNC_ARITY("__exp2", 1, false);
	ADD_FUNC_ARITY("__exp2f", 1, false);
	ADD_FUNC_ARITY("__exp2l", 1, false);
	ADD_FUNC_ARITY("__expf", 1, false);
	ADD_FUNC_ARITY("__expl", 1, false);
	ADD_FUNC_ARITY("__expm1", 1, false);
	ADD_FUNC_ARITY("__expm1f", 1, false);
	ADD_FUNC_ARITY("__expm1l", 1, false);
	ADD_FUNC_ARITY("__fabs", 1, false);
	ADD_FUNC_ARITY("__fabsf", 1, false);
	ADD_FUNC_ARITY("__fabsl", 1, false);
	ADD_FUNC_ARITY("__fdim", 2, false);
	ADD_FUNC_ARITY("__fdimf", 2, false);
	ADD_FUNC_ARITY("__fdiml", 2, false);
	ADD_FUNC_ARITY("__finite", 1, false);
	ADD_FUNC_ARITY("__finitef", 1, false);
	ADD_FUNC_ARITY("__finitel", 1, false);
	ADD_FUNC_ARITY("__floor", 1, false);
	ADD_FUNC_ARITY("__floorf", 1, false);
	ADD_FUNC_ARITY("__floorl", 1, false);
	ADD_FUNC_ARITY("__fma", 3, false);
	ADD_FUNC_ARITY("__fmaf", 3, false);
	ADD_FUNC_ARITY("__fmal", 3, false);
	ADD_FUNC_ARITY("__fmax", 2, false);
	ADD_FUNC_ARITY("__fmaxf", 2, false);
	ADD_FUNC_ARITY("__fmaxl", 2, false);
	ADD_FUNC_ARITY("__fmin", 2, false);
	ADD_FUNC_ARITY("__fminf", 2, false);
	ADD_FUNC_ARITY("__fminl", 2, false);
	ADD_FUNC_ARITY("__fmod", 2, false);
	ADD_FUNC_ARITY("__fmodf", 2, false);
	ADD_FUNC_ARITY("__fmodl", 2, false);
	ADD_FUNC_ARITY("__fpclassify", 1, false);
	ADD_FUNC_ARITY("__fpclassifyf", 1, false);
	ADD_FUNC_ARITY("__fpclassifyl", 1, false);
	ADD_FUNC_ARITY("__frexp", 2, false);
	ADD_FUNC_ARITY("__frexpf", 2, false);
	ADD_FUNC_ARITY("__frexpl", 2, false);
	ADD_FUNC_ARITY("__hypot", 2, false);
	ADD_FUNC_ARITY("__hypotf", 2, false);
	ADD_FUNC_ARITY("__hypotl", 2, false);
	ADD_FUNC_ARITY("__ilogb", 1, false);
	ADD_FUNC_ARITY("__ilogbf", 1, false);
	ADD_FUNC_ARITY("__ilogbl", 1, false);
	ADD_FUNC_ARITY("__isinf", 1, false);
	ADD_FUNC_ARITY("__isinff", 1, false);
	ADD_FUNC_ARITY("__isinfl", 1, false);
	ADD_FUNC_ARITY("__isnan", 1, false);
	ADD_FUNC_ARITY("__isnanf", 1, false);
	ADD_FUNC_ARITY("__isnanl", 1, false);
	ADD_FUNC_ARITY("__ldexp", 2, false);
	ADD_FUNC_ARITY("__ldexpf", 2, false);
	ADD_FUNC_ARITY("__ldexpl", 2, false);
	ADD_FUNC_ARITY("__lgamma", 1, false);
	ADD_FUNC_ARITY("__lgammaf", 1, false);
	ADD_FUNC_ARITY("__lgammal", 1, false);
	ADD_FUNC_ARITY("__llrint", 1, false);
	ADD_FUNC_ARITY("__llrintf", 1, false);
	ADD_FUNC_ARITY("__llrintl", 1, false);
	ADD_FUNC_ARITY("__llround", 1, false);
	ADD_FUNC_ARITY("__llroundf", 1, false);
	ADD_FUNC_ARITY("__llroundl", 1, false);
	ADD_FUNC_ARITY("__log", 1, false);
	ADD_FUNC_ARITY("__log10", 1, false);
	ADD_FUNC_ARITY("__log10f", 1, false);
	ADD_FUNC_ARITY("__log10l", 1, false);
	ADD_FUNC_ARITY("__log1p", 1, false);
	ADD_FUNC_ARITY("__log1pf", 1, false);
	ADD_FUNC_ARITY("__log1pl", 1, false);
	ADD_FUNC_ARITY("__log2", 1, false);
	ADD_FUNC_ARITY("__log2f", 1, false);
	ADD_FUNC_ARITY("__log2l", 1, false);
	ADD_FUNC_ARITY("__logb", 1, false);
	ADD_FUNC_ARITY("__logbf", 1, false);
	ADD_FUNC_ARITY("__logbl", 1, false);
	ADD_FUNC_ARITY("__logf", 1, false);
	ADD_FUNC_ARITY("__logl", 1, false);
	ADD_FUNC_ARITY("__lrint", 1, false);
	ADD_FUNC_ARITY("__lrintf", 1, false);
	ADD_FUNC_ARITY("__lrintl", 1, false);
	ADD_FUNC_ARITY("__lround", 1, false);
	ADD_FUNC_ARITY("__lroundf", 1, false);
	ADD_FUNC_ARITY("__lroundl", 1, false);
	ADD_FUNC_ARITY("__modf", 2, false);
	ADD_FUNC_ARITY("__modff", 2, false);
	ADD_FUNC_ARITY("__modfl", 2, false);
	ADD_FUNC_ARITY("__nan", 1, false);
	ADD_FUNC_ARITY("__nanf", 1, false);
	ADD_FUNC_ARITY("__nanl", 1, false);
	ADD_FUNC_ARITY("__nearbyint", 1, false);
	ADD_FUNC_ARITY("__nearbyintf", 1, false);
	ADD_FUNC_ARITY("__nearbyintl", 1, false);
	ADD_FUNC_ARITY("__nextafter", 2, false);
	ADD_FUNC_ARITY("__nextafterf", 2, false);
	ADD_FUNC_ARITY("__nextafterl", 2, false);
	ADD_FUNC_ARITY("__nexttoward", 2, false);
	ADD_FUNC_ARITY("__nexttowardf", 2, false);
	ADD_FUNC_ARITY("__nexttowardl", 2, false);
	ADD_FUNC_ARITY("__pow", 2, false);
	ADD_FUNC_ARITY("__powf", 2, false);
	ADD_FUNC_ARITY("__powl", 2, false);
	ADD_FUNC_ARITY("__remainder", 2, false);
	ADD_FUNC_ARITY("__remainderf", 2, false);
	ADD_FUNC_ARITY("__remainderl", 2, false);
	ADD_FUNC_ARITY("__remquo", 3, false);
	ADD_FUNC_ARITY("__remquof", 3, false);
	ADD_FUNC_ARITY("__remquol", 3, false);
	ADD_FUNC_ARITY("__rint", 1, false);
	ADD_FUNC_ARITY("__rintf", 1, false);
	ADD_FUNC_ARITY("__rintl", 1, false);
	ADD_FUNC_ARITY("__round", 1, false);
	ADD_FUNC_ARITY("__roundf", 1, false);
	ADD_FUNC_ARITY("__roundl", 1, false);
	ADD_FUNC_ARITY("__scalbln", 2, false);
	ADD_FUNC_ARITY("__scalblnf", 2, false);
	ADD_FUNC_ARITY("__scalblnl", 2, false);
	ADD_FUNC_ARITY("__scalbn", 2, false);
	ADD_FUNC_ARITY("__scalbnf", 2, false);
	ADD_FUNC_ARITY("__scalbnl", 2, false);
	ADD_FUNC_ARITY("__signbit", 1, false);
	ADD_FUNC_ARITY("__signbitf", 1, false);
	ADD_FUNC_ARITY("__signbitl", 1, false);
	ADD_FUNC_ARITY("__sin", 1, false);
	ADD_FUNC_ARITY("__sinf", 1, false);
	ADD_FUNC_ARITY("__sinh", 1, false);
	ADD_FUNC_ARITY("__sinhf", 1, false);
	ADD_FUNC_ARITY("__sinhl", 1, false);
	ADD_FUNC_ARITY("__sinl", 1, false);
	ADD_FUNC_ARITY("__sqrt", 1, false);
	ADD_FUNC_ARITY("__sqrtf", 1, false);
	ADD_FUNC_ARITY("__sqrtl", 1, false);
	ADD_FUNC_ARITY("__tan", 1, false);
	ADD_FUNC_ARITY("__tanf", 1, false);
	ADD_FUNC_ARITY("__tanh", 1, false);
	ADD_FUNC_ARITY("__tanhf", 1, false);
	ADD_FUNC_ARITY("__tanhl", 1, false);
	ADD_FUNC_ARITY("__tanl", 1, false);
	ADD_FUNC_ARITY("__tgamma", 1, false);
	ADD_FUNC_ARITY("__tgammaf", 1, false);
	ADD_FUNC_ARITY("__tgammal", 1, false);
	ADD_FUNC_ARITY("__trunc", 1, false);
	ADD_FUNC_ARITY("__truncf", 1, false);
	ADD_FUNC_ARITY("__truncl", 1, false);
	ADD_FUNC_ARITY("acos", 1, false);
	ADD_FUNC_ARITY("acosf", 1, false);
	ADD_FUNC_ARITY("acosh", 1, false);
	ADD_FUNC_ARITY("acoshf", 1, false);
	ADD_FUNC_ARITY("acoshl", 1, false);
	ADD_FUNC_ARITY("acosl", 1, false);
	ADD_FUNC_ARITY("asin", 1, false);
	ADD_FUNC_ARITY("asinf", 1, false);
	ADD_FUNC_ARITY("asinh", 1, false);
	ADD_FUNC_ARITY("asinhf", 1, false);
	ADD_FUNC_ARITY("asinhl", 1, false);
	ADD_FUNC_ARITY("asinl", 1, false);
	ADD_FUNC_ARITY("atan", 1, false);
	ADD_FUNC_ARITY("atan2", 2, false);
	ADD_FUNC_ARITY("atan2f", 2, false);
	ADD_FUNC_ARITY("atan2l", 2, false);
	ADD_FUNC_ARITY("atanf", 1, false);
	ADD_FUNC_ARITY("atanh", 1, false);
	ADD_FUNC_ARITY("atanhf", 1, false);
	ADD_FUNC_ARITY("atanhl", 1, false);
	ADD_FUNC_ARITY("atanl", 1, false);
	ADD_FUNC_ARITY("cbrt", 1, false);
	ADD_FUNC_ARITY("cbrtf", 1, false);
	ADD_FUNC_ARITY("cbrtl", 1, false);
	ADD_FUNC_ARITY("ceil", 1, false);
	ADD_FUNC_ARITY("ceilf", 1, false);
	ADD_FUNC_ARITY("ceill", 1, false);
	ADD_FUNC_ARITY("copysign", 2, false);
	ADD_FUNC_ARITY("copysignf", 2, false);
	ADD_FUNC_ARITY("copysignl", 2, false);
	ADD_FUNC_ARITY("cos", 1, false);
	ADD_FUNC_ARITY("cosf", 1, false);
	ADD_FUNC_ARITY("cosh", 1, false);
	ADD_FUNC_ARITY("coshf", 1, false);
	ADD_FUNC_ARITY("coshl", 1, false);
	ADD_FUNC_ARITY("cosl", 1, false);
	ADD_FUNC_ARITY("erf", 1, false);
	ADD_FUNC_ARITY("erfc", 1, false);
	ADD_FUNC_ARITY("erfcf", 1, false);
	ADD_FUNC_ARITY("erfcl", 1, false);
	ADD_FUNC_ARITY("erff", 1, false);
	ADD_FUNC_ARITY("erfl", 1, false);
	ADD_FUNC_ARITY("exp", 1, false);
	ADD_FUNC_ARITY("exp2", 1, false);
	ADD_FUNC_ARITY("exp2f", 1, false);
	ADD_FUNC_ARITY("exp2l", 1, false);
	ADD_FUNC_ARITY("expf", 1, false);
	ADD_FUNC_ARITY("expl", 1, false);
	ADD_FUNC_ARITY("expm1", 1, false);
	ADD_FUNC_ARITY("expm1f", 1, false);
	ADD_FUNC_ARITY("expm1l", 1, false);
	ADD_FUNC_ARITY("fabs", 1, false);
	ADD_FUNC_ARITY("fabsf", 1, false);
	ADD_FUNC_ARITY("fabsl", 1, false);
	ADD_FUNC_ARITY("fdim", 2, false);
	ADD_FUNC_ARITY("fdimf", 2, false);
	ADD_FUNC_ARITY("fdiml", 2, false);
	ADD_FUNC_ARITY("floor", 1, false);
	ADD_FUNC_ARITY("floorf", 1, false);
	ADD_FUNC_ARITY("floorl", 1, false);
	ADD_FUNC_ARITY("fma", 3, false);
	ADD_FUNC_ARITY("fmaf", 3, false);
	ADD_FUNC_ARITY("fmal", 3, false);
	ADD_FUNC_ARITY("fmax", 2, false);
	ADD_FUNC_ARITY("fmaxf", 2, false);
	ADD_FUNC_ARITY("fmaxl", 2, false);
	ADD_FUNC_ARITY("fmin", 2, false);
	ADD_FUNC_ARITY("fminf", 2, false);
	ADD_FUNC_ARITY("fminl", 2, false);
	ADD_FUNC_ARITY("fmod", 2, false);
	ADD_FUNC_ARITY("fmodf", 2, false);
	ADD_FUNC_ARITY("fmodl", 2, false);
	ADD_FUNC_ARITY("frexp", 2, false);
	ADD_FUNC_ARITY("frexpf", 2, false);
	ADD_FUNC_ARITY("frexpl", 2, false);
	ADD_FUNC_ARITY("hypot", 2, false);
	ADD_FUNC_ARITY("hypotf", 2, false);
	ADD_FUNC_ARITY("hypotl", 2, false);
	ADD_FUNC_ARITY("ilogb", 1, false);
	ADD_FUNC_ARITY("ilogbf", 1, false);
	ADD_FUNC_ARITY("ilogbl", 1, false);
	ADD_FUNC_ARITY("ldexp", 2, false);
	ADD_FUNC_ARITY("ldexpf", 2, false);
	ADD_FUNC_ARITY("ldexpl", 2, false);
	ADD_FUNC_ARITY("lgamma", 1, false);
	ADD_FUNC_ARITY("lgammaf", 1, false);
	ADD_FUNC_ARITY("lgammal", 1, false);
	ADD_FUNC_ARITY("llrint", 1, false);
	ADD_FUNC_ARITY("llrintf", 1, false);
	ADD_FUNC_ARITY("llrintl", 1, false);
	ADD_FUNC_ARITY("llround", 1, false);
	ADD_FUNC_ARITY("llroundf", 1, false);
	ADD_FUNC_ARITY("llroundl", 1, false);
	ADD_FUNC_ARITY("log", 1, false);
	ADD_FUNC_ARITY("log10", 1, false);
	ADD_FUNC_ARITY("log10f", 1, false);
	ADD_FUNC_ARITY("log10l", 1, false);
	ADD_FUNC_ARITY("log1p", 1, false);
	ADD_FUNC_ARITY("log1pf", 1, false);
	ADD_FUNC_ARITY("log1pl", 1, false);
	ADD_FUNC_ARITY("log2", 1, false);
	ADD_FUNC_ARITY("log2f", 1, false);
	ADD_FUNC_ARITY("log2l", 1, false);
	ADD_FUNC_ARITY("logb", 1, false);
	ADD_FUNC_ARITY("logbf", 1, false);
	ADD_FUNC_ARITY("logbl", 1, false);
	ADD_FUNC_ARITY("logf", 1, false);
	ADD_FUNC_ARITY("logl", 1, false);
	ADD_FUNC_ARITY("lrint", 1, false);
	ADD_FUNC_ARITY("lrintf", 1, false);
	ADD_FUNC_ARITY("lrintl", 1, false);
	ADD_FUNC_ARITY("lround", 1, false);
	ADD_FUNC_ARITY("lroundf", 1, false);
	ADD_FUNC_ARITY("lroundl", 1, false);
	ADD_FUNC_ARITY("modf", 2, false);
	ADD_FUNC_ARITY("modff", 2, false);
	ADD_FUNC_ARITY("modfl", 2, false);
	ADD_FUNC_ARITY("nan", 1, false);
	ADD_FUNC_ARITY("nanf", 1, false);
	ADD_FUNC_ARITY("nanl", 1, false);
	ADD_FUNC_ARITY("nearbyint", 1, false);
	ADD_FUNC_ARITY("nearbyintf", 1, false);
	ADD_FUNC_ARITY("nearbyintl", 1, false);
	ADD_FUNC_ARITY("nextafter", 2, false);
	ADD_FUNC_ARITY("nextafterf", 2, false);
	ADD_FUNC_ARITY("nextafterl", 2, false);
	ADD_FUNC_ARITY("nexttoward", 2, false);
	ADD_FUNC_ARITY("nexttowardf", 2, false);
	ADD_FUNC_ARITY("nexttowardl", 2, false);
	ADD_FUNC_ARITY("pow", 2, false);
	ADD_FUNC_ARITY("powf", 2, false);
	ADD_FUNC_ARITY("powl", 2, false);
	ADD_FUNC_ARITY("remainder", 2, false);
	ADD_FUNC_ARITY("remainderf", 2, false);
	ADD_FUNC_ARITY("remainderl", 2, false);
	ADD_FUNC_ARITY("remquo", 3, false);
	ADD_FUNC_ARITY("remquof", 3, false);
	ADD_FUNC_ARITY("remquol", 3, false);
	ADD_FUNC_ARITY("rint", 1, false);
	ADD_FUNC_ARITY("rintf", 1, false);
	ADD_FUNC_ARITY("rintl", 1, false);
	ADD_FUNC_ARITY("round", 1, false);
	ADD_FUNC_ARITY("roundf", 1, false);
	ADD_FUNC_ARITY("roundl", 1, false);
	ADD_FUNC_ARITY("scalbln", 2, false);
	ADD_FUNC_ARITY("scalblnf", 2, false);
	ADD_FUNC_ARITY("scalblnl", 2, false);
	ADD_FUNC_ARITY("scalbn", 2, false);
	ADD_FUNC_ARITY("scalbnf", 2, false);
	ADD_FUNC_ARITY("scalbnl", 2, false);
	ADD_FUNC_ARITY("sin", 1, false);
	ADD_FUNC_ARITY("sinf", 1, false);
	ADD_FUNC_ARITY("sinh", 1, false);
	ADD_FUNC_ARITY("sinhf", 1, false);
	ADD_FUNC_ARITY("sinhl", 1, false);
	ADD_FUNC_ARITY("sinl", 1, false);
	ADD_FUNC_ARITY("sqrt", 1, false);
	ADD_FUNC_ARITY("sqrtf", 1, false);
	ADD_FUNC_ARITY("sqrtl", 1, false);
	ADD_FUNC_ARITY("tan", 1, false);
	ADD_FUNC_ARITY("tanf", 1, false);
	ADD_FUNC_ARITY("tanh", 1, false);
	ADD_FUNC_ARITY("tanhf", 1, false);
	ADD_FUNC_ARITY("tanhl", 1, false);
	ADD_FUNC_ARITY("tanl", 1, false);
	ADD_FUNC_ARITY("tgamma", 1, false);
	ADD_FUNC_ARITY("tgammaf", 1, false);
	ADD_FUNC_ARITY("tgammal", 1, false);
	ADD_FUNC_ARITY("trunc", 1, false);
	ADD_FUNC_ARITY("truncf", 1, false);
	ADD_FUNC_ARITY("truncl", 1, false);

	// monetary.h
	ADD_FUNC_ARITY("strfmon", 3, true);

	// mqueue.h
	ADD_FUNC_ARITY("mq_close", 1, false);
	ADD_FUNC_ARITY("mq_getattr", 2, false);
	ADD_FUNC_ARITY("mq_notify", 2, false);
	ADD_FUNC_ARITY("mq_open", 2, true);
	ADD_FUNC_ARITY("mq_receive", 4, false);
	ADD_FUNC_ARITY("mq_send", 4, false);
	ADD_FUNC_ARITY("mq_setattr", 3, false);
	ADD_FUNC_ARITY("mq_unlink", 1, false);

	// ndbm.h
	ADD_FUNC_ARITY("dbm_clearerr", 1, false);
	ADD_FUNC_ARITY("dbm_close", 1, false);
	ADD_FUNC_ARITY("dbm_dirfno", 1, false);
	ADD_FUNC_ARITY("dbm_error", 1, false);
	ADD_FUNC_ARITY("dbm_firstkey", 1, false);
	ADD_FUNC_ARITY("dbm_nextkey", 1, false);
	ADD_FUNC_ARITY("dbm_open", 3, false);
	ADD_FUNC_ARITY("dbm_pagfno", 1, false);
	ADD_FUNC_ARITY("dbm_rdonly", 1, false);

	// net/if.h
	ADD_FUNC_ARITY("if_freenameindex", 1, false);
	ADD_FUNC_ARITY("if_indextoname", 2, false);
	ADD_FUNC_ARITY("if_nameindex", 0, false);
	ADD_FUNC_ARITY("if_nametoindex", 1, false);

	// netdb.h
	ADD_FUNC_ARITY("__h_errno_location", 0, false);
	ADD_FUNC_ARITY("endhostent", 0, false);
	ADD_FUNC_ARITY("endnetent", 0, false);
	ADD_FUNC_ARITY("endprotoent", 0, false);
	ADD_FUNC_ARITY("endservent", 0, false);
	ADD_FUNC_ARITY("gethostbyaddr", 3, false);
	ADD_FUNC_ARITY("gethostbyname", 1, false);
	ADD_FUNC_ARITY("gethostent", 0, false);
	ADD_FUNC_ARITY("getnetbyaddr", 2, false);
	ADD_FUNC_ARITY("getnetbyname", 1, false);
	ADD_FUNC_ARITY("getnetent", 0, false);
	ADD_FUNC_ARITY("getprotobyname", 1, false);
	ADD_FUNC_ARITY("getprotobynumber", 1, false);
	ADD_FUNC_ARITY("getprotoent", 0, false);
	ADD_FUNC_ARITY("getservbyname", 2, false);
	ADD_FUNC_ARITY("getservbyport", 2, false);
	ADD_FUNC_ARITY("getservent", 0, false);
	ADD_FUNC_ARITY("sethostent", 1, false);
	ADD_FUNC_ARITY("setnetent", 1, false);
	ADD_FUNC_ARITY("setprotoent", 1, false);
	ADD_FUNC_ARITY("setservent", 1, false);

	// netinet/in.h
	ADD_FUNC_ARITY("htonl", 1, false);
	ADD_FUNC_ARITY("htons", 1, false);
	ADD_FUNC_ARITY("ntohl", 1, false);
	ADD_FUNC_ARITY("ntohs", 1, false);

	// nl_types.h
	ADD_FUNC_ARITY("catclose", 1, false);
	ADD_FUNC_ARITY("catgets", 4, false);
	ADD_FUNC_ARITY("catopen", 2, false);

	// pthread.h
	ADD_FUNC_ARITY("__pthread_register_cancel", 1, false);
	ADD_FUNC_ARITY("__pthread_unregister_cancel", 1, false);
	ADD_FUNC_ARITY("__pthread_unwind_next", 1, false);
	ADD_FUNC_ARITY("pthread_attr_destroy", 1, false);
	ADD_FUNC_ARITY("pthread_attr_getdetachstate", 2, false);
	ADD_FUNC_ARITY("pthread_attr_getguardsize", 2, false);
	ADD_FUNC_ARITY("pthread_attr_getinheritsched", 2, false);
	ADD_FUNC_ARITY("pthread_attr_getschedparam", 2, false);
	ADD_FUNC_ARITY("pthread_attr_getschedpolicy", 2, false);
	ADD_FUNC_ARITY("pthread_attr_getscope", 2, false);
	ADD_FUNC_ARITY("pthread_attr_getstackaddr", 2, false);
	ADD_FUNC_ARITY("pthread_attr_getstacksize", 2, false);
	ADD_FUNC_ARITY("pthread_attr_init", 1, false);
	ADD_FUNC_ARITY("pthread_attr_setdetachstate", 2, false);
	ADD_FUNC_ARITY("pthread_attr_setguardsize", 2, false);
	ADD_FUNC_ARITY("pthread_attr_setinheritsched", 2, false);
	ADD_FUNC_ARITY("pthread_attr_setschedparam", 2, false);
	ADD_FUNC_ARITY("pthread_attr_setschedpolicy", 2, false);
	ADD_FUNC_ARITY("pthread_attr_setscope", 2, false);
	ADD_FUNC_ARITY("pthread_attr_setstackaddr", 2, false);
	ADD_FUNC_ARITY("pthread_attr_setstacksize", 2, false);
	ADD_FUNC_ARITY("pthread_cancel", 1, false);
	ADD_FUNC_ARITY("pthread_cond_broadcast", 1, false);
	ADD_FUNC_ARITY("pthread_cond_destroy", 1, false);
	ADD_FUNC_ARITY("pthread_cond_init", 2, false);
	ADD_FUNC_ARITY("pthread_cond_signal", 1, false);
	ADD_FUNC_ARITY("pthread_cond_timedwait", 3, false);
	ADD_FUNC_ARITY("pthread_cond_wait", 2, false);
	ADD_FUNC_ARITY("pthread_condattr_destroy", 1, false);
	ADD_FUNC_ARITY("pthread_condattr_getpshared", 2, false);
	ADD_FUNC_ARITY("pthread_condattr_init", 1, false);
	ADD_FUNC_ARITY("pthread_condattr_setpshared", 2, false);
	ADD_FUNC_ARITY("pthread_create", 4, false);
	ADD_FUNC_ARITY("pthread_detach", 1, false);
	ADD_FUNC_ARITY("pthread_equal", 2, false);
	ADD_FUNC_ARITY("pthread_exit", 1, false);
	ADD_FUNC_ARITY("pthread_getschedparam", 3, false);
	ADD_FUNC_ARITY("pthread_getspecific", 1, false);
	ADD_FUNC_ARITY("pthread_join", 2, false);
	ADD_FUNC_ARITY("pthread_key_delete", 1, false);
	ADD_FUNC_ARITY("pthread_mutex_destroy", 1, false);
	ADD_FUNC_ARITY("pthread_mutex_getprioceiling", 2, false);
	ADD_FUNC_ARITY("pthread_mutex_init", 2, false);
	ADD_FUNC_ARITY("pthread_mutex_lock", 1, false);
	ADD_FUNC_ARITY("pthread_mutex_setprioceiling", 3, false);
	ADD_FUNC_ARITY("pthread_mutex_trylock", 1, false);
	ADD_FUNC_ARITY("pthread_mutex_unlock", 1, false);
	ADD_FUNC_ARITY("pthread_mutexattr_destroy", 1, false);
	ADD_FUNC_ARITY("pthread_mutexattr_getprioceiling", 2, false);
	ADD_FUNC_ARITY("pthread_mutexattr_getprotocol", 2, false);
	ADD_FUNC_ARITY("pthread_mutexattr_getpshared", 2, false);
	ADD_FUNC_ARITY("pthread_mutexattr_init", 1, false);
	ADD_FUNC_ARITY("pthread_mutexattr_setprioceiling", 2, false);
	ADD_FUNC_ARITY("pthread_mutexattr_setprotocol", 2, false);
	ADD_FUNC_ARITY("pthread_mutexattr_setpshared", 2, false);
	ADD_FUNC_ARITY("pthread_self", 0, false);
	ADD_FUNC_ARITY("pthread_setcancelstate", 2, false);
	ADD_FUNC_ARITY("pthread_setcanceltype", 2, false);
	ADD_FUNC_ARITY("pthread_setschedparam", 3, false);
	ADD_FUNC_ARITY("pthread_setschedprio", 2, false);
	ADD_FUNC_ARITY("pthread_setspecific", 2, false);
	ADD_FUNC_ARITY("pthread_testcancel", 0, false);

	// pwd.h
	ADD_FUNC_ARITY("getpwnam", 1, false);
	ADD_FUNC_ARITY("getpwuid", 1, false);

	// regex.h
	ADD_FUNC_ARITY("regcomp", 3, false);
	ADD_FUNC_ARITY("regerror", 4, false);
	ADD_FUNC_ARITY("regexec", 5, false);
	ADD_FUNC_ARITY("regfree", 1, false);

	// rpc/netdb.h
	ADD_FUNC_ARITY("endrpcent", 0, false);
	ADD_FUNC_ARITY("getrpcbyname", 1, false);
	ADD_FUNC_ARITY("getrpcbynumber", 1, false);
	ADD_FUNC_ARITY("getrpcent", 0, false);
	ADD_FUNC_ARITY("setrpcent", 1, false);

	// sched.h
	ADD_FUNC_ARITY("__sched_cpualloc", 1, false);
	ADD_FUNC_ARITY("__sched_cpucount", 2, false);
	ADD_FUNC_ARITY("__sched_cpufree", 1, false);
	ADD_FUNC_ARITY("sched_get_priority_max", 1, false);
	ADD_FUNC_ARITY("sched_get_priority_min", 1, false);
	ADD_FUNC_ARITY("sched_getparam", 2, false);
	ADD_FUNC_ARITY("sched_getscheduler", 1, false);
	ADD_FUNC_ARITY("sched_rr_get_interval", 2, false);
	ADD_FUNC_ARITY("sched_setparam", 2, false);
	ADD_FUNC_ARITY("sched_setscheduler", 3, false);
	ADD_FUNC_ARITY("sched_yield", 0, false);

	// search.h
	ADD_FUNC_ARITY("hcreate", 1, false);
	ADD_FUNC_ARITY("hdestroy", 0, false);
	ADD_FUNC_ARITY("lfind", 5, false);
	ADD_FUNC_ARITY("lsearch", 5, false);
	ADD_FUNC_ARITY("tdelete", 3, false);
	ADD_FUNC_ARITY("tfind", 3, false);
	ADD_FUNC_ARITY("tsearch", 3, false);
	ADD_FUNC_ARITY("twalk", 2, false);

	// semaphore.h
	ADD_FUNC_ARITY("sem_close", 1, false);
	ADD_FUNC_ARITY("sem_destroy", 1, false);
	ADD_FUNC_ARITY("sem_getvalue", 2, false);
	ADD_FUNC_ARITY("sem_init", 3, false);
	ADD_FUNC_ARITY("sem_open", 2, true);
	ADD_FUNC_ARITY("sem_post", 1, false);
	ADD_FUNC_ARITY("sem_trywait", 1, false);
	ADD_FUNC_ARITY("sem_unlink", 1, false);
	ADD_FUNC_ARITY("sem_wait", 1, false);

	// setjmp.h
	ADD_FUNC_ARITY("__sigsetjmp", 2, false);
	ADD_FUNC_ARITY("_setjmp", 1, false);
	ADD_FUNC_ARITY("longjmp", 2, false);
	ADD_FUNC_ARITY("setjmp", 1, false);

	// signal.h
	ADD_FUNC_ARITY("__libc_current_sigrtmax", 0, false);
	ADD_FUNC_ARITY("__libc_current_sigrtmin", 0, false);
	ADD_FUNC_ARITY("__sysv_signal", 2, false);
	ADD_FUNC_ARITY("raise", 1, false);
	ADD_FUNC_ARITY("signal", 2, false);

	// spawn.h
	ADD_FUNC_ARITY("posix_spawn", 6, false);
	ADD_FUNC_ARITY("posix_spawn_file_actions_addclose", 2, false);
	ADD_FUNC_ARITY("posix_spawn_file_actions_adddup2", 3, false);
	ADD_FUNC_ARITY("posix_spawn_file_actions_addopen", 5, false);
	ADD_FUNC_ARITY("posix_spawn_file_actions_destroy", 1, false);
	ADD_FUNC_ARITY("posix_spawn_file_actions_init", 1, false);
	ADD_FUNC_ARITY("posix_spawnattr_destroy", 1, false);
	ADD_FUNC_ARITY("posix_spawnattr_getflags", 2, false);
	ADD_FUNC_ARITY("posix_spawnattr_getpgroup", 2, false);
	ADD_FUNC_ARITY("posix_spawnattr_getschedparam", 2, false);
	ADD_FUNC_ARITY("posix_spawnattr_getschedpolicy", 2, false);
	ADD_FUNC_ARITY("posix_spawnattr_getsigdefault", 2, false);
	ADD_FUNC_ARITY("posix_spawnattr_getsigmask", 2, false);
	ADD_FUNC_ARITY("posix_spawnattr_init", 1, false);
	ADD_FUNC_ARITY("posix_spawnattr_setflags", 2, false);
	ADD_FUNC_ARITY("posix_spawnattr_setpgroup", 2, false);
	ADD_FUNC_ARITY("posix_spawnattr_setschedparam", 2, false);
	ADD_FUNC_ARITY("posix_spawnattr_setschedpolicy", 2, false);
	ADD_FUNC_ARITY("posix_spawnattr_setsigdefault", 2, false);
	ADD_FUNC_ARITY("posix_spawnattr_setsigmask", 2, false);
	ADD_FUNC_ARITY("posix_spawnp", 6, false);

	// stdio.h
	ADD_FUNC_ARITY("clearerr", 1, false);
	ADD_FUNC_ARITY("fclose", 1, false);
	ADD_FUNC_ARITY("feof", 1, false);
	ADD_FUNC_ARITY("ferror", 1, false);
	ADD_FUNC_ARITY("fflush", 1, false);
	ADD_FUNC_ARITY("fgetc", 1, false);
	ADD_FUNC_ARITY("fgetpos", 2, false);
	ADD_FUNC_ARITY("fgets", 3, false);
	ADD_FUNC_ARITY("fopen", 2, false);
	ADD_FUNC_ARITY("fprintf", 2, true);
	ADD_FUNC_ARITY("fputc", 2, false);
	ADD_FUNC_ARITY("fputs", 2, false);
	ADD_FUNC_ARITY("fread", 4, false);
	ADD_FUNC_ARITY("freopen", 3, false);
	ADD_FUNC_ARITY("fscanf", 2, true);
	ADD_FUNC_ARITY("fseek", 3, false);
	ADD_FUNC_ARITY("fsetpos", 2, false);
	ADD_FUNC_ARITY("ftell", 1, false);
	ADD_FUNC_ARITY("fwrite", 4, false);
	ADD_FUNC_ARITY("getc", 1, false);
	ADD_FUNC_ARITY("getchar", 0, false);
	ADD_FUNC_ARITY("perror", 1, false);
	ADD_FUNC_ARITY("printf", 1, true);
	ADD_FUNC_ARITY("putc", 2, false);
	ADD_FUNC_ARITY("putchar", 1, false);
	ADD_FUNC_ARITY("puts", 1, false);
	ADD_FUNC_ARITY("remove", 1, false);
	ADD_FUNC_ARITY("rename", 2, false);
	ADD_FUNC_ARITY("rewind", 1, false);
	ADD_FUNC_ARITY("scanf", 1, true);
	ADD_FUNC_ARITY("setbuf", 2, false);
	ADD_FUNC_ARITY("setvbuf", 4, false);
	ADD_FUNC_ARITY("snprintf", 3, true);
	ADD_FUNC_ARITY("sprintf", 2, true);
	ADD_FUNC_ARITY("sscanf", 2, true);
	ADD_FUNC_ARITY("tmpfile", 0, false);
	ADD_FUNC_ARITY("tmpnam", 1, false);
	ADD_FUNC_ARITY("ungetc", 2, false);
	ADD_FUNC_ARITY("vfprintf", 3, false);
	ADD_FUNC_ARITY("vfscanf", 3, false);
	ADD_FUNC_ARITY("vprintf", 2, false);
	ADD_FUNC_ARITY("vscanf", 2, false);
	ADD_FUNC_ARITY("vsnprintf", 4, false);
	ADD_FUNC_ARITY("vsprintf", 3, false);
	ADD_FUNC_ARITY("vsscanf", 3, false);

	// stdlib.h
	ADD_FUNC_ARITY("_Exit", 1, false);
	ADD_FUNC_ARITY("__ctype_get_mb_cur_max", 0, false);
	ADD_FUNC_ARITY("abort", 0, false);
	ADD_FUNC_ARITY("abs", 1, false);
	ADD_FUNC_ARITY("atof", 1, false);
	ADD_FUNC_ARITY("atoi", 1, false);
	ADD_FUNC_ARITY("atol", 1, false);
	ADD_FUNC_ARITY("atoll", 1, false);
	ADD_FUNC_ARITY("bsearch", 5, false);
	ADD_FUNC_ARITY("calloc", 2, false);
	ADD_FUNC_ARITY("div", 2, false);
	ADD_FUNC_ARITY("exit", 1, false);
	ADD_FUNC_ARITY("free", 1, false);
	ADD_FUNC_ARITY("getenv", 1, false);
	ADD_FUNC_ARITY("labs", 1, false);
	ADD_FUNC_ARITY("ldiv", 2, false);
	ADD_FUNC_ARITY("llabs", 1, false);
	ADD_FUNC_ARITY("lldiv", 2, false);
	ADD_FUNC_ARITY("malloc", 1, false);
	ADD_FUNC_ARITY("mblen", 2, false);
	ADD_FUNC_ARITY("mbstowcs", 3, false);
	ADD_FUNC_ARITY("mbtowc", 3, false);
	ADD_FUNC_ARITY("qsort", 4, false);
	ADD_FUNC_ARITY("rand", 0, false);
	ADD_FUNC_ARITY("realloc", 2, false);
	ADD_FUNC_ARITY("srand", 1, false);
	ADD_FUNC_ARITY("strtod", 2, false);
	ADD_FUNC_ARITY("strtof", 2, false);
	ADD_FUNC_ARITY("strtol", 3, false);
	ADD_FUNC_ARITY("strtold", 2, false);
	ADD_FUNC_ARITY("strtoll", 3, false);
	ADD_FUNC_ARITY("strtoul", 3, false);
	ADD_FUNC_ARITY("strtoull", 3, false);
	ADD_FUNC_ARITY("system", 1, false);
	ADD_FUNC_ARITY("wcstombs", 3, false);
	ADD_FUNC_ARITY("wctomb", 2, false);

	// string.h
	ADD_FUNC_ARITY("__strtok_r", 3, false);
	ADD_FUNC_ARITY("memchr", 3, false);
	ADD_FUNC_ARITY("memcmp", 3, false);
	ADD_FUNC_ARITY("memcpy", 3, false);
	ADD_FUNC_ARITY("memmove", 3, false);
	ADD_FUNC_ARITY("memset", 3, false);
	ADD_FUNC_ARITY("strcat", 2, false);
	ADD_FUNC_ARITY("strchr", 2, false);
	ADD_FUNC_ARITY("strcmp", 2, false);
	ADD_FUNC_ARITY("strcoll", 2, false);
	ADD_FUNC_ARITY("strcpy", 2, false);
	ADD_FUNC_ARITY("strcspn", 2, false);
	ADD_FUNC_ARITY("strerror", 1, false);
	ADD_FUNC_ARITY("strlen", 1, false);
	ADD_FUNC_ARITY("strncat", 3, false);
	ADD_FUNC_ARITY("strncmp", 3, false);
	ADD_FUNC_ARITY("strncpy", 3, false);
	ADD_FUNC_ARITY("strpbrk", 2, false);
	ADD_FUNC_ARITY("strrchr", 2, false);
	ADD_FUNC_ARITY("strspn", 2, false);
	ADD_FUNC_ARITY("strstr", 2, false);
	ADD_FUNC_ARITY("strtok", 2, false);
	ADD_FUNC_ARITY("strxfrm", 3, false);

	// sys/file.h
	ADD_FUNC_ARITY("flock", 2, false);

	// sys/ipc.h
	ADD_FUNC_ARITY("ftok", 2, false);

	// sys/mman.h
	ADD_FUNC_ARITY("mlock", 2, false);
	ADD_FUNC_ARITY("mlockall", 1, false);
	ADD_FUNC_ARITY("mmap", 6, false);
	ADD_FUNC_ARITY("mprotect", 3, false);
	ADD_FUNC_ARITY("msync", 3, false);
	ADD_FUNC_ARITY("munlock", 2, false);
	ADD_FUNC_ARITY("munlockall", 0, false);
	ADD_FUNC_ARITY("munmap", 2, false);
	ADD_FUNC_ARITY("shm_open", 3, false);
	ADD_FUNC_ARITY("shm_unlink", 1, false);

	// sys/msg.h
	ADD_FUNC_ARITY("msgctl", 3, false);
	ADD_FUNC_ARITY("msgget", 2, false);
	ADD_FUNC_ARITY("msgrcv", 5, false);
	ADD_FUNC_ARITY("msgsnd", 4, false);

	// sys/poll.h
	ADD_FUNC_ARITY("poll", 3, false);

	// sys/prctl.h
	ADD_FUNC_ARITY("prctl", 1, true);

	// sys/resource.h
	ADD_FUNC_ARITY("getpriority", 2, false);
	ADD_FUNC_ARITY("getrlimit", 2, false);
	ADD_FUNC_ARITY("getrusage", 2, false);
	ADD_FUNC_ARITY("setpriority", 3, false);
	ADD_FUNC_ARITY("setrlimit", 2, false);

	// sys/select.h
	ADD_FUNC_ARITY("select", 5, false);

	// sys/sem.h
	ADD_FUNC_ARITY("semctl", 3, true);
	ADD_FUNC_ARITY("semget", 3, false);
	ADD_FUNC_ARITY("semop", 3, false);

	// sys/shm.h
	ADD_FUNC_ARITY("__getpagesize", 0, false);
	ADD_FUNC_ARITY("shmat", 3, false);
	ADD_FUNC_ARITY("shmctl", 3, false);
	ADD_FUNC_ARITY("shmdt", 1, false);
	ADD_FUNC_ARITY("shmget", 3, false);

	// sys/socket.h
	ADD_FUNC_ARITY("__cmsg_nxthdr", 2, false);
	ADD_FUNC_ARITY("accept", 3, false);
	ADD_FUNC_ARITY("bind", 3, false);
	ADD_FUNC_ARITY("connect", 3, false);
	ADD_FUNC_ARITY("getpeername", 3, false);
	ADD_FUNC_ARITY("getsockname", 3, false);
	ADD_FUNC_ARITY("getsockopt", 5, false);
	ADD_FUNC_ARITY("listen", 2, false);
	ADD_FUNC_ARITY("recv", 4, false);
	ADD_FUNC_ARITY("recvfrom", 6, false);
	ADD_FUNC_ARITY("recvmsg", 3, false);
	ADD_FUNC_ARITY("send", 4, false);
	ADD_FUNC_ARITY("sendmsg", 3, false);
	ADD_FUNC_ARITY("sendto", 6, false);
	ADD_FUNC_ARITY("setsockopt", 5, false);
	ADD_FUNC_ARITY("shutdown", 2, false);
	ADD_FUNC_ARITY("socket", 3, false);
	ADD_FUNC_ARITY("socketpair", 4, false);

	// sys/stat.h
	ADD_FUNC_ARITY("chmod", 2, false);
	ADD_FUNC_ARITY("fstat", 2, false);
	ADD_FUNC_ARITY("mkdir", 2, false);
	ADD_FUNC_ARITY("mkfifo", 2, false);
	ADD_FUNC_ARITY("stat", 2, false);
	ADD_FUNC_ARITY("umask", 1, false);

	// sys/statvfs.h
	ADD_FUNC_ARITY("fstatvfs", 2, false);
	ADD_FUNC_ARITY("statvfs", 2, false);

	// sys/syslog.h
	ADD_FUNC_ARITY("closelog", 0, false);
	ADD_FUNC_ARITY("openlog", 3, false);
	ADD_FUNC_ARITY("setlogmask", 1, false);
	ADD_FUNC_ARITY("syslog", 2, true);

	// sys/sysmacros.h
	ADD_FUNC_ARITY("gnu_dev_major", 1, false);
	ADD_FUNC_ARITY("gnu_dev_makedev", 2, false);
	ADD_FUNC_ARITY("gnu_dev_minor", 1, false);

	// sys/time.h
	ADD_FUNC_ARITY("getitimer", 2, false);
	ADD_FUNC_ARITY("gettimeofday", 2, false);
	ADD_FUNC_ARITY("setitimer", 3, false);
	ADD_FUNC_ARITY("utimes", 2, false);

	// sys/times.h
	ADD_FUNC_ARITY("times", 1, false);

	// sys/uio.h
	ADD_FUNC_ARITY("readv", 3, false);
	ADD_FUNC_ARITY("writev", 3, false);

	// sys/utsname.h
	ADD_FUNC_ARITY("uname", 1, false);

	// sys/wait.h
	ADD_FUNC_ARITY("wait", 1, false);
	ADD_FUNC_ARITY("waitpid", 3, false);

	// termios.h
	ADD_FUNC_ARITY("cfgetispeed", 1, false);
	ADD_FUNC_ARITY("cfgetospeed", 1, false);
	ADD_FUNC_ARITY("cfsetispeed", 2, false);
	ADD_FUNC_ARITY("cfsetospeed", 2, false);
	ADD_FUNC_ARITY("tcdrain", 1, false);
	ADD_FUNC_ARITY("tcflow", 2, false);
	ADD_FUNC_ARITY("tcflush", 2, false);
	ADD_FUNC_ARITY("tcgetattr", 2, false);
	ADD_FUNC_ARITY("tcsendbreak", 2, false);
	ADD_FUNC_ARITY("tcsetattr", 3, false);

	// time.h
	ADD_FUNC_ARITY("asctime", 1, false);
	ADD_FUNC_ARITY("clock", 0, false);
	ADD_FUNC_ARITY("ctime", 1, false);
	ADD_FUNC_ARITY("difftime", 2, false);
	ADD_FUNC_ARITY("gmtime", 1, false);
	ADD_FUNC_ARITY("localtime", 1, false);
	ADD_FUNC_ARITY("mktime", 1, false);
	ADD_FUNC_ARITY("strftime", 4, false);
	ADD_FUNC_ARITY("time", 1, false);

	// uchar.h
	ADD_FUNC_ARITY("c16rtomb", 3, false);
	ADD_FUNC_ARITY("c32rtomb", 3, false);
	ADD_FUNC_ARITY("mbrtoc16", 4, false);
	ADD_FUNC_ARITY("mbrtoc32", 4, false);

	// ulimit.h
	ADD_FUNC_ARITY("ulimit", 1, true);

	// unistd.h
	ADD_FUNC_ARITY("__getpgid", 1, false);
	ADD_FUNC_ARITY("_exit", 1, false);
	ADD_FUNC_ARITY("access", 2, false);
	ADD_FUNC_ARITY("alarm", 1, false);
	ADD_FUNC_ARITY("chdir", 1, false);
	ADD_FUNC_ARITY("chown", 3, false);
	ADD_FUNC_ARITY("close", 1, false);
	ADD_FUNC_ARITY("dup", 1, false);
	ADD_FUNC_ARITY("dup2", 2, false);
	ADD_FUNC_ARITY("execl", 2, true);
	ADD_FUNC_ARITY("execle", 2, true);
	ADD_FUNC_ARITY("execlp", 2, true);
	ADD_FUNC_ARITY("execv", 2, false);
	ADD_FUNC_ARITY("execve", 3, false);
	ADD_FUNC_ARITY("execvp", 2, false);
	ADD_FUNC_ARITY("fork", 0, false);
	ADD_FUNC_ARITY("fpathconf", 2, false);
	ADD_FUNC_ARITY("fsync", 1, false);
	ADD_FUNC_ARITY("getcwd", 2, false);
	ADD_FUNC_ARITY("getegid", 0, false);
	ADD_FUNC_ARITY("geteuid", 0, false);
	ADD_FUNC_ARITY("getgid", 0, false);
	ADD_FUNC_ARITY("getgroups", 2, false);
	ADD_FUNC_ARITY("getlogin", 0, false);
	ADD_FUNC_ARITY("getpgrp", 0, false);
	ADD_FUNC_ARITY("getpid", 0, false);
	ADD_FUNC_ARITY("getppid", 0, false);
	ADD_FUNC_ARITY("getuid", 0, false);
	ADD_FUNC_ARITY("isatty", 1, false);
	ADD_FUNC_ARITY("link", 2, false);
	ADD_FUNC_ARITY("lseek", 3, false);
	ADD_FUNC_ARITY("pathconf", 2, false);
	ADD_FUNC_ARITY("pause", 0, false);
	ADD_FUNC_ARITY("pipe", 1, false);
	ADD_FUNC_ARITY("read", 3, false);
	ADD_FUNC_ARITY("rmdir", 1, false);
	ADD_FUNC_ARITY("setgid", 1, false);
	ADD_FUNC_ARITY("setpgid", 2, false);
	ADD_FUNC_ARITY("setsid", 0, false);
	ADD_FUNC_ARITY("setuid", 1, false);
	ADD_FUNC_ARITY("sleep", 1, false);
	ADD_FUNC_ARITY("sysconf", 1, false);
	ADD_FUNC_ARITY("tcgetpgrp", 1, false);
	ADD_FUNC_ARITY("tcsetpgrp", 2, false);
	ADD_FUNC_ARITY("ttyname", 1, false);
	ADD_FUNC_ARITY("ttyname_r", 3, false);
	ADD_FUNC_ARITY("unlink", 1, false);
	ADD_FUNC_ARITY("write", 3, false);

	// utime.h
	ADD_FUNC_ARITY("utime", 2, false);

	// utmpx.h
	ADD_FUNC_ARITY("endutxent", 0, false);
	ADD_FUNC_ARITY("getutxent", 0, false);
	ADD_FUNC_ARITY("getutxid", 1, false);
	ADD_FUNC_ARITY("getutxline", 1, false);
	ADD_FUNC_ARITY("pututxline", 1, false);
	ADD_FUNC_ARITY("setutxent", 0, false);

	// wchar.h
	ADD_FUNC_ARITY("__mbrlen", 3, false);
	ADD_FUNC_ARITY("btowc", 1, false);
	ADD_FUNC_ARITY("fgetwc", 1, false);
	ADD_FUNC_ARITY("fgetws", 3, false);
	ADD_FUNC_ARITY("fputwc", 2, false);
	ADD_FUNC_ARITY("fputws", 2, false);
	ADD_FUNC_ARITY("fwide", 2, false);
	ADD_FUNC_ARITY("fwprintf", 2, true);
	ADD_FUNC_ARITY("fwscanf", 2, true);
	ADD_FUNC_ARITY("getwc", 1, false);
	ADD_FUNC_ARITY("getwchar", 0, false);
	ADD_FUNC_ARITY("mbrlen", 3, false);
	ADD_FUNC_ARITY("mbrtowc", 4, false);
	ADD_FUNC_ARITY("mbsinit", 1, false);
	ADD_FUNC_ARITY("mbsrtowcs", 4, false);
	ADD_FUNC_ARITY("putwc", 2, false);
	ADD_FUNC_ARITY("putwchar", 1, false);
	ADD_FUNC_ARITY("swprintf", 3, true);
	ADD_FUNC_ARITY("swscanf", 2, true);
	ADD_FUNC_ARITY("ungetwc", 2, false);
	ADD_FUNC_ARITY("vfwprintf", 3, false);
	ADD_FUNC_ARITY("vfwscanf", 3, false);
	ADD_FUNC_ARITY("vswprintf", 4, false);
	ADD_FUNC_ARITY("vswscanf", 3, false);
	ADD_FUNC_ARITY("vwprintf", 2, false);
	ADD_FUNC_ARITY("vwscanf", 2, false);
	ADD_FUNC_ARITY("wcrtomb", 3, false);
	ADD_FUNC_ARITY("wcscat", 2, false);
	ADD_FUNC_ARITY("wcschr", 2, false);
	ADD_FUNC_ARITY("wcscmp", 2, false);
	ADD_FUNC_ARITY("wcscoll", 2, false);
	ADD_FUNC_ARITY("wcscpy", 2, false);
	ADD_FUNC_ARITY("wcscspn", 2, false);
	ADD_FUNC_ARITY("wcsftime", 4, false);
	ADD_FUNC_ARITY("wcslen", 1, false);
	ADD_FUNC_ARITY("wcsncat", 3, false);
	ADD_FUNC_ARITY("wcsncmp", 3, false);
	ADD_FUNC_ARITY("wcsncpy", 3, false);
	ADD_FUNC_ARITY("wcspbrk", 2, false);
	ADD_FUNC_ARITY("wcsrchr", 2, false);
	ADD_FUNC_ARITY("wcsrtombs", 4, false);
	ADD_FUNC_ARITY("wcsspn", 2, false);
	ADD_FUNC_ARITY("wcsstr", 2, false);
	ADD_FUNC_ARITY("wcstod", 2, false);
	ADD_FUNC_ARITY("wcstof", 2, false);
	ADD_FUNC_ARITY("wcstok", 3, false);
	ADD_FUNC_ARITY("wcstol", 3, false);
	ADD_FUNC_ARITY("wcstold", 2, false);
	ADD_FUNC_ARITY("wcstoll", 3, false);
	ADD_FUNC_ARITY("wcstoul", 3, false);
	ADD_FUNC_ARITY("wcstoull", 3, false);
	ADD_FUNC_ARITY("wcsxfrm", 3, false);
	ADD_FUNC_ARITY("wctob", 1, false);
	ADD_FUNC_ARITY("wmemchr", 3, false);
	ADD_FUNC_ARITY("wmemcmp", 3, false);
	ADD_FUNC_ARITY("wmemcpy", 3, false);
	ADD_FUNC_ARITY("wmemmove", 3, false);
	ADD_FUNC_ARITY("wmemset", 3, false);
	ADD_FUNC_ARITY("wprintf", 1, true);
	ADD_FUNC_ARITY("wscanf", 1, true);

	// wctype.h
	ADD_FUNC_ARITY("iswalnum", 1, false);
	ADD_FUNC_ARITY("iswalpha", 1, false);
	ADD_FUNC_ARITY("iswblank", 1, false);
	ADD_FUNC_ARITY("iswcntrl", 1, false);
	ADD_FUNC_ARITY("iswctype", 2, false);
	ADD_FUNC_ARITY("iswdigit", 1, false);
	ADD_FUNC_ARITY("iswgraph", 1, false);
	ADD_FUNC_ARITY("iswlower", 1, false);
	ADD_FUNC_ARITY("iswprint", 1, false);
	ADD_FUNC_ARITY("iswpunct", 1, false);
	ADD_FUNC_ARITY("iswspace", 1, false);
	ADD_FUNC_ARITY("iswupper", 1, false);
	ADD_FUNC_ARITY("iswxdigit", 1, false);
	ADD_FUNC_ARITY("towctrans", 2, false);
	ADD_FUNC_ARITY("towlower", 1, false);
	ADD_FUNC_ARITY("towupper", 1, false);
	ADD_FUNC_ARITY("wctrans", 1, false);
	ADD_FUNC_ARITY("wctype", 1, false);

	// wordexp.h
	ADD_FUNC_ARITY("wordexp", 3, false);
	ADD_FUNC_ARITY("wordfree", 1, false);
	return m;
}

#undef ADD_FUNC_ARITY

/// Mapping of function names to their declared arity.
const FuncArityMap& FUNC_ARITY_MAP = initFuncArityMap();

} // anonymous namespace

std::optional<FuncArity> getArityOfFunc(const std::string& funcName)
{
	auto i = FUNC_ARITY_MAP.find(funcName);
	return i != FUNC_ARITY_MAP.end() ? std::optional<FuncArity>(i->second) : std::nullopt;
}

} // namespace gcc_general
} // namespace semantics
} // namespace llvmir2hll
} // namespace retdec
