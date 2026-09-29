import csv,json,os,sys,bisect
b=os.path.abspath(sys.argv[1])
def rows(p):return list(csv.DictReader(open(p)))
def stats(v):
 v=sorted(v);n=len(v)
 return dict(n=n,mean=sum(v)/n,p50=v[(n-1)//2],p95=v[(n-1)*95//100],p99=v[(n-1)*99//100],max=v[-1]) if n else {}
def key(r):return tuple(int(r[k]) for k in ['code','channel','batch','wire','receive_ns'])
label=sys.argv[2];d=b+'/'+label;s=json.load(open(d+'/summary.json'));anchor=s['source_anchor'];wall=s['wall_anchor']
cs={key(r):r for r in rows(d+'/compute.csv')};ds={key(r):r for r in rows(d+'/delivery.csv')};ins=rows(d+'/ingress.csv');pub={int(r['event']):r for r in rows(d+'/publisher.csv')};starts=[int(r['app_entry']) for r in ins]
stages={};close_parts={};input_parts={}
def add(group,k,v):group.setdefault(k,[]).append(v/1000.)
for r in rows(d+'/strategy.csv'):
 k=key(r);c=cs[k];v=ds[k];source=int(r['receive_ns'])-anchor+wall
 for n,a,z in [('total',source,int(r['strategy_entry'])),('to_close',source,int(v['close'])),('close_to_worker',int(v['close']),int(v['worker_start'])),('worker_to_factor',int(v['worker_start']),int(c['factor_start'])),('factor',int(c['factor_start']),int(c['factor_end'])),('model',int(c['model_start']),int(c['model_end'])),('model_to_strategy',int(c['model_end']),int(r['strategy_entry']))]:add(stages,n,z-a)
 p=bisect.bisect_right(starts,int(v['close']))-1;t=ins[p];pu=pub[int(t['event'])];ts=int(t['receive_ns'])-anchor+wall
 for n,x in [('natural_close',int(t['receive_ns'])-int(r['receive_ns'])),('trigger_publish_late',int(pu['begin'])-ts),('trigger_publish',int(pu['end'])-int(pu['begin'])),('trigger_consumer_late',int(t['read_start'])-int(pu['end'])),('trigger_read',int(t['read_end'])-int(t['read_start'])),('trigger_decode',int(t['app_entry'])-int(t['read_end'])),('trigger_app_close',int(v['close'])-int(t['app_entry']))]:add(close_parts,n,x)
for r in ins:
 if r['kind']!='1':continue
 pu=pub[int(r['event'])];src=int(r['receive_ns'])-anchor+wall
 for n,x in [('read',int(r['read_end'])-int(r['read_start'])),('app',int(r['app_done'])-int(r['app_entry'])),('publish_late',int(pu['begin'])-src),('publish',int(pu['end'])-int(pu['begin'])),('consumer_after_publish',int(r['read_start'])-int(pu['end']))]:add(input_parts,n,x)
out={n:{k:stats(v) for k,v in g.items()} for n,g in [('stages',stages),('close_parts',close_parts),('input_parts',input_parts)]}
if label!='baseline':
 orig=json.load(open(b+'/baseline/per-stock.json'));cur=json.load(open(d+'/per-stock.json'));out['parity']={'equal':orig==cur,'stocks':len(cur),'differences':[k for k in set(orig)|set(cur) if orig.get(k)!=cur.get(k)]}
 orig=json.load(open(b+'/baseline/intents.json'));cur=json.load(open(d+'/intents.json'))
 for a in [orig,cur]:
  for r in a:r.pop('monotonic_ns',None)
 out['intents_equal']=orig==cur
json.dump(out,open(d+'/analysis.json','w'),indent=2);print(json.dumps(out,indent=2))
