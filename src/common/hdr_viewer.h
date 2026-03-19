/*
 * dt_hdr_client.h
 *
 * Minimal POSIX-only C client library for sending HDR pixel frames to the
 * darktable HDR Viewer app (https://github.com/MaykThewessen/darktable-hdr-viewer).
 *
 * Protocol (little-endian):
 *   [4 bytes] width  – uint32_t
 *   [4 bytes] height – uint32_t
 *   [width * height * 3 * sizeof(float)] – RGB float32, linear BT.2020,
 *                                           row-major, top-to-bottom
 *
 * Typical usage from darktable:
 *
 *   int fd = dt_hdr_viewer_connect();
 *   if (fd >= 0) {
 *       dt_hdr_viewer_send_frame(fd, width, height, rgb_linear_bt2020);
 *       dt_hdr_viewer_disconnect(fd);
 *   }
 *
 * Or keep `fd` open across frames for lower overhead (the server handles
 * multiple frames per connection).
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default Unix-domain socket path used by the HDR Viewer app. */
#define DT_HDR_VIEWER_SOCKET_PATH "/tmp/dt_hdr_viewer.sock"

/**
 * Connect to the HDR Viewer Unix socket.
 *
 * Returns a connected socket file descriptor on success, or -1 on failure
 * (check errno for details).  The connection attempt times out after
 * DT_HDR_VIEWER_CONNECT_TIMEOUT_MS milliseconds.
 */
int dt_hdr_viewer_connect(void);

/**
 * Send one frame of linear BT.2020 RGB pixels to the HDR Viewer.
 *
 * @param fd              File descriptor returned by dt_hdr_viewer_connect().
 * @param w               Image width  in pixels.
 * @param h               Image height in pixels.
 * @param rgb_linear_bt2020  Row-major, top-to-bottom, interleaved RGB float32
 *                        buffer of size w * h * 3 floats.
 *
 * The call blocks until all data has been written.  On write error the
 * function returns silently; the caller should call dt_hdr_viewer_disconnect()
 * and reconnect on the next frame if reliable delivery is required.
 */
void dt_hdr_viewer_send_frame(int fd,
                              uint32_t w,
                              uint32_t h,
                              const float *rgb_linear_bt2020);

/**
 * Close the connection to the HDR Viewer.
 *
 * @param fd  File descriptor returned by dt_hdr_viewer_connect(), or -1
 *            (no-op in that case).
 */
void dt_hdr_viewer_disconnect(int fd);

#ifdef __cplusplus
}
#endif
