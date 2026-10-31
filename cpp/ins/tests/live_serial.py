#!/usr/bin/env python3
"""Linux production-service regression with one PTY IMU and raw gpsd emulator."""
import datetime
import json
import os
from pathlib import Path
import pty
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time


def frame(code, payload):
    body = bytes([len(payload)+4, 0, code])+payload
    return b'\x77'+body+bytes([sum(body) & 255])


def gps_frame(epoch, valid=True, time_valid=True, tow=None):
    stamp = datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc)
    payload = bytearray(84)
    if tow is None:
        tow = int((epoch - 315964800 + 18) * 1000) % 604800000
    struct.pack_into('<IHBBBBBB', payload, 0, tow, stamp.year, stamp.month,
                     stamp.day, stamp.hour, stamp.minute, stamp.second,
                     7 if time_valid else 0)
    struct.pack_into('<i', payload, 16, stamp.microsecond * 1000)
    payload[20:24] = bytes([3 if valid else 0, 1 if valid else 0, 0, 12])
    struct.pack_into('<iiiiIIiii', payload, 24, 1450000000, -380000000,
                     50000, 50000, 1000, 1500, 0, 0, 0)
    struct.pack_into('<I', payload, 68, 80)
    data = bytes([1, 7, 84, 0]) + payload
    a = b = 0
    for x in data:
        a = (a + x) & 255; b = (b + a) & 255
    return b'\xb5\x62' + data + bytes([a, b])


def lifecycle(binary, schema, capture, case):
    """Exercise production live acquisition, fault exit and recorded transitions."""
    with tempfile.TemporaryDirectory(prefix='ins-lifecycle-') as directory:
        root = Path(directory)
        master, slave = pty.openpty()
        server = client = None
        if case not in ('no_gps', 'baud_delay', 'baud_timeout', 'imu_reject'):
            server = socket.socket()
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind(('127.0.0.1', 2947)); server.listen(); server.setblocking(False)
        config = root/'settings.json'
        config.write_text(json.dumps({'service': {'imu_device': os.ttyname(slave),
            'state_socket': str(root/'state.sock'), 'log_directory': str(root/'logs')}}))
        process = subprocess.Popen([binary, 'live', '--schema', schema, '--config', str(config)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        buffer = bytearray(); commands = []
        sensor_baud = termios.B9600
        uart_ready = 0.
        streaming = None
        counter = 0
        next_sample = next_fix = 0.
        start = time.monotonic()
        stopped = reset = disconnected = False
        fixed_tow = None
        try:
            while process.poll() is None and time.monotonic()-start < 9:
                now = time.monotonic()
                if select.select([master], [], [], .001)[0]:
                    buffer.extend(os.read(master, 4096))
                while len(buffer) >= 2:
                    if buffer[0] != 0x77:
                        del buffer[0]; continue
                    length = buffer[1]+1
                    if len(buffer) < length:
                        break
                    packet = bytes(buffer[:length]); del buffer[:length]
                    code = packet[3]; value = packet[4] if length == 6 else None
                    commands.append((code, value))
                    if termios.tcgetattr(slave)[5] != sensor_baud or now < uart_ready:
                        continue
                    if code == 0x49:
                        os.write(master, frame(code, b'IMU50-T-IAP-GD#AH100-V4-B8#24.09.20SV2.0'))
                    elif code == 0x0b:
                        os.write(master, frame(0x8b, b'\0')); sensor_baud = termios.B460800
                        if case in ('baud_delay', 'baud_timeout'):
                            uart_ready = now + (.15 if case == 'baud_delay' else 10)
                    elif code == 0xf8:
                        os.write(master, frame(code, b'\1'))
                    elif code in (0x0c, 0x56, 0x1f):
                        ack = b'\1' if case == 'imu_reject' and code == 0x56 else b'\0'
                        os.write(master, frame(0x8c if code == 0x0c else code, ack))
                        if code == 0x0c and value == 7:
                            streaming = now; next_sample = now
                if streaming is not None:
                    elapsed = now-streaming
                    if case == 'imu_reset' and elapsed > 2 and not reset:
                        sensor_baud = termios.B9600; reset = True
                    if case == 'imu_counter' and elapsed > 2 and not reset:
                        counter += 64; reset = True
                    while now >= next_sample:
                        send = not (case in ('imu_silent', 'imu_reset') and elapsed > 2)
                        count = counter % 256
                        digits = bytes([count//100, ((count//10)%10)*16+count%10, 0, 0])
                        packet = frame(0x70, struct.pack('<6f', 0, 0, 0, 0, 0, -1)+digits)
                        if case == 'imu_corrupt' and elapsed > 2:
                            packet = packet[:-1] + bytes([packet[-1] ^ 1])
                        if send:
                            os.write(master, packet)
                        counter += 1
                        next_sample += .01 if case == 'imu_rate' else .005
                    if server and client is None and not disconnected:
                        try:
                            client, _ = server.accept(); client.setblocking(False)
                            next_fix = now + (2.2 if case == 'gps_delayed_start' else 0)
                        except BlockingIOError:
                            pass
                    if client and now >= next_fix:
                        if case == 'gps_disconnect' and elapsed > 2:
                            client.close(); client = None; disconnected = True
                        else:
                            epoch = time.time()
                            tow = int((epoch - 315964800 + 18)*1000) % 604800000
                            if fixed_tow is None:
                                fixed_tow = tow
                            if case == 'gps_duplicate':
                                tow = fixed_tow
                            if case == 'gps_no_time':
                                tow = 0  # Cold acquisition need not have a usable epoch yet.
                            if case == 'gps_reset' and elapsed > 2:
                                tow = 0
                            packet = gps_frame(epoch,
                                valid=case not in ('gps_no_fix', 'gps_no_time') and
                                      not (case == 'gps_fix_loss' and elapsed > 2),
                                time_valid=case != 'gps_no_time' and
                                           not (case == 'gps_time_reset' and elapsed > 2), tow=tow)
                            if case == 'gps_silent' and elapsed > 2:
                                packet = b'$GNTXT,unusable output\r\n'
                            try:
                                client.sendall(packet)
                            except (BrokenPipeError, ConnectionResetError):
                                pass
                        next_fix = now + (1. if case == 'gps_rate' else .1)
                    duration = 5 if case in ('gps_fix_loss', 'gps_delayed_start') else 4
                    if elapsed >= duration and case in ('healthy', 'no_gps', 'baud_delay', 'gps_no_fix',
                                                        'gps_no_time', 'gps_fix_loss', 'gps_delayed_start'):
                        process.send_signal(signal.SIGTERM); stopped = True
                if stopped:
                    break
            stdout, stderr = process.communicate(timeout=3)
            if case in ('healthy', 'no_gps', 'baud_delay', 'gps_no_fix', 'gps_no_time', 'gps_fix_loss', 'gps_delayed_start'):
                assert process.returncode == 0, (case, stderr)
            elif case.startswith('gps'):
                assert process.returncode == 75, (case, stderr)
            else:
                assert process.returncode == 1, (case, stderr)
            assert commands.count((0x0b, 7)) == 1, commands
            assert commands.count((0x56, 3)) == (0 if case == 'baud_timeout' else 1), commands
            if case in ('baud_delay', 'baud_timeout'):
                assert commands.count((0x1f, None)) >= 2, commands
            if case == 'baud_timeout':
                assert 'IMU command timed out: 31' in stderr, stderr
            if case == 'imu_reject':
                assert (0x0c, 7) not in commands
            states = []
            for path in (root/'logs').glob('*.mcap'):
                decoded = root/'decoded.jsonl'
                subprocess.run([capture, str(path), str(decoded)], check=True,
                               stdout=subprocess.DEVNULL)
                states.extend(json.loads(line) for line in decoded.read_text().splitlines()
                              if json.loads(line)['kind'] == 'state')
            assert states and states[-1]['stage'] == 'built_in_test', (case, states[-1:])
            assert states[-1]['flags'] == 0, (case, states[-1])
            stages = {s['stage'] for s in states}
            if case in ('healthy', 'gps_delayed_start', 'gps_fix_loss', 'imu_silent', 'imu_reset', 'imu_counter',
                        'imu_corrupt', 'gps_disconnect', 'gps_reset', 'gps_time_reset', 'gps_silent'):
                assert 'coarse_alignment' in stages, (case, stages)
            if case == 'gps_fix_loss':
                assert 'degraded' in stages, stages
            if case in ('no_gps', 'baud_delay', 'baud_timeout', 'gps_no_fix', 'gps_no_time',
                        'gps_duplicate', 'imu_reject'):
                assert stages == {'built_in_test'}, (case, stages)
            assert 'navigating' not in stages, (case, stages)
            return {'case': case, 'exit': process.returncode, 'stages': sorted(stages)}
        finally:
            if process.poll() is None:
                process.kill(); process.wait()
            if client:
                client.close()
            if server:
                server.close()
            os.close(master); os.close(slave)


if __name__ == '__main__':
    assert len(sys.argv) == 4, 'live_serial.py EXECUTABLE SCHEMA.bfbs CAPTURE_EXECUTABLE'
    results = []
    for case in ('baud_delay', 'baud_timeout', 'no_gps', 'healthy', 'imu_reject',
                 'imu_silent', 'imu_reset', 'imu_counter',
                 'imu_corrupt', 'imu_rate', 'gps_no_fix', 'gps_no_time',
                 'gps_fix_loss', 'gps_disconnect', 'gps_silent', 'gps_duplicate',
                 'gps_reset', 'gps_time_reset', 'gps_rate', 'gps_delayed_start'):
        result = lifecycle(*sys.argv[1:], case)
        results.append(result)
        print(json.dumps(result), flush=True)
    print(json.dumps(results, indent=2))
