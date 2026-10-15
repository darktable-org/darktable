#!/usr/bin/env python3
# Remote darktable AI worker: wire protocol v1 (see dev-doc/AI_REMOTE.md).
# Loopback plus SSH is the authentication boundary; models stay on the host.

import argparse
import ipaddress
import json
import logging
import os
import select
import socket
import struct
import sys
import threading
import time
from socketserver import ThreadingTCPServer, StreamRequestHandler

import numpy as np

PROTOCOL_VERSION = 1
MAX_JSON = 64 * 1024
MAX_MESSAGE = 512 * 1024 * 1024
MAX_TENSORS = 16
MIN_RANK, MAX_RANK = 0, 8
CONNECTION_TIMEOUT = 300
MAX_CONNECTIONS = 4
DEFAULT_PORT = 8794

# dt_ai_dtype_t (src/ai/backend.h) -> little-endian numpy dtypes
DTYPES = {1: '<f4', 2: 'u1', 3: 'i1', 4: '<i4', 5: '<i8', 10: '<f2'}
ELEMSIZE = {1: 4, 2: 1, 3: 1, 4: 4, 5: 8, 10: 2}
FLOAT_DTYPES = frozenset((1, 10))
# onnxruntime tensor type strings -> dt_ai_dtype_t
ORT_TYPES = {'tensor(float)': 1, 'tensor(uint8)': 2, 'tensor(int8)': 3,
             'tensor(int32)': 4, 'tensor(int64)': 5, 'tensor(float16)': 10}
OPT_LEVEL_NAMES = {0: 'ORT_ENABLE_ALL', 1: 'ORT_ENABLE_BASIC',
                   2: 'ORT_DISABLE_ALL'}
# wire opt string names -> levels, mirroring _parse_opt_string() in
# src/ai/backend_common.c ("all"/"basic"/"disabled"/"none", case-insensitive)
OPT_STRINGS = {'all': 0, 'basic': 1, 'disabled': 2, 'none': 2}
PROVIDER_NAMES = {'CUDAExecutionProvider': 'CUDA',
                  'CoreMLExecutionProvider': 'COREML'}

INFERENCE_LOCK = threading.Lock()
CONNECTION_SLOTS = threading.BoundedSemaphore(MAX_CONNECTIONS)


class ProtocolError(Exception):
    """Client-visible protocol/validation failure; connection closes after."""


def recv_exact(sock, n):
    parts = []
    remaining = n
    while remaining:
        chunk = sock.recv(min(remaining, 1 << 20))
        if not chunk:
            raise ConnectionError('peer closed connection')
        parts.append(chunk)
        remaining -= len(chunk)
    return b''.join(parts)


def recv_frame(sock):
    (n,) = struct.unpack('>I', recv_exact(sock, 4))
    if n > MAX_JSON:
        raise ProtocolError(f'JSON frame {n} bytes exceeds {MAX_JSON} limit')
    raw = recv_exact(sock, n)
    try:
        msg = json.loads(raw.decode('utf-8'))
    except (UnicodeDecodeError, ValueError):
        raise ProtocolError('malformed JSON frame')
    if not isinstance(msg, dict):
        raise ProtocolError('JSON frame must be an object')
    return msg, n


def send_frame(sock, obj, payloads=()):
    data = json.dumps(obj, separators=(',', ':')).encode('utf-8')
    sock.sendall(struct.pack('>I', len(data)) + data)
    for payload in payloads:
        sock.sendall(payload)


def validate_tensor_desc(desc, where, allow_dynamic=False):
    if not isinstance(desc, dict):
        raise ProtocolError(f'{where}: descriptor must be an object')
    dtype = desc.get('type')
    if isinstance(dtype, bool) or dtype not in DTYPES:
        raise ProtocolError(f'{where}: unknown dtype {dtype!r}')
    shape = desc.get('shape')
    if not isinstance(shape, list) or not MIN_RANK <= len(shape) <= MAX_RANK:
        raise ProtocolError(f'{where}: rank must be {MIN_RANK}..{MAX_RANK}')
    for dim in shape:
        if isinstance(dim, bool) or not isinstance(dim, int) or dim == 0 \
                or (dim < 0 and not (allow_dynamic and dim == -1)):
            raise ProtocolError(f'{where}: dims must be positive integers'
                                + (' or -1' if allow_dynamic else '')
                                + f', got {shape!r}')
    nbytes = desc.get('bytes')
    if isinstance(nbytes, bool) or not isinstance(nbytes, int) or nbytes < 0:
        raise ProtocolError(f'{where}: bad bytes {nbytes!r}')
    if nbytes == 0:
        if not allow_dynamic:
            raise ProtocolError(f'{where}: zero-byte descriptors are only valid for dynamic outputs')
        return 0  # dynamic output: caller allocates after validating the response
    if any(d == -1 for d in shape):
        raise ProtocolError(f'{where}: dynamic dims require bytes=0')
    need = ELEMSIZE[dtype]
    for dim in shape:
        need *= dim
    if nbytes != need:
        raise ProtocolError(f'{where}: bytes {nbytes} does not match shape product {need}')
    return nbytes


def validate_run_request(msg, json_len):
    inputs = msg.get('inputs')
    outputs = msg.get('outputs')
    for name, lst in (('inputs', inputs), ('outputs', outputs)):
        if not isinstance(lst, list) or not 1 <= len(lst) <= MAX_TENSORS:
            raise ProtocolError(f'{name}: must be a list of 1..{MAX_TENSORS} descriptors')
    in_bytes = [validate_tensor_desc(d, f'inputs[{i}]') for i, d in enumerate(inputs)]
    [validate_tensor_desc(d, f'outputs[{i}]', allow_dynamic=True)
     for i, d in enumerate(outputs)]
    if json_len + sum(in_bytes) > MAX_MESSAGE:
        raise ProtocolError('request exceeds 512 MiB message limit')
    return inputs, outputs


def check_response_bounds(total_out_bytes):
    if MAX_JSON + total_out_bytes > MAX_MESSAGE:
        raise ProtocolError('response exceeds 512 MiB message limit')


def _one_component(name, what):
    if not isinstance(name, str) or not name or name in ('.', '..') \
            or os.path.basename(name) != name:
        raise ProtocolError(f'{what} must be a bare name, got {name!r}')
    return name


def resolve_model(root, model, fname):
    model = _one_component(model, 'model')
    _one_component(fname, 'file')
    if not fname.endswith('.onnx') or len(fname) <= len('.onnx'):
        raise ProtocolError('file must be a *.onnx basename')
    # realpath resolves symlinks: dir must sit directly in the models root,
    # the file directly in the model dir, so nothing escapes via links
    model_dir = os.path.realpath(os.path.join(root, model))
    if os.path.dirname(model_dir) != root:
        raise ProtocolError('unknown model')
    path = os.path.realpath(os.path.join(model_dir, fname))
    if os.path.dirname(path) != model_dir or not os.path.isfile(path):
        raise ProtocolError('unknown model file')
    try:
        with open(os.path.join(model_dir, 'config.json'), 'rb') as f:
            cfg = json.load(f)
        model_id = cfg['id']
    except (OSError, ValueError, TypeError, KeyError):
        raise ProtocolError('model config.json missing or invalid')
    if model_id != model:
        raise ProtocolError('model name does not match config.json id')
    return path, cfg


def choose_provider(cfg, fname, cpu, gpu_provider='CUDAExecutionProvider'):
    accelerators = {'CUDAExecutionProvider': 'CUDA', 'CoreMLExecutionProvider': 'COREML'}
    if gpu_provider not in accelerators:
        raise ProtocolError('unsupported GPU provider')
    if cpu:
        return 'CPUExecutionProvider'
    block = cfg.get('cpu_only')
    if isinstance(block, dict):
        block = block.get(fname[:-len('.onnx')], [])
    if isinstance(block, list) and any(
            isinstance(entry, str) and entry.upper() == accelerators[gpu_provider] for entry in block):
        raise ProtocolError(f'model marks this file cpu_only for {accelerators[gpu_provider]}; '
                            'refusing without fallback')
    return gpu_provider


def resolve_opt(cfg, fname, provider_name, requested):
    """Resolve a load request's opt to a concrete session level.

    Explicit 0/1/2 pass through untouched. -1 asks for the manifest
    default, resolved exactly like _resolve_ort_optimization() in
    src/ai/backend_common.c: ort_optimization_provider (keyed by provider
    name, case-insensitive) beats ort_optimization; both accept a flat
    string or a stem-keyed object ("model_bayer": "basic"); unknown or
    missing values fall back to all (0)."""
    if requested in (0, 1, 2):
        return requested
    stem = fname[:-len('.onnx')] if fname.endswith('.onnx') else fname

    def level_of(value):
        return OPT_STRINGS.get(value.lower()) if isinstance(value, str) else None

    def lookup(node):
        if isinstance(node, str):
            return level_of(node)
        if isinstance(node, dict) and stem in node:
            return level_of(node[stem])
        return None

    block = cfg.get('ort_optimization_provider')
    if isinstance(block, dict):
        for key, value in block.items():
            if isinstance(key, str) and key.upper() == provider_name.upper():
                got = lookup(value)
                if got is not None:
                    return got
                break  # provider matched but gave nothing usable: fall through
    got = lookup(cfg.get('ort_optimization'))
    return 0 if got is None else got


def validate_overrides(ovr):
    if ovr is None:
        return []
    if not isinstance(ovr, list):
        raise ProtocolError('overrides must be a list')
    out = []
    for o in ovr:
        if not isinstance(o, dict) or not isinstance(o.get('name'), str) or not o['name'] \
                or isinstance(o.get('value'), bool) or not isinstance(o.get('value'), int) \
                or o['value'] <= 0:
            raise ProtocolError('override entries need {"name": str, "value": positive int}')
        out.append((o['name'], o['value']))
    return out


def convert(arr, to_dtype):
    want = np.dtype(DTYPES[to_dtype])
    if arr.dtype == want:
        return arr
    if to_dtype in FLOAT_DTYPES and arr.dtype.kind == 'f' and arr.dtype.itemsize in (2, 4):
        return arr.astype(want)
    raise ProtocolError(f'no implicit cast from {arr.dtype} to {want}')


class Session:
    def __init__(self, path, model, fname, provider, opt, overrides):
        try:
            import onnxruntime as ort
        except ImportError as exc:
            raise ProtocolError(f'onnxruntime unavailable: {exc}')
        if provider == 'CUDAExecutionProvider':
            ort.preload_dlls(directory="")
        so = ort.SessionOptions()
        so.graph_optimization_level = getattr(ort.GraphOptimizationLevel, OPT_LEVEL_NAMES[opt])
        for name, value in overrides:
            so.add_free_dimension_override_by_name(name, value)
        providers = [provider]
        if provider == 'CoreMLExecutionProvider':
            so.add_session_config_entry('session.disable_cpu_ep_fallback', '1')
            providers = [(provider, {'ModelFormat': 'MLProgram', 'MLComputeUnits': 'CPUAndGPU',
                                     'AllowLowPrecisionAccumulationOnGPU': '0'})]
        try:
            sess = ort.InferenceSession(path, so, providers=providers)
            sess.disable_fallback()
        except Exception as exc:
            raise ProtocolError(f'session creation failed: {exc}')
        self.provider = sess.get_providers()[0]
        if self.provider != provider:
            raise ProtocolError(
                f'{provider} did not load (active: {self.provider}); no CPU fallback')
        self.model, self.file = model, fname
        self.inputs = self._describe(sess.get_inputs())
        self.outputs = self._describe(sess.get_outputs())
        self.input_types = [d['type'] for d in self.inputs]
        self.output_types = [d['type'] for d in self.outputs]
        self._sess = sess

    @staticmethod
    def _describe(metas):
        descs = []
        for m in metas:
            dtype = ORT_TYPES.get(m.type)
            if dtype is None:
                raise ProtocolError(f'{m.name}: unsupported tensor type {m.type}')
            descs.append({'name': m.name, 'type': dtype,
                          'shape': [d if isinstance(d, int) else -1 for d in m.shape]})
        return descs

    def run(self, feed):
        return self._sess.run(None, feed)


def load_session(msg, root, gpu_provider='CUDAExecutionProvider'):
    opt = msg.get('opt', 0)
    if isinstance(opt, bool) or not isinstance(opt, int) or opt not in (-1, 0, 1, 2):
        raise ProtocolError('opt must be -1 (manifest default) or 0, 1, 2')
    cpu = msg.get('cpu', False)
    if not isinstance(cpu, bool):
        raise ProtocolError('cpu must be a boolean')
    path, cfg = resolve_model(root, msg.get('model'), msg.get('file'))
    provider = choose_provider(cfg, msg['file'], cpu, gpu_provider)
    opt = resolve_opt(cfg, msg.get('file'),
                      'CPU' if cpu else PROVIDER_NAMES[gpu_provider], opt)
    session = Session(path, msg['model'], msg['file'], provider, opt,
                      validate_overrides(msg.get('overrides')))
    logging.info('loaded %s/%s opt=%d cpu=%s provider=%s',
                 session.model, session.file, opt, cpu, session.provider)
    return session


def load_response(session):
    return {'ok': True, 'inputs': session.inputs,
            'outputs': session.outputs, 'provider': session.provider}


def run_inference(session, inputs, outputs, sock):
    if len(inputs) != len(session.inputs) or len(outputs) != len(session.outputs):
        raise ProtocolError(f'model expects {len(session.inputs)} inputs and '
                            f'{len(session.outputs)} outputs')
    feed = {}
    for i, desc in enumerate(inputs):
        raw = recv_exact(sock, desc['bytes'])
        arr = np.frombuffer(raw, dtype=np.dtype(DTYPES[desc['type']])).reshape(desc['shape'])
        feed[session.inputs[i]['name']] = convert(arr, session.input_types[i])
    # ponytail: single global lock serializes GPU peak across the <=4
    # connections; replace with a queue if throughput ever matters
    with INFERENCE_LOCK:
        started = time.monotonic()
        results = session.run(feed)
        elapsed = time.monotonic() - started
    out_descs = []
    payloads = []
    total_out = 0
    for i, desc in enumerate(outputs):
        arr = convert(np.asarray(results[i]), desc['type'])
        if desc['bytes']:
            capacity = desc['bytes'] // ELEMSIZE[desc['type']]
            if arr.size > capacity:
                raise ProtocolError(f'outputs[{i}]: {arr.size} elements exceed caller capacity {capacity}')
        if len(arr.shape) > len(desc['shape']):
            raise ProtocolError(f'outputs[{i}]: rank {len(arr.shape)} exceeds caller rank {len(desc["shape"])}')
        total_out += arr.nbytes
        out_descs.append({'type': desc['type'], 'shape': list(arr.shape), 'bytes': arr.nbytes})
        payloads.append(arr.tobytes())
    check_response_bounds(total_out)
    logging.info('run %s/%s: %.0f ms, in %s, out %s', session.model, session.file,
                 elapsed * 1000.0, [d['shape'] for d in inputs],
                 [d['shape'] for d in out_descs])
    return ({'ok': True, 'outputs': out_descs},) + tuple(payloads)


class Handler(StreamRequestHandler):
    models_root = None
    gpu_provider = 'CUDAExecutionProvider'

    def handle(self):
        if not CONNECTION_SLOTS.acquire(blocking=False):
            self._try_send({'ok': False,
                            'error': f'server busy: connection limit is {MAX_CONNECTIONS}'})
            return
        try:
            self._serve()
        finally:
            CONNECTION_SLOTS.release()

    def _serve(self):
        self.connection.settimeout(CONNECTION_TIMEOUT)
        session = None
        try:
            while True:
                select.select([self.connection], [], [])
                msg, json_len = recv_frame(self.connection)
                if msg.get('v') != PROTOCOL_VERSION:
                    raise ProtocolError('unsupported protocol version')
                op = msg.get('op')
                if op == 'load':
                    session = load_session(msg, self.models_root, self.gpu_provider)
                    self._try_send(load_response(session))
                elif op == 'run':
                    if session is None:
                        raise ProtocolError('no model loaded on this connection')
                    inputs, outputs = validate_run_request(msg, json_len)
                    self._try_send(*run_inference(session, inputs, outputs, self.connection))
                else:
                    raise ProtocolError(f'unknown op {op!r}')
        except ProtocolError as exc:
            self._try_send({'ok': False, 'error': str(exc)})
        except OSError:
            pass  # client went away or timed out; the session frees here
        except Exception as exc:
            logging.warning('connection failed: %s: %s', type(exc).__name__, exc)
            self._try_send({'ok': False, 'error': f'{type(exc).__name__}: {exc}'})

    def _try_send(self, obj, *payloads):
        try:
            send_frame(self.connection, obj, payloads)
        except OSError:
            pass


class WorkerServer(ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = MAX_CONNECTIONS


def default_models_root():
    data_home = os.environ.get('XDG_DATA_HOME') or os.path.expanduser('~/.local/share')
    return os.path.join(data_home, 'darktable', 'models')


def main(argv=None):
    ap = argparse.ArgumentParser(
        description='Loopback GPU inference worker for remote darktable AI (protocol v1)')
    ap.add_argument('--bind', default='127.0.0.1',
                    help='numeric loopback address to bind (default 127.0.0.1)')
    ap.add_argument('--port', type=int, default=DEFAULT_PORT)
    ap.add_argument('--models-root', default=default_models_root(),
                    help='darktable models root (default $XDG_DATA_HOME/darktable/models)')
    ap.add_argument('--gpu-provider', choices=('cuda', 'coreml'), default='cuda')
    args = ap.parse_args(argv)

    logging.basicConfig(level=logging.INFO, stream=sys.stderr,
                        format='%(asctime)s %(levelname)s %(message)s')
    try:
        bind = ipaddress.ip_address(args.bind)
    except ValueError:
        sys.exit(f'remote_worker: --bind must be a numeric address, got {args.bind!r}')
    if not bind.is_loopback:
        sys.exit(f'remote_worker: --bind must be a loopback address, got {args.bind}')
    root = os.path.realpath(args.models_root)
    if not os.path.isdir(root):
        sys.exit(f'remote_worker: models root not found: {root}')

    Handler.models_root = root
    Handler.gpu_provider = {'cuda': 'CUDAExecutionProvider',
                            'coreml': 'CoreMLExecutionProvider'}[args.gpu_provider]
    if isinstance(bind, ipaddress.IPv6Address):
        WorkerServer.address_family = socket.AF_INET6
    server = WorkerServer((str(bind), args.port), Handler)
    logging.info('listening on %s:%d, models root %s',
                 bind, server.server_address[1], root)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
