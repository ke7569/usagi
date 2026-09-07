"""Offline reference skeleton for v0.4 Legacy MidMix SSE."""

FACTOR_NAMES = tuple(line.strip() for line in """factor_hermes_permille
factor_tr_sqrt_positive
factor_spread_permille
factor_mid_return_permille
factor_fee_on_tick
factor_bid_volume_change_ratio
factor_ask_volume_change_ratio
factor_weighted_return_permille_1
factor_weighted_return_permille_2
factor_weighted_return_permille_3
factor_weighted_return_permille_4
factor_weighted_return_permille_5
factor_weighted_ask_permille
factor_weighted_bid_permille
factor_weighted_ask_return_permille
factor_weighted_bid_return_permille
factor_weighted_volume_imbalance
factor_volume_imbalance
factor_percent_turnover
factor_liquidity_ask_l1_share
factor_liquidity_bid_l1_share
factor_positive_order_flow
factor_negative_order_flow
factor_market_flow
factor_cancel_buy_flow
factor_cancel_sell_flow
factor_positive_trade
factor_negative_trade
factor_positive_fill_rate
factor_negative_fill_rate
factor_order_flow_imbalance
factor_cfr_imbalance
factor_book_fixdist_imbalance_1pct
factor_book_fixdist_imbalance_5pct
factor_book_fixdist_weighted_1pct
factor_book_fixdist_weighted_5pct
factor_book_avg_size_imbalance
factor_book_avg_size_imbalance_l1
factor_book_avg_size_imbalance_l5
factor_book_count_imbalance
factor_book_count_imbalance_l1
factor_book_count_imbalance_l5
factor_book_life_imbalance
factor_book_life_imbalance_l1
factor_book_life_imbalance_l5
factor_max_bid_distance_ratio
factor_max_ask_distance_ratio
factor_max_vol_distance_imbalance
factor_book_young_imbalance_1pct
factor_book_fixdist_hermes""".splitlines() if line.strip())

FEATURE_COUNT = 50
N_NEURON = 64
SAMPLE_GAP_US = 100


def is_batch_end(current_local_us, next_local_us):
    return next_local_us is not None and next_local_us - current_local_us > SAMPLE_GAP_US


def target_permille(current_mid, future_mid_15s, future_mid_30s, future_mid_60s):
    mids = (future_mid_15s, future_mid_30s, future_mid_60s)
    if current_mid <= 0.0 or any(value <= 0.0 for value in mids):
        raise ValueError("all mids must be positive")
    return sum((value / current_mid - 1.0) * 1000.0 for value in mids) / 3.0


def build_model():
    """Build the exact PyTorch topology when torch is available."""
    try:
        import torch.nn as nn
    except ImportError:
        raise RuntimeError("PyTorch is required only to instantiate the skeleton")

    class LegacyMidMixSSE(nn.Module):
        def __init__(self):
            super().__init__()
            self.proj = nn.Linear(FEATURE_COUNT, 128)
            self.feature_layers = nn.Sequential(
                nn.Linear(FEATURE_COUNT, 512), nn.Softsign(),
                nn.Linear(512, 256), nn.Softsign(),
                nn.Linear(256, 128), nn.Softsign())
            self.gru = nn.GRU(128, N_NEURON, batch_first=True)
            self.res_gru = nn.Linear(128, N_NEURON)
            self.ln = nn.LayerNorm(N_NEURON)
            self.prediction_head = nn.Sequential(
                nn.Linear(N_NEURON, 8), nn.Softsign(), nn.Linear(8, 1))

        def forward(self, x, hidden=None):
            feature = self.feature_layers(x) + self.proj(x)
            recurrent, hidden = self.gru(feature, hidden)
            encoded = self.ln(recurrent + self.res_gru(feature))
            return self.prediction_head(encoded).squeeze(-1), hidden

    return LegacyMidMixSSE()


def load_bundle(bundle_root):
    import torch
    checkpoint = torch.load(bundle_root + "/model/best.pt", map_location="cpu")
    model = build_model()
    model.load_state_dict(checkpoint["model"], strict=True)
    model.eval()
    return model
