/* Small shared library for tests/test_os_dynlib.cpp (os::dynlib). Built natively as forge_fixture and, when clang
   and lld are available, cross-linked as a PE DLL and Mach-O dylibs so the file parsers are checked on every host. */
#if defined(_WIN32)
#define FIXTURE_API __declspec(dllexport)
#define FIXTURE_HIDDEN
#else
#define FIXTURE_API __attribute__((visibility("default")))
#define FIXTURE_HIDDEN __attribute__((visibility("hidden")))
#endif
FIXTURE_API int forge_fixture_counter = 42;
FIXTURE_HIDDEN int forge_fixture_hidden(int x) { return x * 3; }
static int forge_fixture_static(int x) { return x - 1; }
FIXTURE_API int forge_fixture_add(int a, int b) { return forge_fixture_hidden(a) - 2 * a + forge_fixture_static(b) + 1; }
FIXTURE_API const char* forge_fixture_name(void) { return "forge-fixture"; }

#if defined(FIXTURE_ELF_EXTRAS) /* cross-linked ELF fixture only (clang + ld.lld, no libc) */
/* Symbol versions: forge_fixture_add@@FIXV_2 is the default, forge_fixture_add@FIXV_1 the hidden old one. */
FIXTURE_API int forge_fixture_add_v1(int a, int b) { return a - b; }
__asm__(".symver forge_fixture_add_v1,forge_fixture_add@FIXV_1");
/* GNU indirect function: the loader calls the resolver and binds the result. */
static int forge_fixture_twice_impl(int x) { return 2 * x; }
static int (*forge_fixture_twice_resolve(void))(int) { return forge_fixture_twice_impl; }
FIXTURE_API int forge_fixture_twice(int x) __attribute__((ifunc("forge_fixture_twice_resolve")));
/* Thread-local export. */
FIXTURE_API __thread int forge_fixture_tls = 7;
#endif
