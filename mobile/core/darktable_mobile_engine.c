/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "darktable_mobile_engine.h"
#include "common/darktable.h"
#include "common/film.h"
#include "common/image.h"
#include "common/history.h"
#include "common/iop_order.h"
#include "common/whitebalance.h"
#include "common/utility.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "dt_mobile_parameter_policy.h"
#include "dt_mobile_crop_validation.h"
#include "dt_mobile_export.h"
#include "imageio/imageio_module.h"
#include "imageio/imageio_common.h"
#include "gui/draw.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <float.h>
#include <math.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

enum { MOBILE_HISTORY_LIMIT = 64 };
typedef struct mobile_history_state {
  GList *history;
  GList *iop_order;
  GList *active_modules;
  int history_end, iop_order_version;
  /* Original chroma arrays are value-owned. Module pointers are retained while
   * any reachable snapshot owns their active topology or history. */
  dt_dev_chroma_t chroma;
} mobile_history_state;
struct dt_mobile_engine { dt_imgid_t image_id; dt_develop_t develop; atomic_bool cancelled; pthread_mutex_t operation_lock;
  dt_imageio_preview_cancel_t *preview_cancel;
  GPtrArray *history_states;
  guint history_position;
  GHashTable *instance_priorities;
  GBytes *crop_geometry_key;
  GHashTable *crop_input_sizes;
  int preview_max_width, preview_max_height;
  uint64_t tone_gesture;
  dt_iop_module_t *tone_gesture_module;
  guint tone_gesture_position;
};
static pthread_mutex_t runtime_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t engine_work_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned runtime_users;
static gchar *runtime_directory;
/* Configuration outlives any one initialized darktable runtime. */
static gchar *runtime_data_directory;
static gchar *runtime_module_directory;
static gchar *runtime_temp_root;
static void sync_temperature_chroma(dt_develop_t *dev);
static void sync_mobile_overlay_dependencies(dt_develop_t *dev);
static int prepare_mobile_lens_parameters(dt_iop_module_t *module);

/* History replay may visit several former raster sources for one consumer.
 * Rebuild the ephemeral user/sink tables from the final restored parameters,
 * so old history entries cannot leave users attached to retired sources. */
static void sync_raster_mask_registrations(dt_develop_t *dev)
{
  const GList *lists[] = { dev->iop, dev->alliop };
  for(size_t list = 0; list < sizeof(lists) / sizeof(lists[0]); list++)
    for(const GList *item = lists[list]; item; item = item->next)
    {
      dt_iop_module_t *module = item->data;
      if(module->raster_mask.source.users) g_hash_table_remove_all(module->raster_mask.source.users);
      module->raster_mask.sink.source = NULL;
      module->raster_mask.sink.id = INVALID_MASKID;
    }
  for(GList *item = dev->iop; item; item = item->next)
  {
    dt_iop_module_t *module = item->data;
    if(!module->blend_params) continue;
    const dt_develop_blend_params_t current = *module->blend_params;
    dt_iop_commit_blend_params(module, &current, NULL);
  }
}

static void free_history_state(gpointer data)
{
  mobile_history_state *state = data;
  if(!state) return;
  g_list_free_full(state->history, dt_dev_free_history_item);
  g_list_free_full(state->iop_order, free);
  g_list_free(state->active_modules);
  g_free(state);
}

static mobile_history_state *capture_history_state(dt_develop_t *dev)
{
  /* Use the same deep-copy helpers as the desktop history undo recorder.
   * Copy the active topology separately: history alone omits default modules.
   * Retired module objects stay alive until no snapshot can reactivate them. */
  mobile_history_state *state = g_new0(mobile_history_state, 1);
  state->history = dt_history_duplicate(dev->history);
  state->history_end = dev->history_end;
  state->iop_order = dt_ioppr_iop_order_copy_deep(dev->iop_order_list);
  state->iop_order_version = dev->iop_order_version;
  state->active_modules = g_list_copy(dev->iop);
  state->chroma = dev->chroma;
  return state;
}

static void retain_history_modules(GHashTable *reachable, const GList *history)
{
  for(const GList *item = history; item; item = item->next)
  {
    const dt_dev_history_item_t *entry = item->data;
    if(entry->module) g_hash_table_add(reachable, entry->module);
  }
}

static void reclaim_retired_instances(dt_mobile_engine *engine)
{
  dt_develop_t *dev = &engine->develop;
  /* This adapter's develop has no pipeline pieces. Render/export build and
   * destroy their separate upstream develops before releasing engine_work_lock.
   * If a future adapter adds persistent pipes, retirement must wait for them. */
  if(dev->full.pipe || dev->preview_pipe || dev->preview2.pipe || !dev->alliop) return;
  GHashTable *reachable = g_hash_table_new(g_direct_hash, g_direct_equal);
  for(const GList *item = dev->iop; item; item = item->next)
    g_hash_table_add(reachable, item->data);
  retain_history_modules(reachable, dev->history);
  for(guint k = 0; engine->history_states && k < engine->history_states->len; k++)
  {
    const mobile_history_state *state = g_ptr_array_index(engine->history_states, k);
    for(const GList *item = state->active_modules; item; item = item->next)
      g_hash_table_add(reachable, item->data);
    retain_history_modules(reachable, state->history);
    if(state->chroma.temperature) g_hash_table_add(reachable, state->chroma.temperature);
  }
  if(dev->chroma.temperature) g_hash_table_add(reachable, dev->chroma.temperature);
  for(GList *item = dev->alliop; item; )
  {
    GList *next = item->next;
    dt_iop_module_t *module = item->data;
    if(!g_hash_table_contains(reachable, module))
    {
      dev->alliop = g_list_delete_link(dev->alliop, item);
      dt_iop_cleanup_module(module);
      free(module);
    }
    item = next;
  }
  g_hash_table_unref(reachable);
}

static void clear_tone_gesture(dt_mobile_engine *engine)
{
  engine->tone_gesture = 0;
  engine->tone_gesture_module = NULL;
  engine->tone_gesture_position = 0;
}

static void record_history_state(dt_mobile_engine *engine)
{
  clear_tone_gesture(engine);
  mobile_history_state *state = capture_history_state(&engine->develop);
  if(!engine->history_states)
    engine->history_states = g_ptr_array_new_with_free_func(free_history_state);
  else
    g_ptr_array_set_size(engine->history_states, engine->history_position + 1);
  g_ptr_array_add(engine->history_states, state);
  /* Retain the oldest reachable state plus 64 Apply steps. Dropping a state
   * also releases its params, blend settings and independently copied masks. */
  if(engine->history_states->len > MOBILE_HISTORY_LIMIT + 1)
    g_ptr_array_remove_index(engine->history_states, 0);
  engine->history_position = engine->history_states->len - 1;
  reclaim_retired_instances(engine);
}

static void remove_runtime_directory(const char *path)
{
  GDir *directory = g_dir_open(path, 0, NULL);
  if(directory)
  {
    const char *entry;
    while((entry = g_dir_read_name(directory)))
    {
      gchar *child = g_build_filename(path, entry, NULL);
      GStatBuf info;
      if(!g_lstat(child, &info) && S_ISDIR(info.st_mode)) remove_runtime_directory(child);
      else g_unlink(child);
      g_free(child);
    }
    g_dir_close(directory);
  }
  g_rmdir(path);
}

static void cleanup_runtime(void)
{
  dt_cleanup();
  if(runtime_directory)
  {
    remove_runtime_directory(runtime_directory);
    g_clear_pointer(&runtime_directory, g_free);
  }
}

static dt_mobile_status engine_fail(char *error, size_t size, dt_mobile_status status, const char *text)
{ if(error && size) snprintf(error, size, "%s", text); return status; }

static gboolean is_directory(const char *path)
{
  struct stat info;
  return path && path[0] && !stat(path, &info) && S_ISDIR(info.st_mode)
         && !g_access(path, R_OK | X_OK);
}

static gboolean has_runtime_data(const char *directory, char *error, size_t error_size)
{
  /* Necessary startup/RAW data, not a claim that four files provide full desktop
   * parity. The complete installed data tree must still be packaged. */
  static const char *required[] = {
    "darktablerc", "rawspeed/cameras.xml", "noiseprofiles.json", "wb_presets.json"
  };
  for(size_t k = 0; k < sizeof(required) / sizeof(required[0]); k++)
  {
    gchar *path = g_build_filename(directory, required[k], NULL);
    GStatBuf info;
    const gboolean valid = !g_stat(path, &info) && S_ISREG(info.st_mode)
                           && info.st_size > 0 && !g_access(path, R_OK);
    g_free(path);
    if(!valid)
    {
      if(error && error_size) snprintf(error, error_size, "runtime data is missing or unreadable: %s", required[k]);
      return FALSE;
    }
  }
#ifdef __ANDROID__
  /* Android cannot fall back to translations beside the system app_process
   * executable. Reject an incomplete private installation before dt_init,
   * whose desktop path resolver exits the process on missing directories. */
  char canonical_data[PATH_MAX];
  if(!realpath(directory, canonical_data))
  {
    if(error && error_size) snprintf(error, error_size, "unable to resolve runtime data directory");
    return FALSE;
  }
  gchar *runtime_root = g_path_get_dirname(canonical_data);
  gchar *localedir = g_build_filename(runtime_root, "locale", NULL);
  g_free(runtime_root);
  GDir *locales = g_dir_open(localedir, 0, NULL);
  gboolean has_catalog = FALSE;
  const char *locale;
  while(locales && !has_catalog && (locale = g_dir_read_name(locales)))
  {
    gchar *catalog = g_build_filename(localedir, locale, "LC_MESSAGES", "darktable.mo", NULL);
    GStatBuf info;
    has_catalog = !g_stat(catalog, &info) && S_ISREG(info.st_mode)
                  && info.st_size > 0 && !g_access(catalog, R_OK);
    g_free(catalog);
  }
  if(locales) g_dir_close(locales);
  g_free(localedir);
  if(!has_catalog)
  {
    if(error && error_size) snprintf(error, error_size, "runtime locale directory has no readable darktable catalog");
    return FALSE;
  }
#endif
  return TRUE;
}

dt_mobile_status dt_mobile_engine_configure_runtime(const char *data_dir, const char *module_dir,
                                                   const char *temp_root, char *error, size_t error_size)
{
  if(!data_dir || !module_dir || !temp_root || !g_path_is_absolute(data_dir)
     || !g_path_is_absolute(module_dir) || !g_path_is_absolute(temp_root))
    return engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                       "runtime directories must be absolute paths");
  /* Do not wait for a live init/cleanup and then unexpectedly reconfigure the
   * next session. The caller must deliberately retry once runtime work stops. */
  if(pthread_mutex_trylock(&runtime_lock))
    return engine_fail(error, error_size, DT_MOBILE_ERROR_ENGINE, "runtime initialization or cleanup is busy");
  dt_mobile_status status = DT_MOBILE_OK;
  if(runtime_users)
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_ENGINE,
                         "runtime configuration cannot change while an image session is active");
  else if(!is_directory(data_dir) || !is_directory(module_dir) || !is_directory(temp_root))
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_IO, "runtime directories are missing or inaccessible");
  else if(!has_runtime_data(data_dir, error, error_size)) status = DT_MOBILE_ERROR_IO;
  else
  {
    gchar *probe = g_build_filename(temp_root, ".dt-mobile-write-XXXXXX", NULL);
    const int descriptor = g_mkstemp(probe);
    if(descriptor < 0)
      status = engine_fail(error, error_size, DT_MOBILE_ERROR_IO, "runtime temporary root is not writable");
    else
    {
      close(descriptor);
      if(g_unlink(probe))
        status = engine_fail(error, error_size, DT_MOBILE_ERROR_IO, "unable to remove runtime write probe");
    }
    g_free(probe);
    if(status == DT_MOBILE_OK)
    {
      gchar *data_copy = g_strdup(data_dir);
      gchar *module_copy = g_strdup(module_dir);
      gchar *temp_copy = g_strdup(temp_root);
      g_free(runtime_data_directory); runtime_data_directory = data_copy;
      g_free(runtime_module_directory); runtime_module_directory = module_copy;
      g_free(runtime_temp_root); runtime_temp_root = temp_copy;
      if(error && error_size) error[0] = '\0';
    }
  }
  pthread_mutex_unlock(&runtime_lock);
  return status;
}

dt_mobile_status dt_mobile_engine_open(const char *path, dt_mobile_engine **out,
                                        char *error, size_t error_size)
{
  *out = NULL;
  pthread_mutex_lock(&runtime_lock);
  if(!runtime_users)
  {
    const char *datadir = runtime_data_directory ? runtime_data_directory : g_getenv("DT_MOBILE_DATADIR");
    const char *moduledir = runtime_module_directory ? runtime_module_directory : g_getenv("DT_MOBILE_MODULEDIR");
    if(!is_directory(datadir) || !is_directory(moduledir))
    {
      pthread_mutex_unlock(&runtime_lock);
      return engine_fail(error, error_size, DT_MOBILE_ERROR_ENGINE,
                         "configure installed runtime data/modules before opening an image");
    }
    if(!has_runtime_data(datadir, error, error_size))
    { pthread_mutex_unlock(&runtime_lock); return DT_MOBILE_ERROR_IO; }
    /* A document editor must not open the desktop user's database/config or
     * write sidecars beside its cached source document. */
    gchar *runtime_template = g_build_filename(runtime_temp_root ? runtime_temp_root : g_get_tmp_dir(),
                                               "darktable-mobile-XXXXXX", NULL);
    runtime_directory = g_mkdtemp(runtime_template) ? runtime_template : NULL;
    if(!runtime_directory) g_free(runtime_template);
    gchar *configdir = runtime_directory ? g_build_filename(runtime_directory, "config", NULL) : NULL;
    gchar *cachedir = runtime_directory ? g_build_filename(runtime_directory, "cache", NULL) : NULL;
    if(!runtime_directory || g_mkdir_with_parents(configdir, 0700) || g_mkdir_with_parents(cachedir, 0700))
    {
      g_free(configdir); g_free(cachedir);
      if(runtime_directory) { remove_runtime_directory(runtime_directory); g_clear_pointer(&runtime_directory, g_free); }
      pthread_mutex_unlock(&runtime_lock);
      return engine_fail(error, error_size, DT_MOBILE_ERROR_IO, "unable to create private engine runtime directories");
    }
    char *argv[] = { (char *)"darktable-mobile", (char *)"--library", (char *)":memory:",
                     (char *)"--datadir", (char *)datadir,
                     (char *)"--moduledir", (char *)moduledir,
                     (char *)"--configdir", configdir, (char *)"--cachedir", cachedir,
                     (char *)"--tmpdir", runtime_directory,
                     (char *)"--conf", (char *)"write_sidecar_files=never", NULL };
    const int initialized = dt_init(15, argv, FALSE, TRUE, NULL);
    g_free(configdir); g_free(cachedir);
    if(initialized)
    {
      remove_runtime_directory(runtime_directory); g_clear_pointer(&runtime_directory, g_free);
      pthread_mutex_unlock(&runtime_lock);
      return engine_fail(error, error_size, DT_MOBILE_ERROR_ENGINE, "darktable headless initialization failed");
    }
  }
  runtime_users++;
  pthread_mutex_unlock(&runtime_lock);

  pthread_mutex_lock(&engine_work_lock);
  gchar *directory = g_path_get_dirname(path);
  dt_film_t film;
  const dt_filmid_t film_id = dt_film_new(&film, directory);
  g_free(directory);
  const dt_imgid_t image_id = dt_is_valid_filmid(film_id)
    ? dt_image_import(film_id, path, TRUE, FALSE) : NO_IMGID;
  pthread_mutex_unlock(&engine_work_lock);
  if(!dt_is_valid_imgid(image_id))
  {
    pthread_mutex_lock(&runtime_lock); runtime_users--; if(!runtime_users) cleanup_runtime(); pthread_mutex_unlock(&runtime_lock);
    return engine_fail(error, error_size, DT_MOBILE_ERROR_DECODE, "darktable could not import or decode the image");
  }
  dt_mobile_engine *engine = calloc(1, sizeof(*engine));
  if(!engine)
  {
    pthread_mutex_lock(&runtime_lock); runtime_users--; if(!runtime_users) cleanup_runtime(); pthread_mutex_unlock(&runtime_lock);
    return engine_fail(error, error_size, DT_MOBILE_ERROR_OUT_OF_MEMORY, "unable to allocate darktable image context");
  }
  engine->image_id = image_id;
  atomic_init(&engine->cancelled, false);
  pthread_mutex_init(&engine->operation_lock, NULL);
  engine->preview_cancel = dt_imageio_preview_cancel_new();
  pthread_mutex_lock(&engine_work_lock);
  dt_dev_init(&engine->develop, FALSE);
  dt_dev_load_image(&engine->develop, image_id);
  /* Loading history constructs its items. Apply the current history end to
   * module params before exposing controls or editing a single field, so all
   * other values from imported history and auto presets are preserved. */
  dt_dev_pop_history_items_ext(&engine->develop, engine->develop.history_end);
  sync_raster_mask_registrations(&engine->develop);
  sync_mobile_overlay_dependencies(&engine->develop);
  const gboolean decoded = engine->develop.image_storage.load_status == DT_IMAGEIO_OK;
  if(decoded && engine->preview_cancel)
  {
    gboolean prepared_lens = FALSE;
    for(GList *item = engine->develop.iop; item; item = item->next)
    {
      dt_iop_module_t *module = item->data;
      if(module->enabled && prepare_mobile_lens_parameters(module))
      {
        dt_dev_add_history_item_ext(&engine->develop, module, FALSE, TRUE);
        prepared_lens = TRUE;
      }
    }
    if(prepared_lens) dt_dev_write_history(&engine->develop);
    sync_temperature_chroma(&engine->develop);
    record_history_state(engine);
  }
  pthread_mutex_unlock(&engine_work_lock);
  if(!decoded || !engine->preview_cancel)
  {
    dt_mobile_engine_close(engine);
    return engine_fail(error, error_size, decoded ? DT_MOBILE_ERROR_OUT_OF_MEMORY : DT_MOBILE_ERROR_DECODE,
                       decoded ? "unable to allocate cancellation state" : "darktable could not decode full image data");
  }
  *out = engine;
  return DT_MOBILE_OK;
}

/* Module parameters stay in the upstream parameter structs. The JSON is only
 * a description for touch controls, never a second implementation of an iop. */
static void json_string(GString *json, const char *text)
{
  g_string_append_c(json, '"');
  for(const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++)
  {
    if(*p == '"' || *p == '\\') g_string_append_c(json, '\\');
    if(*p < 0x20) g_string_append_printf(json, "\\u%04x", *p);
    else g_string_append_c(json, *p);
  }
  g_string_append_c(json, '"');
}

static void json_number(GString *json, double value)
{
  char number[G_ASCII_DTOSTR_BUF_SIZE];
  g_string_append(json, isfinite(value) ? g_ascii_dtostr(number, sizeof(number), value) : "null");
}

static dt_mobile_status finish_json(GString *json, char **out, char *error, size_t size)
{
  *out = malloc(json->len + 1);
  if(*out) memcpy(*out, json->str, json->len + 1);
  g_string_free(json, TRUE);
  return *out ? DT_MOBILE_OK : engine_fail(error, size, DT_MOBILE_ERROR_OUT_OF_MEMORY,
                                           "unable to allocate module description");
}

static void module_identifier(const dt_iop_module_t *module, char *identifier, size_t size)
{
  if(module->multi_priority)
    snprintf(identifier, size, "%s@%d", module->op, module->multi_priority);
  else snprintf(identifier, size, "%s", module->op);
}

static dt_iop_module_t *find_module(dt_mobile_engine *engine, const char *identifier)
{
  for(GList *item = engine->develop.iop; item; item = g_list_next(item))
  {
    dt_iop_module_t *module = item->data;
    char current[128];
    module_identifier(module, current, sizeof(current));
    if(!strcmp(current, identifier)) return module;
  }
  return NULL;
}

static int highest_instance_priority(const dt_mobile_engine *engine, const dt_iop_module_t *base)
{
  int maximum = engine->instance_priorities
    ? GPOINTER_TO_INT(g_hash_table_lookup(engine->instance_priorities, base->op)) : 0;
  for(const GList *item = engine->develop.iop; item; item = item->next)
  {
    const dt_iop_module_t *module = item->data;
    if(module->instance == base->instance) maximum = MAX(maximum, module->multi_priority);
  }
  return maximum;
}

static gboolean can_create_instance(const dt_mobile_engine *engine, const dt_iop_module_t *module)
{
  return module && !dt_iop_is_hidden(module) && !(module->flags() & IOP_FLAGS_ONE_INSTANCE)
         && highest_instance_priority(engine, module) < INT_MAX;
}

static size_t enum_entry_count(const dt_introspection_field_t *field)
{
  if(!field->Enum.values) return 0;
  size_t count = 0;
  /* The upstream introspection API terminates values with a NULL name. Honor
   * that terminator as well as entries, including when an older generator
   * supplied an inconsistent count for an anonymous enum. */
  while(count < field->Enum.entries && field->Enum.values[count].name) count++;
  return count;
}

static gboolean number_info(const dt_introspection_field_t *field, const void *value,
                            long double *current, long double *minimum, long double *maximum,
                            const char **type)
{
  *type = "int";
#define NUMBER_INFO(tag, ctype, member) \
  case DT_INTROSPECTION_TYPE_##tag: \
    *current = *(const ctype *)value; \
    *minimum = field->member.Min; *maximum = field->member.Max; return TRUE
  switch(field->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:
      *type = "float"; *current = *(const float *)value;
      *minimum = field->Float.Min; *maximum = field->Float.Max; return TRUE;
    case DT_INTROSPECTION_TYPE_DOUBLE:
      *type = "double"; *current = *(const double *)value;
      *minimum = field->Double.Min; *maximum = field->Double.Max; return TRUE;
    NUMBER_INFO(CHAR, char, Char);
    NUMBER_INFO(INT8, int8_t, Int8);
    NUMBER_INFO(UINT8, uint8_t, UInt8);
    NUMBER_INFO(SHORT, short, Short);
    NUMBER_INFO(USHORT, unsigned short, UShort);
    NUMBER_INFO(INT, int, Int);
    NUMBER_INFO(UINT, unsigned int, UInt);
    NUMBER_INFO(LONG, long, Long);
    NUMBER_INFO(ULONG, unsigned long, ULong);
    case DT_INTROSPECTION_TYPE_BOOL:
      *type = "bool"; *current = *(const gboolean *)value;
      *minimum = 0; *maximum = 1; return TRUE;
    case DT_INTROSPECTION_TYPE_ENUM:
    {
      const size_t entries = enum_entry_count(field);
      if(!entries) return FALSE;
      *type = "enum"; *current = *(const int *)value;
      *minimum = *maximum = field->Enum.values[0].value;
      for(size_t k = 1; k < entries; k++)
      {
        *minimum = MIN(*minimum, field->Enum.values[k].value);
        *maximum = MAX(*maximum, field->Enum.values[k].value);
      }
      return TRUE;
    }
    default: return FALSE;
  }
#undef NUMBER_INFO
}

typedef void (*field_visitor)(dt_iop_module_t *, const dt_introspection_field_t *,
                              size_t, const char *, void *);

/* Introspection offsets are absolute within the parameter struct, including
 * nested structs. Array element descriptors describe the first element. */
static void visit_fields(dt_iop_module_t *module, const dt_introspection_field_t *field,
                         size_t delta, const char *path, unsigned depth,
                         field_visitor visitor, void *data)
{
  if(!field || depth > 32 || delta > (size_t)module->params_size
     || field->header.offset > (size_t)module->params_size - delta) return;
  if(field->header.type == DT_INTROSPECTION_TYPE_STRUCT)
  {
    for(size_t k = 0; k < field->Struct.entries; k++)
    {
      const dt_introspection_field_t *child = field->Struct.fields[k];
      if(!child || !child->header.field_name) continue;
      gchar *name = path[0] ? g_strconcat(path, ".", child->header.field_name, NULL)
                             : g_strdup(child->header.field_name);
      visit_fields(module, child, delta, name, depth + 1, visitor, data);
      g_free(name);
    }
  }
  else if(field->header.type == DT_INTROSPECTION_TYPE_ARRAY)
  {
    /* Strings and opaque LUTs need purpose-built controls. Do not present a
     * partial list as a complete editor for those parameter types. */
    if(!field->Array.count || field->Array.count > 4096
       || field->Array.type == DT_INTROSPECTION_TYPE_CHAR || !field->Array.field
       || !field->header.size || field->header.size % field->Array.count
       || field->header.size > (size_t)module->params_size - delta - field->header.offset) return;
    const size_t stride = field->header.size / field->Array.count;
    if(field->Array.field->header.offset != field->header.offset
       || field->Array.field->header.type != field->Array.type
       || field->Array.field->header.size != stride) return;
    for(size_t k = 0; k < field->Array.count; k++)
    {
      if(k && stride > (SIZE_MAX - delta) / k) break;
      gchar *name = g_strdup_printf("%s[%zu]", path, k);
      visit_fields(module, field->Array.field, delta + k * stride, name,
                   depth + 1, visitor, data);
      g_free(name);
    }
  }
  else if(field->header.size <= (size_t)module->params_size - delta - field->header.offset)
    visitor(module, field, field->header.offset + delta, path, data);
}

/* A few original discrete controls use integer storage rather than a C enum.
 * Preserve that storage while advertising the desktop's actual menu labels. */
static const char *mobile_integer_choice_label(const char *operation, const char *path, int value)
{
  static const char *fusion[] = { "none", "two exposures", "three exposures" };
  static const char *interpolators[] = { "contrasted", "faded", "linear", "optimized" };
  static const char *alignment[] = { "top left", "top", "top right", "left", "center", "right",
                                   "bottom left", "bottom", "bottom right" };
  if(!strcmp(operation, "basecurve") && !strcmp(path, "exposure_fusion") && value >= 0 && value < 3)
    return fusion[value];
  if(!strcmp(operation, "filmic") && !strcmp(path, "interpolator") && value >= 0 && value < 4)
    return interpolators[value];
  if(!strcmp(operation, "filmic") && !strcmp(path, "preserve_color") && value >= 0 && value < 2)
    return value ? "on" : "off";
  if((!strcmp(operation, "watermark") || !strcmp(operation, "overlay"))
     && !strcmp(path, "alignment") && value >= 0 && value < 9) return alignment[value];
  return NULL;
}

typedef struct parameter_json { GString *json; gboolean first; } parameter_json;
static void append_parameter(dt_iop_module_t *module, const dt_introspection_field_t *field,
                             size_t offset, const char *path, void *data)
{
  if(!dt_mobile_extended_parameter_applicable(module)
     || !dt_mobile_parameter_allowed(module->op, path, field->header.type)
     || !dt_mobile_array_parameter_visible(module, path)) return;
  parameter_json *description = data;
  long double value, minimum, maximum;
  const char *type;
  if(!number_info(field, (const char *)module->params + offset, &value, &minimum, &maximum, &type)
     || !isfinite((double)value)) return;
  dt_mobile_parameter_limits(module->op, path, &minimum, &maximum);
  dt_mobile_array_parameter_dynamic_limits(module, path, &minimum, &maximum);
  /* Some iops use NaN as an unavailable sensor channel (for example the
   * fourth white-balance coefficient on a three-channel RGB image). Such a
   * sentinel is not an editable numeric control. */
  GString *json = description->json;
  if(!description->first) g_string_append_c(json, ',');
  description->first = FALSE;
  g_string_append(json, "{\"name\":"); json_string(json, path);
  g_string_append(json, ",\"label\":");
  json_string(json, dt_mobile_parameter_label(module->op, path,
                   field->header.description && field->header.description[0]
                     ? field->header.description : path));
  g_string_append(json, ",\"type\":"); json_string(json, type);
  g_string_append(json, ",\"value\":"); json_number(json, (double)value);
  g_string_append(json, ",\"min\":"); json_number(json, (double)minimum);
  g_string_append(json, ",\"max\":"); json_number(json, (double)maximum);
  /* Defaults come from the image's original iop default parameter struct, not
   * a second table in the phone UI. visit_fields already checked this field's
   * complete storage span against the shared params_size schema. */
  if(module->default_params)
  {
    long double original, unused_minimum, unused_maximum;
    const char *unused_type;
    if(number_info(field, (const char *)module->default_params + offset,
                   &original, &unused_minimum, &unused_maximum, &unused_type)
       && isfinite((double)original) && original >= minimum && original <= maximum
       && dt_mobile_parameter_value_valid(module, path, (double)original))
    {
      gboolean valid_default = TRUE;
      if(field->header.type == DT_INTROSPECTION_TYPE_ENUM)
      {
        valid_default = FALSE;
        for(size_t k = 0; k < enum_entry_count(field); k++)
          if(original == field->Enum.values[k].value) valid_default = TRUE;
      }
      if(valid_default)
      {
        g_string_append(json, ",\"default\":"); json_number(json, (double)original);
      }
    }
  }
  double slider_minimum, slider_maximum;
  if(dt_mobile_parameter_slider_limits(module->op, path, &slider_minimum, &slider_maximum)
     && slider_minimum >= minimum && slider_maximum <= maximum && slider_minimum < slider_maximum)
  {
    g_string_append(json, ",\"sliderMin\":"); json_number(json, slider_minimum);
    g_string_append(json, ",\"sliderMax\":"); json_number(json, slider_maximum);
  }
  if(field->header.type == DT_INTROSPECTION_TYPE_ENUM)
  {
    g_string_append(json, ",\"choices\":[");
    gboolean first_choice = TRUE;
    for(size_t k = 0; k < enum_entry_count(field); k++)
    {
      const dt_introspection_type_enum_tuple_t *choice = &field->Enum.values[k];
      if(!dt_mobile_parameter_value_valid(module, path, choice->value)) continue;
      if(!first_choice) g_string_append_c(json, ',');
      first_choice = FALSE;
      g_string_append_printf(json, "{\"value\":%d,\"label\":", choice->value);
      json_string(json, choice->description && choice->description[0] ? choice->description : choice->name);
      g_string_append_c(json, '}');
    }
    g_string_append_c(json, ']');
  }
  if(field->header.type == DT_INTROSPECTION_TYPE_INT && mobile_integer_choice_label(module->op, path, 0))
  {
    g_string_append(json, ",\"choices\":[");
    gboolean first_choice = TRUE;
    for(int choice = 0; choice < 9; choice++)
    {
      const char *label = mobile_integer_choice_label(module->op, path, choice);
      if(!label || choice < minimum || choice > maximum
         || !dt_mobile_parameter_value_valid(module, path, choice)) continue;
      if(!first_choice) g_string_append_c(json, ',');
      first_choice = FALSE;
      g_string_append_printf(json, "{\"value\":%d,\"label\":", choice);
      json_string(json, label);
      g_string_append_c(json, '}');
    }
    g_string_append_c(json, ']');
  }
  g_string_append_c(json, '}');
}

#include "dt_mobile_blending_controls.h"
#include "dt_mobile_lens_controls.h"
#include "dt_mobile_text_controls.h"
#include "dt_mobile_zone_controls.h"
#include "dt_mobile_mask_controls.h"
#include "dt_mobile_liquify_controls.h"

enum { MOBILE_CURVE_CHANNELS = 3, MOBILE_CURVE_MAX_NODES = 20, MOBILE_CURVE_SAMPLES = 256 };
static const float mobile_curve_min_spacing = 1e-6f;
typedef struct mobile_curve_layout
{
  size_t nodes, channel_stride, node_stride, x, y, counts, types;
  int channels, editable_channels, max_nodes;
  gboolean rgb;
} mobile_curve_layout;

static gboolean curve_field_fits(const dt_iop_module_t *module, const dt_introspection_field_t *field)
{
  return field && field->header.offset <= (size_t)module->params_size
    && field->header.size <= (size_t)module->params_size - field->header.offset;
}

/* These upstream tools share the shape of a 3 x 20 node array plus
 * per-channel counts/interpolation. Resolve that shape through introspection,
 * rather than duplicating version-sensitive iop parameter structs here. */
static gboolean curve_layout(dt_iop_module_t *module, mobile_curve_layout *layout)
{
  if(!module || !module->have_introspection || !module->get_introspection()
     || !module->get_f || !module->params) return FALSE;
  const gboolean rgb = !strcmp(module->op, "rgbcurve");
  const gboolean tone = !strcmp(module->op, "tonecurve");
  const gboolean zones = !strcmp(module->op, "colorzones");
  if(!rgb && !tone && !zones && strcmp(module->op, "basecurve")) return FALSE;
  const char *node_name = rgb ? "curve_nodes" : tone ? "tonecurve" : zones ? "curve" : "basecurve";
  const dt_introspection_field_t *nodes = module->get_f(node_name);
  const dt_introspection_field_t *counts = module->get_f(rgb || zones ? "curve_num_nodes" : tone ? "tonecurve_nodes" : "basecurve_nodes");
  const dt_introspection_field_t *types = module->get_f(rgb || zones ? "curve_type" : tone ? "tonecurve_type" : "basecurve_type");
  if(!curve_field_fits(module, nodes) || !curve_field_fits(module, counts)
     || !curve_field_fits(module, types)
     || nodes->header.type != DT_INTROSPECTION_TYPE_ARRAY || nodes->Array.count != MOBILE_CURVE_CHANNELS
     || nodes->Array.type != DT_INTROSPECTION_TYPE_ARRAY || !nodes->Array.field) return FALSE;
  const dt_introspection_field_t *row = nodes->Array.field;
  if(!curve_field_fits(module, row) || row->header.type != DT_INTROSPECTION_TYPE_ARRAY || row->Array.count != MOBILE_CURVE_MAX_NODES
     || row->Array.type != DT_INTROSPECTION_TYPE_STRUCT || !row->Array.field
     || row->header.offset != nodes->header.offset
     || nodes->header.size != MOBILE_CURVE_CHANNELS * row->header.size) return FALSE;
  const dt_introspection_field_t *node = row->Array.field;
  if(!curve_field_fits(module, node) || node->header.type != DT_INTROSPECTION_TYPE_STRUCT || node->Struct.entries != 2
     || !node->Struct.fields || node->header.offset != row->header.offset
     || row->header.size != MOBILE_CURVE_MAX_NODES * node->header.size) return FALSE;
  const dt_introspection_field_t *x = NULL, *y = NULL;
  for(size_t k = 0; k < node->Struct.entries; k++)
  {
    const dt_introspection_field_t *field = node->Struct.fields[k];
    if(!field || !field->header.field_name || field->header.type != DT_INTROSPECTION_TYPE_FLOAT
       || field->header.size != sizeof(float) || field->header.offset < node->header.offset
       || field->header.offset - node->header.offset > node->header.size
       || sizeof(float) > node->header.size - (field->header.offset - node->header.offset)) return FALSE;
    if(!strcmp(field->header.field_name, "x")) x = field;
    else if(!strcmp(field->header.field_name, "y")) y = field;
  }
  if(!x || !y || x->header.offset == y->header.offset) return FALSE;
  const dt_introspection_field_t *arrays[] = { counts, types };
  for(size_t k = 0; k < 2; k++)
  {
    const dt_introspection_field_t *array = arrays[k];
    if(array->header.type != DT_INTROSPECTION_TYPE_ARRAY || array->Array.count != MOBILE_CURVE_CHANNELS
       || array->Array.type != DT_INTROSPECTION_TYPE_INT || !array->Array.field
       || array->Array.field->header.type != DT_INTROSPECTION_TYPE_INT
       || array->Array.field->header.size != sizeof(int)
       || array->Array.field->header.offset != array->header.offset
       || array->header.size != MOBILE_CURVE_CHANNELS * sizeof(int)) return FALSE;
  }
  int editable_channels = 1;
  if(rgb)
  {
    const dt_introspection_field_t *mode = module->get_f("curve_autoscale");
    int value;
    if(!curve_field_fits(module, mode) || mode->header.type != DT_INTROSPECTION_TYPE_ENUM
       || mode->header.size != sizeof(value)) return FALSE;
    memcpy(&value, (const char *)module->params + mode->header.offset, sizeof(value));
    if(value != 0 && value != 1) return FALSE; /* Upstream linked/independent modes. */
    editable_channels = value == 0 ? 1 : MOBILE_CURVE_CHANNELS;
  }
  if(tone)
  {
    const dt_introspection_field_t *mode = module->get_f("tonecurve_autoscale_ab");
    if(!curve_field_fits(module, mode) || mode->header.type != DT_INTROSPECTION_TYPE_ENUM
       || mode->header.size != sizeof(int)) return FALSE;
    int value;
    memcpy(&value, (const char *)module->params + mode->header.offset, sizeof(value));
    if(value < 0 || value > 3) return FALSE;
    editable_channels = value == 0 ? MOBILE_CURVE_CHANNELS : 1;
  }
  int max_nodes = MOBILE_CURVE_MAX_NODES;
  if(zones)
  {
    editable_channels = MOBILE_CURVE_CHANNELS;
    const dt_introspection_field_t *version = module->get_f("splines_version");
    const dt_introspection_field_t *selection = module->get_f("channel");
    if(!curve_field_fits(module, version) || version->header.type != DT_INTROSPECTION_TYPE_INT
       || version->header.size != sizeof(int) || !curve_field_fits(module, selection)
       || selection->header.type != DT_INTROSPECTION_TYPE_ENUM || selection->header.size != sizeof(int)) return FALSE;
    int value, selected;
    memcpy(&value, (const char *)module->params + version->header.offset, sizeof(value));
    memcpy(&selected, (const char *)module->params + selection->header.offset, sizeof(selected));
    if(value < 0 || value > 1 || selected < 0 || selected > 2) return FALSE;
    /* The historical spline path adds two ghost anchors into the upstream
     * MAX_ANCHORS=20 buffer. Keep its editable node list within that capacity. */
    if(value == 0) max_nodes -= 2;
  }
  *layout = (mobile_curve_layout) { nodes->header.offset, row->header.size, node->header.size,
    x->header.offset - node->header.offset, y->header.offset - node->header.offset,
    counts->header.offset, types->header.offset, rgb || tone || zones ? MOBILE_CURVE_CHANNELS : 1, editable_channels, max_nodes, rgb };
  return TRUE;
}

static gboolean valid_curve_points(const dt_mobile_curve_point *points, size_t count, int interpolation)
{
  if(!points || count < 2 || count > MOBILE_CURVE_MAX_NODES || interpolation < 0 || interpolation > 2)
    return FALSE;
  for(size_t k = 0; k < count; k++)
    if(!isfinite(points[k].x) || !isfinite(points[k].y) || points[k].x < 0.0f || points[k].x > 1.0f
       || points[k].y < 0.0f || points[k].y > 1.0f
       || (k && (double)points[k].x - (double)points[k - 1].x < (double)mobile_curve_min_spacing))
      return FALSE;
  return TRUE;
}

static gboolean read_curve(const dt_iop_module_t *module, const mobile_curve_layout *layout, int channel,
                           dt_mobile_curve_point *points, int *count, int *interpolation)
{
  const char *params = module->params;
  memcpy(count, params + layout->counts + channel * sizeof(int), sizeof(*count));
  memcpy(interpolation, params + layout->types + channel * sizeof(int), sizeof(*interpolation));
  if(*count < 2 || *count > layout->max_nodes) return FALSE;
  for(int k = 0; k < *count; k++)
  {
    const char *node = params + layout->nodes + channel * layout->channel_stride + k * layout->node_stride;
    memcpy(&points[k].x, node + layout->x, sizeof(float));
    memcpy(&points[k].y, node + layout->y, sizeof(float));
  }
  return valid_curve_points(points, (size_t)*count, *interpolation);
}

static gboolean curve_samples(const dt_mobile_curve_point *points, size_t count, int interpolation,
                              float *values, float *endpoint)
{
  dt_draw_curve_t *curve = dt_draw_curve_new(0.0f, 1.0f, (unsigned int)interpolation);
  for(size_t k = 0; k < count; k++) dt_draw_curve_add_point(curve, points[k].x, points[k].y);
  dt_draw_curve_calc_values(curve, 0.0f, 1.0f, MOBILE_CURVE_SAMPLES, NULL, values);
  *endpoint = dt_draw_curve_calc_value(curve, 1.0f);
  dt_draw_curve_destroy(curve);
  if(!isfinite(*endpoint)) return FALSE;
  for(size_t k = 0; k < MOBILE_CURVE_SAMPLES; k++) if(!isfinite(values[k])) return FALSE;
  return TRUE;
}

/* Color-zones uses the original desktop GUI's wrapped V1 / periodic V2
 * splines. Nodes remain unscaled in this diagram, matching its desktop editor;
 * actual strength and pixel processing remain in the original IOP. */
static gboolean module_curve_samples(dt_iop_module_t *module,
    const dt_mobile_curve_point *points, size_t count, int interpolation,
    float *values, float *endpoint)
{
  if(strcmp(module->op, "colorzones")) return curve_samples(points, count, interpolation, values, endpoint);
  const dt_introspection_field_t *version = module->get_f("splines_version");
  const dt_introspection_field_t *selection = module->get_f("channel");
  if(!curve_field_fits(module, version) || !curve_field_fits(module, selection)) return FALSE;
  int spline_version, selected;
  memcpy(&spline_version, (const char *)module->params + version->header.offset, sizeof(spline_version));
  memcpy(&selected, (const char *)module->params + selection->header.offset, sizeof(selected));
  if(count + (spline_version == 0 ? 2 : 0) > MAX_ANCHORS) return FALSE;
  dt_draw_curve_t *curve = dt_draw_curve_new(0.f, 1.f, (unsigned int)interpolation);
  if(!curve) return FALSE;
  if(spline_version == 0)
    dt_draw_curve_add_point(curve, points[count - 2].x - 1.f,
      selected == 2 ? points[count - 2].y : points[0].y);
  for(size_t index = 0; index < count; index++) dt_draw_curve_add_point(curve, points[index].x, points[index].y);
  if(spline_version == 0)
  {
    dt_draw_curve_add_point(curve, points[1].x + 1.f,
      selected == 2 ? points[1].y : points[count - 1].y);
    dt_draw_curve_calc_values(curve, 0.f, 1.f, MOBILE_CURVE_SAMPLES, NULL, values);
    *endpoint = dt_draw_curve_calc_value(curve, 1.f);
  }
  else
  {
    dt_draw_curve_calc_values_V2(curve, 0.f, 1.f, MOBILE_CURVE_SAMPLES, NULL, values, selected == 2);
    *endpoint = selected == 2 ? values[0] : values[MOBILE_CURVE_SAMPLES - 1];
  }
  dt_draw_curve_destroy(curve);
  if(!isfinite(*endpoint)) return FALSE;
  for(size_t index = 0; index < MOBILE_CURVE_SAMPLES; index++) if(!isfinite(values[index])) return FALSE;
  return TRUE;
}

typedef struct mobile_rgb_curve_options_layout
{
  const dt_introspection_field_t *mode, *preserve;
  int current_mode, current_preserve;
} mobile_rgb_curve_options_layout;

static gboolean enum_has_value(const dt_introspection_field_t *field, int value)
{
  for(size_t k = 0; k < enum_entry_count(field); k++)
    if(field->Enum.values[k].value == value) return TRUE;
  return FALSE;
}

static gboolean rgb_curve_options_layout(dt_iop_module_t *module, mobile_curve_layout *curves,
                                         mobile_rgb_curve_options_layout *options)
{
  if(!curve_layout(module, curves) || !curves->rgb) return FALSE;
  const char *names[] = { "curve_autoscale", "preserve_colors" };
  const dt_introspection_field_t *fields[2];
  for(size_t k = 0; k < 2; k++)
  {
    fields[k] = module->get_f(names[k]);
    if(!curve_field_fits(module, fields[k]) || !fields[k]->header.field_name
       || strcmp(fields[k]->header.field_name, names[k])
       || fields[k]->header.type != DT_INTROSPECTION_TYPE_ENUM
       || fields[k]->header.size != sizeof(int)) return FALSE;
  }
  if(fields[0]->header.offset == fields[1]->header.offset
     || enum_entry_count(fields[0]) != 2 || !enum_has_value(fields[0], 0) || !enum_has_value(fields[0], 1)
     || enum_entry_count(fields[1]) != 7) return FALSE;
  /* Match the reviewed common/rgb_norms.h choices, not a min/max range that
   * could admit holes or newly added processing modes without review. */
  const int preserve_values[] = { 0, 1, 2, 3, 4, 5, 6 };
  for(size_t k = 0; k < sizeof(preserve_values) / sizeof(preserve_values[0]); k++)
    if(!enum_has_value(fields[1], preserve_values[k])) return FALSE;
  int mode, preserve;
  memcpy(&mode, (const char *)module->params + fields[0]->header.offset, sizeof(mode));
  memcpy(&preserve, (const char *)module->params + fields[1]->header.offset, sizeof(preserve));
  if(!enum_has_value(fields[0], mode) || !enum_has_value(fields[1], preserve)) return FALSE;
  /* Processing builds all three LUTs, including inactive linked-mode channels.
   * A mode change must not enable malformed untouched historical channels. */
  for(int channel = 0; channel < MOBILE_CURVE_CHANNELS; channel++)
  {
    dt_mobile_curve_point points[MOBILE_CURVE_MAX_NODES];
    int count, interpolation;
    if(!read_curve(module, curves, channel, points, &count, &interpolation)) return FALSE;
  }
  *options = (mobile_rgb_curve_options_layout){ fields[0], fields[1], mode, preserve };
  return TRUE;
}

static void append_enum_choices(GString *json, const dt_introspection_field_t *field)
{
  g_string_append_c(json, '[');
  for(size_t k = 0; k < enum_entry_count(field); k++)
  {
    const dt_introspection_type_enum_tuple_t *choice = &field->Enum.values[k];
    if(k) g_string_append_c(json, ',');
    g_string_append_printf(json, "{\"value\":%d,\"label\":", choice->value);
    json_string(json, choice->description && choice->description[0] ? choice->description : choice->name);
    g_string_append_c(json, '}');
  }
  g_string_append_c(json, ']');
}

static void append_rgb_curve_options(dt_iop_module_t *module, GString *json)
{
  mobile_curve_layout curves;
  mobile_rgb_curve_options_layout options;
  if(!rgb_curve_options_layout(module, &curves, &options)) return;
  g_string_append_printf(json, ",\"rgbCurveOptions\":{\"mode\":%d,\"preserveColors\":%d,\"modes\":",
                         options.current_mode, options.current_preserve);
  append_enum_choices(json, options.mode);
  g_string_append(json, ",\"preserveColorChoices\":");
  append_enum_choices(json, options.preserve);
  g_string_append_c(json, '}');
}

static void append_curve_point(GString *json, float x, float y)
{
  g_string_append(json, "{\"x\":"); json_number(json, x);
  g_string_append(json, ",\"y\":"); json_number(json, y);
  g_string_append_c(json, '}');
}

static void append_curves(dt_iop_module_t *module, GString *json)
{
  mobile_curve_layout layout;
  if(!curve_layout(module, &layout)) return;
  gboolean first = TRUE;
  /* Linked RGB mode applies channel zero to every component; its stored
   * independent channels are inactive and are not editing targets. */
  for(int channel = 0; channel < layout.editable_channels; channel++)
  {
    dt_mobile_curve_point points[MOBILE_CURVE_MAX_NODES];
    int count, interpolation;
    float samples[MOBILE_CURVE_SAMPLES], endpoint;
    /* Unsupported historical shapes are omitted without changing that history
     * or making an unrelated module's metadata unavailable. */
    if(!read_curve(module, &layout, channel, points, &count, &interpolation)
       || !module_curve_samples(module, points, (size_t)count, interpolation, samples, &endpoint)) continue;
    if(!first) g_string_append_c(json, ',');
    first = FALSE;
    g_string_append_printf(json, "{\"channel\":%d,\"label\":", channel);
    const char *label = !strcmp(module->op, "tonecurve") ? (channel == 0 ? "Lightness (L)" : channel == 1 ? "Lab a" : "Lab b")
      : !strcmp(module->op, "colorzones") ? (channel == 0 ? "Lightness" : channel == 1 ? "Chroma" : "Hue")
      : layout.rgb && layout.editable_channels == MOBILE_CURVE_CHANNELS
        ? (channel == 0 ? "Red" : channel == 1 ? "Green" : "Blue") : "RGB";
    json_string(json, label);
    g_string_append_printf(json, ",\"editable\":%s,\"interpolation\":%d,\"maxNodes\":%d,\"minSpacing\":",
      "true", interpolation, layout.max_nodes);
    json_number(json, mobile_curve_min_spacing);
    g_string_append(json, ",\"nodes\":[");
    for(int k = 0; k < count; k++)
    {
      if(k) g_string_append_c(json, ',');
      append_curve_point(json, points[k].x, points[k].y);
    }
    g_string_append(json, "],\"samples\":[");
    for(int k = 0; k < MOBILE_CURVE_SAMPLES; k++)
    {
      if(k) g_string_append_c(json, ',');
      append_curve_point(json, (float)k / MOBILE_CURVE_SAMPLES, samples[k]);
    }
    g_string_append_c(json, ','); append_curve_point(json, 1.0f, endpoint);
    g_string_append_c(json, ']');
    if(module->default_params)
    {
      dt_iop_module_t defaults = *module;
      defaults.params = module->default_params;
      dt_mobile_curve_point reset[MOBILE_CURVE_MAX_NODES];
      int reset_count, reset_interpolation;
      if(read_curve(&defaults, &layout, channel, reset, &reset_count, &reset_interpolation))
      {
        g_string_append(json, ",\"resetNodes\":[");
        for(int k = 0; k < reset_count; k++)
        {
          if(k) g_string_append_c(json, ',');
          append_curve_point(json, reset[k].x, reset[k].y);
        }
        g_string_append_c(json, ']');
      }
    }
    g_string_append_c(json, '}');
  }
}

typedef struct mobile_crop_layout { size_t coordinates[4], ratios[2]; } mobile_crop_layout;

/* Resolve the six direct fields of the upstream crop module, not a copied
 * parameter struct. A future schema/type change disables this editor instead
 * of writing a partially compatible history blob. */
static gboolean crop_layout(dt_iop_module_t *module, mobile_crop_layout *layout)
{
  if(!module || strcmp(module->op, "crop") || !module->have_introspection
     || !module->get_introspection || !module->get_introspection()
     || !module->get_f || !module->params || module->params_size <= 0) return FALSE;
  const char *names[] = { "cx", "cy", "cw", "ch", "ratio_n", "ratio_d" };
  const dt_introspection_field_t *fields[6];
  for(size_t k = 0; k < 6; k++)
  {
    fields[k] = module->get_f(names[k]);
    if(!curve_field_fits(module, fields[k]) || !fields[k]->header.field_name
       || strcmp(fields[k]->header.field_name, names[k])) return FALSE;
    if(k < 4)
    {
      if(fields[k]->header.type != DT_INTROSPECTION_TYPE_FLOAT
         || fields[k]->header.size != sizeof(float)
         || fields[k]->Float.Min != 0.0f || fields[k]->Float.Max != 1.0f) return FALSE;
      layout->coordinates[k] = fields[k]->header.offset;
    }
    else
    {
      if(fields[k]->header.type != DT_INTROSPECTION_TYPE_INT
         || fields[k]->header.size != sizeof(int)
         || fields[k]->Int.Min > 0 || fields[k]->Int.Max < 0) return FALSE;
      layout->ratios[k - 4] = fields[k]->header.offset;
    }
    for(size_t other = 0; other < k; other++)
      if(fields[k]->header.offset < fields[other]->header.offset + fields[other]->header.size
         && fields[other]->header.offset < fields[k]->header.offset + fields[k]->header.size) return FALSE;
  }
  return TRUE;
}

/* Use the original dimensions-only pixelpipe, as dt_image_get_final_size does.
 * The editor's private develop deliberately has no persistent pipeline and the
 * processed preview can already be cropped/rotated. Neither is a substitute for
 * the actual input ROI of a particular crop instance. Cache exact geometry
 * parameters so ordinary metadata reads do not repeatedly create a pipeline. */
static void update_crop_input_sizes(dt_mobile_engine *engine)
{
  GByteArray *key = g_byte_array_new();
  const int dimensions[] = { engine->develop.image_storage.width, engine->develop.image_storage.height };
  g_byte_array_append(key, (const guint8 *)dimensions, sizeof(dimensions));
  gboolean has_crop = FALSE;
  for(const GList *item = engine->develop.iop; item; item = item->next)
  {
    const dt_iop_module_t *module = item->data;
    gpointer native_roi = NULL;
    if(!strcmp(module->op, "crop")) has_crop = TRUE;
    /* Modules without this exported callback use imageop.c's pass-through
     * implementation. Include every original override, not a hand-maintained
     * list of tools which happen to change dimensions in today's source. */
    if(!module->module || !g_module_symbol(module->module, "modify_roi_out", &native_roi)) continue;
    g_byte_array_append(key, (const guint8 *)module->op, strlen(module->op) + 1);
    const int identity[] = { module->multi_priority, module->enabled, module->params_size };
    g_byte_array_append(key, (const guint8 *)identity, sizeof(identity));
    if(module->params && module->params_size > 0)
      g_byte_array_append(key, module->params, module->params_size);
  }
  GBytes *candidate = g_byte_array_free_to_bytes(key);
  if(engine->crop_geometry_key && g_bytes_equal(candidate, engine->crop_geometry_key))
  {
    g_bytes_unref(candidate);
    return;
  }
  g_clear_pointer(&engine->crop_geometry_key, g_bytes_unref);
  engine->crop_geometry_key = candidate;
  if(!engine->crop_input_sizes)
    engine->crop_input_sizes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  else g_hash_table_remove_all(engine->crop_input_sizes);
  if(!has_crop || dimensions[0] <= 0 || dimensions[1] <= 0) return;

  dt_develop_t develop;
  dt_dev_init(&develop, FALSE);
  dt_dev_load_image(&develop, engine->image_id);
  dt_dev_pixelpipe_t pipe;
  if(dt_dev_pixelpipe_init_dummy(&pipe, dimensions[0], dimensions[1]))
  {
    dt_dev_pixelpipe_set_input(&pipe, &develop, NULL, dimensions[0], dimensions[1], 1.0f);
    dt_dev_pixelpipe_create_nodes(&pipe, &develop);
    dt_dev_pixelpipe_synch_all(&pipe, &develop);
    dt_dev_pixelpipe_get_dimensions(&pipe, &develop, pipe.iwidth, pipe.iheight,
                                    &pipe.processed_width, &pipe.processed_height);
    for(const GList *node = pipe.nodes; node; node = node->next)
    {
      const dt_dev_pixelpipe_iop_t *piece = node->data;
      if(strcmp(piece->module->op, "crop") || piece->buf_in.width <= 0 || piece->buf_in.height <= 0) continue;
      char identifier[128];
      module_identifier(piece->module, identifier, sizeof(identifier));
      int *size = g_new(int, 2);
      size[0] = piece->buf_in.width;
      size[1] = piece->buf_in.height;
      g_hash_table_replace(engine->crop_input_sizes, g_strdup(identifier), size);
    }
    dt_dev_pixelpipe_cleanup(&pipe);
  }
  dt_dev_cleanup(&develop);
}

static void append_crop(dt_mobile_engine *engine, dt_iop_module_t *module, GString *json)
{
  mobile_crop_layout layout;
  if(!crop_layout(module, &layout)) return;
  float coordinates[4];
  int ratios[2];
  for(size_t k = 0; k < 4; k++)
    memcpy(&coordinates[k], (const char *)module->params + layout.coordinates[k], sizeof(float));
  if(!dt_mobile_crop_valid(coordinates[0], coordinates[1], coordinates[2], coordinates[3])) return;
  for(size_t k = 0; k < 2; k++)
    memcpy(&ratios[k], (const char *)module->params + layout.ratios[k], sizeof(int));
  g_string_append(json, ",\"crop\":{\"left\":"); json_number(json, coordinates[0]);
  g_string_append(json, ",\"top\":"); json_number(json, coordinates[1]);
  g_string_append(json, ",\"right\":"); json_number(json, coordinates[2]);
  g_string_append(json, ",\"bottom\":"); json_number(json, coordinates[3]);
  g_string_append(json, ",\"minSize\":"); json_number(json, DT_MOBILE_CROP_MIN_SIZE);
  /* commit_params also treats 0/n as freehand unless n selects either
   * orientation of the original image ratio (1 or -1). */
  const gboolean freehand = ratios[0] == 0 && ratios[1] != 1 && ratios[1] != -1;
  g_string_append_printf(json, ",\"freehand\":%s", freehand ? "true" : "false");
  char identifier[128];
  module_identifier(module, identifier, sizeof(identifier));
  const int *size = engine->crop_input_sizes ? g_hash_table_lookup(engine->crop_input_sizes, identifier) : NULL;
  if(size) g_string_append_printf(json, ",\"inputWidth\":%d,\"inputHeight\":%d", size[0], size[1]);
  g_string_append_c(json, '}');
}

typedef struct mobile_whitebalance_layout
{
  size_t gains[4], preset;
  int channels;
} mobile_whitebalance_layout;

static gboolean whitebalance_layout(dt_iop_module_t *module, mobile_whitebalance_layout *layout)
{
  if(!module || strcmp(module->op, "temperature") || !module->dev || !module->params
     || module->params_size <= 0 || !module->have_introspection
     || !module->get_introspection || !module->get_introspection() || !module->get_f) return FALSE;
  const char *names[] = { "red", "green", "blue", "various", "preset" };
  const dt_introspection_field_t *fields[5];
  for(size_t k = 0; k < 5; k++)
  {
    fields[k] = module->get_f(names[k]);
    if(!curve_field_fits(module, fields[k]) || !fields[k]->header.field_name
       || strcmp(fields[k]->header.field_name, names[k])) return FALSE;
    if(k < 4)
    {
      if(fields[k]->header.type != DT_INTROSPECTION_TYPE_FLOAT
         || fields[k]->header.size != sizeof(float)
         || fields[k]->Float.Min != 0.0f || fields[k]->Float.Max != 8.0f) return FALSE;
      layout->gains[k] = fields[k]->header.offset;
    }
    else
    {
      if(fields[k]->header.type != DT_INTROSPECTION_TYPE_INT
         || fields[k]->header.size != sizeof(int)) return FALSE;
      layout->preset = fields[k]->header.offset;
    }
    for(size_t previous = 0; previous < k; previous++)
      if(fields[k]->header.offset < fields[previous]->header.offset + fields[previous]->header.size
         && fields[previous]->header.offset < fields[k]->header.offset + fields[k]->header.size) return FALSE;
  }
  layout->channels = module->dev->image_storage.flags & DT_IMAGE_4BAYER ? 4 : 3;
  return TRUE;
}

static gboolean whitebalance_available(const dt_iop_module_t *module)
{
  return module && module->dev
    && !(dt_image_monochrome_flags(&module->dev->image_storage) & DT_IMAGE_MONOCHROME);
}

static const char *whitebalance_setting_label(int setting)
{
  /* Stable built-in setting IDs match temperature.c. USER and SPOT describe
   * existing state but are not client-selectable named coefficient sources. */
  switch(setting)
  {
    case 0: return "as shot";
    case 1: return "by spot";
    case 2: return "user defined";
    case 3: return "camera reference";
    case 4: return "as shot to reference";
    default: return "other";
  }
}

static gboolean whitebalance_basis(const dt_iop_module_t *module,
                                  const mobile_whitebalance_layout *layout,
                                  int setting, float gains[4])
{
  if(setting != 0 && setting != 3 && setting != 4) return FALSE;
  const dt_dev_chroma_t *chroma = &module->dev->chroma;
  const double *basis = setting == 3 ? chroma->D65coeffs : chroma->as_shot;
  for(int channel = 0; channel < layout->channels; channel++)
  {
    /* Original named coefficients are not the manual slider range. Preserve
     * valid values above 8, and reject overflow/underflow before any write. */
    if(!isfinite(basis[channel]) || basis[channel] <= 0.0 || basis[channel] > FLT_MAX) return FALSE;
    gains[channel] = (float)basis[channel];
    if(!isnormal(gains[channel]) || gains[channel] <= 0.0f) return FALSE;
  }
  return TRUE;
}

/* Use the same camera matrices and sRGB fallback as temperature.c. A headless
 * module has no gui_data, so the shared numerical core accepts matrices rather
 * than reaching through GUI state. Missing/singular calibration cannot be
 * advertised as a valid Kelvin editor. */
static gboolean whitebalance_matrices(dt_iop_module_t *module,
                                      double XYZ_to_CAM[4][3], double CAM_to_XYZ[3][4])
{
  dt_image_t *image = &module->dev->image_storage;
  if(!dt_image_is_raw(image))
    dt_whitebalance_srgb_matrices(XYZ_to_CAM, CAM_to_XYZ);
  else if(!dt_colorspaces_conversion_matrices_xyz(image->adobe_XYZ_to_CAM,
                                                  image->d65_color_matrix,
                                                  XYZ_to_CAM, CAM_to_XYZ)) return FALSE;
  const int channels = image->flags & DT_IMAGE_4BAYER ? 4 : 3;
  for(int row = 0; row < 4; row++)
  {
    gboolean nonzero = FALSE;
    for(int col = 0; col < 3; col++)
    {
      if(!isfinite(XYZ_to_CAM[row][col]) || !isfinite(CAM_to_XYZ[col][row])) return FALSE;
      if(XYZ_to_CAM[row][col] != 0.0) nonzero = TRUE;
    }
    /* A 3x3 embedded calibration has a zero inactive row. It cannot produce
     * valid four-channel gains for a real four-Bayer image. */
    if(row < channels && !nonzero) return FALSE;
  }
  return TRUE;
}

static void append_whitebalance_temperature(dt_iop_module_t *module,
                                            const mobile_whitebalance_layout *layout,
                                            const float gains[4], GString *json)
{
  double XYZ_to_CAM[4][3], CAM_to_XYZ[3][4];
  if(!whitebalance_matrices(module, XYZ_to_CAM, CAM_to_XYZ)) return;
  double multipliers[4];
  for(int channel = 0; channel < 4; channel++)
  {
    if(channel < layout->channels && (!isnormal(gains[channel]) || gains[channel] <= 0.0f)) return;
    /* The desktop inversion reads all four parameter fields. Its shared core
     * maps an inactive NaN/nonpositive sentinel to zero camera light. */
    multipliers[channel] = gains[channel];
  }
  const cmsCIEXYZ xyz = dt_whitebalance_multipliers_to_XYZ(CAM_to_XYZ, multipliers);
  if(!isfinite(xyz.X) || !isfinite(xyz.Y) || !isfinite(xyz.Z)
     || xyz.X <= 0.0 || xyz.Y <= 0.0 || xyz.Z <= 0.0) return;
  float kelvin, tint;
  dt_whitebalance_XYZ_to_temperature(xyz, &kelvin, &tint);
  if(!isfinite(kelvin) || !isfinite(tint)) return;
  g_string_append(json, ",\"temperature\":{\"kelvin\":"); json_number(json, kelvin);
  g_string_append(json, ",\"tint\":"); json_number(json, tint);
  g_string_append_printf(json, ",\"minKelvin\":%d,\"maxKelvin\":%d",
                         DT_IOP_LOWEST_TEMPERATURE, DT_IOP_HIGHEST_TEMPERATURE);
  g_string_append(json, ",\"minTint\":"); json_number(json, DT_IOP_LOWEST_TINT);
  g_string_append(json, ",\"maxTint\":"); json_number(json, DT_IOP_HIGHEST_TINT);
  g_string_append_c(json, '}');
}

static void append_whitebalance(dt_iop_module_t *module, GString *json)
{
  mobile_whitebalance_layout layout;
  if(!whitebalance_layout(module, &layout) || !whitebalance_available(module)) return;
  float gains[4];
  for(int channel = 0; channel < 4; channel++)
  {
    memcpy(&gains[channel], (const char *)module->params + layout.gains[channel], sizeof(float));
    if(channel < layout.channels && !isfinite(gains[channel])) return; /* Manual batch can repair this state. */
  }
  int setting;
  memcpy(&setting, (const char *)module->params + layout.preset, sizeof(setting));
  g_string_append(json, ",\"whiteBalance\":{\"channels\":[");
  const gboolean cygm = layout.channels == 4 && FILTERS_ARE_CYGM(module->dev->image_storage.buf_dsc.filters);
  const int cygm_order[] = { 2, 3, 1, 0 };
  const char *rgb_labels[] = { "red", "green", "blue", "emerald" };
  const char *cygm_labels[] = { "green", "magenta", "cyan", "yellow" };
  for(int display = 0; display < layout.channels; display++)
  {
    const int channel = cygm ? cygm_order[display] : display;
    if(display) g_string_append_c(json, ',');
    g_string_append_printf(json, "{\"index\":%d,\"label\":", channel);
    json_string(json, cygm ? cygm_labels[channel] : rgb_labels[channel]);
    g_string_append(json, ",\"value\":"); json_number(json, gains[channel]);
    g_string_append_c(json, '}');
  }
  g_string_append(json, "],\"manualMin\":"); json_number(json, 0.001f);
  g_string_append(json, ",\"manualMax\":"); json_number(json, 8.0f);
  g_string_append_printf(json, ",\"setting\":%d,\"settingLabel\":", setting);
  json_string(json, whitebalance_setting_label(setting));
  g_string_append(json, ",\"settings\":[");
  const int settings[] = { 0, 3, 4 };
  gboolean first = TRUE;
  for(size_t k = 0; k < sizeof(settings) / sizeof(settings[0]); k++)
  {
    float basis[4];
    if(!whitebalance_basis(module, &layout, settings[k], basis)) continue;
    if(!first) g_string_append_c(json, ',');
    first = FALSE;
    g_string_append_printf(json, "{\"id\":%d,\"label\":", settings[k]);
    json_string(json, whitebalance_setting_label(settings[k]));
    g_string_append_c(json, '}');
  }
  g_string_append_c(json, ']');
  append_whitebalance_temperature(module, &layout, gains, json);
  g_string_append_c(json, '}');
}

static void sync_temperature_chroma(dt_develop_t *dev)
{
  /* A headless develop has no persistent pixelpipe commit. Derive the same
   * temperature-owned state as temperature.c commit_params after restoring or
   * editing params. Keep original as-shot/D65 bases and adaptation ownership. */
  for(GList *item = dev->iop; item; item = item->next)
  {
    dt_iop_module_t *module = item->data;
    if(strcmp(module->op, "temperature")) continue;
    if(!whitebalance_available(module) || module->hide_enable_button)
    {
      for(int channel = 0; channel < 4; channel++) dev->chroma.wb_coeffs[channel] = 1.0f;
      return; /* The original true-monochrome commit also returns here. */
    }
    mobile_whitebalance_layout layout;
    if(!whitebalance_layout(module, &layout)) return;
    for(int channel = 0; channel < 4; channel++)
    {
      float gain;
      memcpy(&gain, (const char *)module->params + layout.gains[channel], sizeof(gain));
      dev->chroma.wb_coeffs[channel] = module->enabled ? gain : 1.0f;
    }
    int setting;
    memcpy(&setting, (const char *)module->params + layout.preset, sizeof(setting));
    dev->chroma.late_correction = setting == 4;
    dev->chroma.temperature = module->enabled ? module : NULL;
    return;
  }
}

/* All entry points take locks in the same order. Cancellation deliberately
 * takes neither lock so it can interrupt an active upstream pixelpipe. */
static dt_mobile_status begin_operation(dt_mobile_engine *engine, char *error, size_t size)
{
  pthread_mutex_lock(&engine->operation_lock);
  if(atomic_load(&engine->cancelled))
  {
    pthread_mutex_unlock(&engine->operation_lock);
    return engine_fail(error, size, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
  }
  pthread_mutex_lock(&engine_work_lock);
  sync_temperature_chroma(&engine->develop);
  return DT_MOBILE_OK;
}
static void end_operation(dt_mobile_engine *engine)
{
  pthread_mutex_unlock(&engine_work_lock);
  pthread_mutex_unlock(&engine->operation_lock);
}

dt_mobile_status dt_mobile_engine_modules(dt_mobile_engine *engine, char **json,
                                         char *error, size_t error_size)
{
  *json = NULL;
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  update_crop_input_sizes(engine);
  GString *description = g_string_new("[");
  gboolean first = TRUE;
  for(GList *item = engine->develop.iop; item; item = g_list_next(item))
  {
    dt_iop_module_t *module = item->data;
    /* Internal pipeline/history sentinels remain in native topology/history,
     * but are not user editing tools (notably mask_manager's dummy field). */
    if(dt_iop_is_hidden(module) || !dt_mobile_extended_parameter_applicable(module)) continue;
    char identifier[128];
    module_identifier(module, identifier, sizeof(identifier));
    if(!first) g_string_append_c(description, ',');
    first = FALSE;
    g_string_append(description, "{\"operation\":"); json_string(description, identifier);
    g_string_append(description, ",\"name\":");
    /* Native preset replay may normalize a base module's private automatic
     * label without changing any settings. The upstream instance-name helper
     * keeps explicit user names and extra instances, and omits that internal
     * base-module bookkeeping from the tool's visible identity. */
    const char *instance_name = dt_iop_get_instance_name(module);
    gchar *display_name = module->multi_name_hand_edited ? g_strdup(instance_name)
      : dt_util_localize_segmented_name(instance_name, FALSE);
    gchar *label = display_name[0]
      ? g_strdup_printf("%s · %s", module->name(), display_name) : g_strdup(module->name());
    json_string(description, label);
    g_free(display_name);
    g_free(label);
    g_string_append_printf(description, ",\"enabled\":%s,\"canDisable\":%s,\"canCreateInstance\":%s,\"parameters\":[",
                           module->enabled ? "true" : "false",
                           module->hide_enable_button ? "false" : "true",
                           can_create_instance(engine, module) ? "true" : "false");
    parameter_json fields = { description, TRUE };
    if(strcmp(module->op, "lens") && module->have_introspection && module->get_introspection())
      visit_fields(module, module->get_introspection()->field, 0, "", 0, append_parameter, &fields);
    append_mobile_lens_parameters(module, &fields);
    append_mobile_blending_parameters(module, &fields);
    append_mobile_zone_parameters(module, &fields);
    append_mobile_mask_parameters(module, &fields);
    append_mobile_liquify_parameters(module, &fields);
    g_string_append(description, "],\"curves\":[");
    append_curves(module, description);
    g_string_append_c(description, ']');
    append_rgb_curve_options(module, description);
    append_whitebalance(module, description);
    append_crop(engine, module, description);
    append_mobile_lens_correction(module, description);
    append_mobile_text_parameters(module, description);
    g_string_append_printf(description, ",\"canReset\":%s",
      module->default_params && module->params && module->params_size > 0 ? "true" : "false");
    g_string_append_c(description, '}');
  }
  g_string_append_c(description, ']');
  status = finish_json(description, json, error, error_size);
  end_operation(engine);
  return status;
}

static void save_module(dt_mobile_engine *engine, dt_iop_module_t *module, gboolean enable)
{
  prepare_mobile_lens_parameters(module);
  /* no_image=TRUE avoids touching GTK viewports in this headless develop. */
  dt_dev_add_history_item_ext(&engine->develop, module, enable, TRUE);
  sync_temperature_chroma(&engine->develop);
  sync_mobile_overlay_dependencies(&engine->develop);
  dt_dev_write_history(&engine->develop);
  record_history_state(engine);
}

/* A live slider gesture can deliver several fully rendered frames. Keep the
 * original native history/database current on each frame, but replace only
 * this gesture's latest adapter snapshot so one Undo restores its start. */
static void save_tone_target(dt_mobile_engine *engine, dt_iop_module_t *module, uint64_t gesture)
{
  const gboolean replace = engine->tone_gesture == gesture
    && engine->tone_gesture_module == module
    && engine->tone_gesture_position == engine->history_position
    && engine->history_position > 0
    && engine->history_position + 1 == engine->history_states->len;
  dt_dev_add_history_item_ext(&engine->develop, module, FALSE, TRUE);
  sync_temperature_chroma(&engine->develop);
  sync_mobile_overlay_dependencies(&engine->develop);
  dt_dev_write_history(&engine->develop);
  if(replace)
  {
    mobile_history_state *state = capture_history_state(&engine->develop);
    mobile_history_state *previous = g_ptr_array_index(engine->history_states, engine->history_position);
    g_ptr_array_index(engine->history_states, engine->history_position) = state;
    free_history_state(previous);
    reclaim_retired_instances(engine);
  }
  else record_history_state(engine);
  engine->tone_gesture = gesture;
  engine->tone_gesture_module = module;
  engine->tone_gesture_position = engine->history_position;
}

dt_mobile_status dt_mobile_engine_new_instance(dt_mobile_engine *engine, const char *identifier,
                                               char **new_identifier, char *error, size_t error_size)
{
  *new_identifier = NULL;
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  dt_iop_module_t *base = find_module(engine, identifier);
  if(!can_create_instance(engine, base))
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_UNSUPPORTED,
                         "unknown module or this module cannot create another instance");
  else if(!dt_ioppr_get_iop_order_link(engine->develop.iop_order_list, base->op, base->multi_priority))
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_ENGINE,
                         "the source instance has no valid pipeline position");
  else
  {
    const int priority = highest_instance_priority(engine, base) + 1;
    char result[128];
    const int length = snprintf(result, sizeof(result), "%s@%d", base->op, priority);
    char *owned_identifier = length > 0 && (size_t)length < sizeof(result) ? malloc((size_t)length + 1) : NULL;
    if(!owned_identifier)
      status = engine_fail(error, error_size, DT_MOBILE_ERROR_OUT_OF_MEMORY,
                           "unable to allocate the instance identifier");
    else
    {
      memcpy(owned_identifier, result, (size_t)length + 1);
      /* This is the original desktop creation/order helper. Its ordinary
       * wrappers keep their original priority policy; the floor prevents an
       * identifier from being reused after a mobile redo branch is discarded. */
      dt_iop_module_t *module = dt_dev_module_duplicate_at_priority(&engine->develop, base, TRUE, priority);
      if(!module)
      {
        free(owned_identifier);
        status = engine_fail(error, error_size, DT_MOBILE_ERROR_ENGINE,
                             "the original engine could not create another instance");
      }
      else
      {
        dt_iop_reload_defaults(module);
        if(!engine->instance_priorities)
          engine->instance_priorities = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        g_hash_table_replace(engine->instance_priorities, g_strdup(module->op), GINT_TO_POINTER(priority));
        save_module(engine, module, TRUE);
        *new_identifier = owned_identifier;
      }
    }
  }
  end_operation(engine);
  return status;
}

static void restore_active_modules(dt_develop_t *dev, const mobile_history_state *state)
{
  /* Reset cross-module raster registrations before changing topology. History
   * replay below rebuilds the active registrations through the original blend
   * commit helper. Retired modules must not retain pointers into a later branch. */
  const GList *lists[] = { dev->iop, dev->alliop };
  for(size_t k = 0; k < sizeof(lists) / sizeof(lists[0]); k++)
    for(const GList *item = lists[k]; item; item = item->next)
    {
      dt_iop_module_t *module = item->data;
      g_hash_table_remove_all(module->raster_mask.source.users);
      module->raster_mask.sink.source = NULL;
      module->raster_mask.sink.id = INVALID_MASKID;
    }
  for(const GList *item = dev->iop; item; item = item->next)
    if(!g_list_find(state->active_modules, item->data))
      dev->alliop = g_list_append(dev->alliop, item->data);
  for(const GList *item = state->active_modules; item; item = item->next)
    dev->alliop = g_list_remove(dev->alliop, item->data);
  g_list_free(dev->iop);
  dev->iop = g_list_copy(state->active_modules);
}

dt_mobile_status dt_mobile_engine_history(dt_mobile_engine *engine, char **json,
                                         char *error, size_t error_size)
{
  *json = NULL;
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  const guint length = engine->history_states->len - 1;
  GString *description = g_string_new(NULL);
  g_string_append_printf(description,
    "{\"canUndo\":%s,\"canRedo\":%s,\"position\":%u,\"length\":%u,\"limit\":%u}",
    engine->history_position ? "true" : "false",
    engine->history_position < length ? "true" : "false",
    engine->history_position, length, MOBILE_HISTORY_LIMIT);
  status = finish_json(description, json, error, error_size);
  end_operation(engine);
  return status;
}

static dt_mobile_status move_history(dt_mobile_engine *engine, int direction,
                                     char *error, size_t error_size)
{
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  clear_tone_gesture(engine);
  const guint length = engine->history_states->len - 1;
  if((direction < 0 && !engine->history_position)
     || (direction > 0 && engine->history_position >= length))
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                         direction < 0 ? "no earlier edit is available" : "no later edit is available");
  else
  {
    const guint target = direction < 0 ? engine->history_position - 1 : engine->history_position + 1;
    const mobile_history_state *state = g_ptr_array_index(engine->history_states, target);
    /* Clone before replacing the live list: each stored state remains owned by
     * the undo window, independently of the active develop history. */
    GList *history = dt_history_duplicate(state->history);
    GList *order = dt_ioppr_iop_order_copy_deep(state->iop_order);
    dt_develop_t *dev = &engine->develop;
    dt_pthread_mutex_lock(&dev->history_mutex);
    restore_active_modules(dev, state);
    g_list_free_full(dev->history, dt_dev_free_history_item);
    g_list_free_full(dev->iop_order_list, free);
    dev->history = history;
    dev->iop_order_list = order;
    dev->iop_order_version = state->iop_order_version;
    /* Equal counts may still describe different snapshots. Suppress pop's
     * count-difference mask shortcut and restore the actual active forms once. */
    dev->history_end = state->history_end;
    dt_dev_pop_history_items_ext(dev, state->history_end);
    sync_raster_mask_registrations(dev);
    sync_mobile_overlay_dependencies(dev);
    dev->chroma = state->chroma;
    sync_temperature_chroma(dev);
    GList *forms = NULL;
    guint index = 0;
    for(GList *item = dev->history; item && index < (guint)state->history_end; item = item->next, index++)
    {
      const dt_dev_history_item_t *entry = item->data;
      if(entry->forms) forms = entry->forms;
    }
    dt_masks_replace_current_forms(dev, forms);
    /* The desktop keeps retired forms alive for outstanding GUI references.
     * This private headless develop has none, and its independent history
     * copies own their forms. Release retirement copies between restores so
     * repeated mask undo/redo cannot grow beyond the bounded undo window. */
    g_list_free_full(dev->allforms, (GDestroyNotify)dt_masks_free_form);
    dev->allforms = NULL;
    dev->focus_hash = FALSE;
    dt_dev_pipe_synch_all(dev);
    dt_dev_invalidate_all(dev);
    /* Export/preview construct fresh upstream pipelines from this database;
     * the GUI-only reload/rebuild helpers must not run on a headless develop. */
    dt_dev_write_history(dev);
    dt_pthread_mutex_unlock(&dev->history_mutex);
    engine->history_position = target;
  }
  end_operation(engine);
  return status;
}

dt_mobile_status dt_mobile_engine_undo(dt_mobile_engine *engine, char *error, size_t error_size)
{ return move_history(engine, -1, error, error_size); }

dt_mobile_status dt_mobile_engine_redo(dt_mobile_engine *engine, char *error, size_t error_size)
{ return move_history(engine, 1, error, error_size); }

dt_mobile_status dt_mobile_engine_set_curve(dt_mobile_engine *engine, const char *identifier,
                                           int channel, int interpolation,
                                           const dt_mobile_curve_point *points, size_t count,
                                           char *error, size_t error_size)
{
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  dt_iop_module_t *module = find_module(engine, identifier);
  mobile_curve_layout layout;
  if(!curve_layout(module, &layout))
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_UNSUPPORTED, "this module has no supported curve editor");
  else if(channel < 0 || channel >= layout.editable_channels)
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_UNSUPPORTED, "this curve channel is reserved or linked");
  else if(count > (size_t)layout.max_nodes || !valid_curve_points(points, count, interpolation))
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT, "curve nodes or interpolation are invalid");
  else
  {
    /* RGB processing reads all three channel counts, even in linked mode.
     * Do not enable an imported module with malformed untouched channels. */
    for(int other = 0; other < layout.channels && status == DT_MOBILE_OK; other++)
      if(other != channel)
      {
        dt_mobile_curve_point original[MOBILE_CURVE_MAX_NODES];
        int original_count, original_interpolation;
        if(!read_curve(module, &layout, other, original, &original_count, &original_interpolation))
          status = engine_fail(error, error_size, DT_MOBILE_ERROR_UNSUPPORTED,
                               "an untouched curve channel requires a supported history state");
      }
    if(status == DT_MOBILE_OK)
    {
      char *params = module->params;
      for(size_t k = 0; k < count; k++)
      {
        char *node = params + layout.nodes + channel * layout.channel_stride + k * layout.node_stride;
        memcpy(node + layout.x, &points[k].x, sizeof(float));
        memcpy(node + layout.y, &points[k].y, sizeof(float));
      }
      const int nodes = (int)count;
      memcpy(params + layout.counts + channel * sizeof(int), &nodes, sizeof(nodes));
      memcpy(params + layout.types + channel * sizeof(int), &interpolation, sizeof(interpolation));
      save_module(engine, module, TRUE);
    }
  }
  end_operation(engine);
  return status;
}

dt_mobile_status dt_mobile_engine_set_rgb_curve_options(dt_mobile_engine *engine,
                                                       const char *identifier, int mode,
                                                       int preserve_colors,
                                                       char *error, size_t error_size)
{
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  dt_iop_module_t *module = find_module(engine, identifier);
  mobile_curve_layout curves;
  mobile_rgb_curve_options_layout options;
  if(!rgb_curve_options_layout(module, &curves, &options))
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_UNSUPPORTED,
                         "this module has no supported RGB curve options");
  else if(!enum_has_value(options.mode, mode) || !enum_has_value(options.preserve, preserve_colors))
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                         "RGB curve mode or color-preservation choice is invalid");
  else
  {
    char *params = module->params;
    gboolean copy_channels = options.current_mode != mode && mode == 1;
    for(int channel = 1; channel < MOBILE_CURVE_CHANNELS && copy_channels; channel++)
    {
      dt_mobile_curve_point points[MOBILE_CURVE_MAX_NODES];
      int count = 0, interpolation = 0;
      /* The layout validation above has already checked every channel. */
      if(!read_curve(module, &curves, channel, points, &count, &interpolation)) copy_channels = FALSE;
      for(int node = 0; node < count && copy_channels; node++)
        if(points[node].x != points[node].y) copy_channels = FALSE;
    }
    if(copy_channels)
      for(int channel = 1; channel < MOBILE_CURVE_CHANNELS; channel++)
      {
        /* gui_changed in rgbcurve.c copies the entire fixed node row, then
         * count and interpolation, only when G and B are both identity. */
        memcpy(params + curves.nodes + channel * curves.channel_stride,
               params + curves.nodes, curves.channel_stride);
        memcpy(params + curves.counts + channel * sizeof(int), params + curves.counts, sizeof(int));
        memcpy(params + curves.types + channel * sizeof(int), params + curves.types, sizeof(int));
      }
    memcpy(params + options.mode->header.offset, &mode, sizeof(mode));
    memcpy(params + options.preserve->header.offset, &preserve_colors, sizeof(preserve_colors));
    save_module(engine, module, TRUE);
  }
  end_operation(engine);
  return status;
}

dt_mobile_status dt_mobile_engine_set_whitebalance_gains(dt_mobile_engine *engine,
                                                       const char *identifier,
                                                       const float *gains, size_t count,
                                                       char *error, size_t error_size)
{
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  dt_iop_module_t *module = find_module(engine, identifier);
  mobile_whitebalance_layout layout;
  if(!whitebalance_layout(module, &layout) || !whitebalance_available(module))
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_UNSUPPORTED,
                         "this module or monochrome image has no supported white-balance editor");
  else if(!gains || count != (size_t)layout.channels)
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                         "white-balance gains must match all active channels");
  else
  {
    for(size_t channel = 0; channel < count; channel++)
      if(!isfinite(gains[channel]) || gains[channel] < 0.001f || gains[channel] > 8.0f)
      {
        status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                             "manual white-balance gains must be within 0.001 to 8");
        break;
      }
    if(status == DT_MOBILE_OK)
    {
      for(size_t channel = 0; channel < count; channel++)
        memcpy((char *)module->params + layout.gains[channel], &gains[channel], sizeof(float));
      const int user_setting = 2;
      memcpy((char *)module->params + layout.preset, &user_setting, sizeof(user_setting));
      /* save_module also derives late_correction=FALSE and the enabled
       * temperature coefficients before capturing this single undo step. */
      save_module(engine, module, TRUE);
    }
  }
  end_operation(engine);
  return status;
}

dt_mobile_status dt_mobile_engine_set_whitebalance_temperature(dt_mobile_engine *engine,
                                                             const char *identifier,
                                                             double kelvin, double tint,
                                                             char *error, size_t error_size)
{
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  dt_iop_module_t *module = find_module(engine, identifier);
  mobile_whitebalance_layout layout;
  double XYZ_to_CAM[4][3], CAM_to_XYZ[3][4];
  if(!whitebalance_layout(module, &layout) || !whitebalance_available(module)
     || !whitebalance_matrices(module, XYZ_to_CAM, CAM_to_XYZ))
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_UNSUPPORTED,
                         "this image has no valid white-balance temperature calibration");
  else if(!isfinite(kelvin) || kelvin < DT_IOP_LOWEST_TEMPERATURE || kelvin > DT_IOP_HIGHEST_TEMPERATURE
          || !isfinite(tint) || tint < DT_IOP_LOWEST_TINT || tint > DT_IOP_HIGHEST_TINT)
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                         "white-balance temperature or tint is outside the desktop range");
  else
  {
    double multipliers[4];
    dt_whitebalance_XYZ_to_multipliers(XYZ_to_CAM,
      dt_whitebalance_temperature_tint_to_XYZ(kelvin, tint), multipliers);
    /* Exactly the desktop callback's green normalization. Validate all active
     * results before writing; unusual calibrations and extreme tints can yield
     * nonpositive camera channels. Never partially update the module. */
    const double green = multipliers[1];
    float gains[4];
    for(int channel = 0; channel < layout.channels; channel++)
    {
      const double normalized = channel == 1 ? 1.0 : multipliers[channel] / green;
      if(!isfinite(green) || green <= 0.0 || !isfinite(normalized)
         || normalized <= 0.0 || normalized > FLT_MAX
         || !isnormal((float)normalized))
      {
        status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                             "temperature and tint do not produce valid camera white-balance gains");
        break;
      }
      gains[channel] = (float)normalized;
    }
    if(status == DT_MOBILE_OK)
    {
      for(int channel = 0; channel < layout.channels; channel++)
        memcpy((char *)module->params + layout.gains[channel], &gains[channel], sizeof(float));
      const int user_setting = 2;
      memcpy((char *)module->params + layout.preset, &user_setting, sizeof(user_setting));
      save_module(engine, module, TRUE);
    }
  }
  end_operation(engine);
  return status;
}

dt_mobile_status dt_mobile_engine_set_whitebalance_setting(dt_mobile_engine *engine,
                                                         const char *identifier, int setting,
                                                         char *error, size_t error_size)
{
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  dt_iop_module_t *module = find_module(engine, identifier);
  mobile_whitebalance_layout layout;
  float gains[4];
  if(!whitebalance_layout(module, &layout) || !whitebalance_available(module))
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_UNSUPPORTED,
                         "this module or monochrome image has no supported white-balance editor");
  else if(setting != 0 && setting != 3 && setting != 4)
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                         "unknown built-in white-balance setting");
  else if(!whitebalance_basis(module, &layout, setting, gains))
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_UNSUPPORTED,
                         "this image has no valid native basis for the requested white-balance setting");
  else
  {
    for(int channel = 0; channel < layout.channels; channel++)
      memcpy((char *)module->params + layout.gains[channel], &gains[channel], sizeof(float));
    memcpy((char *)module->params + layout.preset, &setting, sizeof(setting));
    save_module(engine, module, TRUE);
  }
  end_operation(engine);
  return status;
}

dt_mobile_status dt_mobile_engine_set_crop(dt_mobile_engine *engine, const char *identifier,
                                          float left, float top, float right, float bottom,
                                          char *error, size_t error_size)
{
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  dt_iop_module_t *module = find_module(engine, identifier);
  mobile_crop_layout layout;
  if(!crop_layout(module, &layout))
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_UNSUPPORTED, "this module has no supported crop editor");
  else if(!dt_mobile_crop_valid(left, top, right, bottom))
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT, "crop rectangle is invalid");
  else
  {
    const float coordinates[] = { left, top, right, bottom };
    const int freehand = 0;
    for(size_t k = 0; k < 4; k++)
      memcpy((char *)module->params + layout.coordinates[k], &coordinates[k], sizeof(float));
    /* src/iop/crop.c commit_params treats 0/0 as freehand. Keeping a previous
     * fixed ratio could alter the requested rectangle again during export. */
    for(size_t k = 0; k < 2; k++)
      memcpy((char *)module->params + layout.ratios[k], &freehand, sizeof(freehand));
    save_module(engine, module, TRUE);
  }
  end_operation(engine);
  return status;
}

dt_mobile_status dt_mobile_engine_set_module_params(dt_mobile_engine *engine, const char *identifier,
                                                   const void *params, size_t size,
                                                   char *error, size_t error_size)
{
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  dt_iop_module_t *module = find_module(engine, identifier);
  if(!module) status = engine_fail(error, error_size, DT_MOBILE_ERROR_UNSUPPORTED, "unknown processing module");
  else if(size != (size_t)module->params_size)
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                         "module parameter size does not match this engine version");
  else
  {
    memcpy(module->params, params, size);
    save_module(engine, module, TRUE);
  }
  end_operation(engine);
  return status;
}

typedef struct parameter_update { const char *name; double value; gboolean found, valid; } parameter_update;
static void update_parameter(dt_iop_module_t *module, const dt_introspection_field_t *field,
                             size_t offset, const char *path, void *data)
{
  parameter_update *update = data;
  if(strcmp(update->name, path)
     || !dt_mobile_parameter_allowed(module->op, path, field->header.type)) return;
  update->found = TRUE;
  void *destination = (char *)module->params + offset;
  long double current, minimum, maximum;
  const char *type;
  double value = update->value;
  if(!isfinite(value)) return;
  if(field->header.type == DT_INTROSPECTION_TYPE_FLOAT)
  {
    /* Validate the value that will actually be stored. A decimal such as 0.1
     * must compare to a float lower bound as the same float, rather than being
     * rejected because its double representation is slightly smaller. Reject
     * overflow before converting; integer/enum fields retain exact semantics. */
    if(value < -FLT_MAX || value > FLT_MAX) return;
    value = (double)(float)value;
    if(!isfinite(value)) return;
  }
  if(!number_info(field, destination, &current, &minimum, &maximum, &type)) return;
  dt_mobile_parameter_limits(module->op, path, &minimum, &maximum);
  dt_mobile_array_parameter_dynamic_limits(module, path, &minimum, &maximum);
  if((long double)value < minimum || (long double)value > maximum
     || (field->header.type != DT_INTROSPECTION_TYPE_FLOAT
         && field->header.type != DT_INTROSPECTION_TYPE_DOUBLE && trunc(value) != value)
     || !dt_mobile_parameter_value_valid(module, path, value)) return;
  if(field->header.type == DT_INTROSPECTION_TYPE_ENUM)
  {
    gboolean present = FALSE;
    for(size_t k = 0; k < enum_entry_count(field); k++)
      if(value == field->Enum.values[k].value) present = TRUE;
    if(!present) return;
  }
#define WRITE_NUMBER(tag, ctype) \
  case DT_INTROSPECTION_TYPE_##tag: *(ctype *)destination = (ctype)value; break
  switch(field->header.type)
  {
    WRITE_NUMBER(FLOAT, float);
    WRITE_NUMBER(DOUBLE, double);
    WRITE_NUMBER(CHAR, char);
    WRITE_NUMBER(INT8, int8_t);
    WRITE_NUMBER(UINT8, uint8_t);
    WRITE_NUMBER(SHORT, short);
    WRITE_NUMBER(USHORT, unsigned short);
    WRITE_NUMBER(INT, int);
    WRITE_NUMBER(UINT, unsigned int);
    WRITE_NUMBER(LONG, long);
    WRITE_NUMBER(ULONG, unsigned long);
    WRITE_NUMBER(BOOL, gboolean);
    WRITE_NUMBER(ENUM, int);
    default: return;
  }
#undef WRITE_NUMBER
  update->valid = TRUE;
}

dt_mobile_status dt_mobile_engine_set_parameter(dt_mobile_engine *engine, const char *identifier,
                                               const char *parameter, double value,
                                               char *error, size_t error_size)
{
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  dt_iop_module_t *module = find_module(engine, identifier);
  if(module && !strcmp(parameter, "module.reset"))
  {
    if(value != 1.0 || !module->params || !module->default_params || module->params_size <= 0
       || dt_iop_is_hidden(module))
      status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT, "this tool cannot reset settings");
    else
    {
      const mobile_mask_sync_function sync = mobile_mask_special(module) ? mobile_mask_sync(module) : NULL;
      const dt_masks_form_t *group = mobile_mask_special(module) ? mobile_mask_group(module) : NULL;
      mobile_mask_sync_function retouch_reset = NULL;
      if(!strcmp(module->op, "retouch") && module->module)
        g_module_symbol(module->module, "dt_iop_mobile_retouch_reset", (gpointer *)&retouch_reset);
      if(group && group->points && (!sync || (!strcmp(module->op, "retouch") && !retouch_reset)))
        status = engine_fail(error, error_size, DT_MOBILE_ERROR_UNSUPPORTED, "reset requires the original mask form reset operations");
      else
      {
        if(retouch_reset) retouch_reset(module);
        else memcpy(module->params, module->default_params, (size_t)module->params_size);
        /* Retouch and spots keep their original mask group when resetting
         * settings. Rebuild their per-form parameter bookkeeping before any
         * rendering can access those preserved forms. */
        if(sync) sync(module);
        /* A reset is one atomic history item, preserving masks, blending and
         * current enabled state just like resetting settings without power-on. */
        save_module(engine, module, FALSE);
      }
    }
    end_operation(engine);
    return status;
  }
  const int liquify_update = module ? update_mobile_liquify_parameter(module, parameter, value) : 0;
  if(liquify_update)
  {
    if(liquify_update < 0)
      status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT, "unsupported liquify point or warp value");
    else save_module(engine, module, TRUE);
    end_operation(engine);
    return status;
  }
  const int mask_update = module ? update_mobile_mask_parameter(module, parameter, value) : 0;
  if(mask_update)
  {
    if(mask_update < 0)
      status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT, "unsupported mask shape or geometry value");
    else
    {
      /* Forms must be captured in native history before session snapshots;
       * a regular parameter history item would omit their deep copy. */
      prepare_mobile_lens_parameters(module);
      dt_dev_add_masks_history_item_ext(&engine->develop, module, TRUE, TRUE);
      sync_temperature_chroma(&engine->develop);
      dt_dev_write_history(&engine->develop);
      record_history_state(engine);
    }
    end_operation(engine);
    return status;
  }
  const int zone_update = module ? update_mobile_zone_parameter(module, parameter, value) : 0;
  if(zone_update)
  {
    if(zone_update < 0)
      status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT, "unsupported zone partition or boundary value");
    else save_module(engine, module, TRUE);
    end_operation(engine);
    return status;
  }
  const int blend_update = module ? update_mobile_blending_parameter(module, parameter, value) : 0;
  if(blend_update)
  {
    if(blend_update < 0)
      status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT, "unsupported blending value");
    else save_module(engine, module, TRUE);
    end_operation(engine);
    return status;
  }
  const int lens_update = module ? update_mobile_lens_parameter(module, parameter, value) : 0;
  if(lens_update)
  {
    if(lens_update < 0)
      status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                           "this lens correction is unavailable or its settings are invalid");
    else save_module(engine, module, TRUE);
    end_operation(engine);
    return status;
  }
  parameter_update update = { parameter, value, FALSE, FALSE };
  if(module && module->have_introspection && module->get_introspection())
    visit_fields(module, module->get_introspection()->field, 0, "", 0, update_parameter, &update);
  if(!module || !update.found)
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_UNSUPPORTED, "unknown module or numeric parameter");
  else if(!update.valid)
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                         "parameter is outside its supported range, choices or invariants");
  else save_module(engine, module, TRUE);
  end_operation(engine);
  return status;
}

dt_mobile_status dt_mobile_engine_set_text_parameter(dt_mobile_engine *engine, const char *identifier,
    const char *parameter, const char *value, char *error, size_t error_size)
{
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  dt_iop_module_t *module = find_module(engine, identifier);
  int updated = module ? update_mobile_lens_text_parameter(module, parameter, value) : 0;
  if(!updated && module) updated = update_mobile_text_parameter(module, parameter, value);
  if(!updated)
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_UNSUPPORTED, "unknown module or text parameter");
  else if(updated < 0)
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT, "text exceeds the native field or has invalid contents");
  else save_module(engine, module, TRUE);
  end_operation(engine);
  return status;
}

dt_mobile_status dt_mobile_engine_set_enabled(dt_mobile_engine *engine, const char *identifier, int enabled,
                                             char *error, size_t error_size)
{
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  dt_iop_module_t *module = find_module(engine, identifier);
  if(!module) status = engine_fail(error, error_size, DT_MOBILE_ERROR_UNSUPPORTED, "unknown processing module");
  else if(!strcmp(module->op, "temperature") && !whitebalance_available(module))
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_UNSUPPORTED,
                         "white balance is disabled for this true-monochrome image");
  else if(!enabled && module->hide_enable_button)
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT, "this pipeline module must remain enabled");
  else
  {
    module->enabled = enabled != 0;
    save_module(engine, module, FALSE);
  }
  end_operation(engine);
  return status;
}

dt_mobile_status dt_mobile_engine_tone_equalizer_sample(dt_mobile_engine *engine, const char *identifier,
                                                        double x, double y, double *exposure,
                                                        char *error, size_t error_size)
{
  *exposure = NAN;
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  dt_iop_module_t *module = find_module(engine, identifier);
  if(!module || strcmp(module->op, "toneequal") || !module->enabled)
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                         "select an enabled tone equalizer instance before sampling");
  else if(engine->preview_max_width < 1 || engine->preview_max_height < 1)
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                         "render a preview before selecting a tone");
  else
  {
    dt_imageio_toneequal_sample_t sample = { .multi_priority = module->multi_priority, .x = x, .y = y };
    const gboolean failed = dt_imageio_toneequal_sample(engine->image_id,
                                                        engine->preview_max_width, engine->preview_max_height,
                                                        &sample, engine->preview_cancel);
    if(atomic_load(&engine->cancelled))
      status = engine_fail(error, error_size, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
    else if(failed)
      status = engine_fail(error, error_size, DT_MOBILE_ERROR_PROCESSING,
                           "the selected point has no usable tone exposure; select another image point");
    else *exposure = sample.exposure;
  }
  end_operation(engine);
  return status;
}

dt_mobile_status dt_mobile_engine_tone_equalizer_adjust(dt_mobile_engine *engine, const char *identifier,
                                                        double exposure, double offset,
                                                        char *error, size_t error_size)
{
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  dt_iop_module_t *module = find_module(engine, identifier);
  typedef int (*adjust_function)(dt_iop_module_t *, double, double);
  adjust_function adjust = NULL;
  if(!module || strcmp(module->op, "toneequal") || !module->enabled || !module->module
     || !g_module_symbol(module->module, "dt_iop_mobile_toneequal_adjust", (gpointer *)&adjust))
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                         "select an enabled tone equalizer instance before adjusting a tone");
  else
  {
    const int changed = adjust(module, exposure, offset);
    if(changed < 0)
      status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                           "the tone adjustment is out of range or its interpolation is unstable");
    else if(changed) save_module(engine, module, FALSE);
  }
  end_operation(engine);
  return status;
}

dt_mobile_status dt_mobile_engine_tone_equalizer_output(dt_mobile_engine *engine, const char *identifier,
                                                        double exposure, double *output_exposure,
                                                        char *error, size_t error_size)
{
  *output_exposure = NAN;
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  dt_iop_module_t *module = find_module(engine, identifier);
  typedef int (*output_function)(dt_iop_module_t *, double, double *);
  output_function output = NULL;
  if(!module || strcmp(module->op, "toneequal") || !module->enabled || !module->module
     || !g_module_symbol(module->module, "dt_iop_mobile_toneequal_output", (gpointer *)&output))
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                         "select an enabled tone equalizer instance before reading its output");
  else if(output(module, exposure, output_exposure) < 0)
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                         "the tone output cannot be evaluated for these settings");
  end_operation(engine);
  return status;
}

dt_mobile_status dt_mobile_engine_tone_equalizer_set_target(dt_mobile_engine *engine, const char *identifier,
                                                            double exposure, double target, uint64_t gesture,
                                                            double *output_exposure,
                                                            char *error, size_t error_size)
{
  *output_exposure = NAN;
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  dt_iop_module_t *module = find_module(engine, identifier);
  typedef int (*target_function)(dt_iop_module_t *, double, double, double *);
  target_function set_target = NULL;
  if(!module || strcmp(module->op, "toneequal") || !module->enabled || !module->module
     || !g_module_symbol(module->module, "dt_iop_mobile_toneequal_set_target", (gpointer *)&set_target))
    status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                         "select an enabled tone equalizer instance before adjusting a tone");
  else
  {
    if(engine->tone_gesture != gesture || engine->tone_gesture_module != module)
      clear_tone_gesture(engine);
    const int changed = set_target(module, exposure, target, output_exposure);
    if(changed < 0)
      status = engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                           "the tone target is out of range or its interpolation is unstable");
    else if(changed) save_tone_target(engine, module, gesture);
  }
  end_operation(engine);
  return status;
}

dt_mobile_status dt_mobile_engine_export_formats(dt_mobile_engine *engine, char **json,
                                                char *error, size_t error_size)
{
  *json = NULL;
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  GString *description = g_string_new("[");
  gboolean first = TRUE;
  for(GList *item = darktable.imageio->plugins_format; item; item = g_list_next(item))
  {
    dt_imageio_module_format_t *format = item->data;
    if(!strcmp(format->plugin_name, "copy")) continue;
    dt_imageio_module_data_t *params = format->get_params(format);
    if(!params) continue;
    if(!first) g_string_append_c(description, ',');
    first = FALSE;
    g_string_append(description, "{\"id\":"); json_string(description, format->plugin_name);
    g_string_append(description, ",\"name\":"); json_string(description, format->name());
    g_string_append(description, ",\"extension\":"); json_string(description, format->extension(params));
    g_string_append(description, ",\"mimeType\":"); json_string(description, format->mime(params));
    g_string_append_printf(description, ",\"bitDepth\":%d,\"options\":", format->bpp(params));
    dt_mobile_export_append_options(description, format->plugin_name);
    g_string_append_c(description, '}');
    format->free_params(format, params);
  }
  g_string_append_c(description, ']');
  status = finish_json(description, json, error, error_size);
  end_operation(engine);
  return status;
}

dt_mobile_status dt_mobile_engine_export(dt_mobile_engine *engine, const char *path,
                                        const char *format_name, int quality,
                                        char *error, size_t error_size)
{
  if(quality < 0 || quality > 100)
    return engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT, "invalid export quality");
  dt_mobile_export_settings settings = DT_MOBILE_EXPORT_SETTINGS_INIT;
  /* The original API accepts 0..100, while the desktop JPEG encoder clamps
   * values below 5. Keep that behavior for its callers. */
  settings.quality = quality < 5 ? 5 : quality;
  return dt_mobile_engine_export_with_settings(engine, path, format_name, &settings, error, error_size);
}

dt_mobile_status dt_mobile_engine_export_with_settings(dt_mobile_engine *engine, const char *path,
                                                      const char *format,
                                                      const dt_mobile_export_settings *settings,
                                                      char *error, size_t error_size)
{
  dt_mobile_status status = begin_operation(engine, error, error_size);
  if(status != DT_MOBILE_OK) return status;
  status = dt_mobile_export_write(engine->image_id, path, format, settings,
                                   engine->preview_cancel, &engine->cancelled,
                                   error, error_size);
  end_operation(engine);
  return status;
}

dt_mobile_status dt_mobile_engine_render(dt_mobile_engine *engine, int mw, int mh,
                                          uint8_t **pixels, int *width, int *height,
                                          char *error, size_t error_size)
{
  pthread_mutex_lock(&engine->operation_lock);
  if(atomic_load(&engine->cancelled))
  {
    pthread_mutex_unlock(&engine->operation_lock);
    return engine_fail(error, error_size, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
  }
  uint32_t w = 0, h = 0;
  pthread_mutex_lock(&engine_work_lock);
  if(dt_imageio_preview_to_memory(engine->image_id, (size_t)mw, (size_t)mh, pixels, &w, &h,
                                  engine->preview_cancel))
  {
    pthread_mutex_unlock(&engine_work_lock);
    pthread_mutex_unlock(&engine->operation_lock);
    if(atomic_load(&engine->cancelled))
      return engine_fail(error, error_size, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
    return engine_fail(error, error_size, DT_MOBILE_ERROR_PROCESSING, "darktable preview pixelpipe failed");
  }
  pthread_mutex_unlock(&engine_work_lock);
  if(atomic_load(&engine->cancelled)) { free(*pixels); *pixels = NULL; pthread_mutex_unlock(&engine->operation_lock); return engine_fail(error, error_size, DT_MOBILE_ERROR_CANCELLED, "operation cancelled"); }
  *width = (int)w; *height = (int)h;
  engine->preview_max_width = mw;
  engine->preview_max_height = mh;
  pthread_mutex_unlock(&engine->operation_lock);
  return DT_MOBILE_OK;
}
void dt_mobile_engine_cancel(dt_mobile_engine *engine) { if(engine) { atomic_store(&engine->cancelled, true);
  dt_imageio_preview_cancel(engine->preview_cancel); } }
void dt_mobile_engine_close(dt_mobile_engine *engine)
{
  if(!engine) return;
  pthread_mutex_lock(&engine->operation_lock);
  pthread_mutex_lock(&engine_work_lock);
  /* History snapshots borrow module pointers and must go before module cleanup. */
  g_clear_pointer(&engine->history_states, g_ptr_array_unref);
  g_clear_pointer(&engine->instance_priorities, g_hash_table_unref);
  g_clear_pointer(&engine->crop_geometry_key, g_bytes_unref);
  g_clear_pointer(&engine->crop_input_sizes, g_hash_table_unref);
  dt_dev_cleanup(&engine->develop);
  pthread_mutex_unlock(&engine_work_lock);
  pthread_mutex_unlock(&engine->operation_lock);
  pthread_mutex_destroy(&engine->operation_lock);
  dt_imageio_preview_cancel_free(engine->preview_cancel);
  free(engine);
  pthread_mutex_lock(&runtime_lock);
  if(runtime_users && !--runtime_users) cleanup_runtime();
  pthread_mutex_unlock(&runtime_lock);
}
