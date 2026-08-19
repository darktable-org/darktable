#define _POSIX_C_SOURCE 200809L

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
#include "common/database.h"
#include "common/film.h"
#include "common/history.h"
#include "common/image.h"
#include "common/exif.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
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
#include <stdlib.h>

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

// Echo-suppress: after we push XMP for a path, ignore the daemon's xmp_updated
// echo for that same path for 10 seconds.  Without this the echo triggers
// _xmp_reload_on_main_thread → dt_dev_reload_history_items →
// DT_SIGNAL_DEVELOP_HISTORY_CHANGE → another push 3 s later, ad infinitum.
static char   _p2p_push_echo_path[PATH_MAX] = { 0 };
static gint64 _p2p_push_echo_until_ms = 0;  // g_get_monotonic_time()/1000
static GMutex _p2p_push_echo_mu;

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

// Open a fresh socket, send one JSON command, read one newline-terminated JSON
// response, close, and return it.  Caller must g_free() the result.
// Returns NULL when the daemon is not running or the send/read fails.
static gchar *_p2p_request(const gchar *cmd_json)
{
  char sock_path[256];
  _socket_path(sock_path, sizeof(sock_path));

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if(fd < 0) return NULL;

  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  g_strlcpy(addr.sun_path, sock_path, sizeof(addr.sun_path));

  struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
  {
    close(fd);
    return NULL;
  }

  gchar *line = g_strdup_printf("%s\n", cmd_json);
  ssize_t len  = (ssize_t)strlen(line);
  ssize_t sent = send(fd, line, len, MSG_NOSIGNAL);
  g_free(line);

  if(sent != len)
  {
    close(fd);
    return NULL;
  }

  // Read until newline or EOF (response is one JSON line).
  GString *buf = g_string_new(NULL);
  char tmp[4096];
  ssize_t n;
  while((n = recv(fd, tmp, sizeof(tmp) - 1, 0)) > 0)
  {
    tmp[n] = '\0';
    g_string_append(buf, tmp);
    if(strchr(buf->str, '\n')) break;
  }
  close(fd);

  gchar *result = g_strdup(buf->str);
  g_string_free(buf, TRUE);
  return result;
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
// Push-event handling (image_imported, xmp_updated, generate_preview)
// ---------------------------------------------------------------------------

typedef struct _gen_preview_ctx_t
{
  dt_imgid_t imgid;
} _gen_preview_ctx_t;

// Background thread: generate DT_MIPMAP_1 via a blocking get, then evict to
// flush it from the in-memory LRU to the mipmap disk cache.  The desktop
// daemon's findMipmapJPEG looks for the disk file; PREFETCH alone is not enough
// because it only populates the in-memory cache (disk write happens on eviction).
static gpointer _generate_preview_thread(gpointer data)
{
  _gen_preview_ctx_t *ctx = data;
  const dt_imgid_t imgid = ctx->imgid;
  free(ctx);

  dt_mipmap_buffer_t mbuf;
  dt_mipmap_cache_get(&mbuf, imgid, DT_MIPMAP_1, DT_MIPMAP_BLOCKING, 'r');
  if(mbuf.buf)
    dt_mipmap_cache_release(&mbuf);

  // Evicting forces _mipmap_cache_deallocate_dynamic which writes the JPEG
  // to the disk cache at mipmaps-*.d/1/<imgid>.jpg where the daemon can find it.
  dt_mipmap_cache_evict_at_size(imgid, DT_MIPMAP_1);
  dt_mipmap_cache_evict_at_size(imgid, DT_MIPMAP_2);

  dt_print(DT_DEBUG_IMAGEIO, "[p2p] generate_preview: mipmap flushed to disk for id=%d", imgid);
  return NULL;
}

typedef struct _import_ctx_t
{
  char path[PATH_MAX];
} _import_ctx_t;

// Coalescing flag: 0 = no filmrolls signal pending, 1 = already queued.
// Prevents 30 rapid-fire DT_SIGNAL_FILMROLLS_CHANGED when a manifest syncs.
static gint _filmrolls_refresh_pending = 0;

// Main-thread callback: one-shot filmrolls refresh after a p2p import batch.
static gboolean _import_refresh_filmrolls_main_thread(gpointer data)
{
  (void)data;
  g_atomic_int_set(&_filmrolls_refresh_pending, 0);
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_FILMROLLS_CHANGED);
  return G_SOURCE_REMOVE;
}

// Main-thread callback: import one image into the darktable library.
//
// Running on the main thread (via g_main_context_invoke_full) serializes all
// dt_image_import calls through the GTK event loop, eliminating the data races
// that occur when the job queue runs multiple _import_image_job_run calls
// concurrently — those concurrent calls corrupt the image/mipmap caches that
// the main thread reads while rendering, causing SIGSEGV.
static gboolean _import_on_main_thread(gpointer data)
{
  _import_ctx_t *ctx = data;

  // Skip if neither the raw nor the .proxy.avif sidecar is present locally.
  // Avoids NFS/autofs stalls from probing non-existent paths.
  if(!g_file_test(ctx->path, G_FILE_TEST_EXISTS))
  {
    char proxy_path[PATH_MAX];
    g_snprintf(proxy_path, sizeof(proxy_path), "%s.proxy.avif", ctx->path);
    if(!g_file_test(proxy_path, G_FILE_TEST_EXISTS))
    {
      dt_print(DT_DEBUG_IMAGEIO, "[p2p] import skipped, file not found: '%s'", ctx->path);
      free(ctx);
      return G_SOURCE_REMOVE;
    }
  }

  char *dir = g_path_get_dirname(ctx->path);
  dt_film_t film;
  dt_film_init(&film);
  dt_filmid_t fid = dt_film_new(&film, dir);
  g_free(dir);

  if(dt_is_valid_filmid(fid))
  {
    // raise_signals=FALSE: we raise a coalesced DT_SIGNAL_FILMROLLS_CHANGED
    // via idle callback instead of one DT_SIGNAL_IMAGE_IMPORT per image.
    const dt_imgid_t imgid = dt_image_import(fid, ctx->path, TRUE, FALSE);
    if(dt_is_valid_imgid(imgid))
    {
      dt_print(DT_DEBUG_IMAGEIO, "[p2p] imported image id=%d '%s'", imgid, ctx->path);
      if(g_atomic_int_compare_and_exchange(&_filmrolls_refresh_pending, 0, 1))
        g_idle_add(_import_refresh_filmrolls_main_thread, NULL);
    }
    dt_film_cleanup(&film);
  }
  else
  {
    dt_print(DT_DEBUG_IMAGEIO, "[p2p] could not find/create film for '%s'", ctx->path);
  }

  free(ctx);
  return G_SOURCE_REMOVE;
}

static void _p2p_queue_import(const char *path)
{
  _import_ctx_t *ctx = malloc(sizeof(*ctx));
  if(!ctx) return;
  g_strlcpy(ctx->path, path, sizeof(ctx->path));
  // Run on the main thread at low priority so concurrent manifest syncs are
  // serialized through the GTK event loop rather than running simultaneously
  // in the job pool (concurrent dt_image_import calls corrupt shared caches).
  g_main_context_invoke_full(NULL, G_PRIORITY_LOW,
                             _import_on_main_thread, ctx, NULL);
}

// Runs on GTK main thread: reads XMP sidecar into SQLite then refreshes the
// UI.  Running on the main thread (which is single-threaded) prevents the
// concurrent-writer race that caused COMMIT→SQLITE_BUSY→assert when two XMP
// updates arrived simultaneously on background job threads.
static gboolean _xmp_reload_on_main_thread(gpointer data)
{
  _import_ctx_t *ctx = data;

  // Discard the daemon's echo of our own most-recent push.  Darktable is
  // already in the correct state (it wrote this XMP), so the reload is a
  // no-op that would needlessly re-arm the debounce and cause an edit loop.
  g_mutex_lock(&_p2p_push_echo_mu);
  const gboolean suppress =
      _p2p_push_echo_path[0] &&
      strcmp(ctx->path, _p2p_push_echo_path) == 0 &&
      g_get_monotonic_time() / 1000 < _p2p_push_echo_until_ms;
  g_mutex_unlock(&_p2p_push_echo_mu);
  if(suppress)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[p2p] xmp_updated echo suppressed for '%s'", ctx->path);
    free(ctx);
    return G_SOURCE_REMOVE;
  }

  const dt_imgid_t imgid = dt_image_get_id_full_path(ctx->path);
  if(dt_is_valid_imgid(imgid))
  {
    char xmp_path[PATH_MAX];
    g_snprintf(xmp_path, sizeof(xmp_path), "%s.xmp", ctx->path);

    // Write XMP history.  Release image lock before the UI refresh so
    // dt_dev_reload_history_items can re-acquire it without deadlocking.
    dt_lock_image(imgid);
    dt_image_t *img = dt_image_cache_get(imgid, 'w');
    if(img)
    {
      dt_exif_xmp_read(img, xmp_path, FALSE);
      dt_image_cache_write_release_info(img, DT_IMAGE_CACHE_SAFE, "p2p xmp reload");
    }
    dt_unlock_image(imgid);

    if(darktable.develop && dt_dev_is_current_image(darktable.develop, imgid))
      dt_dev_reload_history_items(darktable.develop);

    dt_mipmap_cache_remove(imgid);
    dt_image_update_final_size(imgid);
    dt_image_cache_set_change_timestamp(imgid);
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_MIPMAP_UPDATED, imgid);
    GList *_p2p_imgs = g_list_prepend(NULL, GINT_TO_POINTER(imgid));
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_IMAGE_INFO_CHANGED, _p2p_imgs);
    dt_print(DT_DEBUG_IMAGEIO, "[p2p] applied XMP for '%s' id=%d", ctx->path, imgid);
  }
  else
  {
    // Image not in library yet — import it.
    _p2p_queue_import(ctx->path);
  }

  free(ctx);
  return G_SOURCE_REMOVE;
}

static void _p2p_queue_xmp_reload(const char *path)
{
  _import_ctx_t *ctx = malloc(sizeof(*ctx));
  if(!ctx) return;
  g_strlcpy(ctx->path, path, sizeof(ctx->path));
  // Dispatch to the main thread so SQLite writes are serialized —
  // concurrent background writes caused COMMIT→SQLITE_BUSY→assert.
  g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT,
                             _xmp_reload_on_main_thread, ctx, NULL);
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

static gpointer _announce_existing_proxies(gpointer arg);  // defined below

// Background thread: maintains a persistent event connection to the daemon
// and dispatches push events to the GTK main thread.  Automatically reconnects
// when the daemon restarts (e.g. after a daemon update).
static gpointer _event_thread_func(gpointer user_data)
{
  (void)user_data;

  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  g_strlcpy(addr.sun_path, _p2p.socket_path, sizeof(addr.sun_path));

  while(!g_atomic_int_get(&_p2p_evt.stop))
  {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if(fd < 0) return NULL;

  if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
  {
    close(fd);
    // Daemon not yet ready; wait and retry.
    g_usleep(2 * G_USEC_PER_SEC);
    continue;
  }
  _p2p_evt.sockfd = fd;

  // Tell the daemon we want push events on this connection.
  const char *sub = "{\"type\":\"subscribe_events\"}\n";
  send(fd, sub, strlen(sub), MSG_NOSIGNAL);
  dt_print(DT_DEBUG_IMAGEIO, "[p2p] event thread: connected to daemon");

  // Use read() directly rather than fdopen()+fgets() to avoid holding a FILE
  // _IO_lock during a blocking read.  fgets holds the lock for the entire
  // duration of the kernel read; any concurrent fflush_all (e.g. from Lua IO)
  // tries to acquire that same lock, which deadlocks the Lua exec_lock and
  // freezes the GTK main thread.
  char buf[65536];
  int buflen = 0;

  while(!g_atomic_int_get(&_p2p_evt.stop))
  {
    ssize_t n = read(fd, buf + buflen, sizeof(buf) - 1 - buflen);
    if(n <= 0) break;
    buflen += n;

    char *start = buf;
    char *nl;
    while((nl = (char *)memchr(start, '\n', buflen - (int)(start - buf))) != NULL)
    {
      *nl = '\0';
      char *line = start;
      start = nl + 1;

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
      else if(strstr(line, "\"generate_preview\""))
      {
        const char *data = strstr(line, "\"data\"");
        if(!data) continue;
        char path[PATH_MAX];
        if(_extract_path(data, path, sizeof(path)))
        {
          const dt_imgid_t imgid = dt_image_get_id_full_path(path);
          if(dt_is_valid_imgid(imgid))
          {
            dt_print(DT_DEBUG_IMAGEIO, "[p2p] generate_preview for '%s' id=%d happened",
                     path, imgid);
            _gen_preview_ctx_t *ctx = malloc(sizeof(*ctx));
            if(ctx)
            {
              ctx->imgid = imgid;
              GThread *t = g_thread_try_new("p2p-gen-preview",
                                            _generate_preview_thread, ctx, NULL);
              if(t) g_thread_unref(t);
              else   free(ctx);
            }
          }
        }
      }
      else if(strstr(line, "\"request_announce\""))
      {
        // Daemon (re)started while we were running: re-announce all proxy
        // sidecars so the daemon's localIndex is repopulated.
        dt_print(DT_DEBUG_IMAGEIO, "[p2p] daemon requested re-announce");
        GThread *t = g_thread_try_new("p2p-reannounce",
                                      _announce_existing_proxies, (gpointer)1, NULL);
        if(t) g_thread_unref(t);
      }
    }

    // Shift remaining partial line to the front; discard if buffer is full.
    int remaining = buflen - (int)(start - buf);
    if(remaining > 0 && start != buf)
      memmove(buf, start, remaining);
    buflen = ((int)(sizeof(buf) - 1) == remaining) ? 0 : remaining;
  }

  // Inner read loop exited: connection dropped (daemon restarted or shut down).
  close(fd);
  _p2p_evt.sockfd = -1;
  if(!g_atomic_int_get(&_p2p_evt.stop))
  {
    dt_print(DT_DEBUG_IMAGEIO, "[p2p] event thread: connection lost, reconnecting in 2s");
    g_usleep(2 * G_USEC_PER_SEC);
  }
  } // outer reconnect loop

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

  // Write the XMP file first to ensure it contains latest changes.
  // If the raw lives on an unmounted remote filesystem, fall back to the p2p
  // proxy directory so the edit can still be pushed to peers.
  if(dt_exif_xmp_write(imgid, xmp_path, FALSE))
  {
    const char *proxy_dir = dt_conf_get_string_const("plugins/p2p/proxy_dir");
    if(!proxy_dir || !proxy_dir[0])
    {
      dt_print(DT_DEBUG_IMAGEIO, "[p2p] xmp write failed for '%s', no proxy dir configured", xmp_path);
      return 0;
    }
    gchar *base = g_path_get_basename(raw_path);
    g_snprintf(xmp_path, sizeof(xmp_path), "%s/%s.xmp", proxy_dir, base);
    g_free(base);
    if(dt_exif_xmp_write(imgid, xmp_path, FALSE))
    {
      dt_print(DT_DEBUG_IMAGEIO, "[p2p] xmp write also failed for proxy path '%s'", xmp_path);
      return 0;
    }
    dt_print(DT_DEBUG_IMAGEIO, "[p2p] xmp write fell back to proxy dir: '%s'", xmp_path);
  }

  dt_print(DT_DEBUG_IMAGEIO, "[p2p] pushing XMP for '%s' from image id=%d", raw_path, imgid);
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

  // Log the image ID for debugging purposes
  dt_print(DT_DEBUG_IMAGEIO, "[p2p] attempting to push XMP for image id=%d", imgid);

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
// Startup keepalive — fires every 100 ms from the GTK main loop for the
// first 10 seconds after p2p init.  Without this, the main loop can block
// indefinitely in epoll_wait after the thumbtable adds new child widgets
// inside a draw callback: the pending HIGH_IDLE resize sources exist but
// the loop never wakes to dispatch them.  The timer provides that wakeup.
// ---------------------------------------------------------------------------

static gint _p2p_keepalive_tick = 0;

static gboolean _p2p_keepalive(gpointer user_data)
{
  const gint n = g_atomic_int_add(&_p2p_keepalive_tick, 1) + 1;
  dt_print(DT_DEBUG_IMAGEIO, "[p2p] keepalive #%d", n);
  return (n < 100) ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

// Walk the darktable DB and announce every raw file that already has a
// .proxy.avif sidecar on disk so the daemon can serve it to peers.
// Pass (gpointer)1 as argument to skip the startup delay (re-announce path).
static gpointer _announce_existing_proxies(gpointer arg)
{
  // On first startup the daemon needs a moment to finish binding the socket.
  // On re-announce (daemon reconnect) we skip the delay — socket is ready.
  if(arg == NULL)
    g_usleep(2 * G_USEC_PER_SEC);

  // images.filename is the basename only; join film_rolls for the full path.
  sqlite3_stmt *stmt;
  const int rc = sqlite3_prepare_v2(dt_database_get(darktable.db),
    "SELECT r.folder || '/' || i.filename"
    "  FROM main.images i"
    "  JOIN main.film_rolls r ON i.film_id = r.id",
    -1, &stmt, NULL);
  if(rc != SQLITE_OK) return NULL;

  int count = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *path = (const char *)sqlite3_column_text(stmt, 0);
    if(!path) continue;

    char proxy_path[PATH_MAX];
    g_snprintf(proxy_path, sizeof(proxy_path), "%s.proxy.avif", path);
    if(g_file_test(proxy_path, G_FILE_TEST_IS_REGULAR))
    {
      dt_p2p_announce_proxy(path);
      count++;
    }
  }
  sqlite3_finalize(stmt);

  if(count > 0)
    dt_print(DT_DEBUG_IMAGEIO, "[p2p] announced %d existing proxies to daemon", count);
  return NULL;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void dt_p2p_init(void)
{
  g_mutex_init(&_p2p.lock);
  g_mutex_init(&_p2p_push_echo_mu);
  _socket_path(_p2p.socket_path, sizeof(_p2p.socket_path));

  if(!dt_conf_get_bool("plugins/p2p/enabled"))
  {
    dt_print(DT_DEBUG_IMAGEIO, "[p2p] disabled — enable in preferences to start daemon");
    return;
  }

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
    g_timeout_add(100, _p2p_keepalive, NULL);
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

  // Announce any proxy sidecars that already exist on disk.
  g_thread_new("p2p-proxy-announce", _announce_existing_proxies, NULL);

  // Push XMP to peers within 3 s of any history change.
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_HISTORY_CHANGE, _p2p_on_history_change, NULL);
  g_timeout_add(100, _p2p_keepalive, NULL);
}

void dt_p2p_cleanup(void)
{
  g_mutex_clear(&_p2p_push_echo_mu);

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
  const gboolean sent = _connect_socket() && _send_json(json);
  g_mutex_unlock(&_p2p.lock);

  if(sent)
  {
    // Record the push so the daemon's echo is ignored (see _xmp_reload_on_main_thread).
    g_mutex_lock(&_p2p_push_echo_mu);
    g_strlcpy(_p2p_push_echo_path, raw_path, sizeof(_p2p_push_echo_path));
    _p2p_push_echo_until_ms = g_get_monotonic_time() / 1000 + 10000; // 10 s
    g_mutex_unlock(&_p2p_push_echo_mu);
  }

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

void dt_p2p_announce_proxy(const char *raw_path)
{
  if(!raw_path) return;

  GString *ep = g_string_new(NULL);
  for(const char *p = raw_path; *p; p++)
  {
    if(*p == '"' || *p == '\\') g_string_append_c(ep, '\\');
    g_string_append_c(ep, *p);
  }

  char *json = g_strdup_printf(
    "{\"type\":\"announce_proxy\",\"data\":{\"path\":\"%s\"}}", ep->str);

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

void dt_p2p_accept_peer(const char *fingerprint)
{
  if(!fingerprint || !fingerprint[0]) return;

  GString *ep = g_string_new(NULL);
  for(const char *p = fingerprint; *p; p++)
  {
    if(*p == '"' || *p == '\\') g_string_append_c(ep, '\\');
    g_string_append_c(ep, *p);
  }

  char *json = g_strdup_printf(
    "{\"type\":\"accept_peer\",\"data\":{\"fingerprint\":\"%s\"}}", ep->str);

  g_mutex_lock(&_p2p.lock);
  if(_connect_socket())
    _send_json(json);
  g_mutex_unlock(&_p2p.lock);

  g_free(json);
  g_string_free(ep, TRUE);
}

gchar *dt_p2p_query_peer_status(void)
{
  return _p2p_request("{\"type\":\"list_peer_status\",\"data\":null}");
}

void dt_p2p_restart(void)
{
  dt_p2p_cleanup();
  dt_p2p_init();
}
