//
// Created by Administrator on 2024/1/15.
//
#include <sstream>
#include <cstring>
#include "common/market_data/legacy/snap_generator.h"
#include "common/market_data/legacy/snap_latency.h"

namespace {

inline double timeStringToDouble(const char* timeStr) {
    int hours = (timeStr[0] - '0') * 10 + (timeStr[1] - '0');
    int minutes = (timeStr[3] - '0') * 10 + (timeStr[4] - '0');
    int seconds = (timeStr[6] - '0') * 10 + (timeStr[7] - '0');
    int milliseconds = (timeStr[9] - '0') * 100 + (timeStr[10] - '0') * 10 + (timeStr[11] - '0');
    return hours * 10000000.0 + minutes * 100000.0 + seconds * 1000.0 + milliseconds;
}

}  // namespace

SnapGenerator::SnapGenerator() {
//    mMarketDataPtr= nullptr;
    mSnapIndex=-1;
    strcpy(mCurrentInstrumentID,"");
    mCurrentInstrumentIdValue = 0.0;
    mHasCurrentInstrumentIdValue = false;
    mCurrentOrderMatchEnginePtr = nullptr;
}



SnapGenerator::SnapGenerator(const std::string& instrument_id){

    strcpy(mCurrentInstrumentID,instrument_id.c_str());
    mCurrentInstrumentIdValue = parse_instrument_id_value(mCurrentInstrumentID);
    mHasCurrentInstrumentIdValue = true;
    if (mOrderMatchEngineMap.find(mCurrentInstrumentID)==mOrderMatchEngineMap.end()){
        mStockCodeVec.emplace_back(mCurrentInstrumentID);
        mOrderMatchEngineMap[mStockCodeVec[mStockCodeVec.size()-1].c_str()]=OrderMatchEngine();
    }
    mCurrentOrderMatchEnginePtr = &mOrderMatchEngineMap.find(mCurrentInstrumentID)->second;
    mCurrentOrderMatchEnginePtr->set_instrument_id_value(mCurrentInstrumentIdValue);

    mSnapIndex=-1;
//    mMarketDataPtr=std::vector<double>((mTradeDataSize+mOrderDataSize)*BASIC_FIELD_NUM,0.0);

}



bool SnapGenerator::is_auction_time(const char* time){
    if((strcmp(time,"09:26:00.000")<0)||(strcmp(time,"14:57:00.000")>=0)){
        return true;
    }
    return false;
}
//
//bool SnapGenerator::is_done() const{
//    if((mOrderIndex>=mOrderDataSize)&&(mTradeIndex>=mTradeDataSize)){
//        return true;
//    }else{
//        return false;
//    }
//}

void SnapGenerator::process_trade(const LFL2TradeField*trade_data){
    const bool latency_on = snap_latency_enabled();
    const uint64_t begin_ns = latency_on ? snap_now_ns() : 0;
    MarketType market_type=judge_market_type(trade_data->InstrumentID);
    switch (market_type) {

        case MarketType::SH_STOCK_MARKET:
        {
            process_sh_stock_trade(trade_data);
            break;
        }
        case MarketType::SZ_STOCK_MARKET:
        {
            process_sz_stock_trade(trade_data);
            break;
        }
        default:
            break;
    }
    if (latency_on) {
        auto& stats = snap_latency_stats();
        stats.process_trade_ns += (snap_now_ns() - begin_ns);
        ++stats.process_trade_count;
    }
}
void SnapGenerator::process_order(const LFL2OrderField*order_data){
    const bool latency_on = snap_latency_enabled();
    const uint64_t begin_ns = latency_on ? snap_now_ns() : 0;
    MarketType market_type= judge_market_type(order_data->InstrumentID);        
    switch (market_type) {
        case MarketType::SH_STOCK_MARKET:
        {
            process_sh_stock_order(order_data);
            break;
        }
        case MarketType::SZ_STOCK_MARKET:
        {
            if(order_data->OrdType[0]=='1'){
                process_sz_stock_market_order(order_data);
            }else{
                process_sz_stock_order(order_data);
            }
            break;
        }
        default:
            break;
    }
    if (latency_on) {
        auto& stats = snap_latency_stats();
        stats.process_order_ns += (snap_now_ns() - begin_ns);
        ++stats.process_order_count;
    }
}

void SnapGenerator::process_sh_stock_trade(const LFL2TradeField *trade_data) {
    if(is_auction_time(trade_data->TradeTime)){
        process_sh_stock_auction_trade(trade_data);
    }else{
        process_sh_stock_non_auction_trade(trade_data);
    }
}

void SnapGenerator::process_sz_stock_trade(const LFL2TradeField *trade_data) {
    if(is_auction_time(trade_data->TradeTime)){
        process_sz_stock_auction_trade(trade_data);
    }else{
        process_sz_stock_non_auction_trade(trade_data);
    }
}


    


double SnapGenerator::parse_instrument_id_value(const char* instrument_id) {
    double value = 0.0;
    for (int i = 0; i < 6 && instrument_id[i] != '\0'; ++i) {
        value = value * 10.0 + static_cast<double>(instrument_id[i] - '0');
    }
    return value;
}

void SnapGenerator::ensure_current_order_match_engine(const char* instrument_id) {
    if (strcmp(mCurrentInstrumentID, instrument_id) != 0) {
        strcpy(mCurrentInstrumentID, instrument_id);
        mCurrentInstrumentIdValue = parse_instrument_id_value(mCurrentInstrumentID);
        mHasCurrentInstrumentIdValue = true;
        mCurrentOrderMatchEnginePtr = nullptr;
    }

    if (mCurrentOrderMatchEnginePtr != nullptr) {
        return;
    }

    auto it = mOrderMatchEngineMap.find(mCurrentInstrumentID);
    if (it == mOrderMatchEngineMap.end()) {
        mStockCodeVec.emplace_back(mCurrentInstrumentID);
        const char* key = mStockCodeVec.back().c_str();
        auto inserted = mOrderMatchEngineMap.emplace(key, OrderMatchEngine());
        inserted.first->second.set_instrument_id_value(mCurrentInstrumentIdValue);
        mCurrentOrderMatchEnginePtr = &inserted.first->second;
        return;
    }
    mCurrentOrderMatchEnginePtr = &it->second;
    mCurrentOrderMatchEnginePtr->set_instrument_id_value(mCurrentInstrumentIdValue);
}

MarketType SnapGenerator::judge_market_type(const char*instrument_id) {
    if (instrument_id[0] == '6' && (instrument_id[1] == '0' || instrument_id[1] == '8')) {
        return MarketType::SH_STOCK_MARKET;
    }
    if ((instrument_id[0] == '0' && instrument_id[1] == '0') ||
        (instrument_id[0] == '3' && instrument_id[1] == '0') ||
        (instrument_id[0] == '3' && instrument_id[1] == '1')) {
        return MarketType::SZ_STOCK_MARKET;
    }
    return MarketType::NONE;
}



void SnapGenerator::insert_quote_data(MSQuoteData&quote_data) {
    const bool latency_on = snap_latency_enabled();
    const uint64_t begin_ns = latency_on ? snap_now_ns() : 0;
    ensure_current_order_match_engine(quote_data.InstrumentID);
    mCurrentOrderMatchEnginePtr->insert_quote_data(quote_data);
    if (latency_on) {
        auto& stats = snap_latency_stats();
        stats.insert_quote_ns += (snap_now_ns() - begin_ns);
        ++stats.insert_quote_count;
    }
}


//void SnapGenerator::next() {
//    //要修改一下，一会来修改
//    //需要花一天，改成纯流式能接受的数据
//    mCurrentTradeData=(*mTradeDataCollectionPtr)[mTradeIndex];
//    mCurrentOrderData=(*mOrderDataCollectionPtr)[mOrderIndex];
//    const auto *trade_data=&mCurrentTradeData;
//    const auto *order_data=&mCurrentOrderData;
//    bool is_trade;
//    if(mOrderIndex>=mOrderDataSize){
//        mTradeIndex++;
//        is_trade=true;
//    }else if(mTradeIndex>=mTradeDataSize){
//        mOrderIndex++;
//        is_trade=false;
//    }else{
//        if(mOrderDataCollectionPtr->mRank[mOrderIndex]<mTradeDataCollectionPtr->mRank[mTradeIndex]){
//            mOrderIndex++;
//
//            is_trade=false;
//        }else{
//
//            mTradeIndex++;
//            is_trade=true;
//        }
//    }
////分离两者，这边就处理一些不重要的细节，其他的，丢到另外一边去处理
//    if(is_trade){
//        process_trade(trade_data);
//    }else{
//        process_order(order_data);
//    }
//}

void SnapGenerator::assign_snap_market_data(){
    const bool latency_on = snap_latency_enabled();
    const uint64_t begin_ns = latency_on ? snap_now_ns() : 0;
    ensure_current_order_match_engine(mCurrentQuoteData.InstrumentID);
    auto &mMsMarketData=mMsMarketDataFieldArray[(mSnapIndex+1)%MARKET_ARRAY_LENGTH].ms_market_data;
    uint64_t parse_begin_ns = latency_on ? snap_now_ns() : 0;
    const double market_time_value = timeStringToDouble(mCurrentQuoteData.QuoteTime);
    if (latency_on) {
        snap_latency_stats().assign_instrument_parse_ns += (snap_now_ns() - parse_begin_ns);
        parse_begin_ns = snap_now_ns();
    }
    if (latency_on) {
        snap_latency_stats().assign_time_parse_ns += (snap_now_ns() - parse_begin_ns);
        parse_begin_ns = snap_now_ns();
    }
    mCurrentOrderMatchEnginePtr->copy_snapshot_data(mMsMarketData, mCurrentQuoteData, market_time_value);
    if (latency_on) {
        auto& stats = snap_latency_stats();
        stats.assign_get_snap_ns += (snap_now_ns() - parse_begin_ns);
        stats.assign_snap_ns += (snap_now_ns() - begin_ns);
        ++stats.assign_snap_count;
    }
    mSnapIndex+=1;
}
size_t SnapGenerator::get_snap_data_length() const{
    return mSnapIndex-1;
}
bool SnapGenerator::is_best_price_empty(MSQuoteData&quote_data){
    ensure_current_order_match_engine(quote_data.InstrumentID);
    bool is_sell=quote_data.is_sell;
    if(is_sell){
        return mCurrentOrderMatchEnginePtr->mAskBook.empty();
    }else{
        return mCurrentOrderMatchEnginePtr->mBidBook.empty();
    }
}
bool SnapGenerator::is_market_price_cancel(MSQuoteData&quote_data,const LFL2TradeField*trade_data){
    bool is_sell=trade_data->OfferApplSeqNum>0;

    if((strcmp(quote_data.InstrumentID,trade_data->InstrumentID)==0)&&(std::abs(quote_data.Price-MARKET_PRICE*(is_sell?-1:1)))<=0.1){
        if(is_sell){
            if(quote_data.QuoteTag==trade_data->OfferApplSeqNum){
                return true;
            }
            else{

                return false;
            }
        }else{
            if(quote_data.QuoteTag==trade_data->BidApplSeqNum){
                return true;
            }else{
                return false;
            }
        }
    }else{
        return false;
    }

//    bool is_sell=quote_data.is_sell;
//    if(is_sell){
//        return mOrderMatchEngineMap[quote_data.InstrumentID].mBidBook.empty();
//    }else{
//        return mOrderMatchEngineMap[quote_data.InstrumentID].mAskBook.empty();
//    }
}
void SnapGenerator::process_sh_stock_order(const LFL2OrderField*order_data){

    if(order_data->OrdType[0]=='A'){
        mCurrentQuoteData.is_sell=order_data->OrderKind[0]=='S';
        mCurrentQuoteData.is_cancel=false;
        strcpy(mCurrentQuoteData.InstrumentID,order_data->InstrumentID);
        strcpy(mCurrentQuoteData.QuoteTime,order_data->OrderTime);
        mCurrentQuoteData.Price=static_cast<int>(round(order_data->Price*PRICE_MULTIPLIER));
        mCurrentQuoteData.Volume=order_data->Volume;
        mCurrentQuoteData.ApplSeqNum=order_data->BizIndex;
        mCurrentQuoteData.QuoteTag=order_data->OrderNo;
    }else{
        mCurrentQuoteData.is_sell=order_data->OrderKind[0]=='S';
        mCurrentQuoteData.is_cancel=true;
        strcpy(mCurrentQuoteData.InstrumentID,order_data->InstrumentID);
        strcpy(mCurrentQuoteData.QuoteTime,order_data->OrderTime);
        mCurrentQuoteData.Price=static_cast<int>(round(order_data->Price*PRICE_MULTIPLIER));
        mCurrentQuoteData.Volume=order_data->Volume;
        mCurrentQuoteData.ApplSeqNum=order_data->BizIndex;
        mCurrentQuoteData.QuoteTag=order_data->OrderNo;
    }
    insert_quote_data(mCurrentQuoteData);

    assign_snap_market_data();
}
void SnapGenerator::process_sz_stock_order(const LFL2OrderField*order_data){
    if(mIsMarketPriceOrder){

        insert_quote_data(mCurrentQuoteData);
        assign_snap_market_data();

        mIsMarketPriceOrder=false;
    }
    mCurrentQuoteData.is_sell=order_data->OrderKind[0]=='S';
    mCurrentQuoteData.is_cancel=false;
    strcpy(mCurrentQuoteData.InstrumentID,order_data->InstrumentID);
    strcpy(mCurrentQuoteData.QuoteTime,order_data->OrderTime);
    mCurrentQuoteData.Price=order_data->OrdType[0]=='U'?BEST_PRICE:static_cast<int>(round(order_data->Price*PRICE_MULTIPLIER));
    mCurrentQuoteData.Volume=order_data->Volume;
    mCurrentQuoteData.ApplSeqNum=order_data->ApplSeqNum;
    mCurrentQuoteData.QuoteTag=order_data->ApplSeqNum;
    if(order_data->OrdType[0]=='U'&&is_best_price_empty(mCurrentQuoteData)){

    }else{
        insert_quote_data(mCurrentQuoteData);
        assign_snap_market_data();
    }

}

void SnapGenerator::process_sh_stock_auction_trade(const LFL2TradeField *trade_data) {
    mCurrentQuoteData.is_cancel=true;
    strcpy(mCurrentQuoteData.InstrumentID,trade_data->InstrumentID);
    strcpy(mCurrentQuoteData.QuoteTime,trade_data->TradeTime);
    mCurrentQuoteData.Price=static_cast<int>(round(trade_data->Price*PRICE_MULTIPLIER));
    mCurrentQuoteData.Volume=trade_data->Volume;
    mCurrentQuoteData.ApplSeqNum=trade_data->BizIndex;
    mCurrentQuoteData.is_sell=false;
    mCurrentQuoteData.QuoteTag=trade_data->BidApplSeqNum;
    insert_quote_data(mCurrentQuoteData);
    mCurrentQuoteData.is_sell=true;
    mCurrentQuoteData.QuoteTag=trade_data->OfferApplSeqNum;
    insert_quote_data(mCurrentQuoteData);
    assign_snap_market_data();
}

void SnapGenerator::process_sz_stock_auction_trade(const LFL2TradeField *trade_data) {

    if(trade_data->OrderKind[0]=='F'){
        mCurrentQuoteData.is_sell=false;
        mCurrentQuoteData.is_cancel=true;
        strcpy(mCurrentQuoteData.InstrumentID,trade_data->InstrumentID);
        strcpy(mCurrentQuoteData.QuoteTime,trade_data->TradeTime);
        mCurrentQuoteData.Price=static_cast<int>(round(trade_data->Price*PRICE_MULTIPLIER));
        mCurrentQuoteData.Volume=trade_data->Volume;
        mCurrentQuoteData.ApplSeqNum=trade_data->ApplSeqNum;
        mCurrentQuoteData.QuoteTag=trade_data->BidApplSeqNum;
        insert_quote_data(mCurrentQuoteData);
        mCurrentQuoteData.is_sell=true;
        mCurrentQuoteData.QuoteTag=trade_data->OfferApplSeqNum;
        insert_quote_data(mCurrentQuoteData);
        assign_snap_market_data();
    }else{
        mCurrentQuoteData.is_sell=trade_data->OfferApplSeqNum>trade_data->BidApplSeqNum;
        mCurrentQuoteData.is_cancel=true;
        strcpy(mCurrentQuoteData.InstrumentID,trade_data->InstrumentID);
        strcpy(mCurrentQuoteData.QuoteTime,trade_data->TradeTime);
        mCurrentQuoteData.Price=static_cast<int>(round(trade_data->Price*PRICE_MULTIPLIER));
        mCurrentQuoteData.Volume=trade_data->Volume;
        mCurrentQuoteData.ApplSeqNum=trade_data->ApplSeqNum;
        mCurrentQuoteData.QuoteTag=trade_data->BidApplSeqNum+trade_data->OfferApplSeqNum;
        insert_quote_data(mCurrentQuoteData);
        assign_snap_market_data();
    }

}

void SnapGenerator::process_sh_stock_non_auction_trade(const LFL2TradeField *trade_data) {

    mCurrentQuoteData.is_sell=trade_data->OfferApplSeqNum>trade_data->BidApplSeqNum;
    mCurrentQuoteData.is_cancel=false;
    strcpy(mCurrentQuoteData.InstrumentID,trade_data->InstrumentID);
    strcpy(mCurrentQuoteData.QuoteTime,trade_data->TradeTime);
    mCurrentQuoteData.Price=static_cast<int>(round(trade_data->Price*PRICE_MULTIPLIER));
    mCurrentQuoteData.Volume=trade_data->Volume;
    mCurrentQuoteData.ApplSeqNum=trade_data->BizIndex;
    mCurrentQuoteData.QuoteTag=mCurrentQuoteData.is_sell?trade_data->OfferApplSeqNum:trade_data->BidApplSeqNum;
    insert_quote_data(mCurrentQuoteData);
    assign_snap_market_data();

}

void SnapGenerator::process_sz_stock_non_auction_trade(const LFL2TradeField *trade_data) {
    if(trade_data->OrderKind[0]=='4'){
        if(mIsMarketPriceOrder){
            mIsMarketPriceOrder=false;
            if(!is_market_price_cancel(mCurrentQuoteData,trade_data)){
                insert_quote_data(mCurrentQuoteData);
                assign_snap_market_data();
            }else{
                return;
            }

        }
        mCurrentQuoteData.is_sell=trade_data->OfferApplSeqNum>trade_data->BidApplSeqNum;
        mCurrentQuoteData.is_cancel=true;
        strcpy(mCurrentQuoteData.InstrumentID,trade_data->InstrumentID);
        strcpy(mCurrentQuoteData.QuoteTime,trade_data->TradeTime);
        mCurrentQuoteData.Price=static_cast<int>(round(trade_data->Price*PRICE_MULTIPLIER));
        mCurrentQuoteData.Volume=trade_data->Volume;
        mCurrentQuoteData.ApplSeqNum=trade_data->ApplSeqNum;
        mCurrentQuoteData.QuoteTag=std::max(trade_data->OfferApplSeqNum,trade_data->BidApplSeqNum);
        insert_quote_data(mCurrentQuoteData);
        assign_snap_market_data();
    }else{
        if(mIsMarketPriceOrder){
            if(((trade_data->OfferApplSeqNum==mCurrentQuoteData.QuoteTag)&&(mCurrentQuoteData.is_sell))||((trade_data->BidApplSeqNum==mCurrentQuoteData.QuoteTag)&&(!mCurrentQuoteData.is_sell))){
                mCurrentQuoteData.Price=static_cast<int>(round(trade_data->Price*PRICE_MULTIPLIER));
            }else{
                insert_quote_data(mCurrentQuoteData);
                assign_snap_market_data();
                mIsMarketPriceOrder=false;
            }
        }
    }

}

void SnapGenerator::process_sz_stock_market_order(const LFL2OrderField *order_data) {
    if(mIsMarketPriceOrder){
        insert_quote_data(mCurrentQuoteData);
        assign_snap_market_data();
        mIsMarketPriceOrder=false;
    }
    mIsMarketPriceOrder=true;
    mCurrentQuoteData.is_sell=order_data->OrderKind[0]=='S';
    mCurrentQuoteData.is_cancel=false;
    strcpy(mCurrentQuoteData.InstrumentID,order_data->InstrumentID);
    strcpy(mCurrentQuoteData.QuoteTime,order_data->OrderTime);
    mCurrentQuoteData.Price=MARKET_PRICE*(mCurrentQuoteData.is_sell?-1:1);
    mCurrentQuoteData.Volume=order_data->Volume;
    mCurrentQuoteData.ApplSeqNum=order_data->ApplSeqNum;
    mCurrentQuoteData.QuoteTag=order_data->ApplSeqNum;
//    insert_quote_data(mCurrentQuoteData);
//    assign_snap_market_data();
}


//void SnapGenerator::assign_ms_market_data(long index, const MSMarketData &market_data) const {
//    mMarketDataPtr[InstrumentIDIndex+index*BASIC_FIELD_NUM]=market_data.ms_market_data[InstrumentIDIndex];
//    mMarketDataPtr[MarketTimeIndex+index*BASIC_FIELD_NUM]=market_data.ms_market_data[MarketTimeIndex];
//    mMarketDataPtr[LastPriceIndex+index*BASIC_FIELD_NUM]=market_data.ms_market_data[LastPriceIndex];
//    mMarketDataPtr[MidPriceIndex+index*BASIC_FIELD_NUM]=market_data.ms_market_data[MidPriceIndex];
//    mMarketDataPtr[VolumeIndex+index*BASIC_FIELD_NUM]=market_data.ms_market_data[VolumeIndex];
//    mMarketDataPtr[TurnoverIndex+index*BASIC_FIELD_NUM]=market_data.ms_market_data[TurnoverIndex];
//    for(int i=BidPrice1Index;i<=BidPriceAIndex;i++){
//        mMarketDataPtr[i+index*BASIC_FIELD_NUM]=market_data.ms_market_data[i];
//    }
//    for(int i=AskPrice1Index;i<=AskPriceAIndex;i++){
//        mMarketDataPtr[i+index*BASIC_FIELD_NUM]=market_data.ms_market_data[i];
//    }
//    for(int i=BidVolume1Index;i<=BidVolumeAIndex;i++){
//        mMarketDataPtr[i+index*BASIC_FIELD_NUM]=market_data.ms_market_data[i];
//    }
//    for(int i=AskVolume1Index;i<=AskVolumeAIndex;i++){
//        mMarketDataPtr[i+index*BASIC_FIELD_NUM]=market_data.ms_market_data[i];
//    }
//    mMarketDataPtr[OrderVolumeIndex+index*BASIC_FIELD_NUM]=market_data.ms_market_data[OrderVolumeIndex];
//    mMarketDataPtr[OrderPriceIndex+index*BASIC_FIELD_NUM]=market_data.ms_market_data[OrderPriceIndex];
//    mMarketDataPtr[CancelPriceIndex+index*BASIC_FIELD_NUM]=market_data.ms_market_data[CancelPriceIndex];
//    mMarketDataPtr[CancelVolumeIndex+index*BASIC_FIELD_NUM]=market_data.ms_market_data[CancelVolumeIndex];
//    mMarketDataPtr[IsSellIndex+index*BASIC_FIELD_NUM]=market_data.ms_market_data[IsSellIndex];
//    mMarketDataPtr[IsCancelIndex+index*BASIC_FIELD_NUM]=market_data.ms_market_data[IsCancelIndex];
//}
