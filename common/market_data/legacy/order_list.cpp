//
// Created by Administrator on 2024/1/2.
//
#include "common/market_data/legacy/order_list.h"
#include <iostream>
OrderList::OrderList() : mVolume(0) {
}

void OrderList::insert_quote_tag(int quote_volume, bool is_cancel, long quote_tag)  {

    if (!is_cancel) {
        this->mVolume += quote_volume;
    } else {
        this->mVolume -= quote_volume;
    }
}


void OrderList::cross_trade_volume(int total_trade_volume) {

    mVolume -= total_trade_volume;

}

