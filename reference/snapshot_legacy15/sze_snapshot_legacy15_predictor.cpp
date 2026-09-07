#define _GNU_SOURCE

#include "sze/market_data/SZEProtocol.h"
#include "snapshot_legacy15_factors.h"
#include "snapshot_legacy15_model.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <getopt.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {
volatile std::sig_atomic_t g_stop = 0;
void stop_handler(int) { g_stop = 1; }

struct Config {
    std::string group = "239.35.80.1";
    std::string iface_ip = "11.11.11.11";
    std::string ifname = "hqh-p1-k2";
    std::string weights = "snapshot_legacy15.bin";
    std::string scaler = "scaler.json";
    std::string output = "sze_snapshot_legacy15_predictions.csv";
    int port = 37100;
    int seconds = 0;
    int rcvbuf_mb = 256;
};

struct InstrumentState {
    sze_snapshot15::Snapshot previous;
    sze_snapshot15::State hidden;
    bool has_previous = false;
    std::string trading_day;
};

bool parse_args(int argc, char** argv, Config* cfg) {
    enum { GROUP=1000, PORT, IFACE_IP, IFNAME, WEIGHTS, SCALER, OUTPUT, SECONDS, RCVBUF };
    const option options[] = {
        {"group", required_argument, 0, GROUP}, {"port", required_argument, 0, PORT},
        {"iface-ip", required_argument, 0, IFACE_IP}, {"ifname", required_argument, 0, IFNAME},
        {"weights", required_argument, 0, WEIGHTS}, {"scaler", required_argument, 0, SCALER},
        {"output", required_argument, 0, OUTPUT}, {"seconds", required_argument, 0, SECONDS},
        {"rcvbuf-mb", required_argument, 0, RCVBUF}, {"help", no_argument, 0, 'h'}, {0,0,0,0}
    };
    for (;;) {
        const int c = getopt_long(argc, argv, "h", options, 0);
        if (c == -1) break;
        switch (c) {
        case GROUP: cfg->group=optarg; break; case PORT: cfg->port=std::atoi(optarg); break;
        case IFACE_IP: cfg->iface_ip=optarg; break; case IFNAME: cfg->ifname=optarg; break;
        case WEIGHTS: cfg->weights=optarg; break; case SCALER: cfg->scaler=optarg; break;
        case OUTPUT: cfg->output=optarg; break; case SECONDS: cfg->seconds=std::atoi(optarg); break;
        case RCVBUF: cfg->rcvbuf_mb=std::atoi(optarg); break;
        case 'h': return false; default: return false;
        }
    }
    return cfg->port > 0 && cfg->port <= 65535 && cfg->seconds >= 0 && cfg->rcvbuf_mb > 0;
}

int open_socket(const Config& cfg) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one=1; (void)::setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    const int bytes = cfg.rcvbuf_mb * 1024 * 1024; (void)::setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&bytes,sizeof(bytes));
    if (!cfg.ifname.empty() && ::setsockopt(fd,SOL_SOCKET,SO_BINDTODEVICE,cfg.ifname.c_str(),cfg.ifname.size()+1)!=0){::close(fd);return -1;}
    sockaddr_in bind_addr{}; bind_addr.sin_family=AF_INET; bind_addr.sin_port=htons((std::uint16_t)cfg.port); bind_addr.sin_addr.s_addr=htonl(INADDR_ANY);
    if (::bind(fd,(sockaddr*)&bind_addr,sizeof(bind_addr))!=0){::close(fd);return -1;}
    ip_mreqn membership{};
    if (::inet_pton(AF_INET,cfg.group.c_str(),&membership.imr_multiaddr)!=1||::inet_pton(AF_INET,cfg.iface_ip.c_str(),&membership.imr_address)!=1){::close(fd);return -1;}
    if (!cfg.ifname.empty()) { membership.imr_ifindex=(int)::if_nametoindex(cfg.ifname.c_str()); if(!membership.imr_ifindex){::close(fd);return -1;} }
    if (::setsockopt(fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&membership,sizeof(membership))!=0){::close(fd);return -1;}
    return fd;
}

std::uint64_t monotonic_ns(){timespec ts{};::clock_gettime(CLOCK_MONOTONIC,&ts);return (std::uint64_t)ts.tv_sec*1000000000ULL+ts.tv_nsec;}
std::uint64_t realtime_ns(){timespec ts{};::clock_gettime(CLOCK_REALTIME,&ts);return (std::uint64_t)ts.tv_sec*1000000000ULL+ts.tv_nsec;}

sze_snapshot15::Snapshot convert(const LFMarketDataField& v) {
    sze_snapshot15::Snapshot s; s.symbol=v.InstrumentID; s.trading_day=v.TradingDay;
    int h=0,m=0,sec=0,ms=0; std::sscanf(v.UpdateTime,"%d:%d:%d",&h,&m,&sec); ms=v.UpdateMillisec;
    s.exchange_time_ms=((std::uint64_t)h*3600+(std::uint64_t)m*60+sec)*1000+ms;
    s.last_price=v.LastPrice; s.volume=v.Volume; s.turnover=v.Turnover;
    for(int i=0;i<5;++i){s.bid_prices[i]=v.aBidPrice[i];s.ask_prices[i]=v.aAskPrice[i];s.bid_volumes[i]=v.aBidVolume[i];s.ask_volumes[i]=v.aAskVolume[i];}
    return s;
}

bool decode_source88_snapshot(void* record, std::size_t length,
                              LFMarketDataField* value) {
    if (length != sze_md::kSnapshotRecordSize) return false;
    auto* wire = static_cast<sze_md::SzeHpfSnapshot*>(record);
    const std::uint64_t sequence_num = wire->head.sequence_num;
    if (sequence_num == 0) wire->head.sequence_num = 1;
    const bool decoded = sze_md::decode_snapshot(wire, length, value);
    wire->head.sequence_num = sequence_num;
    return decoded;
}

void header(std::ofstream& out){out<<"recv_realtime_ns,symbol,trading_day,update_time,update_millisec,prediction_permille,inference_ns,sequence\n";}

} // namespace

int main(int argc,char**argv){
    Config cfg; if(!parse_args(argc,argv,&cfg)){std::cerr<<"usage: "<<argv[0]<<" [--weights PATH --scaler PATH --output PATH --seconds N]\n";return 2;}
    sze_snapshot15::Model model; std::string error;
    if(!model.load(cfg.weights,cfg.scaler,&error)){std::cerr<<"model load failed: "<<error<<"\n";return 1;}
    std::ofstream out(cfg.output.c_str(),std::ios::out|std::ios::trunc); if(!out){std::cerr<<"cannot open output\n";return 1;} header(out);
    const int fd=open_socket(cfg); if(fd<0){std::perror("open_socket");return 1;}
    std::signal(SIGINT,stop_handler);std::signal(SIGTERM,stop_handler);
    std::array<unsigned char,65536> packet{};std::unordered_map<std::string,InstrumentState> states;
    std::uint64_t packets=0,snapshots=0,predictions=0,ignored=0,malformed=0,total_infer_ns=0;const std::uint64_t started=monotonic_ns();
    std::cerr<<"listening group="<<cfg.group<<" port="<<cfg.port<<" iface="<<cfg.ifname<<" weights="<<cfg.weights<<"\n";
    while(!g_stop){if(cfg.seconds>0&&monotonic_ns()-started>=(std::uint64_t)cfg.seconds*1000000000ULL)break;pollfd pfd{fd,POLLIN,0};int ready=::poll(&pfd,1,1000);if(ready<0){if(errno==EINTR)continue;break;}if(ready==0)continue;if(!(pfd.revents&POLLIN))continue;
        const ssize_t len=::recv(fd,packet.data(),packet.size(),0);if(len<0){if(errno==EINTR)continue;break;}++packets;std::size_t off=0;
        while(off+9<=(std::size_t)len){const auto* head=(const sze_md::SzeHpfHead*)(packet.data()+off);const std::size_t size=sze_md::wire_record_size(head->message_type);if(!size||off+size>(std::size_t)len){++malformed;break;}if(head->message_type!=sze_md::kSnapshotMessage){++ignored;off+=size;continue;}
            LFMarketDataField value{};if(!decode_source88_snapshot(packet.data()+off,size,&value)){++malformed;off+=size;continue;}++snapshots;sze_snapshot15::Snapshot current=convert(value);InstrumentState& state=states[current.symbol];if(state.trading_day!=current.trading_day){state=InstrumentState();state.trading_day=current.trading_day;}
            if(state.has_previous&&current.exchange_time_ms>state.previous.exchange_time_ms&&current.exchange_time_ms-state.previous.exchange_time_ms<=4000&&current.volume>=state.previous.volume&&current.turnover>=state.previous.turnover){std::array<float,36> factors;if(sze_snapshot15::build_factors(state.previous,current,&factors)){const std::uint64_t t0=monotonic_ns();float pred=0;if(model.predict(factors,&state.hidden,&pred,&error)){const std::uint64_t dt=monotonic_ns()-t0;total_infer_ns+=dt;++predictions;out<<realtime_ns()<<','<<value.InstrumentID<<','<<value.TradingDay<<','<<value.UpdateTime<<','<<value.UpdateMillisec<<','<<std::setprecision(9)<<pred<<','<<dt<<','<<head->sequence<<'\n';}}}
            state.previous=current;state.has_previous=true;off+=size;
        }
    }
    out.flush();::close(fd);std::cerr<<"done packets="<<packets<<" snapshots="<<snapshots<<" predictions="<<predictions<<" ignored="<<ignored<<" malformed="<<malformed<<" infer_avg_ns="<<(predictions?total_infer_ns/predictions:0)<<"\n";return 0;
}
