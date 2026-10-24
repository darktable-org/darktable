#!/usr/bin/env python3
# Smoke probe for the remote AI client path (wire protocol v1, see
# dev-doc/AI_REMOTE.md). Speaks the same frames the editor client sends
# after the remote-only routing change: a load with the CPU preference
# forwarded, then one run with synthetic tensors. Read-only against the
# worker; models stay on the worker host.

import argparse
import hashlib
import json
import math
import socket
import struct
import sys

ELEMSIZE = {1: 4, 2: 1, 3: 1, 4: 4, 5: 8, 10: 2}


def recv_exact(sock, n):
    parts = []
    got = 0
    while got < n:
        chunk = sock.recv(min(1 << 20, n - got))
        if not chunk:
            raise ConnectionError('worker closed mid-frame')
        parts.append(chunk)
        got += len(chunk)
    return b''.join(parts)


def send_frame(sock, obj):
    data = json.dumps(obj, separators=(',', ':')).encode('utf-8')
    sock.sendall(struct.pack('>I', len(data)) + data)


def recv_frame(sock):
    (n,) = struct.unpack('>I', recv_exact(sock, 4))
    return json.loads(recv_exact(sock, n))


def synth_input(desc, i):
    # deterministic floats; run inputs must be fully concrete, so dynamic
    # metadata dims (-1) resolve to 1 here like a real caller would
    desc = dict(desc, shape=[d if d > 0 else 1 for d in desc['shape']])
    count = math.prod(desc['shape'])
    fmt = '<%d%s' % (count, {1: 'f', 2: 'B', 3: 'b', 4: 'i', 5: 'q', 10: 'e'}[desc['type']])
    raw = struct.pack(fmt, *(1.5 + i + (k % 7) * 0.25 for k in range(count)))
    desc = dict(desc, bytes=len(raw))
    return desc, raw


def report(name, descs, blobs):
    print('%s:' % name)
    for d, blob in zip(descs, blobs):
        shape = '/'.join(str(x) for x in d['shape'])
        digest = hashlib.sha256(blob).hexdigest()[:12]
        head = ''
        if d['type'] in (1, 10) and len(blob) >= 2:
            unit = 'f' if d['type'] == 1 else 'e'
            n = min(4, len(blob) // struct.calcsize('<' + unit))
            vals = struct.unpack('<%d%s' % (n, unit), blob[:n * struct.calcsize('<' + unit)])
            head = ' first=%s' % (tuple(round(v, 4) for v in vals),)
        finite = all(math.isfinite(v) for v in
                     struct.unpack('<%df' % (len(blob) // 4), blob)) \
            if d['type'] == 1 and blob else None
        print('  %-24s [%s] %s %dB sha256:%s%s'
              % (d.get('name', '?'), shape, d['type'], len(blob), digest,
                 head + (' all-finite' if finite else '')))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('endpoint', help='HOST:PORT (numeric, loopback via SSH tunnel)')
    ap.add_argument('--model', required=True, help='model directory name')
    ap.add_argument('--file', default='model.onnx', help='onnx basename')
    ap.add_argument('--cpu', action='store_true',
                    help='forward cpu:true (worker runs its CPU EP)')
    ap.add_argument('--opt', type=int, default=0, choices=(0, 1, 2))
    ap.add_argument('--timeout', type=float, default=300.0)
    args = ap.parse_args(argv)

    host, _, port = args.endpoint.rpartition(':')
    try:
        sock = socket.create_connection((host, int(port)), timeout=args.timeout)
    except OSError as exc:
        sys.exit('endpoint %s unreachable: %s' % (args.endpoint, exc))
    with sock:
        sock.settimeout(args.timeout)
        send_frame(sock, {'v': 1, 'op': 'load', 'model': args.model,
                          'file': args.file, 'opt': args.opt,
                          'cpu': bool(args.cpu), 'overrides': []})
        meta = recv_frame(sock)
        if not meta.get('ok'):
            sys.exit('load refused: %s' % meta.get('error', '?'))
        print('provider: %s (cpu=%s)' % (meta.get('provider', '?'), args.cpu))

        run = {'v': 1, 'op': 'run',
               'inputs': [], 'outputs': [dict(d, bytes=0) for d in meta['outputs']]}
        payloads = []
        for i, d in enumerate(meta['inputs']):
            desc, raw = synth_input(d, i)
            run['inputs'].append(desc)
            payloads.append(raw)
        try:
            send_frame(sock, run)
            for raw in payloads:
                sock.sendall(raw)
        except (BrokenPipeError, ConnectionResetError):
            # worker rejected the frame and closed; surface its reason
            sys.exit('run refused: %s' % recv_frame(sock).get('error', '?'))

        resp = recv_frame(sock)
        if not resp.get('ok'):
            sys.exit('run refused: %s' % resp.get('error', '?'))
        blobs = [recv_exact(sock, d['bytes']) for d in resp['outputs']]
        for d, m in zip(resp['outputs'], meta['outputs']):
            d['name'] = m.get('name', '?')
        report('outputs', resp['outputs'], blobs)
        print('smoke OK')


if __name__ == '__main__':
    main()
