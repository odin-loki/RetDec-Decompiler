/**
 * @file tests/decompiler/msvc_fixture_printf.c
 * @brief A printf the MSVC corpus PEs can link without the UCRT.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * /EXPORT:main kept the CRT. The decoder then never roots on the user
 * program: ctest-windows corpus_fib.c was 581 KiB of api-ms-win-*
 * UTF-16 arrays and contained no `printf`. /ENTRY:main /NODEFAULTLIB
 * needs a definition for the printf the fixtures call.
 */
int __cdecl printf(const char* fmt, ...)
{
	(void)fmt;
	return 0;
}
