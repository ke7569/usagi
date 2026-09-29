#include <algorithm>
#include "common/recovery/SZERecoverable.h"
#include <boost/crc.hpp>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>
#include <fstream>
using namespace sze_recovery;
void check(bool b){if(!b)throw std::runtime_error("CRC or ring regression");}
volatile unsigned sink;
int main(){
 std::vector<unsigned char> data(65536+64);unsigned rng=12345;
 for(auto&x:data){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;x=rng;}
 check(crc32("123456789",9)==0xcbf43926U);check(crc32(0,0)==0);
 unsigned cases=0;for(unsigned align=0;align<32;++align)for(unsigned n=0;n<=8192;++n){boost::crc_32_type ref;ref.process_bytes(data.data()+align,n);check(crc32(data.data()+align,n)==ref.checksum());++cases;}
 for(unsigned n:{16383,16384,16385,65535,65536}){boost::crc_32_type ref;ref.process_bytes(data.data()+1,n);check(crc32(data.data()+1,n)==ref.checksum());}
 // Guard page: SIMD must never read beyond the buffer, including tiny tails.
 long page=sysconf(_SC_PAGESIZE);auto p=static_cast<unsigned char*>(mmap(0,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));check(p!=MAP_FAILED);check(mprotect(p+page,page,PROT_NONE)==0);memcpy(p,data.data(),page);
 for(unsigned n=0;n<=unsigned(page);++n){boost::crc_32_type ref;ref.process_bytes(p+page-n,n);check(crc32(p+page-n,n)==ref.checksum());}munmap(p,page*2);
 std::cout<<"oracle_cases="<<cases+5+page+1<<" passed\n";
 RingConfig cfg;cfg.path="/dev/shm/usagi-crc-test-"+std::to_string(getpid());cfg.trading_day=20260918;cfg.source_id=89;cfg.generation=123;cfg.capacity=64;cfg.max_payload_bytes=8192;
 ShmEventRing ring;check(ring.create(cfg));std::vector<unsigned char> result(8192);unsigned long long id=0;
 for(unsigned size:{0,72,440,1224,8192}){
  CanonicalEvent e={};e.payload_size=size;CanonicalEvent got;
  const unsigned loops=100000;auto start=monotonic_time_ns();for(unsigned i=0;i<loops;++i)sink=crc32(data.data(),size);auto crcend=monotonic_time_ns();
  for(unsigned i=0;i<loops;++i){e.event_id=++id;check(ring.publish(e,data.data()));check(ring.read(id,&got,result.data(),result.size())==kRingReadOk);}
  auto end=monotonic_time_ns();check(memcmp(result.data(),data.data(),size)==0);
  std::cout<<"bytes="<<size<<" crc_ns="<<double(crcend-start)/loops<<" publish_read_ns="<<double(end-crcend)/loops<<'\n';
 }
 // Published payload corruption must still be detected by the consumer.
 CanonicalEvent e={};e.event_id=++id;e.payload_size=1224;check(ring.publish(e,data.data()));
 std::fstream file(cfg.path,std::ios::in|std::ios::out|std::ios::binary);std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
 auto it=std::search(bytes.begin(),bytes.end(),data.begin(),data.begin()+1224,[](char a,unsigned char b){return static_cast<unsigned char>(a)==b;});check(it!=bytes.end());
 // Corrupt all matching payloads so the selected current slot is included.
 for(;it!=bytes.end();it=std::search(it+1224,bytes.end(),data.begin(),data.begin()+1224,[](char a,unsigned char b){return static_cast<unsigned char>(a)==b;})){file.clear();file.seekp(it-bytes.begin());char bad=(*it)^1;file.write(&bad,1);}file.flush();
 CanonicalEvent got;check(ring.read(id,&got,result.data(),result.size())==kRingReadOverrun);unlink(cfg.path.c_str());std::cout<<"ring corruption detection passed\n";
}
