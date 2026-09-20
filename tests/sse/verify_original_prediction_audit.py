#!/usr/bin/env python
"""Compare a fixed replay prefix against the original production v0.6 audit."""
from __future__ import print_function
import collections,json,struct,sys
from verify_parallel_parity import RECORD_BYTES,description

def predictions(path):
    with open(path,'rb') as f:
        if f.read(8)!=b'SSEPAR01':raise ValueError('invalid parity header')
        while True:
            chunk=f.read(RECORD_BYTES*4096)
            if not chunk:break
            if len(chunk)%RECORD_BYTES:raise ValueError('partial output record')
            for offset in range(0,len(chunk),RECORD_BYTES):
                record=chunk[offset:offset+RECORD_BYTES]
                if struct.unpack_from('<I',record,8)[0]!=1:continue
                flags=struct.unpack_from('<I',record,36)[0]
                if flags&3!=3:continue
                yield description(record)

def audit(path):
    with open(path) as f:
        for line in f:
            r=json.loads(line)
            if r.get('event')=='v06_prediction':yield r

def compare(prefix,old):
    current=predictions(prefix+'.outputs.bin');previous=audit(old);counts=collections.Counter();count=0
    while True:
        try:a=next(current)
        except StopIteration:a=None
        try:b=next(previous)
        except StopIteration:b=None
        if a is None or b is None:
            if a is None and b is None:return {'ok':True,'equal_predictions':count,'stocks':len(counts),'600000':counts['600000']}
            return {'ok':False,'equal_predictions':count,'error':'prediction counts differ','replay':a,'original':b}
        match=(a['instrument']==b['instrument'] and a['time_us']==b['time_us'] and
               a['wire_sequence']==b['wire_sequence'] and a['provenance'][1]==b['stream_sequence'] and
               bool(a['prediction_flags']&4)==b['selected'] and struct.pack('<4f',*a['heads'])==struct.pack('<4f',*b['heads']))
        if not match:return {'ok':False,'equal_predictions':count,'replay':a,'original':b}
        count+=1;counts[a['instrument']]+=1
if __name__=='__main__':
    if len(sys.argv)!=3:raise SystemExit('usage: verify_original_prediction_audit.py REPLAY_PREFIX ORIGINAL_AUDIT')
    result=compare(*sys.argv[1:]);print(json.dumps(result,indent=2,sort_keys=True));sys.exit(0 if result['ok'] else 1)
