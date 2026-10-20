#!/usr/bin/env python3
"""navigation-trace.py LOG: splits a log of Summit run with SUMMIT_NAV_TRACE=1,
SUMMIT_NET_TRACE=1 (and SUMMIT_STARTUP_TRACE=1) into navigations, each from an
"Enter in the address field" (or "navigation") line, and prints the
milliseconds from the key to each stage: the tab showing that it loads, the
load on the engine's main loop, the old web process asking the navigation
policy, the provisional load (the request leaving, in a new process on a
swap), the network request and its connection and first byte, the response
policy, the first subresource request, the commit, the first visually
non-empty layout, the first frame after the commit and the end of the load.
Notes say when the navigation swapped processes and when web processes were
launched. In a process that showed a page before, the first frame after the
commit can come before the first visually non-empty layout (a frame of the
new, still empty document). See docs/navigation-timing.md."""
import re
import sys

NAV = re.compile(r'^Summit nav (\d+\.\d+) pid=(\d+) (?:\+[\d.]+ ms )?(.*)$')
NET = re.compile(r'^Summit net: (\w+) .*?at=(\d+\.\d+)')
STARTUP = re.compile(r'^Summit startup (\d+\.\d+) pid=(\d+) (.*)$')

STAGES = [
    ('loadurl', 'LoadURL returned'),
    ('spinner', 'tab shows loading'),
    ('mainloop', 'load on engine main loop'),
    ('policy', 'policy asked by web process'),
    ('provisional', 'provisional load started'),
    ('netstart', 'first network start'),
    ('reqstart', 'main request start'),
    ('connected', 'connection ready'),
    ('firstbyte', 'first byte'),
    ('respolicy', 'response policy'),
    ('substart', 'first subresource request'),
    ('commit', 'commit'),
    ('fvnel', 'first visually non-empty layout'),
    ('frame', 'first frame after commit'),
    ('finished', 'load finished'),
]


def parse(path):
    navs = []
    cur = None
    for raw in open(path, errors='replace'):
        line = raw.rstrip('\n')
        m = NAV.match(line)
        if m:
            t = float(m.group(1)) * 1000
            text = m.group(3)
            if text.startswith('Enter in the address field') or text.startswith('navigation (key at'):
                cur = {'t0': t, 'events': {}, 'notes': [], 'url': None, 'mainreq': None, 'launch': []}
                navs.append(cur)
                continue
            if cur is None:
                continue
            ev = cur['events']
            rel = t - cur['t0']

            def put(key):
                ev.setdefault(key, rel)
            if text.startswith('LoadURL returned'):
                put('loadurl')
            elif text.startswith('LoadURL '):
                cur['url'] = text.split(' ', 1)[1]
            elif text.startswith('tab state loading=1'):
                put('spinner')
            elif text.startswith('load request reached the main loop'):
                put('mainloop')
            elif text.startswith('navigation policy asked'):
                put('policy')
                if 'redirect' in text:
                    cur['notes'].append('redirect@%.0f' % rel)
            elif text.startswith('provisional load started'):
                put('provisional')
                if 'new web process' in text and 'swap' not in cur:
                    cur['swap'] = True
            elif text.startswith('response policy asked'):
                put('respolicy')
            elif text.startswith('committed'):
                put('commit')
            elif text.startswith('first visually non-empty'):
                put('fvnel')
            elif text.startswith('first frame after the commit'):
                put('frame')
            elif text.startswith('load finished'):
                put('finished')
            elif text.startswith('provisional load failed'):
                cur['notes'].append('failed@%.0f' % rel)
            continue
        m = STARTUP.match(line)
        if m and cur is not None and 'launching WebProcess' in m.group(3):
            cur['launch'].append(float(m.group(1)) * 1000 - cur['t0'])
            continue
        m = NET.match(line)
        if m and cur is not None:
            kind, at = m.group(1), float(m.group(2)) * 1000
            rel = at - cur['t0']
            ev = cur['events']
            url = line.rsplit(' ', 1)[-1]
            target = cur['url'] or ''
            def norm(u):
                return u.split('#')[0].rstrip('/')
            same = bool(target) and norm(url) == norm(target)
            ident = re.search(r'id=(0x[0-9a-f]+)', line)
            ident = ident.group(1) if ident else None
            if kind == 'start' and not same and target:
                ev.setdefault('substart', rel)
            if kind == 'start' and same:
                ev.setdefault('netstart', rel)
                if cur.get('cachelookup'):
                    ev.setdefault('reqstart', rel)
                    cur['mainreq'] = cur['mainreq'] or ident
            elif kind == 'cache' and same:
                cur['cachelookup'] = True
                if ' hit ' in line:
                    cur['notes'].append('cache hit')
            elif kind == 'preconnected' and same:
                ev.setdefault('connected', rel)
            elif kind == 'done' and ident and ident == cur['mainreq']:
                fb = re.search(r'first-byte-at=(\d+\.\d+)', line)
                if fb:
                    ev.setdefault('firstbyte', float(fb.group(1)) * 1000 - cur['t0'])
                m2 = re.search(r'dns=(-?\d+) connect=(-?\d+) tls=(-?\d+)', line)
                if m2 and m2.group(2) != '0':
                    cur['notes'].append('own connection dns=%s connect=%s tls=%s' % m2.groups())
                # Request identifiers are addresses, used again by later requests.
                cur['mainreq'] = 'done'
    return navs


def main():
    path = sys.argv[1]
    navs = parse(path)
    keys = [k for k, _ in STAGES]
    print('%-46s ' % 'url' + ' '.join('%8s' % k[:8] for k in keys) + '  notes')
    for nav in navs:
        ev = nav['events']
        cells = ['%8.0f' % ev[k] if k in ev else '%8s' % '-' for k in keys]
        notes = list(nav['notes'])
        if nav.get('swap'):
            notes.append('process swap')
        if nav['launch']:
            notes.append('launch@' + ','.join('%.0f' % x for x in nav['launch']))
        print('%-46.46s ' % (nav['url'] or '?') + ' '.join(cells) + '  ' + '; '.join(notes))


if __name__ == '__main__':
    main()
