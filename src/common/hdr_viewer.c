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

#include "common/hdr_viewer.h"

// The viewer protocol uses POSIX (Berkeley) sockets. Build the real client on
// the POSIX platforms darktable supports and fall back to no-op stubs elsewhere
// (e.g. Windows), so no platform-specific build logic is needed: the feature is
// simply inert where the transport is unavailable.
#if defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__) \
    || defined(__NetBSD__) || defined(__OpenBSD__)

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Timeout when the viewer is not yet running (milliseconds).
#ifndef DT_HDR_VIEWER_CONNECT_TIMEOUT_MS
#define DT_HDR_VIEWER_CONNECT_TIMEOUT_MS 200
#endif

// Write exactly `len` bytes from `buf` to `fd`, restarting on EINTR.
// Returns 0 on success, -1 on error.
static int _write_exact(const int fd, const void *buf, const size_t len)
{
  const char *p = (const char *)buf;
  size_t left = len;

  while(left > 0)
  {
#ifdef __linux__
    // Linux has no SO_NOSIGPIPE; suppress SIGPIPE per write instead.
    const ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
#else
    const ssize_t n = write(fd, p, left);
#endif
    if(n < 0)
    {
      if(errno == EINTR) continue;
      return -1;
    }
    p += (size_t)n;
    left -= (size_t)n;
  }
  return 0;
}

// Encode a uint32_t as 4 little-endian bytes into `out`.
static void _encode_le32(uint8_t out[4], const uint32_t v)
{
  out[0] = (uint8_t)(v & 0xFFu);
  out[1] = (uint8_t)((v >> 8) & 0xFFu);
  out[2] = (uint8_t)((v >> 16) & 0xFFu);
  out[3] = (uint8_t)((v >> 24) & 0xFFu);
}

int dt_hdr_viewer_connect(void)
{
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if(fd < 0) return -1;

  // Prevent SIGPIPE from killing darktable if the viewer crashes or disconnects
  // while we are writing a frame (BSD/macOS; Linux uses MSG_NOSIGNAL above).
#ifdef SO_NOSIGPIPE
  {
    const int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
  }
#endif

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, DT_HDR_VIEWER_SOCKET_PATH, sizeof(addr.sun_path) - 1);

  // Non-blocking connect with a short timeout so darktable does not stall when
  // the viewer is not running.
  const int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, (flags < 0 ? 0 : flags) | O_NONBLOCK);

  int rc = connect(fd, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr));
  if(rc == 0)
  {
    // Connected immediately (unlikely for a Unix socket, but possible).
    fcntl(fd, F_SETFL, (flags < 0 ? 0 : flags));
    return fd;
  }

  if(errno != EINPROGRESS && errno != EAGAIN)
  {
    close(fd);
    return -1;
  }

  // Wait for the socket to become writable (= connected) or time out.
  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(fd, &wfds);

  struct timeval tv;
  tv.tv_sec = DT_HDR_VIEWER_CONNECT_TIMEOUT_MS / 1000;
  tv.tv_usec = (DT_HDR_VIEWER_CONNECT_TIMEOUT_MS % 1000) * 1000;

  rc = select(fd + 1, NULL, &wfds, NULL, &tv);
  if(rc <= 0)
  {
    close(fd);
    return -1;
  }

  // Confirm the connection actually succeeded.
  int err = 0;
  socklen_t errlen = (socklen_t)sizeof(err);
  getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
  if(err != 0)
  {
    close(fd);
    return -1;
  }

  // Restore blocking mode for the subsequent frame writes.
  fcntl(fd, F_SETFL, (flags < 0 ? 0 : flags));
  return fd;
}

void dt_hdr_viewer_send_frame(const int fd,
                              const uint32_t w,
                              const uint32_t h,
                              const float *rgb_linear,
                              const float rgb_to_xyz[9])
{
  if(fd < 0 || w == 0 || h == 0 || rgb_linear == NULL || rgb_to_xyz == NULL)
    return;

  // Header (protocol v2, 60 bytes), see hdr_viewer.h for the layout. The magic
  // is written as literal bytes so it is endian-independent; integers are
  // little-endian; floats are copied in host byte order (little-endian on every
  // supported platform).
  uint8_t header[60];
  header[0] = 'D';
  header[1] = 'T';
  header[2] = 'H';
  header[3] = 'V';
  _encode_le32(header + 4, DT_HDR_VIEWER_VERSION);
  _encode_le32(header + 8, w);
  _encode_le32(header + 12, h);
  _encode_le32(header + 16, 3u);                    // channels
  _encode_le32(header + 20, DT_HDR_VIEWER_XFER_LINEAR);
  memcpy(header + 24, rgb_to_xyz, 9u * sizeof(float)); // 36 bytes

  if(_write_exact(fd, header, sizeof(header)) != 0) return;

  const size_t pixel_bytes = (size_t)w * (size_t)h * 3u * sizeof(float);
  _write_exact(fd, rgb_linear, pixel_bytes);
  // Errors are silently ignored; the caller reconnects on the next frame.
}

void dt_hdr_viewer_disconnect(const int fd)
{
  if(fd >= 0) close(fd);
}

#else // no POSIX sockets (e.g. Windows): inert stubs

int dt_hdr_viewer_connect(void)
{
  return -1;
}

void dt_hdr_viewer_send_frame(const int fd,
                              const uint32_t w,
                              const uint32_t h,
                              const float *rgb_linear,
                              const float rgb_to_xyz[9])
{
  (void)fd;
  (void)w;
  (void)h;
  (void)rgb_linear;
  (void)rgb_to_xyz;
}

void dt_hdr_viewer_disconnect(const int fd)
{
  (void)fd;
}

#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
