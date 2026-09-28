#!/usr/bin/env python3
"""Build an isolated V06 trading launch from the already validated daily plan."""
import argparse
import hashlib
import json
import os
from pathlib import Path

MODEL_SHA256 = '38b0ee6598c80750d3f401bfd9d3368e0cd34e522e183134188e64ebf0684cb1'


def load(path):
    with open(str(path)) as f:
        return json.load(f)


def write(path, value):
    temporary = str(path) + '.tmp'
    with open(temporary, 'w') as f:
        json.dump(value, f, indent=2, sort_keys=True, allow_nan=False)
        f.write('\n')
    os.replace(temporary, str(path))


def prepare(day, original, release, output):
    original, release, output = Path(original), Path(release), Path(output)
    config = load(original / 'config.json')
    main = load(original / 'main.conf')
    model = release / 'model.bin'
    release_manifest = load(release / 'manifest.json')
    for name in ('model.bin', 'libt0_strategy_sze.so', 'libsze_td.so'):
        if hashlib.sha256((release / name).read_bytes()).hexdigest() != release_manifest['sha256'][name]:
            raise ValueError('release checksum mismatch: ' + name)
    if hashlib.sha256(model.read_bytes()).hexdigest() != MODEL_SHA256:
        raise ValueError('V06 model checksum mismatch')
    if config.get('trading_day') != day:
        raise ValueError('daily trading date mismatch')
    if not config.get('ins_params'):
        raise ValueError('empty trade universe')
    if len(main.get('vtd', [])) != 1 or len(main.get('vstr', [])) != 1:
        raise ValueError('V06 launch requires one strategy and one TD')
    for symbol, fields in config['ins_params'].items():
        if fields.get('Date') != day or fields.get('static_position', 0) < 0:
            raise ValueError('invalid dated static position: ' + symbol)
        for key in ('Close', 'FreeShare', 'HistoryAmount', 'HpLowerPrice', 'HpUpperPrice'):
            if not isinstance(fields.get(key), (int, float)) or fields[key] <= 0:
                raise ValueError('invalid static field ' + symbol + ':' + key)
    config['strategy_version'] = 'v06-b15-mh4'
    quarantine = Path('/home/zane/run_main/risk/sze') / str(day)
    quarantine.mkdir(parents=True, exist_ok=True)
    config['sze_daily_book_guard_directory'] = str(quarantine)
    config['strategy_name'] = 'sze_v06_b15_mh4_{}'.format(day)
    config['model_path'] = str(model)
    config['mix153060_model_sha256'] = MODEL_SHA256
    config['sze_startup_warmup_signals'] = 0
    config['v06_strategy'] = {
        'offset_permille': 1.0, 'quote_ratio': 10.0, 'skewness_bps': 1.0,
        'bias_factor': 0.3, 'position_limit_factor': 1.0,
        'position_base_line': 500000.0, 'max_exposure': 0.05,
        'max_global_skewness_bps': 10.0,
    }
    config['global_params'].update(offset=1.0, quote_offset=10.0, bias_factor=0.3,
                                   position_limit=1.0, global_bias_factor=1.0,
                                   position_base_line=500000.0)
    # Keep the launch's existing universe, baseline positions and quantity cap.
    # Research capital targets are not authority to buy a larger live inventory.
    cap = config['sze_order_routing'].get('max_position')
    if cap != 200:
        raise ValueError('expected existing 200-share per-order launch cap')
    capture = config.get('sze_prediction_capture', {})
    capture['directory'] = '/home/zane/run_main/log/sze_v06_trade_{}'.format(day)
    capture['prefix'] = 'sze_v06_{}'.format(day)
    config['sze_prediction_capture'] = capture
    output.mkdir(parents=True, exist_ok=True)
    main['vstr'][0]['lib'] = str(release / 'libt0_strategy_sze.so')
    main['vstr'][0]['config'] = str(output / 'config.json')
    main['vtd'][0]['lib'] = str(release / 'libsze_td.so')
    for item in main['vstr'] + main['vtd']:
        if not Path(item['lib']).is_file():
            raise ValueError('missing release library: ' + item['lib'])
    write(output / 'config.json', config)
    write(output / 'main.conf', main)
    summary = {'trading_day': day, 'strategy_version': config['strategy_version'],
               'model_sha256': MODEL_SHA256, 'instruments': len(config['ins_params']),
               'per_order_share_cap': cap, 'baseline_positions_preserved': True,
               'original_runtime': str(original), 'release': str(release),
               'offset_permille': 1.0}
    write(output / 'manifest.json', summary)
    return summary


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--day', required=True, type=int)
    p.add_argument('--original', required=True)
    p.add_argument('--release', required=True)
    p.add_argument('--output', required=True)
    args = p.parse_args()
    print(json.dumps(prepare(args.day, args.original, args.release, args.output), sort_keys=True))
