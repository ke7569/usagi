#!/usr/bin/env python
"""Compare ordered replay evidence without loading all outputs into memory."""
from __future__ import print_function
import json,os,struct,sys

RECORD_BYTES=232
def description(record):
    first=struct.unpack('<QI8sQQIIdqQI4f',record[:88])
    return {'ordinal':first[0],'kind':first[1],'instrument':first[2].rstrip(b'\0').decode('ascii'),
        'time_us':first[3],'wire_sequence':first[4],'prediction_flags':first[5],
        'sample_reasons':first[6],'window_turnover':first[7],'window_volume':first[8],
        'window_exchange_time_us':first[9],'factor_crc32':first[10],'heads':list(first[11:15]),
        'provenance':list(struct.unpack('<17Q',record[88:224])),
        'full_output_hash':struct.unpack('<Q',record[224:])[0]}

def compare(left,right):
    result={'ok':True,'serial_prefix':left,'parallel_prefix':right}
    with open(left+'.summary.json') as f: a=json.load(f)
    with open(right+'.summary.json') as f: b=json.load(f)
    keys=('ok','last_event_id','events','configured_stocks','datagrams','idle_events','heartbeats',
          'raw_tick_records','raw_snapshot_records','decoded_record_bytes','outputs','tick_samples',
          'snapshot_outputs','batch_outputs','model_predictions','ordered_evidence_hash','stock_600000')
    diffs=dict((k,{'serial':a.get(k),'parallel':b.get(k)}) for k in keys if a.get(k)!=b.get(k))
    if diffs:result['ok']=False;result['summary_differences']=diffs
    with open(left+'.outputs.bin','rb') as fa,open(right+'.outputs.bin','rb') as fb:
        if fa.read(8)!=b'SSEPAR01' or fb.read(8)!=b'SSEPAR01':raise ValueError('bad evidence header')
        count=0
        while True:
            ba=fa.read(RECORD_BYTES*4096);bb=fb.read(RECORD_BYTES*4096)
            if ba!=bb:
                result['ok']=False
                for start in range(0,max(len(ba),len(bb)),RECORD_BYTES):
                    ra=ba[start:start+RECORD_BYTES];rb=bb[start:start+RECORD_BYTES]
                    if ra!=rb:
                        result['first_output_difference']={'ordinal':count+start//RECORD_BYTES+1,
                            'serial':description(ra) if len(ra)==RECORD_BYTES else {'bytes':len(ra)},
                            'parallel':description(rb) if len(rb)==RECORD_BYTES else {'bytes':len(rb)}}
                        break
                break
            if not ba:break
            if len(ba)%RECORD_BYTES:raise ValueError('truncated evidence record')
            count+=len(ba)//RECORD_BYTES
        result['equal_records_before_difference']=count
    with open(left+'.stocks.csv','rb') as fa,open(right+'.stocks.csv','rb') as fb:
        result['per_stock_equal']=fa.read()==fb.read()
    result['ok']=result['ok'] and result['per_stock_equal']
    result['serial_wall_events_per_second']=a['wall_events_per_second']
    result['parallel_wall_events_per_second']=b['wall_events_per_second']
    result['wall_speedup']=float(a['wall_ns'])/b['wall_ns']
    result['serial_processing_call_ns']=a['processing_call_ns']
    result['parallel_processing_call_ns']=b['processing_call_ns']
    result['stock_600000']=a['stock_600000']
    return result
if __name__=='__main__':
    if len(sys.argv)!=3:raise SystemExit('usage: verify_parallel_parity.py SERIAL_PREFIX PARALLEL_PREFIX')
    result=compare(*sys.argv[1:]);print(json.dumps(result,indent=2,sort_keys=True));sys.exit(0 if result['ok'] else 1)
