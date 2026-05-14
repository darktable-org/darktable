# AI Tasks Reference

This document describes each AI task available in darktable, including
model requirements, I/O specifications, and integration details.

See [AI.md](AI.md) for the architecture overview and how to add new tasks.

---

## Object Mask

Interactive object masking using SAM/SAM2/SegNext models.

**Task key**: `"mask"`
**API**: `src/common/ai/segmentation.h`
**Consumer**: `src/develop/masks/object.c`

### How It Works

1. user selects the object mask tool in the mask manager
2. the image is exported as sRGB uint8 and encoded by the SAM encoder
   (runs once per image, cached)
3. user clicks to place foreground/background points
4. each click runs the lightweight decoder to produce a mask
5. iterative refinement: previous mask is fed back to improve accuracy
6. the mask is resized to image dimensions and applied as a darktable
   mask shape

### Supported Architectures

| Architecture | `config.json` arch | Encoder Outputs | Mask Candidates | Box Prompts |
|--------------|-------------------|-----------------|-----------------|-------------|
| SAM 2.1 | `"sam2"` | 3 tensors | 3 (multi-mask) | yes |
| SegNext | `"segnext"` | 2 tensors | 1 (single-mask) | no |

### Encoder

| Tensor | Shape | Type | Description |
|--------|-------|------|-------------|
| Input 0 | `[1, 3, 1024, 1024]` | float32 | preprocessed image |

Preprocessing (applied by `segmentation.c`):
1. resize longest side to 1024 px (bilinear), preserve aspect ratio
2. zero-pad shorter side to 1024x1024
3. SAM: normalize with ImageNet mean/std. SegNext: scale to [0,1]
4. convert HWC -> CHW

Encoder outputs (typical):

| Output | Shape | Description |
|--------|-------|-------------|
| 0 | `[1, 256, 64, 64]` | image embeddings |
| 1 | `[1, 32, 256, 256]` | high-resolution features |
| 2 (SAM2) | `[1, 64, 128, 128]` | mid-resolution features |

### Decoder

Inputs:

| Index | Name | Shape | Description |
|-------|------|-------|-------------|
| 0..E | encoder outputs | varies | passed through from encoder |
| E+1 | `point_coords` | `[1, N+1, 2]` | point coords (N prompts + 1 SAM padding) |
| E+2 | `point_labels` | `[1, N+1]` | 1=foreground, 0=background, -1=padding |
| E+3 | `mask_input` | `[1, 1, 256, 256]` | previous low-res mask |
| E+4 | `has_mask_input` | `[1]` | 0.0 (first click) or 1.0 (refinement) |

Outputs:

| Index | Name | Shape | Description |
|-------|------|-------|-------------|
| 0 | `masks` | `[1, M, 1024, 1024]` | mask logits (pre-sigmoid) |
| 1 | `iou_predictions` | `[1, M]` | predicted IoU per mask |
| 2 | `low_res_masks` | `[1, M, 256, 256]` | for iterative refinement |

### Mask Post-Processing

1. select mask with highest predicted IoU score
2. crop out the zero-padded region
3. bilinear resize to original image dimensions
4. apply sigmoid: `mask = 1 / (1 + exp(-logits))`
5. output values in [0, 1] range

### Iterative Refinement

- first decode: `has_mask_input = 0.0`, decoder ignores `mask_input`
- subsequent decodes: previous low-res mask is fed back with
  `has_mask_input = 1.0`
- `dt_seg_reset_prev_mask()` clears the cached mask without clearing
  image embeddings
- `dt_seg_reset_encoding()` clears everything (call on image change)

### config.json Example

```json
{
  "id": "mask-object-sam21-small",
  "name": "mask sam2.1 hiera small",
  "description": "Segment Anything 2.1 (Hiera Small) for interactive masking",
  "task": "mask",
  "arch": "sam2",
  "backend": "onnx"
}
```

### Directory Layout

```
mask-object-sam21-small/
  config.json
  encoder.onnx
  decoder.onnx
```

### ONNX Export

Conversion scripts are maintained in the
[darktable-ai](https://github.com/darktable-org/darktable-ai)
repository. Requirements for the decoder export:

- no `orig_im_size` input
- `masks` output at fixed 1024x1024 (include `F.interpolate` in graph)
- `low_res_masks` at 256x256
- all spatial dimensions concrete (no symbolic dims like `num_labels`)
- only `num_points` may be dynamic

---

## Denoise

Removes noise from developed images using neural network inference.

**Task key**: `"denoise"`
**API**: `src/common/ai/restore.h` (loader: `dt_restore_load_denoise`), `src/common/ai/restore_rgb.h` (processing: `dt_restore_process_tiled`)
**Consumer**: `src/libs/neural_restore.c`

### How It Works

1. darktable exports the image through the full processing pipeline
   (white balance, exposure, lens correction, etc.) producing linear
   Rec.709 float4 RGBA pixels
2. the restore module converts linear RGB to sRGB, tiles the image
   with overlap, and runs each tile through the ONNX model
3. output tiles are reassembled and converted back to linear RGB
4. optionally, DWT-based detail recovery blends fine texture from the
   original back into the denoised result
5. the result is written as TIFF with embedded ICC profile and EXIF

### Model Requirements

#### Single-Input (NAFNet, UNet, NIND)

| Tensor | Name | Shape | Type | Description |
|--------|------|-------|------|-------------|
| Input 0 | `input` | `[1, 3, H, W]` | float32 | sRGB image, NCHW planar layout, values [0,1] |
| Output 0 | `output` | `[1, 3, H, W]` | float32 | denoised sRGB image, same layout |

- H and W are dynamic (determined by tile size at runtime)
- input and output spatial dimensions must match (scale = 1x)

#### Multi-Input (FFDNet)

| Tensor | Name | Shape | Type | Description |
|--------|------|-------|------|-------------|
| Input 0 | `input` | `[1, 3, H, W]` | float32 | sRGB image |
| Input 1 | `sigma` | `[1, 1, H, W]` | float32 | noise level map, values = sigma / 255.0 |
| Output 0 | `output` | `[1, 3, H, W]` | float32 | denoised image |

Set `"num_inputs": 2` in `config.json`.

### Color Space

Models operate in sRGB. The restore module handles conversion:
- before inference: linear Rec.709 -> sRGB (IEC 61966-2-1)
- after inference: sRGB -> linear Rec.709

### Tiling

- tile sizes (tried in order): 2048, 1536, 1024, 768, 512, 384, 256
- overlap: 64 pixels on each edge
- memory budget: 1/4 of available darktable memory
- border handling: mirror padding

### Detail Recovery

DWT (discrete wavelet transform) based luminance detail recovery:
- extracts luminance residual: original - denoised
- filters with wavelet decomposition (5 bands)
- fine bands (noise) are thresholded aggressively
- coarse bands (texture) are preserved
- filtered residual is blended back at user-controlled strength

### config.json Example

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

### ONNX Export

```python
torch.onnx.export(model, dummy_input, "model.onnx",
                  input_names=["input"],
                  output_names=["output"],
                  dynamic_axes={
                    "input": {2: "height", 3: "width"},
                    "output": {2: "height", 3: "width"}
                  })
```

---

## Upscale

Super-resolution upscaling of developed images (2x or 4x).

**Task key**: `"upscale"`
**API**: `src/common/ai/restore.h` (loaders: `dt_restore_load_upscale_x2`, `dt_restore_load_upscale_x4`), `src/common/ai/restore_rgb.h` (processing: `dt_restore_process_tiled`)
**Consumer**: `src/libs/neural_restore.c`

### How It Works

Same pipeline as denoise, but the output dimensions are scaled:
- 2x: output is `[1, 3, H*2, W*2]`
- 4x: output is `[1, 3, H*4, W*4]`

A single model can provide both scales via separate ONNX files:
- `model_x2.onnx` for 2x upscale
- `model_x4.onnx` for 4x upscale

### Model Requirements

| Tensor | Name | Shape | Type | Description |
|--------|------|-------|------|-------------|
| Input 0 | `input` | `[1, 3, H, W]` | float32 | sRGB image, NCHW layout |
| Output 0 | `output` | `[1, 3, H*S, W*S]` | float32 | upscaled sRGB image (S = scale factor) |

### Tiling

- tile sizes (tried in order): 512, 384, 256, 192 (smaller than denoise
  due to scale^2 memory multiplier)
- overlap: 16 pixels on each edge
- for TIFF streaming: scanlines are written directly without buffering
  the full upscaled output (important for large images -- a 4x upscale
  of 60MP would need ~3.6GB)

### config.json Example

```json
{
  "id": "upscale-bsrgan",
  "name": "upscale bsrgan",
  "description": "BSRGAN 2x and 4x blind super-resolution",
  "task": "upscale",
  "github_asset": "upscale-bsrgan.dtmodel",
  "default": true
}
```

### Directory Layout

```
upscale-bsrgan/
  config.json
  model_x2.onnx
  model_x4.onnx
```
