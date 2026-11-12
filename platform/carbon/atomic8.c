/*
 * atomic8.c - the 64-bit __atomic helpers GCC calls on 32-bit PowerPC (no 8-byte lwarx/stwcx.), for miniaudio's
 * decoder-side code. In the classic Mac OS / Carbon build every miniaudio call is on the main thread (the audio
 * interrupt only copies sdl_core.c's ring), so plain loads and stores are correct here; do not reuse elsewhere.
 */
#include <stdint.h>
uint64_t __atomic_load_8(const volatile void *p, int m) { (void)m; return *(const volatile uint64_t *)p; }
void __atomic_store_8(volatile void *p, uint64_t v, int m) { (void)m; *(volatile uint64_t *)p = v; }
uint64_t __atomic_exchange_8(volatile void *p, uint64_t v, int m) { uint64_t o = *(volatile uint64_t *)p; (void)m; *(volatile uint64_t *)p = v; return o; }
uint64_t __atomic_fetch_add_8(volatile void *p, uint64_t v, int m) { uint64_t o = *(volatile uint64_t *)p; (void)m; *(volatile uint64_t *)p = o + v; return o; }
uint64_t __atomic_fetch_sub_8(volatile void *p, uint64_t v, int m) { uint64_t o = *(volatile uint64_t *)p; (void)m; *(volatile uint64_t *)p = o - v; return o; }
_Bool __atomic_compare_exchange_8(volatile void *p, void *expected, uint64_t desired, _Bool weak, int s, int f)
{
    uint64_t *e = (uint64_t *)expected;
    (void)weak; (void)s; (void)f;
    if (*(volatile uint64_t *)p == *e) { *(volatile uint64_t *)p = desired; return 1; }
    *e = *(volatile uint64_t *)p;
    return 0;
}
