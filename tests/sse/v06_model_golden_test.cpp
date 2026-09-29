#include "sse/model/v06_model.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
std::vector<float> read(const std::string& p, unsigned n) {
    std::vector<float> x(n);std::ifstream f(p.c_str(),std::ios::binary);
    f.read(reinterpret_cast<char*>(x.data()),n*4);
    if(!f || f.peek()!=std::char_traits<char>::eof())throw std::runtime_error(p);
    return x;
}
void check(bool v,const char* s){if(!v)throw std::runtime_error(s);}
int main(int argc,char** argv) {
    if(argc!=2 && argc!=3)return 2; const std::string root=argv[1];
    sse_v06::Model model;std::string error;check(model.load(root+"/v06.bin",&error),error.c_str());
    const unsigned rows=4017;
    auto raw=read(argc==3?argv[2]:root+"/raw_input.f32",rows*50), norm=read(root+"/normalized_input.f32",rows*50);
    auto proj=read(root+"/projected_input.f32",rows*128),rec=read(root+"/recurrent_output.f32",rows*128);
    auto pred=read(root+"/prediction.f32",rows*4),hidden=read(root+"/final_hidden.f32",256);
    std::ifstream names(root+"/factors.txt");std::string name;
    for(unsigned i=0;i<50;++i){std::getline(names,name);check(name==sse_v06::factor_name(i),"factor order");}
    sse_v06::State state;float maxnorm=0,maxproj=0,maxrec=0,maxpred=0,maxhidden=0;unsigned disagreed=0;
    const auto start=std::chrono::steady_clock::now();
    for(unsigned row=0;row<rows;++row) {
        sse_v06::Factors x;std::copy(raw.begin()+row*50,raw.begin()+(row+1)*50,x.begin());
        sse_v06::Heads heads,expected;sse_v06::Stages stages;
        check(model.predict(x,&state,&heads,&stages),"predict");
        for(unsigned i=0;i<50;++i)maxnorm=std::max(maxnorm,std::fabs(stages.normalized[i]-norm[row*50+i]));
        for(unsigned i=0;i<128;++i){maxproj=std::max(maxproj,std::fabs(stages.projected[i]-proj[row*128+i]));
            maxrec=std::max(maxrec,std::fabs(state.hidden[128+i]-rec[row*128+i]));}
        for(unsigned i=0;i<4;++i){expected[i]=pred[row*4+i];maxpred=std::max(maxpred,std::fabs(heads[i]-expected[i]));}
        disagreed += sse_v06::agreement(heads)!=sse_v06::agreement(expected);
    }
    for(unsigned i=0;i<256;++i)maxhidden=std::max(maxhidden,std::fabs(state.hidden[i]-hidden[i]));
    const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
    std::cout<<"rows="<<rows<<" norm="<<maxnorm<<" proj="<<maxproj<<" recurrent="<<maxrec
        <<" prediction="<<maxpred<<" final_hidden="<<maxhidden<<" agreement_differences="<<disagreed
        <<" us_per_row="<<double(elapsed)/rows<<'\n';
    check(maxnorm<2e-5 && maxproj<2e-5 && maxrec<2e-5 && maxpred<2e-5 && maxhidden<2e-5 && !disagreed,"golden tolerance");
    sse_v06::Heads h={{1,2,3,4}};check(sse_v06::agreement(h)==1,"positive");
    h[3]=0;check(sse_v06::agreement(h)==0,"zero");h[3]=std::numeric_limits<float>::infinity();check(!sse_v06::agreement(h),"inf");
    h[3]=std::numeric_limits<float>::quiet_NaN();check(!sse_v06::agreement(h),"nan");
    state.reset();sse_v06::Factors invalid={};invalid[0]=h[3];check(!model.predict(invalid,&state,&h)&&state.rows==0,"invalid mutates state");
}
