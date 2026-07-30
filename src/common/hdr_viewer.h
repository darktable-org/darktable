/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
    Minimal POSIX-only client for streaming the darkroom preview to an external
    HDR viewer over a Unix domain socket. The viewer is a separate, platform
    specific application (https://github.com/MaykThewessen/darktable-hdr-viewer);
    nothing platform specific is built into darktable: this client is plain
    POSIX C and does nothing unless the viewer is running and the feature is
    enabled via plugins/darkroom/hdr_viewer_enabled.

    The frame buffer carries the working-space linear RGB image (the input to
    the output color profile module, "colorout"), so the receiver can do its own
    accurate, display-referred color management. The working profile's
    RGB -> XYZ(D50) matrix travels in the header so the viewer is correct for any
    working profile, not just the linear Rec.2020 default.

    Wire format (protocol version 2, all multi-byte values little-endian, which
    matches the host byte order on every platform darktable supports):

      offset  size  field
      0       4     magic     : bytes 'D','T','H','V'
      4       4     version   : uint32, currently DT_HDR_VIEWER_VERSION (2)
      8       4     width     : uint32, pixels
      12      4     height    : uint32, pixels
      16      4     channels  : uint32, currently 3 (interleaved RGB)
      20      4     transfer  : uint32, DT_HDR_VIEWER_XFER_LINEAR (0) = linear light
      24      36    rgb_to_xyz: 9 x float32, row-major working RGB -> XYZ (D50 PCS)
      60      w*h*channels*4  : float32 pixels, row-major, top-to-bottom

    The server also accepts multiple frames on a single connection.
*/

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default Unix-domain socket path used by the HDR viewer app. */
#define DT_HDR_VIEWER_SOCKET_PATH "/tmp/dt_hdr_viewer.sock"

/** Wire protocol version understood by both ends. */
#define DT_HDR_VIEWER_VERSION 2u

/** Transfer function tag: the pixel data is linear light. */
#define DT_HDR_VIEWER_XFER_LINEAR 0u

/**
 * Connect to the HDR viewer Unix socket.
 *
 * Returns a connected socket file descriptor on success, or -1 on failure
 * (check errno for details). The connection attempt times out after
 * DT_HDR_VIEWER_CONNECT_TIMEOUT_MS milliseconds so it is safe to call
 * unconditionally in a hot path: when the viewer is not running it returns
 * -1 quickly rather than blocking.
 */
int dt_hdr_viewer_connect(void);

/**
 * Send one frame of working-space linear RGB pixels to the HDR viewer.
 *
 * @param fd          file descriptor returned by dt_hdr_viewer_connect().
 * @param w           image width in pixels.
 * @param h           image height in pixels.
 * @param rgb_linear  row-major, top-to-bottom, interleaved RGB float32 buffer
 *                    of size w * h * 3 floats, linear light in the working
 *                    profile's primaries. Values may exceed 1.0 (HDR signal).
 * @param rgb_to_xyz  9 floats, row-major 3x3 matrix converting the working
 *                    profile's linear RGB to XYZ (ICC D50 PCS). This is
 *                    darktable's work-profile matrix_in with the SIMD padding
 *                    column removed.
 *
 * The call blocks until all data has been written. On write error the function
 * returns silently; the caller should disconnect and reconnect on the next
 * frame.
 */
void dt_hdr_viewer_send_frame(int fd,
                              uint32_t w,
                              uint32_t h,
                              const float *rgb_linear,
                              const float rgb_to_xyz[9]);

/**
 * Close the connection to the HDR viewer.
 *
 * @param fd  file descriptor returned by dt_hdr_viewer_connect(), or -1
 *            (no-op in that case).
 */
void dt_hdr_viewer_disconnect(int fd);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
