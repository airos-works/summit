#!/usr/bin/env python3
"""Exercise real wheel input through an isolated Summit's built-in MCP server.

Serve tests/fixtures, launch a test browser with MCP enabled, then pass its URL,
tab and fixture URL. Read its access token from SUMMIT_TEST_MCP_TOKEN.
This navigates and scrolls the specified tab.
"""
import argparse
import json
import os
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mcp-url', required=True)
    parser.add_argument('--tab', required=True)
    parser.add_argument('--fixture-url', required=True)
    args = parser.parse_args()
    token = os.environ['SUMMIT_TEST_MCP_TOKEN']

    def call(name, **arguments):
        payload = {'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call',
                   'params': {'name': name, 'arguments': {'tab': args.tab, **arguments}}}
        request = urllib.request.Request(args.mcp_url, data=json.dumps(payload).encode(),
                    headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=20) as response:
            answer = json.load(response)
        if 'error' in answer:
            raise RuntimeError(answer['error'])
        result = answer['result']
        if result.get('isError'):
            raise RuntimeError(result['content'])
        text = result['content'][0]['text']
        try:
            return json.loads(text)
        except ValueError:
            return text

    def evaluate(expression):
        result = call('evaluate', expression=expression, timeout_ms=5000)
        if isinstance(result, dict) and 'value' in result:
            return result['value']
        return result

    def check_position(axis, expected):
        # Wait for the animation, then check that it stays at the snap point.
        deadline = time.monotonic() + 3
        while True:
            state = evaluate('scrollState()')
            if state[axis] == expected or time.monotonic() >= deadline:
                break
            time.sleep(.05)
        time.sleep(.15)
        state = evaluate('scrollState()')
        other = 'vertical' if axis == 'horizontal' else 'horizontal'
        assert state == {'x': 0, 'y': 0, axis: expected, other: 0}, state
        print(f'PASS {axis}={expected}; page and other container stay still', flush=True)

    def reset():
        evaluate('recording=false; scrollTo(0,0); horizontal.scrollLeft=0; vertical.scrollTop=0; true')
        time.sleep(.4)

    call('navigate', url=args.fixture_url)
    deadline = time.monotonic() + 30
    while evaluate("typeof scrollState==='function'") is not True:
        assert time.monotonic() < deadline, 'Fixture did not load'
        time.sleep(.1)
    print(f"Display scale: {evaluate('devicePixelRatio')}", flush=True)

    for axis, step in [('horizontal', 320), ('vertical', 180)]:
        reset()
        point = evaluate(f"(()=>{{const r={axis}.getBoundingClientRect();return {{x:r.x+r.width/2,y:r.y+r.height/2}}}})()")
        delta = {'dx': 1, 'dy': 0} if axis == 'horizontal' else {'dx': 0, 'dy': 1}
        for index in range(1, 5):
            call('scroll', **point, **delta, activate=True)
            check_position(axis, index * step)
        for index in range(3, -1, -1):
            call('scroll', **point, **{key: -value for key, value in delta.items()}, activate=True)
            check_position(axis, index * step)
        # A count of one takes the burst path, which used to discard x/y/dx.
        call('scroll', **point, **delta, count=1, activate=True)
        check_position(axis, step)
        reset()
        call('scroll', **point, **delta, count=3, interval_ms=650, activate=True)
        check_position(axis, 3 * step)
        for invalid in ({'x': -1}, {'y': -1}, {'dx': 1001}):
            try:
                call('scroll', **{**point, **delta, **invalid}, count=1, activate=True)
            except RuntimeError:
                print(f'PASS rejects invalid burst {invalid}', flush=True)
            else:
                raise AssertionError(f'Invalid burst accepted: {invalid}')
        check_position(axis, 3 * step)
    reset()
    print('SCROLL_SNAP_RESULT PASS (both axes, reversal, targeted bursts, invalid input)', flush=True)


if __name__ == '__main__':
    main()
