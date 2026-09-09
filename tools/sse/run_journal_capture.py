#!/usr/bin/env python
"""Launch raw SSE capture in a fresh epoch; no strategy/model inputs."""
from __future__ import print_function

import argparse
import errno
import fcntl
import json
import os
import sys
import tempfile
import time
import uuid


def mkdir(path):
    try:
        os.makedirs(path)
    except OSError as error:
        if error.errno != errno.EEXIST or not os.path.isdir(path):
            raise


def replace_link(target, link):
    temporary = link + '.' + str(os.getpid())
    os.symlink(target, temporary)
    os.rename(temporary, link)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True)
    parser.add_argument('--binary', required=True)
    parser.add_argument('--runtime-root', required=True)
    args = parser.parse_args()
    os.environ['TZ'] = 'Asia/Shanghai'
    time.tzset()
    binary = os.path.abspath(args.binary)
    if not os.path.isfile(binary) or not os.access(binary, os.X_OK):
        raise RuntimeError('capture binary is not executable: ' + binary)
    with open(args.config) as source:
        config = json.load(source)
    root = os.path.abspath(args.runtime_root)
    mkdir(root)
    lock = open(os.path.join(root, 'journal_capture.lock'), 'a')
    fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    # The capture inherits this lock, including when invoked outside systemd.
    fcntl.fcntl(lock.fileno(), fcntl.F_SETFD, 0)

    active = os.path.join(root, 'active_capture')
    previous_config = os.path.join(active, 'capture.json')
    if os.path.isfile(previous_config):
        with open(previous_config) as source:
            previous = json.load(source)
        ring = previous.get('shm_path', '')
        # Only this launcher's old disposable ring; journal files are retained.
        if (os.path.dirname(ring) == '/dev/shm' and
                os.path.basename(ring).startswith('usagi-sse-journal-')):
            try:
                os.unlink(ring)
            except OSError as error:
                if error.errno != errno.ENOENT:
                    raise

    day = time.strftime('%Y%m%d')
    day_root = os.path.join(root, day)
    mkdir(day_root)
    epoch = tempfile.mkdtemp(prefix='capture-' + time.strftime('%H%M%S') + '-',
                             dir=day_root)
    config['trading_day'] = int(day)
    config['generation'] = uuid.uuid4().int & ((1 << 63) - 1) or 1
    with open('/proc/sys/kernel/random/boot_id') as source:
        config['boot_id'] = source.read().strip()
    config['journal_directory'] = os.path.join(epoch, 'journal')
    config['shm_path'] = '/dev/shm/usagi-sse-journal-' + os.path.basename(epoch)
    config['duration_ms'] = 0
    config_path = os.path.join(epoch, 'capture.json')
    with open(config_path, 'w') as output:
        json.dump(config, output, indent=2, sort_keys=True)
        output.write('\n')
    replace_link(epoch, os.path.join(day_root, 'current'))
    replace_link(epoch, active)
    print('SSE capture config=' + config_path)
    sys.stdout.flush()
    for descriptor, name in ((1, 'capture.stdout'), (2, 'capture.stderr')):
        fd = os.open(os.path.join(epoch, name), os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o640)
        os.dup2(fd, descriptor)
        os.close(fd)
    os.execv(binary, [binary, config_path])


if __name__ == '__main__':
    main()
