#include "sse/runtime/sse_capture_history.h"
#include <cassert>
#include <iostream>
using J=nlohmann::json;
using E=deepwin_market_data::StreamEvent;
E packet(unsigned c,const unsigned char* data,unsigned n,unsigned hw){E e={};e.kind=deepwin_market_data::kDatagramEvent;e.channel_id=c;e.data=data;e.size=n;e.hardware_ns=hw;return e;}
J proof(unsigned c,unsigned id,const E&e){return J{{"channel",c},{"event_id",id},{"hardware_ns",e.hardware_ns},{"payload_crc",sze_recovery::crc32(e.data,e.size)},{"payload_bytes",e.size}};}
int main(){
 unsigned char a[]={1,2,3},b[]={4,5};auto e0=packet(0,a,3,100),e1=packet(1,b,2,200);E idle={};idle.kind=deepwin_market_data::kIdleEvent;
 J ps=J::array();ps.push_back(proof(0,5,e0));ps.push_back(proof(1,8,e1));
 sse_journal::OverlapFilter f;f.configure(ps,2);assert(!f.accept(e0,1));assert(!f.accept(idle,2));assert(!f.accept(e0,5));assert(f.accept(e0,6));assert(f.accept(idle,7));assert(!f.accept(e1,8));f.complete();assert(f.accept(e1,9));
 bool bad=false;try{sse_journal::OverlapFilter x;x.configure(ps,2);x.accept(e0,6);}catch(...){bad=true;}assert(bad);
 bad=false;try{sse_journal::OverlapFilter x;x.configure(ps,2);x.accept(e0,5);x.complete();}catch(...){bad=true;}assert(bad);
 bad=false;try{sse_journal::OverlapFilter x;x.configure(ps,2);auto wrong=e0;wrong.hardware_ns=101;x.accept(wrong,5);}catch(...){bad=true;}assert(bad);
 bad=false;try{sse_journal::OverlapFilter x;auto dup=ps;dup[1]=ps[0];x.configure(dup,2);}catch(...){bad=true;}assert(bad);
 sse_journal::OverlapFilter legacy;assert(legacy.accept(e0,1));assert(legacy.accept(idle,2));legacy.complete();std::cout<<"overlap filter PASS\n";
}
