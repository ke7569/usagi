#include "adapters/td/atp/AtpOrderReportMapping.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void test_new_order_ack_maps_current_cl_ord() {
    const atp_order_report::Mapping mapping = atp_order_report::map(100000001, 0, 0);
    require(!mapping.cancel_object, "new order classified as cancel object");
    require(mapping.route_cl_ord_no == 100000001, "new order route key changed");
    require(mapping.bind_report_cl_ord && mapping.publish_oms_order,
            "new order must bind and publish");
}

void test_cancel_object_uses_original_and_is_not_published() {
    const atp_order_report::Mapping mapping = atp_order_report::map(
        100000002, 100000001, atp_order_report::kCancelOrderSign);
    require(mapping.cancel_object, "cancel object not classified");
    require(mapping.route_cl_ord_no == 100000001, "cancel did not route by original cl ord");
    require(!mapping.bind_report_cl_ord && !mapping.publish_oms_order,
            "cancel object would overwrite OMS order identity");
}

void test_orig_cl_ord_is_authoritative_even_without_ord_sign() {
    const atp_order_report::Mapping mapping = atp_order_report::map(100000002, 100000001, 0);
    require(mapping.cancel_object && mapping.route_cl_ord_no == 100000001,
            "OrigClOrdNo did not identify cancel object");
    require(!mapping.bind_report_cl_ord && !mapping.publish_oms_order,
            "OrigClOrdNo cancel object would be published");
}

void test_cancel_sign_without_original_fails_closed_for_oms_route() {
    const atp_order_report::Mapping mapping = atp_order_report::map(
        100000002, 0, atp_order_report::kCancelOrderSign);
    require(mapping.cancel_object, "cancel sign not classified");
    require(mapping.route_cl_ord_no == 0, "missing original cl ord was invented");
    require(!mapping.bind_report_cl_ord && !mapping.publish_oms_order,
            "unassociated cancel object would be published");
}

void test_missing_current_cl_ord_fails_closed() {
    const atp_order_report::Mapping zero = atp_order_report::map(0, 0, 0);
    require(!zero.cancel_object && zero.route_cl_ord_no == 0,
            "missing current cl ord changed route identity");
    require(!zero.bind_report_cl_ord && !zero.publish_oms_order,
            "missing current cl ord remained publishable");

    const atp_order_report::Mapping negative = atp_order_report::map(-1, 0, 0);
    require(!negative.cancel_object && negative.route_cl_ord_no == -1,
            "negative current cl ord changed route identity");
    require(!negative.bind_report_cl_ord && !negative.publish_oms_order,
            "negative current cl ord remained publishable");
}

void test_duplicate_cancel_ack_keeps_the_same_route_decision() {
    const atp_order_report::Mapping first = atp_order_report::map(
        100000002, 100000001, atp_order_report::kCancelOrderSign);
    const atp_order_report::Mapping duplicate = atp_order_report::map(
        100000002, 100000001, atp_order_report::kCancelOrderSign);
    require(first.cancel_object == duplicate.cancel_object &&
            first.route_cl_ord_no == duplicate.route_cl_ord_no &&
            first.bind_report_cl_ord == duplicate.bind_report_cl_ord &&
            first.publish_oms_order == duplicate.publish_oms_order,
            "duplicate cancel ack changed route decision");
}

}  // namespace

int main() {
    try {
        test_new_order_ack_maps_current_cl_ord();
        test_cancel_object_uses_original_and_is_not_published();
        test_orig_cl_ord_is_authoritative_even_without_ord_sign();
        test_cancel_sign_without_original_fails_closed_for_oms_route();
        test_missing_current_cl_ord_fails_closed();
        test_duplicate_cancel_ack_keeps_the_same_route_decision();
    } catch (const std::exception& error) {
        std::cerr << "atp_order_report_mapping_test: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
