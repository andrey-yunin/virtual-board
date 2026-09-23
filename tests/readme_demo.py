#!/usr/bin/env python3
"""Checks 1–3 from README; run with sudo after make. Logs stay in /tmp.

Does not replace an already loaded module or bring an existing interface down.
The optional CAN down/up test is intentionally left to the manual instructions.
"""
import errno
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
DEVICE = Path('/dev/virtual_board')
PROC = Path('/proc/virtual_board')
CLASS = Path('/sys/class/virtual_board')
MODULE = Path('/sys/module/virtual_board')


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    require(os.geteuid() == 0, 'Run with sudo after make')
    require(not MODULE.exists(), 'Module already loaded; finish the previous session first')
    require((ROOT / 'virtual_board.ko').exists(), 'Run make first')
    require((ROOT / 'tools/boardctl').exists(), 'Run make first')
    logs = Path(tempfile.mkdtemp(prefix='virtual-board-readme-'))
    print(f'Logs: {logs}', flush=True)
    journal = (logs / 'run.log').open('w', buffering=1)
    children = []
    handles = []
    loaded = False
    success = False

    def note(text):
        print(text, flush=True)
        journal.write(text + '\n')

    def run(*args):
        result = subprocess.run(args, cwd=ROOT, text=True, capture_output=True, timeout=30)
        note('$ ' + ' '.join(map(str, args)) + '\n' + result.stdout + result.stderr)
        require(result.returncode == 0, f'Command failed: {args}')
        return result.stdout.strip()

    def write(value):
        note('write: ' + value)
        with DEVICE.open('w') as stream:
            stream.write(value + '\n')

    def status(temp, state, diagnosis, high=700, period=1000):
        expected = (f'temp={temp} low=100 high={high} period_ms={period} '
                    f'state={state} temp_status={diagnosis}')
        require(run('./tools/boardctl', 'status') == expected, 'Unexpected ioctl status')

    def background(name, *args):
        out = (logs / (name + '.log')).open('wb')
        err = (logs / (name + '.err')).open('wb')
        handles.extend([out, err])
        child = subprocess.Popen(args, stdout=out, stderr=err)
        children.append(child)
        return child

    def snapshot():
        value = PROC.read_text()
        note('/proc/virtual_board:\n' + value)
        return value

    def readers(expected):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            values = [(logs / (name + '.log')).read_text().splitlines()
                      for name in ('reader-b', 'reader-c')]
            if all(len(lines) >= len(expected) for lines in values):
                break
            time.sleep(0.05)
        require(values == [expected, expected], f'Unexpected reader output: {values}')

    def frames():
        text = (logs / 'can.log').read_text()
        return [(float(ts), ident, payload.upper()) for ts, ident, payload in
                re.findall(r'\((\d+\.\d+)\)\s+vcan0\s+(123|124)#([0-9A-Fa-f]{16})', text)]

    def event(kind, temp, state, diagnosis, high=700):
        return (f'event={kind} temp={temp} low=100 high={high} state={state} '
                f'temp_status={diagnosis} error=0')

    try:
        run('modprobe', 'vcan')
        links = json.loads(run('ip', '-details', '-json', 'link', 'show'))
        existing = next((link for link in links if link['ifname'] == 'vcan0'), None)
        if existing:
            require(existing.get('linkinfo', {}).get('info_kind') == 'vcan',
                    'Existing vcan0 is not a vcan interface')
            require('UP' in existing['flags'], 'Existing vcan0 is down; prepare it manually')
        else:
            run('ip', 'link', 'add', 'dev', 'vcan0', 'type', 'vcan')
            run('ip', 'link', 'set', 'dev', 'vcan0', 'up')
        run('insmod', './virtual_board.ko', 'can_iface=vcan0', 'temperature_decic=250',
            'low_decic=100', 'high_decic=700', 'period_ms=1000')
        loaded = True
        require(all(path.exists() for path in (DEVICE, PROC, CLASS / 'virtual_board')),
                'Missing device, proc or sys path')
        status(250, 'stopped', 'normal')
        cats = [background(name, 'cat', str(DEVICE)) for name in ('reader-b', 'reader-c')]
        background('can', 'candump', '-L', 'vcan0')
        deadline = time.monotonic() + 3
        while 'clients=2\n' not in PROC.read_text() and time.monotonic() < deadline:
            time.sleep(0.05)
        initial = snapshot()
        for child in cats:
            require(f'pid={child.pid} opens=1 readers=1 lost_events=0' in initial,
                    'Reader did not register')
        time.sleep(0.3)
        require(not frames(), 'Unexpected CAN traffic before start')
        expected = []
        steps = [('start', 250, 'normal', 'started', 'FA006400BC020001', 3),
                 ('temp 850', 850, 'overheat', 'temp_transition', '52036400BC020201', 2),
                 ('temp 500', 500, 'normal', 'temp_transition', 'F4016400BC020001', 2),
                 ('temp 50', 50, 'undertemp', 'temp_transition', '32006400BC020101', 2),
                 ('temp 100', 100, 'normal', 'temp_transition', '64006400BC020001', 2),
                 ('temp 700', 700, 'normal', None, 'BC026400BC020001', 2)]
        for command, temp, diagnosis, kind, payload, delay in steps:
            write(command)
            time.sleep(delay)
            status(temp, 'running', diagnosis)
            if kind:
                expected.append(event(kind, temp, 'running', diagnosis))
            readers(expected)
            require(any(ident == '123' and data == payload for _, ident, data in frames()),
                    f'Missing periodic payload: {payload}')
        write('stop')
        time.sleep(3)
        status(700, 'stopped', 'normal')
        expected.append(event('stopped', 700, 'stopped', 'normal'))
        readers(expected)
        captured = frames()
        transitions = [data for _, ident, data in captured if ident == '124']
        require(transitions == [step[4] for step in steps[1:5]], 'Wrong CAN transitions')
        timestamps = [ts for ts, ident, _ in captured if ident == '123']
        intervals = [b - a for a, b in zip(timestamps, timestamps[1:])]
        require(bool(intervals), 'Not enough periodic CAN frames')
        median = statistics.median(intervals)
        note(f'CAN interval median: {median:.6f} seconds')
        require(0.75 <= median <= 1.25, 'CAN period is not approximately one second')
        note('PASS: check 1')

        write('temp 850')
        status(850, 'stopped', 'overheat')
        require((CLASS / 'virtual_board/temp_status').read_text().strip() == 'overheat',
                'sysfs did not show overheat')
        run('./tools/boardctl', 'limits', '100', '900')
        run('./tools/boardctl', 'period', '2000')
        status(850, 'stopped', 'normal', 900, 2000)
        for name, value in [('temperature_decic', '850'), ('low_decic', '100'),
                            ('high_decic', '900'), ('period_ms', '2000')]:
            actual = (MODULE / 'parameters' / name).read_text().strip()
            note(f'sysfs {name}={actual}')
            require(actual == value, 'Numeric sysfs mismatch')
        for name, value in [('state', 'stopped'), ('temp_status', 'normal')]:
            actual = (CLASS / 'virtual_board' / name).read_text().strip()
            note(f'sysfs {name}={actual}')
            require(actual == value, 'Class sysfs mismatch')
        summary = snapshot()
        require(summary.splitlines()[0] ==
                'temp=850 low=100 high=900 period_ms=2000 state=stopped temp_status=normal clients=2',
                'proc status mismatch')
        require(summary.count('opens=1 readers=1 lost_events=0') == 2, 'proc readers mismatch')
        expected.extend([event('temp_transition', 850, 'stopped', 'overheat'),
                         event('temp_transition', 850, 'stopped', 'normal', 900)])
        readers(expected)
        note('PASS: check 2')

        before = run('./tools/boardctl', 'status')
        try:
            write('limits 900 100')
        except OSError as error:
            note(f'Invalid write: errno={error.errno} ({error.strerror})')
            require(error.errno == errno.EINVAL, 'Expected EINVAL')
        else:
            raise RuntimeError('Invalid limits accepted')
        require(run('./tools/boardctl', 'status') == before, 'Rejected write changed state')
        time.sleep(2.2)
        readers(expected)
        require(frames() == captured, 'CAN continued after stop')
        require(all(child.poll() is None for child in children), 'A reader or candump exited')
        note('PASS: check 3')
        success = True
    finally:
        cleanup_errors = []
        if loaded:
            try:
                write('stop')
            except Exception as error:
                cleanup_errors.append(str(error))
        for child in children:
            if child.poll() is None:
                child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        for handle in handles:
            handle.close()
        if loaded:
            try:
                summary = snapshot()
                require('state=stopped' in summary and 'clients=0\n' in summary,
                        'Expected stopped with no clients before unload')
            except Exception as error:
                cleanup_errors.append(str(error))
            try:
                run('rmmod', 'virtual_board')
                require(not any(path.exists() for path in (DEVICE, PROC, CLASS, MODULE)),
                        'Interface paths survived unload')
            except Exception as error:
                cleanup_errors.append(str(error))
        if cleanup_errors:
            note('CLEANUP FAILED: ' + '; '.join(cleanup_errors))
        note('PASS: checks 1–3 and unload' if success and not cleanup_errors else 'FAIL')
        note('Optional check 4 was not run. vcan0 is left in place, as in README.')
        journal.close()
        require(not cleanup_errors, 'Cleanup failed; see run.log')


if __name__ == '__main__':
    main()
