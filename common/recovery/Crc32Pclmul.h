// IEEE reflected CRC32 folding adapted from Chromium crc32_simd.c.
// Copyright 2017 The Chromium Authors. All rights reserved.
// BSD license and pinned upstream source: see LICENSE.crc32-pclmul.
#ifndef USAGI_CRC32_PCLMUL_H
#define USAGI_CRC32_PCLMUL_H
#include <cstddef>
#include <cstdint>
#if defined(__x86_64__) && defined(__GNUC__)
#include <cpuid.h>
#include <emmintrin.h>
// GCC 4.8 intrinsic headers reject per-function target attributes. Use the
// compiler builtin inside the targeted function instead of global ISA flags.
#define USAGI_CLMUL(a,b,i) ((__m128i)__builtin_ia32_pclmulqdq128((__v2di)(a),(__v2di)(b),(i)))
namespace sze_recovery { namespace crc_detail {
inline bool pclmul_available() {
    static const bool supported=[](){unsigned a,b,c,d;
        return __get_cpuid(1,&a,&b,&c,&d) && (c&(1U<<1)) && (c&(1U<<19));}();
    return supported;
}
// Input length >=64 and divisible by 16. Uncomplemented running CRC in/out.
__attribute__((target("pclmul,sse4.1")))
static std::uint32_t fold(const unsigned char* buf,std::size_t len,std::uint32_t crc) {
    static const std::uint64_t k1k2[2] __attribute__((aligned(16)))={0x0154442bd4ULL,0x01c6e41596ULL};
    static const std::uint64_t k3k4[2] __attribute__((aligned(16)))={0x01751997d0ULL,0x00ccaa009eULL};
    static const std::uint64_t k5k0[2] __attribute__((aligned(16)))={0x0163cd6124ULL,0};
    static const std::uint64_t poly[2] __attribute__((aligned(16)))={0x01db710641ULL,0x01f7011641ULL};
    __m128i x1=_mm_loadu_si128(reinterpret_cast<const __m128i*>(buf));
    __m128i x2=_mm_loadu_si128(reinterpret_cast<const __m128i*>(buf+16));
    __m128i x3=_mm_loadu_si128(reinterpret_cast<const __m128i*>(buf+32));
    __m128i x4=_mm_loadu_si128(reinterpret_cast<const __m128i*>(buf+48));
    x1=_mm_xor_si128(x1,_mm_cvtsi32_si128(crc));
    __m128i k=_mm_load_si128(reinterpret_cast<const __m128i*>(k1k2));
    buf+=64;len-=64;
    while(len>=64) {
        __m128i a=USAGI_CLMUL(x1,k,0x00),b=USAGI_CLMUL(x2,k,0x00);
        __m128i c=USAGI_CLMUL(x3,k,0x00),d=USAGI_CLMUL(x4,k,0x00);
        x1=_mm_xor_si128(USAGI_CLMUL(x1,k,0x11),a);
        x2=_mm_xor_si128(USAGI_CLMUL(x2,k,0x11),b);
        x3=_mm_xor_si128(USAGI_CLMUL(x3,k,0x11),c);
        x4=_mm_xor_si128(USAGI_CLMUL(x4,k,0x11),d);
        x1=_mm_xor_si128(x1,_mm_loadu_si128(reinterpret_cast<const __m128i*>(buf)));
        x2=_mm_xor_si128(x2,_mm_loadu_si128(reinterpret_cast<const __m128i*>(buf+16)));
        x3=_mm_xor_si128(x3,_mm_loadu_si128(reinterpret_cast<const __m128i*>(buf+32)));
        x4=_mm_xor_si128(x4,_mm_loadu_si128(reinterpret_cast<const __m128i*>(buf+48)));
        buf+=64;len-=64;
    }
    k=_mm_load_si128(reinterpret_cast<const __m128i*>(k3k4));
    __m128i x5=USAGI_CLMUL(x1,k,0x00);
    x1=_mm_xor_si128(_mm_xor_si128(USAGI_CLMUL(x1,k,0x11),x2),x5);
    x5=USAGI_CLMUL(x1,k,0x00);
    x1=_mm_xor_si128(_mm_xor_si128(USAGI_CLMUL(x1,k,0x11),x3),x5);
    x5=USAGI_CLMUL(x1,k,0x00);
    x1=_mm_xor_si128(_mm_xor_si128(USAGI_CLMUL(x1,k,0x11),x4),x5);
    while(len>=16) {
        x2=_mm_loadu_si128(reinterpret_cast<const __m128i*>(buf));
        x5=USAGI_CLMUL(x1,k,0x00);
        x1=_mm_xor_si128(_mm_xor_si128(USAGI_CLMUL(x1,k,0x11),x2),x5);
        buf+=16;len-=16;
    }
    x2=USAGI_CLMUL(x1,k,0x10);
    x3=_mm_setr_epi32(~0,0,~0,0);
    x1=_mm_xor_si128(_mm_srli_si128(x1,8),x2);
    k=_mm_loadl_epi64(reinterpret_cast<const __m128i*>(k5k0));
    x2=_mm_srli_si128(x1,4);
    x1=USAGI_CLMUL(_mm_and_si128(x1,x3),k,0x00);
    x1=_mm_xor_si128(x1,x2);
    k=_mm_load_si128(reinterpret_cast<const __m128i*>(poly));
    x2=USAGI_CLMUL(_mm_and_si128(x1,x3),k,0x10);
    x2=USAGI_CLMUL(_mm_and_si128(x2,x3),k,0x00);
    return _mm_cvtsi128_si32(_mm_srli_si128(_mm_xor_si128(x1,x2),4));
}
}}
#undef USAGI_CLMUL
#endif
#endif
