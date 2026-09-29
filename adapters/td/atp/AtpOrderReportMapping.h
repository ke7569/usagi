#ifndef USAGI_ATP_ORDER_REPORT_MAPPING_H
#define USAGI_ATP_ORDER_REPORT_MAPPING_H

#include <cstdint>

namespace atp_order_report {

// ATP uses ord_sign=1 for the independent cancel object in live returns.
static const std::uint8_t kCancelOrderSign = 1;

struct Mapping {
    bool cancel_object;
    std::int64_t route_cl_ord_no;
    bool bind_report_cl_ord;
    bool publish_oms_order;

    Mapping(bool cancel, std::int64_t route_cl_ord, bool bind, bool publish)
        : cancel_object(cancel), route_cl_ord_no(route_cl_ord),
          bind_report_cl_ord(bind), publish_oms_order(publish) {}
};

inline Mapping map(std::int64_t cl_ord_no, std::int64_t orig_cl_ord_no,
                   std::uint8_t ord_sign) {
    const bool cancel_object = orig_cl_ord_no > 0 || ord_sign == kCancelOrderSign;
    // A normal return without a positive current ClOrdNo has no stable route
    // key. Let the caller fail closed instead of binding/publishing it under
    // the sentinel key zero.
    const bool routeable_current = !cancel_object && cl_ord_no > 0;
    return Mapping(cancel_object,
                   cancel_object ? orig_cl_ord_no : cl_ord_no,
                   routeable_current, routeable_current);
}

}  // namespace atp_order_report

#endif
