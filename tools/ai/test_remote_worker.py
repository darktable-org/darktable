#!/usr/bin/env python3
"""Trust-boundary and conversion tests for remote_worker.py (protocol v1).

Runs the real worker process over loopback sockets plus pure checks of its
validation and conversion logic. No GPU, model or onnxruntime needed: ORT is
only exercised by real inference, which the deployment owner verifies per
contract (no fake inference here).

Run: python3 tools/ai/test_remote_worker.py
"""

import json
import os
import queue
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import remote_worker as rw


def send_msg(sock, obj, payload=b''):
    data = json.dumps(obj).encode()
    sock.sendall(struct.pack('>I', len(data)) + data + payload)


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError('closed')
        buf += chunk
    return bytes(buf)


def recv_msg(sock):
    (n,) = struct.unpack('>I', recv_exact(sock, 4))
    return json.loads(recv_exact(sock, n))


def tensor(dtype=1, shape=(1, 3, 8, 8), **kw):
    n = rw.ELEMSIZE.get(dtype, 4)
    for x in shape:
        n *= x if isinstance(x, int) and not isinstance(x, bool) else 1
    d = {'type': dtype, 'shape': list(shape), 'bytes': n}
    d.update(kw)
    return d


def load_req(model='m1', file='m.onnx', **kw):
    req = {'v': 1, 'op': 'load', 'model': model, 'file': file,
           'opt': 0, 'cpu': False}
    req.update(kw)
    return req


def make_model(root, name, model_id=None, files=('m.onnx',), cpu_only=None):
    d = os.path.join(root, name)
    os.makedirs(d, exist_ok=True)
    cfg = {'id': model_id if model_id is not None else name, 'name': name}
    if cpu_only is not None:
        cfg['cpu_only'] = cpu_only
    with open(os.path.join(d, 'config.json'), 'w') as f:
        json.dump(cfg, f)
    for fn in files:
        with open(os.path.join(d, fn), 'wb') as f:
            f.write(b'\x00' * 16)
    return d


class LiveWorker:
    def __init__(self, root, extra=()):
        self.proc = subprocess.Popen(
            [sys.executable, rw.__file__, '--bind', '127.0.0.1', '--port', '0',
             '--models-root', root, *extra],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        lines = queue.Queue()
        threading.Thread(target=self._pump, args=(lines,), daemon=True).start()
        deadline = time.monotonic() + 15
        while True:
            line = lines.get(timeout=deadline - time.monotonic())
            m = re.search(r'listening on \S+:(\d+)', line)
            if m:
                self.port = int(m.group(1))
                return

    def _pump(self, lines):
        for line in self.proc.stderr:
            lines.put(line)

    def connect(self):
        return socket.create_connection(('127.0.0.1', self.port), timeout=10)

    def stop(self):
        self.proc.terminate()
        self.proc.wait(timeout=10)


class TestDescriptors(unittest.TestCase):
    def test_valid(self):
        inputs, outputs = rw.validate_run_request(
            {'inputs': [tensor()], 'outputs': [tensor(1, (1, 3, 16, 16))]}, 100)
        self.assertEqual(len(inputs), 1)
        self.assertEqual(len(outputs), 1)

    def test_bad_dtype(self):
        for bad in (0, 6, 11, True, '1', None):
            with self.assertRaises(rw.ProtocolError):
                rw.validate_run_request({'inputs': [tensor(dtype=bad)],
                                         'outputs': [tensor()]}, 0)

    def test_bad_rank(self):
        with self.assertRaises(rw.ProtocolError):
            rw.validate_run_request(
                {'inputs': [tensor(shape=tuple(range(9)))], 'outputs': [tensor()]}, 0)

    def test_scalar_rank_zero(self):
        # MoGe-style int64 scalars: empty shape, bytes = one element
        self.assertEqual(
            rw.validate_tensor_desc({'type': 5, 'shape': [], 'bytes': 8}, 'inputs'), 8)
        ins, outs = rw.validate_run_request(
            {'inputs': [{'type': 5, 'shape': [], 'bytes': 8}],
             'outputs': [{'type': 5, 'shape': [], 'bytes': 0}]}, 100)
        self.assertEqual(ins[0]['bytes'], 8)
        self.assertEqual(outs[0]['bytes'], 0)

    def test_bad_dims(self):
        for dim in (0, -1, '2', True, 1.5, None):
            with self.assertRaises(rw.ProtocolError):
                rw.validate_run_request(
                    {'inputs': [tensor(shape=(1, dim, 4))],
                     'outputs': [tensor()]}, 0)

    def test_bytes_mismatch(self):
        d = tensor()
        d['bytes'] += 1
        with self.assertRaises(rw.ProtocolError):
            rw.validate_run_request({'inputs': [d], 'outputs': [tensor()]}, 0)

    def test_shape_product_overflow_rejected(self):
        huge = tensor(shape=(2 ** 40, 2 ** 40), bytes=16)
        with self.assertRaises(rw.ProtocolError):
            rw.validate_run_request({'inputs': [huge], 'outputs': [tensor()]}, 0)

    def test_tensor_count_bounds(self):
        for count in (0, rw.MAX_TENSORS + 1):
            with self.assertRaises(rw.ProtocolError):
                rw.validate_run_request(
                    {'inputs': [tensor()] * count, 'outputs': [tensor()]}, 0)
        with self.assertRaises(rw.ProtocolError):
            rw.validate_run_request({'inputs': 'x', 'outputs': [tensor()]}, 0)

    def test_message_limit(self):
        half = tensor(shape=(1, 3, 8192, 8192))  # 512 MiB f32 -> over with JSON
        with self.assertRaises(rw.ProtocolError):
            rw.validate_run_request({'inputs': [half], 'outputs': [half]}, 0)


class TestModelPaths(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix='dt-worker-test-')
        self.addCleanup(shutil.rmtree, self.root, True)
        self.root = os.path.realpath(self.root)
        make_model(self.root, 'm1')
        make_model(self.root, 'idm', model_id='other')
        outside = tempfile.mkdtemp(prefix='dt-worker-outside-')
        self.addCleanup(shutil.rmtree, outside, True)
        make_model(outside, 'evil')
        with open(os.path.join(outside, 'secret.onnx'), 'wb') as f:
            f.write(b'x')
        os.symlink(os.path.join(outside, 'evil'),
                   os.path.join(self.root, 'link'))
        os.symlink(os.path.join(outside, 'secret.onnx'),
                   os.path.join(self.root, 'm1', 'linked.onnx'))

    def test_valid(self):
        path, cfg = rw.resolve_model(self.root, 'm1', 'm.onnx')
        self.assertTrue(path.startswith(os.path.join(self.root, 'm1')))
        self.assertEqual(cfg['id'], 'm1')

    def test_model_must_be_bare_name(self):
        for bad in ('../evil', 'a/b', '.', '..', '', None, 3):
            with self.assertRaises(rw.ProtocolError):
                rw.resolve_model(self.root, bad, 'm.onnx')

    def test_file_must_be_onnx_basename(self):
        for bad in ('../secret.onnx', 'sub/m.onnx', 'm.txt', '.onnx', '', None):
            with self.assertRaises(rw.ProtocolError):
                rw.resolve_model(self.root, 'm1', bad)

    def test_symlink_escape_rejected(self):
        with self.assertRaises(rw.ProtocolError):
            rw.resolve_model(self.root, 'link', 'm.onnx')
        with self.assertRaises(rw.ProtocolError):
            rw.resolve_model(self.root, 'm1', 'linked.onnx')

    def test_config_id_must_match(self):
        with self.assertRaises(rw.ProtocolError):
            rw.resolve_model(self.root, 'idm', 'm.onnx')


class TestProviderChoice(unittest.TestCase):
    def test_default_cuda(self):
        self.assertEqual(rw.choose_provider({}, 'm.onnx', False),
                         'CUDAExecutionProvider')

    def test_explicit_cpu(self):
        self.assertEqual(rw.choose_provider({}, 'm.onnx', True),
                         'CPUExecutionProvider')

    def test_cpu_only_flat_list(self):
        for entries in (['cuda'], ['CUDA'], ['CoreML', 'cuda']):
            with self.assertRaises(rw.ProtocolError):
                rw.choose_provider({'cpu_only': entries}, 'm.onnx', False)
        self.assertEqual(rw.choose_provider({'cpu_only': ['directml']},
                                           'm.onnx', False),
                         'CUDAExecutionProvider')

    def test_cpu_only_stem_keyed(self):
        cfg = {'cpu_only': {'m_bayer': ['cuda'], 'm_linear': []}}
        with self.assertRaises(rw.ProtocolError):
            rw.choose_provider(cfg, 'm_bayer.onnx', False)
        self.assertEqual(rw.choose_provider(cfg, 'm_linear.onnx', False),
                         'CUDAExecutionProvider')

    def test_cpu_request_overrides_metadata_block(self):
        self.assertEqual(
            rw.choose_provider({'cpu_only': ['cuda']}, 'm.onnx', True),
            'CPUExecutionProvider')

    def test_malformed_cpu_only_ignored(self):
        self.assertEqual(rw.choose_provider({'cpu_only': 'x'}, 'm.onnx', False),
                         'CUDAExecutionProvider')

    def test_coreml_respects_model_cpu_only_restrictions(self):
        for cfg in ({"cpu_only": ["coreml"]},
                    {"cpu_only": {"m": ["CoreML"]}}):
            with self.assertRaises(rw.ProtocolError):
                rw.choose_provider(cfg, "m.onnx", False, "CoreMLExecutionProvider")
        self.assertEqual(
            rw.choose_provider({"cpu_only": ["CUDA"]}, "m.onnx", False,
                               "CoreMLExecutionProvider"), "CoreMLExecutionProvider")

    def test_unknown_accelerator_is_rejected(self):
        with self.assertRaises(rw.ProtocolError):
            rw.choose_provider({}, "m.onnx", False, "BogusExecutionProvider")


class TestResolveOpt(unittest.TestCase):
    def test_explicit_levels_pass_through(self):
        for requested in (0, 1, 2):
            self.assertEqual(
                rw.resolve_opt({'ort_optimization': 'disabled'},
                               'm.onnx', 'CUDA', requested),
                requested)

    def test_default_without_manifest_is_all(self):
        self.assertEqual(rw.resolve_opt({}, 'm.onnx', 'CUDA', -1), 0)

    def test_default_top_level_string(self):
        self.assertEqual(
            rw.resolve_opt({'ort_optimization': 'basic'}, 'm.onnx', 'CUDA', -1), 1)

    def test_default_top_level_stem_keyed(self):
        cfg = {'ort_optimization': {'m_bayer': 'basic', 'm_linear': 'none'}}
        self.assertEqual(rw.resolve_opt(cfg, 'm_bayer.onnx', 'CUDA', -1), 1)
        self.assertEqual(rw.resolve_opt(cfg, 'm_linear.onnx', 'CUDA', -1), 2)
        # unlisted stem falls back to all
        self.assertEqual(rw.resolve_opt(cfg, 'm_other.onnx', 'CUDA', -1), 0)

    def test_default_provider_override_wins(self):
        cfg = {'ort_optimization': 'basic',
               'ort_optimization_provider': {'CUDA': 'disabled'}}
        self.assertEqual(rw.resolve_opt(cfg, 'm.onnx', 'CUDA', -1), 2)
        # other providers still see the top-level value
        self.assertEqual(rw.resolve_opt(cfg, 'm.onnx', 'COREML', -1), 1)

    def test_default_provider_match_without_value_falls_through(self):
        cfg = {'ort_optimization': 'basic',
               'ort_optimization_provider': {'CUDA': {'other_stem': 'disabled'}}}
        self.assertEqual(rw.resolve_opt(cfg, 'm.onnx', 'CUDA', -1), 1)

    def test_default_unknown_strings_fall_to_all(self):
        self.assertEqual(
            rw.resolve_opt({'ort_optimization': 'turbo'}, 'm.onnx', 'CUDA', -1), 0)

    def test_load_session_rejects_out_of_range_opt(self):
        # validated in load_session before anything touches the models root
        for bad in (3, -2, 'basic', True):
            with self.assertRaises(rw.ProtocolError):
                rw.load_session({'model': 'x', 'file': 'x.onnx', 'opt': bad}, None)


class TestConversions(unittest.TestCase):
    def test_elemsize_table_matches_numpy(self):
        for dt, name in rw.DTYPES.items():
            self.assertEqual(np.dtype(name).itemsize, rw.ELEMSIZE[dt])

    def test_float32_to_float16(self):
        arr = np.array([1.5, -2.25, 100.0], dtype='<f4')
        out = rw.convert(arr, 10)
        self.assertEqual(out.dtype, np.dtype('<f2'))
        np.testing.assert_array_equal(out.astype('<f4'), arr)

    def test_float16_to_float32(self):
        arr = np.array([0.5, -0.25], dtype='<f2')
        out = rw.convert(arr, 1)
        self.assertEqual(out.dtype, np.dtype('<f4'))
        np.testing.assert_array_equal(out, arr.astype('<f4'))

    def test_same_dtype_no_copy(self):
        arr = np.zeros((2, 2), dtype='<f4')
        self.assertIs(rw.convert(arr, 1), arr)

    def test_integer_casts_rejected(self):
        for arr, to in ((np.zeros(3, '<f4'), 4),   # f32 -> int32
                        (np.zeros(3, 'u1'), 1),    # uint8 -> f32
                        (np.zeros(3, '<i4'), 10),  # int32 -> f16
                        (np.zeros(3, '<f2'), 5),   # f16 -> int64
                        (np.zeros(3, '<i8'), 2)):  # int64 -> uint8
            with self.assertRaises(rw.ProtocolError):
                rw.convert(arr, to)

    def test_float16_roundtrip_is_little_endian(self):
        arr = rw.convert(np.array([1.0, 2.0], '<f4'), 10)
        raw = arr.tobytes()
        self.assertEqual(len(raw), 4)
        self.assertEqual(struct.unpack('<2e', raw), (1.0, 2.0))


class TestDynamicOutputs(unittest.TestCase):
    def req(self, inputs, outputs):
        return rw.validate_run_request({'inputs': inputs, 'outputs': outputs}, 0)

    def test_dynamic_output_accepted(self):
        inputs, outputs = self.req([tensor()],
                                   [{'type': 1, 'shape': [1, 3, -1, -1], 'bytes': 0}])
        self.assertEqual(outputs[0]['bytes'], 0)

    def test_dynamic_concrete_shape_accepted(self):
        self.req([tensor()], [{'type': 2, 'shape': [1, 64, 64], 'bytes': 0}])

    def test_inputs_stay_strict(self):
        for bad in ({'type': 1, 'shape': [1, -1], 'bytes': 0},
                    {'type': 1, 'shape': [1, 4], 'bytes': 0}):
            with self.assertRaises(rw.ProtocolError):
                self.req([bad], [tensor()])

    def test_nonzero_bytes_with_dynamic_dims_rejected(self):
        with self.assertRaises(rw.ProtocolError):
            self.req([tensor()], [{'type': 1, 'shape': [1, 3, -1, -1], 'bytes': 12}])

    def test_dynamic_bad_dims(self):
        for dim in (0, -2, 'x', True, 1.5, None):
            with self.assertRaises(rw.ProtocolError):
                self.req([tensor()], [{'type': 1, 'shape': [1, dim], 'bytes': 0}])

    def test_dynamic_bad_rank(self):
        # rank 0 is legal since scalar support; only >8 is invalid
        with self.assertRaises(rw.ProtocolError):
            self.req([tensor()], [{'type': 1, 'shape': list(range(9)), 'bytes': 0}])

    def test_negative_bytes_rejected(self):
        with self.assertRaises(rw.ProtocolError):
            self.req([tensor()], [{'type': 1, 'shape': [1, -1], 'bytes': -1}])

    def test_response_bound(self):
        rw.check_response_bounds(rw.MAX_MESSAGE - rw.MAX_JSON)
        with self.assertRaises(rw.ProtocolError):
            rw.check_response_bounds(rw.MAX_MESSAGE - rw.MAX_JSON + 1)


class TestIdleConnection(unittest.TestCase):
    def test_idle_wait_survives_but_partial_frame_times_out(self):
        previous = rw.CONNECTION_TIMEOUT
        rw.CONNECTION_TIMEOUT = 0.05
        server = rw.WorkerServer(("127.0.0.1", 0), rw.Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with socket.create_connection(server.server_address, timeout=2) as sock:
                time.sleep(0.15)
                send_msg(sock, {"v": 1, "op": "unknown"})
                self.assertIn("unknown op", recv_msg(sock)["error"])
            with socket.create_connection(server.server_address, timeout=2) as sock:
                sock.sendall(b"\x00")
                self.assertEqual(sock.recv(1), b"")
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
            rw.CONNECTION_TIMEOUT = previous


class TestLiveProtocol(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = tempfile.mkdtemp(prefix='dt-worker-live-')
        make_model(cls.root, 'm1')
        make_model(cls.root, 'idm', model_id='other')
        cls.worker = LiveWorker(cls.root)

    @classmethod
    def tearDownClass(cls):
        cls.worker.stop()
        shutil.rmtree(cls.root, True)

    def test_malformed_json_rejected_and_closes(self):
        sock = self.worker.connect()
        self.addCleanup(sock.close)
        sock.sendall(struct.pack('>I', 5) + b'nope{')
        reply = recv_msg(sock)
        self.assertFalse(reply['ok'])
        self.assertIn('JSON', reply['error'])
        self.assertEqual(sock.recv(1), b'')

    def test_oversized_json_rejected(self):
        sock = self.worker.connect()
        self.addCleanup(sock.close)
        sock.sendall(struct.pack('>I', rw.MAX_JSON + 1))
        reply = recv_msg(sock)
        self.assertFalse(reply['ok'])
        self.assertEqual(sock.recv(1), b'')

    def test_unknown_op_rejected(self):
        sock = self.worker.connect()
        self.addCleanup(sock.close)
        send_msg(sock, {'v': 1, 'op': 'eval'})
        reply = recv_msg(sock)
        self.assertFalse(reply['ok'])
        self.assertIn('unknown op', reply['error'])

    def test_wrong_version_rejected(self):
        sock = self.worker.connect()
        self.addCleanup(sock.close)
        send_msg(sock, load_req(v=2))
        reply = recv_msg(sock)
        self.assertFalse(reply['ok'])
        self.assertIn('version', reply['error'])

    def test_run_without_load_rejected(self):
        sock = self.worker.connect()
        self.addCleanup(sock.close)
        send_msg(sock, {'v': 1, 'op': 'run',
                        'inputs': [tensor()], 'outputs': [tensor()]})
        reply = recv_msg(sock)
        self.assertFalse(reply['ok'])
        self.assertIn('no model loaded', reply['error'])

    def test_load_path_boundaries_rejected_over_wire(self):
        cases = [('../evil', 'm.onnx', 'model must be a bare name'),
                 ('m1', '../secret.onnx', 'file must be a bare name'),
                 ('m1', 'sub/m.onnx', 'file must be a bare name'),
                 ('m1', 'm.txt', '*.onnx'),
                 ('nosuch', 'm.onnx', 'unknown model'),
                 ('idm', 'm.onnx', 'config.json id')]
        for model, file, expected in cases:
            with self.subTest(model=model, file=file):
                sock = self.worker.connect()
                self.addCleanup(sock.close)
                send_msg(sock, load_req(model=model, file=file))
                reply = recv_msg(sock)
                self.assertFalse(reply['ok'])
                self.assertIn(expected, reply['error'])
                self.assertEqual(sock.recv(1), b'')

    def test_connection_limit_enforced(self):
        idle = [self.worker.connect() for _ in range(rw.MAX_CONNECTIONS)]
        try:
            time.sleep(0.5)  # let handlers take their slots
            busy = self.worker.connect()
            self.addCleanup(busy.close)
            reply = recv_msg(busy)
            self.assertFalse(reply['ok'])
            self.assertIn('busy', reply['error'])
        finally:
            for sock in idle:
                sock.close()


class TestBindGuard(unittest.TestCase):
    def test_non_loopback_bind_refused(self):
        root = tempfile.mkdtemp(prefix='dt-worker-bind-')
        self.addCleanup(shutil.rmtree, root, True)
        for bind in ('0.0.0.0', '192.168.1.10', 'localhost'):
            with self.subTest(bind=bind):
                proc = subprocess.run(
                    [sys.executable, rw.__file__, '--bind', bind, '--port', '0',
                     '--models-root', root],
                    capture_output=True, text=True, timeout=30)
                self.assertNotEqual(proc.returncode, 0)
                self.assertTrue(proc.stderr.strip())
                self.assertIn(bind, proc.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
