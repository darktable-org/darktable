# darktable AI Backend — Developer Documentation

## Architecture Overview

The AI subsystem is built as a static library (`darktable_ai`) that wraps
ONNX Runtime's C API behind a backend-agnostic interface. It provides:

- **Model discovery** — scans directories for `config.json` manifests
- **Session management** — load/unload ONNX models with hardware acceleration
- **Tensor I/O** — type-safe inference with automatic Float32/Float16 conversion
- **Provider abstraction** — unified API for CPU, GPU, and NPU execution providers

### Source Files

| File | Role |
|------|------|
| `backend.h` | Public API: types, enums, function declarations |
| `backend_common.c` | Environment, model registry, provider string conversion |
| `backend_onnx.c` | ONNX Runtime C API wrapper, inference engine |
| `segmentation.h` | Segmentation (SAM) public API |
| `segmentation.c` | SAM encoder-decoder implementation |
| `CMakeLists.txt` | Build config, ONNX Runtime linkage, install rules |

Higher-level consumers live outside `src/ai/`:

| File | Role |
|------|------|
| `src/libs/denoise_ai.c` | Denoise lighttable module (tiled inference) |
| `src/develop/masks/object.c` | Object mask tool using segmentation API |
| `src/common/ai_models.c` | Model registry UI, download, extraction |
| `src/gui/preferences_ai.c` | AI preferences tab (provider selection, model management) |

---

## ONNX Runtime Integration

### Initialization

ONNX Runtime is initialized lazily via `g_once()` singletons:
- **`g_ort`** — `OrtApi` pointer (one per process)
- **`g_env`** — `OrtEnv` instance (one per process)

Both are created on first model load or provider probe and persist for the
lifetime of the process.

### Model Loading

```
dt_ai_load_model(env, model_id, model_file, provider)
  └─ dt_ai_load_model_ext(env, model_id, model_file, provider, opt_level, dim_overrides, n_overrides)
       └─ dt_ai_onnx_load_ext(model_dir, model_file, provider, opt_level, dim_overrides, n_overrides)
```

Loading a model:
1. Resolves `model_id` to a directory path via the environment's model registry
2. Creates `OrtSessionOptions` with intra-op parallelism (all cores)
3. Sets graph optimization level
4. Applies symbolic dimension overrides (for dynamic-shape models)
5. Calls `_enable_acceleration()` to attach the selected execution provider
6. Creates `OrtSession` from the `.onnx` file
7. Introspects all input/output names, types, and shapes
8. Detects dynamic output shapes (any dim <= 0) for ORT-allocated output mode

### Inference (`dt_ai_run`)

Callers provide `dt_ai_tensor_t` arrays for inputs and outputs:

```c
typedef struct dt_ai_tensor_t {
  void *data;         // Pointer to raw data buffer
  dt_ai_dtype_t type; // Data type of elements
  int64_t *shape;     // Array of dimensions
  int ndim;           // Number of dimensions
} dt_ai_tensor_t;
```

The runtime handles two special cases transparently:

- **Float16 auto-conversion**: If the caller provides Float32 data but the model
  expects Float16, the backend converts on-the-fly (and vice versa for outputs).
- **Dynamic output shapes**: If any output has symbolic dimensions, ORT allocates
  the output tensors internally. After inference, the backend copies data to the
  caller's buffer and updates the caller's shape array with actual dimensions.

### Graph Optimization Levels

| Level | Enum | Use Case |
|-------|------|----------|
| All | `DT_AI_OPT_ALL` | Default, fastest. Works for most models. |
| Basic | `DT_AI_OPT_BASIC` | Constant folding + redundancy elimination only. Required for SAM2 decoder (aggressive optimization breaks shape inference on dynamic dims). |
| Disabled | `DT_AI_OPT_DISABLED` | Reserved for future use. |

### Symbolic Dimension Overrides

Models with symbolic dimensions (e.g., SAM2 decoder's `num_labels`) can cause
ONNX Runtime to fail shape inference. Use `dt_ai_load_model_ext()` with
`dt_ai_dim_override_t` to bind concrete values:

```c
dt_ai_dim_override_t overrides[] = { { "num_labels", 1 } };
ctx = dt_ai_load_model_ext(env, id, file, provider,
                            DT_AI_OPT_BASIC, overrides, 1);
```

---

## Execution Providers

### Provider Table

| Provider | Enum | Config String | Platform |
|----------|------|---------------|----------|
| Auto | `DT_AI_PROVIDER_AUTO` | `auto` | All |
| CPU | `DT_AI_PROVIDER_CPU` | `CPU` | All |
| Apple CoreML | `DT_AI_PROVIDER_COREML` | `CoreML` | macOS |
| NVIDIA CUDA | `DT_AI_PROVIDER_CUDA` | `CUDA` | Linux, Windows |
| AMD MIGraphX | `DT_AI_PROVIDER_MIGRAPHX` | `MIGraphX` | Linux |
| Intel OpenVINO | `DT_AI_PROVIDER_OPENVINO` | `OpenVINO` | Linux, Windows, macOS (x86_64) |
| Windows DirectML | `DT_AI_PROVIDER_DIRECTML` | `DirectML` | Windows |

The `available` field in the provider descriptor is a compile-time platform
guard controlled by `#if` preprocessor directives. It determines which providers
are shown in the UI — runtime availability is checked separately.

### Auto-Detection Strategy

When `DT_AI_PROVIDER_AUTO` is selected, the backend tries platform-native
acceleration first and falls back gracefully:

- **macOS**: CoreML
- **Windows**: DirectML
- **Linux**: CUDA → MIGraphX → ROCm (legacy)

All providers fall back to CPU if the accelerator is unavailable.

### Runtime Provider Loading

Provider functions are loaded at runtime via dynamic symbol lookup
(`GModule`/`dlsym` on Unix, `GetProcAddress` on Windows) from the linked
ONNX Runtime shared library. This means:

- No compile-time dependency on provider-specific headers
- Providers are optional — missing symbols are handled gracefully
- The same binary works with CPU-only and GPU-enabled ORT builds

### MIGraphX / ROCm Fallback

ONNX Runtime is transitioning from ROCm to MIGraphX on AMD. When the MIGraphX
provider is selected (or reached via auto-detection on Linux), the backend tries
MIGraphX first, then falls back to ROCm for older ORT builds that only ship the
ROCm provider.

### Provider Probing

`dt_ai_probe_provider()` tests if a provider can be initialized at runtime
without loading a model. It creates temporary `OrtSessionOptions`, attempts to
attach the provider, and returns 1 (available) or 0 (unavailable). This is used
by the preferences UI to show a warning when a selected provider is not
available.

### ONNX Runtime Packages

| Platform | Package Source | Providers Included |
|----------|---------------|--------------------|
| macOS | GitHub releases (CPU) | CPU, CoreML (via Apple frameworks) |
| Linux | GitHub releases (CPU) | CPU only |
| Linux | System packages (vendor repos) | CPU + CUDA / MIGraphX / OpenVINO |
| Windows | NuGet (DirectML variant) | CPU, DirectML |

The build system (`cmake/modules/FindONNXRuntime.cmake`) auto-downloads the
appropriate package if ONNX Runtime is not found on the system. On Windows, it
downloads the DirectML NuGet package for vendor-agnostic GPU support (AMD,
Intel, NVIDIA via DirectX 12).

---

## Model Discovery

### Directory Layout

Models are discovered by scanning for `config.json` files in subdirectories of:

1. Custom paths passed to `dt_ai_env_init()` (semicolon-separated)
2. `<darktable_configdir>/models/` — darktable's own config directory (respects `--configdir`)
   - Linux: `~/.config/darktable/models/`
   - Windows: `%APPDATA%\darktable\models\`
   - macOS: `~/.config/darktable/models/`

First discovered model ID wins (duplicates are skipped).

Model downloads (`ai_models.c`) also extract to the configdir-based path, so
downloaded models are immediately discoverable.

### config.json Format

Each model directory must contain a `config.json`:

```json
{
  "id": "denoise-nind",
  "name": "denoise nind",
  "description": "UNet denoiser trained on NIND dataset",
  "task": "denoise",
  "backend": "onnx",
  "num_inputs": 1
}
```

| Field | Required | Default | Description |
|-------|----------|---------|-------------|
| `id` | Yes | — | Unique identifier (see naming convention below) |
| `name` | Yes | — | Display name shown in UI |
| `description` | No | `""` | Short description |
| `task` | No | `"general"` | Task type: `"denoise"`, `"mask"` |
| `backend` | No | `"onnx"` | Backend type (only `"onnx"` supported) |
| `num_inputs` | No | `1` | Number of model inputs (used by denoise for multi-input models) |

### Model ID Naming Convention

Model IDs follow the pattern `<task>-<model>[-<size>]`:

| ID | Task | Description |
|----|------|-------------|
| `denoise-nind` | `denoise` | UNet denoiser (NIND dataset) |
| `mask-object-sam21-small` | `mask` | SAM 2.1 Hiera Small for object masking |
| `mask-light-hq-sam` | `mask` | SAM-HQ lightweight variant |

Rules:
- Use lowercase, hyphen-separated
- First component is the task type (`denoise`, `mask`)
- For masks, the second component is the subtask (`object`, `light`)
- Append size suffix when multiple sizes exist (`small`, `base`, `large`)

---

## Task: Denoise

**Module**: `src/libs/denoise_ai.c`
**Task type**: `"denoise"`

### Model Requirements

#### Single-Input Denoise Models (NAFNet, UNet, etc.)

| Tensor | Name | Shape | Type | Description |
|--------|------|-------|------|-------------|
| Input 0 | `input` | `[1, 3, H, W]` | float32 | RGB image, NCHW layout, sRGB color space |
| Output 0 | `output` | `[1, 3, H, W]` | float32 | Denoised RGB image, same layout |

- H and W are dynamic (determined by tile size at runtime)
- Input and output spatial dimensions must match

#### Multi-Input Denoise Models (FFDNet)

| Tensor | Name | Shape | Type | Description |
|--------|------|-------|------|-------------|
| Input 0 | `input` | `[1, 3, H, W]` | float32 | RGB image, NCHW layout, sRGB color space |
| Input 1 | `sigma` | `[1, 1, H, W]` | float32 | Noise level map, values = sigma / 255.0 |
| Output 0 | `output` | `[1, 3, H, W]` | float32 | Denoised RGB image, same layout |

Set `"num_inputs": 2` in `config.json` for multi-input models.

### Color Space

Models operate in sRGB. The denoise module converts:
- **Before inference**: linear RGB → sRGB (IEC 61966-2-1 transfer function)
- **After inference**: sRGB → linear RGB

### Tiling Strategy

Large images are processed in overlapping tiles to fit within memory:

- **Tile sizes** (tried in order): 2048, 1536, 1024, 768, 512, 384, 256
- **Overlap**: 64 pixels on each edge
- **Memory budget**: 1/4 of available darktable memory
- **Border handling**: mirror padding at image edges

The output is streamed to TIFF scanlines — no full-resolution buffer is needed.

---

## Task: Mask (Object Segmentation)

**Module**: `src/ai/segmentation.c`
**API**: `src/ai/segmentation.h`
**Task type**: `"mask"`

### Supported Models

All SAM variants (SAM, SAM-HQ, SAM2/2.1) are exported to a common interface
via conversion scripts in the
[darktable-ai](https://github.com/andriiryzhkov/darktable-ai) repository.
The model directory must contain both `encoder.onnx` and `decoder.onnx`.

### Encoder Requirements

| Tensor | Shape | Type | Description |
|--------|-------|------|-------------|
| Input 0 | `[1, 3, 1024, 1024]` | float32 | Preprocessed image (CHW, ImageNet-normalized) |

**Preprocessing** (applied by `segmentation.c`):
1. Resize longest side to 1024 px (bilinear), preserve aspect ratio
2. Zero-pad the shorter side to reach 1024x1024
3. Normalize each channel: `(pixel - mean) / std`
   - Mean: `[123.675, 116.28, 103.53]`
   - Std: `[58.395, 57.12, 57.375]`
4. Convert HWC → CHW layout

| Output | Typical Shape | Description |
|--------|---------------|-------------|
| 0 | `[1, 256, 64, 64]` | Image embeddings |
| 1 | `[1, 32, 256, 256]` | High-resolution feature (skip connection) |
| 2 (SAM2 only) | `[1, 64, 128, 128]` | Mid-resolution feature |

Exact shapes vary by model variant. The encoder's outputs are passed to the
decoder, reordered by name matching if necessary (SAM2 encoder may output in a
different order than the decoder expects).

### Decoder Requirements

All model families use the same decoder interface. The number of encoder
outputs varies (2 for SAM-HQ, 3 for SAM2), but the prompt inputs and outputs
are identical.

#### Inputs

| Index | Name | Shape | Type | Description |
|-------|------|-------|------|-------------|
| 0..E | (encoder outputs) | (varies) | float32 | Passed through from encoder |
| E+1 | `point_coords` | `[1, N+1, 2]` | float32 | Point coordinates (N prompts + 1 padding) |
| E+2 | `point_labels` | `[1, N+1]` | float32 | 1=foreground, 0=background, -1=padding |
| E+3 | `mask_input` | `[1, 1, 256, 256]` | float32 | Low-res mask from previous iteration |
| E+4 | `has_mask_input` | `[1]` | float32 | 0.0 (first decode) or 1.0 (iterative refinement) |

Where E = number of encoder outputs. No `orig_im_size` input — resizing to
original image dimensions is done by darktable at runtime.

#### Outputs

| Index | Name | Shape | Description |
|-------|------|-------|-------------|
| 0 | `masks` | `[1, M, 1024, 1024]` | Mask logits (pre-sigmoid) at fixed resolution |
| 1 | `iou_predictions` | `[1, M]` | Predicted IoU score per mask |
| 2 | `low_res_masks` | `[1, M, 256, 256]` | Low-res masks for iterative refinement |

Where M = number of mask candidates:

- **SAM-HQ** (hq-token-only): M=1
- **SAM2**: M=3 (multi-mask)
- **SAM2 HQ**: M=4 (3 SAM + 1 HQ)

All spatial dimensions must be concrete (no symbolic dims). The `masks` output
must include `F.interpolate` upsampling from 256 to 1024 in the exported graph.

### Mask Post-Processing

1. Select mask with highest IoU score
2. Crop out the zero-padded region
3. Bilinear resize to original image dimensions
4. Apply sigmoid: `mask = 1 / (1 + exp(-logits))`
5. Output values in `[0, 1]` range

### Iterative Refinement

The decoder supports iterative mask refinement:
- **First decode**: `has_mask_input = 0.0`, decoder ignores `mask_input`
- **Subsequent decodes**: previous low-res mask is fed back with `has_mask_input = 1.0`
- `dt_seg_reset_prev_mask()` clears the cached mask without clearing image embeddings

### Encoding Cache

The encoder output is cached in `dt_seg_context_t`. Multiple decode calls
(different point prompts) reuse the same encoding. Call
`dt_seg_reset_encoding()` when the image changes.

### config.json for Mask Models

```json
{
  "id": "mask-object-sam21-small",
  "name": "sam2.1 hiera small",
  "description": "Segment Anything 2.1 (Hiera Small) for masking",
  "task": "mask",
  "backend": "onnx"
}
```

The model directory must contain:

```
mask-object-sam21-small/
  config.json
  encoder.onnx
  decoder.onnx
```

---

## Exporting Models for darktable

### Denoise Models

Export to ONNX with dynamic batch and spatial dimensions:

```python
torch.onnx.export(model, dummy_input, "model.onnx",
                  input_names=["input"],
                  output_names=["output"],
                  dynamic_axes={
                      "input": {2: "height", 3: "width"},
                      "output": {2: "height", 3: "width"}
                  })
```

### SAM / SAM-HQ / SAM2 Models

Export encoder and decoder as separate ONNX files (`encoder.onnx`, `decoder.onnx`).
Conversion scripts are maintained in the
[darktable-ai](https://github.com/andriiryzhkov/darktable-ai) repository.

All decoders must follow the interface described above:

- No `orig_im_size` input
- `masks` output at fixed 1024x1024 (include `F.interpolate` in the graph)
- `low_res_masks` output at 256x256 (for iterative refinement)
- All spatial dimensions concrete (no symbolic dims like `num_labels`)
- Only `num_points` may be dynamic (for variable number of point prompts)

---

## Adding a New Provider

1. Add enum value to `dt_ai_provider_t` in `backend.h` (before `DT_AI_PROVIDER_COUNT`)
2. Add entry to `dt_ai_providers[]` table with config string, display name, and platform guard
3. Add `case` in `_enable_acceleration()` in `backend_onnx.c` with the ORT append function symbol
4. Add `case` in `dt_ai_probe_provider()` in `backend_onnx.c`
5. The `_Static_assert` ensures the table stays in sync with the enum

---

## Adding a New Task

1. Create a new source file (e.g., `src/ai/newtask.c` + `newtask.h`)
2. Use `dt_ai_load_model()` / `dt_ai_load_model_ext()` to load models
3. Use `dt_ai_run()` for inference with `dt_ai_tensor_t` arrays
4. Set `"task": "newtask"` in the model's `config.json`
5. Add the new files to `src/ai/CMakeLists.txt`

---

## AI Payload Acceptance Contract

This contract defines how darktable accepts generated persistence artifacts (`.xmp` and `.dtstyle`) and what output is guaranteed when payload content is imperfect.

### XMP Sidecar (`.xmp`)

| Payload state | Source-backed branch | Result | Behavior | Evidence in code |
|---|---|---|---|---|
| `write_sidecar_files` missing/unknown | default in `dt_image_get_xmp_mode()` | `ALWAYS` (for writes) | Missing/invalid config is normalized to `on import` | `src/common/image.c:351-377` |
| `write_sidecar_files = "on import"` | `DT_WRITE_XMP_ALWAYS` | Accept write target | `dt_image_write_sidecar_file()` always calls `dt_exif_xmp_write()` | `src/common/image.c:2993-2997` |
| `write_sidecar_files = "after edit"` + altered data OR user tags | `DT_WRITE_XMP_LAZY` + `_any_altered_data()` | Attempt write | `src/common/image.c:2993-2995` |
| `write_sidecar_files = "after edit"` + no altered data/user tags | `DT_WRITE_XMP_LAZY` | Existing sidecar is deleted | `src/common/image.c:2998-3004` |
| `write_sidecar_files = "never"` | `DT_WRITE_XMP_NEVER` | No sidecar write/delete action | `src/common/image.c:2992-3005` |
| write path success | `dt_exif_xmp_write()` | success | Returns `FALSE` on success | `src/common/exif.cc:6008-6110` |
| write path failure | file cannot be opened, EXIF encode exception | reject write attempt | Returns `TRUE` (error) | `src/common/exif.cc:6093-6118` |
| no change vs existing sidecar | sidecar hash equals computed old+new hash | skip rewrite | Still updates DB timestamp as success | `src/common/exif.cc:6074-6110` |

Notes:
- Darktable writes sidecars as `<?xml version="1.0" encoding="UTF-8"?>` followed by the compact packet.
- `dt_exif_xmp_write()` writes sidecars as version `DT_XMP_EXIF_VERSION` (`5`) through `_exif_xmp_read_data`.

### XMP Import and History Semantics

| Payload field / shape | Source-backed branch | Deterministic outcome | Evidence in code |
|---|---|---|---|
| `Xmp.darktable.xmp_version > 5` | `dt_exif_xmp_read()` | Hard reject (`TRUE`) | `src/common/exif.cc:4206-4214` |
| `xmp_version = 5` and history entry keys complete | `_read_history_v2()` | Full history + masks imported | `src/common/exif.cc:3528-3682`, `4301-4399`, `4400-4538` |
| `xmp_version = 4/3/2` and history index is malformed (`[a/b]` parse failure) | `_read_history_v2()` | History parsing aborts to empty list (`history_entries = NULL`); DB import continues with empty history unless later DB steps fail | `src/common/exif.cc:3550-3571`, `4301-4538` |
| `xmp_version = 4/3/2` with a missing required field (`operation`/`modversion`/`params`) on any history entry | `_read_history_v2()` final sanity check | Whole history list is discarded (`history_entries = NULL`), then import proceeds without history unless DB write fails | `src/common/exif.cc:3669-3678`, `4301-4538` |
| `xmp_version = 1` / `0` | legacy path `_read_history_v1()` + possible fallback | Legacy history parser used | `src/common/exif.cc:4192-4205` |
| `xmp_version` missing | non-versioned XMP path | Metadata decode still runs, and `DT_IMAGE_NO_LEGACY_PRESETS` is set | `src/common/exif.cc:4048-4092`, `4085-4088` |
| `Xmp.darktable.history` present with malformed hex params | `dt_exif_xmp_decode()` | Blob decode returns `NULL`, DB bind can write NULL and import may still succeed | `src/common/exif.cc:3230-3256`, `3630-3660`, `4332-4351` |
| `xmp_version < 5`, `rawprepare` exists, no `highlights` entry | `_read_history_v2()` compat shim | synthetic highlights module added to history | `src/common/exif.cc:4233-4297` |

### DTStyle (`.dtstyle`)

| Payload state | Source-backed branch | Deterministic outcome | Evidence in code |
|---|---|---|---|
| XML not well formed | `g_markup_parse_context_parse()`/`end_parse()` | Import aborts with no DB write | `src/common/styles.c:1647-1671` |
| Empty `<name>` text | name text handler | Name defaults to `imported-style` | `src/common/styles.c:1477-1487` |
| Duplicate style name | `dt_styles_create_style_header()` | Insert is skipped; no item insertion | `src/common/styles.c:247-257`, `1603-1610` |
| Missing `<plugin>` numeric fields (`module`, `num`, `enabled`, etc.) | `dt_style_start/tag/text handlers` with `atoi()` defaults | Stored as `0` (or existing defaults) and style is still imported | `src/common/styles.c:1456-1544` |
| Plugin payload text not in expected encoding | `dt_exif_xmp_decode()` in `dt_style_plugin_save()` | Decodes to `NULL`; insert uses NULL blob payloads | `src/common/styles.c:1575-1597` |
| Missing style root schema checks (only permissive XML parser path) | parser + save path | No schema/version enforcement before DB import | `src/common/styles.c:1623-1679` |

### Validation checklist for AI output

1. **Write-side contract check (for generated `.xmp`)**
   - Confirm payload includes `Xmp.darktable.xmp_version` as integer `2..5` (prefer `5`).
   - Confirm each relevant `history` entry (v2+) has `darktable:operation`, `darktable:modversion`, and `darktable:params`.
   - Confirm encoded payloads are either `gzNN...` base64+zlib or lowercase hex (`[0-9a-f]`) and decode successfully with `dt_exif_xmp_decode` semantics.
2. **Import-path behavior check**
   - Feed the file through XMP import and verify `dt_exif_xmp_read()` returns success (`FALSE`).
   - If it returns error (`TRUE`), treat payload as hard-reject and inspect reason:
     - unsupported `xmp_version > 5`, or malformed file/Exif exception path.
3. **Style payload check (`.dtstyle`)**
   - Validate XML well-formedness before import.
   - Verify root element is `darktable_style` and plugin blocks are present if expected.
   - Validate numeric nodes can be parsed (`atoi` equivalent) and defaulting behavior is acceptable.
   - Decode all blob fields with dt-style decode rules and check for NULL; NULL is accepted but loses params.
4. **Partial acceptance expectations**
   - `.xmp`: missing history entries/fields can still import image metadata with reduced edit history (partial success).
   - `.dtstyle`: malformed/partial plugin fields can import with defaulted/empty values; only parse failures, duplicate name, or duplicate style header prevents insertion.
5. **Version drift check**
   - `dt_exif_xmp_write()` outputs `xmp_version = 5` and writes `<?xml version="1.0" encoding="UTF-8"?>` before payload. Reject generators that emit a different darktable version unless explicitly intended for compatibility.
