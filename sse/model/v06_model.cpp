#include "sse/model/v06_model.h"
#ifdef SSE_V06_AVX512_KERNELS
#include "sse/model/v06_matrix_kernels.h"
#endif
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include "third_party/eigen3/Eigen/Core"
#if defined(__x86_64__)
#include <immintrin.h>
#include <cpuid.h>
#endif

namespace sse_v06 {
namespace {
#if defined(__x86_64__)
bool has_fma() {
    static const bool supported=[](){unsigned a,b,c,d;return __get_cpuid(1,&a,&b,&c,&d) && (c & bit_FMA); }();
    return supported;
}
#endif
const unsigned sizes[14] = {50,50,6400,128,49152,49152,384,384,49152,49152,384,384,512,4};
const int order[50] = {2,3,7,8,9,10,11,16,17,18,19,20,0,1,4,5,6,12,13,14,15,
    28,29,30,31,40,41,37,38,43,44,32,33,34,35,36,39,42,48,45,46,47,49,21,22,23,24,25,26,27};
const char* names[50] = {
"factor_spread_permille","factor_mid_return_permille","factor_weighted_return_permille_1",
"factor_weighted_return_permille_2","factor_weighted_return_permille_3","factor_weighted_return_permille_4",
"factor_weighted_return_permille_5","factor_weighted_volume_imbalance","factor_volume_imbalance",
"factor_percent_turnover","factor_liquidity_ask_l1_share","factor_liquidity_bid_l1_share",
"factor_hermes_permille","factor_tr_sqrt_positive","factor_fee_on_tick","factor_bid_volume_change_ratio",
"factor_ask_volume_change_ratio","factor_weighted_ask_permille","factor_weighted_bid_permille",
"factor_weighted_ask_return_permille","factor_weighted_bid_return_permille","factor_positive_fill_rate",
"factor_negative_fill_rate","factor_order_flow_imbalance","factor_cfr_imbalance",
"factor_book_count_imbalance_l1","factor_book_count_imbalance_l5","factor_book_avg_size_imbalance_l1",
"factor_book_avg_size_imbalance_l5","factor_book_life_imbalance_l1","factor_book_life_imbalance_l5",
"factor_book_fixdist_imbalance_1pct","factor_book_fixdist_imbalance_5pct","factor_book_fixdist_weighted_1pct",
"factor_book_fixdist_weighted_5pct","factor_book_avg_size_imbalance","factor_book_count_imbalance",
"factor_book_life_imbalance","factor_book_young_imbalance_1pct","factor_max_bid_distance_ratio",
"factor_max_ask_distance_ratio","factor_max_vol_distance_imbalance","factor_book_fixdist_hermes",
"factor_positive_order_flow_log1p","factor_negative_order_flow_log1p","factor_market_flow_asinh",
"factor_cancel_buy_flow_log1p","factor_cancel_sell_flow_log1p","factor_positive_trade_log1p","factor_negative_trade_log1p"};
#if defined(__x86_64__)
__attribute__((target("avx2")))
void linear_avx(const float* w,const float* bias,const float* x,unsigned rows,unsigned cols,float* y) {
    unsigned r=0;
    for(;r+3<rows;r+=4) {
        __m256 a0=_mm256_setzero_ps(),b0=a0,a1=a0,b1=a0,a2=a0,b2=a0,a3=a0,b3=a0;
        unsigned j=0;
        for(;j+15<cols;j+=16){
            const auto x0=_mm256_loadu_ps(x+j),x1=_mm256_loadu_ps(x+j+8);
            a0=_mm256_add_ps(a0,_mm256_mul_ps(_mm256_loadu_ps(w+r*cols+j),x0));
            b0=_mm256_add_ps(b0,_mm256_mul_ps(_mm256_loadu_ps(w+r*cols+j+8),x1));
            a1=_mm256_add_ps(a1,_mm256_mul_ps(_mm256_loadu_ps(w+(r+1)*cols+j),x0));
            b1=_mm256_add_ps(b1,_mm256_mul_ps(_mm256_loadu_ps(w+(r+1)*cols+j+8),x1));
            a2=_mm256_add_ps(a2,_mm256_mul_ps(_mm256_loadu_ps(w+(r+2)*cols+j),x0));
            b2=_mm256_add_ps(b2,_mm256_mul_ps(_mm256_loadu_ps(w+(r+2)*cols+j+8),x1));
            a3=_mm256_add_ps(a3,_mm256_mul_ps(_mm256_loadu_ps(w+(r+3)*cols+j),x0));
            b3=_mm256_add_ps(b3,_mm256_mul_ps(_mm256_loadu_ps(w+(r+3)*cols+j+8),x1));
        }
        float lanes[4][8];
        _mm256_storeu_ps(lanes[0],_mm256_add_ps(a0,b0));_mm256_storeu_ps(lanes[1],_mm256_add_ps(a1,b1));
        _mm256_storeu_ps(lanes[2],_mm256_add_ps(a2,b2));_mm256_storeu_ps(lanes[3],_mm256_add_ps(a3,b3));
        for(unsigned k=0;k<4;++k){float value=0;for(float v:lanes[k])value+=v;for(unsigned q=j;q<cols;++q)value+=w[(r+k)*cols+q]*x[q];y[r+k]=value+bias[r+k];}
    }
    for(;r<rows;++r) {
        __m256 a=_mm256_setzero_ps(),b=_mm256_setzero_ps();unsigned j=0;
        for(;j+15<cols;j+=16){
            a=_mm256_add_ps(a,_mm256_mul_ps(_mm256_loadu_ps(w+r*cols+j),_mm256_loadu_ps(x+j)));
            b=_mm256_add_ps(b,_mm256_mul_ps(_mm256_loadu_ps(w+r*cols+j+8),_mm256_loadu_ps(x+j+8)));
        }
        float lanes[8];_mm256_storeu_ps(lanes,_mm256_add_ps(a,b));
        float value=0;for(float v:lanes)value+=v;
        for(;j<cols;++j)value+=w[r*cols+j]*x[j];y[r]=value+bias[r];
    }
}
__attribute__((target("avx2")))
// Packed four-row tiles retain the original 16-lane accumulation order.
void linear_gru_packed(const float* w,const float* bias,const float* x,float* y) {
    unsigned r=0;
    for(;r+3<384;r+=4) {
        __m256 a0=_mm256_setzero_ps(),b0=a0,a1=a0,b1=a0,a2=a0,b2=a0,a3=a0,b3=a0;
        unsigned j=0;
        for(;j+15<128;j+=16){
            const auto x0=_mm256_loadu_ps(x+j),x1=_mm256_loadu_ps(x+j+8);
            a0=_mm256_add_ps(a0,_mm256_mul_ps(_mm256_loadu_ps(w+r*128+j*4+0),x0));
            b0=_mm256_add_ps(b0,_mm256_mul_ps(_mm256_loadu_ps(w+r*128+j*4+8),x1));
            a1=_mm256_add_ps(a1,_mm256_mul_ps(_mm256_loadu_ps(w+r*128+j*4+16),x0));
            b1=_mm256_add_ps(b1,_mm256_mul_ps(_mm256_loadu_ps(w+r*128+j*4+24),x1));
            a2=_mm256_add_ps(a2,_mm256_mul_ps(_mm256_loadu_ps(w+r*128+j*4+32),x0));
            b2=_mm256_add_ps(b2,_mm256_mul_ps(_mm256_loadu_ps(w+r*128+j*4+40),x1));
            a3=_mm256_add_ps(a3,_mm256_mul_ps(_mm256_loadu_ps(w+r*128+j*4+48),x0));
            b3=_mm256_add_ps(b3,_mm256_mul_ps(_mm256_loadu_ps(w+r*128+j*4+56),x1));
        }
        float lanes[4][8];
        _mm256_storeu_ps(lanes[0],_mm256_add_ps(a0,b0));_mm256_storeu_ps(lanes[1],_mm256_add_ps(a1,b1));
        _mm256_storeu_ps(lanes[2],_mm256_add_ps(a2,b2));_mm256_storeu_ps(lanes[3],_mm256_add_ps(a3,b3));
        for(unsigned k=0;k<4;++k){float value=0;for(float v:lanes[k])value+=v;y[r+k]=value+bias[r+k];}
    }
}
#if defined(__FMA__)
__attribute__((target("avx2,fma")))
void linear_gru_packed_fma(const float* w,const float* bias,const float* x,float* y) {
    unsigned r=0;
    for(;r+3<384;r+=4) {
        __m256 a0=_mm256_setzero_ps(),b0=a0,a1=a0,b1=a0,a2=a0,b2=a0,a3=a0,b3=a0;
        unsigned j=0;
        for(;j+15<128;j+=16){
            const auto x0=_mm256_loadu_ps(x+j),x1=_mm256_loadu_ps(x+j+8);
            a0=_mm256_fmadd_ps(_mm256_loadu_ps(w+r*128+j*4+0),x0,a0);
            b0=_mm256_fmadd_ps(_mm256_loadu_ps(w+r*128+j*4+8),x1,b0);
            a1=_mm256_fmadd_ps(_mm256_loadu_ps(w+r*128+j*4+16),x0,a1);
            b1=_mm256_fmadd_ps(_mm256_loadu_ps(w+r*128+j*4+24),x1,b1);
            a2=_mm256_fmadd_ps(_mm256_loadu_ps(w+r*128+j*4+32),x0,a2);
            b2=_mm256_fmadd_ps(_mm256_loadu_ps(w+r*128+j*4+40),x1,b2);
            a3=_mm256_fmadd_ps(_mm256_loadu_ps(w+r*128+j*4+48),x0,a3);
            b3=_mm256_fmadd_ps(_mm256_loadu_ps(w+r*128+j*4+56),x1,b3);
        }
        float lanes[4][8];
        _mm256_storeu_ps(lanes[0],_mm256_add_ps(a0,b0));_mm256_storeu_ps(lanes[1],_mm256_add_ps(a1,b1));
        _mm256_storeu_ps(lanes[2],_mm256_add_ps(a2,b2));_mm256_storeu_ps(lanes[3],_mm256_add_ps(a3,b3));
        for(unsigned k=0;k<4;++k){float value=0;for(float v:lanes[k])value+=v;y[r+k]=value+bias[r+k];}
    }
}
#endif
#endif
void linear(const std::vector<float>& w, const std::vector<float>& bias,
            const float* x, unsigned rows, unsigned cols, float* y) {
#ifdef SSE_V06_AVX512_KERNELS
    if(matrix::avx512_available()) {
        if(rows==384 && cols==128){matrix::linear_gru_avx512(w.data(),bias.data(),x,y);return;}
        if(rows==128 && cols==50){matrix::linear_projection_avx512(w.data(),bias.data(),x,y);return;}
    }
#endif
#if defined(__x86_64__)
    if(rows==384 && cols==128 && __builtin_cpu_supports("avx2")) {
#if defined(__FMA__)
        if(has_fma()){linear_gru_packed_fma(w.data(),bias.data(),x,y);return;}
#endif
        linear_gru_packed(w.data(),bias.data(),x,y);return;
    }
    static const bool avx=__builtin_cpu_supports("avx2");
    if(avx){linear_avx(w.data(),bias.data(),x,rows,cols,y);return;}
#endif
    for (unsigned r=0;r<rows;++r) {
        float a=0,b=0,c=0,d=0;
        unsigned j=0;
        for (;j+3<cols;j+=4) {
            a+=w[r*cols+j]*x[j]; b+=w[r*cols+j+1]*x[j+1];
            c+=w[r*cols+j+2]*x[j+2]; d+=w[r*cols+j+3]*x[j+3];
        }
        float v=(a+b)+(c+d);
        for (;j<cols;++j) v+=w[r*cols+j]*x[j];
        y[r]=v+bias[r];
    }
}
float sigmoid(float x) { return 1.0f/(1.0f+std::exp(-x)); }
}
const char* factor_name(std::size_t i) { return i<50 ? names[i] : ""; }
Factors from_legacy_factors(const Factors& input) {
    Factors out;
    for (unsigned i=0;i<50;++i) {
        double v=input[order[i]];
        if (i>=43) v=i==45 ? std::asinh(v) : std::log1p(std::max(0.0,v));
        out[i]=static_cast<float>(v);
    }
    return out;
}
int agreement(const Heads& h) {
    bool positive=true,negative=true;
    for (float v:h) {
        positive=positive && std::isfinite(v) && v>0;
        negative=negative && std::isfinite(v) && v<0;
    }
    return positive ? 1 : negative ? -1 : 0;
}
bool Model::load(const std::string& path, std::string* error) {
    ready_=false;
    try {
        std::ifstream f(path.c_str(),std::ios::binary);
        char magic[8]; f.read(magic,8);
        if (!f || std::memcmp(magic,"SSEV06M1",8)) throw std::runtime_error("invalid v06 artifact header");
        for (unsigned i=0;i<14;++i) {
            tensors_[i].resize(sizes[i]);
            f.read(reinterpret_cast<char*>(tensors_[i].data()),sizes[i]*sizeof(float));
            if (!f) throw std::runtime_error("truncated v06 weights");
            for (float v:tensors_[i]) if (!std::isfinite(v)) throw std::runtime_error("nonfinite v06 weights");
        }
        if (f.peek()!=std::char_traits<char>::eof()) throw std::runtime_error("trailing v06 artifact data");
#ifdef SSE_V06_AVX512_KERNELS
        if(matrix::avx512_available()) {
            for(unsigned k : {2U,4U,5U,8U,9U}) {
                const unsigned rows=k==2?128:384,cols=k==2?50:128;
                std::vector<float> packed(tensors_[k].size());
                for(unsigned r=0;r<rows;r+=16)
                    for(unsigned c=0;c<cols;++c)
                        for(unsigned lane=0;lane<16;++lane)
                            packed[r*cols+c*16+lane]=tensors_[k][(r+lane)*cols+c];
                tensors_[k].swap(packed);
            }
        } else
#endif
#if defined(__x86_64__)
        if(__builtin_cpu_supports("avx2")) {
            for(unsigned k : {4U,5U,8U,9U}) {
                std::vector<float> packed(tensors_[k].size());
                for(unsigned r=0;r<384;r+=4)
                    for(unsigned j=0;j<128;j+=16)
                        for(unsigned row=0;row<4;++row)
                            for(unsigned c=0;c<16;++c)
                                packed[r*128+j*4+row*16+c]=tensors_[k][(r+row)*128+j+c];
                tensors_[k].swap(packed);
            }
        }
#endif
        ready_=true; return true;
    } catch (const std::exception& e) { if(error)*error=e.what(); return false; }
}
bool Model::predict(const Factors& input, State* state, Heads* heads, Stages* stages) const {
    if (!ready_ || !state || !heads) return false;
    double mean=0,var=0;
    for (float v:input) { if(!std::isfinite(v))return false; mean+=v; }
    mean/=50;
    for(float v:input)var+=(v-mean)*(v-mean);
    const float inv=static_cast<float>(1/std::sqrt(var/50+1e-5));
    Factors norm;
    for(unsigned i=0;i<50;++i)norm[i]=(input[i]-static_cast<float>(mean))*inv*tensors_[0][i]+tensors_[1][i];
    std::array<float,128> projected;
    linear(tensors_[2],tensors_[3],norm.data(),128,50,projected.data());
    State next=*state;
    const float* x=projected.data();
    for(unsigned layer=0;layer<2;++layer) {
        float a[384],b[384]; const unsigned k=4+layer*4;
        float* h=next.hidden.data()+128*layer;
        linear(tensors_[k],tensors_[k+2],x,384,128,a);
        linear(tensors_[k+1],tensors_[k+3],h,384,128,b);
#if defined(__AVX__) && defined(__FMA__)
        if(__builtin_cpu_supports("avx2") && has_fma()) {
        for(unsigned i=0;i<128;i+=8) {
            using namespace Eigen::internal;
            const Packet8f one=pset1<Packet8f>(1.0f);
            const Packet8f ar=padd(ploadu<Packet8f>(a+i),ploadu<Packet8f>(b+i));
            const Packet8f az=padd(ploadu<Packet8f>(a+128+i),ploadu<Packet8f>(b+128+i));
            const Packet8f r=pdiv(one,padd(one,pexp(pnegate(ar))));
            const Packet8f z=pdiv(one,padd(one,pexp(pnegate(az))));
            const Packet8f candidate=padd(ploadu<Packet8f>(a+256+i),pmul(r,ploadu<Packet8f>(b+256+i)));
            const Packet8f n=ptanh(candidate);
            const Packet8f next=padd(n,pmul(z,psub(ploadu<Packet8f>(h+i),n)));
            pstoreu(h+i,next);
        }
        for(unsigned i=0;i<128;++i)if(!std::isfinite(h[i]))return false;
        } else
#endif
        {
        for(unsigned i=0;i<128;++i) {
            const float r=sigmoid(a[i]+b[i]),z=sigmoid(a[128+i]+b[128+i]);
            const float n=std::tanh(a[256+i]+r*b[256+i]);
            h[i]=n+z*(h[i]-n);
            if(!std::isfinite(h[i]))return false;
        }
        }
        x=h;
    }
    linear(tensors_[12],tensors_[13],x,4,128,heads->data());
    for(float v:*heads)if(!std::isfinite(v))return false;
    ++next.rows; *state=next;
    if(stages){stages->normalized=norm;stages->projected=projected;}
    return true;
}
}
