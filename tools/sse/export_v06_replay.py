#!/usr/bin/env python3
"""Export supplied Arrow raw events and golden checkpoints for C++ factor validation."""
import json,pathlib,sys
import pyarrow.ipc as ipc
def main(root,out):
    root=pathlib.Path(root);out=pathlib.Path(out);out.mkdir(exist_ok=True,parents=True)
    raw=root/'raw/20260401/stocks/600000.SH';gold=root/'golden/600000.SH_20260401'
    orders=ipc.open_file(raw/'order.arrow').read_all().to_pylist()
    trades=ipc.open_file(raw/'trade.arrow').read_all().to_pylist()
    checkpoints=ipc.open_file(gold/'features.arrow').read_all().to_pylist()
    print('order_types',sorted(set(r['type'] for r in orders)),'directions',sorted(set(r['direction'] for r in orders)))
    events=[]
    for r in orders:
        kind={0:'A',1:'D',2:'S'}[r['type']]
        side='B' if r['direction']==1 else 'S'
        events.append((r['app_seq'],kind,r['ex_time']%86400000000,round(r['price']*1000),r['volume']*1000,
                       r['order_id'] if side=='B' else 0,r['order_id'] if side=='S' else 0,side))
    for r in trades:
        assert r['type']==0,r
        events.append((r['app_seq'],'T',r['ex_time']%86400000000,round(r['price']*1000),r['volume']*1000,r['buy_no'],r['sell_no'],'B'))
    with open(out/'raw-events.txt','w') as f:
        for row in sorted(events):f.write(' '.join(map(str,row))+'\n')
    first=checkpoints[0]
    with open(out/'checkpoints.txt','w') as f:
        f.write('%d %d -1\n'%(first['window_start_app_seq'],first['window_start_ex_time_micros']%86400000000))
        previous=None
        for i,r in enumerate(checkpoints):
            if previous is not None and r['window_start_app_seq']!=previous:
                f.write('%d %d -1\n'%(r['window_start_app_seq'],r['window_start_ex_time_micros']%86400000000))
            f.write('%d %d %d\n'%(r['app_seq'],r['ex_time_micros']%86400000000,i))
            previous=r['app_seq']
    with open(out/'golden-factors.f32','wb') as f:
        names=(root/'factors/factors.txt').read_text().splitlines()
        import struct
        for r in checkpoints:f.write(struct.pack('<50f',*[r[n] for n in names]))
    (out/'static.json').write_bytes((gold/'case_parameters.json').read_bytes())
if __name__=='__main__':main(*sys.argv[1:])
