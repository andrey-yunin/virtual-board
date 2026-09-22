#!/usr/bin/env python3
"""Run as root after make, with vcan0 UP and virtual_board unloaded."""
import errno
import fcntl
import os
from pathlib import Path
import socket
import statistics
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
PARAMS = Path('/sys/module/virtual_board/parameters')


def read(name):
    return int((PARAMS / name).read_text())


# Linux x86_64 ioctl encoding; types/sizes match virtual_board_uapi.h.
SET_LIMITS = 0x40085601
SET_PERIOD = 0x40045602
GET_STATUS = 0x80185603


def command(value):
    with open('/dev/virtual_board', 'w') as stream:
        stream.write(value + '\n')


def ioctl(cmd, data, flags=os.O_RDWR):
    fd = os.open('/dev/virtual_board', flags)
    try:
        return fcntl.ioctl(fd, cmd, data)
    finally:
        os.close(fd)


def status():
    data = bytearray(24)
    ioctl(GET_STATUS, data, os.O_RDONLY)
    return struct.unpack('=iiiIII', data)


def limits(low, high):
    ioctl(SET_LIMITS, struct.pack('=ii', low, high), os.O_WRONLY)


def period(value):
    ioctl(SET_PERIOD, struct.pack('=I', value), os.O_WRONLY)


def reject(action, expected):
    before = status()
    try:
        action()
    except OSError as error:
        assert error.errno == expected, error
    else:
        raise AssertionError('Invalid operation accepted')
    assert status() == before


def drain(sock):
    sock.setblocking(False)
    try:
        while True:
            sock.recv(16)
    except BlockingIOError:
        pass
    finally:
        sock.settimeout(2)


def frames(sock, count=4):
    result = []
    for _ in range(count):
        frame = sock.recv(16)
        can_id, length, data = struct.unpack('=IB3x8s', frame)
        assert can_id == 0x123 and length == 8
        result.append((time.monotonic(), struct.unpack('<hhhBB', data)))
    return result


def check_interval(sock, expected):
    drain(sock)
    sample = frames(sock)
    intervals = [b[0] - a[0] for a, b in zip(sample, sample[1:])]
    measured = statistics.median(intervals)
    assert abs(measured - expected) < 0.05, intervals
    print(f'PASS period {expected}s: median {measured:.6f}s', flush=True)
    return sample[-1][1]


if Path('/sys/module/virtual_board').exists():
    raise SystemExit('Refusing to replace an already loaded module')

with socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as sock:
    sock.setsockopt(socket.SOL_CAN_RAW, socket.CAN_RAW_FILTER,
                    struct.pack('=II', 0x123, 0xFFFFFFFF))
    sock.bind(('vcan0',))
    loaded = False
    try:
        # low exceeds the default high: only the final pair must be checked.
        subprocess.run(['insmod', str(ROOT / 'virtual_board.ko'),
                        'low_decic=800', 'high_decic=1000',
                        'temperature_decic=850', 'period_ms=200'], check=True)
        loaded = True
        assert [read(n) for n in ('temperature_decic', 'low_decic',
                                  'high_decic', 'period_ms')] == [850, 800, 1000, 200]
        print('PASS load parameters and argument ordering', flush=True)
        command('limits 100 700')
        command('temp 250')
        assert read('temperature_decic') == 250
        assert (read('low_decic'), read('high_decic')) == (100, 700)
        command('temp 850')
        assert read('temperature_decic') == 850
        assert status() == (850, 100, 700, 200, 0, 2)
        for name in ('temperature_decic', 'low_decic', 'high_decic', 'period_ms'):
            assert (PARAMS / name).stat().st_mode & 0o222 == 0
        reject(lambda: command('temp 1251'), errno.ERANGE)
        reject(lambda: command('temp abc'), errno.EINVAL)
        reject(lambda: limits(700, 700), errno.EINVAL)
        reject(lambda: limits(700, 100), errno.EINVAL)
        reject(lambda: period(9), errno.ERANGE)
        reject(lambda: period(60001), errno.ERANGE)
        reject(lambda: ioctl(0, 0), errno.ENOTTY)
        reject(lambda: ioctl(GET_STATUS, 0), errno.EFAULT)
        reject(lambda: ioctl(SET_LIMITS, 0), errno.EFAULT)
        reject(lambda: ioctl(SET_PERIOD, 0), errno.EFAULT)
        reject(lambda: ioctl(GET_STATUS, bytearray(24), os.O_WRONLY), errno.EBADF)
        reject(lambda: ioctl(SET_PERIOD, struct.pack('=I', 100), os.O_RDONLY), errno.EBADF)
        reject(lambda: ioctl(GET_STATUS ^ (1 << 16), bytearray(24)), errno.ENOTTY)
        print('PASS write/ioctl coherence, readonly sysfs, errors and access checks', flush=True)
        command('start')
        assert check_interval(sock, .2) == (850, 100, 700, 2, 1)
        limits(50, 900)
        period(400)
        assert status() == (850, 50, 900, 400, 1, 0)
        assert check_interval(sock, .4) == (850, 50, 900, 0, 1)
        command('stop')
        drain(sock)
        period(100)
        command('temp -100')
        sock.settimeout(.3)
        try:
            sock.recv(16)
        except socket.timeout:
            pass
        else:
            raise AssertionError('status sent while stopped')
        command('start')
        assert check_interval(sock, .1) == (-100, 50, 900, 1, 1)
        # Deliberately unload with an active timer.
    finally:
        if loaded:
            subprocess.run(['rmmod', 'virtual_board'], check=True)
    assert not PARAMS.exists()
    print('PASS stopped updates, restart and running unload', flush=True)

for args in [('low_decic=700', 'high_decic=100'), ('period_ms=9',)]:
    result = subprocess.run(['insmod', str(ROOT / 'virtual_board.ko'), *args],
                            capture_output=True, text=True)
    if result.returncode == 0:
        subprocess.run(['rmmod', 'virtual_board'], check=True)
        raise AssertionError(f'Invalid load accepted: {args}')
    assert not PARAMS.exists()
print('PASS invalid loads rejected', flush=True)
