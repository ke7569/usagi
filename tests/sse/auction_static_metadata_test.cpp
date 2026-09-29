#include "sse/auction/auction_static_metadata.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << "\n"; std::exit(1); }
}
}

int main(int argc, char** argv) {
    if (argc == 3) {
        sse_auction59::StaticMetadataMap values;
        std::string error;
        const std::uint32_t date = static_cast<std::uint32_t>(std::strtoul(argv[2], 0, 10));
        const bool loaded = sse_auction59::load_static_metadata_csv(
            argv[1], date, &values, &error);
        require(loaded, error.c_str());
        std::size_t ready = 0U, ipo = 0U;
        for (sse_auction59::StaticMetadataMap::const_iterator it = values.begin();
             it != values.end(); ++it) {
            ready += it->second.auction_static_ready();
            ipo += it->second.is_ipo_first_day;
        }
        std::cout << "auction_static_metadata_real: rows=" << values.size()
                  << " ready=" << ready << " ipo=" << ipo << "\n";
        return 0;
    }
    const std::string path = "/tmp/sse_auction_static_metadata_test.csv";
    {
        std::ofstream out(path.c_str());
        out << "security_id,name,date,pre_close,upper_limit,lower_limit,listing_date,is_ipo_first_day,limits_valid,source,quality,error\n"
            << "600519,maotai,20260819,1293.09,1422.40,1163.78,20010827,0,1,test,ok,\n"
            << "688836,ipo,20260819,150.80,,,20260819,1,0,test,ipo_no_explicit_limits,\n";
    }
    sse_auction59::StaticMetadataMap values;
    std::string error;
    require(sse_auction59::load_static_metadata_csv(path, 20260819, &values, &error),
            error.c_str());
    require(values.size() == 2U && values["600519"].auction_static_ready(),
            "regular static metadata rejected");
    require(!values["688836"].auction_static_ready() && values["688836"].is_ipo_first_day,
            "IPO missing limits accepted");
    sse_live::Snapshot snapshot;
    snapshot.security_id = "600519"; snapshot.pre_close_price = 1293.09;
    std::string quality;
    require(sse_auction59::reconcile_snapshot_pre_close(snapshot, values, &quality),
            "matching pre-close rejected");
    snapshot.pre_close_price = 1293.08;
    require(!sse_auction59::reconcile_snapshot_pre_close(snapshot, values, &quality) &&
            quality == "pre_close_conflict", "pre-close conflict accepted");
    std::remove(path.c_str());
    std::cout << "auction_static_metadata_test: ok\n";
    return 0;
}
