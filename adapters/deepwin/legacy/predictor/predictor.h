//
// Created by Administrator on 25-9-9.
//

#ifndef PREDICTOR_H
#define PREDICTOR_H
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <string>
#include <sstream>
#include <array>
#include <mutex>

#include "common/contracts/legacy/LFDataStruct.h"
#include "common/factors/legacy/factor.h"
#include "common/strategy/common.h"
#include "common/model/legacy/real_gru.h"
#include "common/model/legacy/gru_ver_1.h"

class PredictorBase {
public:
    PredictorBase();
    virtual ~PredictorBase() {
        if (last_ob != nullptr) {
            delete last_ob;
            last_ob = nullptr;
        }
        if (bse_last_ob != nullptr) {
            // 需要先调用析构函数，然后释放内存
            bse_last_ob->bse_snapshot.~BseSnapshot();
            ::operator delete(bse_last_ob);
            bse_last_ob = nullptr;
        }
        // 注意：不再删除共享的模型实例，它们由静态map管理
        // 模型指针只是引用，不负责删除
        real_gru_model_ = nullptr;
        gru_ver_1_model_ = nullptr;
    }

    virtual void handle_trade(const LFL2TradeField* td);
    virtual void handle_order(const LFL2OrderField* od);
    virtual void handle_cached_trade(OrderflowFactor& factor);
    virtual void handle_cached_order(OrderflowFactor& factor);
    virtual bool MayPredict(const MSMarketDataField* cur_ob);
    virtual double DoPredict(const MSMarketDataField* cur_ob);
    virtual void AdvancePredictState(const MSMarketDataField* cur_ob);
    virtual void ProbeFeatureTiming(const MSMarketDataField* cur_ob);

    virtual void generate_of_factors(OrderflowFactor &factor);

    double history_amount = 1e7;
    double threshold_ = 0;
    double cum_amount_ = 0;
    double price_tick = 0.01;
    std::queue<LFL2TradeField> td_queue;
    std::queue<LFL2OrderField> od_queue;


protected:

    void generate_ob_factors(const MSMarketDataField* cur_ob, OrderBookFactor &factor);
    void generate_ob_factors(const BseSnapshotField* cur_ob, OrderBookFactor &factor);
    float predict(const std::vector<float>& features);
    MSMarketDataField* cur_ob = nullptr;
    MSMarketDataField* last_ob;
    BseSnapshotField* bse_last_ob = nullptr;
    BseSnapshotField* bse_curr_ob = nullptr;
    StrategyContext context_;
    StrategyContextBse bse_context_;

    // RNN模型相关成员变量
    static constexpr int kFeatureDim = 30;
    static constexpr int kHiddenDim = 64;
    RealGRU* real_gru_model_;
    gru_ver_1* gru_ver_1_model_ = nullptr;
    bool rnn_initialized_;
    Eigen::VectorXf hidden_state_;
    std::string model_type_;
    bool use_gru_ver_1_ = false;
    bool scaler_loaded_ = false;
    std::array<float, kFeatureDim> scaler_mean_{};
    std::array<float, kFeatureDim> scaler_inv_std_{};
    std::array<float, kFeatureDim> scaled_features_{};

    // 静态模型缓存，避免重复加载
    static std::unordered_map<std::string, RealGRU*> s_shared_real_gru_models_;
    static std::unordered_map<std::string, gru_ver_1*> s_shared_gru_ver_1_models_;

    void initialize_rnn_model(const std::string& model_path,
                              const std::string& scaler_path,
                              const std::string& model_type);

    std::vector<float> extract_features(const OrderflowFactor &orderflow_factor,
                                        const OrderBookFactor &orderbook_factor);

    // 获取共享模型实例的key（基于model_path, input_dim）
    static std::string get_model_key(const std::string& model_path, int input_dim);

    // 获取或创建共享的RealGRU
    static RealGRU* get_shared_real_gru_model(const std::string& model_path, int input_dim);

    static std::string get_gru_ver_1_model_key(const std::string& model_path,
                                               int input_dim,
                                               int hidden_dim);
    static gru_ver_1* get_shared_gru_ver_1_model(const std::string& model_path,
                                                 int input_dim,
                                                 int hidden_dim);
    bool load_scaler(const std::string& scaler_path);
};

// 使用public继承
class SzePredictor : public PredictorBase {
public:
    SzePredictor(double threshold, double history_amount,
                 const std::string& model_type = "real_gru",
                 const std::string& model_path = "model/model_test.json",
                 const std::string& scaler_path = "");
    ~SzePredictor();

    // 重写基类方法
    bool MayPredict(const MSMarketDataField* cur_ob) final;
    double DoPredict(const MSMarketDataField* cur_ob) final;
    void handle_order(const LFL2OrderField* od) final;
    void handle_trade(const LFL2TradeField* td) final;
    void handle_cached_order(OrderflowFactor& factor) override;
    void handle_cached_trade(OrderflowFactor& factor) override;
    void generate_of_factors(OrderflowFactor& factor) override;
    void AdvancePredictState(const MSMarketDataField* cur_ob) override;
    void ProbeFeatureTiming(const MSMarketDataField* cur_ob) override;
    void saveToCSV(const MSMarketDataField* cur_ob, const OrderBookFactor& order_book_factor, const OrderflowFactor& orderflow_factor);

    // RNN 模型相关方法
    // void initialize_rnn_model();
    // std::vector<float> extract_features(const OrderflowFactor& orderflow_factor, const OrderBookFactor& orderbook_factor);

private:
    int predict_count_;  // 记录DoPredict调用次数，用于前5组feature的强制设置
    static const std::vector<std::vector<float>> preset_features_;  // 前5组预设feature值
};


class   BsePredictor : public PredictorBase {
public:
    static constexpr int kBseFeatureDim = 21;

    struct DebugSnapshot {
        std::array<float, kBseFeatureDim> raw_features{};
        std::array<float, kBseFeatureDim> scaled_features{};
        std::array<float, kHiddenDim> hidden_in{};
        std::array<float, kHiddenDim> hidden_out{};
        float prediction = 0.0f;
        bool valid = false;
    };

    BsePredictor(double threshold,
                 double history_amount,
                 const std::string& model_type = "real_gru",
                 const std::string& model_path = "model/model_test.json",
                 const std::string& scaler_path = "");
    bool SeedLastSnapshot(const BseSnapshotField& snapshot);
    bool MayPredict(const BseSnapshotField* cur_ob) ;
    double DoPredict(const BseSnapshotField* cur_ob) ;
    const std::string& LastMayPredictReason() const { return may_predict_reason_; }
    std::vector<float> extract_features(const OrderBookFactor& orderbook_factor);
    void initialize_rnn_model(const std::string& model_path,
                              const std::string& scaler_path,
                              const std::string& model_type);
    const DebugSnapshot& LastDebugSnapshot() const { return debug_snapshot_; }
    int predict_count_ = 0;
private:
    struct BseScalerCacheEntry {
        std::array<float, kBseFeatureDim> mean{};
        std::array<float, kBseFeatureDim> inv_std{};
    };

    float predict(const std::vector<float>& features);
    bool load_bse_scaler(const std::string& scaler_path);
    static bool get_shared_bse_scaler(const std::string& scaler_path,
                                      BseScalerCacheEntry* out);
    std::string model_path_;
    std::array<float, kBseFeatureDim> bse_scaler_mean_{};
    std::array<float, kBseFeatureDim> bse_scaler_inv_std_{};
    std::array<float, kBseFeatureDim> bse_scaled_features_{};
    std::array<float, kBseFeatureDim> bse_raw_features_{};
    bool bse_scaler_loaded_ = false;
    static std::unordered_map<std::string, BseScalerCacheEntry> s_bse_scaler_cache_;
    static std::mutex s_bse_scaler_mutex_;
    std::string may_predict_reason_ = "init";
    DebugSnapshot debug_snapshot_{};
};



class SsePredictor : public PredictorBase {
public:
    bool pending = false;
    SsePredictor(double threshold, double history_amount,
                 const std::string& model_type = "real_gru",
                 const std::string& model_path = "model/model_test.json",
                 const std::string& scaler_path = "");
    bool MayPredict(const MSMarketDataField* cur_ob) override;
    double DoPredict(const MSMarketDataField* cur_ob) override;
    void handle_order(const LFL2OrderField* od) final;
    void handle_trade(const LFL2TradeField* td) final;
    void handle_cached_order(OrderflowFactor& factor) override;
    void handle_cached_trade(OrderflowFactor& factor) override;
};





#endif //PREDICTOR_H
