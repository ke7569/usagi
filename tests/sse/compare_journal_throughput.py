#!/usr/bin/env python
"""Summarize matching-prefix serial/parallel throughput, without rerunning it."""
from __future__ import print_function
import json,sys

def read(path):
    with open(path) as f:return json.load(f)

def main(serial_prefix,parallel_prefix):
    a=read(serial_prefix+'.summary.json');b=read(parallel_prefix+'.summary.json')
    keys=('last_event_id','events','universe','datagrams','idle_events','payload_bytes',
          'callbacks','tick_samples','model_predictions','selected','snapshot_outputs','batch_outputs',
          'ordered_output_checksum')
    differences=dict((k,{'serial':a.get(k),'parallel':b.get(k)}) for k in keys if a.get(k)!=b.get(k))
    out={'ok':not differences,'differences':differences,'serial_prefix':serial_prefix,'parallel_prefix':parallel_prefix,
         'events':a['events'],'predictions':a['model_predictions'],'callbacks':a['callbacks'],
         'serial_wall_s':a['overall_wall_ns']/1e9,'parallel_wall_s':b['overall_wall_ns']/1e9,
         'serial_processor_s':a['processor_call_ns']/1e9,'parallel_processor_s':b['processor_call_ns']/1e9,
         'wall_speedup':float(a['overall_wall_ns'])/b['overall_wall_ns'],
         'serial_events_s':a['events_per_second'],'parallel_events_s':b['events_per_second'],
         'serial_amortized_us_per_event':a['amortized_processor_us_per_event'],
         'parallel_amortized_us_per_event':b['amortized_processor_us_per_event'],
         'serial_amortized_us_per_prediction':a['amortized_processor_us_per_prediction'],
         'parallel_amortized_us_per_prediction':b['amortized_processor_us_per_prediction'],
         'parallel_enqueue_calls':b['enqueue_calls'],'parallel_enqueue_s':b['enqueue_ns']/1e9,
         'parallel_flush_calls':b['flush_calls'],'parallel_flush_s':b['flush_call_ns']/1e9,
         'parallel_final_flush_s':b['final_flush_ns']/1e9,
         'scope_note':'Amortized processor time includes decoding, book, sampling, factors, GRU and callback. Parallel flush time includes worker execution, barrier and ordered merge; it is not pure barrier overhead.',
         'phases':[]}
    for sa,pb in zip(a['phase_summaries'],b['phase_summaries']):
        out['phases'].append({'phase':sa['receive_phase'],'events':sa['events'],
                             'predictions':sa['model_predictions'],'serial_events_s':sa['events_per_second'],
                             'parallel_events_s':pb['events_per_second'],
                             'serial_processor_s':sa['processor_call_ns']/1e9,
                             'parallel_processor_s':pb['processor_call_ns']/1e9})
    try:
        micro=read(serial_prefix+'.model-micro.json');out['model_microbenchmark']=micro
        out['estimated_hot_model_s_for_prefix_predictions']=micro['mean_us']*a['model_predictions']/1e6
        out['model_estimate_note']='Hot-model estimate is an illustrative product of average microbenchmark call time and measured model count, not a direct stage profile of the real replay.'
    except IOError:pass
    return out

if __name__=='__main__':
    if len(sys.argv)!=3:raise SystemExit('usage: compare_journal_throughput.py SERIAL_PREFIX PARALLEL_PREFIX')
    result=main(*sys.argv[1:]);print(json.dumps(result,indent=2,sort_keys=True));sys.exit(0 if result['ok'] else 1)
