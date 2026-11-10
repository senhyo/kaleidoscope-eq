// arm64 shim: _mm_getcsr/_mm_setcsr mapped onto FPCR. 0x8040 (FTZ|DAZ) -> FPCR.FZ (bit 24).
#pragma once
#if defined(__x86_64__) || defined(__i386__)
#include_next <xmmintrin.h>
#else
#include <stdint.h>
static inline unsigned _mm_getcsr(void) {
    uint64_t f; __asm__ volatile("mrs %0, fpcr" : "=r"(f));
    return (f & (1ull << 24)) ? 0x8040u : 0u;
}
static inline void _mm_setcsr(unsigned v) {
    uint64_t f; __asm__ volatile("mrs %0, fpcr" : "=r"(f));
    if (v & 0x8040u) f |= (1ull << 24); else f &= ~(1ull << 24);
    __asm__ volatile("msr fpcr, %0" :: "r"(f));
}
#endif
