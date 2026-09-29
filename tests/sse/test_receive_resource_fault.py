#!/usr/bin/env python
"""Exercise fair full-batch draining and persistent gap faults over real UDP."""
from __future__ import print_function
import glob, json, os, signal, socket, struct, subprocess, sys, tempfile, time
import sse_journal_integration_test as helper

def run(capture_binary, predictor_binary):
    root = tempfile.mkdtemp(prefix='sse-receive-fault-')
    processes = []
    handles = []
    try:
        day = int(time.strftime('%Y%m%d'))
        config = helper.make_config(root, (helper.reserve_port(), helper.reserve_port()), day)
        if os.environ.get('SSE_TEST_CPUS'):
            cpus = list(map(int, os.environ['SSE_TEST_CPUS'].split(',')))
            assert len(cpus) == 4
            for key, cpu in zip(('receive_cpu', 'dispatch_cpu', 'journal_cpu', 'prediction_cpu'), cpus):
                config[key] = cpu
        config['receive_batch_size'] = 1
        config['receive_busy_poll'] = config['dispatch_busy_poll'] = bool(os.environ.get('SSE_TEST_BUSY_POLL'))
        config['queue_capacity'] = 16384
        config['journal_queue_capacity'] = 16384
        transport = root + '/transport.json'; profile = root + '/profile.json'
        helper.write_json(transport, config); helper.make_profile(profile, day)
        capture = helper.launch(capture_binary, [transport], root, 'capture')
        processes.append(capture[0]); handles.extend(capture[1:3])
        deadline = time.time() + 10
        while 'Shanghai journal capture ready' not in helper.read_text(capture[4]):
            assert time.time() < deadline
            helper.require_alive(capture[0], 0.02, capture[4], 'capture')
        if os.environ.get('SSE_TEST_CONTROL_CPU'):
            for status in glob.glob('/proc/%d/task/*/status' % capture[0].pid):
                for line in open(status):
                    if line.startswith('Cpus_allowed_list:'):
                        mask = line.split(':', 1)[1].strip()
                        if ',' in mask or '-' in mask:
                            subprocess.check_output(['taskset', '-pc', os.environ['SSE_TEST_CONTROL_CPU'], status.split('/')[-2]])
        sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        pulse = lambda kind: struct.pack('<IIB7s', 1, 0, kind, b'\0'*7)*2
        for unused in range(500):
            for index, kind in ((0, 0xa2), (1, 0x8b)):
                sender.sendto(pulse(kind), ('127.0.0.1', config['channels'][index]['port']))
        for sequence in (1, 3):
            sender.sendto(helper.raw_tick(sequence, sequence, 'A', 1000+sequence, 0),
                          ('127.0.0.1', config['channels'][0]['port']))
        helper.wait_for_datagrams(config['journal_directory'], 1002, 10)
        predictor = helper.launch(predictor_binary, [transport, profile], root, 'predictor')
        processes.append(predictor[0]); handles.extend(predictor[1:3])
        deadline = time.time() + 15
        while predictor[0].poll() is None and time.time() < deadline: time.sleep(0.02)
        assert predictor[0].poll() == 65, helper.read_text(predictor[4])
        fault = [json.loads(line) for line in helper.read_text(predictor[4]).splitlines()
                 if line.startswith('{') and 'journal_prediction_fault' in line][-1]
        assert 'wire_sequence=1' in fault['error'] and 'wire_sequence=3' in fault['error']
        assert 'provider_sequence=' in fault['error'] and 'hardware_ns=' in fault['error']
        with open(fault['journal_file'], 'rb') as f:
            f.seek(fault['journal_offset'] + 16)
            assert struct.unpack('<Q', f.read(8))[0] == fault['event_id']
        assert capture[0].poll() is None
        sender.sendto(pulse(0xa2), ('127.0.0.1', config['channels'][0]['port']))
        sender.close()
        helper.wait_for_datagrams(config['journal_directory'], 1003, 10)
        helper.stop_process(capture[0], signal.SIGTERM, 10, 'capture')
        result = helper.last_json(capture[3])
        assert result['ok'] and result['datagrams'] == 1003, result
        assert result['channels'][1]['datagrams'] == 500, result
        assert result['stats']['full_receive_batches'] == 1003, result
        assert result['stats']['kernel_drops'] == 0, result
        print(json.dumps({'ok': True, 'packets': 1003, 'gap_exit': 65, 'evidence': root}))
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate(); process.wait()
        for handle in handles: handle.close()

if __name__ == '__main__': run(*sys.argv[1:])
