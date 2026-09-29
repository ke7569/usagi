#!/usr/bin/env python2
"""SSE receive tuning and timestamped loss/resource evidence (Python 2/3)."""
from __future__ import print_function
import argparse, datetime, glob, json, os, re, shlex, subprocess, sys, time

IFACE = 'hqh-p1-k2'
ROOT = '/home/zane/usagi-runtime'
CAPTURE_TEMPLATE = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../deploy/sse/journal_capture.live.json"))
CONTROL_CPU = 128

def read(path):
    with open(path) as f: return f.read().strip()

def cpus(text):
    result = set()
    for part in text.split(','):
        bounds = list(map(int, part.split('-')))
        result.update(range(bounds[0], bounds[-1] + 1))
    return result

def command(args):
    return subprocess.check_output(args).decode('utf-8')

def irqs():
    result = []
    for line in read('/proc/interrupts').splitlines():
        a = line.split()
        if a and a[0].rstrip(':').isdigit():
            counts = []
            for v in a[1:]:
                if not v.isdigit(): break
                counts.append(int(v))
            result.append((a[0][:-1], a[-1], counts))
    return result

def service_environment():
    value = command(['systemctl', 'show', 'sse-journal-trading.service', '-p', 'Environment']).strip()
    return dict(item.split('=', 1) for item in shlex.split(value.split('=', 1)[1]) if '=' in item)

def resource_policy(capture=None, live=None, environment=None, online=None):
    if capture is None:
        with open(CAPTURE_TEMPLATE) as f: capture = json.load(f)
    if live is None:
        with open(ROOT+'/journal_trading.live.json') as f: live = json.load(f)
    if environment is None: environment = service_environment()
    if online is None: online = cpus(read('/sys/devices/system/cpu/online'))
    roles = dict((key, set([int(capture[key])])) for key in
                 ('receive_cpu', 'dispatch_cpu', 'journal_cpu', 'prediction_cpu'))
    roles['capture_control'] = set([CONTROL_CPU])
    roles['td_cpu'] = set([int(live['td_cpu'])])
    workers = environment.get('SSE_PREDICTION_CPUS', '')
    roles['prediction_workers'] = cpus(workers) if workers else set()
    roles['model_workers'] = (set(cpu+1 for cpu in roles['prediction_workers'])
                              if live.get('model_version', 'legacy') == 'v0.6' else set())
    if 'SSE_AUDIT_CPU' in environment:
        base = int(environment['SSE_AUDIT_CPU']); roles['audit'] = set(range(base, base+5))
    for key in ('SSE_OMS_JOURNAL_CPU', 'SSE_ATP_TRACE_CPU', 'SSE_STRATEGY_CPU', 'SSE_SHM_READER_CPU'):
        if key in environment: roles[key] = set([int(environment[key])])
    protected = set().union(*roles.values())
    if not protected <= online: raise ValueError('configured CPU is not online')
    # Keep the established market IRQ domain, excluding every application role.
    market_irq = sorted((set(range(64, 96)) & online) - protected)
    other_irq = sorted(online - protected - set(market_irq))
    if not market_irq or not other_irq: raise ValueError('no nonconflicting IRQ CPUs')
    return {'roles': roles, 'protected': protected, 'market_irq': market_irq, 'other_irq': other_irq}

def irq_target(irq, name, old, movable, policy):
    if name.startswith(IFACE+'-'):
        suffix=name[len(IFACE)+1:]
        cpus_=policy['market_irq']
        return set([cpus_[int(suffix) % len(cpus_) if suffix.isdigit() else -1]])
    if movable and old & (policy['protected'] | set(policy['market_irq'])):
        remaining = old & set(policy['other_irq'])
        return remaining or set([policy['other_irq'][int(irq) % len(policy['other_irq'])]])
    return old

def check_irq_affinity(policy):
    movable = set(line.split()[0].rstrip(':') for line in read('/proc/interrupts').splitlines()
                  if 'PCI-MSI' in line)
    conflicts = []; fixed = []
    try:
        with open(ROOT+'/receive-tuning/fixed-irqs.json') as f: receipt=json.load(f)
        known=receipt['irqs'] if receipt.get('boot_id')==read('/proc/sys/kernel/random/boot_id') else {}
    except (IOError, ValueError, KeyError): known={}
    for irq, name, counts in irqs():
        old=cpus(read('/proc/irq/'+irq+'/smp_affinity_list'))
        if name.startswith(IFACE+'-'):
            if not old <= set(policy['market_irq']): conflicts.append(irq+':'+name)
        elif irq in movable and old & (policy['protected'] | set(policy['market_irq'])):
            proof=known.get(irq,{})
            if proof.get('name')==name and proof.get('affinity')==sorted(old):
                fixed.append({'irq':irq,'name':name,'affinity':sorted(old),'overlap':sorted(old & policy['protected']),'error':proof.get('error')})
            else: conflicts.append(irq+':'+name)
    if conflicts: raise ValueError('IRQ affinity conflicts: '+', '.join(conflicts))
    return fixed

def executable_name(path):
    target = os.readlink(path)
    if target.endswith(' (deleted)'): target = target[:-10]
    name = os.path.basename(target)
    # Atomic replacement can leave the old executable dentry under the
    # temporary filename. The original exec argv remains authoritative.
    with open(os.path.dirname(path) + '/cmdline', 'rb') as source:
        invoked = os.path.basename(source.read().split('\0')[0])
    if invoked in ('t0_sse_journal_capture', 't0_sse_journal_predict') and name.startswith(invoked + '.'):
        return invoked
    return name

def tune():
    # Run only before capture opens its sockets; all originals are retained.
    for exe in glob.glob('/proc/[0-9]*/exe'):
        try:
            if executable_name(exe) == 't0_sse_journal_capture':
                raise RuntimeError('capture is already running')
        except OSError: pass
    backup = {'time': time.time(), 'ring': command(['/usr/sbin/ethtool', '-g', IFACE]), 'affinity': {},
              'numa_balancing': read('/proc/sys/kernel/numa_balancing')}
    policy = resource_policy()
    backup['cpu_roles'] = dict((k, sorted(v)) for k,v in policy['roles'].items())
    plan = {}; required = set(); unmanaged = []; irq_names={}
    movable = set(line.split()[0].rstrip(':') for line in read('/proc/interrupts').splitlines()
                  if 'PCI-MSI' in line)
    for irq, name, counts in irqs():
        path = '/proc/irq/' + irq + '/smp_affinity_list'
        irq_names[path]=name
        old = read(path)
        target = irq_target(irq, name, cpus(old), irq in movable, policy)
        if target != cpus(old):
            plan[path] = ','.join(map(str, sorted(target)))
            backup['affinity'][path] = old
            if name.startswith(IFACE+'-'): required.add(path)
    folder = ROOT + '/receive-tuning'
    if not os.path.isdir(folder): os.makedirs(folder)
    with open(folder + '/before-' + str(int(time.time())) + '.json', 'w') as f:
        json.dump(backup, f, indent=2)
    command(['sysctl', '-w', 'kernel.numa_balancing=0'])
    current = backup['ring'].split('Current hardware settings:')[-1]
    if not re.search(r'RX:\s+4096\b', current):
        subprocess.check_call(['/usr/sbin/ethtool', '-G', IFACE, 'rx', '4096'])
    for path, value in sorted(plan.items()):
        try:
            with open(path, 'w') as f: f.write(value)
        except IOError as error:
            if path in required:
                raise RuntimeError('IRQ write failed %s -> %s: %s' % (path, value, error))
            unmanaged.append({'path': path, 'irq':path.split('/')[3], 'name':irq_names[path], 'affinity': sorted(cpus(read(path))), 'error': str(error)})
            continue
        if cpus(read(path)) != cpus(value): raise RuntimeError('IRQ affinity not applied: ' + path)
    ring = command(['/usr/sbin/ethtool', '-g', IFACE]).split('Current hardware settings:')[-1]
    if not re.search(r'RX:\s+4096\b', ring): raise RuntimeError('RX ring verification failed')
    with open(folder+'/fixed-irqs.json','w') as f:
        json.dump({'boot_id':read('/proc/sys/kernel/random/boot_id'),
                   'irqs':dict((row['irq'],row) for row in unmanaged)},f,indent=2)
    fixed = check_irq_affinity(policy)
    print(json.dumps({'event': 'receive_tuning_applied', 'rx': 4096,
                      'irq_cpus': policy['market_irq'], 'protected_cpus': sorted(policy['protected']), 'changed_irqs': len(plan)-len(unmanaged),
                      'fixed_other_irqs': unmanaged}))

def counters():
    result = {}
    for line in command(['/usr/sbin/ethtool', '-S', IFACE]).splitlines():
        a = line.strip().split(':')
        if len(a) == 2 and re.search('drop|discard|overflow|error|rx_packets', a[0]):
            try: result['nic.' + a[0]] = int(a[1])
            except ValueError: pass
    lines = read('/proc/net/snmp').splitlines()
    for i in range(len(lines)-1):
        if lines[i].startswith('Udp:') and 'InDatagrams' in lines[i]:
            result.update(('udp.' + k, int(v)) for k, v in zip(lines[i].split()[1:], lines[i+1].split()[1:]))
    soft = [list(map(lambda x: int(x, 16), l.split())) for l in read('/proc/net/softnet_stat').splitlines()]
    for i, row in enumerate(soft):
        result['softnet.%d.dropped' % i] = row[1]
        result['softnet.%d.time_squeeze' % i] = row[2]
    return result

def bind_control():
    deadline = time.time() + 15
    while time.time() < deadline:
        pid = command(['systemctl', 'show', 'sse-journal-capture.service', '-p', 'MainPID']).strip().split('=')[-1]
        try:
            log = read(ROOT + '/active_capture/capture.stderr')
            ready = 'Shanghai journal capture ready' in log and executable_name('/proc/'+pid+'/exe') == 't0_sse_journal_capture'
        except (IOError, OSError): ready = False
        if ready:
            changed = []
            for task in glob.glob('/proc/'+pid+'/task/*'):
                allowed = re.search(r'Cpus_allowed_list:\s*(\S+)', read(task+'/status')).group(1)
                if len(cpus(allowed)) > 1:
                    tid = task.rsplit('/', 1)[-1]
                    command(['taskset', '-pc', '128', tid]); changed.append(tid)
            print(json.dumps({'event': 'capture_control_affinity', 'cpu': 128, 'threads': changed}))
            return
        time.sleep(0.05)
    raise RuntimeError('capture did not become ready for control thread affinity')

def sample():
    sockets = []
    for line in read('/proc/net/udp').splitlines()[1:]:
        a = line.split()
        if int(a[1].split(':')[1], 16) in (37105, 37109):
            sockets.append({'local': a[1], 'rx_queue': int(a[4].split(':')[1], 16),
                            'inode': a[9], 'drops': int(a[-1])})
    cpu = {}
    for line in read('/proc/stat').splitlines():
        a = line.split()
        if a and re.match(r'cpu\d+$', a[0]): cpu[a[0]] = list(map(int, a[1:9]))
    irq = dict((i, {'name': n, 'counts': dict((str(c), v) for c, v in enumerate(vs) if v),
                   'affinity': read('/proc/irq/'+i+'/smp_affinity_list')})
               for i, n, vs in irqs() if n.startswith(IFACE) or n.startswith('megasas'))
    capture = None
    try:
        with open(ROOT + '/active_capture/capture.stderr', 'rb') as f:
            f.seek(0, 2); size = f.tell(); f.seek(max(0, size - 65536))
            for line in reversed(f.read().decode('utf-8').splitlines()):
                try: capture = json.loads(line); break
                except ValueError: pass
    except IOError: pass
    return {'sockets': sockets, 'cpu_ticks': cpu, 'irq': irq, 'capture_status': capture,
            'capture_epoch': os.path.realpath(ROOT + '/active_capture'),
            'diskstats': read('/proc/diskstats'), 'loadavg': read('/proc/loadavg')}

def threads():
    result = []
    for proc in glob.glob('/proc/[0-9]*'):
        try:
            name = read(proc + '/comm')
            if not read(proc + '/cmdline'): continue
            for task in glob.glob(proc + '/task/*'):
                status = read(task + '/status')
                allowed = re.search(r'Cpus_allowed_list:\s*(\S+)', status).group(1)
                stat = read(task + '/stat').rsplit(') ', 1)[1].split()
                result.append({'pid': int(proc.rsplit('/', 1)[1]), 'tid': int(task.rsplit('/', 1)[1]),
                    'name': name, 'cpu': int(stat[36]), 'allowed': allowed,
                    'schedstat': read(task + '/schedstat')})
        except (IOError, OSError): pass
    return result

def monitor(duration):
    folder = ROOT + '/' + time.strftime('%Y%m%d')
    if not os.path.isdir(folder): os.makedirs(folder)
    path = folder + '/receive-resources-' + time.strftime('%H%M%S') + '.jsonl'
    previous = {}; started = time.time(); index = 0
    with open(path, 'a', 1) as out:
        while duration <= 0 or time.time() - started < duration:
            stamp = time.time()
            try:
                values = counters()
                delta = dict((k, v - previous[k]) for k, v in values.items() if k in previous and v != previous[k])
                row = sample()
                row.update(event='receive_resources', timestamp=datetime.datetime.now().isoformat(),
                           epoch=stamp, counters=values, delta=delta, baseline=not previous)
                row['counter_resets'] = [k for k, v in delta.items() if v < 0]
                row['loss_alert'] = dict((k, v) for k, v in delta.items() if v > 0 and
                    re.search('drop|discard|overflow|error|Errors', k))
                if index % 5 == 0: row['threads'] = threads()
                previous = values
            except Exception as error:
                row = {'event': 'receive_monitor_error', 'epoch': stamp, 'error': str(error)}
            out.write(json.dumps(row, separators=(',', ':')) + '\n')
            index += 1
            interval = 1 if '09:24' <= time.strftime('%H:%M') <= '09:26' else 5
            time.sleep(max(0.01, interval - (time.time() - stamp)))
    print(path)

def check_startup(day, running):
    import run_journal_trading as launcher
    result = {'event': 'sse_startup_check', 'date': day, 'time': datetime.datetime.now().isoformat()}
    try:
        live = launcher.load(ROOT + '/journal_trading.live.json')
        path = time.strftime(live['daily_config_pattern'], time.strptime(str(day), '%Y%m%d'))
        daily = launcher.load(path)
        if daily.get('trading_day') != day: raise ValueError('daily trading day mismatch')
        daily = launcher.resolve_daily_global_params(live, daily, day)
        profile = launcher.project(live, daily, day, 'monitor', 1, '/dev/null')
        for key in (('model_path',) if live.get('model_version') == 'v0.6' else launcher.MODELS):
            if not os.access(profile['prediction'][key], os.R_OK): raise ValueError('model not readable: ' + key)
        for binary in ('t0_sse_journal_capture', 't0_sse_journal_predict'):
            if not os.access('/home/zane/usagi-bin/'+binary, os.X_OK): raise ValueError('binary missing: '+binary)
        result['instruments'] = len(profile['instruments'])
        if running:
            for unit in ('sse-journal-capture', 'sse-journal-trading', 'sse-receive-monitor'):
                state = command(['systemctl', 'is-active', unit+'.service']).strip()
                if state != 'active': raise ValueError(unit+' is '+state)
            config = launcher.load(ROOT+'/active_capture/capture.json')
            if config.get('trading_day') != day: raise ValueError('active capture date mismatch')
            status = sample()['capture_status'] or {}
            if not status.get('ready') or not status.get('input_valid'): raise ValueError('capture not healthy')
            if not status.get('processing_valid') or status.get('journal_errors') or status.get('journal_overflows'):
                raise ValueError('capture processing or journal is invalid')
            if len(status.get('channels', [])) != 2 or any(c.get('datagrams', 0) == 0 for c in status['channels']):
                raise ValueError('capture has not received both channels')
            prediction = None
            for line in reversed(command(['journalctl', '-u', 'sse-journal-trading.service', '-n', '30', '-o', 'cat', '--no-pager']).splitlines()):
                try: row = json.loads(line)
                except ValueError: continue
                if row.get('event') == 'journal_prediction_status': prediction = row; break
            if not prediction or prediction.get('generation') != config['generation'] or prediction.get('mode') != 'live':
                raise ValueError('prediction has not reached live for this capture generation')
            if prediction.get('processing', {}).get('model_version', 'legacy') != live.get('model_version', 'legacy'):
                raise ValueError('running model version differs from selected version')
            result['model_version'] = live.get('model_version', 'legacy')
            td = prediction.get('processing', {}).get('strategy', {})
            if not td.get('ready') or not td.get('connected'): raise ValueError('TD not ready')
            ring = command(['/usr/sbin/ethtool', '-g', IFACE]).split('Current hardware settings:')[-1]
            if not re.search(r'RX:\s+4096\b', ring): raise ValueError('RX ring is not 4096')
            policy = resource_policy(config, live)
            result['fixed_irq_overlaps'] = check_irq_affinity(policy)
            if read('/proc/sys/kernel/numa_balancing') != '0':
                raise ValueError('automatic NUMA balancing is enabled')
            thread_rows = threads()
            roles = policy['roles']
            capture_roles = ('receive_cpu','dispatch_cpu','journal_cpu','capture_control')
            capture_cpus = set().union(*(roles[k] for k in capture_roles))
            trading_cpus = set().union(*(v for k,v in roles.items() if k not in capture_roles))
            for unit, expected in (('sse-journal-capture', set(map(str,capture_cpus))),
                                   ('sse-journal-trading', set(map(str,trading_cpus)))):
                pid = int(command(['systemctl','show',unit+'.service','-p','MainPID']).strip().split('=')[-1])
                if unit == 'sse-journal-trading':
                    owner = str(config['prediction_cpu'])
                    main_affinity = re.search(r'Cpus_allowed_list:\s*(\S+)',
                        read('/proc/'+str(pid)+'/task/'+str(pid)+'/status')).group(1)
                    if main_affinity != owner:
                        raise ValueError('prediction owner affinity mismatch: '+main_affinity+' expected '+owner)
                actual = set(t['allowed'] for t in thread_rows if t['pid'] == pid)
                if actual != expected:
                    raise ValueError(unit+' thread affinity mismatch: actual='+str(sorted(actual))+' expected='+str(sorted(expected)))
            result.update(capture_generation=config['generation'], prediction_mode='live', td_ready=True,
                          orders_enabled=td.get('orders_enabled'), threads=thread_rows)
        result['ok'] = True
    except Exception as error:
        result.update(ok=False, error=str(error))
    print(json.dumps(result, separators=(',', ':')))
    return result['ok']

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('action', choices=['tune', 'bind-control', 'monitor', 'preflight', 'check-startup'])
    ap.add_argument('--duration', type=int, default=0)
    ap.add_argument('--date', type=int, default=int(time.strftime('%Y%m%d')))
    args = ap.parse_args()
    if args.action == 'tune': tune()
    elif args.action == 'bind-control': bind_control()
    elif args.action == 'monitor': monitor(args.duration)
    else: sys.exit(0 if check_startup(args.date, args.action == 'check-startup') else 1)
