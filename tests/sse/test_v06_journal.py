#!/usr/bin/env python
"""Real UDP -> journal -> v06 predictions, including 09:30 warm-up rows."""
from __future__ import print_function
import json,os,signal,sys,tempfile,time
import sse_journal_integration_test as h
def main(capture_binary,predict_binary,artifact):
    root=tempfile.mkdtemp(prefix='sse-v06-journal-');processes=[];handles=[]
    try:
        day=int(time.strftime('%Y%m%d'));c=h.make_config(root,(h.reserve_port(),h.reserve_port()),day)
        cp=root+'/capture.json';pp=root+'/profile.json';h.write_json(cp,c);h.make_profile(pp,day)
        profile=json.load(open(pp));profile.update(processing_mode='prediction')
        profile['prediction']={'model_version':'v0.6','model_path':artifact,'auction59':{'enabled':False}}
        h.write_json(pp,profile)
        capture=h.launch(capture_binary,[cp],root,'capture');processes.append(capture[0]);handles.extend(capture[1:3])
        deadline=time.time()+10
        while 'Shanghai journal capture ready' not in h.read_text(capture[4]):
            assert time.time()<deadline;h.require_alive(capture[0],.02,capture[4],'capture')
        state={'round':0,'wire_sequence':1,'tick_index':1}
        h.send_round(c['channels'][0]['port'],state,True,c['channels'][1]['port'])
        h.wait_for_datagrams(c['journal_directory'],4,5)
        audit_path=root+'/v06-audit.jsonl'
        previous_audit=os.environ.get('SSE_V06_AUDIT_PATH')
        os.environ['SSE_V06_AUDIT_PATH']=audit_path
        try: pred=h.launch(predict_binary,[cp,pp],root,'predict')
        finally:
            if previous_audit is None:os.environ.pop('SSE_V06_AUDIT_PATH',None)
            else:os.environ['SSE_V06_AUDIT_PATH']=previous_audit
        processes.append(pred[0]);handles.extend(pred[1:3])
        h.require_alive(pred[0],1,pred[4],'predict')
        h.send_round(c['channels'][0]['port'],state,False)
        h.wait_for_datagrams(c['journal_directory'],7,5)
        h.require_alive(pred[0],.5,pred[4],'predict')
        h.stop_process(pred[0],signal.SIGTERM,10,'predict')
        result=h.last_json(pred[3]);assert result['ok'],result
        rows=[json.loads(l) for l in h.read_text(audit_path).splitlines() if l.startswith('{') and 'v06_prediction' in l]
        assert rows and all(len(r['heads'])==4 and r['units']=='permille' for r in rows),h.read_text(pred[4])
        assert any(34200000000<=r['time_us']<34380000000 for r in rows),rows
        assert result['processing']['model_version']=='v0.6',result
        print(json.dumps({'ok':True,'warmup_predictions':len(rows),'evidence':root}))
    finally:
        for p in processes:
            if p.poll() is None:p.terminate();p.wait()
        for f in handles:f.close()
if __name__=='__main__':main(*sys.argv[1:])
