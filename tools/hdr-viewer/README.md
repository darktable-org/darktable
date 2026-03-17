# darktable HDR Viewer

A standalone macOS application that displays HDR pixel data sent from darktable
over a Unix domain socket, using Metal with Extended Dynamic Range (EDR) output.

## Requirements

- macOS 12 Monterey or later
- Xcode Command Line Tools (provides `swift build`)
- An HDR-capable display (Pro Display XDR, MacBook Pro/Air with Liquid Retina XDR,
  or an external HDR monitor).  On SDR displays the app still works but applies a
  Reinhard tone map to keep values in [0, 1].

## Building

```sh
cd /Users/mayk/darktable/tools/hdr-viewer
swift build -c release
```

The binary is placed at `.build/release/HDRViewer`.

For development / debugging:

```sh
swift build          # debug build
swift run            # build + launch immediately
```

## Running

Launch the app **before** triggering a preview export in darktable:

```sh
.build/release/HDRViewer
```

A window titled **"darktable HDR Preview"** appears and waits on
`/tmp/dt_hdr_viewer.sock`.  Once darktable sends a frame the status text
disappears and the image is displayed.

## Protocol

darktable communicates via a Unix domain socket at `/tmp/dt_hdr_viewer.sock`.
Each frame is a simple binary message (all integers little-endian):

| Offset | Size | Type    | Description        |
|--------|------|---------|--------------------|
| 0      | 4    | uint32  | Image width        |
| 4      | 4    | uint32  | Image height       |
| 8      | w×h×3×4 | float32[] | RGB pixels, linear BT.2020, row-major top-to-bottom |

A new TCP connection is established per frame (or the connection may be kept
open for multiple frames; the server handles both).

## Integrating with darktable (C side)

Copy `dt_hdr_client.h` and `dt_hdr_client.c` into the darktable source tree
and add them to the relevant CMakeLists.  Then call from a soft-proofing or
export code path:

```c
#include "dt_hdr_client.h"

// On every preview update:
int fd = dt_hdr_viewer_connect();
if (fd >= 0) {
    // buf is float*, width * height * 3 floats, linear BT.2020
    dt_hdr_viewer_send_frame(fd, width, height, buf);
    dt_hdr_viewer_disconnect(fd);
}
```

`dt_hdr_viewer_connect()` returns -1 (and does **not** block) when the viewer
is not running, so it is safe to call unconditionally in a hot path.

## Architecture

```
darktable process                     HDRViewer.app
─────────────────                     ─────────────────────────────────────
dt_hdr_client.c          TCP/Unix     IPCServer.swift
  connect()          ──────────────▶   accept()
  send_frame()       ──────frame──▶    decode → [Float]
  disconnect()                              │
                                      HDRViewController.swift
                                            │  DispatchQueue.main
                                      HDRMetalView.swift
                                            │  MTLTexture (RGBA32Float)
                                      Shaders.metal
                                            │  BT.2020 → Display-P3
                                            │  tone map to [0, EDR headroom]
                                      CAMetalLayer (RGBA16Float, EDR)
                                            │
                                      Display hardware (HDR)
```

### Shader pipeline

1. **Sample** the source RGBA32Float texture (linear BT.2020).
2. **Matrix multiply** with the BT.2020 → Linear Display-P3 3×3 matrix.
3. **Tone map** using a smooth knee:
   - Values ≤ 1.0 pass through unchanged (SDR range).
   - Values in (1.0, EDR headroom] are kept as HDR signal.
   - Values above `headroom` are soft-compressed toward `headroom`.
4. **Output** as `half4` into the `RGBA16Float` CAMetalLayer drawable, whose
   colorspace is set to `extendedLinearDisplayP3` so the OS compositor
   interprets the values correctly without additional color conversion.

### EDR headroom

The shader receives `screen.maximumExtendedDynamicRangeColorComponentValue`
each frame.  This value is typically 2.0–4.0 on an XDR display at full
brightness, and 1.0 on SDR displays.

## Known limitations

- Only one connected client at a time is fully supported (the accept loop is
  serial).  darktable sends one frame per connection, so this is not a
  practical limitation.
- The Metal library is compiled at build time from `Shaders.metal`.  If you
  modify the shader, re-run `swift build`.
- Aspect ratio locking triggers only when the image dimensions change; it does
  not prevent arbitrary window resizing.
