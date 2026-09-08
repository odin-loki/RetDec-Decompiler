// ESBMC-OPTIONS: --unwind 3 --no-unwinding-assertions
#include <cassert>
void proof_zz_repro() {
	unsigned s = 0;
	for (unsigned i = 0; i < 8; ++i) { s += i; }
	assert(s == 28);
}
