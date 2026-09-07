#ifndef SHSZ_FULL_ORDERBOOK_MANAGER_H
#define SHSZ_FULL_ORDERBOOK_MANAGER_H

#include <string>
#include <unordered_map>

#include "common/contracts/legacy/LFDataStruct.h"
#include "common/market_data/legacy/shsz_full_orderbook_engine.h"

class ShSzFullOrderBookManager {
public:
    enum MarketType {
        SH_STOCK_MARKET,
        SZ_STOCK_MARKET,
        NONE
    };

    ShSzFullOrderBookManager();

    void process_order(const LFL2OrderField* order_data);
    void process_trade(const LFL2TradeField* trade_data);

    bool has_instrument(const char* instrument_id) const;
    ShSzFullOrderBookEngine* get_engine(const char* instrument_id);
    const ShSzFullOrderBookEngine* get_engine(const char* instrument_id) const;
    ShSzVisibleBook snapshot_visible_book(const char* instrument_id, uint32_t now_time_ms) const;
    ShSzFullOrderBookSummary snapshot_summary(const char* instrument_id, uint32_t now_time_ms) const;
    ShSzFullOb snapshot_full_orderbook_aggregate(const char* instrument_id, uint32_t now_time_ms) const;
    struct PendingMarketOrderSnapshot {
        bool active;
        bool is_sell;
        long quote_tag;
        int volume;
        uint32_t create_time_ms;
        int resolved_price;

        PendingMarketOrderSnapshot();
    };
    struct InstrumentSnapshot {
        bool valid;
        PendingMarketOrderSnapshot pending_market_order;
        ShSzFullOrderBookStateSnapshot book_state;

        InstrumentSnapshot();
    };
    InstrumentSnapshot snapshot_instrument(const char* instrument_id, uint32_t now_time_ms) const;

    void clear();

    static MarketType judge_market_type(const char* instrument_id);

private:
    struct PendingMarketOrder {
        bool active;
        bool is_sell;
        long quote_tag;
        int volume;
        uint32_t create_time_ms;
        int resolved_price;

        PendingMarketOrder();
    };

    struct InstrumentState {
        std::string instrument_id;
        double instrument_id_value;
        ShSzFullOrderBookEngine engine;
        PendingMarketOrder pending_market_order;

        InstrumentState();
    };

    InstrumentState& ensure_instrument(const char* instrument_id);
    void process_sh_stock_order(InstrumentState* state, const LFL2OrderField* order_data);
    void process_sz_stock_order(InstrumentState* state, const LFL2OrderField* order_data);
    void process_sh_stock_trade(InstrumentState* state, const LFL2TradeField* trade_data);
    void process_sz_stock_trade(InstrumentState* state, const LFL2TradeField* trade_data);

    void execute_if_present(InstrumentState* state, long quote_tag, int volume);
    void cancel_if_present(InstrumentState* state, long quote_tag, int volume);
    void materialize_pending_market_order(InstrumentState* state, int price);
    void try_resolve_pending_market_order(InstrumentState* state, const LFL2TradeField* trade_data);
    void flush_pending_market_order_if_resolvable(InstrumentState* state);
    static PendingMarketOrderSnapshot snapshot_pending_market_order(const PendingMarketOrder& pending_market_order);
    static int pending_market_order_reference_price(const InstrumentState* state);

    static bool is_auction_time(const char* time_text);
    static int round_price(double price);
    static double parse_instrument_id_value(const char* instrument_id);

    std::unordered_map<std::string, InstrumentState> mInstrumentStates;
};

#endif // SHSZ_FULL_ORDERBOOK_MANAGER_H
