#!/usr/bin/env python
from __future__ import print_function
"""Thin Shanghai launcher: live settings + today's daily, no merged config."""
import argparse, copy, errno, fcntl, glob, hashlib, json, math, os, re, sys, tempfile, time

MODELS = ('model_path','snapshot_baseline_model_path','snapshot_baseline_scaler_path',
          'snapshot_auction59_model_path','snapshot_auction59_scaler_path')
try:
    STRING_TYPES = (basestring,)
    NUMBER_TYPES = (int, long, float)
except NameError:
    STRING_TYPES = (str,)
    NUMBER_TYPES = (int, float)

def load(path):
    with open(path) as f: return json.load(f)
def mkdir(path):
    try: os.makedirs(path)
    except OSError as e:
        if e.errno != errno.EEXIST or not os.path.isdir(path): raise
def abs_setting(cfg, key):
    value = cfg.get(key)
    if not isinstance(value, STRING_TYPES) or not os.path.isabs(value): raise ValueError('absolute '+key+' required')
    return value
def finite(value, key, positive=False, integer=False):
    if isinstance(value, bool) or not isinstance(value, NUMBER_TYPES) or math.isnan(value) or math.isinf(value):
        raise ValueError('finite numeric '+key+' required')
    if (positive and value<=0) or (integer and value!=int(value)):
        raise ValueError('invalid '+key)
    return value
def validate_global_params(globals_):
    if not isinstance(globals_,dict): raise ValueError('daily global_params must be an object')
    for key in ('offset','position_base_line','position_limit'):
        finite(globals_.get(key),'global_params.'+key,positive=True)
    bias='global_bias_factor' if 'global_bias_factor' in globals_ else 'bias_factor'
    finite(globals_.get(bias),'global_params.'+bias,positive=True)
    return globals_
def daily_path_day(pattern, path):
    parts=[]; fields=[]; index=0
    while index < len(pattern):
        token=pattern[index:index+2]
        if token == '%Y':
            parts.append(r'([0-9]{4})'); fields.append('year'); index += 2
        elif token == '%m':
            parts.append(r'([0-9]{2})'); fields.append('month'); index += 2
        elif token == '%d':
            parts.append(r'([0-9]{2})'); fields.append('day'); index += 2
        else:
            parts.append(re.escape(pattern[index])); index += 1
    match=re.match('^'+''.join(parts)+'$',path)
    if not match: return None
    values=dict(zip(fields,match.groups()))
    if set(values) != set(('year','month','day')): return None
    try:
        date_text='%s%s%s'%(values['year'],values['month'],values['day'])
        time.strptime(date_text,'%Y%m%d')
        return int(date_text)
    except (TypeError,ValueError): return None
def inherit_global_params(live, day):
    pattern=abs_setting(live,'daily_config_pattern')
    wildcard=pattern.replace('%Y','????').replace('%m','??').replace('%d','??')
    candidates=[]
    for path in glob.glob(wildcard):
        candidate_day=daily_path_day(pattern,path)
        if candidate_day is not None and candidate_day < day:
            candidates.append((candidate_day,path))
    for candidate_day,path in sorted(candidates,reverse=True):
        try:
            candidate=load(path)
            if not isinstance(candidate,dict):
                continue
            if int(candidate.get('trading_day',0)) != candidate_day:
                continue
            if 'global_params' not in candidate:
                continue
            validate_global_params(candidate['global_params'])
            print('run_journal_trading: global_params inherited from daily %08d' % candidate_day, file=sys.stderr)
            return copy.deepcopy(candidate['global_params'])
        except (IOError,OSError,TypeError,ValueError):
            continue
    raise ValueError('daily global_params missing and no valid historical daily global_params')
def resolve_daily_global_params(live, daily, day):
    if 'global_params' in daily:
        validate_global_params(daily['global_params'])
        return daily
    resolved=copy.deepcopy(daily)
    resolved['global_params']=inherit_global_params(live,day)
    return resolved
def next_epoch(path):
    mkdir(os.path.dirname(path)); lock = open(path, 'a+')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB); lock.seek(0); history=lock.read()
    if history and not history.endswith('\n'): raise ValueError('incomplete TD epoch file')
    old=history.splitlines()[-1] if history else ''
    if old and not old.isdigit(): raise ValueError('invalid TD epoch file')
    # Append rather than truncate the previous durable epoch. A torn last line
    # fails explicitly; it cannot silently reset the account's generation.
    value=max(int(old or '0')+1, int(time.time()*1000000)); lock.seek(0,os.SEEK_END); lock.write(str(value)+'\n'); lock.flush(); os.fsync(lock.fileno())
    fcntl.fcntl(lock.fileno(), fcntl.F_SETFD, 0); return lock, value
def project(live, daily, day, mode, epoch, journal):
    version=live.get('model_version','legacy')
    if version not in ('legacy','v0.6'): raise ValueError('unknown model_version')
    params=daily.get('ins_params'); globals_=daily.get('global_params')
    if not isinstance(params,dict) or not params: raise ValueError('daily ins_params is empty')
    validate_global_params(globals_)
    instruments=[]
    for symbol,p in sorted(params.items()):
        if len(symbol)!=9 or not symbol.endswith('.SH') or not symbol[:6].isdigit(): raise ValueError('invalid SH symbol '+symbol)
        if int(p.get('Date',0))!=day: raise ValueError(symbol+' Date differs from today')
        for key in ('Close','HistoryAmount','FreeShare','HpUpperPrice','HpLowerPrice'):
            finite(p.get(key),symbol+'.'+key,positive=True)
        finite(p.get('HistoryVolatility20d'),symbol+'.HistoryVolatility20d')
        if finite(p.get('static_position'),symbol+'.static_position',integer=True)<0: raise ValueError('negative static_position')
        instruments.append({'instrument':symbol[:6],'trading_date':day,'average_amount':p['HistoryAmount'],'turnover_threshold':p['HistoryAmount']/8000.0,'free_share':p['FreeShare'],'pre_close':p['Close'],'upper_limit':p['HpUpperPrice'],'lower_limit':p['HpLowerPrice'],'history_volatility_20d':p['HistoryVolatility20d']})
        for key in ('listing_date', 'is_ipo_first_day'):
            if key in p: instruments[-1][key] = p[key]
    legacy={'market':'SH','trading_day':day,'global_params':copy.deepcopy(globals_),'ins_params':copy.deepcopy(params)}
    if version == 'v0.6':
        legacy['model_version']=version
        legacy['v06_strategy']=copy.deepcopy(live.get('v06_strategy',{}))
    for p in legacy['ins_params'].values(): p.pop('last_position',None)
    td={'library':abs_setting(live,'td_library'),'config_path':abs_setting(live,'td_config'),'trading_enabled':mode=='live' and live.get('trading_enabled') is True,'production_approval':mode=='live' and live.get('production_approval') is True,'epoch':epoch}
    if 'td_cpu' in live:
        td['cpu'] = int(finite(live['td_cpu'],'td_cpu',integer=True))
        if td['cpu'] < -1 or td['cpu']>254: raise ValueError('td_cpu must be -1 or in [0,254]')
    if finite(live.get('fee_reserve_per_order'),'fee_reserve_per_order')<0: raise ValueError('negative fee reserve')
    runtime={'mode':mode,'account_reference':live['account_reference'],'legacy_config':legacy,'oms':{'journal_path':journal,'fee_reserve_per_order':live['fee_reserve_per_order']},'td':td}
    prediction=dict((k,abs_setting(live,k)) for k in (('model_path',) if version=='v0.6' else MODELS))
    if version == 'v0.6':
        prediction['model_version']=version
        prediction['auction59']={'enabled':False}
    if 'durable_order_intents' in live:
        if type(live['durable_order_intents']) is not bool: raise ValueError('durable_order_intents must be boolean')
        runtime['oms']['durable_order_intents'] = live['durable_order_intents']
    if 'auction59' in live: prediction['auction59'] = copy.deepcopy(live['auction59'])
    profile={'schema_version':1,'market':'SH','execution':mode,'processing_mode':'prediction','processing_contract':'sse-per-instrument-v2','trading_day':day,'processing_sha256':hashlib.sha256(json.dumps({'prediction':prediction,'instruments':instruments},sort_keys=True).encode('utf-8')).hexdigest(),'environment':{'execution':mode,'mode':'live','clock':'host'},'prediction':prediction,'instruments':instruments,'strategy_runtime':runtime}
    return profile
def inherited(value):
    stream=tempfile.TemporaryFile(dir='/dev/shm'); stream.write(json.dumps(value,allow_nan=False).encode('utf-8')); stream.flush(); stream.seek(0); fcntl.fcntl(stream.fileno(),fcntl.F_SETFD,0); return stream,'/proc/self/fd/'+str(stream.fileno())
def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--live-config',required=True); ap.add_argument('--daily-config'); ap.add_argument('--capture-config'); ap.add_argument('--duration-ms',type=int); ap.add_argument('--binary',default='/home/zane/usagi-bin/t0_sse_journal_predict'); g=ap.add_mutually_exclusive_group(); g.add_argument('--query-only',action='store_true'); g.add_argument('--live-orders',action='store_true'); a=ap.parse_args()
    os.environ['TZ']='Asia/Shanghai'; time.tzset(); live=load(a.live_config); a.daily_config=a.daily_config or time.strftime(abs_setting(live,'daily_config_pattern')); daily=load(a.daily_config); day=int(time.strftime('%Y%m%d'))
    if a.duration_ms is not None and (a.duration_ms<0 or a.query_only): raise ValueError('--duration-ms requires monitor/live and a nonnegative value')
    if int(daily.get('trading_day',0))!=day: raise ValueError('daily config is not today')
    mkdir('/run/usagi/oms/accounts')
    root=abs_setting(live,'runtime_root'); epoch_lock,epoch=next_epoch(os.path.join(root,'td.epoch'))
    if a.live_orders and (live.get('trading_enabled') is not True or live.get('production_approval') is not True or os.environ.get('SSE_ENABLE_LIVE_ORDER')!='YES'): raise ValueError('live orders require two config gates and SSE_ENABLE_LIVE_ORDER=YES')
    if not a.live_orders: os.environ['SSE_ENABLE_LIVE_ORDER']='NO'
    if a.query_only:
        journal=os.path.join(root,str(day),'td-query','oms.journal'); mkdir(os.path.dirname(journal)); value={'library':abs_setting(live,'td_library'),'config_path':abs_setting(live,'td_config'),'broker':'guoxin','account':live['account_reference'],'trading_day':day,'daily_config':os.path.abspath(a.daily_config),'journal_path':journal,'epoch':epoch,'timeout_ms':20000}
        if 'td_cpu' in live: value['cpu']=live['td_cpu']
        fd,path=inherited(value); command=[a.binary,'--td-query-only',path]
    else:
        capture=os.path.realpath(a.capture_config or os.path.join(root,'active_capture','capture.json')); c=load(capture)
        if int(c.get('trading_day',0))!=day: raise ValueError('active capture is not today')
        daily=resolve_daily_global_params(live,daily,day)
        journal=os.path.join(root,str(day),'td' if a.live_orders else 'td-monitor','oms.journal'); mkdir(os.path.dirname(journal)); fd,path=inherited(project(live,daily,day,'live' if a.live_orders else 'monitor',epoch,journal)); command=[a.binary,capture,path]
        if a.duration_ms is not None: command.extend(['--duration-ms',str(a.duration_ms)])
    if live.get('library_path'): os.environ['LD_LIBRARY_PATH']=abs_setting(live,'library_path')
    if live.get('model_version')=='v0.6' and not a.query_only:
        audit_dir=os.path.join(root,str(day),'v06');mkdir(audit_dir)
        os.environ['SSE_V06_AUDIT_PATH']=os.path.join(audit_dir,'audit-'+str(epoch)+'.jsonl')
        latency_path=os.path.join(audit_dir,'order-latency-'+str(epoch)+'.jsonl')
        latency_fd=os.open(latency_path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600);os.close(latency_fd)
        os.environ['SSE_ORDER_LATENCY_PATH']=latency_path
    print('SSE trading launcher mode='+('query-only' if a.query_only else ('live' if a.live_orders else 'monitor'))+' day='+str(day)); sys.stdout.flush(); os.execv(a.binary,command)
if __name__=='__main__':
    try: main()
    except (ValueError,KeyError,OSError) as e: print('run_journal_trading: '+str(e),file=sys.stderr); sys.exit(2)
