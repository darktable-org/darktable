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
#include "common/film.h"
#include "common/image.h"
#include "common/exif.h"
#include "common/image_cache.h"
#include "control/conf.h"
#include "control/signal.h"
#include "gui/gtk.h"

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
  int      sockfd;      // command socket, -1 when disconnected
  GPid     daemon_pid;
  char     socket_path[256];
} _p2p = { .sockfd = -1, .daemon_pid = 0 };

// Event subscription connection (separate socket, persistent read loop).
static struct
{
  int      sockfd;
  GThread *thread;
  gint     stop;        // atomic flag
} _p2p_evt = { .sockfd = -1, .thread = NULL, .stop = 0 };

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

static gboolean _send_json(const char *json)
{
  if(_p2p.sockfd < 0) return FALSE;
  char *line = g_strdup_printf("%s\n", json);
  ssize_t len  = (ssize_t)strlen(line);
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
// Push-event handling (image_imported, xmp_updated)
// ---------------------------------------------------------------------------

typedef struct _import_ctx_t
{
  char path[PATH_MAX];
} _import_ctx_t;

// Called on the GTK main thread; imports the image into the darktable library.
static gboolean _import_image_idle(gpointer data)
{
  _import_ctx_t *ctx = data;

  char *dir = g_path_get_dirname(ctx->path);
  dt_film_t film;
  dt_filmid_t fid = dt_film_new(&film, dir);
  g_free(dir);

  if(dt_is_valid_filmid(fid))
  {
    const dt_imgid_t imgid = dt_image_import(fid, ctx->path, TRUE, TRUE);
    if(dt_is_valid_imgid(imgid))
      dt_print(DT_DEBUG_IMAGEIO, "[p2p] imported image id=%d '%s'", imgid, ctx->path);
    dt_film_cleanup(&film);
  }
  else
  {
    dt_print(DT_DEBUG_IMAGEIO, "[p2p] could not find/create film for '%s'", ctx->path);
  }

  free(ctx);
  return G_SOURCE_REMOVE;
}

// Called on the GTK main thread; reloads the XMP sidecar for an already-imported image.
static gboolean _xmp_reload_idle(gpointer data)
{
  _import_ctx_t *ctx = data;

  dt_imgid_t imgid = dt_image_get_id_full_path(ctx->path);
  if(dt_is_valid_imgid(imgid))
  {
    char xmp_path[PATH_MAX];
    g_snprintf(xmp_path, sizeof(xmp_path), "%s.xmp", ctx->path);
    dt_image_t *img = dt_image_cache_get(imgid, 'w');
    if(img)
    {
      dt_exif_xmp_read(img, xmp_path, FALSE);
      dt_image_cache_write_release(img, DT_IMAGE_CACHE_RELAXED);
      dt_print(DT_DEBUG_IMAGEIO, "[p2p] reloaded XMP for '%s'", ctx->path);
    }
  }
  else
  {
    // Image not in library yet — import it (the proxy and XMP are both on disk).
    return _import_image_idle(ctx);
  }

  free(ctx);
  return G_SOURCE_REMOVE;
}

// Extract the value of the first "path":"..." field found in json_fragment.
// Handles basic JSON string escaping (\\ and \").
static gboolean _extract_path(const char *json_fragment, char *out, size_t out_size)
{
  const char *p = strstr(json_fragment, "\"path\"");
  if(!p) return FALSE;
  p += 6; // past "path"
  while(*p && *p != '"') p++;
  if(!*p) return FALSE;
  p++; // past opening quote
  size_t i = 0;
  while(*p && *p != '"' && i + 1 < out_size)
  {
    if(*p == '\\' && *(p + 1))
    {
      p++;
      switch(*p)
      {
        case 'n':  out[i++] = '\n'; break;
        case 'r':  out[i++] = '\r'; break;
        case 't':  out[i++] = '\t'; break;
        default:   out[i++] = *p;   break;
      }
    }
    else
    {
      out[i++] = *p;
    }
    p++;
  }
  out[i] = '\0';
  return (i > 0);
}

// Background thread: maintains a persistent event connection to the daemon
// and dispatches push events to the GTK main thread.
static gpointer _event_thread_func(gpointer user_data)
{
  (void)user_data;

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if(fd < 0) return NULL;

  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  g_strlcpy(addr.sun_path, _p2p.socket_path, sizeof(addr.sun_path));

  if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
  {
    close(fd);
    dt_print(DT_DEBUG_IMAGEIO, "[p2p] event thread: connect failed");
    return NULL;
  }
  _p2p_evt.sockfd = fd;

  // Tell the daemon we want push events on this connection.
  const char *sub = "{\"type\":\"subscribe_events\"}\n";
  send(fd, sub, strlen(sub), MSG_NOSIGNAL);

  FILE *f = fdopen(fd, "r");
  if(!f)
  {
    close(fd);
    _p2p_evt.sockfd = -1;
    return NULL;
  }

  char line[65536];
  while(!g_atomic_int_get(&_p2p_evt.stop) && fgets(line, sizeof(line), f))
  {
    if(strstr(line, "\"image_imported\""))
    {
      const char *data = strstr(line, "\"data\"");
      if(!data) continue;
      _import_ctx_t *ctx = malloc(sizeof(*ctx));
      if(ctx && _extract_path(data, ctx->path, sizeof(ctx->path)))
      {
        dt_print(DT_DEBUG_IMAGEIO, "[p2p] received image_imported: %s", ctx->path);
        gdk_threads_add_idle(_import_image_idle, ctx);
      }
      else
        free(ctx);
    }
    else if(strstr(line, "\"xmp_updated\""))
    {
      const char *data = strstr(line, "\"data\"");
      if(!data) continue;
      _import_ctx_t *ctx = malloc(sizeof(*ctx));
      if(ctx && _extract_path(data, ctx->path, sizeof(ctx->path)))
      {
        dt_print(DT_DEBUG_IMAGEIO, "[p2p] received xmp_updated: %s", ctx->path);
        gdk_threads_add_idle(_xmp_reload_idle, ctx);
      }
      else
        free(ctx);
    }
  }

  fclose(f);
  _p2p_evt.sockfd = -1;
  return NULL;
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

  char *exe_dir = g_path_get_dirname(g_get_prgname());
  char *daemon  = g_build_filename(exe_dir, "dt-p2p-daemon", NULL);
  g_free(exe_dir);

  if(!g_file_test(daemon, G_FILE_TEST_IS_EXECUTABLE))
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "[p2p] daemon not found at '%s'; P2P sync disabled", daemon);
    g_free(daemon);
    return;
  }

  // Build argv, optionally with --peers and --proxy-dir.
  GPtrArray *argv_arr = g_ptr_array_new();
  g_ptr_array_add(argv_arr, daemon);
  g_ptr_array_add(argv_arr, (gchar *)"--socket");
  g_ptr_array_add(argv_arr, _p2p.socket_path);
  g_ptr_array_add(argv_arr, (gchar *)"--passphrase");
  g_ptr_array_add(argv_arr, (gchar *)passphrase);

  const char *proxy_dir = dt_conf_get_string_const("plugins/p2p/proxy_dir");
  if(proxy_dir && proxy_dir[0])
  {
    g_ptr_array_add(argv_arr, (gchar *)"--proxy-dir");
    g_ptr_array_add(argv_arr, (gchar *)proxy_dir);
  }

  const char *peers = dt_conf_get_string_const("plugins/p2p/peers");
  if(peers && peers[0])
  {
    g_ptr_array_add(argv_arr, (gchar *)"--peers");
    g_ptr_array_add(argv_arr, (gchar *)peers);
  }

  g_ptr_array_add(argv_arr, NULL);

  GError *err = NULL;
  if(!g_spawn_async(NULL, (gchar **)argv_arr->pdata, NULL,
                    G_SPAWN_DO_NOT_REAP_CHILD
                    | G_SPAWN_STDOUT_TO_DEV_NULL
                    | G_SPAWN_STDERR_TO_DEV_NULL,
                    NULL, NULL, &_p2p.daemon_pid, &err))
  {
    dt_print(DT_DEBUG_IMAGEIO, "[p2p] failed to spawn daemon: %s",
             err ? err->message : "unknown error");
    if(err) g_error_free(err);
  }
  g_ptr_array_free(argv_arr, TRUE);
  g_free(daemon);

  // Wait up to 2 s for the daemon to bind its socket.
  for(int i = 0; i < 20; i++)
  {
    struct timespec ts = { .tv_nsec = 100000000 }; // 100 ms
    nanosleep(&ts, NULL);
    if(_connect_socket()) break;
  }

  if(_p2p.sockfd < 0)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[p2p] daemon started but command socket not ready");
    return;
  }

  dt_print(DT_DEBUG_IMAGEIO, "[p2p] connected to daemon (pid %d)", (int)_p2p.daemon_pid);

  // Start the event reader thread for push notifications.
  g_atomic_int_set(&_p2p_evt.stop, 0);
  _p2p_evt.thread = g_thread_new("p2p-events", _event_thread_func, NULL);
}

void dt_p2p_cleanup(void)
{
  // Stop event thread first.
  g_atomic_int_set(&_p2p_evt.stop, 1);
  if(_p2p_evt.sockfd >= 0)
    shutdown(_p2p_evt.sockfd, SHUT_RDWR);
  if(_p2p_evt.thread)
  {
    g_thread_join(_p2p_evt.thread);
    _p2p_evt.thread = NULL;
  }
  _p2p_evt.sockfd = -1;

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
