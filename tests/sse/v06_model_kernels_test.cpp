#include "sse/model/v06_model.cpp"
#include <random>
#include <iostream>
#include <limits>
int main(){
#if !defined(__AVX__) || !defined(__FMA__)
 std::cout<<"SIMD kernel test skipped on this build\n";return 0;
#else
 if(!__builtin_cpu_supports("avx2") || !sse_v06::has_fma()){std::cout<<"SIMD kernel test skipped on this CPU\n";return 0;}

 std::mt19937 random(17);std::uniform_real_distribution<float>distribution(-3,3);
 std::vector<float>w(384*128),packed(w.size()),bias(384),x(128),expected(384),actual(384);
 float max_fma=0;
 for(unsigned round=0;round<30;++round){for(float&v:w)v=distribution(random);for(float&v:bias)v=distribution(random);for(float&v:x)v=distribution(random);
 for(unsigned r=0;r<384;r+=4)for(unsigned j=0;j<128;j+=16)for(unsigned k=0;k<4;++k)for(unsigned c=0;c<16;++c)packed[r*128+j*4+k*16+c]=w[(r+k)*128+j+c];
 sse_v06::linear_avx(w.data(),bias.data(),x.data(),384,128,expected.data());
 sse_v06::linear_gru_packed(packed.data(),bias.data(),x.data(),actual.data());
 if(std::memcmp(expected.data(),actual.data(),actual.size()*4))return 1;
 sse_v06::linear_gru_packed_fma(packed.data(),bias.data(),x.data(),actual.data());
 for(unsigned i=0;i<384;++i)max_fma=std::max(max_fma,std::abs(actual[i]-expected[i]));
 }
 using namespace Eigen::internal;double max_sigmoid=0,max_tanh=0;
 for(unsigned n=0;n<20000;++n){float input[8],sigmoid[8],tanhv[8];for(unsigned k=0;k<8;++k)input[k]=(int(n*8+k)-80000)*.001f;
 auto v=ploadu<Packet8f>(input);auto one=pset1<Packet8f>(1);pstoreu(sigmoid,pdiv(one,padd(one,pexp(pnegate(v)))));pstoreu(tanhv,ptanh(v));
 for(unsigned k=0;k<8;++k){max_sigmoid=std::max(max_sigmoid,std::abs(double(sigmoid[k])-1.0/(1.0+std::exp(-double(input[k])))));max_tanh=std::max(max_tanh,std::abs(double(tanhv[k])-std::tanh(double(input[k]))));}}
 if(max_sigmoid>3e-7||max_tanh>5e-7)return 2;
 std::cout<<"packed_exact=PASS random_matrices=30 max_fma_abs="<<max_fma<<" activation_points=160000 max_sigmoid_abs="<<max_sigmoid<<" max_tanh_abs="<<max_tanh<<"\n";
#endif
}
