# AI Subsystem Developer Guide

This guide covers building AI-powered features for darktable using the
ONNX Runtime backend.

---

## Architecture Overview

The AI subsystem has three layers:

```
src/ai/                          ONNX Runtime backend (darktable_ai static lib)
  backend.h                        public API: types, model loading, inference
  backend_common.c                 environment, model registry, provider resolution
  backend_onnx.c                   ONNX Runtime C API wrapper

src/common/ai/                   higher-level AI modules (compiled in lib_darktable)
  segmentation.c/.h                SAM/SegNext interactive masking
  restore.c/.h                     generic env/ctx lifecycle + model loaders
  restore_common.h                 private struct defs shared by restore_*
  restore_rgb.c/.h                 RGB-path denoise + upscale (tiled inference,
                                   shadow boost, DWT detail recovery)
  restore_raw_bayer.c/.h           RawNIND Bayer denoise (batch + piped preview)
  restore_raw_linear.c/.h          RawNIND linear/X-Trans denoise

src/common/ai_models.c/.h       model registry, download, preferences integration
src/gui/preferences_ai.c        AI preferences tab
```

**Key separation**: `src/ai/` is a self-contained static library with no
darktable core dependencies (only GLib + ONNX Runtime). `src/common/ai/`
bridges the AI backend with darktable core APIs (DWT, image buffers,
config, etc.) and is compiled conditionally with `USE_AI=ON`.

### Build Flags

| Flag | Default | Effect |
|------|---------|--------|
| `USE_AI` | OFF | enable AI subsystem |
| `USE_AI_DOWNLOAD` | ON (if `USE_AI`) | enable model downloading from GitHub |

When `USE_AI=ON`, the preprocessor defines `HAVE_AI`. When download is
enabled, `HAVE_AI_DOWNLOAD` is also defined.

### Runtime Enable/Disable

AI features are disabled by default in preferences. When disabled:
- no ONNX Runtime is loaded
- no model directories are created or scanned
- all `dt_ai_env_init()` / `dt_ai_load_model()` calls return NULL
- AI-dependent modules hide themselves via `reload_defaults()`

Users enable AI in preferences -> AI -> "enable AI features". The
`dt_ai_models_init_lazy()` function handles deferred initialization
when AI is re-enabled at runtime without restart.

---

## ONNX Runtime Integration

### Initialization

ONNX Runtime is initialized lazily via `g_once()` singletons:
- **`g_ort`** - `OrtApi` pointer (one per process)
- **`g_env`** - `OrtEnv` instance (one per process)

Both are created on first model load or provider probe and persist for
the lifetime of the process.

On Linux, ONNX Runtime is always lazy-loaded via `g_module_open()` to
prevent GPU provider libraries (MIGraphX/ROCm) from initializing at
process startup - which can `abort()` on unsupported GPUs before
darktable has a chance to check preferences.

### Model Loading

```
dt_ai_load_model(env, model_id, model_file, provider)
  -> dt_ai_load_model_ext(env, model_id, model_file, provider,
                          opt_level, dim_overrides, n_overrides)
       -> dt_ai_onnx_load_ext(model_dir, model_file, provider,
                              opt_level, dim_overrides, n_overrides)
```

Loading a model:
1. Resolves `model_id` to a directory path via the environment's model
   registry
2. Creates `OrtSessionOptions` with intra-op parallelism (all cores)
3. Sets graph optimization level
4. Applies symbolic dimension overrides (for dynamic-shape models)
5. Calls `_enable_acceleration()` to attach the selected execution
   provider
6. Creates `OrtSession` from the `.onnx` file
7. Introspects all input/output names, types, and shapes
8. Detects dynamic output shapes (any dim <= 0) for ORT-allocated
   output mode

### Inference (`dt_ai_run`)

Callers provide `dt_ai_tensor_t` arrays for inputs and outputs:

```c
typedef struct dt_ai_tensor_t {
  void *data;         // pointer to raw data buffer
  dt_ai_dtype_t type; // data type of elements
  int64_t *shape;     // array of dimensions
  int ndim;           // number of dimensions
} dt_ai_tensor_t;
```

The runtime handles two special cases transparently:

- **Float16 auto-conversion**: if the caller provides Float32 data but
  the model expects Float16, the backend converts on-the-fly (and vice
  versa for outputs)
- **Dynamic output shapes**: if any output has symbolic dimensions, ORT
  allocates the output tensors internally. After inference, the backend
  copies data to the caller's buffer and updates the caller's shape
  array with actual dimensions

### Graph Optimization Levels

| Level | Enum | Use Case |
|-------|------|----------|
| All | `DT_AI_OPT_ALL` | default, fastest. works for most models |
| Basic | `DT_AI_OPT_BASIC` | constant folding + redundancy elimination only. Required for SAM2 decoder (aggressive optimization breaks shape inference on dynamic dims) |
| Disabled | `DT_AI_OPT_DISABLED` | reserved for future use |

### Symbolic Dimension Overrides

Models with symbolic dimensions (e.g., SAM2 decoder's `num_labels`) can
cause ONNX Runtime to fail shape inference. Use
`dt_ai_load_model_ext()` with `dt_ai_dim_override_t` to bind concrete
values:

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
| Auto | `DT_AI_PROVIDER_AUTO` | `auto` | all |
| CPU | `DT_AI_PROVIDER_CPU` | `CPU` | all |
| Apple CoreML | `DT_AI_PROVIDER_COREML` | `CoreML` | macOS |
| NVIDIA CUDA | `DT_AI_PROVIDER_CUDA` | `CUDA` | Linux, Windows |
| AMD MIGraphX | `DT_AI_PROVIDER_MIGRAPHX` | `MIGraphX` | Linux |
| Intel OpenVINO | `DT_AI_PROVIDER_OPENVINO` | `OpenVINO` | Linux, Windows, macOS (x86_64) |
| Windows DirectML | `DT_AI_PROVIDER_DIRECTML` | `DirectML` | Windows |

In addition, `DT_AI_PROVIDER_CONFIGURED` (`#define`, value -1) is a
sentinel for `dt_ai_load_model()` / `dt_ai_load_model_ext()`: it reads
the user's provider preference from `darktablerc` at call time. It is
not a real provider and must never be stored in config or the provider
table. Consumers that want to respect the user's setting (e.g. restore,
segmentation) pass `CONFIGURED`; consumers that need a specific EP
(e.g. the decoder forced to CPU) pass the EP directly.

The `available` field in the provider descriptor is a compile-time
platform guard controlled by `#if` preprocessor directives. It
determines which providers are shown in the UI -- runtime availability
is checked separately.

### Auto-Detection Strategy

When `DT_AI_PROVIDER_AUTO` is selected (either by the user in
preferences or resolved from `CONFIGURED`), the backend tries
platform-native acceleration first and falls back gracefully:

- **macOS**: CoreML -> CPU
- **Windows**: DirectML -> CPU
- **Linux**: CUDA -> MIGraphX -> ROCm (legacy) -> CPU

### Runtime Provider Loading

Provider functions are loaded at runtime via dynamic symbol lookup
(`GModule`/`dlsym` on Unix, `GetProcAddress` on Windows) from the
linked ONNX Runtime shared library. This means:

- no compile-time dependency on provider-specific headers
- providers are optional -- missing symbols are handled gracefully
- the same binary works with CPU-only and GPU-enabled ORT builds

### Provider Probing

`dt_ai_probe_provider()` tests if a provider can be initialized at
runtime without loading a model. It creates temporary
`OrtSessionOptions`, attempts to attach the provider, and returns 1
(available) or 0 (unavailable). Used by the preferences UI to show a
warning when a selected provider is not available.

### ONNX Runtime Packages

| Platform | Package Source | Providers Included |
|----------|---------------|--------------------|
| macOS | GitHub releases (CPU) | CPU, CoreML (via Apple frameworks) |
| Linux (x86_64) | GitHub releases (GPU) | CPU, CUDA, TensorRT |
| Linux (aarch64) | GitHub releases (CPU) | CPU only |
| Linux | System packages (distro) | CPU only (no GPU providers in Debian/Ubuntu/Fedora packages) |
| Windows | NuGet (DirectML variant) | CPU, DirectML |

The build system (`cmake/modules/FindONNXRuntime.cmake`) auto-downloads
the appropriate package if ONNX Runtime is not found on the system. On
Linux x86_64, the GPU variant is downloaded by default to enable CUDA
acceleration. System packages (e.g. `libonnxruntime-dev` on
Debian/Ubuntu) are CPU-only and do not include GPU providers.

---

## Model Discovery and Registry

### Directory Layout

Models are discovered by scanning for `config.json` files in
subdirectories of:

1. Custom paths passed to `dt_ai_env_init()` (semicolon-separated)
2. `<user_data_dir>/darktable/models/`
   - Linux: `~/.local/share/darktable/models/`
   - Windows: `%APPDATA%\darktable\models\`
   - macOS: `~/.local/share/darktable/models/`

First discovered model ID wins (duplicates are skipped). Model
downloads (`ai_models.c`) extract to the same path, so downloaded
models are immediately discoverable.

### config.json Format

Each model directory must contain a `config.json`:

```json
{
  "id": "mask-object-segnext-b2hq",
  "name": "mask segnext vitb-sax2 hq",
  "description": "SegNext ViT-B SAx2 HQ fine-tuned for interactive masking",
  "task": "mask",
  "arch": "segnext",
  "backend": "onnx"
}
```

| Field | Required | Default | Description |
|-------|----------|---------|-------------|
| `id` | yes | -- | unique identifier |
| `name` | yes | -- | display name shown in UI |
| `description` | no | `""` | short description |
| `task` | no | `"general"` | task type: `"denoise"`, `"upscale"`, `"mask"`, `"depth"` |
| `backend` | no | `"onnx"` | backend type (only `"onnx"` supported) |
| `arch` | no | `""` | model architecture (e.g. `"sam2"`, `"segnext"`) |
| `num_inputs` | no | `1` | number of model inputs |

### Model ID Naming Convention

Pattern: `<task>-<model>[-<size>]`

- use lowercase, hyphen-separated
- first component is the task type (`denoise`, `mask`, `upscale`, `depth`)
- for masks, second component is the subtask (`object`)
- append size suffix when multiple sizes exist (`small`, `base`, `large`)

Examples: `denoise-nind`, `mask-object-sam21-small`, `upscale-bsrgan`,
`mask-depth-da2-small`

---

## Model Repository (darktable-ai)

Models available for download in darktable are hosted as release assets
in the [darktable-ai](https://github.com/darktable-org/darktable-ai)
repository. This repository also contains:

- **conversion scripts** -- export PyTorch models to ONNX in the format
  darktable expects (correct I/O names, dynamic axes, interpolation
  baked into the graph, etc.)
- **packaging scripts** -- bundle `config.json` + ONNX files into
  `.dtmodel` archives (zip) for distribution
- **model metadata** -- the source `ai_models.json` that lists all
  available models with their task, description, and asset filename

The download flow:
1. darktable reads `data/ai_models.json` (bundled with the build) to
   know which models exist
2. user clicks "download" in AI preferences
3. `ai_models.c` fetches the `.dtmodel` asset from the latest
   darktable-ai release via the GitHub API
4. the archive is extracted to `~/.local/share/darktable/models/`
5. the model is immediately discoverable by `dt_ai_env_init()`

The repository is configured in `darktablerc`:
```
plugins/ai/repository=darktable-org/darktable-ai
```

### Adding a New Model to the Repository

1. export the model to ONNX using the conversion scripts (or write a
   new one following existing examples)
2. create `config.json` with the correct `id`, `task`, `arch` fields
3. package into a `.dtmodel` archive
4. add the model entry to `ai_models.json` in darktable-ai
5. create a new release with the `.dtmodel` as an asset
6. update `data/ai_models.json` in the darktable source to include
   the new model entry

---

## How to Add a New AI Feature

### Step 1: Create the AI Module

Create your processing module in `src/common/ai/`. Use opaque types
for encapsulation. The module should wrap `dt_ai_*` calls from
`ai/backend.h` and expose a clean public API.

```c
// src/common/ai/your_task.h
#pragma once
#include <glib.h>

typedef struct dt_your_task_env_t dt_your_task_env_t;
typedef struct dt_your_task_ctx_t dt_your_task_ctx_t;

dt_your_task_env_t *dt_your_task_env_init(void);
void dt_your_task_env_destroy(dt_your_task_env_t *env);

dt_your_task_ctx_t *dt_your_task_load(
  dt_your_task_env_t *env);
void dt_your_task_free(dt_your_task_ctx_t *ctx);

gboolean dt_your_task_available(
  dt_your_task_env_t *env);

int dt_your_task_process(
  dt_your_task_ctx_t *ctx,
  const float *in, float *out,
  int width, int height);
```

```c
// src/common/ai/your_task.c
#include "common/ai/your_task.h"
#include "ai/backend.h"
#include "common/ai_models.h"

#define TASK_KEY "your_task"

struct dt_your_task_env_t
{
  dt_ai_environment_t *ai_env;
};

struct dt_your_task_ctx_t
{
  dt_ai_context_t *ai_ctx;
  dt_your_task_env_t *env;
};

dt_your_task_ctx_t *dt_your_task_load(
  dt_your_task_env_t *env)
{
  if(!env) return NULL;
  char *model_id
    = dt_ai_models_get_active_for_task(TASK_KEY);
  if(!model_id || !model_id[0])
  {
    g_free(model_id);
    return NULL;
  }
  dt_ai_context_t *ai_ctx = dt_ai_load_model(
    env->ai_env, model_id, NULL, DT_AI_PROVIDER_CONFIGURED);
  g_free(model_id);
  if(!ai_ctx) return NULL;

  dt_your_task_ctx_t *ctx
    = g_new0(dt_your_task_ctx_t, 1);
  ctx->ai_ctx = ai_ctx;
  ctx->env = env;
  return ctx;
}

int dt_your_task_process(
  dt_your_task_ctx_t *ctx,
  const float *in, float *out,
  int width, int height)
{
  // prepare input tensor (convert colorspace, layout)
  // call dt_ai_run(ctx->ai_ctx, ...)
  // post-process output
  return 0;
}
```

### Step 2: Register in Build System

Add to `src/CMakeLists.txt` under the `USE_AI` section:

```cmake
FILE(GLOB SOURCE_FILES_AI
  "common/ai_models.c"
  "common/ai/segmentation.c"
  "common/ai/restore.c"
  "common/ai/restore_rgb.c"
  "common/ai/restore_raw_bayer.c"
  "common/ai/restore_raw_linear.c"
  "common/ai/your_task.c"       # add here
  ...
)
```

### Step 3: Add Model Entry

Add the model to `data/ai_models.json`:

```json
{
  "id": "your-task-model-name",
  "name": "your task model name",
  "description": "description of the model",
  "task": "your_task",
  "github_asset": "your-task-model-name.dtmodel",
  "default": true
}
```

The `.dtmodel` file is a zip/tar archive containing `config.json` and
the ONNX model file(s).

### Step 4: Create the UI Consumer

For a **lighttable module**, create `src/libs/your_module.c`.
For a **darkroom IOP**, create `src/iop/your_module.c`.

The UI module should only include your `src/common/ai/` header --
never include `ai/backend.h` or `common/ai_models.h` directly:

```c
#include "common/ai/your_task.h"

// in gui_init or job startup:
dt_your_task_env_t *env = dt_your_task_env_init();

// check availability:
gboolean avail = dt_your_task_available(env);

// process:
dt_your_task_ctx_t *ctx = dt_your_task_load(env);
dt_your_task_process(ctx, input, output, w, h);
dt_your_task_free(ctx);
```

---

## Available Tasks

| Task | Key | API | Consumer |
|------|-----|-----|----------|
| Object Mask | `"mask"` | `src/common/ai/segmentation.h` | `src/develop/masks/object.c` |
| Denoise | `"denoise"` | `src/common/ai/restore_rgb.h` | `src/libs/neural_restore.c` |
| Upscale | `"upscale"` | `src/common/ai/restore_rgb.h` | `src/libs/neural_restore.c` |
| Raw Denoise (Bayer)   | `"rawdenoise"` | `src/common/ai/restore_raw_bayer.h`  | `src/libs/neural_restore.c` |
| Raw Denoise (Linear)  | `"rawdenoise"` | `src/common/ai/restore_raw_linear.h` | `src/libs/neural_restore.c` |

For model requirements, I/O specifications, tiling strategies, color
space conventions, ONNX export instructions, and config.json examples
for each task, see **[AI_Tasks.md](AI_Tasks.md)**.

---

## Adding a New Execution Provider

1. Add enum value to `dt_ai_provider_t` in `backend.h` (before
   `DT_AI_PROVIDER_COUNT`)
2. Add entry to `dt_ai_providers[]` table with config string, display
   name, and platform guard
3. Add `case` in `_enable_acceleration()` in `backend_onnx.c`
4. Add `case` in `dt_ai_probe_provider()` in `backend_onnx.c`
5. The `_Static_assert` ensures the table stays in sync with the enum
