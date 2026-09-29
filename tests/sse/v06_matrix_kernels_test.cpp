#include "sse/model/v06_matrix_kernels.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <vector>
#include <stdexcept>
int main(){try{
 if(!sse_v06::matrix::avx512_available()){std::cout<<"AVX512 unavailable: SKIP\n";return 0;}
 std::mt19937 rng(29);std::uniform_real_distribution<float> random(-1,1);double largest=0;
 for(unsigned shape=0;shape<2;++shape){const unsigned rows=shape?128:384,cols=shape?50:128;
  std::vector<float>original(rows*cols),packed(rows*cols),x(cols),bias(rows),y(rows+32,999);
  for(unsigned round=0;round<100;++round){
   for(auto& v:original)v=random(rng);for(auto&v:x)v=random(rng);for(auto&v:bias)v=random(rng);
   for(unsigned r=0;r<rows;r+=16)for(unsigned c=0;c<cols;++c)for(unsigned k=0;k<16;++k)packed[r*cols+c*16+k]=original[(r+k)*cols+c];
   if(shape)sse_v06::matrix::linear_projection_avx512(packed.data(),bias.data(),x.data(),y.data()+16);
   else sse_v06::matrix::linear_gru_avx512(packed.data(),bias.data(),x.data(),y.data()+16);
   for(unsigned r=0;r<rows;++r){double expected=bias[r];for(unsigned c=0;c<cols;++c)expected+=double(original[r*cols+c])*x[c];const double delta=std::abs(expected-y[r+16]);largest=std::max(largest,delta);if(delta>2e-5)throw std::runtime_error("matrix mismatch");}
   for(unsigned k=0;k<16;++k)if(y[k]!=999||y[rows+16+k]!=999)throw std::runtime_error("matrix output bounds");
  }
 }
 std::cout<<"AVX512 executed: 200 random matrices, both shapes and output bounds PASS; max_absolute_error="<<largest<<'\n';
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
