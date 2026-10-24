#!/bin/env python3
# A native messaging host for the fixture: answers each message with
# {"echo": message, "argv": arguments, "count": n}; {"quit": true} ends it,
# {"big": true} sends a reply over the 1 MB limit.
import json, struct, sys

count = 0
while True:
    header = sys.stdin.buffer.read(4)
    if len(header) < 4:
        break
    message = json.loads(sys.stdin.buffer.read(struct.unpack('=I', header)[0]))
    count += 1
    if isinstance(message, dict) and message.get('quit'):
        break
    reply = {'echo': message, 'argv': sys.argv[1:], 'count': count}
    if isinstance(message, dict) and message.get('big'):
        reply['padding'] = 'x' * (1024 * 1024 + 16)
    data = json.dumps(reply).encode()
    sys.stdout.buffer.write(struct.pack('=I', len(data)) + data)
    sys.stdout.buffer.flush()
