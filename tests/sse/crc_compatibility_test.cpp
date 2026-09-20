#include "common/recovery/SZERecoverable.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include <iostream>
std::uint32_t reference(const unsigned char* p,std::size_t n) {
    std::uint32_t crc=~0U;
    while(n--){crc^=*p++;for(unsigned k=0;k<8;++k)crc=(crc>>1)^((crc&1)?0xedb88320U:0);}
    return ~crc;
}
int main(){
    assert(sze_recovery::crc32(0,0)==0);
    assert(sze_recovery::crc32("123456789",9)==0xcbf43926U);
    std::vector<unsigned char> bytes(16400);std::uint32_t seed=7;
    for(auto& b:bytes){seed=seed*1664525U+1013904223U;b=seed>>24;}
    for(unsigned offset=0;offset<16;++offset)
        for(unsigned n=0;n<=16384;n+=(n<1024?1:37))
            assert(sze_recovery::crc32(bytes.data()+offset,n)==reference(bytes.data()+offset,n));
    std::cout<<"CRC IEEE compatibility: PASS (unaligned, empty, tails, large payloads)\n";
}
