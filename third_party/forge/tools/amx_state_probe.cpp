// Detects hosts (e.g. some VMs) that lose AMX tile registers across context switches.
// Build: c++ -O2 -mamx-tile tools/amx_state_probe.cpp. Any "corrupt" > 0 means AMX results on this host are unreliable.
#include <immintrin.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include <sys/syscall.h>
struct alignas(64) cfg{uint8_t p,s,r[14];uint16_t c[16];uint8_t rows[16];};
int main(){syscall(SYS_arch_prctl,0x1023,18);cfg c{};c.p=1;c.c[0]=64;c.rows[0]=16;_tile_loadconfig(&c);
alignas(64) uint8_t src[1024],dst[1024];for(int i=0;i<1024;i++)src[i]=i*7+1;int bad=0;
for(int it=0;it<3000;it++){_tile_loadd(0,src,64);volatile double x=0;for(int k=0;k<20000;k++)x=x+k;memset(dst,0,1024);asm volatile("":::"memory");_tile_stored(0,dst,64);asm volatile("":::"memory");if(memcmp(src,dst,1024))bad++;}
printf("corrupt %d/3000\n",bad);}
