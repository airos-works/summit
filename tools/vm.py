#!/usr/bin/env python3
"""QMP control for Summit's independent Haiku VM. No automatic restarts.

SUMMIT_VM_QMP names another machine's QMP socket (the stock Haiku machine of
tools/haiku-vm: .vm/haiku-r1beta6/qmp.sock)."""
import argparse
import json
import os
import pathlib
import socket
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]


class QMP:
    def __init__(self, path=None):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(15)
        self.socket.connect(str(path or os.environ.get('SUMMIT_VM_QMP') or ROOT / '.vm/qmp.sock'))
        self.file = self.socket.makefile('rwb', buffering=0)
        self.read()
        self.command('qmp_capabilities')

    def read(self):
        line = self.file.readline()
        if not line:
            raise ConnectionError('QEMU closed the QMP connection')
        return json.loads(line)

    def command(self, name, **args):
        self.file.write((json.dumps({'execute': name, 'arguments': args}) + '\n').encode())
        while True:
            reply = self.read()
            if 'error' in reply:
                raise RuntimeError(reply['error'])
            if 'return' in reply:
                return reply['return']

    def key(self, keys):
        return self.command('human-monitor-command', **{'command-line': 'sendkey ' + keys + ' 30'})

    def click(self, x, y, width=1280, height=800, button='left'):
        self.command('input-send-event', events=[
            {'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32767 / width)}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32767 / height)}}])
        self.command('input-send-event', events=[{'type': 'btn', 'data': {'button': button, 'down': True}}])
        time.sleep(0.08)
        self.command('input-send-event', events=[{'type': 'btn', 'data': {'button': button, 'down': False}}])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['status', 'screenshot', 'key', 'click'])
    parser.add_argument('args', nargs='*')
    args = parser.parse_args()
    q = QMP()
    if args.action == 'status':
        print(json.dumps(q.command('query-status')))
    elif args.action == 'screenshot':
        target = pathlib.Path(args.args[0]).resolve() if args.args else ROOT / '.vm/screen.png'
        q.command('screendump', filename=str(target), format='png')
        print(target)
    elif args.action == 'key':
        q.key(args.args[0])
    elif args.action == 'click':
        q.click(*map(int, args.args))
