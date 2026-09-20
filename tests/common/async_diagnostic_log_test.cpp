#include "common/oms/AsyncDiagnosticLog.h"
#include "common/oms/OrderLatencyLog.h"
#include <cassert>
#include <chrono>
#include <sstream>
#include <vector>
#include <unistd.h>
using Json=nlohmann::json;
int main(){
    int pipefd[2];assert(pipe(pipefd)==0);
    const std::string path="/proc/self/fd/"+std::to_string(pipefd[1]);setenv("USAGI_TEST_DIAGNOSTIC_PATH",path.c_str(),1);
    std::string captured;std::mutex start_mutex;std::condition_variable start_cv;bool start=false;
    // Deliberately withhold reads. A writer that holds the producer lock over
    // file IO will stall once the pipe fills and fail the elapsed-time check.
    std::thread reader([&](){
        {std::unique_lock<std::mutex> lock(start_mutex);start_cv.wait_for(lock,std::chrono::seconds(3),[&](){return start;});}
        char bytes[8192];ssize_t n;while((n=read(pipefd[0],bytes,sizeof(bytes)))>0)captured.append(bytes,n);
        close(pipefd[0]);
    });
    std::uint64_t elapsed;
    {
        diagnostic_log::Sink sink("USAGI_TEST_DIAGNOSTIC_PATH");assert(sink.enabled());close(pipefd[1]);
        const auto begin=std::chrono::steady_clock::now();std::vector<std::thread> producers;
        for(unsigned p=0;p<4;++p)producers.emplace_back([&,p](){for(unsigned i=0;i<200;++i)sink.push(Json{{"producer",p},{"sequence",i},{"payload",std::string(4096,'x')}});});
        for(auto& t:producers)t.join();
        elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count();
        assert(sink.lost()==0);
        {std::lock_guard<std::mutex> lock(start_mutex);start=true;}start_cv.notify_one();
        sink.drain();assert(sink.lost()==0);
        // Explicit drain and destruction must finish every accepted builder.
    }
    reader.join();assert(elapsed<2000);
    unsigned next[4]={},count=0;std::istringstream lines(captured);std::string line;
    while(std::getline(lines,line)){auto row=Json::parse(line);unsigned p=row.at("producer");assert(p<4);assert(row.at("sequence")==next[p]++);assert(row.at("payload").get<std::string>().size()==4096);++count;}
    assert(count==800);for(auto n:next)assert(n==200);
    setenv("USAGI_TEST_DIAGNOSTIC_PATH","/dev/full",1);
    {diagnostic_log::Sink sink("USAGI_TEST_DIAGNOSTIC_PATH");sink.push(Json{{"test","write failure does not throw"}});sink.drain();assert(sink.lost()>0);}
    unsetenv("USAGI_TEST_DIAGNOSTIC_PATH");
    {diagnostic_log::Sink sink("USAGI_TEST_DIAGNOSTIC_PATH");assert(!sink.enabled());sink.push(Json{{"disabled",true}});}
    // A delayed formatter must own the command/result snapshot, even after
    // the strategy reuses or destroys its original command.
    char latency_path[]="/tmp/usagi-order-latency-XXXXXX";
    const int latency_fd=mkstemp(latency_path);assert(latency_fd>=0);close(latency_fd);
    setenv("SSE_ORDER_LATENCY_PATH",latency_path,1);
    bool format=false;std::mutex format_mutex;std::condition_variable format_cv;
    auto& latency_sink=order_latency::sink();
    latency_sink.enqueue([&](){std::unique_lock<std::mutex> lock(format_mutex);format_cv.wait(lock,[&](){return format;});return Json{{"test","barrier"}};});
    {
        oms::Command command;command.id=42;command.scope.epoch=17;command.intent.instrument={"SSE","600000"};command.intent.quantity=200;
        order_latency::Timing t;t.receive=1000;t.signal=2000;t.strategy=2100;
        command.intent.signal_id=order_latency::identity("600000",1,t);
        oms::SendResult result;result.disposition=oms::SendDisposition::Submitted;
        order_latency::record(command,result,true,2800,2900);
        command.id=99;command.intent.quantity=999;result.disposition=oms::SendDisposition::NotSent;
    }
    {std::lock_guard<std::mutex> lock(format_mutex);format=true;}format_cv.notify_one();latency_sink.drain();
    std::ifstream latency_file(latency_path);std::getline(latency_file,line);std::getline(latency_file,line);
    const auto latency=Json::parse(line);assert(latency.at("request_id")==42 && latency.at("quantity")==200);
    assert(latency.at("send_disposition")=="submitted" && latency.at("timing_valid")==true);
    assert(latency.at("receive_ns")==1000 && latency.at("td_return_ns")==2900);
    unlink(latency_path);
    std::puts("async_diagnostic_log_test: PASS concurrent FIFO, stalled IO, shutdown drain, disabled and failed output");
}
