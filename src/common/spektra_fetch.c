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

#include "common/spektra_fetch.h"

#include "common/spektra_sim.h"

#include "common/curl_tools.h"
#include "common/darktable.h"
#include "common/file_location.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"

#include <curl/curl.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* configuration                                                          */
/* ---------------------------------------------------------------------- */

#define CONF_REPOSITORY "plugins/darkroom/spektrafilm/repository"
#define CONF_REF "plugins/darkroom/spektrafilm/ref"

/* Owner/repo holding the exported packs, and the git ref to read them at.
   Both are overridable so a packager can point at a mirror and a developer can
   test an unreleased pack without touching the source.

   The manifest and the files it lists are fetched in one pass, so a ref that
   moves underneath a running download does not install mismatched data: the
   per-file checksum catches it and the install is discarded. Pointing this at
   a tag is still worth doing once the data repository cuts one, for a
   different reason -- an immutable ref is what makes an old edit reproducible,
   where a branch hands whatever is current to a darktable that may predate
   it. */
#define SF_DEFAULT_REPOSITORY "darktable-org/darktable-spektrafilm"
#define SF_DEFAULT_REF "main"

/* Files are read straight out of the repository tree over HTTPS. This keeps the
   directory layout of the pack intact -- release assets live in a flat
   namespace and would force profiles/kodak_portra_400.json to be renamed to
   something like profiles-kodak_portra_400.json on the way up and back down. */
#define SF_RAW_HOST "https://raw.githubusercontent.com"

/* Bounds on anything the manifest can ask us to do. A manifest is remote input:
   it decides how many files we create and how many bytes we write, so it needs
   a ceiling that does not depend on it being well-formed. The spectral LUT is
   the only large file at roughly 12 MB; the rest are JSON in the tens of KB. */
#define SF_MAX_FILES 512
#define SF_MAX_FILE_BYTES (64u * 1024u * 1024u)
#define SF_MAX_TOTAL_BYTES (256u * 1024u * 1024u)
#define SF_MAX_MANIFEST_BYTES (4u * 1024u * 1024u)

/* Long enough for a 12 MB file on a slow link, short enough that a dead host
   does not leave the thread parked forever. Connect is separate and tight. */
#define SF_CONNECT_TIMEOUT 10L
#define SF_TRANSFER_TIMEOUT 600L

/* ---------------------------------------------------------------------- */
/* module state                                                           */
/* ---------------------------------------------------------------------- */

typedef struct sf_fetch_file_t
{
  char *path;   /* pack-relative, validated: no absolute paths, no ".." */
  char *sha256; /* lowercase hex, 64 chars */
  guint64 size; /* advertised; enforced as a ceiling while downloading */
} sf_fetch_file_t;

static struct
{
  GMutex lock;      /* guards every field below */
  GThread *thread;  /* non-NULL while a fetch is in flight */
  sf_fetch_state_t state;
  gboolean cancel;
  double progress;
  char message[256];
  guint generation; /* bumped whenever an install changes what is on disk */
  gboolean inited;
} _sf = { .state = SF_FETCH_IDLE };

guint sf_fetch_generation(void)
{
  if(!_sf.inited) return 0;
  g_mutex_lock(&_sf.lock);
  const guint g = _sf.generation;
  g_mutex_unlock(&_sf.lock);
  return g;
}

static void _set_status(const sf_fetch_state_t state,
                        const double progress,
                        const char *msg)
{
  g_mutex_lock(&_sf.lock);
  _sf.state = state;
  if(progress >= 0.0) _sf.progress = CLAMP(progress, 0.0, 1.0);
  if(msg) g_strlcpy(_sf.message, msg, sizeof(_sf.message));
  g_mutex_unlock(&_sf.lock);
}

static gboolean _cancelled(void)
{
  g_mutex_lock(&_sf.lock);
  const gboolean c = _sf.cancel;
  g_mutex_unlock(&_sf.lock);
  return c;
}

void sf_fetch_init(void)
{
  if(_sf.inited) return;
  g_mutex_init(&_sf.lock);
  _sf.state = SF_FETCH_IDLE;
  _sf.inited = TRUE;
}

void sf_fetch_cleanup(void)
{
  if(!_sf.inited) return;
  sf_fetch_cancel();
  /* Join rather than detach: the worker writes into _sf and into the pack
     directory, and darktable is on its way down. Letting it run past the mutex
     being cleared is a use-after-free waiting for a slow link to time out. */
  GThread *t = NULL;
  g_mutex_lock(&_sf.lock);
  t = _sf.thread;
  _sf.thread = NULL;
  g_mutex_unlock(&_sf.lock);
  if(t) g_thread_join(t);
  g_mutex_clear(&_sf.lock);
  _sf.inited = FALSE;
}

sf_fetch_state_t sf_fetch_status(char *msg,
                                 size_t msgsz,
                                 double *progress)
{
  g_mutex_lock(&_sf.lock);
  const sf_fetch_state_t s = _sf.state;
  if(msg && msgsz) g_strlcpy(msg, _sf.message, msgsz);
  if(progress) *progress = _sf.progress;
  g_mutex_unlock(&_sf.lock);
  return s;
}

void sf_fetch_cancel(void)
{
  g_mutex_lock(&_sf.lock);
  _sf.cancel = TRUE;
  g_mutex_unlock(&_sf.lock);
}

/* ---------------------------------------------------------------------- */
/* local pack discovery                                                   */
/* ---------------------------------------------------------------------- */

/* <user data>/darktable/spektrafilm -- the folder a user can put a pack in by
   hand, and where nothing will overwrite it: downloads go one level deeper, in
   `packs`, so installing a pack here pins it and takes it out of the fetcher's
   hands entirely. That is what makes it the right place for a pack built from a
   spektrafilm checkout, or edited, or kept for an old release.

   The shared data directory rather than the configuration one, because every
   darktable instance on the machine reads the same place: a configuration
   directory can be given per instance, and a pack duplicated per instance is a
   12 MB spectral table stored and fetched twice for nothing. It sits beside the
   AI models in <user data>/darktable/models for the same reason. */
static void _data_pack_dir(char *dst,
                           size_t dstsz)
{
  char *dir = g_build_filename(g_get_user_data_dir(), "darktable", "spektrafilm", NULL);
  g_strlcpy(dst, dir, dstsz);
  g_free(dir);
}

/* <user data>/darktable/spektrafilm/packs -- everything the fetcher installs,
   one subdirectory per spectral table, named for its hash. Kept a level below
   the hand-install folder so the two can never collide: the fetcher owns this
   directory and nothing else writes to it, which is what lets a user drop a
   pack in the folder above and know it will be left alone.

   Not the cache directory, even though these are downloads. A pack cannot be
   rebuilt from anything on the machine, and clearing the cache -- which users
   and packagers do freely -- would quietly leave every edit made against an
   older spectral table unreproducible until it was fetched again. Sharing a
   root with the hand-install folder also keeps the download temp directory on
   the filesystem it is renamed into, so installing stays atomic; across two
   roots that can be two mounts, where rename() fails with EXDEV. */
static void _packs_dir(char *dst,
                       size_t dstsz)
{
  char *dir = g_build_filename(g_get_user_data_dir(), "darktable", "spektrafilm",
                               "packs", NULL);
  g_strlcpy(dst, dir, dstsz);
  g_free(dir);
}

/* Read the identity out of a pack's spectra_lut.f32 without loading it.
 *
 * The header is fixed-width up to the id string: "SFS2", int32 header version,
 * int32 dims[3], int32 dtype, uint32 lut_hash, int32 id_len. Reading those 32
 * bytes answers "is this the pack that edit wants" for the price of one open,
 * where sf_pack_load() would pull roughly 12 MB of floats through the page
 * cache to answer the same question. That matters because the answer is needed
 * once per candidate directory, on the pixelpipe thread. */
static gboolean _peek_lut_hash(const char *packdir,
                               uint32_t *out_hash)
{
  gboolean ok = FALSE;
  char *lut = g_build_filename(packdir, "spectra_lut.f32", NULL);
  char *meta = g_build_filename(packdir, "pack.json", NULL);
  char *profiles = g_build_filename(packdir, "profiles", NULL);

  /* All three have to be there. A LUT and a pack.json with no profiles beside
     them is not a usable pack -- it is a spectral table with no film to apply
     it to, which is what a download looks like partway through and what a
     half-deleted directory looks like afterwards. Accepting it would mean
     resolving to a directory that then fails at profile load, which reads as
     the module being broken rather than the data being absent. */
  if(!g_file_test(meta, G_FILE_TEST_IS_REGULAR)) goto out;
  if(!g_file_test(profiles, G_FILE_TEST_IS_DIR)) goto out;

  FILE *fh = g_fopen(lut, "rb");
  if(!fh) goto out;

  char magic[4];
  int32_t hdr_version = 0, dims[3], dtype = 0;
  uint32_t lut_hash = 0;
  if(fread(magic, 1, 4, fh) == 4 && memcmp(magic, "SFS2", 4) == 0
     && fread(&hdr_version, 4, 1, fh) == 1 && hdr_version == 2
     && fread(dims, 4, 3, fh) == 3 && fread(&dtype, 4, 1, fh) == 1
     && fread(&lut_hash, 4, 1, fh) == 1)
  {
    *out_hash = lut_hash;
    ok = TRUE;
  }
  fclose(fh);

out:
  g_free(lut);
  g_free(meta);
  g_free(profiles);
  return ok;
}

static gboolean _downloaded_dir_for_hash(const uint32_t lut_hash,
                                         char *dst,
                                         size_t dstsz)
{
  char packs[PATH_MAX] = { 0 };
  _packs_dir(packs, sizeof(packs));
  g_snprintf(dst, dstsz, "%s%s%08x", packs, G_DIR_SEPARATOR_S, lut_hash);
  uint32_t got = 0;
  /* Trust the header, not the directory name. A user can copy a directory to
     the wrong name and a truncated download can leave a plausible-looking
     tree behind; both would otherwise be served as a match. */
  return _peek_lut_hash(dst, &got) && got == lut_hash;
}

gboolean sf_fetch_have_lut_hash(const uint32_t lut_hash)
{
  if(!lut_hash) return FALSE;

  char handdir[PATH_MAX] = { 0 };
  _data_pack_dir(handdir, sizeof(handdir));
  uint32_t got = 0;
  if(_peek_lut_hash(handdir, &got) && got == lut_hash) return TRUE;

  char dldir[PATH_MAX] = { 0 };
  return _downloaded_dir_for_hash(lut_hash, dldir, sizeof(dldir));
}

gboolean sf_fetch_resolve_pack_dir(const uint32_t wanted_lut_hash,
                                   char *dst,
                                   const size_t dstsz,
                                   gboolean *out_exact)
{
  if(out_exact) *out_exact = FALSE;

  char handdir[PATH_MAX] = { 0 };
  _data_pack_dir(handdir, sizeof(handdir));
  uint32_t hand_hash = 0;
  const gboolean hand_ok = _peek_lut_hash(handdir, &hand_hash);

  /* No recorded preference: whatever is installed by hand is the answer. This
     is the common path -- a fresh edit on a machine with a pack in place must
     not consult the network or the downloaded packs at all. */
  if(!wanted_lut_hash)
  {
    if(hand_ok)
    {
      g_strlcpy(dst, handdir, dstsz);
      if(out_exact) *out_exact = TRUE;
      return TRUE;
    }
  }
  else if(hand_ok && hand_hash == wanted_lut_hash)
  {
    g_strlcpy(dst, handdir, dstsz);
    if(out_exact) *out_exact = TRUE;
    return TRUE;
  }

  /* A specific table was asked for and the hand-installed pack cannot supply it.
     Look for a downloaded one. */
  if(wanted_lut_hash)
  {
    char cachedir[PATH_MAX] = { 0 };
    if(_downloaded_dir_for_hash(wanted_lut_hash, cachedir, sizeof(cachedir)))
    {
      g_strlcpy(dst, cachedir, dstsz);
      if(out_exact) *out_exact = TRUE;
      return TRUE;
    }
  }

  /* Nothing matches. Fall back to the config pack if there is one: rendering
     with the wrong spectral table and a visible warning beats refusing to
     render, and the module already words that warning precisely. */
  if(hand_ok)
  {
    g_strlcpy(dst, handdir, dstsz);
    return TRUE;
  }

  /* Last resort: any downloaded pack, newest first. Reached when the user has
     never installed one by hand and is opening an edit whose table was never
     downloaded, but some other table was. */
  char packs[PATH_MAX] = { 0 };
  _packs_dir(packs, sizeof(packs));
  GDir *d = g_dir_open(packs, 0, NULL);
  if(d)
  {
    const char *ent = NULL;
    char best[PATH_MAX] = { 0 };
    gint64 best_mtime = -1;
    while((ent = g_dir_read_name(d)))
    {
      /* Only ever consider a directory named for the table it claims to hold,
         and only when the LUT header agrees with that name.

         This is what keeps a download in progress invisible. The temp
         directory is a sibling of the finished ones -- it has to be, so the
         install rename stays on one filesystem and stays atomic -- and
         pack.json and spectra_lut.f32 are the first two files fetched. From
         that moment until the last profile lands, the temp directory looks
         like a loadable pack to anything that just peeks at the header. A
         reader picking it up would get a spectral table with no film profiles
         behind it. Requiring a bare 8-hex-digit name excludes it by
         construction, since it is named ".incoming-<hash>" precisely so it
         cannot pass. */
      if(strlen(ent) != 8 || strspn(ent, "0123456789abcdefABCDEF") != 8) continue;

      char *cand = g_build_filename(packs, ent, NULL);
      uint32_t h = 0;
      const uint32_t named = (uint32_t)g_ascii_strtoull(ent, NULL, 16);
      if(_peek_lut_hash(cand, &h) && h == named)
      {
        GStatBuf st;
        const gint64 mt = (g_stat(cand, &st) == 0) ? (gint64)st.st_mtime : 0;
        if(mt > best_mtime)
        {
          best_mtime = mt;
          g_strlcpy(best, cand, sizeof(best));
        }
      }
      g_free(cand);
    }
    g_dir_close(d);
    if(best[0])
    {
      g_strlcpy(dst, best, dstsz);
      return TRUE;
    }
  }

  return FALSE;
}

/* ---------------------------------------------------------------------- */
/* http                                                                   */
/* ---------------------------------------------------------------------- */

typedef struct sf_buf_t
{
  char *data;
  size_t len;
  size_t cap;
} sf_buf_t;

static size_t _write_to_buf(void *ptr,
                            size_t size,
                            size_t nmemb,
                            void *userdata)
{
  sf_buf_t *b = (sf_buf_t *)userdata;
  const size_t n = size * nmemb;
  if(b->len + n > b->cap) return 0; /* refuse rather than grow without bound */
  memcpy(b->data + b->len, ptr, n);
  b->len += n;
  return n;
}

typedef struct sf_dl_t
{
  FILE *fh;
  guint64 written;
  guint64 limit;      /* hard ceiling for this one file */
  guint64 done_bytes; /* bytes finished before this file, for overall progress */
  guint64 total_bytes;
} sf_dl_t;

static size_t _write_to_file(void *ptr,
                             size_t size,
                             size_t nmemb,
                             void *userdata)
{
  sf_dl_t *d = (sf_dl_t *)userdata;
  const size_t n = size * nmemb;
  if(d->written + n > d->limit) return 0; /* server sent more than advertised */
  const size_t w = fwrite(ptr, 1, n, d->fh);
  d->written += w;
  return w;
}

static int _progress_cb(void *clientp,
                        curl_off_t dltotal,
                        curl_off_t dlnow,
                        curl_off_t ultotal,
                        curl_off_t ulnow)
{
  (void)dltotal;
  (void)ultotal;
  (void)ulnow;
  if(_cancelled()) return 1; /* aborts the transfer */

  const sf_dl_t *d = (const sf_dl_t *)clientp;
  if(d && d->total_bytes)
  {
    const double got = (double)(d->done_bytes + (guint64)dlnow);
    _set_status(SF_FETCH_RUNNING, got / (double)d->total_bytes, NULL);
  }
  return 0;
}

static void _curl_common(CURL *curl,
                         const char *url)
{
  dt_curl_init(curl, FALSE);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  /* Redirects are expected -- the raw host hands off to a CDN -- but they must
     stay on https, or a hijacked redirect could downgrade the transfer.
     CURLOPT_REDIR_PROTOCOLS_STR arrived in 7.85; darktable's floor is 7.56, so
     fall back to the deprecated bitmask below that. */
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, SF_CONNECT_TIMEOUT);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, SF_TRANSFER_TIMEOUT);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "darktable-spektrafilm");
}

/* GET a small document into memory. Returns a NUL-terminated string the caller
   frees, or NULL. */
static char *_http_get_string(CURL *curl,
                              const char *url,
                              const size_t maxlen)
{
  sf_buf_t buf = { .data = g_malloc0(maxlen + 1), .len = 0, .cap = maxlen };

  curl_easy_reset(curl);
  _curl_common(curl, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _write_to_buf);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

  const CURLcode res = curl_easy_perform(curl);
  if(res != CURLE_OK)
  {
    dt_print(DT_DEBUG_DEV, "[spektrafilm] GET %s failed: %s", url,
             curl_easy_strerror(res));
    g_free(buf.data);
    return NULL;
  }
  buf.data[buf.len] = 0;
  return buf.data;
}

static gboolean _http_get_file(CURL *curl,
                               const char *url,
                               const char *path,
                               sf_dl_t *dl)
{
  FILE *fh = g_fopen(path, "wb");
  if(!fh)
  {
    dt_print(DT_DEBUG_ALWAYS, "[spektrafilm] cannot write %s: %s", path,
             strerror(errno));
    return FALSE;
  }
  dl->fh = fh;
  dl->written = 0;

  curl_easy_reset(curl);
  _curl_common(curl, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _write_to_file);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, dl);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, _progress_cb);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, dl);

  const CURLcode res = curl_easy_perform(curl);
  fclose(fh);
  dl->fh = NULL;

  if(res != CURLE_OK)
  {
    dt_print(DT_DEBUG_DEV, "[spektrafilm] GET %s failed: %s", url,
             curl_easy_strerror(res));
    g_unlink(path);
    return FALSE;
  }
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* integrity                                                              */
/* ---------------------------------------------------------------------- */

static char *_sha256_file(const char *path)
{
  FILE *fh = g_fopen(path, "rb");
  if(!fh) return NULL;

  GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
  guchar buf[64 * 1024];
  size_t n;
  while((n = fread(buf, 1, sizeof(buf), fh)) > 0) g_checksum_update(sum, buf, n);
  fclose(fh);

  char *hex = g_ascii_strdown(g_checksum_get_string(sum), -1);
  g_checksum_free(sum);
  return hex;
}

static gboolean _verify(const char *path,
                        const char *expected_hex)
{
  char *got = _sha256_file(path);
  if(!got) return FALSE;
  const gboolean ok = (g_ascii_strcasecmp(got, expected_hex) == 0);
  if(!ok)
    dt_print(DT_DEBUG_ALWAYS,
             "[spektrafilm] checksum mismatch for %s (expected %s, got %s)", path,
             expected_hex, got);
  g_free(got);
  return ok;
}

/* ---------------------------------------------------------------------- */
/* manifest                                                               */
/* ---------------------------------------------------------------------- */

static gboolean _valid_repository(const char *repo)
{
  return repo
         && g_regex_match_simple("^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$", repo, 0, 0);
}

static gboolean _valid_ref(const char *ref)
{
  return ref && g_regex_match_simple("^[A-Za-z0-9._/-]{1,128}$", ref, 0, 0)
         && !strstr(ref, "..");
}

/* Every path in the manifest becomes a file we create. The manifest is remote
   input, so a path that escapes the destination directory is a write-anywhere
   primitive -- reject rather than sanitise, so a manifest that tries it fails
   loudly instead of being quietly rewritten into something that works. */
static gboolean _valid_relpath(const char *p)
{
  if(!p || !*p) return FALSE;
  if(strlen(p) > 255) return FALSE;
  if(g_path_is_absolute(p)) return FALSE;
  if(p[0] == '/' || p[0] == '\\' || p[0] == '.') return FALSE;
  if(strstr(p, "..")) return FALSE;
  if(strchr(p, '\\')) return FALSE;
  if(strchr(p, ':')) return FALSE; /* drive letters and NTFS streams */
  /* one optional subdirectory (profiles/), nothing deeper */
  const char *slash = strchr(p, '/');
  if(slash && strchr(slash + 1, '/')) return FALSE;
  return g_regex_match_simple("^[A-Za-z0-9._/-]+$", p, 0, 0);
}

static gboolean _valid_sha256(const char *s)
{
  return s && strlen(s) == 64
         && g_regex_match_simple("^[A-Fa-f0-9]{64}$", s, 0, 0);
}

static void _files_free(GPtrArray *files)
{
  if(!files) return;
  for(guint i = 0; i < files->len; i++)
  {
    sf_fetch_file_t *f = g_ptr_array_index(files, i);
    g_free(f->path);
    g_free(f->sha256);
    g_free(f);
  }
  g_ptr_array_free(files, TRUE);
}

/* Parse the manifest and pull out the requested pack.
 *
 * Shape:
 *   { "format": 1,
 *     "packs": [ { "lut_hash": "a1b2c3d4", "lut_id": "...", "default": true,
 *                  "base": "packs/2026-07",
 *                  "files": [ {"path": "pack.json", "size": 1234,
 *                              "sha256": "..."} ] } ] }
 *
 * wanted 0 picks the entry flagged default, else the first one. */
static GPtrArray *_parse_manifest(const char *json,
                                  const uint32_t wanted,
                                  char **out_base,
                                  uint32_t *out_hash,
                                  guint64 *out_total,
                                  int *out_unsupported_fmt)
{
  /* 0 when nothing was skipped for its format, else the format that was --
     which way it misses decides what to tell the user, and the two point at
     opposite fixes. */
  if(out_unsupported_fmt) *out_unsupported_fmt = 0;

  GPtrArray *files = NULL;
  JsonParser *parser = json_parser_new();
  GError *err = NULL;

  if(!json_parser_load_from_data(parser, json, -1, &err))
  {
    dt_print(DT_DEBUG_ALWAYS, "[spektrafilm] bad manifest: %s",
             err ? err->message : "parse error");
    g_clear_error(&err);
    goto out;
  }

  JsonNode *root = json_parser_get_root(parser);
  if(!root || !JSON_NODE_HOLDS_OBJECT(root)) goto out;
  JsonObject *robj = json_node_get_object(root);

  if(!json_object_has_member(robj, "format")
     || json_object_get_int_member(robj, "format") != 1)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[spektrafilm] manifest format not understood by this build");
    goto out;
  }
  if(!json_object_has_member(robj, "packs")) goto out;

  JsonArray *packs = json_object_get_array_member(robj, "packs");
  if(!packs) goto out;

  JsonObject *chosen = NULL;
  uint32_t chosen_hash = 0;
  int saw_unsupported_fmt = 0;
  const guint npacks = json_array_get_length(packs);
  for(guint i = 0; i < npacks && !chosen; i++)
  {
    JsonObject *p = json_array_get_object_element(packs, i);
    if(!p || !json_object_has_member(p, "lut_hash")) continue;

    const char *hs = json_object_get_string_member(p, "lut_hash");
    if(!hs) continue;
    const uint32_t h = (uint32_t)g_ascii_strtoull(hs, NULL, 16);
    if(!h) continue;

    /* Skip anything this build could not load anyway. Checking here rather
       than after the download is the difference between a clear message and
       several MB spent on a pack that sf_pack_load() will reject. The field is
       required rather than defaulted: an entry without it would fail the same
       check in the loader after being downloaded, so accepting it here only
       moves the error later. */
    const int fmt = json_object_has_member(p, "pack_format")
                        ? (int)json_object_get_int_member(p, "pack_format")
                        : 0;
    if(fmt < SF_PACK_FORMAT_MIN || fmt > SF_PACK_FORMAT_MAX)
    {
      dt_print(DT_DEBUG_DEV,
               "[spektrafilm] manifest pack %08x is format %d, this build reads"
               " %d..%d -- skipping",
               h, fmt, SF_PACK_FORMAT_MIN, SF_PACK_FORMAT_MAX);
      if(!wanted || h == wanted) saw_unsupported_fmt = fmt;
      continue;
    }

    if(wanted)
    {
      if(h == wanted) { chosen = p; chosen_hash = h; }
    }
    else if(json_object_has_member(p, "default")
            && json_object_get_boolean_member(p, "default"))
    {
      chosen = p;
      chosen_hash = h;
    }
  }
  /* No default flagged and none requested: take the first readable entry. The
     format filter has to be repeated here -- taking "the first entry" without
     it would hand back exactly the pack the loop above rejected. */
  if(!chosen && !wanted)
  {
    for(guint i = 0; i < npacks && !chosen; i++)
    {
      JsonObject *p = json_array_get_object_element(packs, i);
      if(!p || !json_object_has_member(p, "lut_hash")) continue;
      const char *hs = json_object_get_string_member(p, "lut_hash");
      const uint32_t h = hs ? (uint32_t)g_ascii_strtoull(hs, NULL, 16) : 0;
      if(!h) continue;
      const int fmt = json_object_has_member(p, "pack_format")
                          ? (int)json_object_get_int_member(p, "pack_format")
                          : 0;
      if(fmt < SF_PACK_FORMAT_MIN || fmt > SF_PACK_FORMAT_MAX) continue;
      chosen = p;
      chosen_hash = h;
    }
  }
  if(!chosen)
  {
    /* Distinguish "the repository has nothing for you" from "it has exactly
       what you asked for, but this darktable cannot read it" -- and, within
       the second, which side the mismatch falls on. A pack newer than this
       build means update darktable; one older means the repository is behind
       and needs re-exporting, which is somebody else's job entirely. Reporting
       both as an upgrade prompt sends half of them after a release that does
       not exist. */
    if(out_unsupported_fmt) *out_unsupported_fmt = saw_unsupported_fmt;
    goto out;
  }

  const char *base = json_object_has_member(chosen, "base")
                         ? json_object_get_string_member(chosen, "base")
                         : NULL;
  if(!base || !_valid_relpath(base)) goto out;

  JsonArray *farr = json_object_has_member(chosen, "files")
                        ? json_object_get_array_member(chosen, "files")
                        : NULL;
  if(!farr) goto out;

  const guint nfiles = json_array_get_length(farr);
  if(!nfiles || nfiles > SF_MAX_FILES)
  {
    dt_print(DT_DEBUG_ALWAYS, "[spektrafilm] manifest lists %u files, refusing",
             nfiles);
    goto out;
  }

  files = g_ptr_array_new();
  guint64 total = 0;
  gboolean have_meta = FALSE, have_lut = FALSE;

  for(guint i = 0; i < nfiles; i++)
  {
    JsonObject *fo = json_array_get_object_element(farr, i);
    if(!fo) goto bad;

    const char *path = json_object_has_member(fo, "path")
                           ? json_object_get_string_member(fo, "path")
                           : NULL;
    const char *sha = json_object_has_member(fo, "sha256")
                          ? json_object_get_string_member(fo, "sha256")
                          : NULL;
    const guint64 size = json_object_has_member(fo, "size")
                             ? (guint64)json_object_get_int_member(fo, "size")
                             : 0;

    /* Every file must carry a checksum. Installing an unverified file is worse
       than not installing it: the pack drives colour rendering, and a corrupt
       LUT renders plausibly wrong rather than failing. */
    if(!_valid_relpath(path) || !_valid_sha256(sha)) goto bad;
    if(!size || size > SF_MAX_FILE_BYTES) goto bad;

    total += size;
    if(total > SF_MAX_TOTAL_BYTES) goto bad;

    if(!g_strcmp0(path, "pack.json")) have_meta = TRUE;
    if(!g_strcmp0(path, "spectra_lut.f32")) have_lut = TRUE;

    sf_fetch_file_t *f = g_malloc0(sizeof(sf_fetch_file_t));
    f->path = g_strdup(path);
    f->sha256 = g_ascii_strdown(sha, -1);
    f->size = size;
    g_ptr_array_add(files, f);
  }

  /* A pack without these two is not loadable, and finding that out after
     writing 200 files is a worse error message than finding it out now. */
  if(!have_meta || !have_lut)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[spektrafilm] manifest entry lacks pack.json or spectra_lut.f32");
    goto bad;
  }

  *out_base = g_strdup(base);
  *out_hash = chosen_hash;
  *out_total = total;
  g_object_unref(parser);
  return files;

bad:
  _files_free(files);
  files = NULL;
out:
  g_object_unref(parser);
  return files;
}

/* ---------------------------------------------------------------------- */
/* install                                                                */
/* ---------------------------------------------------------------------- */

static gboolean _rmdir_recursive(const char *path)
{
  GDir *d = g_dir_open(path, 0, NULL);
  if(d)
  {
    const char *ent;
    while((ent = g_dir_read_name(d)))
    {
      char *child = g_build_filename(path, ent, NULL);
      if(g_file_test(child, G_FILE_TEST_IS_DIR))
        _rmdir_recursive(child);
      else
        g_unlink(child);
      g_free(child);
    }
    g_dir_close(d);
  }
  return g_rmdir(path) == 0;
}

typedef struct sf_worker_args_t
{
  uint32_t wanted;
} sf_worker_args_t;

/* Reprocess so the freshly installed pack takes effect without the user having
   to reopen the image. Runs on the GUI thread; the worker cannot touch the
   pixelpipe itself. */
static gboolean _finished_idle(gpointer user_data)
{
  const gboolean ok = GPOINTER_TO_INT(user_data);
  char msg[256] = { 0 };
  sf_fetch_status(msg, sizeof(msg), NULL);

  if(ok)
  {
    dt_control_log(_("spektrafilm: data pack installed"));
    if(darktable.develop) dt_dev_reprocess_all(darktable.develop);
  }
  else
    dt_control_log(_("spektrafilm: data pack download failed -- %s"), msg);

  return G_SOURCE_REMOVE;
}

static gpointer _fetch_worker(gpointer data)
{
  sf_worker_args_t *args = (sf_worker_args_t *)data;
  const uint32_t wanted = args->wanted;
  g_free(args);

  gboolean success = FALSE;
  char *repo = NULL, *ref = NULL, *manifest_url = NULL, *manifest = NULL;
  char *base = NULL, *tmpdir = NULL, *destdir = NULL, *profdir = NULL;
  GPtrArray *files = NULL;
  CURL *curl = NULL;

  repo = dt_conf_key_exists(CONF_REPOSITORY) ? dt_conf_get_string(CONF_REPOSITORY)
                                             : g_strdup(SF_DEFAULT_REPOSITORY);
  ref = dt_conf_key_exists(CONF_REF) ? dt_conf_get_string(CONF_REF)
                                     : g_strdup(SF_DEFAULT_REF);

  if(!_valid_repository(repo) || !_valid_ref(ref))
  {
    _set_status(SF_FETCH_FAILED, -1.0, _("invalid repository configuration"));
    goto out;
  }

  curl = curl_easy_init();
  if(!curl)
  {
    _set_status(SF_FETCH_FAILED, -1.0, _("could not initialise download"));
    goto out;
  }

  _set_status(SF_FETCH_RUNNING, 0.0, _("fetching manifest"));
  manifest_url =
      g_strdup_printf("%s/%s/%s/manifest.json", SF_RAW_HOST, repo, ref);
  manifest = _http_get_string(curl, manifest_url, SF_MAX_MANIFEST_BYTES);
  if(!manifest)
  {
    _set_status(SF_FETCH_FAILED, -1.0, _("could not reach the data repository"));
    goto out;
  }
  if(_cancelled()) goto out;

  uint32_t got_hash = 0;
  guint64 total = 0;
  int unsupported_fmt = 0;
  files = _parse_manifest(manifest, wanted, &base, &got_hash, &total, &unsupported_fmt);
  if(!files)
  {
    _set_status(SF_FETCH_FAILED, -1.0,
                unsupported_fmt > SF_PACK_FORMAT_MAX
                    ? _("that data pack needs a newer darktable")
                    : unsupported_fmt
                        ? _("that data pack is too old for this darktable -- "
                            "the data repository needs re-exporting")
                        : (wanted ? _("no pack with that spectral table is published")
                                  : _("could not read the pack manifest")));
    goto out;
  }

  /* Download into a sibling temp directory and rename it into place at the end.
     A half-written pack directory would be indistinguishable from a complete
     one at the next startup: pack.json and a truncated LUT is exactly what
     _peek_lut_hash accepts. */
  char packs[PATH_MAX] = { 0 };
  _packs_dir(packs, sizeof(packs));
  if(g_mkdir_with_parents(packs, 0700))
  {
    _set_status(SF_FETCH_FAILED, -1.0, _("cannot create the pack directory"));
    goto out;
  }

  destdir = g_strdup_printf("%s%s%08x", packs, G_DIR_SEPARATOR_S, got_hash);
  tmpdir = g_strdup_printf("%s%s.incoming-%08x", packs, G_DIR_SEPARATOR_S, got_hash);
  _rmdir_recursive(tmpdir); /* leftovers from an interrupted run */
  profdir = g_build_filename(tmpdir, "profiles", NULL);
  if(g_mkdir_with_parents(profdir, 0700))
  {
    _set_status(SF_FETCH_FAILED, -1.0, _("cannot create the pack directory"));
    goto out;
  }

  sf_dl_t dl = { .total_bytes = total, .done_bytes = 0 };

  for(guint i = 0; i < files->len; i++)
  {
    if(_cancelled())
    {
      _set_status(SF_FETCH_FAILED, -1.0, _("cancelled"));
      goto out;
    }

    const sf_fetch_file_t *f = g_ptr_array_index(files, i);

    char progress_msg[256];
    g_snprintf(progress_msg, sizeof(progress_msg), _("downloading %s (%u/%u)"),
               f->path, i + 1, files->len);
    _set_status(SF_FETCH_RUNNING, -1.0, progress_msg);

    char *url =
        g_strdup_printf("%s/%s/%s/%s/%s", SF_RAW_HOST, repo, ref, base, f->path);
    char *dest = g_build_filename(tmpdir, f->path, NULL);

    /* One byte of slack over the advertised size so an off-by-one in the
       exporter does not fail the whole install; the checksum is what actually
       decides whether the bytes are right. */
    dl.limit = f->size + 1;
    const gboolean ok = _http_get_file(curl, url, dest, &dl);
    if(ok) dl.done_bytes += dl.written;

    g_free(url);

    if(!ok || !_verify(dest, f->sha256))
    {
      g_free(dest);
      _set_status(SF_FETCH_FAILED, -1.0,
                  ok ? _("a downloaded file failed its checksum")
                     : _("a file could not be downloaded"));
      goto out;
    }
    g_free(dest);
  }

  /* The pack must actually carry the table the manifest claimed, or the
     directory name is a lie and every later lookup for that hash misses. */
  uint32_t installed_hash = 0;
  if(!_peek_lut_hash(tmpdir, &installed_hash) || installed_hash != got_hash)
  {
    _set_status(SF_FETCH_FAILED, -1.0,
                _("the downloaded pack does not carry the expected table"));
    goto out;
  }

  _rmdir_recursive(destdir); /* replacing an older copy of the same hash */
  if(g_rename(tmpdir, destdir) != 0)
  {
    dt_print(DT_DEBUG_ALWAYS, "[spektrafilm] cannot install pack into %s: %s",
             destdir, strerror(errno));
    _set_status(SF_FETCH_FAILED, -1.0, _("could not install the downloaded pack"));
    goto out;
  }

  dt_print(DT_DEBUG_DEV, "[spektrafilm] installed data pack %08x into %s",
           got_hash, destdir);
  /* Publish the new state before the status flips to DONE, so anything woken
     by the completion sees a generation that already accounts for this
     install rather than racing it. */
  g_mutex_lock(&_sf.lock);
  _sf.generation++;
  g_mutex_unlock(&_sf.lock);
  _set_status(SF_FETCH_DONE, 1.0, _("done"));
  success = TRUE;

out:
  if(!success && tmpdir) _rmdir_recursive(tmpdir);
  if(curl) curl_easy_cleanup(curl);
  _files_free(files);
  g_free(manifest);
  g_free(manifest_url);
  g_free(base);
  g_free(repo);
  g_free(ref);
  g_free(tmpdir);
  g_free(destdir);
  g_free(profdir);

  g_mutex_lock(&_sf.lock);
  _sf.cancel = FALSE;
  g_mutex_unlock(&_sf.lock);

  if(darktable.gui)
    g_idle_add(_finished_idle, GINT_TO_POINTER(success ? 1 : 0));

  return NULL;
}

gboolean sf_fetch_start(const uint32_t wanted_lut_hash)
{
  if(!_sf.inited) return FALSE;

  g_mutex_lock(&_sf.lock);
  if(_sf.state == SF_FETCH_RUNNING)
  {
    g_mutex_unlock(&_sf.lock);
    return FALSE;
  }
  /* The previous run left its handle behind so this one can join it. Joining a
     thread that has already returned is cheap and reaps it; skipping this leaks
     one GThread per fetch and, worse, leaves _sf.thread non-NULL forever, which
     would make every later fetch look like one already in flight. */
  GThread *prev = _sf.thread;
  _sf.thread = NULL;
  g_mutex_unlock(&_sf.lock);
  if(prev) g_thread_join(prev);

  g_mutex_lock(&_sf.lock);
  _sf.cancel = FALSE;
  _sf.progress = 0.0;
  _sf.state = SF_FETCH_RUNNING;
  g_strlcpy(_sf.message, _("starting"), sizeof(_sf.message));

  sf_worker_args_t *args = g_malloc0(sizeof(sf_worker_args_t));
  args->wanted = wanted_lut_hash;
  _sf.thread = g_thread_new("sf-fetch", _fetch_worker, args);
  const gboolean started = _sf.thread != NULL;
  if(!started) _sf.state = SF_FETCH_FAILED;
  g_mutex_unlock(&_sf.lock);

  return started;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
