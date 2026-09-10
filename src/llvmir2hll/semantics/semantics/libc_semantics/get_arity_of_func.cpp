/**
 * @file src/llvmir2hll/semantics/semantics/libc_semantics/get_arity_of_func.cpp
 * @brief Implementation of semantics::libc::getArityOfFunc() for LibcSemantics.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include "retdec/llvmir2hll/semantics/semantics/libc_semantics/get_arity_of_func.h"

namespace retdec {
namespace llvmir2hll {
namespace semantics {
namespace libc {

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

	// assert.h
	ADD_FUNC_ARITY("assert", 1, false);

	// complex.h
	ADD_FUNC_ARITY("cabs", 1, false);
	ADD_FUNC_ARITY("cabsf", 1, false);
	ADD_FUNC_ARITY("cabsl", 1, false);
	ADD_FUNC_ARITY("cacos", 1, false);
	ADD_FUNC_ARITY("cacosf", 1, false);
	ADD_FUNC_ARITY("cacosh", 1, false);
	ADD_FUNC_ARITY("cacoshf", 1, false);
	ADD_FUNC_ARITY("cacoshl", 1, false);
	ADD_FUNC_ARITY("cacosl", 1, false);
	ADD_FUNC_ARITY("carg", 1, false);
	ADD_FUNC_ARITY("cargf", 1, false);
	ADD_FUNC_ARITY("cargl", 1, false);
	ADD_FUNC_ARITY("casin", 1, false);
	ADD_FUNC_ARITY("casinf", 1, false);
	ADD_FUNC_ARITY("casinh", 1, false);
	ADD_FUNC_ARITY("casinhf", 1, false);
	ADD_FUNC_ARITY("casinhl", 1, false);
	ADD_FUNC_ARITY("casinl", 1, false);
	ADD_FUNC_ARITY("catan", 1, false);
	ADD_FUNC_ARITY("catanf", 1, false);
	ADD_FUNC_ARITY("catanh", 1, false);
	ADD_FUNC_ARITY("catanhf", 1, false);
	ADD_FUNC_ARITY("catanhl", 1, false);
	ADD_FUNC_ARITY("catanl", 1, false);
	ADD_FUNC_ARITY("ccos", 1, false);
	ADD_FUNC_ARITY("ccosf", 1, false);
	ADD_FUNC_ARITY("ccosh", 1, false);
	ADD_FUNC_ARITY("ccoshf", 1, false);
	ADD_FUNC_ARITY("ccoshl", 1, false);
	ADD_FUNC_ARITY("ccosl", 1, false);
	ADD_FUNC_ARITY("cexp", 1, false);
	ADD_FUNC_ARITY("cexpf", 1, false);
	ADD_FUNC_ARITY("cexpl", 1, false);
	ADD_FUNC_ARITY("cimag", 1, false);
	ADD_FUNC_ARITY("cimagf", 1, false);
	ADD_FUNC_ARITY("cimagl", 1, false);
	ADD_FUNC_ARITY("clog", 1, false);
	ADD_FUNC_ARITY("clogf", 1, false);
	ADD_FUNC_ARITY("clogl", 1, false);
	ADD_FUNC_ARITY("conj", 1, false);
	ADD_FUNC_ARITY("conjf", 1, false);
	ADD_FUNC_ARITY("conjl", 1, false);
	ADD_FUNC_ARITY("cpow", 2, false);
	ADD_FUNC_ARITY("cpowf", 2, false);
	ADD_FUNC_ARITY("cpowl", 2, false);
	ADD_FUNC_ARITY("cproj", 1, false);
	ADD_FUNC_ARITY("cprojf", 1, false);
	ADD_FUNC_ARITY("cprojl", 1, false);
	ADD_FUNC_ARITY("creal", 1, false);
	ADD_FUNC_ARITY("crealf", 1, false);
	ADD_FUNC_ARITY("creall", 1, false);
	ADD_FUNC_ARITY("csin", 1, false);
	ADD_FUNC_ARITY("csinf", 1, false);
	ADD_FUNC_ARITY("csinh", 1, false);
	ADD_FUNC_ARITY("csinhf", 1, false);
	ADD_FUNC_ARITY("csinhl", 1, false);
	ADD_FUNC_ARITY("csinl", 1, false);
	ADD_FUNC_ARITY("csqrt", 1, false);
	ADD_FUNC_ARITY("csqrtf", 1, false);
	ADD_FUNC_ARITY("csqrtl", 1, false);
	ADD_FUNC_ARITY("ctan", 1, false);
	ADD_FUNC_ARITY("ctanf", 1, false);
	ADD_FUNC_ARITY("ctanh", 1, false);
	ADD_FUNC_ARITY("ctanhf", 1, false);
	ADD_FUNC_ARITY("ctanhl", 1, false);
	ADD_FUNC_ARITY("ctanl", 1, false);

	// ctype.h
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

	// inttypes.h
	ADD_FUNC_ARITY("imaxabs", 1, false);
	ADD_FUNC_ARITY("imaxdiv", 2, false);

	// locale.h
	ADD_FUNC_ARITY("localeconv", 0, false);
	ADD_FUNC_ARITY("setlocale", 2, false);

	// math.h
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

	// setjmp.h
	ADD_FUNC_ARITY("longjmp", 2, false);
	ADD_FUNC_ARITY("setjmp", 1, false);

	// signal.h
	ADD_FUNC_ARITY("raise", 1, false);
	ADD_FUNC_ARITY("signal", 2, false);

	// stdarg.h
	ADD_FUNC_ARITY("va_copy", 2, false);
	ADD_FUNC_ARITY("va_end", 1, false);
	ADD_FUNC_ARITY("va_start", 2, false);

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
	ADD_FUNC_ARITY("abort", 0, false);
	ADD_FUNC_ARITY("abs", 1, false);
	ADD_FUNC_ARITY("at_quick_exit", 1, false);
	ADD_FUNC_ARITY("atexit", 1, false);
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
	ADD_FUNC_ARITY("qsort", 4, false);
	ADD_FUNC_ARITY("quick_exit", 1, false);
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

	// string.h
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

	// wchar.h
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

} // namespace libc
} // namespace semantics
} // namespace llvmir2hll
} // namespace retdec
