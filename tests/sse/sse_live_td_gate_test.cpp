#include "apps/StreamProcessingCli.h"
#include "tests/sse/sse_test_artifacts.h"
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <sched.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>

namespace {
typedef nlohmann::json Json;
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
struct RestoreEnv {
    bool present;
    std::string value;
    RestoreEnv() : present(std::getenv("SSE_ENABLE_LIVE_ORDER") != 0),
        value(present ? std::getenv("SSE_ENABLE_LIVE_ORDER") : "") {}
    ~RestoreEnv() {
        if (present) ::setenv("SSE_ENABLE_LIVE_ORDER", value.c_str(), 1);
        else ::unsetenv("SSE_ENABLE_LIVE_ORDER");
    }
};
Json profile(const std::string& dir, const std::string& library) {
    Json legacy = {{"market", "SH"}, {"trading_day", 20260909},
        {"global_params", {{"offset", 1.0}, {"position_base_line", 100000.0},
                            {"position_limit", 1.0}, {"global_bias_factor", 1.0}}},
        {"ins_params", {{"600000.SH", {{"static_position", 200}, {"last_position", 999}}}}}};
    return Json{
        {"schema_version", 1}, {"market", "SH"}, {"execution", "live"},
        {"processing_mode", "prediction"}, {"processing_contract", "sse-per-instrument-v2"},
        {"trading_day", 20260909}, {"processing_sha256", std::string(64, '0')},
        {"environment", {{"execution", "live"}, {"mode", "live"}, {"clock", "host"}}},
        {"prediction", {
            {"model_path", dir + "/tick.bin"},
            {"snapshot_baseline_model_path", dir + "/baseline.bin"},
            {"snapshot_baseline_scaler_path", dir + "/baseline.json"},
            {"snapshot_auction59_model_path", dir + "/auction.bin"},
            {"snapshot_auction59_scaler_path", dir + "/auction.json"}}},
        {"instruments", Json::array({{
            {"instrument", "600000"}, {"trading_date", 20260909},
            {"average_amount", 8000000}, {"turnover_threshold", 1000},
            {"free_share", 10000000}, {"pre_close", 10},
            {"upper_limit", 11}, {"lower_limit", 9}, {"history_volatility_20d", 0.02}}})},
        {"strategy_runtime", {{"mode", "live"}, {"account_reference", "FAKE-GATE-TEST"},
            {"legacy_config", legacy}, {"oms", {{"journal_path", dir + "/must-not-open.oms"},
                                                   {"fee_reserve_per_order", 0.0}}},
            {"td", {{"library", library}, {"config_path", dir + "/reject-after-gates"},
                    {"trading_enabled", true}, {"production_approval", true}, {"epoch", 1}}}}}
    };
}
void rejects(const Json& config, const std::string& dir, bool capture, bool handoff,
             const std::string& expected, bool factory_reached = false) {
    const std::string marker = dir + "/reject-after-gates";
    ::unlink(marker.c_str());
    { std::ofstream out((dir + "/profile.json").c_str()); out << config.dump(); }
    std::string error;
    try {
        sse_application::StreamProcessingCli app(dir + "/profile.json", capture, dir, 2,
            []() { return true; }, "raw", handoff);
    } catch (const std::runtime_error& failure) { error = failure.what(); }
    require(error.find(expected) != std::string::npos,
            "wrong live gate rejection: expected " + expected + ", got " + error);
    require((::access(marker.c_str(), F_OK) == 0) == factory_reached,
            "TD factory reached despite closed gate or unreachable with all gates open");
    require(::access((dir + "/must-not-open.oms").c_str(), F_OK) != 0,
            "gate test must not open an account OMS journal");
}
void monitor_reconciles_without_orders(const Json& base, const std::string& dir) {
    Json config = base;
    config["execution"] = "monitor";
    config["environment"]["execution"] = "monitor";
    config["strategy_runtime"]["mode"] = "monitor";
    config["strategy_runtime"]["td"]["config_path"] = dir + "/monitor.trace";
    config["strategy_runtime"]["td"]["trading_enabled"] = false;
    config["strategy_runtime"]["td"]["production_approval"] = false;
    config["strategy_runtime"]["td"]["cpu"] = -1;
    config["strategy_runtime"]["oms"]["journal_path"] = dir + "/monitor.oms";
    config["strategy_runtime"]["account_reference"] = "FAKE-MONITOR-" + std::to_string(::getpid());
    { std::ofstream out((dir + "/profile.json").c_str()); out << config.dump(); }
    // An accidentally enabled environment cannot turn monitor into live.
    ::setenv("SSE_ENABLE_LIVE_ORDER", "YES", 1);
    {
        sse_application::StreamProcessingCli app(dir + "/profile.json", true, dir, 2,
            []() { return true; }, "raw", true);
        app.poll();
        const Json summary = app.summary();
        require(summary.at("execution") == "monitor" && summary.at("strategy").at("mode") == "monitor",
            "monitor must expose its execution mode");
        require(summary.at("strategy").at("orders_enabled") == false,
            "monitor must never enable orders");
        require(summary.at("strategy").at("oms").at("connected") == true &&
                summary.at("strategy").at("oms").at("ready") == true,
            "monitor did not complete the native TD query and owner callback path");
        require(summary.at("strategy").at("td_status").get<std::string>().find(
            "submits=0 cancels=0 queries=1") != std::string::npos,
            "monitor submitted or canceled instead of querying");
        const Json heartbeat = app.runtime_status();
        require(heartbeat.at("execution") == "monitor" &&
                heartbeat.at("strategy").at("orders_enabled") == false &&
                heartbeat.at("strategy").at("connected") == true &&
                heartbeat.at("strategy").at("ready") == true &&
                heartbeat.at("strategy").at("order_intents") == 0 &&
                !heartbeat.at("strategy").count("audit_tail"),
            "compact runtime heartbeat disagrees with monitor account status");
        app.begin_stop();
    }
    std::ifstream trace((dir + "/monitor.trace").c_str());
    std::string line; bool query_only = false;
    while (std::getline(trace, line)) {
        if (line == "create-query-only") query_only = true;
        require(line != "submit" && line != "cancel", "monitor reached a TD trading sender");
    }
    require(query_only, "monitor must construct the plugin with allow_orders=false");
    ::unlink((dir + "/monitor.trace").c_str());
    ::unlink((dir + "/monitor.oms").c_str());
}

void async_monitor_progresses_without_clock_messages(const Json& base, const std::string& dir) {
    cpu_set_t allowed;CPU_ZERO(&allowed);
    require(!sched_getaffinity(0,sizeof(allowed),&allowed),"read test CPU affinity");
    std::vector<int> cpus;
    for(int i=0;i<CPU_SETSIZE && cpus.size()<3;++i)if(CPU_ISSET(i,&allowed))cpus.push_back(i);
    require(cpus.size()==3,"async monitor test requires three allowed CPUs");
    struct Environment {
        std::string key,value;bool present;
        Environment(const char* name,const std::string& setting):key(name),present(std::getenv(name)!=0) {
            if(present)value=std::getenv(name);::setenv(name,setting.c_str(),1);
        }
        ~Environment(){if(present)::setenv(key.c_str(),value.c_str(),1);else ::unsetenv(key.c_str());}
    } workers("SSE_PREDICTION_CPUS",std::to_string(cpus[0])+","+std::to_string(cpus[1])),
      strategy("SSE_STRATEGY_CPU",std::to_string(cpus[2]));
    const std::string model=dir+"/v06.bin";
    {std::ofstream out(model.c_str(),std::ios::binary);out.write("SSEV06M1",8);
     const unsigned sizes[]={50,50,6400,128,49152,49152,384,384,49152,49152,384,384,512,4};
     for(unsigned size:sizes){std::vector<float> zero(size,0);out.write(reinterpret_cast<const char*>(zero.data()),size*4);}}
    Json config=base;
    config["execution"]="monitor";config["environment"]["execution"]="monitor";
    config["strategy_runtime"]["mode"]="monitor";
    config["strategy_runtime"]["legacy_config"]["model_version"]="v0.6";
    config["strategy_runtime"]["legacy_config"]["ins_params"]["600001.SH"]=
        config["strategy_runtime"]["legacy_config"]["ins_params"]["600000.SH"];
    Json second=config["instruments"][0];second["instrument"]="600001";
    config["instruments"].push_back(second);
    config["prediction"]={{"model_path",model},{"model_version","v0.6"},{"auction59",{{"enabled",false}}}};
    config["strategy_runtime"]["td"]["config_path"]=dir+"/async-monitor.trace";
    config["strategy_runtime"]["td"]["trading_enabled"]=false;
    config["strategy_runtime"]["td"]["production_approval"]=false;
    config["strategy_runtime"]["oms"]["journal_path"]=dir+"/async-monitor.oms";
    config["strategy_runtime"]["account_reference"]="FAKE-ASYNC-MONITOR-"+std::to_string(::getpid());
    {std::ofstream out((dir+"/profile.json").c_str());out<<config.dump();}
    {
        sse_application::StreamProcessingCli app(dir+"/profile.json",true,dir,2,
            [](){return true;},"raw",true);
        Json status;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        do {
            status=app.runtime_status();
            if(status["strategy"]["ready"].get<bool>())break;
            std::this_thread::yield();
        }while(std::chrono::steady_clock::now()<deadline);
        require(status["strategy"]["ready"].get<bool>(),"owner failed to reconcile without market input");
        require(status["strategy_consumer"]["enabled"].get<bool>(),"async monitor not using owner");
        const auto published=status["strategy_consumer"]["published"].get<std::uint64_t>();
        for(unsigned i=0;i<1000;++i)app.poll();
        auto after=app.runtime_status();
        require(after["strategy_consumer"]["published"]==published,"idle polling published clock tasks");
        require(after["strategy"]["order_intents"]==0 && !after["strategy"]["orders_enabled"].get<bool>(),
                "monitor must remain query-only");
        app.begin_stop();
    }
    for(const auto& suffix:{"v06.bin","async-monitor.trace","async-monitor.oms"})
        require(!::unlink((dir+"/"+suffix).c_str()),"remove async monitor fixture");
}
}

int main(int argc, char** argv) {
    // The real launcher creates this shared lease directory before starting
    // the native runtime. The direct fixture must establish it independently.
    for (const char* path : {"/run/usagi", "/run/usagi/oms", "/run/usagi/oms/accounts"}) {
        if (::mkdir(path, 0755) != 0 && errno != EEXIST) {
            std::cerr << "cannot prepare OMS lease directory: " << path << std::endl;
            return 1;
        }
    }
    RestoreEnv restore;
    try {
        require(argc == 2, "usage: sse_live_td_gate_test FAKE_TD_LIBRARY");
        char temporary[] = "/tmp/sse-live-td-gates-XXXXXX";
        const char* created = ::mkdtemp(temporary); require(created != 0, "mkdtemp");
        const std::string dir(created);
        sse_test_artifacts::write_tick_artifact(dir + "/tick.bin");
        sse_test_artifacts::write_snapshot_artifact(dir + "/baseline.bin", 36);
        sse_test_artifacts::write_snapshot_artifact(dir + "/auction.bin", 95);
        sse_test_artifacts::write_scaler(dir + "/baseline.json", 36);
        sse_test_artifacts::write_scaler(dir + "/auction.json", 95);
        const Json base = profile(dir, argv[1]);
        ::setenv("SSE_ENABLE_LIVE_ORDER", "YES", 1);
        Json changed = base; changed["strategy_runtime"]["td"]["trading_enabled"] = false;
        rejects(changed, dir, true, true, "requires trading_enabled");
        changed = base; changed["strategy_runtime"]["td"]["production_approval"] = false;
        rejects(changed, dir, true, true, "requires trading_enabled");
        ::unsetenv("SSE_ENABLE_LIVE_ORDER");
        rejects(base, dir, true, true, "requires trading_enabled");
        ::setenv("SSE_ENABLE_LIVE_ORDER", "yes", 1);
        rejects(base, dir, true, true, "requires trading_enabled");
        ::setenv("SSE_ENABLE_LIVE_ORDER", "YES", 1);
        changed = base; changed["execution"] = "disabled";
        rejects(changed, dir, true, true, "environment disagrees");
        changed = base; changed["environment"]["execution"] = "disabled";
        rejects(changed, dir, true, true, "environment disagrees");
        rejects(base, dir, true, false, "journal handoff strategy entry point");
        rejects(base, dir, false, true, "journal handoff strategy entry point");
        changed = base; changed["strategy_runtime"]["mode"] = "paper-intents";
        rejects(changed, dir, true, true, "live strategy runtime");
        // Once all gates pass the loader reaches our fake factory. It rejects
        // deliberately before Engine creation, without any account/SDK action.
        rejects(base, dir, true, true, "fake TD factory reached", true);
        changed = base; changed["execution"] = "monitor";
        changed["environment"]["execution"] = "monitor";
        changed["strategy_runtime"]["mode"] = "monitor";
        rejects(changed, dir, true, false, "journal handoff strategy entry point");
        rejects(changed, dir, false, true, "journal handoff strategy entry point");
        monitor_reconciles_without_orders(base, dir);
        async_monitor_progresses_without_clock_messages(base, dir);
        rejects(base, dir, true, true, "fake TD factory reached", true);
        const char* files[] = {"tick.bin", "baseline.bin", "auction.bin", "baseline.json",
            "auction.json", "profile.json", "reject-after-gates"};
        for (const char* file : files) require(::unlink((dir + "/" + file).c_str()) == 0, "remove fixture");
        require(::rmdir(dir.c_str()) == 0, "remove fixture directory");
    } catch (const std::exception& error) {
        std::cerr << "sse_live_td_gate_test: " << error.what() << std::endl; return EXIT_FAILURE;
    }
    std::cout << "sse_live_td_gate_test ok\n"; return EXIT_SUCCESS;
}
