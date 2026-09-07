#!/usr/bin/env python3
"""Build a causal daily mix153060 Shenzhen live configuration.

The producer of the normalized daily CSV is intentionally outside this tool.  This
script only consumes a small, auditable EOD contract and publishes dated artifacts
under t0-main/build/configs.  It uses the Python 3.6 standard library exclusively.
"""

from __future__ import print_function

import argparse
import csv
import datetime
import glob
import hashlib
import json
import math
import os
import re
import sys
import tempfile


DEFAULT_HISTORY_AMOUNT_DAYS = 5
DEFAULT_HISTORY_VOLATILITY_DAYS = 20
DEFAULT_MIN_AMOUNT_OBSERVATIONS = 3
DEFAULT_MODE = "hp-shadow"
SZE_STRATEGY_LIBRARY_FILENAME = "libt0_strategy_sze.so"
DEFAULT_SZE_STRATEGY_LIBRARY = "./" + SZE_STRATEGY_LIBRARY_FILENAME
VALID_MODES = ("hp-shadow", "hp-realtime")
DATE_RE = re.compile(r"(\d{8})")
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
SOURCE_INDEX_MIN = 0
SOURCE_INDEX_MAX = 32767
INT32_MIN = -2147483648
INT32_MAX = 2147483647
OUTPUT_FILE_MODE = 0o644


class ConfigError(Exception):
    """A validation or publication error that should fail the daily job."""


def _path(value):
    return os.path.abspath(os.path.expanduser(str(value)))


def _parse_date(value, field="date"):
    text = str(value or "").strip().replace("-", "")
    if not re.match(r"^\d{8}$", text):
        raise ConfigError("{} must be YYYYMMDD: {}".format(field, value))
    try:
        datetime.datetime.strptime(text, "%Y%m%d")
    except ValueError:
        raise ConfigError("{} is not a calendar date: {}".format(field, value))
    return text


def _normalize_code(value):
    text = str(value or "").strip().upper()
    text = text.replace("SZE:", "").replace("SZ:", "")
    if text.endswith(".SZ"):
        text = text[:-3]
    elif text.endswith("SZ") and text[:-2].isdigit():
        text = text[:-2]
    text = text.strip()
    if text.isdigit():
        text = text.zfill(6)
    if not re.match(r"^\d{6}$", text):
        raise ConfigError("invalid Shenzhen instrument code: {}".format(value))
    return text + ".SZ"


def _field_map(row):
    return {str(key).strip().lower(): value for key, value in row.items()}


def _raw_value(fields, aliases):
    for alias in aliases:
        value = fields.get(alias.lower())
        if value is not None and str(value).strip() != "":
            return str(value).strip(), alias
    return None, None


def _float_value(fields, aliases, context, required=True):
    raw, source = _raw_value(fields, aliases)
    if raw is None:
        if required:
            raise ConfigError("missing {} for {}".format(aliases[0], context))
        return None, None
    try:
        value = float(raw)
    except (TypeError, ValueError):
        raise ConfigError("invalid {} for {}: {}".format(aliases[0], context, raw))
    if not math.isfinite(value):
        raise ConfigError("non-finite {} for {}".format(aliases[0], context))
    return value, source


def _amount_value(fields, context, required=False):
    aliases = (
        ("amount_cny", 1.0),
        ("turnover_cny", 1.0),
        ("amount", 1.0),
        ("turnover", 1.0),
        ("amount_10k_cny", 10000.0),
        ("turnover_10k_cny", 10000.0),
        # Wind's S_DQ_AMOUNT is expressed in thousand CNY.
        ("s_dq_amount", 1000.0),
    )
    raw, source = _raw_value(fields, [item[0] for item in aliases])
    if raw is None:
        if required:
            raise ConfigError("missing amount for {}".format(context))
        return None, None
    try:
        value = float(raw)
    except (TypeError, ValueError):
        raise ConfigError("invalid amount for {}: {}".format(context, raw))
    multiplier = dict(aliases)[source]
    value *= multiplier
    if not math.isfinite(value):
        raise ConfigError("non-finite amount for {}".format(context))
    return value, source


def _limit_value(fields, upper, context):
    if upper:
        aliases = (
            "upper_limit", "limit_up", "limit_price", "s_dq_limit", "s_dq_limit_up",
            "hp_upper_price", "hpupperprice",
        )
    else:
        aliases = (
            "lower_limit", "limit_down", "stop_price", "s_dq_stopping", "s_dq_limit_down",
            "hp_lower_price", "hplowerprice",
        )
    return _float_value(fields, aliases, context, required=True)[0]


def _date_from_filename(path):
    match = DATE_RE.search(os.path.basename(path))
    return _parse_date(match.group(1), "filename date") if match else None


def _record_from_row(row, path, row_number):
    fields = _field_map(row)
    raw_date, _ = _raw_value(
        fields,
        ("date", "trade_date", "trade_dt", "trading_date", "datetime"),
    )
    date = _parse_date(raw_date, "date") if raw_date else _date_from_filename(path)
    if date is None:
        raise ConfigError("missing date in {} row {}".format(path, row_number))
    raw_code, _ = _raw_value(
        fields,
        ("code", "instrument_id", "instrument", "symbol", "wind_code", "s_info_windcode"),
    )
    if raw_code is None:
        raise ConfigError("missing code in {} row {}".format(path, row_number))
    code = _normalize_code(raw_code)
    context = "{} {}".format(code, date)
    close = _float_value(
        fields,
        ("close", "s_dq_close", "close_price"),
        context,
        required=False,
    )[0]
    pre_close = _float_value(
        fields,
        ("pre_close", "preclose", "s_dq_preclose", "previous_close"),
        context,
        required=True,
    )[0]
    amount, amount_source = _amount_value(fields, context, required=False)
    upper = _limit_value(fields, True, context) if _raw_value(
        fields,
        (
            "upper_limit", "limit_up", "limit_price", "s_dq_limit", "s_dq_limit_up",
            "hp_upper_price", "hpupperprice",
        ),
    )[0] is not None else None
    lower = _limit_value(fields, False, context) if _raw_value(
        fields,
        (
            "lower_limit", "limit_down", "stop_price", "s_dq_stopping", "s_dq_limit_down",
            "hp_lower_price", "hplowerprice",
        ),
    )[0] is not None else None
    name = _raw_value(fields, ("name", "security_name", "s_info_name"))[0] or ""
    return {
        "date": date,
        "code": code,
        "name": name,
        "close": close,
        "pre_close": pre_close,
        "amount": amount,
        "amount_source": amount_source,
        "upper_limit": upper,
        "lower_limit": lower,
        "source": _path(path),
        "row": row_number,
    }


def _record_signature(record):
    return tuple(record.get(key) for key in (
        "date", "code", "close", "pre_close", "amount", "upper_limit", "lower_limit",
    ))


def load_free_share_records(path):
    """Load historical free-float shares, expressed in ten-thousand shares.

    The v0.4 reference uses the Wind S_SHARE_FREESHARES unit (万股).  Keep the
    unit explicit at the file boundary so a provider returning raw shares
    cannot be silently mixed into the model inputs.
    """
    path = _path(path)
    if not os.path.isfile(path):
        raise ConfigError("free-share input is not a file: {}".format(path))
    result = {}
    with open(path, "r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise ConfigError("free-share CSV has no header: {}".format(path))
        for row_number, row in enumerate(reader, 2):
            fields = _field_map(row)
            raw_date, _ = _raw_value(fields, ("date", "trade_date", "trading_date"))
            raw_code, _ = _raw_value(fields, ("code", "instrument_id", "instrument", "wind_code"))
            value, _ = _float_value(
                fields, ("free_share", "freeshares", "s_share_freeshares"),
                "{} row {}".format(path, row_number), required=True,
            )
            unit, _ = _raw_value(fields, ("unit", "free_share_unit"))
            if unit is not None and unit.strip().lower() not in ("万股", "10k_shares", "10k"):
                raise ConfigError(
                    "free-share unit must be 万股/10k_shares in {} row {}".format(path, row_number)
                )
            if raw_date is None or raw_code is None:
                raise ConfigError("free-share row {} requires date and code".format(row_number))
            date = _parse_date(raw_date, "free-share date")
            code = _normalize_code(raw_code)
            if value <= 0.0:
                raise ConfigError("free_share must be positive for {} {}".format(code, date))
            key = (date, code)
            if key in result and result[key] != value:
                raise ConfigError("conflicting free-share rows for {} {}".format(code, date))
            result[key] = value
    if not result:
        raise ConfigError("free-share CSV contains no data rows: {}".format(path))
    return result


def load_daily_records(daily_dir, pattern, explicit_files=None):
    if explicit_files:
        paths = [_path(path) for path in explicit_files]
    else:
        paths = sorted(glob.glob(os.path.join(_path(daily_dir), pattern)))
    paths = sorted(set(paths))
    if not paths:
        raise ConfigError("no daily CSV files matched under {} ({})".format(daily_dir, pattern))

    records = {}
    row_count = 0
    for path in paths:
        if not os.path.isfile(path):
            raise ConfigError("daily input is not a file: {}".format(path))
        filename_date = _date_from_filename(path)
        with open(path, "r", encoding="utf-8-sig", newline="") as handle:
            reader = csv.DictReader(handle)
            if reader.fieldnames is None:
                raise ConfigError("daily CSV has no header: {}".format(path))
            for row_number, row in enumerate(reader, 2):
                if not any(str(value or "").strip() for value in row.values()):
                    continue
                record = _record_from_row(row, path, row_number)
                if filename_date is not None and record["date"] != filename_date:
                    raise ConfigError(
                        "row date {} does not match filename date {} in {} row {}".format(
                            record["date"], filename_date, path, row_number
                        )
                    )
                row_count += 1
                key = (record["date"], record["code"])
                previous = records.get(key)
                if previous is None:
                    records[key] = record
                elif _record_signature(previous) != _record_signature(record):
                    raise ConfigError(
                        "conflicting duplicate daily row for {} {}: {}:{} vs {}:{}".format(
                            record["code"], record["date"], previous["source"], previous["row"],
                            record["source"], record["row"],
                        )
                    )
    if not records:
        raise ConfigError("daily CSV files contain no data rows")
    return records, paths, row_count


def _sample_stddev(values):
    if len(values) < 5:
        return 0.0
    mean = math.fsum(values) / float(len(values))
    variance = math.fsum((value - mean) * (value - mean) for value in values)
    variance /= float(len(values) - 1)
    return math.sqrt(max(0.0, variance))


def _sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while True:
            block = handle.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def _unique_json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key: {}".format(key))
        result[key] = value
    return result


def _read_json(path):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return json.load(handle, object_pairs_hook=_unique_json_object)
    except (IOError, OSError, ValueError) as exc:
        raise ConfigError("failed to read JSON {}: {}".format(path, exc))


def _write_atomic(path, content):
    directory = os.path.dirname(path)
    if not os.path.isdir(directory):
        os.makedirs(directory)
    fd, temporary = tempfile.mkstemp(prefix=".{}.".format(os.path.basename(path)), dir=directory)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(content)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(temporary, OUTPUT_FILE_MODE)
        os.replace(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def _write_atomic_bytes(path, content):
    directory = os.path.dirname(path)
    if not os.path.isdir(directory):
        os.makedirs(directory)
    fd, temporary = tempfile.mkstemp(prefix=".{}.".format(os.path.basename(path)), dir=directory)
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(content)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(temporary, OUTPUT_FILE_MODE)
        os.replace(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def _replace_symlink(link_path, target_name):
    directory = os.path.dirname(link_path)
    temporary = os.path.join(
        directory,
        ".{}.{}.tmp".format(os.path.basename(link_path), os.getpid()),
    )
    try:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        os.symlink(target_name, temporary)
        os.replace(temporary, link_path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def _parse_index_list(value):
    if value is None or str(value).strip() == "":
        return None
    result = []
    for item in str(value).split(","):
        item = item.strip()
        if not item:
            continue
        try:
            result.append(int(item))
        except ValueError:
            raise ConfigError("source index must be an integer: {}".format(item))
    return result


def _validate_index_list(value, field, allow_empty=False):
    if not isinstance(value, list) or (not value and not allow_empty):
        raise ConfigError("{} must be provided and non-empty".format(field))
    for item in value:
        if isinstance(item, bool) or not isinstance(item, int):
            raise ConfigError("{} entries must be integers: {}".format(field, item))
        if item < SOURCE_INDEX_MIN or item > SOURCE_INDEX_MAX:
            raise ConfigError(
                "{} entry out of signed-short range [{}..{}]: {}".format(
                    field, SOURCE_INDEX_MIN, SOURCE_INDEX_MAX, item
                )
            )


def _template_instruments(template):
    raw = template.get("ins_params")
    if not isinstance(raw, dict) or not raw:
        raise ConfigError("template must contain a non-empty ins_params object")
    result = []
    seen_codes = {}
    for key, params in raw.items():
        if not isinstance(params, dict):
            raise ConfigError("ins_params entry is not an object: {}".format(key))
        code = _normalize_code(key)
        previous = seen_codes.get(code)
        if previous is not None:
            raise ConfigError(
                "template contains duplicate normalized instrument {} ({} and {})".format(
                    code, previous, key
                )
            )
        seen_codes[code] = str(key)
        result.append((str(key), code, dict(params)))
    return result


def _validated_sha256(value, context):
    text = str(value or "").strip()
    if not SHA256_RE.match(text):
        raise ConfigError("{} must be a 64-character SHA-256 hex digest".format(context))
    return text.lower()


def _model_path_and_hash(template, explicit_path, expected_hash):
    raw_path = explicit_path or template.get("model_path")
    if not raw_path:
        raise ConfigError("model_path is not configured")
    model_path = _path(raw_path)
    if not os.path.isfile(model_path):
        raise ConfigError("model artifact does not exist: {}".format(model_path))
    actual_hash = _sha256(model_path)
    sidecar_path = model_path + ".json"
    if not os.path.isfile(sidecar_path):
        raise ConfigError("model sidecar does not exist: {}".format(sidecar_path))
    sidecar = _read_json(sidecar_path)
    sidecar_hash = _validated_sha256(sidecar.get("binary_sha256"), "model sidecar binary_sha256")
    if actual_hash.lower() != sidecar_hash:
        raise ConfigError(
            "model SHA-256 mismatch: expected {}, actual {}".format(sidecar_hash, actual_hash)
        )
    pinned_hash = expected_hash or template.get("mix153060_model_sha256")
    if pinned_hash is None:
        raise ConfigError(
            "accepted model SHA-256 must be pinned in mix153060_model_sha256 or CLI"
        )
    pinned_hash = _validated_sha256(pinned_hash, "accepted model SHA-256")
    if actual_hash.lower() != pinned_hash:
        raise ConfigError(
            "model SHA-256 mismatch: expected {}, actual {}".format(pinned_hash, actual_hash)
        )
    return model_path, actual_hash


def _model_requires_free_share(model_path):
    sidecar = _read_json(model_path + ".json")
    contract = sidecar.get("static_input_contract")
    return isinstance(contract, dict) and "free_share" in contract


def _history_for_code(records, code, target_date, history_days, amount_days, min_amount):
    # Use exchange dates from the complete source, not code-specific dates.  A
    # suspension must reduce that instrument's observation count instead of
    # pulling an older date into the fixed five/20-day windows.
    dates = sorted({date for date, _ in records if date < target_date})
    if len(dates) < amount_days:
        raise ConfigError(
            "{} has only {} completed source dates before {}, need {}".format(
                code, len(dates), target_date, amount_days
            )
        )
    amount_dates = dates[-amount_days:]
    volatility_dates = dates[-history_days:]
    amount_values = []
    for date in amount_dates:
        record = records.get((date, code))
        if record is not None and record["amount"] is not None and record["amount"] > 0.0:
            amount_values.append(record["amount"])
    if len(amount_values) < min_amount:
        raise ConfigError(
            "{} has {} positive amount observations in prior {} dates, need {}".format(
                code, len(amount_values), amount_days, min_amount
            )
        )

    returns = []
    for date in volatility_dates:
        record = records.get((date, code))
        if record is None:
            continue
        close = record["close"]
        pre_close = record["pre_close"]
        if close is not None and close > 0.0 and pre_close > 0.0:
            returns.append(math.log(close / pre_close))
    return {
        "history_amount": math.fsum(amount_values) / float(len(amount_values)),
        "history_amount_dates": amount_dates,
        "history_amount_observations": len(amount_values),
        "history_volatility": _sample_stddev(returns),
        "history_volatility_dates": volatility_dates,
        "history_volatility_observations": len(returns),
    }


def _position_value(params, field, fallback, index, template):
    if field in params:
        return params[field]
    values = template.get(field)
    if isinstance(values, list) and index < len(values):
        return values[index]
    return fallback


def _validated_position(value, field, code):
    try:
        numeric = float(value)
    except (TypeError, ValueError):
        raise ConfigError("invalid {} for {}: {}".format(field, code, value))
    if not math.isfinite(numeric) or numeric != math.floor(numeric):
        raise ConfigError("{} must be a finite integer for {}: {}".format(field, code, value))
    result = int(numeric)
    if result < INT32_MIN or result > INT32_MAX:
        raise ConfigError("{} is outside int32 range for {}: {}".format(field, code, value))
    if field == "static_position" and result < 0:
        raise ConfigError("static_position must be non-negative for {}".format(code))
    return result


def build_config(template, records, target_date, args, source_paths, row_count,
                 free_share_records=None, free_share_path=None):
    if template.get("market") != "SZ":
        raise ConfigError("live template market must be SZ")
    mode = args.mode or template.get("sz_orderbook_mode") or template.get("orderbook_mode")
    mode = mode or DEFAULT_MODE
    if mode not in VALID_MODES:
        raise ConfigError("mode must be hp-shadow or hp-realtime: {}".format(mode))

    instruments = _template_instruments(template)
    model_path, model_hash = _model_path_and_hash(
        template, args.model_path, args.expected_model_sha256
    )
    if _model_requires_free_share(model_path) and free_share_records is None:
        raise ConfigError(
            "model static_input_contract requires free_share; provide --free-share-file"
        )

    md_indices = _parse_index_list(args.md_source_index)
    td_indices = _parse_index_list(args.td_source_index)
    if md_indices is None:
        md_indices = template.get("md_source_index")
    if td_indices is None:
        td_indices = template.get("td_source_index")
    _validate_index_list(md_indices, "md_source_index")
    _validate_index_list(td_indices, "td_source_index", allow_empty=mode == "hp-shadow")
    if mode == "hp-realtime" and not td_indices:
        raise ConfigError("hp-realtime requires a non-empty td_source_index")

    config = json.loads(json.dumps(template))
    # These fields are meaningful only to the offline BacktestEngine and must
    # never leak into a live configuration.
    for key in (
        "order_data_source", "trade_data_source", "replay_window",
        "mix153060_model_artifact", "hp_model_artifact", "model_type", "scaler_path",
    ):
        config.pop(key, None)
    config["market"] = "SZ"
    config["sz_orderbook_mode"] = mode
    config["orderbook_mode"] = mode
    config["model_path"] = model_path
    config["mix153060_model_sha256"] = model_hash
    config["md_source_index"] = list(md_indices)
    config["td_source_index"] = list(td_indices)
    capture = config.get("sze_prediction_capture")
    if not isinstance(capture, dict):
        capture = config.get("mix153060_capture")
    if isinstance(capture, dict) and capture.get("enabled"):
        prefix = capture.get("prefix")
        if isinstance(prefix, str):
            capture["prefix"] = prefix.replace("{target_date}", target_date)

    audit_rows = []
    top_codes = []
    top_amounts = []
    top_static_positions = []
    top_last_positions = []
    for index, (template_key, code, params) in enumerate(instruments):
        target = records.get((target_date, code))
        if target is None:
            raise ConfigError("missing target row for {} {}".format(code, target_date))
        if target["pre_close"] <= 0.0:
            raise ConfigError("target pre_close must be positive for {}".format(code))
        if target["upper_limit"] is None or target["lower_limit"] is None:
            raise ConfigError("target price limits are required for {}".format(code))
        if target["upper_limit"] <= 0.0 or target["lower_limit"] <= 0.0:
            raise ConfigError("target price limits must be positive for {}".format(code))
        if target["lower_limit"] > target["upper_limit"]:
            raise ConfigError("lower limit exceeds upper limit for {}".format(code))

        history = _history_for_code(
            records,
            code,
            target_date,
            args.history_volatility_days,
            args.history_amount_days,
            args.min_amount_observations,
        )
        if not math.isfinite(history["history_amount"]) or history["history_amount"] <= 0.0:
            raise ConfigError("invalid HistoryAmount for {}".format(code))
        if not math.isfinite(history["history_volatility"]) or history["history_volatility"] < 0.0:
            raise ConfigError("invalid HistoryVolatility20d for {}".format(code))

        free_share = None
        if free_share_records is not None:
            free_share = free_share_records.get((target_date, code))
            if free_share is None:
                raise ConfigError(
                    "missing FreeShare for {} {} in {}".format(code, target_date, free_share_path)
                )

        static_position = _position_value(params, "static_position", 0, index, template)
        last_position = _position_value(params, "last_position", 0, index, template)
        static_position = _validated_position(static_position, "static_position", code)
        last_position = _validated_position(last_position, "last_position", code)
        if static_position + last_position < 0:
            raise ConfigError(
                "static_position + last_position must be non-negative for {}".format(code)
            )

        updated = dict(params)
        updated.update({
            "Date": int(target_date),
            # StrategyBase passes Close to the native runtime as pre_close.
            "Close": target["pre_close"],
            "HistoryAmount": history["history_amount"],
            "HpUpperPrice": target["upper_limit"],
            "HpLowerPrice": target["lower_limit"],
            "HistoryVolatility20d": history["history_volatility"],
            "static_position": static_position,
            "last_position": last_position,
        })
        if free_share is not None:
            updated["FreeShare"] = free_share
        config["ins_params"][template_key] = updated
        top_codes.append(code[:-3])
        top_amounts.append(history["history_amount"])
        top_static_positions.append(static_position)
        top_last_positions.append(last_position)
        audit_rows.append({
            "instrument": code,
            "target_source": target["source"],
            "history_amount": history["history_amount"],
            "history_amount_dates": history["history_amount_dates"],
            "history_amount_observations": history["history_amount_observations"],
            "history_volatility_20d": history["history_volatility"],
            "history_volatility_dates": history["history_volatility_dates"],
            "history_volatility_observations": history["history_volatility_observations"],
            "pre_close": target["pre_close"],
            "upper_limit": target["upper_limit"],
            "lower_limit": target["lower_limit"],
            "static_position": static_position,
            "last_position": last_position,
        })
        if free_share is not None:
            audit_rows[-1]["free_share"] = free_share

    if mode == "hp-realtime" and not any(value > 0 for value in top_static_positions):
        raise ConfigError(
            "hp-realtime requires at least one positive static_position; "
            "the checked-in zero-position template is shadow-only"
        )
    config["instrument_id"] = top_codes
    config["his_amt"] = top_amounts
    config["static_position"] = top_static_positions
    config["last_position"] = top_last_positions
    return config, {
        "schema_version": 1,
        "target_date": target_date,
        "mode": mode,
        "model_path": model_path,
        "model_sha256": model_hash,
        "template_sha256": args.template_sha256,
        "daily_input_files": [
            {"path": _path(path), "sha256": _sha256(path)} for path in source_paths
        ],
        "daily_input_row_count": row_count,
        "free_share_input": (
            {"path": _path(free_share_path), "sha256": _sha256(free_share_path),
             "unit": "万股", "date_semantics": "value effective for target trading date"}
            if free_share_path else None
        ),
        "history_amount_days": args.history_amount_days,
        "history_volatility_days": args.history_volatility_days,
        "min_amount_observations": args.min_amount_observations,
        "history_amount_formula": (
            "mean(positive amount_cny) over prior {} completed trading dates".format(
                args.history_amount_days
            )
        ),
        "history_volatility_formula": (
            "stddevSamp(log(close / pre_close)) over prior {} completed trading dates; "
            "zero when valid observations < 5"
        ).format(args.history_volatility_days),
        "source_indices": {"md": list(md_indices), "td": list(td_indices)},
        "instruments": audit_rows,
    }


def _json_text(value):
    try:
        return json.dumps(
            value,
            ensure_ascii=True,
            indent=2,
            sort_keys=False,
            allow_nan=False,
        ) + "\n"
    except (TypeError, ValueError) as exc:
        raise ConfigError("configuration is not strict finite JSON: {}".format(exc))


def _main_conf_sources(main_conf, key, allow_empty=False):
    entries = main_conf.get(key)
    if entries is None and allow_empty:
        return []
    if not isinstance(entries, list) or (not entries and not allow_empty):
        raise ConfigError("main conf must contain a non-empty {} array".format(key))
    result = []
    for entry in entries:
        if not isinstance(entry, dict):
            raise ConfigError("main conf {} entry is not an object".format(key))
        source = entry.get("source")
        if isinstance(source, bool) or not isinstance(source, int):
            raise ConfigError("main conf {} source must be an integer: {}".format(key, source))
        result.append(source)
    _validate_index_list(result, "main conf {} source".format(key), allow_empty=allow_empty)
    return result


def _render_main_conf(template_path, config_ref, strategy_library, md_indices, td_indices):
    if not isinstance(config_ref, str) or not config_ref.strip():
        raise ConfigError("main conf strategy config reference must be non-empty")
    main_conf = _read_json(template_path)
    vstr = main_conf.get("vstr")
    if not isinstance(vstr, list) or len(vstr) != 1 or not isinstance(vstr[0], dict):
        raise ConfigError("main conf must contain exactly one vstr object")
    conf_md_sources = _main_conf_sources(main_conf, "vmd")
    conf_td_sources = _main_conf_sources(main_conf, "vtd", allow_empty=not td_indices)
    missing_md = sorted(set(md_indices) - set(conf_md_sources))
    missing_td = sorted(set(td_indices) - set(conf_td_sources))
    if missing_md:
        raise ConfigError("main conf vmd is missing configured sources: {}".format(missing_md))
    if missing_td:
        raise ConfigError("main conf vtd is missing configured sources: {}".format(missing_td))

    entry = vstr[0]
    entry["config"] = config_ref
    if strategy_library:
        entry["lib"] = strategy_library
    if not isinstance(entry.get("lib"), str) or not entry["lib"].strip():
        raise ConfigError("main conf vstr library must be configured")
    if os.path.basename(os.path.normpath(entry["lib"])) != SZE_STRATEGY_LIBRARY_FILENAME:
        raise ConfigError(
            "Shenzhen main conf strategy library must be {}: {}".format(
                SZE_STRATEGY_LIBRARY_FILENAME, entry["lib"]
            )
        )
    return _json_text(main_conf)


def _snapshot_path(path):
    if os.path.islink(path):
        return ("symlink", os.readlink(path))
    if os.path.exists(path):
        with open(path, "rb") as handle:
            return ("file", handle.read())
    return ("missing", None)


def _restore_path(path, snapshot):
    kind, value = snapshot
    if os.path.lexists(path):
        os.unlink(path)
    if kind == "file":
        _write_atomic_bytes(path, value)
    elif kind == "symlink":
        _replace_symlink(path, value)


def publish(config, audit, output_dir, target_date, main_conf_template=None,
            strategy_library=None, runtime_config_ref=None, update_current=True):
    output_dir = _path(output_dir)
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    prefix = "sze_mix153060_live_{}".format(target_date)
    json_name = "config_{}.json".format(prefix)
    manifest_name = "manifest_{}.json".format(prefix)
    conf_name = "main_{}.conf".format(prefix)
    json_path = os.path.join(output_dir, json_name)
    manifest_path = os.path.join(output_dir, manifest_name)
    conf_path = os.path.join(output_dir, conf_name)
    current_json = os.path.join(output_dir, "config_sze_mix153060_live_current.json")
    current_conf = os.path.join(output_dir, "main_sze_mix153060_live_current.conf")
    current_manifest = os.path.join(output_dir, "manifest_sze_mix153060_live_current.json")
    tracked_paths = [json_path, manifest_path]
    if main_conf_template:
        tracked_paths.append(conf_path)
    if update_current:
        tracked_paths.extend([current_json, current_manifest])
        if main_conf_template:
            tracked_paths.append(current_conf)
    snapshots = {path: _snapshot_path(path) for path in tracked_paths}
    try:
        config_text = _json_text(config)
        audit["config_sha256"] = hashlib.sha256(config_text.encode("utf-8")).hexdigest()
        audit["config_path"] = json_path
        conf_text = None
        if main_conf_template:
            if runtime_config_ref is None:
                runtime_config_ref = json_path
            conf_text = _render_main_conf(
                _path(main_conf_template),
                runtime_config_ref,
                strategy_library,
                config["md_source_index"],
                config["td_source_index"],
            )
            audit["main_conf_template_sha256"] = _sha256(_path(main_conf_template))
            audit["main_conf_runtime_config_ref"] = runtime_config_ref
            audit["main_conf_strategy_library_override"] = strategy_library
            audit["main_conf_path"] = conf_path
        _write_atomic(json_path, config_text)
        if conf_text is not None:
            _write_atomic(conf_path, conf_text)
        _write_atomic(manifest_path, _json_text(audit))
        if update_current:
            _replace_symlink(current_json, json_name)
            if main_conf_template:
                _replace_symlink(current_conf, conf_name)
            _replace_symlink(current_manifest, manifest_name)
    except Exception:
        for path in reversed(tracked_paths):
            _restore_path(path, snapshots[path])
        raise
    return {
        "json": json_path,
        "manifest": manifest_path,
        "conf": conf_path if main_conf_template else None,
    }


def parse_args(argv=None):
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target-date", required=True, help="Target trading date YYYYMMDD")
    parser.add_argument(
        "--template-json",
        default=os.path.join(repo_root, "config", "examples", "sze", "config_sze_mix153060_live.template.json"),
        help="Live strategy JSON template",
    )
    parser.add_argument("--main-conf-template", default=None, help="Optional Deepwin main conf template")
    parser.add_argument("--daily-dir", default="/home/data/sze_daily", help="Normalized daily CSV directory")
    parser.add_argument(
        "--daily-pattern",
        default="sze_daily_????????.csv",
        help="Daily CSV glob within --daily-dir",
    )
    parser.add_argument("--daily-file", action="append", default=None, help="Explicit daily CSV; may repeat")
    parser.add_argument(
        "--free-share-file", default=None,
        help="CSV of date,code,free_share[,unit]; free_share is in 万股",
    )
    parser.add_argument("--output-dir", default=os.path.join(repo_root, "build", "configs", "sze_mix153060_live"))
    parser.add_argument("--mode", choices=VALID_MODES, default=None)
    parser.add_argument("--model-path", default=None)
    parser.add_argument("--expected-model-sha256", default=None)
    parser.add_argument("--md-source-index", default=None, help="Comma-separated MD source indices")
    parser.add_argument("--td-source-index", default=None, help="Comma-separated TD source indices")
    parser.add_argument(
        "--strategy-library",
        default=DEFAULT_SZE_STRATEGY_LIBRARY,
        help="Shenzhen strategy library path to put in generated main conf",
    )
    parser.add_argument("--runtime-config-ref", default=None, help="Config path written into main conf")
    parser.add_argument("--history-amount-days", type=int, default=DEFAULT_HISTORY_AMOUNT_DAYS)
    parser.add_argument("--history-volatility-days", type=int, default=DEFAULT_HISTORY_VOLATILITY_DAYS)
    parser.add_argument("--min-amount-observations", type=int, default=DEFAULT_MIN_AMOUNT_OBSERVATIONS)
    parser.add_argument("--no-current", action="store_true", help="Do not update current symlinks")
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args(argv)


def run(args):
    target_date = _parse_date(args.target_date, "target date")
    if args.history_amount_days <= 0 or args.history_volatility_days <= 0:
        raise ConfigError("history windows must be positive")
    if args.min_amount_observations <= 0 or args.min_amount_observations > args.history_amount_days:
        raise ConfigError("min amount observations must be within the amount window")
    template_path = _path(args.template_json)
    template = _read_json(template_path)
    args.template_sha256 = _sha256(template_path)
    records, source_paths, row_count = load_daily_records(
        args.daily_dir, args.daily_pattern, args.daily_file
    )
    free_share_records = None
    if args.free_share_file:
        free_share_records = load_free_share_records(args.free_share_file)
    config, audit = build_config(
        template, records, target_date, args, source_paths, row_count,
        free_share_records=free_share_records,
        free_share_path=args.free_share_file,
    )
    if args.main_conf_template:
        prospective_json = os.path.join(
            _path(args.output_dir),
            "config_sze_mix153060_live_{}.json".format(target_date),
        )
        prospective_ref = args.runtime_config_ref or prospective_json
        _render_main_conf(
            _path(args.main_conf_template),
            prospective_ref,
            args.strategy_library,
            config["md_source_index"],
            config["td_source_index"],
        )
    _json_text(config)
    if args.dry_run:
        print(_json_text(audit), end="")
        return {"config": config, "audit": audit, "published": None}
    published = publish(
        config,
        audit,
        args.output_dir,
        target_date,
        main_conf_template=args.main_conf_template,
        strategy_library=args.strategy_library,
        runtime_config_ref=args.runtime_config_ref,
        update_current=not args.no_current,
    )
    print("target_date={}".format(target_date))
    print("instruments={}".format(len(audit["instruments"])))
    print("model_sha256={}".format(audit["model_sha256"]))
    print("config={}".format(published["json"]))
    print("manifest={}".format(published["manifest"]))
    if published["conf"]:
        print("main_conf={}".format(published["conf"]))
    return {"config": config, "audit": audit, "published": published}


def main(argv=None):
    try:
        run(parse_args(argv))
    except ConfigError as exc:
        print("sze-live-config: {}".format(exc), file=sys.stderr)
        return 2
    except (IOError, OSError) as exc:
        print("sze-live-config: I/O error: {}".format(exc), file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
