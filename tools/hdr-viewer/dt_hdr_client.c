/*
 * dt_hdr_client.c
 *
 * Minimal POSIX-only C client for the darktable HDR Viewer.
 * No external dependencies; compiles cleanly on macOS 10.13+ and Linux.
 *
 * See dt_hdr_client.h for API documentation.
 */

#include "dt_hdr_client.h"

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* Timeout when the viewer is not yet running (milliseconds). */
#ifndef DT_HDR_VIEWER_CONNECT_TIMEOUT_MS
#  define DT_HDR_VIEWER_CONNECT_TIMEOUT_MS 200
#endif

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/**
 * Write exactly `len` bytes from `buf` to `fd`, restarting on EINTR.
 * Returns 0 on success, -1 on error.
 */
static int write_exact(int fd, const void *buf, size_t len)
{
    const char *p    = (const char *)buf;
    size_t      left = len;

    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p    += (size_t)n;
        left -= (size_t)n;
    }
    return 0;
}

/**
 * Encode a uint32_t as 4 little-endian bytes into `out`.
 */
static void encode_le32(uint8_t out[4], uint32_t v)
{
    out[0] = (uint8_t)(v        & 0xFFu);
    out[1] = (uint8_t)((v >>  8) & 0xFFu);
    out[2] = (uint8_t)((v >> 16) & 0xFFu);
    out[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int dt_hdr_viewer_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, DT_HDR_VIEWER_SOCKET_PATH,
            sizeof(addr.sun_path) - 1);

    /*
     * Set a non-blocking connect with a short timeout so darktable does not
     * stall when the viewer is not running.
     */
#if defined(__APPLE__) || defined(__linux__)
    {
        /* Set socket to non-blocking */
        int flags = 0;
#  if defined(O_NONBLOCK)
        {
            /* POSIX fcntl path */
            flags = fcntl(fd, F_GETFL, 0);
            if (flags < 0) flags = 0;
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
#  endif

        int rc = connect(fd,
                         (const struct sockaddr *)&addr,
                         (socklen_t)sizeof(addr));

        if (rc == 0) {
            /* Connected immediately (unlikely for Unix sockets but possible) */
#  if defined(O_NONBLOCK)
            fcntl(fd, F_SETFL, flags);  /* restore blocking */
#  endif
            return fd;
        }

        if (errno != EINPROGRESS && errno != EAGAIN) {
            close(fd);
            return -1;
        }

        /* Wait for the socket to become writable (= connected) */
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);

        struct timeval tv;
        tv.tv_sec  = DT_HDR_VIEWER_CONNECT_TIMEOUT_MS / 1000;
        tv.tv_usec = (DT_HDR_VIEWER_CONNECT_TIMEOUT_MS % 1000) * 1000;

        rc = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (rc <= 0) {
            /* Timeout or error */
            close(fd);
            return -1;
        }

        /* Check that the connection actually succeeded */
        int err = 0;
        socklen_t errlen = (socklen_t)sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) {
            close(fd);
            return -1;
        }

        /* Restore blocking mode for subsequent writes */
#  if defined(O_NONBLOCK)
        fcntl(fd, F_SETFL, flags);
#  endif
        return fd;
    }
#else
    /* Fallback: plain blocking connect (may hang briefly if viewer is absent) */
    if (connect(fd, (const struct sockaddr *)&addr,
                (socklen_t)sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
#endif
}

void dt_hdr_viewer_send_frame(int              fd,
                              uint32_t         w,
                              uint32_t         h,
                              const float     *rgb_linear_bt2020)
{
    if (fd < 0 || w == 0 || h == 0 || rgb_linear_bt2020 == NULL) return;

    /* Send 8-byte header: width and height as little-endian uint32 */
    uint8_t header[8];
    encode_le32(header + 0, w);
    encode_le32(header + 4, h);

    if (write_exact(fd, header, sizeof(header)) != 0) return;

    /* Send pixel data – already in host byte order (float32).
     * On all modern Macs (and x86/ARM64 Linux) the host is little-endian,
     * which matches what the Swift receiver expects.
     * If big-endian support is ever needed, swap bytes here. */
    size_t pixel_bytes = (size_t)w * (size_t)h * 3u * sizeof(float);
    write_exact(fd, rgb_linear_bt2020, pixel_bytes);
    /* Errors are silently ignored; caller should reconnect if needed. */
}

void dt_hdr_viewer_disconnect(int fd)
{
    if (fd >= 0) close(fd);
}
