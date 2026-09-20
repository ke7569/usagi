// AVX512 matrix kernels. Compile this translation unit with GCC >= 7.
// Scalar ABI only: the rest of the application can keep its original compiler.
#include "sse/model/v06_matrix_kernels.h"
#include <cpuid.h>
#include <immintrin.h>
namespace sse_v06 { namespace matrix {
bool avx512_available() {
    static const bool supported=[](){
        unsigned a,b,c,d; if(!__get_cpuid(1,&a,&b,&c,&d) || !(c&(1U<<27)) || !(c&(1U<<12)))return false;
        unsigned lo,hi;asm volatile("xgetbv":"=a"(lo),"=d"(hi):"c"(0));
        if((lo&0xe6)!=0xe6 || __get_cpuid_max(0,0)<7)return false;
        __cpuid_count(7,0,a,b,c,d);return bool(b&(1U<<16));
    }();return supported;
}
__attribute__((target("avx512f,fma")))
void linear_gru_avx512(const float* w,const float* bias,const float* x,float* y) {
  for(unsigned r=0;r<384;r+=96) {
    __m512 a0_0=_mm512_setzero_ps();
    __m512 a0_1=_mm512_setzero_ps();
    __m512 a0_2=_mm512_setzero_ps();
    __m512 a0_3=_mm512_setzero_ps();
    __m512 a1_0=_mm512_setzero_ps();
    __m512 a1_1=_mm512_setzero_ps();
    __m512 a1_2=_mm512_setzero_ps();
    __m512 a1_3=_mm512_setzero_ps();
    __m512 a2_0=_mm512_setzero_ps();
    __m512 a2_1=_mm512_setzero_ps();
    __m512 a2_2=_mm512_setzero_ps();
    __m512 a2_3=_mm512_setzero_ps();
    __m512 a3_0=_mm512_setzero_ps();
    __m512 a3_1=_mm512_setzero_ps();
    __m512 a3_2=_mm512_setzero_ps();
    __m512 a3_3=_mm512_setzero_ps();
    __m512 a4_0=_mm512_setzero_ps();
    __m512 a4_1=_mm512_setzero_ps();
    __m512 a4_2=_mm512_setzero_ps();
    __m512 a4_3=_mm512_setzero_ps();
    __m512 a5_0=_mm512_setzero_ps();
    __m512 a5_1=_mm512_setzero_ps();
    __m512 a5_2=_mm512_setzero_ps();
    __m512 a5_3=_mm512_setzero_ps();
    for(unsigned j=0;j<128;j+=4) {
      const __m512 x0=_mm512_set1_ps(x[j+0]);
      a0_0=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+0)*128+(j+0)*16),x0,a0_0);
      a1_0=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+16)*128+(j+0)*16),x0,a1_0);
      a2_0=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+32)*128+(j+0)*16),x0,a2_0);
      a3_0=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+48)*128+(j+0)*16),x0,a3_0);
      a4_0=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+64)*128+(j+0)*16),x0,a4_0);
      a5_0=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+80)*128+(j+0)*16),x0,a5_0);
      const __m512 x1=_mm512_set1_ps(x[j+1]);
      a0_1=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+0)*128+(j+1)*16),x1,a0_1);
      a1_1=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+16)*128+(j+1)*16),x1,a1_1);
      a2_1=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+32)*128+(j+1)*16),x1,a2_1);
      a3_1=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+48)*128+(j+1)*16),x1,a3_1);
      a4_1=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+64)*128+(j+1)*16),x1,a4_1);
      a5_1=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+80)*128+(j+1)*16),x1,a5_1);
      const __m512 x2=_mm512_set1_ps(x[j+2]);
      a0_2=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+0)*128+(j+2)*16),x2,a0_2);
      a1_2=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+16)*128+(j+2)*16),x2,a1_2);
      a2_2=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+32)*128+(j+2)*16),x2,a2_2);
      a3_2=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+48)*128+(j+2)*16),x2,a3_2);
      a4_2=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+64)*128+(j+2)*16),x2,a4_2);
      a5_2=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+80)*128+(j+2)*16),x2,a5_2);
      const __m512 x3=_mm512_set1_ps(x[j+3]);
      a0_3=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+0)*128+(j+3)*16),x3,a0_3);
      a1_3=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+16)*128+(j+3)*16),x3,a1_3);
      a2_3=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+32)*128+(j+3)*16),x3,a2_3);
      a3_3=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+48)*128+(j+3)*16),x3,a3_3);
      a4_3=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+64)*128+(j+3)*16),x3,a4_3);
      a5_3=_mm512_fmadd_ps(_mm512_loadu_ps(w+(r+80)*128+(j+3)*16),x3,a5_3);
    }
    _mm512_storeu_ps(y+r+0,_mm512_add_ps(_mm512_add_ps(_mm512_add_ps(_mm512_add_ps(a0_0,a0_1),a0_2),a0_3),_mm512_loadu_ps(bias+r+0)));
    _mm512_storeu_ps(y+r+16,_mm512_add_ps(_mm512_add_ps(_mm512_add_ps(_mm512_add_ps(a1_0,a1_1),a1_2),a1_3),_mm512_loadu_ps(bias+r+16)));
    _mm512_storeu_ps(y+r+32,_mm512_add_ps(_mm512_add_ps(_mm512_add_ps(_mm512_add_ps(a2_0,a2_1),a2_2),a2_3),_mm512_loadu_ps(bias+r+32)));
    _mm512_storeu_ps(y+r+48,_mm512_add_ps(_mm512_add_ps(_mm512_add_ps(_mm512_add_ps(a3_0,a3_1),a3_2),a3_3),_mm512_loadu_ps(bias+r+48)));
    _mm512_storeu_ps(y+r+64,_mm512_add_ps(_mm512_add_ps(_mm512_add_ps(_mm512_add_ps(a4_0,a4_1),a4_2),a4_3),_mm512_loadu_ps(bias+r+64)));
    _mm512_storeu_ps(y+r+80,_mm512_add_ps(_mm512_add_ps(_mm512_add_ps(_mm512_add_ps(a5_0,a5_1),a5_2),a5_3),_mm512_loadu_ps(bias+r+80)));
  }
}
__attribute__((target("avx512f,fma")))
void linear_projection_avx512(const float* w,const float* bias,const float* x,float* y) {
    for(unsigned row=0;row<128;row+=32) {
        __m512 a0=_mm512_setzero_ps(),a1=a0,a2=a0,a3=a0,b0=a0,b1=a0,b2=a0,b3=a0;
        for(unsigned j=0;j<48;j+=4) {
            const __m512 x0=_mm512_set1_ps(x[j]),x1=_mm512_set1_ps(x[j+1]),x2=_mm512_set1_ps(x[j+2]),x3=_mm512_set1_ps(x[j+3]);
            a0=_mm512_fmadd_ps(_mm512_loadu_ps(w+row*50+j*16),x0,a0);
            a1=_mm512_fmadd_ps(_mm512_loadu_ps(w+row*50+(j+1)*16),x1,a1);
            a2=_mm512_fmadd_ps(_mm512_loadu_ps(w+row*50+(j+2)*16),x2,a2);
            a3=_mm512_fmadd_ps(_mm512_loadu_ps(w+row*50+(j+3)*16),x3,a3);
            b0=_mm512_fmadd_ps(_mm512_loadu_ps(w+(row+16)*50+j*16),x0,b0);
            b1=_mm512_fmadd_ps(_mm512_loadu_ps(w+(row+16)*50+(j+1)*16),x1,b1);
            b2=_mm512_fmadd_ps(_mm512_loadu_ps(w+(row+16)*50+(j+2)*16),x2,b2);
            b3=_mm512_fmadd_ps(_mm512_loadu_ps(w+(row+16)*50+(j+3)*16),x3,b3);
        }
        __m512 a=_mm512_add_ps(_mm512_add_ps(a0,a1),_mm512_add_ps(a2,a3));
        __m512 b=_mm512_add_ps(_mm512_add_ps(b0,b1),_mm512_add_ps(b2,b3));
        for(unsigned j=48;j<50;++j){const __m512 xx=_mm512_set1_ps(x[j]);a=_mm512_fmadd_ps(_mm512_loadu_ps(w+row*50+j*16),xx,a);b=_mm512_fmadd_ps(_mm512_loadu_ps(w+(row+16)*50+j*16),xx,b);}
        _mm512_storeu_ps(y+row,_mm512_add_ps(a,_mm512_loadu_ps(bias+row)));
        _mm512_storeu_ps(y+row+16,_mm512_add_ps(b,_mm512_loadu_ps(bias+row+16)));
    }
}

}}
