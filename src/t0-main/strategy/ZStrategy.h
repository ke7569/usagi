//
// Created by Administrator on 25-9-14.
//

#ifndef ZSTRATEGY_H
#define ZSTRATEGY_H

#include "../wc_strategy.h"
#include "../util.h"
#include "../common.h"
#include "../RawDataStruct.h"
#include <unordered_map>
#include <unordered_set>
#include <mutex>

struct InsParams;

enum OrderTypeEnum
{
    FAK = 0,
    LMP
};

enum DirectionEnum
{
    BUY = 0,
    SELL
};

struct RT_Order
{
    double Price;
    double Volume;
    OrderTypeEnum Type;
    DirectionEnum Direction;
    double SameLevelOrder;

    RT_Order(double p, double v, DirectionEnum d, OrderTypeEnum t, double l)
    {
        Price = p;
        Volume = v;
        Direction = d;
        Type = t;
        SameLevelOrder = l;
    }

    RT_Order(double p, double v, DirectionEnum d, OrderTypeEnum t)
    {
        Price = p;
        Volume = v;
        Direction = d;
        Type = t;
        SameLevelOrder = 0;
    }

    RT_Order()
    {
        Price = 0;
        Volume = 0;
        Type = FAK;
        Direction = BUY;
        SameLevelOrder = 0;
    }
};

class UnFinishOrders
{
public:
    UnFinishOrders();
    void Insert(int request_id, RT_Order order);

    void Erase(DirectionEnum direction, int request_id);

    void OnRtnOrder(DirectionEnum direction, int request_id, char orderStatus);

    int longPostion;
    int shortPostion;

    //    int longHitPostion;
    //    int shortHitPosition;	//hit
private:
    //    std::map<u_int64_t,int> m_lastHitSellOrders;  //orderrefst => ordernum
    //    std::map<u_int64_t,int> m_lastHitBuyOrders;
    std::map<uint64_t,int> m_lastQuoSellOrders;  //orderrefst => ordernum
    std::map<uint64_t,int> m_lastQuoBuyOrders;

};

class ZStrategy {
public:
    ZStrategy(const std::string & InstrumentID,
        const InsParams &ins_params,
        json &config,
        WCStrategyUtilPtr other_util,
        KfLogPtr other_logger);
    std::unordered_set<std::string> SZPrefix={"00","30"};
    std::unordered_set<std::string> SHPrefix={"60","68"};
    std::unordered_set<std::string> BJPrefix={"920"};
    std::int32_t getPositionLimit();
    std::string ExchangeID;
    std::int32_t getRemainingShortable();
    void setGlobalPredAdjFactor(double factor);

    void setOffset();

    KfLogPtr logger;
    WCStrategyUtilPtr util;
    const std::string& mTradeInstrument;
    void on_signal(const MSMarketDataField * market_data, double signal, short source, long rcv_time);
    void on_rtn_order(const LFRtnOrderField *data, int request_id, short source, long rcv_time);
    void on_rtn_trade(const LFRtnTradeField *data, int request_id, short source, long rcv_time);
    void cancel_order(int request_id);
    void delay_cancel_order(int request_id,int delay_ms);
    void sync_startup_position(int32_t total_position, int32_t available_position);
    void set_latency_trace(const StrategyLatencyTrace& trace);



protected:
    Theo theo_;
    GlobalParams g_params;
    InstrumentParams i_params;
    StrategyContext context;
    double current_prediction_;  // 保存当前的prediction值，用于日志输出
    StrategyLatencyTrace latency_trace_;

private:
    struct TestOrderConfig {
        bool enabled = false;
        std::string instrument;
        DirectionEnum direction = BUY;
        double price = 0.0;
        int volume = 100;
        int trigger_after_signals = 1;
        int cancel_delay_ms = 1000;
    };

    const double pred_unit = 0.001;
    const int32_t vol_unit = 100;
    const double price_unit = 0.01;
    short td_source_ = 28;
    bool routing_enabled_ = false;
    bool virtual_routing_ = false;
    bool recovery_routing_ = false;
    int startup_warmup_signal_count_ = 50;
    int max_position_ = 0;
    TestOrderConfig test_order_;
    bool test_order_sent_ = false;
    json j_config;
    double global_bias_factor_base_line_ = 0.0;

    double getCurPosition();
    int insertOrder(RT_Order order, int cancel_delay_ms = 1001);
    void maybe_send_test_order();
    void calcTheo(double prediction);
    void handleT0();
    void hitBuy();
    void hitSell();
    void cancelBuy();
    void cancelSell();
    int32_t maxCanBuy();
    int32_t maxCanSell();
    bool can_send_order(DirectionEnum direction, long long now) const;
    void on_order_reject(int request_id, const RT_Order& order);
    double bias;
    double skew;
    MSMarketDataField* last_ob_ptr;
    MSMarketData last_ob;
    int startup_signal_count_ = 0;
    long long order_reject_throttle_until_nano_ = 0;
    long long buy_block_until_nano_ = 0;
    long long sell_block_until_nano_ = 0;
    std::unordered_map<long, int> order_ref_last_traded_;
    std::unordered_map<long, int> order_ref_last_leaves_;
    std::unordered_map<long, bool> order_fill_from_order_seen_;
    std::unordered_set<long> terminal_order_state_keys_;
    std::unordered_set<std::string> seen_trade_keys_;
    // Requests with a live remainder are eligible for the deferred FAK cancel.
    // The timer may run after a terminal order callback, so this set is the
    // authoritative guard against cancel-after-fill requests.
    std::unordered_set<int> cancel_pending_request_ids_;
    mutable std::mutex state_mutex_;


};



#endif //ZSTRATEGY_H
