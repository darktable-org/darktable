/*
    This file is part of darktable,
    Copyright (C) 2024 darktable developers.

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

#include "common/p2p.h"
#include "common/darktable.h"
#include "control/conf.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static struct
{
  GMutex   lock;
  int      sockfd;      // -1 when disconnected
  GPid     daemon_pid;  // 0 when not spawned
  char     socket_path[256];
} _p2p = { .sockfd = -1, .daemon_pid = 0 };

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void _socket_path(char *out, size_t len)
{
  const char *xdg = g_getenv("XDG_RUNTIME_DIR");
  if(xdg)
    g_snprintf(out, len, "%s/darktable-p2p.sock", xdg);
  else
    g_snprintf(out, len, "/tmp/darktable-p2p.sock");
}

// Send a JSON-line to the daemon. Returns TRUE on success.
static gboolean _send_json(const char *json)
{
  if(_p2p.sockfd < 0) return FALSE;
  // Append newline (JSON-lines protocol)
  char *line = g_strdup_printf("%s\n", json);
  ssize_t len = (ssize_t)strlen(line);
  ssize_t sent = send(_p2p.sockfd, line, len, MSG_NOSIGNAL);
  g_free(line);
  if(sent != len)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[p2p] socket send failed: %s", strerror(errno));
    close(_p2p.sockfd);
    _p2p.sockfd = -1;
    return FALSE;
  }
  return TRUE;
}

static gboolean _connect_socket(void)
{
  if(_p2p.sockfd >= 0) return TRUE;

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if(fd < 0) return FALSE;

  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  g_strlcpy(addr.sun_path, _p2p.socket_path, sizeof(addr.sun_path));

  if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
  {
    close(fd);
    return FALSE;
  }
  _p2p.sockfd = fd;
  return TRUE;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void dt_p2p_init(void)
{
  g_mutex_init(&_p2p.lock);
  _socket_path(_p2p.socket_path, sizeof(_p2p.socket_path));

  const char *passphrase = dt_conf_get_string_const("plugins/p2p/passphrase");
  if(!passphrase || passphrase[0] == '\0') return;

  // Locate daemon binary next to darktable executable
  char *exe_dir  = g_path_get_dirname(g_get_prgname());
  char *daemon   = g_build_filename(exe_dir, "dt-p2p-daemon", NULL);
  g_free(exe_dir);

  if(!g_file_test(daemon, G_FILE_TEST_IS_EXECUTABLE))
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "[p2p] daemon not found at '%s'; P2P sync disabled", daemon);
    g_free(daemon);
    return;
  }

  // Build argv
  const char *argv[] = {
    daemon,
    "--socket", _p2p.socket_path,
    "--passphrase", passphrase,
    NULL
  };
  GError *err = NULL;
  if(!g_spawn_async(NULL, (gchar **)argv, NULL,
                    G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                    NULL, NULL, &_p2p.daemon_pid, &err))
  {
    dt_print(DT_DEBUG_IMAGEIO, "[p2p] failed to spawn daemon: %s",
             err ? err->message : "unknown error");
    if(err) g_error_free(err);
    g_free(daemon);
    return;
  }
  g_free(daemon);

  // Give daemon a moment to bind its socket, then connect
  for(int i = 0; i < 20; i++)
  {
    struct timespec ts = { .tv_nsec = 100000000 }; // 100 ms
    nanosleep(&ts, NULL);
    if(_connect_socket()) break;
  }

  if(_p2p.sockfd < 0)
    dt_print(DT_DEBUG_IMAGEIO, "[p2p] daemon started but socket not ready");
  else
    dt_print(DT_DEBUG_IMAGEIO, "[p2p] connected to daemon (pid %d)", (int)_p2p.daemon_pid);
}

void dt_p2p_cleanup(void)
{
  g_mutex_lock(&_p2p.lock);
  if(_p2p.sockfd >= 0)
  {
    close(_p2p.sockfd);
    _p2p.sockfd = -1;
  }
  if(_p2p.daemon_pid)
  {
    kill((pid_t)_p2p.daemon_pid, SIGTERM);
    g_spawn_close_pid(_p2p.daemon_pid);
    _p2p.daemon_pid = 0;
  }
  g_mutex_unlock(&_p2p.lock);
  g_mutex_clear(&_p2p.lock);
}

void dt_p2p_push_xmp(const char *raw_path, const char *xmp_path)
{
  if(!raw_path || !xmp_path) return;

  gchar *content = NULL;
  if(!g_file_get_contents(xmp_path, &content, NULL, NULL)) return;

  // Escape content for inline JSON: replace " and \ and newlines
  GString *escaped = g_string_new(NULL);
  for(const char *p = content; *p; p++)
  {
    if(*p == '"')       g_string_append(escaped, "\\\"");
    else if(*p == '\\') g_string_append(escaped, "\\\\");
    else if(*p == '\n') g_string_append(escaped, "\\n");
    else if(*p == '\r') g_string_append(escaped, "\\r");
    else                g_string_append_c(escaped, *p);
  }
  g_free(content);

  // Escape raw_path
  GString *ep = g_string_new(NULL);
  for(const char *p = raw_path; *p; p++)
  {
    if(*p == '"' || *p == '\\') g_string_append_c(ep, '\\');
    g_string_append_c(ep, *p);
  }

  const gint64 now_ns = g_get_real_time() * 1000LL;
  char *json = g_strdup_printf(
    "{\"type\":\"xmp_push\",\"data\":"
    "{\"path\":\"%s\",\"content\":\"%s\",\"mtime\":%" G_GINT64_FORMAT "}}",
    ep->str, escaped->str, now_ns);

  g_mutex_lock(&_p2p.lock);
  if(_connect_socket())
    _send_json(json);
  g_mutex_unlock(&_p2p.lock);

  g_free(json);
  g_string_free(escaped, TRUE);
  g_string_free(ep, TRUE);
}

void dt_p2p_fetch_proxy(const char *raw_path)
{
  if(!raw_path) return;

  GString *ep = g_string_new(NULL);
  for(const char *p = raw_path; *p; p++)
  {
    if(*p == '"' || *p == '\\') g_string_append_c(ep, '\\');
    g_string_append_c(ep, *p);
  }

  char *json = g_strdup_printf(
    "{\"type\":\"fetch_proxy\",\"data\":{\"path\":\"%s\"}}", ep->str);

  g_mutex_lock(&_p2p.lock);
  if(_connect_socket())
    _send_json(json);
  g_mutex_unlock(&_p2p.lock);

  g_free(json);
  g_string_free(ep, TRUE);
}

gboolean dt_p2p_is_running(void)
{
  g_mutex_lock(&_p2p.lock);
  const gboolean ok = _p2p.sockfd >= 0;
  g_mutex_unlock(&_p2p.lock);
  return ok;
}
