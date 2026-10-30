# Remote AI worker (wire protocol v1)

Offloads ONNX inference from the darktable editor to a CUDA or Apple GPU host. The editor
keeps all other image processing; only denoise/upscale/mask inference runs
remotely. The worker is loopback-only and is reached over an SSH tunnel, so
SSH provides authentication and encryption and the worker does neither.

## Scope

- `tools/ai/remote_worker.py` — the worker (this feature).
- `tools/ai/test_remote_worker.py` — trust-boundary and conversion tests.
- `tools/ai/remote_smoke.py` — load+run smoke probe against a live worker.
The editor selects the worker through `DT_AI_REMOTE`. Every AI load routes
to the worker, including explicit CPU requests: the CPU preference is
forwarded (`cpu:true`) and the worker runs its CPU execution provider —
SAM/segmentation decoders included. Ordinary processing and file storage
also remain local.

## Requirements (GPU host)

- Linux x86_64 with an NVIDIA GPU and driver.
- `python3` with `numpy` and a CUDA build of `onnxruntime` >= 1.21
  (`pip install onnxruntime-gpu`, or the library layout from
  `tools/ai/README.md`; the worker only needs the Python wheel — it calls
  `ort.preload_dlls()` before creating the CUDA session and applies load
  overrides with `SessionOptions.add_free_dimension_override_by_name`).
- The same darktable model packages the editor uses, under a models root.

Alternatively, use Apple Silicon macOS with `numpy` and the standard
`onnxruntime` wheel. Select `--gpu-provider coreml`. This uses Core ML
MLProgram with `CPUAndGPU` and disables low-precision GPU accumulation.
Unsupported Core ML graph partitions fail rather than falling back to ONNX CPU.
Models must be tested on each backend; CUDA support does not imply Core ML support.

## Models root

Layout is identical to what darktable scans
(`$XDG_DATA_HOME/darktable/models` by default):

```
<models-root>/<model-dir>/config.json   # "id" must equal <model-dir>
<models-root>/<model-dir>/<file>.onnx
```

Provisioning splits along the same line the editor already uses: the
`config.json` manifests are small metadata the editor keeps locally for
enumeration, settings and preprocessing parameters (input sizes,
policies), while the multi-megabyte `.onnx` weights are only needed on
the worker host. A metadata-only local model dir (config.json without
weights) is sufficient for the editor when `DT_AI_REMOTE` is configured;
the worker's root, however, must contain both manifest and weights.

- Requests name the model directory and `.onnx` file as bare names; the
  worker resolves them inside the root with symlinks resolved, so neither
  may point outside it.
- `config.json` `id` must match the requested directory name.
- Per-model `cpu_only` metadata is honored exactly like
  `_provider_cpu_only()` in `src/ai/backend_common.c`: a flat list applies
  to the whole package, an object is keyed by the `.onnx` filename stem,
  matching is case-insensitive. A `cuda` or `coreml` entry refuses a load
  on that accelerator. An explicit `cpu:true` request still selects CPU.

## Running

```sh
python3 tools/ai/remote_worker.py --models-root ~/darktable-models
# binds 127.0.0.1:8794; override with --bind/--port (loopback numeric only)

# Apple Silicon worker (same protocol and models directory)
python3 tools/ai/remote_worker.py --gpu-provider coreml --models-root ~/darktable-models
```

On the editor host, point darktable at the tunnel:

```sh
ssh -N -L 8794:127.0.0.1:8794 gpu-host &
DT_AI_REMOTE=127.0.0.1:8794 darktable
```

`DT_AI_REMOTE` unset keeps existing local behavior. A configured but
unreachable remote fails the load loudly; it never falls back to CPU
locally, and CPU-requested loads forward their preference (`cpu:true`)
so they run on the worker's CPU execution provider.

## Wire protocol v1

One TCP connection per loaded model context, persistent, with one request
in flight. Frame: network-endian `uint32` length, then a UTF-8 JSON object
(max 64 KiB); binary tensor payloads follow the JSON in descriptor order,
little-endian.

- `{"v":1,"op":"load","model":...,"file":...,"opt":0..2,"cpu":bool,
  "overrides":[{"name":...,"value":...}]}`
  → `{"ok":true,"inputs":[{name,type,shape}],"outputs":[...],
  "provider":"CUDAExecutionProvider"}`. `opt` mirrors `DT_AI_OPT`
  (0=all, 1=basic, 2=disabled) or is `-1`: the worker then resolves the
  manifest default exactly like the editor's local backend —
  `ort_optimization_provider` (keyed by provider name) first, then
  `ort_optimization`, both as flat string or stem-keyed object
  ("all"/"basic"/"disabled"/"none", unknown → all). Explicit 0..2 is
  always a caller override and passes through untouched. `cpu` forwards
  the caller's explicit CPU preference (CPU provider request,
  segmentation decoder, CPU reloads); it is not a fallback. Dynamic
  metadata dims are `-1`.
- `{"v":1,"op":"run","inputs":[{type,shape,bytes}],
  "outputs":[{type,shape,bytes}]}` followed by the input bytes
  → `{"ok":true,"outputs":[{type,shape,bytes}]}` followed by the output
  bytes. `type` uses `dt_ai_dtype_t` values: float32=1, uint8=2, int8=3,
  int32=4, int64=5, float16=10.
- Static output descriptors declare a caller buffer: all dims positive and
  `bytes` exactly the shape product; the actual result must fit that
  capacity and its rank must not exceed the declared rank.
- Dynamic output descriptors (`bytes == 0`) request allocation instead:
  shape is a template that may contain `-1` dims (rank 0..8; rank 0 is a
  scalar). The
  worker runs the model and responds with the real concrete shape and
  exact byte count; the client allocates from that and owns the buffer.
  Actual results are bounded (each rank ≤ 8 and ≤ the declared rank, all
  output bytes plus JSON ≤ 512 MiB) before anything is serialized.
- The worker casts float32↔float16 to the model's I/O dtypes and nothing
  else — no implicit integer casts. Response `bytes` is always exact.
- Any error replies `{"ok":false,"error":...}` and closes the connection;
  there is no partial success.

Limits enforced before any allocation: 512 MiB per message, 16 tensors,
rank 0..8 (0 = scalar), positive integer dims, byte count must equal the
shape product (an empty shape is one element)
(inputs) or be 0 (dynamic outputs).
The 300-second timeout bounds partial requests, not time between edits. Models
remain loaded while their editor connection is open and release on disconnect.
At most four connections are accepted; GPU inference is serialized.

## Security scope

- The worker refuses non-loopback, non-numeric bind addresses.
- Model files are read from the models root by basename only; there is no
  upload path — the operator provisions models on the GPU host.
- Trusted-user only (loopback + SSH); it executes model files that already
  sit in its root, so anyone with loopback access is trusted with them.
- Logs contain provider, model name, duration and tensor shapes — never
  image data.

## Tests

```sh
python3 tools/ai/test_remote_worker.py
```

Covers bind guard, path traversal/symlink escapes, config id matching,
descriptor bounds, dtype cast rules, protocol framing errors and the
connection limit against the real worker process. Real CUDA and Core ML inference
on actual models is verified at deployment, not here.
