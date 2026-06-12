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
#include "control/jobs.h"
#include "control/signal.h"
#include "develop/develop.h"
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

// Background job: imports a remote image into the darktable library.
static int32_t _import_image_job_run(dt_job_t *job)
{
  _import_ctx_t *ctx = dt_control_job_get_params(job);

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

  return 0;
}

// Background job: reloads the XMP sidecar for an already-imported image.
// Running this as a job (not a GTK idle) avoids blocking the main thread on
// the image-cache write lock, which can deadlock with background jobs that
// hold a read lock on the same image at startup.
static int32_t _xmp_reload_job_run(dt_job_t *job)
{
  _import_ctx_t *ctx = dt_control_job_get_params(job);

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
    _import_image_job_run(job);
  }

  return 0;
}

static void _p2p_queue_import(const char *path)
{
  _import_ctx_t *ctx = malloc(sizeof(*ctx));
  if(!ctx) return;
  g_strlcpy(ctx->path, path, sizeof(ctx->path));
  dt_job_t *job = dt_control_job_create(&_import_image_job_run, "p2p import");
  if(!job) { free(ctx); return; }
  dt_control_job_set_params(job, ctx, free);
  dt_control_add_job(DT_JOB_QUEUE_USER_BG, job);
}

static void _p2p_queue_xmp_reload(const char *path)
{
  _import_ctx_t *ctx = malloc(sizeof(*ctx));
  if(!ctx) return;
  g_strlcpy(ctx->path, path, sizeof(ctx->path));
  dt_job_t *job = dt_control_job_create(&_xmp_reload_job_run, "p2p xmp reload");
  if(!job) { free(ctx); return; }
  dt_control_job_set_params(job, ctx, free);
  dt_control_add_job(DT_JOB_QUEUE_USER_BG, job);
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
      char path[PATH_MAX];
      if(_extract_path(data, path, sizeof(path)))
      {
        dt_print(DT_DEBUG_IMAGEIO, "[p2p] received image_imported: %s", path);
        _p2p_queue_import(path);
      }
    }
    else if(strstr(line, "\"xmp_updated\""))
    {
      const char *data = strstr(line, "\"data\"");
      if(!data) continue;
      char path[PATH_MAX];
      if(_extract_path(data, path, sizeof(path)))
      {
        dt_print(DT_DEBUG_IMAGEIO, "[p2p] received xmp_updated: %s", path);
        _p2p_queue_xmp_reload(path);
      }
    }
  }

  fclose(f);
  _p2p_evt.sockfd = -1;
  return NULL;
}

// ---------------------------------------------------------------------------
// History-change hook: debounced XMP sync triggered by module edits
// ---------------------------------------------------------------------------

// Timer source ID for the debounce; 0 when no timer is pending.
static guint _p2p_debounce_src = 0;

typedef struct
{
  dt_imgid_t imgid;
} _write_push_ctx_t;

// Background job: resolve paths, write XMP locally, push to network.
// Runs entirely off the GTK main thread so no lock on the image cache
// can ever block the UI.
static int32_t _p2p_write_and_push_job_run(dt_job_t *job)
{
  _write_push_ctx_t *ctx = dt_control_job_get_params(job);
  const dt_imgid_t imgid = ctx->imgid;

  if(!dt_is_valid_imgid(imgid)) return 0;

  char raw_path[PATH_MAX] = { 0 };
  gboolean from_cache = FALSE;
  dt_image_full_path(imgid, raw_path, sizeof(raw_path), &from_cache);
  if(!raw_path[0]) return 0;

  char xmp_path[PATH_MAX];
  g_strlcpy(xmp_path, raw_path, sizeof(xmp_path));
  dt_image_path_append_version(imgid, xmp_path, sizeof(xmp_path));
  g_strlcat(xmp_path, ".xmp", sizeof(xmp_path));

  if(dt_exif_xmp_write(imgid, xmp_path, FALSE)) return 0;

  dt_p2p_push_xmp(raw_path, xmp_path);
  return 0;
}

// GLib timeout callback: fires 3 s after the last history change.
// Does no I/O — only queues a background job so the main thread
// is never blocked by image-cache locks or socket I/O.
static gboolean _p2p_debounce_fire(gpointer user_data)
{
  _p2p_debounce_src = 0;
  if(!darktable.develop) return G_SOURCE_REMOVE;

  const dt_imgid_t imgid = darktable.develop->image_storage.id;
  if(!dt_is_valid_imgid(imgid)) return G_SOURCE_REMOVE;

  _write_push_ctx_t *ctx = malloc(sizeof(*ctx));
  if(!ctx) return G_SOURCE_REMOVE;
  ctx->imgid = imgid;

  dt_job_t *job = dt_control_job_create(&_p2p_write_and_push_job_run, "p2p xmp write+push");
  if(!job) { free(ctx); return G_SOURCE_REMOVE; }
  dt_control_job_set_params(job, ctx, free);
  dt_control_add_job(DT_JOB_QUEUE_USER_BG, job);
  return G_SOURCE_REMOVE;
}

// Signal callback: reset the debounce timer on every history change.
static void _p2p_on_history_change(gpointer user_data)
{
  if(!dt_p2p_is_running()) return;
  if(_p2p_debounce_src) g_source_remove(_p2p_debounce_src);
  _p2p_debounce_src = g_timeout_add(3000, _p2p_debounce_fire, NULL);
}

// ---------------------------------------------------------------------------
// Startup heartbeat — fires every 100 ms from the GTK main loop so we can
// tell from the log exactly when (and whether) the main loop freezes.
// Stops automatically after 50 ticks (5 seconds).
// ---------------------------------------------------------------------------

static gint _p2p_heartbeat_tick = 0;

static gboolean _p2p_heartbeat(gpointer user_data)
{
  const gint n = g_atomic_int_add(&_p2p_heartbeat_tick, 1) + 1;
  dt_print(DT_DEBUG_IMAGEIO, "[p2p] main-loop heartbeat #%d", n);
  return (n < 50) ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void dt_p2p_init(void)
{
  g_mutex_init(&_p2p.lock);
  _socket_path(_p2p.socket_path, sizeof(_p2p.socket_path));

  const char *cfg_dir = g_get_user_config_dir();
  char pw_file[PATH_MAX];
  g_snprintf(pw_file, sizeof(pw_file), "%s/darktable/peer.pw", cfg_dir);

  const char *passphrase = dt_conf_get_string_const("plugins/p2p/passphrase");
  if(!passphrase || passphrase[0] == '\0')
  {
    // Try peer.pw first.
    gchar *file_pw = NULL;
    if(g_file_get_contents(pw_file, &file_pw, NULL, NULL) && file_pw && file_pw[0])
    {
      // Strip trailing whitespace so editors don't break things.
      g_strchomp(file_pw);
      dt_conf_set_string("plugins/p2p/passphrase", file_pw);
      passphrase = dt_conf_get_string_const("plugins/p2p/passphrase");
      g_free(file_pw);
      dt_print(DT_DEBUG_IMAGEIO, "[p2p] loaded passphrase from %s", pw_file);
    }
    else
    {
      g_free(file_pw);
      // Fall back: auto-start when peers.txt or peers.db exists.
      char peers_txt[PATH_MAX], peers_db[PATH_MAX];
      g_snprintf(peers_txt, sizeof(peers_txt), "%s/darktable/peers.txt", cfg_dir);
      g_snprintf(peers_db,  sizeof(peers_db),  "%s/darktable/peers.db",  cfg_dir);
      if(!g_file_test(peers_txt, G_FILE_TEST_EXISTS)
         && !g_file_test(peers_db, G_FILE_TEST_EXISTS))
        return;

      // Generate a random passphrase, save it to peer.pw and to config so the
      // peer identity stays stable across restarts.
      guint8 rnd[24];
      for(int i = 0; i < (int)sizeof(rnd); i++)
        rnd[i] = (guint8)g_random_int_range(0, 256);
      gchar *auto_pass = g_base64_encode(rnd, sizeof(rnd));
      g_file_set_contents(pw_file, auto_pass, -1, NULL);
      dt_conf_set_string("plugins/p2p/passphrase", auto_pass);
      passphrase = dt_conf_get_string_const("plugins/p2p/passphrase");
      g_free(auto_pass);
      dt_print(DT_DEBUG_IMAGEIO, "[p2p] auto-generated passphrase → %s", pw_file);
    }
  }

  // If a daemon is already listening at the socket (e.g. started manually
  // for debugging), skip spawning and just attach to it.
  if(_connect_socket())
  {
    dt_print(DT_DEBUG_IMAGEIO, "[p2p] attached to existing daemon at %s", _p2p.socket_path);
    g_atomic_int_set(&_p2p_evt.stop, 0);
    _p2p_evt.thread = g_thread_new("p2p-events", _event_thread_func, NULL);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_HISTORY_CHANGE, _p2p_on_history_change, NULL);
    g_timeout_add(100, _p2p_heartbeat, NULL);
    return;
  }

  // Resolve the real executable path so we find dt-p2p-daemon even when
  // darktable is invoked by name without a full path.
  char exe_buf[PATH_MAX] = { 0 };
  ssize_t exe_len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
  char *exe_dir;
  if(exe_len > 0)
    exe_dir = g_path_get_dirname(exe_buf);
  else
    exe_dir = g_path_get_dirname(g_get_prgname());

  char *daemon = g_build_filename(exe_dir, "dt-p2p-daemon", NULL);
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

  const char *import_dir = dt_conf_get_string_const("plugins/p2p/import_dir");
  if(import_dir && import_dir[0])
  {
    g_ptr_array_add(argv_arr, (gchar *)"--import-dir");
    g_ptr_array_add(argv_arr, (gchar *)import_dir);
  }

  g_ptr_array_add(argv_arr, NULL);

  GError *err = NULL;
  if(!g_spawn_async(NULL, (gchar **)argv_arr->pdata, NULL,
                    G_SPAWN_DO_NOT_REAP_CHILD,
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

  // Push XMP to peers within 3 s of any history change.
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_HISTORY_CHANGE, _p2p_on_history_change, NULL);
  g_timeout_add(100, _p2p_heartbeat, NULL);
}

void dt_p2p_cleanup(void)
{
  // Cancel any pending debounce timer.
  if(_p2p_debounce_src)
  {
    g_source_remove(_p2p_debounce_src);
    _p2p_debounce_src = 0;
  }
  DT_CONTROL_SIGNAL_DISCONNECT(_p2p_on_history_change, NULL);

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
