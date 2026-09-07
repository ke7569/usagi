#!/usr/bin/env python3
"""Reproduce the mix153060 SZE factors and CPU model from a handoff bundle."""

import argparse
import hashlib
import json
import math
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.ipc as ipc


DAY_US = 86_400_000_000
MORNING_OPEN_US = (9 * 60 + 30) * 60 * 1_000_000
MORNING_CLOSE_US = (11 * 60 + 30) * 60 * 1_000_000
AFTERNOON_OPEN_US = 13 * 60 * 60 * 1_000_000
FEATURE_END_US = (14 * 60 + 57) * 60 * 1_000_000
CHANGE_START_US = (9 * 60 * 60 + 31 * 60) * 1_000_000
CLOSE_US = 15 * 60 * 60 * 1_000_000
TIME_TRIGGER_US = 100 * 1_000_000
CHANGE_MIN_VOLUME = 100
MAX_CHANGE_LOCAL_GAP_US = 10_000
YOUNG_ORDER_AGE_SECONDS = 30.0
TOP_LEVELS = 10
MODEL_DEPTH = 5


FACTOR_NAMES = (
    "factor_hermes_permille",
    "factor_tr_sqrt_positive_ha3",
    "factor_mid_return_permille",
    "factor_bid_volume_change_ratio",
    "factor_ask_volume_change_ratio",
    "factor_weighted_return_permille_1",
    "factor_weighted_return_permille_2",
    "factor_weighted_return_permille_3",
    "factor_weighted_return_permille_4",
    "factor_weighted_return_permille_5",
    "factor_weighted_ask_permille",
    "factor_weighted_bid_permille",
    "factor_weighted_ask_return_permille",
    "factor_weighted_bid_return_permille",
    "factor_weighted_volume_imbalance",
    "factor_liquidity_ask_l1_share",
    "factor_liquidity_bid_l1_share",
    "factor_positive_order_flow_ha3",
    "factor_negative_order_flow_ha3",
    "factor_market_flow_ha3",
    "factor_cancel_buy_flow_ha3",
    "factor_cancel_sell_flow_ha3",
    "factor_positive_trade_ha3",
    "factor_negative_trade_ha3",
    "factor_positive_fill_rate",
    "factor_negative_fill_rate",
    "factor_order_flow_imbalance",
    "factor_cfr_imbalance_ha3",
    "factor_book_fixdist_weighted_1pct",
    "factor_book_fixdist_weighted_5pct",
    "factor_book_avg_size_imbalance",
    "factor_book_avg_size_imbalance_l1",
    "factor_book_avg_size_imbalance_l5",
    "factor_book_count_imbalance",
    "factor_book_count_imbalance_l1",
    "factor_book_count_imbalance_l5",
    "factor_book_life_imbalance",
    "factor_book_life_imbalance_l1",
    "factor_book_life_imbalance_l5",
    "factor_max_bid_distance_ratio",
    "factor_max_ask_distance_ratio",
    "factor_max_vol_distance_imbalance",
    "factor_book_young_imbalance_1pct",
    "factor_book_fixdist_hermes",
    "factor_time_of_day_progress",
    "factor_is_amount_trigger",
    "factor_sample_gap_log1p_seconds",
    "factor_open_to_now_return",
    "factor_cum_amount_to_history_3d",
    "factor_history_volatility_20d",
)


def read_arrow(path: Path) -> pa.Table:
    with pa.memory_map(str(path), "r") as source:
        return ipc.open_file(source).read_all()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def instrument_text(value: object) -> str:
    if isinstance(value, bytes):
        return value.rstrip(b"\0").decode("ascii")
    return str(value)


def price_tick(price: float) -> int:
    if not math.isfinite(price) or price <= 0.0:
        return 0
    return int(math.floor(price * 100.0 + 0.5))


def tick_price(tick: int) -> float:
    # minimatch's tick_to_f64 multiplies by the tick size. The one-ULP
    # difference from division is observable at the tr_sqrt clamp boundary.
    return tick * 0.01


def safe_div(numerator: float, denominator: float) -> float:
    return 0.0 if abs(denominator) < 1e-12 else numerator / denominator


def per_mille(delta: float, reference: float) -> float:
    return safe_div(delta, reference) * 1000.0


def time_of_day(epoch_us: int) -> int:
    return epoch_us % DAY_US


def session_id(epoch_us: int) -> Optional[int]:
    tod = time_of_day(epoch_us)
    if MORNING_OPEN_US <= tod <= MORNING_CLOSE_US:
        return 1
    if AFTERNOON_OPEN_US <= tod < FEATURE_END_US:
        return 2
    return None


def same_session(left: int, right: int) -> bool:
    value = session_id(left)
    return value is not None and session_id(right) == value


@dataclass
class Order:
    order_id: int
    buy: bool
    tick: int
    remaining: int
    insert_us: int


@dataclass
class Level:
    tick: int
    volume: int
    count: int
    orders: Tuple[Order, ...]


class Book:
    def __init__(self) -> None:
        self.orders: Dict[int, Order] = {}
        self.sides: Tuple[Dict[int, Dict[int, Order]], Dict[int, Dict[int, Order]]] = (
            defaultdict(dict),
            defaultdict(dict),
        )

    @staticmethod
    def side_index(buy: bool) -> int:
        return 1 if buy else 0

    def add(self, row: dict) -> None:
        buy = int(row["direction"]) == 1
        side = self.side_index(buy)
        tick = price_tick(float(row["price"]))
        kind = int(row["type_char"])
        contra = self.side_index(not buy)
        if kind == ord("1") and self.sides[contra]:
            # SZE market remainders rest at the contra best that was visible
            # when the order entered, not at the price carried by the message.
            tick = min(self.sides[contra]) if buy else max(self.sides[contra])
        elif kind == ord("U") and self.sides[side]:
            tick = max(self.sides[side]) if buy else min(self.sides[side])
        order = Order(
            int(row["app_seq"]), buy, tick, int(row["volume"]), int(row["ex_time"])
        )
        self.orders[order.order_id] = order
        self.sides[side][tick][order.order_id] = order

    def remove(self, order_id: int, quantity: Optional[int] = None) -> None:
        order = self.orders.get(int(order_id))
        if order is None:
            return
        removed = order.remaining if quantity is None else min(order.remaining, int(quantity))
        order.remaining -= removed
        if order.remaining > 0:
            return
        side = self.side_index(order.buy)
        del self.orders[order.order_id]
        del self.sides[side][order.tick][order.order_id]
        if not self.sides[side][order.tick]:
            del self.sides[side][order.tick]

    def apply_trade(self, row: dict) -> None:
        if int(row["type_char"]) == ord("4"):
            self.remove(int(row["buy_no"]) or int(row["sell_no"]))
            return
        quantity = int(row["volume"])
        self.remove(int(row["buy_no"]), quantity)
        self.remove(int(row["sell_no"]), quantity)

    def levels(self, buy: bool, limit: Optional[int] = None) -> List[Level]:
        side = self.sides[self.side_index(buy)]
        ticks = sorted(side, reverse=buy)
        if limit is not None:
            ticks = ticks[:limit]
        result: List[Level] = []
        for tick in ticks:
            orders = tuple(side[tick].values())
            result.append(Level(tick, sum(x.remaining for x in orders), len(orders), orders))
        return result


@dataclass
class Cut:
    cut_index: int
    app_seq: int
    ex_time_us: int
    local_time_us: int
    mid: float
    mid_source: int
    limit_state: int
    last_price: float
    turnover: float
    volume: int
    bids: Tuple[Level, ...]
    asks: Tuple[Level, ...]

    @property
    def has_two_sided_l1(self) -> bool:
        return bool(self.bids and self.asks and self.bids[0].volume > 0 and self.asks[0].volume > 0)

    @property
    def best_bid_price(self) -> float:
        return tick_price(self.bids[0].tick) if self.bids else 0.0

    @property
    def best_ask_price(self) -> float:
        return tick_price(self.asks[0].tick) if self.asks else 0.0

    @property
    def best_bid_volume(self) -> int:
        return self.bids[0].volume if self.bids else 0

    @property
    def best_ask_volume(self) -> int:
        return self.asks[0].volume if self.asks else 0


@dataclass
class StaticInputs:
    instrument: str
    avg_amount: float
    turnover_threshold: float
    pre_close: float
    upper_limit: float
    lower_limit: float
    history_volatility_20d: float
    history_volatility_source: str


@dataclass
class FlowAccumulator:
    turnover: float = 0.0
    positive_order_amount: float = 0.0
    negative_order_amount: float = 0.0
    market_buy_amount: float = 0.0
    market_sell_amount: float = 0.0
    cancel_buy_volume: float = 0.0
    cancel_sell_volume: float = 0.0
    cancel_buy_amount: float = 0.0
    cancel_sell_amount: float = 0.0
    positive_trade_amount: float = 0.0
    negative_trade_amount: float = 0.0
    buy_order_volume: float = 0.0
    sell_order_volume: float = 0.0
    buy_filled_volume: float = 0.0
    sell_filled_volume: float = 0.0
    buy_filled_amount: float = 0.0
    sell_filled_amount: float = 0.0

    @staticmethod
    def amount(price: float, volume: float) -> float:
        if math.isfinite(price) and price > 0.0 and math.isfinite(volume) and volume > 0.0:
            return price * volume
        return 0.0

    def on_order(self, start: Cut, row: dict) -> None:
        volume = max(int(row["volume"]), 0)
        price = float(row["price"])
        amount = self.amount(price, volume)
        is_market = bool(row["is_market"])
        if int(row["direction"]) == 1:
            self.buy_order_volume += volume
            if is_market or price > start.best_ask_price + 1e-6:
                self.market_buy_amount += amount
            if start.best_bid_volume != 0:
                bench = start.best_bid_price
                limit = start.best_ask_price if start.best_ask_volume else bench + 0.01
                if price < limit - 1e-6 and price > 0.0 and bench > 0.0:
                    self.positive_order_amount += amount * (
                        1.0 - math.tanh((bench / price - 1.0) * 100.0)
                    )
        else:
            self.sell_order_volume += volume
            if is_market or price < start.best_bid_price - 1e-6:
                self.market_sell_amount += amount
            if start.best_ask_volume != 0:
                bench = start.best_ask_price
                limit = start.best_bid_price if start.best_bid_volume else bench - 0.01
                if price > limit + 1e-6 and price > 0.0 and bench > 0.0:
                    self.negative_order_amount += amount * (
                        1.0 - math.tanh((price / bench - 1.0) * 100.0)
                    )

    def on_trade(self, start: Cut, row: dict, cancel_price: float) -> None:
        volume = max(int(row["volume"]), 0)
        buy_no = int(row["buy_no"])
        sell_no = int(row["sell_no"])
        if int(row["type_char"]) == ord("4"):
            amount = self.amount(cancel_price, volume)
            if sell_no == 0:
                self.cancel_buy_volume += volume
                self.cancel_buy_amount += amount
            elif buy_no == 0:
                self.cancel_sell_volume += volume
                self.cancel_sell_amount += amount
            return
        price = float(row["price"])
        amount = self.amount(price, volume)
        self.turnover += amount
        if buy_no > sell_no:
            self.positive_trade_amount += amount
            self.buy_filled_volume += volume
            self.buy_filled_amount += amount
        elif buy_no < sell_no:
            self.negative_trade_amount += amount
            self.sell_filled_volume += volume
            self.sell_filled_amount += amount

    def factors(self, history_amount: float) -> Dict[str, float]:
        bench = max(history_amount, 1.0)
        buy_cfr = safe_div(
            self.buy_filled_amount,
            self.buy_filled_amount + self.cancel_buy_amount + 1.0,
        )
        sell_cfr = safe_div(
            self.sell_filled_amount,
            self.sell_filled_amount + self.cancel_sell_amount + 1.0,
        )
        return {
            "factor_positive_order_flow_ha3": self.positive_order_amount / bench,
            "factor_negative_order_flow_ha3": self.negative_order_amount / bench,
            "factor_market_flow_ha3": (self.market_buy_amount - self.market_sell_amount) / bench,
            "factor_cancel_buy_flow_ha3": self.cancel_buy_amount / bench,
            "factor_cancel_sell_flow_ha3": self.cancel_sell_amount / bench,
            "factor_positive_trade_ha3": self.positive_trade_amount / bench,
            "factor_negative_trade_ha3": self.negative_trade_amount / bench,
            "factor_positive_fill_rate": min(
                max(safe_div(self.buy_filled_volume, self.buy_order_volume), 0.0), 1.0
            ),
            "factor_negative_fill_rate": min(
                max(safe_div(self.sell_filled_volume, self.sell_order_volume), 0.0), 1.0
            ),
            "factor_order_flow_imbalance": safe_div(
                self.buy_order_volume - self.sell_order_volume,
                self.buy_order_volume + self.sell_order_volume + 1.0,
            ),
            "factor_cfr_imbalance_ha3": safe_div(
                buy_cfr - sell_cfr, buy_cfr + sell_cfr + 1.0
            ),
        }


@dataclass
class SamplingWindow:
    start: Cut
    flow: FlowAccumulator = field(default_factory=FlowAccumulator)


def level_price(levels: Sequence[Level], index: int) -> float:
    return tick_price(levels[index].tick) if index < len(levels) else 0.0


def level_volume(levels: Sequence[Level], index: int) -> int:
    return levels[index].volume if index < len(levels) else 0


def classic_hermes(cut: Cut) -> float:
    if cut.best_bid_volume == 0 or cut.best_ask_volume == 0:
        return cut.last_price
    total = 0.0
    weight_sum = 0.0
    for index, weight in enumerate((5.0, 4.0, 3.0, 2.0, 1.0)):
        bid_volume = level_volume(cut.bids, index)
        ask_volume = level_volume(cut.asks, index)
        if bid_volume == 0 or ask_volume == 0:
            break
        total += (
            level_price(cut.asks, index) * bid_volume
            + level_price(cut.bids, index) * ask_volume
        ) / (ask_volume + bid_volume) * weight
        weight_sum += weight
    return safe_div(total, weight_sum)


def weighted_distance(cut: Cut, buy: bool) -> float:
    levels = cut.bids if buy else cut.asks
    dot = sum(level_price(levels, i) * level_volume(levels, i) for i in range(MODEL_DEPTH))
    volume = sum(level_volume(levels, i) for i in range(MODEL_DEPTH))
    if volume == 0:
        return 0.0
    weighted = dot / volume
    return cut.mid - weighted if buy else weighted - cut.mid


def volume_change(start: Cut, current: Cut, buy: bool) -> float:
    current_bid = current.best_bid_volume
    current_ask = current.best_ask_volume
    front = current_bid + current_ask
    if front == 0:
        return 0.0
    if buy:
        delta = current.best_bid_price - start.best_bid_price
        if delta < -1e-6:
            raw = -start.best_bid_volume / front
        elif delta > 1e-6:
            raw = (start.best_ask_volume + current_bid) / front
        else:
            raw = (current_bid - start.best_bid_volume) / front
    else:
        delta = current.best_ask_price - start.best_ask_price
        if delta < -1e-6:
            raw = (start.best_bid_volume + current_ask) / front
        elif delta > 1e-6:
            raw = -start.best_ask_volume / front
        else:
            raw = (current_ask - start.best_ask_volume) / front
    return min(max(raw, -200.0), 200.0)


def weighted_returns(start: Cut, current: Cut) -> List[float]:
    if abs(start.limit_state) == 1 or abs(current.limit_state) == 1:
        return [0.0] * MODEL_DEPTH
    result: List[float] = []
    start_dot = current_dot = 0.0
    start_bid = start_ask = current_bid = current_ask = 0.0
    for index in range(MODEL_DEPTH):
        sb = level_volume(start.bids, index)
        sa = level_volume(start.asks, index)
        cb = level_volume(current.bids, index)
        ca = level_volume(current.asks, index)
        start_dot += level_price(start.bids, index) * sb + level_price(start.asks, index) * sa
        current_dot += level_price(current.bids, index) * cb + level_price(current.asks, index) * ca
        start_bid += sb
        start_ask += sa
        current_bid += cb
        current_ask += ca
        value = 0.0
        if start_bid > 0 and start_ask > 0 and current_bid > 0 and current_ask > 0:
            start_weighted = start_dot / (start_bid + start_ask)
            current_weighted = current_dot / (current_bid + current_ask)
            value = min(max(per_mille(current_weighted - start_weighted, current.mid), -100.0), 100.0)
        result.append(value)
    return result


def imbalance(ask: float, bid: float) -> float:
    return safe_div(ask - bid, ask + bid)


def in_band(levels: Sequence[Level], mid_tick: float, distance: float, ask: bool) -> List[Level]:
    # The reference computes the distance from the midpoint first and then
    # adds/subtracts it. Keeping that order avoids admitting an integer tick
    # that lies exactly on a fixed-distance boundary.
    span = mid_tick * distance
    bound = mid_tick + span if ask else mid_tick - span
    return [x for x in levels if x.tick < bound] if ask else [x for x in levels if x.tick > bound]


def avg_size(levels: Sequence[Level]) -> float:
    return safe_div(sum(x.volume for x in levels), sum(x.count for x in levels))


def average_life(levels: Sequence[Level], now_us: int) -> float:
    orders = [order for level in levels for order in level.orders]
    if not orders:
        return 0.0
    age_sum_us = sum(now_us - order.insert_us for order in orders)
    return age_sum_us / 1_000_000.0 / len(orders)


def max_volume_level(levels: Sequence[Level]) -> Optional[Level]:
    result: Optional[Level] = None
    for level in levels:
        if result is None or level.volume >= result.volume:
            result = level
    return result


def weighted_band(levels: Sequence[Level], mid_tick: float, distance: float, ask: bool) -> Tuple[bool, float]:
    maximum = mid_tick * distance
    bound = mid_tick + maximum if ask else mid_tick - maximum
    total = 0.0
    found = False
    for level in levels:
        inside = level.tick < bound if ask else level.tick > bound
        if inside:
            found = True
            delta = level.tick - mid_tick if ask else mid_tick - level.tick
            total += level.volume * (1.0 - delta / maximum)
    return found, total


def young_band(levels: Sequence[Level], now_us: int, mid_tick: float, distance: float, ask: bool) -> float:
    maximum = mid_tick * distance
    total = 0.0
    for level in levels:
        if (ask and level.tick < mid_tick) or (not ask and level.tick > mid_tick):
            break
        delta = abs(level.tick - mid_tick)
        if delta > maximum:
            continue
        weight = 1.0 - delta / maximum
        for order in level.orders:
            if (now_us - order.insert_us) / 1_000_000.0 <= YOUNG_ORDER_AGE_SECONDS:
                total += order.remaining * weight
    return total


def fixdist_hermes(bids: Sequence[Level], asks: Sequence[Level], mid_tick: float) -> float:
    maximum = mid_tick * 0.05
    ask_dot = ask_weight = bid_dot = bid_weight = 0.0
    for level in asks:
        delta = level.tick - mid_tick
        if level.tick < mid_tick or delta > maximum:
            continue
        price_weight = 1.0 - delta / maximum
        weight = level.volume * price_weight
        if weight > 0.0:
            ask_dot += level.tick * weight
            ask_weight += weight
    for level in bids:
        delta = mid_tick - level.tick
        if level.tick > mid_tick or delta > maximum:
            continue
        price_weight = 1.0 - delta / maximum
        weight = level.volume * price_weight
        if weight > 0.0:
            bid_dot += level.tick * weight
            bid_weight += weight
    if ask_weight == 0.0 and bid_weight == 0.0:
        return 0.0
    effective_ask = ask_dot / ask_weight if ask_weight else (asks[0].tick if asks else mid_tick)
    effective_bid = bid_dot / bid_weight if bid_weight else (bids[0].tick if bids else mid_tick)
    return min(max(((effective_ask + effective_bid) * 0.5 / mid_tick - 1.0) * 1000.0, -5.0), 5.0)


def book_factors(book: Book, current: Cut) -> Dict[str, float]:
    result = {name: 0.0 for name in FACTOR_NAMES[28:44]}
    if abs(current.limit_state) == 1 or not current.has_two_sided_l1:
        return result
    bids = book.levels(True)
    asks = book.levels(False)
    mid_tick = (bids[0].tick + asks[0].tick) * 0.5
    bid_1 = bids[:1]
    ask_1 = asks[:1]
    bid_5 = bids[:5]
    ask_5 = asks[:5]
    bid_band = in_band(bids, mid_tick, 0.1, False)
    ask_band = in_band(asks, mid_tick, 0.1, True)
    ask_w1_found, ask_w1 = weighted_band(asks, mid_tick, 0.01, True)
    bid_w1_found, bid_w1 = weighted_band(bids, mid_tick, 0.01, False)
    ask_w5_found, ask_w5 = weighted_band(asks, mid_tick, 0.05, True)
    bid_w5_found, bid_w5 = weighted_band(bids, mid_tick, 0.05, False)
    result.update(
        {
            "factor_book_fixdist_weighted_1pct": imbalance(ask_w1, bid_w1) if ask_w1_found and bid_w1_found else 0.0,
            "factor_book_fixdist_weighted_5pct": imbalance(ask_w5, bid_w5) if ask_w5_found and bid_w5_found else 0.0,
            "factor_book_avg_size_imbalance": imbalance(avg_size(ask_band), avg_size(bid_band)),
            "factor_book_avg_size_imbalance_l1": imbalance(avg_size(ask_1), avg_size(bid_1)),
            "factor_book_avg_size_imbalance_l5": imbalance(avg_size(ask_5), avg_size(bid_5)),
            "factor_book_count_imbalance": imbalance(sum(x.count for x in ask_band), sum(x.count for x in bid_band)),
            "factor_book_count_imbalance_l1": imbalance(sum(x.count for x in ask_1), sum(x.count for x in bid_1)),
            "factor_book_count_imbalance_l5": imbalance(sum(x.count for x in ask_5), sum(x.count for x in bid_5)),
            "factor_book_life_imbalance": imbalance(average_life(ask_band, current.ex_time_us), average_life(bid_band, current.ex_time_us)),
            "factor_book_life_imbalance_l1": imbalance(average_life(ask_1, current.ex_time_us), average_life(bid_1, current.ex_time_us)),
            "factor_book_life_imbalance_l5": imbalance(average_life(ask_5, current.ex_time_us), average_life(bid_5, current.ex_time_us)),
            "factor_book_fixdist_hermes": fixdist_hermes(bids, asks, mid_tick),
        }
    )
    max_bid = max_volume_level(bids)
    max_ask = max_volume_level(asks)
    if max_bid is not None:
        result["factor_max_bid_distance_ratio"] = safe_div(max_bid.tick - mid_tick, mid_tick)
    if max_ask is not None:
        result["factor_max_ask_distance_ratio"] = safe_div(max_ask.tick - mid_tick, mid_tick)
    if max_bid is not None and max_ask is not None and max_bid.volume + max_ask.volume != 0:
        ask_distance = safe_div(max_ask.tick - mid_tick, mid_tick)
        bid_distance = -safe_div(max_bid.tick - mid_tick, mid_tick)
        result["factor_max_vol_distance_imbalance"] = imbalance(ask_distance, bid_distance)
    ask_young = young_band(asks, current.ex_time_us, mid_tick, 0.01, True)
    bid_young = young_band(bids, current.ex_time_us, mid_tick, 0.01, False)
    result["factor_book_young_imbalance_1pct"] = (
        imbalance(ask_young, bid_young) if ask_young != 0.0 or bid_young != 0.0 else 0.0
    )
    return result


def timeline_factors(start: Cut, current: Cut, history_amount: float) -> Dict[str, float]:
    result: Dict[str, float] = {}
    mid = current.mid
    start_mid = start.mid
    current_locked = abs(current.limit_state) == 1
    valid_book = not current_locked and bool(current.bids) and bool(current.asks)
    weighted = weighted_returns(start, current)
    weighted_ask = weighted_distance(current, False)
    weighted_bid = weighted_distance(current, True)
    start_weighted_ask = weighted_distance(start, False)
    start_weighted_bid = weighted_distance(start, True)
    hermes = classic_hermes(current)
    result["factor_hermes_permille"] = (
        min(max(per_mille(hermes - mid, mid), -5.0), 5.0) if 0.01 <= mid <= 1e6 else 0.0
    )
    turnover = current.turnover - start.turnover
    volume = current.volume - start.volume
    tr_sqrt = 0.0
    if history_amount > 0.0 and turnover >= 1e-5 and abs(volume) >= 1e-5:
        spread = start.best_ask_price - start.best_bid_price
        if start.best_bid_price > 0.0 and start.best_ask_price > 0.0 and abs(spread) >= 1e-6:
            ratio = min(max((turnover / volume - start.mid) / spread, -0.5), 0.5)
            scaled = turnover / history_amount
            tr_sqrt = math.sqrt(scaled * (0.5 + ratio)) - math.sqrt(scaled * (0.5 - ratio))
    result["factor_tr_sqrt_positive_ha3"] = tr_sqrt
    result["factor_mid_return_permille"] = per_mille(mid - start_mid, mid)
    result["factor_bid_volume_change_ratio"] = volume_change(start, current, True)
    result["factor_ask_volume_change_ratio"] = volume_change(start, current, False)
    for index, value in enumerate(weighted, 1):
        result[f"factor_weighted_return_permille_{index}"] = value
    result["factor_weighted_ask_permille"] = per_mille(weighted_ask, mid)
    result["factor_weighted_bid_permille"] = per_mille(weighted_bid, mid)
    result["factor_weighted_ask_return_permille"] = per_mille(weighted_ask - start_weighted_ask, start_mid)
    result["factor_weighted_bid_return_permille"] = per_mille(weighted_bid - start_weighted_bid, start_mid)
    ask_weighted_volume = sum(level_volume(current.asks, i) * (5 - i) for i in range(MODEL_DEPTH))
    bid_weighted_volume = sum(level_volume(current.bids, i) * (5 - i) for i in range(MODEL_DEPTH))
    total_weighted = ask_weighted_volume + bid_weighted_volume
    result["factor_weighted_volume_imbalance"] = (
        ask_weighted_volume / total_weighted - 0.5 if valid_book and total_weighted > 0.0 else 0.0
    )
    ask_total = sum(level_volume(current.asks, i) for i in range(MODEL_DEPTH))
    bid_total = sum(level_volume(current.bids, i) for i in range(MODEL_DEPTH))
    result["factor_liquidity_ask_l1_share"] = safe_div(current.best_ask_volume, ask_total) if valid_book else 0.0
    result["factor_liquidity_bid_l1_share"] = safe_div(current.best_bid_volume, bid_total) if valid_book else 0.0
    return result


class Replay:
    def __init__(self, static: StaticInputs) -> None:
        self.static = static
        self.book = Book()
        self.order_prices: Dict[int, float] = {}
        self.window: Optional[SamplingWindow] = None
        self.open_state: Optional[Tuple[float, float]] = None
        self.last_accepted_ex_time: Optional[int] = None
        self.last_accepted_cut_index: Optional[int] = None
        self.cumulative_turnover = 0.0
        self.cumulative_volume = 0
        self.last_price = 0.0
        self.rows: List[dict] = []

    def make_cut(self, index: int, row: dict) -> Cut:
        bids = tuple(self.book.levels(True, TOP_LEVELS))
        asks = tuple(self.book.levels(False, TOP_LEVELS))
        best_bid = tick_price(bids[0].tick) if bids else 0.0
        best_ask = tick_price(asks[0].tick) if asks else 0.0
        two_sided = bool(bids and asks and bids[0].volume > 0 and asks[0].volume > 0)
        if two_sided:
            mid, mid_source = (best_bid + best_ask) * 0.5, 0
        elif self.last_price > 0.0:
            mid, mid_source = self.last_price, 1
        elif self.static.pre_close > 0.0:
            mid, mid_source = self.static.pre_close, 1
        elif best_bid > 0.0:
            mid, mid_source = best_bid, 1
        else:
            mid, mid_source = best_ask, 1
        upper = price_tick(self.static.upper_limit)
        lower = price_tick(self.static.lower_limit)
        last_tick = price_tick(self.last_price)
        bid_tick = bids[0].tick if bids else 0
        ask_tick = asks[0].tick if asks else 0
        if upper and not asks and (last_tick == upper or bid_tick == upper):
            state = 1
        elif lower and not bids and (last_tick == lower or ask_tick == lower):
            state = -1
        elif upper and upper in (last_tick, bid_tick, ask_tick):
            state = 2
        elif lower and lower in (last_tick, bid_tick, ask_tick):
            state = -2
        else:
            state = 0
        return Cut(
            index,
            int(row["app_seq"]),
            int(row["ex_time"]),
            int(row["timestamp"]),
            mid,
            mid_source,
            state,
            self.last_price,
            self.cumulative_turnover,
            self.cumulative_volume,
            bids,
            asks,
        )

    def mutate_trade(self, row: dict) -> None:
        self.book.apply_trade(row)
        if int(row["type_char"]) == ord("F"):
            price = float(row["price"])
            volume = int(row["volume"])
            self.cumulative_turnover += price * volume
            self.cumulative_volume += volume
            if price > 0.0:
                self.last_price = price

    def dispatch_order(self, row: dict, fills: Sequence[dict]) -> None:
        order_id = int(row["app_seq"])
        self.order_prices[order_id] = float(row["price"])
        if self.window is not None and same_session(self.window.start.ex_time_us, int(row["ex_time"])):
            self.window.flow.on_order(self.window.start, row)
        for trade in fills:
            self.dispatch_trade(trade)

    def dispatch_trade(self, row: dict) -> None:
        buy_no = int(row["buy_no"])
        sell_no = int(row["sell_no"])
        cancel_order = buy_no if buy_no != 0 else sell_no
        cancel_price = self.order_prices.get(cancel_order, float(row["price"]))
        if self.window is not None and same_session(self.window.start.ex_time_us, int(row["ex_time"])):
            self.window.flow.on_trade(self.window.start, row, cancel_price)
        for order_id in (buy_no, sell_no):
            if order_id > 0 and order_id not in self.book.orders:
                self.order_prices.pop(order_id, None)

    def process_frame(self, index: int, kind: str, row: dict, fills: Sequence[dict]) -> None:
        if kind == "order":
            self.book.add(row)
            for trade in fills:
                self.mutate_trade(trade)
        else:
            self.mutate_trade(row)
        # An OrderWithFills is one timeline frame, but its watermark advances
        # through the final attached fill rather than stopping at the order id.
        watermark = fills[-1] if kind == "order" and fills else row
        cut = self.make_cut(index, watermark)
        if self.open_state is None and session_id(cut.ex_time_us) is not None and cut.has_two_sided_l1:
            self.open_state = (cut.mid, cut.turnover)
        if kind == "order":
            self.dispatch_order(row, fills)
        else:
            self.dispatch_trade(row)
        self.maybe_emit(cut)

    def maybe_emit(self, cut: Cut) -> None:
        if self.window is None:
            self.window = SamplingWindow(cut)
            return
        if session_id(cut.ex_time_us) is None:
            return
        start = self.window.start
        if cut.cut_index <= start.cut_index or cut.ex_time_us == start.ex_time_us:
            return
        amount = self.window.flow.turnover >= self.static.turnover_threshold
        time_trigger = cut.ex_time_us - start.ex_time_us >= TIME_TRIGGER_US
        raw_change = abs(cut.mid - start.mid) > 1e-6 and cut.volume - start.volume >= CHANGE_MIN_VOLUME
        change = (
            raw_change
            and time_of_day(cut.ex_time_us) >= CHANGE_START_US
            and abs(cut.local_time_us - cut.ex_time_us) <= MAX_CHANGE_LOCAL_GAP_US
        )
        if not (amount or time_trigger or change):
            return
        if self.last_accepted_ex_time is not None and cut.ex_time_us // 1000 <= self.last_accepted_ex_time // 1000:
            return
        if self.last_accepted_cut_index is not None and cut.cut_index <= self.last_accepted_cut_index:
            self.window = SamplingWindow(cut)
            return
        factors = timeline_factors(start, cut, self.static.avg_amount)
        factors.update(self.window.flow.factors(self.static.avg_amount))
        factors.update(book_factors(self.book, cut))
        tod = time_of_day(cut.ex_time_us)
        factors["factor_time_of_day_progress"] = min(
            max(safe_div(tod - MORNING_OPEN_US, CLOSE_US - MORNING_OPEN_US), 0.0), 1.0
        )
        factors["factor_is_amount_trigger"] = 1.0 if amount else 0.0
        factors["factor_sample_gap_log1p_seconds"] = (
            math.log1p(max(cut.ex_time_us - self.last_accepted_ex_time, 0) / 1_000_000.0)
            if self.last_accepted_ex_time is not None
            else 0.0
        )
        if self.open_state is not None and self.open_state[0] > 0.0 and cut.mid > 0.0:
            factors["factor_open_to_now_return"] = math.log(cut.mid / self.open_state[0]) * 1000.0
            cumulative = max(cut.turnover - self.open_state[1], 0.0)
        else:
            factors["factor_open_to_now_return"] = 0.0
            cumulative = 0.0
        factors["factor_cum_amount_to_history_3d"] = math.log1p(cumulative / self.static.avg_amount)
        factors["factor_history_volatility_20d"] = self.static.history_volatility_20d
        output = {
            "row_in_stock_day": len(self.rows),
            "ex_time_micros": cut.ex_time_us,
            "app_seq": cut.app_seq,
            "cut_index": cut.cut_index,
            "window_start_ex_time_micros": start.ex_time_us,
            "window_start_app_seq": start.app_seq,
            "window_start_cut_index": start.cut_index,
            "window_turnover": self.window.flow.turnover,
            "window_start_mid_price": start.mid,
        }
        output.update({name: float(np.float32(factors[name])) for name in FACTOR_NAMES})
        self.rows.append(output)
        self.last_accepted_cut_index = cut.cut_index
        self.last_accepted_ex_time = cut.ex_time_us
        self.window = SamplingWindow(cut)


def timeline_events(orders: Sequence[dict], trades: Sequence[dict]) -> List[Tuple[str, dict, Sequence[dict]]]:
    order_by_id = {int(row["app_seq"]): row for row in orders}
    fills: Dict[int, List[dict]] = defaultdict(list)
    grouped_trade_ids = set()
    for trade in trades:
        aggressor = max(int(trade["buy_no"]), int(trade["sell_no"]))
        order = order_by_id.get(aggressor)
        if (
            int(trade["type_char"]) == ord("F")
            and order is not None
            and int(order["ex_time"]) == int(trade["ex_time"])
        ):
            fills[aggressor].append(trade)
            grouped_trade_ids.add(int(trade["app_seq"]))
    events: List[Tuple[str, dict, Sequence[dict]]] = [
        ("order", row, tuple(fills[int(row["app_seq"])])) for row in orders
    ]
    events.extend(
        ("trade", row, ())
        for row in trades
        if int(row["app_seq"]) not in grouped_trade_ids
    )
    events.sort(key=lambda value: int(value[1]["app_seq"]))
    return events


def reference_history_volatility(features: pa.Table, stock_order: int) -> float:
    selected = features.filter(pc.equal(features["selection_order"], stock_order))
    values = pc.unique(selected["factor_history_volatility_20d"]).to_pylist()
    if len(values) != 1:
        raise ValueError(f"stock_order={stock_order} has non-constant history volatility: {values}")
    return float(values[0])


def _window_rows(rows: Sequence[dict], start_ex_time: Optional[int], end_ex_time: Optional[int]) -> List[dict]:
    if start_ex_time is None and end_ex_time is None:
        return list(rows)
    return [
        row
        for row in rows
        if (start_ex_time is None or int(row["ex_time_micros"]) >= start_ex_time)
        and (end_ex_time is None or int(row["ex_time_micros"]) < end_ex_time)
    ]


def _expected_window(
    expected: pa.Table,
    stock_order: int,
    start_ex_time: Optional[int],
    end_ex_time: Optional[int],
) -> pa.Table:
    selected = expected.filter(pc.equal(expected["selection_order"], stock_order))
    if start_ex_time is not None:
        selected = selected.filter(pc.greater_equal(selected["ex_time_micros"], start_ex_time))
    if end_ex_time is not None:
        selected = selected.filter(pc.less(selected["ex_time_micros"], end_ex_time))
    return selected


def replay_stock(
    bundle: Path,
    stock: dict,
    static_row: dict,
    golden: pa.Table,
    start_ex_time: Optional[int] = None,
    end_ex_time: Optional[int] = None,
) -> Tuple[List[dict], dict]:
    instrument = str(stock["instrument_id"])
    stock_order = int(stock["selection_order"])
    raw_root = bundle / "raw/20260715/stocks" / instrument
    orders = read_arrow(raw_root / "order.arrow").to_pylist()
    trades = read_arrow(raw_root / "trade.arrow").to_pylist()
    history_volatility = reference_history_volatility(golden, stock_order)
    static = StaticInputs(
        instrument,
        float(static_row["avg_amount"]),
        float(static_row["turnover_threshold"]),
        float(static_row["pre_close"]),
        float(static_row["limit_price"]),
        float(static_row["stop_price"]),
        history_volatility,
        "golden/features.arrow:factor_history_volatility_20d",
    )
    replay = Replay(static)
    events = timeline_events(orders, trades)
    expected_window = _expected_window(
        golden, stock_order, start_ex_time, end_ex_time
    )
    frame_limit = len(events)
    if end_ex_time is not None and expected_window.num_rows > 0:
        frame_limit = int(expected_window["cut_index"][-1].as_py()) + 1
    processed_frames = 0
    for index, (kind, row, fills) in enumerate(events[:frame_limit]):
        replay.process_frame(index, kind, row, fills)
        processed_frames += 1
    window_rows = _window_rows(replay.rows, start_ex_time, end_ex_time)
    return window_rows, {
        "instrument_id": instrument,
        "frames": len(events),
        "frames_processed": processed_frames,
        "warmup_frames": processed_frames - sum(
            1 for kind, row, fills in events[:processed_frames]
            if start_ex_time is not None
            and int((fills[-1] if kind == "order" and fills else row)["ex_time"]) >= start_ex_time
        ),
        "orders": len(orders),
        "trades": len(trades),
        "rows": len(window_rows),
        "rows_full_replay": len(replay.rows),
        "window_start_ex_time_micros": start_ex_time,
        "window_end_ex_time_micros": end_ex_time,
        "history_volatility_20d": history_volatility,
        "history_volatility_source": static.history_volatility_source,
    }


def compare_rows(
    actual: Sequence[dict],
    expected: pa.Table,
    stock_order: int,
    start_ex_time: Optional[int] = None,
    end_ex_time: Optional[int] = None,
) -> dict:
    expected = _expected_window(expected, stock_order, start_ex_time, end_ex_time)
    identity = (
        "row_in_stock_day",
        "ex_time_micros",
        "app_seq",
        "cut_index",
        "window_start_ex_time_micros",
        "window_start_app_seq",
        "window_start_cut_index",
    )
    if len(actual) != expected.num_rows:
        return {
            "status": "mismatch",
            "actual_rows": len(actual),
            "expected_rows": expected.num_rows,
            "first_failure": "row_count",
        }
    expected_rows = expected.select(list(identity) + list(FACTOR_NAMES)).to_pylist()
    max_abs = {name: 0.0 for name in FACTOR_NAMES}
    first_failure = None
    differing_values = 0
    for index, (left, right) in enumerate(zip(actual, expected_rows)):
        for name in identity:
            if int(left[name]) != int(right[name]):
                first_failure = first_failure or {
                    "row": index,
                    "column": name,
                    "actual": left[name],
                    "expected": right[name],
                }
        for name in FACTOR_NAMES:
            delta = abs(float(left[name]) - float(right[name]))
            max_abs[name] = max(max_abs[name], delta)
            if delta != 0.0:
                differing_values += 1
                first_failure = first_failure or {
                    "row": index,
                    "column": name,
                    "actual": left[name],
                    "expected": right[name],
                    "abs_error": delta,
                }
    return {
        "status": "ok" if first_failure is None else "mismatch",
        "actual_rows": len(actual),
        "expected_rows": expected.num_rows,
        "differing_factor_values": differing_values,
        "max_factor_abs_error": max(max_abs.values()),
        "max_factor_abs_error_by_name": max_abs,
        "first_failure": first_failure,
    }


def numpy_model(bundle: Path, features: pa.Table) -> np.ndarray:
    weights = np.load(bundle / "model/state_dict.npz")
    outputs: List[np.ndarray] = []
    selection = np.asarray(features["selection_order"])
    for stock_order in range(5):
        mask = selection == stock_order
        values = np.column_stack([np.asarray(features[name])[mask] for name in FACTOR_NAMES]).astype(np.float32)
        mean = values.mean(axis=1, keepdims=True, dtype=np.float32)
        centered = values - mean
        variance = (centered * centered).mean(axis=1, keepdims=True, dtype=np.float32)
        normalized = centered / np.sqrt(variance + np.float32(1e-5), dtype=np.float32)
        normalized = normalized * weights["input_norm.weight"] + weights["input_norm.bias"]
        projected = normalized @ weights["input_proj.weight"].T + weights["input_proj.bias"]
        hidden = [np.zeros(128, dtype=np.float32), np.zeros(128, dtype=np.float32)]
        stock_output = []
        for projected_row in projected:
            layer_input = projected_row
            for layer in range(2):
                input_gates = weights[f"gru.weight_ih_l{layer}"] @ layer_input + weights[f"gru.bias_ih_l{layer}"]
                hidden_gates = weights[f"gru.weight_hh_l{layer}"] @ hidden[layer] + weights[f"gru.bias_hh_l{layer}"]
                input_reset, input_update, input_new = np.split(input_gates, 3)
                hidden_reset, hidden_update, hidden_new = np.split(hidden_gates, 3)
                with np.errstate(over="ignore"):
                    reset = 1.0 / (1.0 + np.exp(-(input_reset + hidden_reset)))
                    update = 1.0 / (1.0 + np.exp(-(input_update + hidden_update)))
                new = np.tanh(input_new + reset * hidden_new)
                hidden[layer] = (1.0 - update) * new + update * hidden[layer]
                layer_input = hidden[layer]
            stock_output.append(layer_input.copy())
        gru_output = np.asarray(stock_output, dtype=np.float32)
        outputs.append(gru_output @ weights["head.weight"][0] + weights["head.bias"][0])
    return np.concatenate(outputs)


def torch_model(bundle: Path, features: pa.Table) -> Tuple[np.ndarray, str]:
    try:
        import torch
        import torch.nn as nn
    except ImportError as exc:
        raise RuntimeError(
            "PyTorch is unavailable; use --model-backend numpy or --skip-model"
        ) from exc

    class MixModel(nn.Module):
        def __init__(self) -> None:
            super().__init__()
            self.input_norm = nn.LayerNorm(50, eps=1e-5)
            self.input_proj = nn.Linear(50, 128)
            self.gru = nn.GRU(128, 128, num_layers=2, batch_first=True, dropout=0.0)
            self.head = nn.Linear(128, 1)

        def forward(self, values):
            normalized = self.input_norm(values)
            projected = self.input_proj(normalized)
            output, _ = self.gru(projected)
            return self.head(output)

    weights = np.load(bundle / "model/state_dict.npz", allow_pickle=False)
    model = MixModel()
    model.load_state_dict(
        {name: torch.from_numpy(np.array(weights[name], copy=True)) for name in weights.files}
    )
    model.eval()
    selection = np.asarray(features["selection_order"])
    outputs = []
    with torch.inference_mode():
        for stock_order in range(5):
            mask = selection == stock_order
            values = np.column_stack(
                [np.asarray(features[name])[mask] for name in FACTOR_NAMES]
            ).astype(np.float32, copy=False)
            prediction = model(torch.from_numpy(values).unsqueeze(0))
            outputs.append(prediction.squeeze(0).squeeze(-1).numpy())
    return np.concatenate(outputs), str(torch.__version__)


def write_reproduced_features(path: Path, rows: Sequence[dict]) -> None:
    identity = (
        "row_in_stock_day",
        "ex_time_micros",
        "app_seq",
        "cut_index",
        "window_start_ex_time_micros",
        "window_start_app_seq",
        "window_start_cut_index",
    )
    arrays = [
        pa.array([row["selection_order"] for row in rows], type=pa.int8()),
        pa.array(
            [row["instrument_id"].encode("ascii").ljust(16, b"\0") for row in rows],
            type=pa.binary(16),
        ),
    ]
    names = ["selection_order", "instrument_id"]
    int32_identity = {"row_in_stock_day", "cut_index", "window_start_cut_index"}
    for name in identity:
        value_type = pa.int32() if name in int32_identity else pa.int64()
        arrays.append(pa.array([row[name] for row in rows], type=value_type))
        names.append(name)
    for name in FACTOR_NAMES:
        arrays.append(pa.array([row[name] for row in rows], type=pa.float32()))
        names.append(name)
    table = pa.Table.from_arrays(arrays, names=names)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with pa.OSFile(str(temporary), "wb") as sink:
        with ipc.new_file(sink, table.schema) as writer:
            writer.write_table(table)
    temporary.replace(path)


def load_static_rows(bundle: Path) -> Dict[str, dict]:
    rows = read_arrow(bundle / "factors/stock_day_selected.arrow").to_pylist()
    return {instrument_text(row["instrument_id"]): row for row in rows}


def run(
    bundle: Path,
    instruments: Optional[Iterable[str]],
    model_backend: str = "numpy",
    factors_output: Optional[Path] = None,
    start_ex_time: Optional[int] = None,
    end_ex_time: Optional[int] = None,
) -> dict:
    if (start_ex_time is not None and end_ex_time is not None and
            start_ex_time >= end_ex_time):
        raise ValueError("start exchange time must be before end exchange time")
    selection = json.loads((bundle / "raw/20260715/selection.json").read_text())
    golden = read_arrow(bundle / "golden/features.arrow")
    predictions = read_arrow(bundle / "golden/predictions.arrow")
    static_rows = load_static_rows(bundle)
    requested = set(instruments or ())
    stock_reports = []
    all_rows: List[dict] = []
    for stock in selection["stocks"]:
        instrument = str(stock["instrument_id"])
        if requested and instrument not in requested:
            continue
        rows, replay_report = replay_stock(
            bundle,
            stock,
            static_rows[instrument],
            golden,
            start_ex_time=start_ex_time,
            end_ex_time=end_ex_time,
        )
        comparison = compare_rows(
            rows,
            golden,
            int(stock["selection_order"]),
            start_ex_time=start_ex_time,
            end_ex_time=end_ex_time,
        )
        replay_report["factor_comparison"] = comparison
        stock_reports.append(replay_report)
        all_rows.extend(
            dict(row, selection_order=int(stock["selection_order"]), instrument_id=instrument)
            for row in rows
        )
    result = {
        "schema": "mix153060-reproduction-v1",
        "bundle": str(bundle.resolve()),
        "factor_contract": list(FACTOR_NAMES),
        "stocks": stock_reports,
        "factor_parity": all(x["factor_comparison"]["status"] == "ok" for x in stock_reports),
        "reproduced_factor_rows": len(all_rows),
        "replay_window": {
            "start_ex_time_micros": start_ex_time,
            "end_ex_time_micros": end_ex_time,
            "warmup_required": start_ex_time is not None,
        },
        "model_contract_tolerance": 1e-6,
        "static_input_gap": {
            "field": "factor_history_volatility_20d",
            "missing_from": "factors/stock_day_selected.arrow and raw/20260715",
            "reproduction_source": "golden/features.arrow constant per stock",
        },
    }
    if factors_output is not None:
        write_reproduced_features(factors_output, all_rows)
        result["reproduced_features_arrow"] = str(factors_output.resolve())
        result["reproduced_features_sha256"] = sha256(factors_output)
    if model_backend != "skip":
        if model_backend == "torch":
            cpu_prediction, torch_version = torch_model(bundle, golden)
            backend_name = "torch-cpu-float32"
            result["torch_version"] = torch_version
        else:
            cpu_prediction = numpy_model(bundle, golden)
            backend_name = "numpy-float32-diagnostic"
        expected_cpu = np.asarray(predictions["pred_cpu_fp32"])
        if start_ex_time is not None or end_ex_time is not None:
            mask = np.zeros(golden.num_rows, dtype=bool)
            selection_values = np.asarray(golden["selection_order"])
            ex_time_values = np.asarray(golden["ex_time_micros"])
            requested_orders = set(
                int(stock["selection_order"])
                for stock in selection["stocks"]
                if not requested or str(stock["instrument_id"]) in requested
            )
            mask = np.isin(selection_values, list(requested_orders))
            if start_ex_time is not None:
                mask &= ex_time_values >= start_ex_time
            if end_ex_time is not None:
                mask &= ex_time_values < end_ex_time
            expected_cpu = expected_cpu[mask]
            cpu_prediction = cpu_prediction[mask]
        prediction_error = np.abs(cpu_prediction - expected_cpu)
        max_error_index = int(np.argmax(prediction_error)) if prediction_error.size else -1
        result.update(
            {
                "model_backend": backend_name,
                "model_rows": int(cpu_prediction.size),
                "model_cpu_max_abs_error": float(prediction_error.max())
                if prediction_error.size
                else 0.0,
                "model_cpu_mean_abs_error": float(prediction_error.mean())
                if prediction_error.size
                else 0.0,
                "model_cpu_values_over_tolerance": int(
                    np.count_nonzero(prediction_error > 1e-6)
                ),
                "model_cpu_max_error_row": max_error_index,
                "model_within_contract_tolerance": bool(np.all(prediction_error <= 1e-6)),
            }
        )
    else:
        result.update({"model_backend": "skipped", "model_within_contract_tolerance": None})
    result["overall_parity"] = result["factor_parity"] and (
        result["model_within_contract_tolerance"] is not False
    )
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle-root", required=True, type=Path)
    parser.add_argument("--instrument", action="append")
    parser.add_argument("--skip-model", action="store_true")
    parser.add_argument(
        "--model-backend",
        choices=("numpy", "torch", "skip"),
        default="numpy",
    )
    parser.add_argument("--factors-output", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--start-ex-time-us",
        type=int,
        help="inclusive exchange-time lower bound; replay still warms from the first event",
    )
    parser.add_argument(
        "--end-ex-time-us",
        type=int,
        help="exclusive exchange-time upper bound",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    model_backend = "skip" if args.skip_model else args.model_backend
    report = run(
        args.bundle_root.resolve(),
        args.instrument,
        model_backend=model_backend,
        factors_output=args.factors_output,
        start_ex_time=args.start_ex_time_us,
        end_ex_time=args.end_ex_time_us,
    )
    encoded = json.dumps(report, indent=2, sort_keys=True)
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)
    return 0 if report["overall_parity"] else 1


if __name__ == "__main__":
    sys.exit(main())
