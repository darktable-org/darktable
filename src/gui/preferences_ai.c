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
#include "gui/preferences_ai.h"
#include "bauhaus/bauhaus.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "dtgtk/paint_cell.h"
#include "ai/backend.h"
#include "common/ai_models.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "control/signal.h"
#include "gui/gtk.h"

#include <glib/gi18n.h>

// non-default indicator
#define NON_DEF_CHAR "\xe2\x97\x8f"

// update the non-default indicator dot for a preference
static void _update_string_indicator(GtkWidget *indicator, const char *confkey)
{
  const gboolean is_default = dt_conf_is_default(confkey);
  if(is_default)
  {
    gtk_label_set_text(GTK_LABEL(indicator), "");
    gtk_widget_set_tooltip_text(indicator, NULL);
  }
  else
  {
    gtk_label_set_text(GTK_LABEL(indicator), NON_DEF_CHAR);
    gtk_widget_set_tooltip_text(indicator, _("this setting has been modified"));
  }
}

// create the indicator label widget
static GtkWidget *_create_indicator(const char *confkey)
{
  const gboolean is_default = dt_conf_is_default(confkey);
  GtkWidget *label;
  if(is_default)
    label = gtk_label_new("");
  else
  {
    label = gtk_label_new(NON_DEF_CHAR);
    gtk_widget_set_tooltip_text(label, _("this setting has been modified"));
  }
  gtk_widget_set_name(label, "preference_non_default");
  return label;
}

// column indices for model list store
enum
{
  COL_SELECTED,
  COL_NAME,
  COL_INFO,     // info icon visibility flag (TRUE on downloaded rows)
  COL_VERSION,
  COL_TASK,
  COL_ENABLED,
  COL_ENABLED_SENSITIVE, // whether the enabled checkbox is clickable
  COL_STATUS,
  COL_REPOSITORY,
  COL_DEFAULT,
  COL_ID,
  NUM_COLS
};

typedef struct dt_prefs_ai_data_t
{
  GtkWidget *enable_toggle;
  GtkWidget *enable_indicator;
  GtkWidget *provider_combo;
  GtkWidget *gpu_combo;
  GtkWidget *provider_indicator;
  GtkWidget *provider_status;
  GtkWidget *model_list;
  GtkListStore *model_store;
#ifdef HAVE_AI_DOWNLOAD
  GtkWidget *download_selected_btn;
  GtkWidget *download_default_btn;
  GtkWidget *install_repo_btn;
#endif
  GtkWidget *install_btn;
  GtkWidget *delete_selected_btn;
  GtkWidget *parent_dialog;
  GtkWidget *select_all_toggle;
  GtkTreeViewColumn *info_col;
  GtkTreeViewColumn *repo_col;  // hidden unless a third-party model exists
  GtkWidget *controls_box;  // container for all controls below the enable toggle
  GtkWidget *ort_path_entry;
  GtkWidget *ort_path_indicator;
  GtkWidget *settings_grid;
  int controls_start_row;   // first row to grey out when AI disabled
  guint supported_providers;
} dt_prefs_ai_data_t;

#ifdef HAVE_AI_DOWNLOAD
// download dialog data
typedef struct dt_download_dialog_t
{
  GtkWidget *dialog;
  GtkWidget *progress_bar;
  GtkWidget *status_label;
  dt_prefs_ai_data_t *prefs_data;
  char *model_id;
  char *error;
  double progress;
  gboolean finished;
  gboolean cancelled;
  gboolean finish_handled;  // one-shot guard, GTK main thread only -- no mutex needed
  GMutex mutex;
} dt_download_dialog_t;
#endif

// sort by task, then default (yes before no), then name
static gint _model_sort_func(GtkTreeModel *model,
                             GtkTreeIter *a,
                             GtkTreeIter *b,
                             gpointer user_data)
{
  gchar *task_a, *task_b, *default_a, *default_b, *name_a, *name_b;
  gtk_tree_model_get(model, a, COL_TASK, &task_a, COL_DEFAULT, &default_a,
                     COL_NAME, &name_a, -1);
  gtk_tree_model_get(model, b, COL_TASK, &task_b, COL_DEFAULT, &default_b,
                     COL_NAME, &name_b, -1);

  int cmp = g_strcmp0(task_a, task_b);
  if(cmp == 0)
  {
    // "yes" sorts before "no" (reverse alphabetical)
    cmp = g_strcmp0(default_b, default_a);
    if(cmp == 0)
      cmp = g_strcmp0(name_a, name_b);
  }

  g_free(task_a);
  g_free(task_b);
  g_free(default_a);
  g_free(default_b);
  g_free(name_a);
  g_free(name_b);
  return cmp;
}

static const char *_status_to_string(dt_ai_model_status_t status)
{
  switch(status)
  {
  case DT_AI_MODEL_DOWNLOADED:
    return _("downloaded");
  case DT_AI_MODEL_UPDATE_AVAILABLE:
    return _("update available");
  case DT_AI_MODEL_UPDATE_REQUIRED:
    return _("update required");
  case DT_AI_MODEL_DOWNLOADING:
    return _("downloading...");
  case DT_AI_MODEL_ERROR:
    return _("error");
  default:
    return _("not downloaded");
  }
}

#ifdef HAVE_AI_DOWNLOAD
// enable "download / update selected" only when at least one row is ticked
static void _update_download_selected_sensitivity(dt_prefs_ai_data_t *data)
{
  if(!data->download_selected_btn) return;
  gboolean any = FALSE;
  GtkTreeIter iter;
  gboolean valid
    = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(data->model_store), &iter);
  while(valid)
  {
    gboolean sel = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(data->model_store), &iter,
                       COL_SELECTED, &sel, -1);
    if(sel) { any = TRUE; break; }
    valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(data->model_store), &iter);
  }
  gtk_widget_set_sensitive(data->download_selected_btn, any);
}
#endif

static void _refresh_model_list(dt_prefs_ai_data_t *data)
{
  if(!darktable.ai_registry)
  {
    dt_print(DT_DEBUG_AI, "[preferences_ai] registry is NULL");
    return;
  }

  gtk_list_store_clear(data->model_store);

  dt_ai_models_refresh_status();
  dt_ai_models_check_updates();

  const int count = dt_ai_models_get_count();
  dt_print(DT_DEBUG_AI, "[preferences_ai] refreshing model list, count=%d", count);
  gboolean any_third_party = FALSE;
  for(int i = 0; i < count; i++)
  {
    dt_ai_model_t *model = dt_ai_models_get_by_index(i);
    if(!model)
    {
      dt_print(DT_DEBUG_AI, "[preferences_ai] model at index %d is NULL", i);
      continue;
    }
    dt_print(
      DT_DEBUG_AI,
      "[preferences_ai] adding model: %s",
      model->id ? model->id : "(null)");

    // check if this model is the active one for its task
    const gboolean is_downloaded = (model->status == DT_AI_MODEL_DOWNLOADED
                                    || model->status == DT_AI_MODEL_UPDATE_AVAILABLE);
    gboolean is_active = FALSE;
    if(model->task && model->task[0])
    {
      char *active_id = dt_ai_models_get_active_for_task(model->task);
      is_active = (active_id && g_strcmp0(active_id, model->id) == 0);
      g_free(active_id);
    }

    // only a non-official origin is worth naming; the column is hidden
    // entirely when nothing here has one
    const gboolean third_party
      = model->repository
        && !dt_ai_models_is_official_repository(model->repository);
    if(third_party) any_third_party = TRUE;

    GtkTreeIter iter;
    gtk_list_store_append(data->model_store, &iter);
    gtk_list_store_set(
      data->model_store,
      &iter,
      COL_SELECTED,
      FALSE,
      COL_ENABLED,
      is_active,
      COL_ENABLED_SENSITIVE,
      is_downloaded,
      COL_NAME,
      model->name ? model->name : model->id,
      COL_TASK,
      model->task ? model->task : "",
      COL_STATUS,
      _status_to_string(model->status),
      COL_REPOSITORY,
      third_party ? model->repository : "",
      COL_DEFAULT,
      model->is_default ? _("yes") : _("no"),
      COL_VERSION,
      (model->status == DT_AI_MODEL_DOWNLOADED
       || model->status == DT_AI_MODEL_UPDATE_AVAILABLE
       || model->status == DT_AI_MODEL_UPDATE_REQUIRED)
        ? (model->version ? model->version : "0.0") : "–",
      COL_ID,
      model->id,
      COL_INFO, is_downloaded,
      -1);
    dt_ai_model_free(model);
  }

  // an all-official install never sees this column
  if(data->repo_col)
    gtk_tree_view_column_set_visible(data->repo_col, any_third_party);

  // reset select-all toggle
  if(data->select_all_toggle)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->select_all_toggle), FALSE);

#ifdef HAVE_AI_DOWNLOAD
  _update_download_selected_sensitivity(data);
#endif
}

static void _ai_models_changed_cb(gpointer instance, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;
  if(data) _refresh_model_list(data);
}

// disconnect before free so a late signal dispatch can't touch freed data
static void _prefs_ai_data_free(gpointer user_data)
{
  DT_CONTROL_SIGNAL_DISCONNECT(_ai_models_changed_cb, user_data);
  g_free(user_data);
}

static void _update_controls_sensitivity(dt_prefs_ai_data_t *data, gboolean enabled)
{
  // grey out settings grid rows below the enable toggle
  if(data->settings_grid)
  {
    GList *children = gtk_container_get_children(GTK_CONTAINER(data->settings_grid));
    for(GList *l = children; l; l = g_list_next(l))
    {
      GtkWidget *child = l->data;
      int child_row = 0;
      gtk_container_child_get(GTK_CONTAINER(data->settings_grid), child,
                              "top-attach", &child_row, NULL);
      if(child_row >= data->controls_start_row)
        gtk_widget_set_sensitive(child, enabled);
    }
    g_list_free(children);
  }
  // grey out models section
  if(data->controls_box)
    gtk_widget_set_sensitive(data->controls_box, enabled);
}

static void _on_enable_toggled(GtkWidget *widget, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;
  const gboolean enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  dt_conf_set_bool("plugins/ai/enabled", enabled);
  if(darktable.ai_registry)
  {
    dt_ai_registry_set_enabled(enabled);

    // lazy-init directories + models on first enable
    if(enabled)
    {
      dt_ai_models_init_lazy();
      _refresh_model_list(data);
    }
  }

  // notify modules so reload_defaults can show/hide
  // AI-dependent features without requiring image switch
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_AI_MODELS_CHANGED);

  _update_controls_sensitivity(data, enabled);

  // update non-default indicator dot
  _update_string_indicator(data->enable_indicator,
                           "plugins/ai/enabled");
}

static int _provider_to_combo_idx(const dt_ai_provider_t provider,
                                  const guint supported);
static void _on_provider_changed(GtkWidget *widget, gpointer user_data);
static void _update_provider_status(dt_prefs_ai_data_t *data,
                                    const dt_ai_provider_t provider);
static void _on_gpu_changed(GtkWidget *widget, gpointer user_data);
static void _refresh_gpu_combo(dt_prefs_ai_data_t *data);

// a provider is visible in the combo iff it is compiled in AND the
// currently-loaded ORT library advertises it (or it's a builtin like
// AUTO / CPU)
static gboolean _provider_visible(const int i, const guint supported)
{
  return dt_ai_providers[i].available
         && (supported & (1u << dt_ai_providers[i].value));
}

// probe the ORT library at `path` (NULL/empty = use the bundled mask)
// and return a bitmask of dt_ai_provider_t values it advertises;
// falls back to the bundled mask when the probe fails, so the combo
// only offers providers we can reasonably expect to work
static guint _compute_supported_providers(const char *path)
{
  if(path && path[0])
  {
    char *eps = NULL;
    if(dt_ai_ort_probe_library_full(path, NULL, &eps) && eps)
    {
      const guint mask = dt_ai_providers_from_eps(eps);
      g_free(eps);
      return mask;
    }
    g_free(eps);
  }
  return dt_ai_providers_bundled();
}

// rebuild the provider combo to reflect data->supported_providers;
// intermediate states don't fire _on_provider_changed.
// SIDE EFFECT: writes DT_AI_CONF_PROVIDER if the resolved selection
// differs from the saved value (e.g. when an old GPU EP isn't in the
// new library and we auto-fall-back to another GPU EP) so the on-disk
// config stays consistent with what the user sees in the combo
static void _refresh_provider_combo(dt_prefs_ai_data_t *data)
{
  if(!data->provider_combo) return;
  g_signal_handlers_block_by_func(data->provider_combo,
                                  _on_provider_changed, data);
  gchar *cfg = dt_conf_get_string("plugins/ai/provider");
  const dt_ai_provider_t prev = dt_ai_provider_from_string(cfg);
  g_free(cfg);

  dt_bauhaus_combobox_clear(data->provider_combo);
  GString *tooltip =
    g_string_new(_("select hardware acceleration for AI inference:"));
  for(int i = 0; i < DT_AI_PROVIDER_COUNT; i++)
  {
    if(!_provider_visible(i, data->supported_providers)) continue;
    const char *label = dt_ai_providers[i].value == DT_AI_PROVIDER_AUTO
                          ? _("auto") : dt_ai_providers[i].display_name;
    dt_bauhaus_combobox_add(data->provider_combo, label);
    g_string_append_printf(tooltip, "\n- %s", dt_ai_providers[i].display_name);
  }
  gtk_widget_set_tooltip_text(data->provider_combo, tooltip->str);
  g_string_free(tooltip, TRUE);

  // bauhaus sizes the combo to fit the longest entry at allocation
  // time; after clear/repopulate the cached layout would keep the old
  // width, so force GTK to re-request natural size
  gtk_widget_queue_resize(data->provider_combo);

  // resolve the new selection:
  //   1. keep prev if it's still in supported_providers (covers AUTO/CPU
  //      always, and the same GPU EP across same-vendor library swaps)
  //   2. if prev was a GPU EP and isn't supported any more, pick the
  //      first visible GPU EP — switching CUDA→ROCm libraries should
  //      land on ROCm rather than dropping the user back to AUTO/CPU
  //   3. otherwise fall back to AUTO
  dt_ai_provider_t selected = DT_AI_PROVIDER_AUTO;
  if(data->supported_providers & (1u << prev))
  {
    selected = prev;
  }
  else if(prev != DT_AI_PROVIDER_AUTO && prev != DT_AI_PROVIDER_CPU)
  {
    // previous was a specific GPU EP that's gone — find another GPU EP
    for(int i = 0; i < DT_AI_PROVIDER_COUNT; i++)
    {
      if(!_provider_visible(i, data->supported_providers)) continue;
      const dt_ai_provider_t v = dt_ai_providers[i].value;
      if(v == DT_AI_PROVIDER_AUTO || v == DT_AI_PROVIDER_CPU) continue;
      selected = v;
      break;
    }
  }
  dt_bauhaus_combobox_set(data->provider_combo,
                          _provider_to_combo_idx(selected,
                                                 data->supported_providers));

  // persist the resolved selection so a subsequent close-without-change
  // doesn't leave a stale provider in the config (e.g. CUDA still saved
  // after the user switched to a ROCm-only library)
  if(selected != prev)
  {
    for(int i = 0; i < DT_AI_PROVIDER_COUNT; i++)
    {
      if(dt_ai_providers[i].value == selected)
      {
        dt_conf_set_string(DT_AI_CONF_PROVIDER,
                           dt_ai_providers[i].config_string);
        break;
      }
    }
  }

  g_signal_handlers_unblock_by_func(data->provider_combo,
                                    _on_provider_changed, data);

  // runtime-probe the selection so the status label reflects the new
  // EP. the value-changed signal was blocked during the rebuild, so
  // _on_provider_changed didn't fire
  _update_provider_status(data, selected);

  // and the per-EP GPU device combo (visible only when 2+ devices)
  _refresh_gpu_combo(data);
}

// rebuild the GPU device combo for the currently-selected EP. only
// visible when the EP supports per-device selection AND >=2 devices
// are enumerable (single-device or unsupported EPs leave the combo
// hidden — AUTO/CPU naturally fall here too)
static void _refresh_gpu_combo(dt_prefs_ai_data_t *data)
{
  if(!data->gpu_combo) return;

  // resolve current EP from conf (matches what the rest of the UI uses)
  gchar *prov_str = dt_conf_get_string(DT_AI_CONF_PROVIDER);
  const dt_ai_provider_t prov = dt_ai_provider_from_string(prov_str);
  g_free(prov_str);

  GList *devices = dt_ai_enum_devices_for_provider(prov);
  const guint count = g_list_length(devices);
  const char *conf_key = dt_ai_device_conf_key_for_provider(prov);

  if(count < 2 || !conf_key)
  {
    gtk_widget_hide(data->gpu_combo);
    g_list_free_full(devices, dt_ai_device_free);
    return;
  }

  // populate without firing _on_gpu_changed
  g_signal_handlers_block_by_func(data->gpu_combo, _on_gpu_changed, data);
  dt_bauhaus_combobox_clear(data->gpu_combo);
  const int saved = dt_conf_key_exists(conf_key) ? dt_conf_get_int(conf_key) : 0;
  int sel_idx = 0, i = 0;
  for(GList *l = devices; l; l = g_list_next(l), i++)
  {
    const dt_ai_device_t *d = (const dt_ai_device_t *)l->data;
    dt_bauhaus_combobox_add(data->gpu_combo, d->name);
    if(d->id == saved) sel_idx = i;
  }
  dt_bauhaus_combobox_set(data->gpu_combo, sel_idx);
  g_signal_handlers_unblock_by_func(data->gpu_combo, _on_gpu_changed, data);

  gtk_widget_show(data->gpu_combo);
  gtk_widget_queue_resize(data->gpu_combo);
  g_list_free_full(devices, dt_ai_device_free);
}

static void _on_gpu_changed(GtkWidget *widget, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;
  const int combo_idx = dt_bauhaus_combobox_get(widget);
  if(combo_idx < 0) return;

  gchar *prov_str = dt_conf_get_string(DT_AI_CONF_PROVIDER);
  const dt_ai_provider_t prov = dt_ai_provider_from_string(prov_str);
  g_free(prov_str);

  const char *conf_key = dt_ai_device_conf_key_for_provider(prov);
  if(!conf_key) return;

  // re-enumerate to get the device id at this combo position. cheap
  // (shell-out caches in OS pagecache, ~1 ms after first call)
  GList *devices = dt_ai_enum_devices_for_provider(prov);
  const dt_ai_device_t *d = g_list_nth_data(devices, combo_idx);
  if(d) dt_conf_set_int(conf_key, d->id);
  g_list_free_full(devices, dt_ai_device_free);

  // refresh the status — selecting a different GPU mid-session means
  // restart needed (current EP session is bound to the old device)
  _update_provider_status(data, prov);
}

// map combo box index to provider table index (skipping unavailable providers)
static int _combo_idx_to_provider(const int combo_idx, const guint supported)
{
  int visible = -1;
  for(int i = 0; i < DT_AI_PROVIDER_COUNT; i++)
  {
    if(!_provider_visible(i, supported)) continue;
    if(++visible == combo_idx)
      return i;
  }
  return 0;  // fallback to AUTO
}

// map provider enum value to combo box index
static int _provider_to_combo_idx(const dt_ai_provider_t provider,
                                  const guint supported)
{
  int visible = -1;
  for(int i = 0; i < DT_AI_PROVIDER_COUNT; i++)
  {
    if(!_provider_visible(i, supported)) continue;
    visible++;
    if(dt_ai_providers[i].value == provider)
      return visible;
  }
  return 0;  // fallback to first visible (AUTO)
}

static void _update_provider_status(dt_prefs_ai_data_t *data,
                                    const dt_ai_provider_t provider)
{
  if(!data->provider_status) return;

  // don't probe GPU providers when AI is disabled —
  // initializing MIGraphX/ROCm on unsupported GPUs can abort()
  if(!dt_ai_registry_is_enabled())
  {
    gtk_label_set_text(GTK_LABEL(data->provider_status), "");
    return;
  }

  // long-lived sessions (e.g. SAM2 encoder) are bound to the EP they
  // were loaded with; show "restart to apply" for GPU EPs
  if((dt_ai_ort_path_changed_since_load()
      || dt_ai_provider_changed_since_load()
      || dt_ai_device_id_changed_since_load(provider))
     && provider != DT_AI_PROVIDER_AUTO && provider != DT_AI_PROVIDER_CPU)
  {
    gtk_label_set_markup(GTK_LABEL(data->provider_status),
                         _("<i>restart to apply</i>"));
    return;
  }

  if(provider == DT_AI_PROVIDER_AUTO || provider == DT_AI_PROVIDER_CPU
     || dt_ai_probe_provider(provider))
  {
    gtk_label_set_text(GTK_LABEL(data->provider_status), "");
    return;
  }

  gtk_label_set_markup(GTK_LABEL(data->provider_status),
                       _("<i>not available, will fall back to CPU</i>"));
}

static void _on_provider_changed(GtkWidget *widget, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;
  const int combo_idx = dt_bauhaus_combobox_get(widget);
  const int pi = _combo_idx_to_provider(combo_idx, data->supported_providers);
  dt_conf_set_string(DT_AI_CONF_PROVIDER, dt_ai_providers[pi].config_string);
  dt_ai_registry_set_provider(dt_ai_providers[pi].value);
  _update_string_indicator(data->provider_indicator, DT_AI_CONF_PROVIDER);
  _update_provider_status(data, dt_ai_providers[pi].value);
}

// double-click on label resets the enable toggle to default
static void _reset_enable_click_cb(GtkGestureSingle *gesture, int n_press,
                                      double x, double y,
                                      GtkWidget *widget)
{
  if(n_press < 2) return;
  const gboolean def = dt_confgen_get_bool("plugins/ai/enabled", DT_DEFAULT);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), def);
}

// double-click on label resets the provider combo to default
static void
_reset_provider_click_cb(GtkGestureSingle *gesture, int n_press,
                           double x, double y,
                           gpointer user_data)
{
  if(n_press < 2) return;
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;
  const char *def = dt_confgen_get(DT_AI_CONF_PROVIDER, DT_DEFAULT);
  dt_ai_provider_t provider = dt_ai_provider_from_string(def);
  dt_bauhaus_combobox_set(data->provider_combo,
                          _provider_to_combo_idx(provider,
                                                 data->supported_providers));
}

static void _on_model_selection_toggled(GtkCellRendererToggle *cell,
                                        gchar *path_string,
                                        gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;

  GtkTreeIter iter;
  GtkTreePath *path = gtk_tree_path_new_from_string(path_string);
  gtk_tree_model_get_iter(GTK_TREE_MODEL(data->model_store), &iter, path);
  gtk_tree_path_free(path);

  gboolean selected;
  gtk_tree_model_get(
    GTK_TREE_MODEL(data->model_store),
    &iter,
    COL_SELECTED,
    &selected,
    -1);

  // toggle the value
  gtk_list_store_set(data->model_store, &iter, COL_SELECTED, !selected, -1);

#ifdef HAVE_AI_DOWNLOAD
  _update_download_selected_sensitivity(data);
#endif
}

static void _on_enabled_toggled(GtkCellRendererToggle *cell,
                                gchar *path_string,
                                gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;

  GtkTreeIter iter;
  GtkTreePath *path = gtk_tree_path_new_from_string(path_string);
  gtk_tree_model_get_iter(GTK_TREE_MODEL(data->model_store), &iter, path);
  gtk_tree_path_free(path);

  gboolean currently_enabled;
  gchar *model_id = NULL;
  gchar *task = NULL;
  gtk_tree_model_get(
    GTK_TREE_MODEL(data->model_store),
    &iter,
    COL_ENABLED, &currently_enabled,
    COL_ID, &model_id,
    COL_TASK, &task,
    -1);

  if(!task || !task[0] || !model_id)
  {
    g_free(model_id);
    g_free(task);
    return;
  }

  if(currently_enabled)
    dt_ai_models_set_active_for_task(task, NULL);
  else
    dt_ai_models_set_active_for_task(task, model_id);

  g_free(model_id);
  g_free(task);

  // refresh to update all checkboxes (previous active model unchecks)
  _refresh_model_list(data);

  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_AI_MODELS_CHANGED);
}

static void _on_select_all_toggled(GtkToggleButton *toggle, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;
  const gboolean select_all = gtk_toggle_button_get_active(toggle);

  GtkTreeIter iter;
  gboolean valid
    = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(data->model_store), &iter);
  while(valid)
  {
    gtk_list_store_set(data->model_store, &iter, COL_SELECTED, select_all, -1);
    valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(data->model_store), &iter);
  }

#ifdef HAVE_AI_DOWNLOAD
  _update_download_selected_sensitivity(data);
#endif
}

static void _on_select_all_header_clicked(GtkWidget *button, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;
  // only toggle if the click wasn't already handled by the checkbox itself.
  // block the toggled signal to prevent double-fire, then toggle manually
  g_signal_handlers_block_by_func(data->select_all_toggle, _on_select_all_toggled, data);
  const gboolean active
    = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->select_all_toggle));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->select_all_toggle), !active);
  g_signal_handlers_unblock_by_func(
    data->select_all_toggle,
    _on_select_all_toggled,
    data);

  // now manually apply the selection since we blocked the signal
  _on_select_all_toggled(GTK_TOGGLE_BUTTON(data->select_all_toggle), data);
}

// collect selected model IDs from the list store
static GList *_get_selected_model_ids(dt_prefs_ai_data_t *data)
{
  GList *ids = NULL;
  GtkTreeIter iter;
  gboolean valid
    = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(data->model_store), &iter);
  while(valid)
  {
    gboolean selected;
    gchar *model_id;
    gtk_tree_model_get(
      GTK_TREE_MODEL(data->model_store),
      &iter,
      COL_SELECTED,
      &selected,
      COL_ID,
      &model_id,
      -1);
    if(selected && model_id)
      ids = g_list_append(ids, model_id);
    else
      g_free(model_id);
    valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(data->model_store), &iter);
  }
  return ids;
}

#ifdef HAVE_AI_DOWNLOAD
// progress callback called from download thread
static void
_download_progress_callback(const char *model_id, double progress, gpointer user_data)
{
  dt_download_dialog_t *dl = (dt_download_dialog_t *)user_data;
  g_mutex_lock(&dl->mutex);
  dl->progress = progress;
  g_mutex_unlock(&dl->mutex);
}

// idle callback to update progress bar from main thread
static gboolean _update_progress_idle(gpointer user_data)
{
  dt_download_dialog_t *dl = (dt_download_dialog_t *)user_data;

  g_mutex_lock(&dl->mutex);
  const double progress = dl->progress;
  const gboolean finished = dl->finished;
  g_mutex_unlock(&dl->mutex);

  if(dl->dialog && GTK_IS_WIDGET(dl->dialog))
  {
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dl->progress_bar), progress);

    char *text = g_strdup_printf("%.0f%%", progress * 100.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(dl->progress_bar), text);
    g_free(text);
  }

  const gboolean dialog_alive = dl->dialog && GTK_IS_WIDGET(dl->dialog);
  if(finished && !dl->finish_handled && dialog_alive)
  {
    dl->finish_handled = TRUE;
    // close the progress dialog in both cases — error is surfaced as
    // a standard message dialog by the caller after the thread joins
    gtk_dialog_response(GTK_DIALOG(dl->dialog), GTK_RESPONSE_OK);
  }

  // removal is owned by the caller; returning G_SOURCE_REMOVE here
  // would race with that explicit remove
  return G_SOURCE_CONTINUE;
}

// download thread function
static gpointer _download_thread_func(gpointer user_data)
{
  dt_download_dialog_t *dl = (dt_download_dialog_t *)user_data;

  char *error = dt_ai_models_download_sync(
    dl->model_id,
    _download_progress_callback,
    dl,
    &dl->cancelled);

  g_mutex_lock(&dl->mutex);
  dl->error = error;
  dl->finished = TRUE;
  g_mutex_unlock(&dl->mutex);

  return NULL;
}

// show modal download dialog for a single model
static gboolean
_download_model_with_dialog(dt_prefs_ai_data_t *data, const char *model_id)
{
  dt_ai_model_t *model = dt_ai_models_get_by_id(model_id);
  if(!model)
    return FALSE;

  // create dialog
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
    _("downloading AI model"),
    GTK_WINDOW(data->parent_dialog),
    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
    _("_cancel"),
    GTK_RESPONSE_CANCEL,
    NULL);

  gtk_window_set_default_size(GTK_WINDOW(dialog), 400, -1);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 10);
  gtk_box_set_spacing(GTK_BOX(content), 10);

  // model name label (use fields from copy, then free it)
  char *title
    = g_strdup_printf(_("downloading: %s"), model->name ? model->name : model->id);
  dt_ai_model_free(model);
  GtkWidget *title_label = gtk_label_new(title);
  g_free(title);
  dt_gui_box_add(content, title_label);

  // progress bar
  GtkWidget *progress_bar = gtk_progress_bar_new();
  gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(progress_bar), TRUE);
  gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), "0%");
  dt_gui_box_add(content, progress_bar);

  // status label (for errors)
  GtkWidget *status_label = gtk_label_new("");
  gtk_widget_set_no_show_all(status_label, TRUE);
  dt_gui_box_add(content, status_label);

  gtk_widget_show_all(dialog);

  // set up download data (heap-allocated for thread safety)
  dt_download_dialog_t *dl = g_new0(dt_download_dialog_t, 1);
  g_mutex_init(&dl->mutex);
  dl->dialog = dialog;
  dl->progress_bar = progress_bar;
  dl->status_label = status_label;
  dl->prefs_data = data;
  dl->model_id = g_strdup(model_id);
  dl->progress = 0.0;
  dl->finished = FALSE;
  dl->cancelled = FALSE;
  dl->error = NULL;

  // start download thread
  GThread *thread = g_thread_new("ai-download", _download_thread_func, dl);

  // start progress update timer
  guint timer_id = g_timeout_add(100, _update_progress_idle, dl);

  // run dialog
  gint response = gtk_dialog_run(GTK_DIALOG(dialog));

  // handle cancellation (atomic so curl progress callback can read it safely)
  if(response == GTK_RESPONSE_CANCEL)
    g_atomic_int_set(&dl->cancelled, TRUE);

  // wait for thread to finish — after this, dl->finished is TRUE
  g_thread_join(thread);

  // remove the timer -- sole removal point; idle callback always
  // returns G_SOURCE_CONTINUE to avoid racing with this remove
  g_source_remove(timer_id);

  // destroy the dialog before freeing dl so no straggling callback
  // can touch destroyed widgets.
  gtk_widget_destroy(dialog);
  dl->dialog = NULL;

  const gboolean success = (dl->error == NULL);

  // notify modules that models have changed
  if(success)
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_AI_MODELS_CHANGED);
  else if(response != GTK_RESPONSE_CANCEL)
  {
    // download failed (not user-cancelled) — surface in a standard error dialog
    GtkWidget *err = gtk_message_dialog_new(
      GTK_WINDOW(data->parent_dialog),
      GTK_DIALOG_MODAL,
      GTK_MESSAGE_ERROR,
      GTK_BUTTONS_OK,
      "%s", dl->error);
    gtk_window_set_title(GTK_WINDOW(err), _("model download failed"));
    gtk_dialog_run(GTK_DIALOG(err));
    gtk_widget_destroy(err);
  }

  // clean up
  g_mutex_clear(&dl->mutex);
  g_free(dl->model_id);
  g_free(dl->error);
  g_free(dl);

  return success;
}

static void _on_download_selected(GtkButton *button, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;

  GList *ids = _get_selected_model_ids(data);
  for(GList *l = ids; l; l = g_list_next(l))
  {
    const char *id = (const char *)l->data;
    dt_ai_model_t *model = dt_ai_models_get_by_id(id);
    if(model)
    {
      const gboolean need_download = (model->status == DT_AI_MODEL_NOT_DOWNLOADED
                                     || model->status == DT_AI_MODEL_UPDATE_AVAILABLE
                                     || model->status == DT_AI_MODEL_UPDATE_REQUIRED);
      dt_ai_model_free(model);
      if(need_download && !_download_model_with_dialog(data, id))
        break; // stop on error or cancel
    }
  }
  g_list_free_full(ids, g_free);
  _refresh_model_list(data);
}

static void _on_download_default(GtkButton *button, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;

  // download default models that need downloading
  const int count = dt_ai_models_get_count();
  for(int i = 0; i < count; i++)
  {
    dt_ai_model_t *model = dt_ai_models_get_by_index(i);
    if(!model)
      continue;
    const gboolean need_download =
            (model->is_default
             && (model->status == DT_AI_MODEL_NOT_DOWNLOADED
                 || model->status == DT_AI_MODEL_UPDATE_AVAILABLE
                 || model->status == DT_AI_MODEL_UPDATE_REQUIRED));
    char *id = need_download ? g_strdup(model->id) : NULL;
    dt_ai_model_free(model);
    if(need_download)
    {
      if(!_download_model_with_dialog(data, id))
      {
        g_free(id);
        break; // stop on error or cancel
      }
      g_free(id);
    }
  }
  _refresh_model_list(data);
}

// --- install from repository -----------------------------------------
// the bundled catalog is fixed at build time, so a model released
// afterwards is undownloadable until darktable ships again. the repository's
// versions.json lists the whole release, which is what this dialog offers

enum
{
  REPO_COL_SELECTED = 0,
  REPO_COL_NAME,
  REPO_COL_VERSION,
  REPO_COL_TASK,
  REPO_COL_STATUS,
  REPO_COL_SELECTABLE,
  REPO_COL_TOOLTIP,
  REPO_COL_ENTRY,
  REPO_NUM_COLS
};

typedef struct dt_repo_fetch_t
{
  char *repository;
  GList *models;        // dt_ai_repo_model_t*, owned once finished
  char *error;
  gboolean finished;    // set last, read by the gui via g_atomic
} dt_repo_fetch_t;

typedef struct dt_repo_dialog_t
{
  dt_prefs_ai_data_t *prefs;
  GtkWidget *dialog;
  GtkWidget *combo;
  GtkWidget *stack;    // "list" or "message"
  GtkWidget *message;
  GtkWidget *manage_btn;
  GtkListStore *store;
  GHashTable *cache;    // repository -> GList* of dt_ai_repo_model_t*,
                        // so switching back does not refetch
  gboolean busy;
  guint idle_id;        // pending initial fetch, see _repo_initial_fetch
} dt_repo_dialog_t;

static gpointer _repo_fetch_thread(gpointer user_data)
{
  dt_repo_fetch_t *fetch = (dt_repo_fetch_t *)user_data;
  fetch->models = dt_ai_models_fetch_repository_list(fetch->repository,
                                                     &fetch->error);
  g_atomic_int_set(&fetch->finished, TRUE);
  return NULL;
}

// install acts on the ticked rows, so it stays insensitive until there is
// at least one. mirrors _update_download_selected_sensitivity above
static void _repo_update_install_sensitivity(dt_repo_dialog_t *rd)
{
  gboolean any = FALSE;
  GtkTreeIter iter;
  gboolean valid
    = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(rd->store), &iter);
  while(valid)
  {
    gboolean sel = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(rd->store), &iter,
                       REPO_COL_SELECTED, &sel, -1);
    if(sel) { any = TRUE; break; }
    valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(rd->store), &iter);
  }
  gtk_dialog_set_response_sensitive(GTK_DIALOG(rd->dialog),
                                    GTK_RESPONSE_ACCEPT, any);
}

static void _on_repo_toggle(GtkCellRendererToggle *renderer,
                            gchar *path_str,
                            gpointer user_data)
{
  dt_repo_dialog_t *rd = (dt_repo_dialog_t *)user_data;
  GtkTreeIter iter;
  GtkTreePath *path = gtk_tree_path_new_from_string(path_str);

  if(gtk_tree_model_get_iter(GTK_TREE_MODEL(rd->store), &iter, path))
  {
    gboolean selected = FALSE, selectable = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(rd->store), &iter,
                       REPO_COL_SELECTED, &selected,
                       REPO_COL_SELECTABLE, &selectable, -1);
    if(selectable)
      gtk_list_store_set(rd->store, &iter, REPO_COL_SELECTED, !selected, -1);
  }
  gtk_tree_path_free(path);

  _repo_update_install_sensitivity(rd);
}

// the table area shows either the list or a centered message, never both
static void _repo_show_message(dt_repo_dialog_t *rd, const char *text)
{
  gtk_label_set_text(GTK_LABEL(rd->message), text);
  gtk_stack_set_visible_child_name(GTK_STACK(rd->stack), "message");
}

// fill the table from a repository's listing, fetching it unless already
// cached for this dialog
static void _repo_populate(dt_repo_dialog_t *rd, const char *repository)
{
  gtk_list_store_clear(rd->store);

  GList *models = g_hash_table_lookup(rd->cache, repository);
  if(!models)
  {
    rd->busy = TRUE;
    gtk_widget_set_sensitive(rd->combo, FALSE);
    // editing the list mid-fetch would rebuild the combo under us, freeing
    // the very string this call is holding. the busy flag cannot help: the
    // button is a separate handler, and the event pump below runs it
    gtk_widget_set_sensitive(rd->manage_btn, FALSE);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(rd->dialog),
                                      GTK_RESPONSE_ACCEPT, FALSE);
    _repo_show_message(rd, _("retrieving the list of models…"));
    // paint the notice before blocking on the network, or the dialog just
    // sits there looking hung
    dt_gui_process_events();

    dt_repo_fetch_t fetch = { (char *)repository, NULL, NULL, FALSE };
    GThread *thread = g_thread_new("ai-repo-list", _repo_fetch_thread, &fetch);
    while(!g_atomic_int_get(&fetch.finished))
    {
      dt_gui_process_events();
      g_usleep(10000);
    }
    g_thread_join(thread);

    rd->busy = FALSE;
    gtk_widget_set_sensitive(rd->combo, TRUE);
    gtk_widget_set_sensitive(rd->manage_btn, TRUE);

    if(!fetch.models)
    {
      _repo_show_message(rd, fetch.error
                               ? fetch.error
                               : _("nothing to install from here"));
      g_free(fetch.error);
      return;
    }
    g_free(fetch.error);
    g_hash_table_insert(rd->cache, g_strdup(repository), fetch.models);
    models = fetch.models;
  }

  for(GList *l = models; l; l = g_list_next(l))
  {
    const dt_ai_repo_model_t *m = (const dt_ai_repo_model_t *)l->data;
    const char *status = _status_to_string(m->status);
    const gboolean actionable = m->status != DT_AI_MODEL_DOWNLOADED;

    // description, license and size go in the tooltip, not the columns
    GString *tip = g_string_new(NULL);
    if(m->description) g_string_append_printf(tip, "%s\n\n", m->description);
    g_string_append_printf(tip, "%s: %s", _("model"), m->id);
    if(m->size > 0)
    {
      gchar *pretty = g_format_size((guint64)m->size);
      g_string_append_printf(tip, "\n%s: %s", _("download size"), pretty);
      g_free(pretty);
    }
    if(m->license)
      g_string_append_printf(tip, "\n%s: %s", _("license"), m->license);

    gtk_list_store_insert_with_values(
      rd->store, NULL, -1,
      REPO_COL_SELECTED, FALSE,
      REPO_COL_NAME, m->name ? m->name : m->id,
      REPO_COL_VERSION, m->version ? m->version : "",
      REPO_COL_TASK, m->task ? m->task : "",
      REPO_COL_STATUS, status,
      REPO_COL_SELECTABLE, actionable,
      REPO_COL_TOOLTIP, tip->str,
      REPO_COL_ENTRY, m,
      -1);
    g_string_free(tip, TRUE);
  }

  // show what the repository holds even when all of it is installed: the
  // versions and statuses are the point, and install is disabled anyway
  gtk_stack_set_visible_child_name(GTK_STACK(rd->stack), "list");

  // nothing is ticked in a freshly populated table
  _repo_update_install_sensitivity(rd);
}

static void _on_repo_combo_changed(GtkWidget *combo, gpointer user_data)
{
  dt_repo_dialog_t *rd = (dt_repo_dialog_t *)user_data;
  if(rd->busy) return;  // re-entry while the previous fetch pumps events

  // the combo owns what it returns and repopulating frees it, so the fetch
  // below must not be left holding a pointer into it
  gchar *repository = g_strdup(dt_bauhaus_combobox_get_text(combo));
  if(repository) _repo_populate(rd, repository);
  g_free(repository);
}

// the first fetch must not run before gtk_dialog_run(): until its loop is
// up, closing the window destroys the dialog outright, and the fetch pumps
// events for as long as the network takes. from an idle the loop is
// already running, and a close is a response rather than a destroy
static gboolean _repo_initial_fetch(gpointer user_data)
{
  dt_repo_dialog_t *rd = (dt_repo_dialog_t *)user_data;
  rd->idle_id = 0;
  dt_bauhaus_combobox_set(rd->combo, 0);
  return G_SOURCE_REMOVE;
}

// installing an id that already belongs elsewhere overwrites it on disk:
// the archive extracts to <models>/<id>/, whoever published it
static gboolean _confirm_replacement(const dt_ai_repo_model_t *m)
{
  dt_ai_model_t *existing = dt_ai_models_get_by_id(m->id);
  if(!existing) return TRUE;

  const gboolean same_source =
    !g_strcmp0(existing->repository, m->repository)
    || (existing->from_catalog
        && dt_ai_models_is_official_repository(m->repository));
  const gboolean on_disk = existing->status != DT_AI_MODEL_NOT_DOWNLOADED;

  if(same_source || !on_disk)
  {
    dt_ai_model_free(existing);
    return TRUE;
  }

  gchar *owner = g_strdup(existing->from_catalog
                            ? _("the official repository")
                            : (existing->repository ? existing->repository
                                                    : _("a local install")));
  dt_ai_model_free(existing);

  const gboolean ok = dt_gui_show_yes_no_dialog(
    _("replace installed model?"),
    "ai_replace_model",
    _("\"%s\" is already installed from %s.\n\n"
      "installing it from %s replaces those files"),
    m->id, owner, m->repository);
  g_free(owner);
  return ok;
}

// --- manage repositories --------------------------------------------
// verifying means fetching the repository's listing — the same call the
// combo makes. A repo that answers is usable; one that does not would only
// fail later with less context
static gboolean _verify_repository(GtkWindow *parent, const char *repository)
{
  dt_repo_fetch_t fetch = { (char *)repository, NULL, NULL, FALSE };
  GThread *thread = g_thread_new("ai-repo-verify", _repo_fetch_thread, &fetch);
  while(!g_atomic_int_get(&fetch.finished))
  {
    dt_gui_process_events();
    g_usleep(10000);
  }
  g_thread_join(thread);

  const int count = g_list_length(fetch.models);
  char *error = fetch.error;
  dt_ai_repo_model_list_free(fetch.models);

  if(!count)
  {
    GtkWidget *err = gtk_message_dialog_new(
      parent, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
      "%s", error ? error
                  : _("this repository offers no models for this version "
                      "of darktable"));
    gtk_window_set_title(GTK_WINDOW(err), _("repository not usable"));
    gtk_dialog_run(GTK_DIALOG(err));
    gtk_widget_destroy(err);
    g_free(error);
    return FALSE;
  }
  g_free(error);

  // adding a repository is the moment the trust decision is made, so say
  // what it means while the user is making it
  return dt_gui_show_yes_no_dialog(
    _("add this repository?"), "ai_add_repository",
    _("%s offers %d model(s) for this version of darktable\n\n"
      "it is not maintained or reviewed by the darktable project, and a "
      "model installed from it replaces any installed model of the same "
      "name"),
    repository, count);
}

#define DT_AI_REPO_PATTERN "^[a-zA-Z0-9._-]+/[a-zA-Z0-9._-]+$"

typedef struct dt_repo_edit_t
{
  GtkWidget *dialog;
  GtkListStore *store;
  GtkWidget *entry;
  GtkWidget *view;
  GtkWidget *add_btn;
  GtkWidget *remove_btn;
  gboolean busy;        // a verification is pumping events; ignore re-entry
} dt_repo_edit_t;

// add stays insensitive until the field holds something that could be a
// repository, so the shape is taught by the button rather than by an error
static void _on_repo_entry_changed(GtkEditable *editable, gpointer user_data)
{
  dt_repo_edit_t *ed = (dt_repo_edit_t *)user_data;
  gchar *repo = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(ed->entry))));
  gtk_widget_set_sensitive(ed->add_btn,
                           g_regex_match_simple(DT_AI_REPO_PATTERN, repo, 0, 0));
  g_free(repo);
}

// the official repository is listed for context but is a different setting
static void _on_repo_selection_changed(GtkTreeSelection *sel, gpointer user_data)
{
  dt_repo_edit_t *ed = (dt_repo_edit_t *)user_data;
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  gboolean removable = FALSE;
  if(gtk_tree_selection_get_selected(sel, &model, &iter))
    gtk_tree_model_get(model, &iter, 1, &removable, -1);
  gtk_widget_set_sensitive(ed->remove_btn, removable);
}

static gboolean _repo_already_listed(GtkListStore *store, const char *repo)
{
  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter);
  while(valid)
  {
    gchar *existing = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, 0, &existing, -1);
    const gboolean same = !g_strcmp0(existing, repo);
    g_free(existing);
    if(same) return TRUE;
    valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter);
  }
  return FALSE;
}

// the work, so each signal gets a correctly typed callback below rather
// than one handler doing duty for two different first arguments
static void _repo_add(dt_repo_edit_t *ed)
{
  // _verify_repository pumps events, so add stays clickable; a second
  // click would start a second verification
  if(ed->busy) return;

  GtkWindow *parent = GTK_WINDOW(ed->dialog);

  gchar *repo = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(ed->entry))));

  // shape is already guaranteed by the button's sensitivity; a duplicate
  // still needs saying, since it looks perfectly valid
  if(_repo_already_listed(ed->store, repo))
  {
    GtkWidget *err = gtk_message_dialog_new(
      parent, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
      "%s", _("that repository is already listed"));
    gtk_dialog_run(GTK_DIALOG(err));
    gtk_widget_destroy(err);
  }
  else
  {
    ed->busy = TRUE;
    const gboolean accepted = _verify_repository(parent, repo);
    ed->busy = FALSE;
    if(accepted)
    {
      gtk_list_store_insert_with_values(ed->store, NULL, -1,
                                        0, repo, 1, TRUE, -1);
      gtk_entry_set_text(GTK_ENTRY(ed->entry), "");
    }
  }
  g_free(repo);
}

static void _on_repo_add_clicked(GtkButton *button, gpointer user_data)
{
  _repo_add((dt_repo_edit_t *)user_data);
}

static void _on_repo_entry_activate(GtkEntry *entry, gpointer user_data)
{
  dt_repo_edit_t *ed = (dt_repo_edit_t *)user_data;
  // enter should do nothing when add itself is unavailable
  if(gtk_widget_get_sensitive(ed->add_btn)) _repo_add(ed);
}

static void _on_repo_remove(GtkButton *button, gpointer user_data)
{
  dt_repo_edit_t *ed = (dt_repo_edit_t *)user_data;
  GtkTreeSelection *sel
    = gtk_tree_view_get_selection(GTK_TREE_VIEW(ed->view));

  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  if(!gtk_tree_selection_get_selected(sel, &model, &iter)) return;

  gboolean removable = FALSE;
  gtk_tree_model_get(model, &iter, 1, &removable, -1);
  if(removable) gtk_list_store_remove(ed->store, &iter);
}

static void _on_manage_repositories(GtkButton *button, gpointer user_data)
{
  dt_repo_dialog_t *rd = (dt_repo_dialog_t *)user_data;

  GtkWidget *dialog = gtk_dialog_new_with_buttons(
    _("repositories"),
    GTK_WINDOW(rd->dialog),
    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
    _("_cancel"), GTK_RESPONSE_CANCEL,
    _("_ok"), GTK_RESPONSE_ACCEPT,
    NULL);
  gtk_window_set_default_size(GTK_WINDOW(dialog), DT_PIXEL_APPLY_DPI(460),
                              DT_PIXEL_APPLY_DPI(300));

  GtkListStore *store = gtk_list_store_new(2,
                                           G_TYPE_STRING,   // repository
                                           G_TYPE_BOOLEAN); // removable

  gtk_list_store_insert_with_values(
    store, NULL, -1,
    0, dt_ai_models_official_repository(),
    1, FALSE, -1);

  GList *third_party = dt_ai_models_get_third_party_repositories();
  for(GList *l = third_party; l; l = g_list_next(l))
    gtk_list_store_insert_with_values(store, NULL, -1,
                                      0, (const char *)l->data, 1, TRUE, -1);
  g_list_free_full(third_party, g_free);

  GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
  g_object_unref(store);
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), FALSE);
  gtk_tree_view_append_column(
    GTK_TREE_VIEW(view),
    gtk_tree_view_column_new_with_attributes(
      "", gtk_cell_renderer_text_new(), "text", 0, NULL));
  dt_gui_dialog_add(dialog, dt_gui_scroll_wrap(view));

  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "owner/repo");
  GtkWidget *add_btn = gtk_button_new_with_label(_("add"));
  GtkWidget *remove_btn = gtk_button_new_with_label(_("remove"));
  dt_gui_dialog_add(dialog,
                    dt_gui_hbox(dt_gui_expand(entry), add_btn, remove_btn));

  dt_repo_edit_t ed = { dialog, store, entry, view, add_btn, remove_btn, FALSE };
  g_signal_connect(add_btn, "clicked", G_CALLBACK(_on_repo_add_clicked), &ed);
  g_signal_connect(remove_btn, "clicked", G_CALLBACK(_on_repo_remove), &ed);
  g_signal_connect(entry, "changed", G_CALLBACK(_on_repo_entry_changed), &ed);
  // enter in the field does what the button does
  g_signal_connect(entry, "activate", G_CALLBACK(_on_repo_entry_activate), &ed);
  g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), "changed",
                   G_CALLBACK(_on_repo_selection_changed), &ed);

  // empty field, nothing selected
  gtk_widget_set_sensitive(add_btn, FALSE);
  gtk_widget_set_sensitive(remove_btn, FALSE);

  gtk_widget_show_all(dialog);
  const gint response = gtk_dialog_run(GTK_DIALOG(dialog));

  if(response == GTK_RESPONSE_ACCEPT)
  {
    // the official repository is row zero and not ours to write
    GList *edited = NULL;
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter);
    while(valid)
    {
      gchar *repo = NULL;
      gboolean removable = FALSE;
      gtk_tree_model_get(GTK_TREE_MODEL(store), &iter,
                         0, &repo, 1, &removable, -1);
      if(removable && repo) edited = g_list_append(edited, repo);
      else g_free(repo);
      valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter);
    }
    dt_ai_models_set_third_party_repositories(edited);
    g_list_free_full(edited, g_free);
  }

  gtk_widget_destroy(dialog);

  if(response != GTK_RESPONSE_ACCEPT) return;

  // rebuild the combo so an added repository is selectable straight away.
  // the handler is blocked meanwhile: repopulating fires value-changed for
  // every entry, and each one would start a fetch
  const char *current = dt_bauhaus_combobox_get_text(rd->combo);
  gchar *keep = g_strdup(current);

  g_signal_handlers_block_by_func(rd->combo, _on_repo_combo_changed, rd);
  dt_bauhaus_combobox_clear(rd->combo);

  GList *repos = dt_ai_models_get_repositories();
  int index = 0, restore = 0;
  for(GList *l = repos; l; l = g_list_next(l), index++)
  {
    dt_bauhaus_combobox_add(rd->combo, (const char *)l->data);
    if(!g_strcmp0((const char *)l->data, keep)) restore = index;
  }
  g_list_free_full(repos, g_free);
  g_signal_handlers_unblock_by_func(rd->combo, _on_repo_combo_changed, rd);

  // a removed repository leaves the selection elsewhere, so repopulate
  dt_bauhaus_combobox_set(rd->combo, restore);
  g_free(keep);
}

static void _on_install_from_repository(GtkButton *button, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;

  GList *repositories = dt_ai_models_get_repositories();
  if(!repositories)
  {
    dt_gui_show_yes_no_dialog(_("no repository configured"), "ai_no_repo",
                              "%s", _("set plugins/ai/repository first"));
    return;
  }

  GtkWidget *dialog = gtk_dialog_new_with_buttons(
    _("install models from repository"),
    GTK_WINDOW(data->parent_dialog),
    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
    _("_cancel"), GTK_RESPONSE_CANCEL,
    _("_install"), GTK_RESPONSE_ACCEPT,
    NULL);
  gtk_window_set_default_size(GTK_WINDOW(dialog), DT_PIXEL_APPLY_DPI(520),
                              DT_PIXEL_APPLY_DPI(400));

  GtkListStore *store = gtk_list_store_new(REPO_NUM_COLS,
                                           G_TYPE_BOOLEAN,  // selected
                                           G_TYPE_STRING,   // name
                                           G_TYPE_STRING,   // version
                                           G_TYPE_STRING,   // task
                                           G_TYPE_STRING,   // status
                                           G_TYPE_BOOLEAN,  // selectable
                                           G_TYPE_STRING,   // tooltip
                                           G_TYPE_POINTER); // entry, owned
                                                            // by the cache

  // handlers need the dialog state, and some are connected while widgets
  // are still being built, so it is declared up front and filled in as it
  // goes rather than assembled at the end
  dt_repo_dialog_t rd = { 0 };
  rd.prefs = data;
  rd.dialog = dialog;
  rd.store = store;
  rd.cache = g_hash_table_new_full(
    g_str_hash, g_str_equal, g_free,
    (GDestroyNotify)dt_ai_repo_model_list_free);

  GtkWidget *combo = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(combo, NULL, N_("repository"));
  for(GList *r = repositories; r; r = g_list_next(r))
    dt_bauhaus_combobox_add(combo, (const char *)r->data);

  rd.combo = combo;
  // a bauhaus widget carries its own label and expects the full row
  dt_gui_dialog_add(dialog, combo);

  GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
  g_object_unref(store);  // the view holds its own reference now
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), TRUE);
  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(view), REPO_COL_TOOLTIP);

  GtkCellRenderer *toggle = gtk_cell_renderer_toggle_new();
  g_signal_connect(toggle, "toggled", G_CALLBACK(_on_repo_toggle), &rd);
  gtk_tree_view_append_column(
    GTK_TREE_VIEW(view),
    gtk_tree_view_column_new_with_attributes(
      "", toggle,
      "active", REPO_COL_SELECTED,
      "activatable", REPO_COL_SELECTABLE, NULL));
  gtk_tree_view_append_column(
    GTK_TREE_VIEW(view),
    gtk_tree_view_column_new_with_attributes(
      _("name"), gtk_cell_renderer_text_new(),
      "text", REPO_COL_NAME, NULL));
  gtk_tree_view_append_column(
    GTK_TREE_VIEW(view),
    gtk_tree_view_column_new_with_attributes(
      _("version"), gtk_cell_renderer_text_new(),
      "text", REPO_COL_VERSION, NULL));
  gtk_tree_view_append_column(
    GTK_TREE_VIEW(view),
    gtk_tree_view_column_new_with_attributes(
      _("task"), gtk_cell_renderer_text_new(),
      "text", REPO_COL_TASK, NULL));
  gtk_tree_view_append_column(
    GTK_TREE_VIEW(view),
    gtk_tree_view_column_new_with_attributes(
      _("status"), gtk_cell_renderer_text_new(),
      "text", REPO_COL_STATUS, NULL));

  // a message replaces the table rather than sitting above it, so the
  // dialog never shows an empty grid with an explanation floating over it
  GtkWidget *message = gtk_label_new("");
  gtk_label_set_line_wrap(GTK_LABEL(message), TRUE);
  gtk_label_set_justify(GTK_LABEL(message), GTK_JUSTIFY_CENTER);
  gtk_widget_set_halign(message, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(message, GTK_ALIGN_CENTER);

  GtkWidget *stack = gtk_stack_new();
  gtk_widget_set_vexpand(stack, TRUE);
  gtk_stack_add_named(GTK_STACK(stack), dt_gui_scroll_wrap(view), "list");
  gtk_stack_add_named(GTK_STACK(stack), message, "message");
  dt_gui_dialog_add(dialog, stack);

  // editing the list is rare and its effect outlives this dialog, so it
  // goes to the left of the action row, away from install. same treatment
  // as the help button in dt_gui_dialog_add_help: GTK_RESPONSE_NONE with
  // the dialog's own handler disconnected, so clicking does not close it
  GtkWidget *manage_btn = gtk_dialog_add_button(
    GTK_DIALOG(dialog), _("manage repositories…"), GTK_RESPONSE_NONE);
  gtk_widget_set_tooltip_text(manage_btn,
    _("add or remove the repositories models can be installed from"));
  // the action row packs to the end, so reordering alone would only move
  // it within the right-hand cluster; secondary puts it at the far left
  GtkWidget *action_box = gtk_widget_get_parent(manage_btn);
  gtk_button_box_set_child_non_homogeneous(GTK_BUTTON_BOX(action_box),
                                           manage_btn, TRUE);
  gtk_button_box_set_child_secondary(GTK_BUTTON_BOX(action_box),
                                     manage_btn, TRUE);
  g_signal_handlers_disconnect_by_data(manage_btn, dialog);
  g_signal_connect(manage_btn, "clicked",
                   G_CALLBACK(_on_manage_repositories), &rd);

  rd.stack = stack;
  rd.message = message;
  rd.manage_btn = manage_btn;

  g_signal_connect(combo, "value-changed",
                   G_CALLBACK(_on_repo_combo_changed), &rd);

  gtk_widget_show_all(dialog);
  rd.idle_id = g_idle_add(_repo_initial_fetch, &rd);  // fetches the first

  const gint response = gtk_dialog_run(GTK_DIALOG(dialog));

  // rd lives on this stack, so nothing may reference it past here
  if(rd.idle_id) g_source_remove(rd.idle_id);

  // collect the selection before tearing the dialog down
  GList *to_install = NULL;
  if(response == GTK_RESPONSE_ACCEPT)
  {
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter);
    while(valid)
    {
      gboolean selected = FALSE;
      dt_ai_repo_model_t *entry = NULL;
      gtk_tree_model_get(GTK_TREE_MODEL(store), &iter,
                         REPO_COL_SELECTED, &selected,
                         REPO_COL_ENTRY, &entry, -1);
      if(selected && entry) to_install = g_list_append(to_install, entry);
      valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter);
    }
  }

  gtk_widget_destroy(dialog);

  // register first so the shared download dialog applies unchanged
  gboolean any = FALSE;
  for(GList *l = to_install; l; l = g_list_next(l))
  {
    const dt_ai_repo_model_t *m = (const dt_ai_repo_model_t *)l->data;
    if(!_confirm_replacement(m)) continue;
    if(!dt_ai_models_register_repository_model(m)) continue;
    if(!_download_model_with_dialog(data, m->id))
    {
      // nothing landed on disk, so put back what the registration replaced
      dt_ai_models_unregister_repository_model(m->id);
      break;  // error or cancel
    }
    // only now is the publisher a fact worth recording
    dt_ai_models_record_origin(m->id, m->repository);
    any = TRUE;
  }

  if(any)
  {
    dt_ai_models_refresh_status();
    _refresh_model_list(data);
  }

  g_list_free(to_install);
  g_hash_table_destroy(rd.cache);
  g_list_free_full(repositories, g_free);
}

#endif // HAVE_AI_DOWNLOAD

static void _on_install_model(GtkButton *button, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;

  GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
    _("install AI models"),
    GTK_WINDOW(data->parent_dialog),
    GTK_FILE_CHOOSER_ACTION_OPEN,
    _("_open"), _("_cancel"));

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), TRUE);

  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, _("AI model packages (*.dtmodel)"));
  gtk_file_filter_add_pattern(filter, "*.dtmodel");
  gtk_file_filter_add_pattern(filter, "*.DTMODEL");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);
  gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser))
     != GTK_RESPONSE_ACCEPT)
  {
    g_object_unref(filechooser);
    return;
  }

  GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(filechooser));
  g_object_unref(filechooser);

  int ok = 0;
  GString *errors = g_string_new(NULL);
  for(GSList *l = files; l; l = l->next)
  {
    const char *filepath = (const char *)l->data;
    char *error = dt_ai_models_install_local(filepath);
    if(error)
    {
      gchar *base = g_path_get_basename(filepath);
      g_string_append_printf(errors, "%s: %s\n", base, error);
      g_free(base);
      g_free(error);
    }
    else
    {
      ok++;
    }
  }
  g_slist_free_full(files, g_free);

  if(ok)
  {
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_AI_MODELS_CHANGED);
    _refresh_model_list(data);
  }

  if(errors->len)
  {
    GtkWidget *err_dialog = gtk_message_dialog_new(
      GTK_WINDOW(data->parent_dialog),
      GTK_DIALOG_MODAL,
      GTK_MESSAGE_ERROR,
      GTK_BUTTONS_OK,
      "%s", errors->str);
    gtk_dialog_run(GTK_DIALOG(err_dialog));
    gtk_widget_destroy(err_dialog);
  }
  g_string_free(errors, TRUE);
}

static void _on_delete_selected(GtkButton *button, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;

  // collect selected models that are downloaded
  GList *ids = _get_selected_model_ids(data);
  GList *to_delete = NULL;
  int delete_count = 0;

  for(GList *l = ids; l; l = g_list_next(l))
  {
    const char *id = (const char *)l->data;
    dt_ai_model_t *model = dt_ai_models_get_by_id(id);
    if(model)
    {
      if(model->status == DT_AI_MODEL_DOWNLOADED
         || model->status == DT_AI_MODEL_UPDATE_AVAILABLE)
      {
        to_delete = g_list_append(to_delete, g_strdup(id));
        delete_count++;
      }
      dt_ai_model_free(model);
    }
  }
  g_list_free_full(ids, g_free);

  if(delete_count == 0)
  {
    g_list_free_full(to_delete, g_free);
    return;
  }

  // confirm deletion (uses DT helper for consistent yes/no lowercase
  // buttons with the rest of the UI, e.g. image deletion confirm)
  const gboolean confirmed = dt_gui_show_yes_no_dialog(
    ngettext("delete model?", "delete models?", delete_count),
    "",
    ngettext("do you really want to delete %d selected model?",
             "do you really want to delete %d selected models?",
             delete_count),
    delete_count);

  if(confirmed)
  {
    gboolean any_deleted = FALSE;
    for(GList *l = to_delete; l; l = g_list_next(l))
    {
      const char *model_id = (const char *)l->data;
      if(dt_ai_models_delete(model_id))
      {
        dt_print(DT_DEBUG_AI, "[preferences_ai] deleted model: %s", model_id);
        any_deleted = TRUE;
      }
    }

    if(any_deleted)
      DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_AI_MODELS_CHANGED);

    _refresh_model_list(data);
  }

  g_list_free_full(to_delete, g_free);
}

// show model card dialog for the given model_id
static void _show_model_card(dt_prefs_ai_data_t *data,
                             const char *model_id)
{
  if(!model_id || !model_id[0]) return;

  const char *dash = "\xe2\x80\x93";  // en dash for missing fields
  dt_ai_model_card_t *card = dt_ai_models_get_card(model_id);

  const char *name = (card && card->name)
    ? card->name : model_id;
  const char *desc = (card && card->long_description)
    ? card->long_description : dash;

  GtkWidget *dlg = gtk_message_dialog_new(
    GTK_WINDOW(data->parent_dialog),
    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
    GTK_MESSAGE_INFO,
    GTK_BUTTONS_CLOSE,
    "%s", desc);
  gtk_window_set_title(GTK_WINDOW(dlg), name);

  // field grid in the message area below the description
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_widget_set_margin_top(grid, 12);

  GtkWidget *msg_area
    = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(dlg));
  gtk_widget_set_margin_top(msg_area, 8);
  gtk_container_add(GTK_CONTAINER(msg_area), grid);

  const char *labels[] = {
    N_("scope"), N_("author"),
    N_("source"), N_("paper"),
    N_("license"), N_("training data"),
    N_("data license"), N_("notes")
  };
  const char *values[] = {
    card ? card->scope : NULL,
    card ? card->author : NULL,
    card ? card->source : NULL,
    card ? card->paper : NULL,
    card ? card->license : NULL,
    card ? card->training_data : NULL,
    card ? card->training_data_license : NULL,
    card ? card->notes : NULL
  };
  const int n_fields = (int)(sizeof(labels) / sizeof(labels[0]));

  for(int i = 0; i < n_fields; i++)
  {
    GtkWidget *lbl = gtk_label_new(_(labels[i]));
    gtk_label_set_xalign(GTK_LABEL(lbl), 1.0f);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, i, 1, 1);

    const char *v = values[i] ? values[i] : dash;
    GtkWidget *val;
    // render URLs as clickable links
    if(g_str_has_prefix(v, "http://")
       || g_str_has_prefix(v, "https://"))
    {
      gchar *markup = g_markup_printf_escaped(
        "<a href=\"%s\">%s</a>", v, v);
      val = gtk_label_new(NULL);
      gtk_label_set_markup(GTK_LABEL(val), markup);
      g_free(markup);
    }
    else
    {
      val = gtk_label_new(v);
    }
    gtk_label_set_xalign(GTK_LABEL(val), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(val), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(val), 50);
    gtk_label_set_selectable(GTK_LABEL(val), TRUE);
    gtk_grid_attach(GTK_GRID(grid), val, 1, i, 1, 1);
  }

  gtk_widget_show_all(dlg);
  gtk_dialog_run(GTK_DIALOG(dlg));
  gtk_widget_destroy(dlg);

  dt_ai_model_card_free(card);
}

// is the info column at bin-window coords (bx,by) active (downloaded row)?
static gboolean _info_active_at_bin(dt_prefs_ai_data_t *data,
                                    GtkTreeView *tv, gint bx, gint by)
{
  GtkTreePath *path = NULL;
  GtkTreeViewColumn *column = NULL;
  gboolean active = FALSE;
  if(gtk_tree_view_get_path_at_pos(tv, bx, by, &path, &column, NULL, NULL)
     && column == data->info_col)
  {
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(GTK_TREE_MODEL(data->model_store), &iter, path))
      gtk_tree_model_get(GTK_TREE_MODEL(data->model_store),
                         &iter, COL_INFO, &active, -1);
  }
  if(path) gtk_tree_path_free(path);
  return active;
}

// tooltip on info column for downloaded rows. query-tooltip x/y are
// in widget coords; convert to bin-window coords for the row lookup
static gboolean _on_query_tooltip(GtkWidget *widget,
                                  gint x, gint y,
                                  gboolean keyboard_mode,
                                  GtkTooltip *tooltip,
                                  gpointer user_data)
{
  (void)keyboard_mode;
  GtkTreeView *tv = GTK_TREE_VIEW(widget);
  gint bx, by;
  gtk_tree_view_convert_widget_to_bin_window_coords(tv, x, y, &bx, &by);
  if(!_info_active_at_bin(user_data, tv, bx, by)) return FALSE;
  gtk_tooltip_set_text(tooltip, _("click for model details"));
  return TRUE;
}

// hand cursor on info column for downloaded rows
static void _on_tree_motion_cb(GtkEventControllerMotion *controller,
                                  double x, double y,
                                  gpointer user_data)
{
  GtkWidget *widget = dt_gui_get_widget(controller);
  GtkTreeView *tv = GTK_TREE_VIEW(widget);
  GdkWindow *bin = gtk_tree_view_get_bin_window(tv);
  if(!bin) return;
  gint bx, by;
  gtk_tree_view_convert_widget_to_bin_window_coords(tv, (gint)x, (gint)y, &bx, &by);
  if(_info_active_at_bin(user_data, tv, bx, by))
  {
    GdkCursor *cursor = gdk_cursor_new_from_name(gdk_window_get_display(bin), "pointer");
    gdk_window_set_cursor(bin, cursor);
    g_object_unref(cursor);
  }
  else
  {
    gdk_window_set_cursor(bin, NULL);
  }
}

// click on the ⓘ info column opens the model card
static void _on_info_button_press_cb(GtkGestureSingle *gesture, int n_press,
                                       double x, double y,
                                       gpointer user_data)
{
  GtkWidget *widget = dt_gui_get_widget(gesture);
  if(gtk_gesture_single_get_current_button(gesture) != 1) return;

  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;
  GtkTreeView *tv = GTK_TREE_VIEW(widget);
  GtkTreePath *path = NULL;
  GtkTreeViewColumn *column = NULL;

  /* gesture coordinates are relative to the widget allocation, while
   * gtk_tree_view_get_path_at_pos() expects bin-window coordinates */
  gint bin_x, bin_y;
  gtk_tree_view_convert_widget_to_bin_window_coords(tv, (gint)x, (gint)y, &bin_x, &bin_y);

  if(!gtk_tree_view_get_path_at_pos(tv, bin_x, bin_y,
                                    &path, &column, NULL, NULL))
    return;

  // only react to clicks on the info column
  if(column != data->info_col)
  {
    gtk_tree_path_free(path);
    return;
  }

  GtkTreeIter iter;
  if(gtk_tree_model_get_iter(GTK_TREE_MODEL(data->model_store), &iter, path))
  {
    gchar *model_id = NULL;
    gboolean has_info = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(data->model_store),
                       &iter, COL_ID, &model_id, COL_INFO, &has_info, -1);
    if(model_id && has_info)
      _show_model_card(data, model_id);
    g_free(model_id);
  }
  gtk_tree_path_free(path);
}

#if !defined(__APPLE__)
static void _show_ort_probe_result(GtkWindow *parent, const char *path, const char *version)
{
  GtkWidget *dlg;
  if(version)
    dlg = gtk_message_dialog_new(parent,
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
      _("ONNX Runtime %s detected.\nRestart darktable to apply."), version);
  else
    dlg = gtk_message_dialog_new(parent,
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
      _("not a valid ONNX Runtime library:\n%s"), path);
  gtk_dialog_run(GTK_DIALOG(dlg));
  gtk_widget_destroy(dlg);
}

// commit a new ORT library path: update conf + indicator and refresh
// the provider combo to reflect the new library's advertised EPs
static void _set_ort_path(dt_prefs_ai_data_t *data, const char *path)
{
  dt_conf_set_string("plugins/ai/ort_library_path", path ? path : "");
  _update_string_indicator(data->ort_path_indicator,
                           "plugins/ai/ort_library_path");
  data->supported_providers = _compute_supported_providers(path);
  _refresh_provider_combo(data);
}

static void _on_detect_system_ort(GtkButton *button, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;
  GList *found = dt_ai_ort_find_libraries();
  const guint count = g_list_length(found);

  if(count == 0)
  {
    GtkWidget *dlg = gtk_message_dialog_new(
      GTK_WINDOW(data->parent_dialog),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
      _("no system ONNX Runtime library found.\n\n"
        "install one via your package manager or use\n"
        "the browse button to select a custom build."));
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
  }
  else if(count == 1)
  {
    const dt_ai_ort_found_t *f = found->data;
    gtk_entry_set_text(GTK_ENTRY(data->ort_path_entry), f->path);
    _set_ort_path(data, f->path);
    GtkWidget *dlg = gtk_message_dialog_new(
      GTK_WINDOW(data->parent_dialog),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
      _("ONNX Runtime %s [%s]\n%s\n\nRestart darktable to apply."),
      f->version, f->eps, f->path);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
  }
  else
  {
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
      _("select ONNX Runtime library"),
      GTK_WINDOW(data->parent_dialog),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      _("_cancel"), GTK_RESPONSE_CANCEL,
      _("_select"), GTK_RESPONSE_ACCEPT,
      NULL);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), DT_PIXEL_APPLY_DPI(10));

    GtkWidget *label = gtk_label_new(_("multiple ONNX Runtime libraries found:"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_margin_bottom(label, DT_PIXEL_APPLY_DPI(5));
    gtk_container_add(GTK_CONTAINER(content), label);

    GtkWidget *combo = gtk_combo_box_text_new();
    for(GList *l = found; l; l = g_list_next(l))
    {
      const dt_ai_ort_found_t *f = l->data;
      gchar *entry = g_strdup_printf("ONNX Runtime %s [%s]  %s",
                                     f->version, f->eps, f->path);
      gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), entry);
      g_free(entry);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    gtk_container_add(GTK_CONTAINER(content), combo);
    gtk_widget_show_all(content);

    if(gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
    {
      const int sel = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
      if(sel >= 0)
      {
        const dt_ai_ort_found_t *f = g_list_nth_data(found, sel);
        gtk_entry_set_text(GTK_ENTRY(data->ort_path_entry), f->path);
        _set_ort_path(data, f->path);
      }
    }
    gtk_widget_destroy(dlg);
  }

  g_list_free_full(found, (GDestroyNotify)dt_ai_ort_found_free);
}

static void _reset_ort_path_click_cb(GtkGestureSingle *gesture, int n_press,
                                        double x, double y,
                                        gpointer user_data)
{
  if(n_press < 2) return;
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;
  gtk_entry_set_text(GTK_ENTRY(data->ort_path_entry), "");
  _set_ort_path(data, "");
}
static void _on_ort_path_changed(GtkEntry *entry, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;
  const char *text = gtk_entry_get_text(GTK_ENTRY(data->ort_path_entry));

  // empty = reset to bundled
  if(!text || !text[0])
  {
    _set_ort_path(data, "");
    return;
  }

  gchar *version = dt_ai_ort_probe_library(text);
  _show_ort_probe_result(GTK_WINDOW(data->parent_dialog), text, version);
  if(!version)
  {
    // revert entry to saved config
    gchar *prev = dt_conf_get_string("plugins/ai/ort_library_path");
    gtk_entry_set_text(GTK_ENTRY(data->ort_path_entry), prev ? prev : "");
    g_free(prev);
    return;
  }

  _set_ort_path(data, text);
  g_free(version);
}

static void _on_ort_browse_clicked(GtkButton *button, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;

  GtkFileChooserNative *chooser = gtk_file_chooser_native_new(
    _("select ONNX Runtime library"),
    GTK_WINDOW(data->parent_dialog),
    GTK_FILE_CHOOSER_ACTION_OPEN,
    _("_open"), _("_cancel"));

  // filter for shared libraries
  GtkFileFilter *filter = gtk_file_filter_new();
#ifdef _WIN32
  gtk_file_filter_set_name(filter, _("ONNX Runtime (onnxruntime*.dll)"));
  gtk_file_filter_add_pattern(filter, "onnxruntime*.dll");
#else
  gtk_file_filter_set_name(filter, _("ONNX Runtime (libonnxruntime.so*)"));
  gtk_file_filter_add_pattern(filter, "libonnxruntime.so*");
#endif
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);

  GtkFileFilter *all_filter = gtk_file_filter_new();
  gtk_file_filter_set_name(all_filter, _("all files"));
  gtk_file_filter_add_pattern(all_filter, "*");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), all_filter);

  // start in the current path's directory if set
  gchar *cur = dt_conf_get_string("plugins/ai/ort_library_path");
  if(cur && cur[0])
  {
    gchar *dir = g_path_get_dirname(cur);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), dir);
    g_free(dir);
  }
  g_free(cur);

  gchar *filename = NULL;
  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser))
     == GTK_RESPONSE_ACCEPT)
    filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));

  g_object_unref(chooser);

  if(filename)
  {
    gchar *version = dt_ai_ort_probe_library(filename);
    _show_ort_probe_result(GTK_WINDOW(data->parent_dialog), filename, version);
    if(version)
    {
      gtk_entry_set_text(GTK_ENTRY(data->ort_path_entry), filename);
      _set_ort_path(data, filename);
      g_free(version);
    }
    g_free(filename);
  }
}
#endif // !__APPLE__

void init_tab_ai(GtkWidget *dialog, GtkWidget *stack)
{
  dt_prefs_ai_data_t *data = g_new0(dt_prefs_ai_data_t, 1);
  data->parent_dialog = dialog;

  // main vertical box holds two independent sections
  GtkWidget *main_box = dt_gui_vbox();

  int row = 0;

  // enable AI toggle
  GtkWidget *enable_label = gtk_label_new(_("enable AI features"));
  gtk_widget_set_halign(enable_label, GTK_ALIGN_START);
  GtkWidget *enable_labelev = gtk_event_box_new();

  gtk_container_add(GTK_CONTAINER(enable_labelev), enable_label);
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(enable_labelev), FALSE);

  data->enable_indicator = _create_indicator("plugins/ai/enabled");
  data->enable_toggle = gtk_check_button_new();
  gtk_toggle_button_set_active(
    GTK_TOGGLE_BUTTON(data->enable_toggle),
    dt_conf_get_bool("plugins/ai/enabled"));
  g_signal_connect(
    data->enable_toggle,
    "toggled",
    G_CALLBACK(_on_enable_toggled),
    data);
  dt_gui_connect_click_all(enable_labelev, _reset_enable_click_cb, NULL, data->enable_toggle);
  // single grid for enable, provider, and ORT path (column alignment)
  GtkWidget *settings_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(settings_grid), DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(GTK_GRID(settings_grid), DT_PIXEL_APPLY_DPI(5));
  row = 0;

  {
    GtkWidget *seclabel = gtk_label_new(_("general"));
    GtkWidget *lbox = dt_gui_hbox(seclabel);
    gtk_widget_set_name(lbox, "pref_section");
    gtk_grid_attach(GTK_GRID(settings_grid), lbox, 0, row++, 5, 1);
  }

  gtk_grid_attach(GTK_GRID(settings_grid), enable_labelev, 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(settings_grid), data->enable_indicator, 1, row, 1, 1);
  gtk_grid_attach(GTK_GRID(settings_grid), data->enable_toggle, 2, row++, 1, 1);

  // rows below this are greyed out when AI is disabled
  data->settings_grid = settings_grid;
  data->controls_start_row = row;
  const gboolean ai_on = dt_conf_get_bool("plugins/ai/enabled");

  // provider dropdown
  GtkWidget *provider_label = gtk_label_new(_("AI acceleration"));
  gtk_widget_set_halign(provider_label, GTK_ALIGN_START);
  GtkWidget *provider_labelev = gtk_event_box_new();

  gtk_container_add(GTK_CONTAINER(provider_labelev), provider_label);
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(provider_labelev), FALSE);

  data->provider_indicator = _create_indicator(DT_AI_CONF_PROVIDER);
  data->provider_combo = dt_bauhaus_combobox_new(NULL);
  data->gpu_combo      = dt_bauhaus_combobox_new(NULL);
  gtk_widget_set_tooltip_text(data->gpu_combo,
                              _("select which GPU to use when the active provider"
                                " supports more than one"));

  // connect signals before the first refresh — the refresh blocks the
  // value-changed handler while it repopulates, so we need it attached
  g_signal_connect(data->provider_combo,
                   "value-changed",
                   G_CALLBACK(_on_provider_changed),
                   data);
  dt_gui_connect_click_all(provider_labelev, _reset_provider_click_cb, NULL, data);
  g_signal_connect(data->gpu_combo, "value-changed",
                   G_CALLBACK(_on_gpu_changed), data);

  // filter the combo to what the currently-configured ORT advertises
  char *ort_path = dt_conf_get_string("plugins/ai/ort_library_path");
  data->supported_providers = _compute_supported_providers(ort_path);
  g_free(ort_path);
  _refresh_provider_combo(data);  // will also refresh gpu_combo
  data->provider_status = gtk_label_new(NULL);
  gtk_label_set_use_markup(GTK_LABEL(data->provider_status), TRUE);
  gtk_widget_set_halign(data->provider_status, GTK_ALIGN_START);
  gtk_widget_set_margin_start(data->provider_status, DT_PIXEL_APPLY_DPI(8));

  // put combo + gpu + status in an hbox so the combo doesn't stretch
  // when column 2 expands for the ORT path entry below
  GtkWidget *provider_hbox = dt_gui_hbox(data->provider_combo,
                                         data->gpu_combo,
                                         data->provider_status);
  // small gap between provider_combo and gpu_combo
  gtk_widget_set_margin_start(data->gpu_combo, DT_PIXEL_APPLY_DPI(8));
  // gpu_combo is hidden by default; _refresh_gpu_combo shows it if applicable
  gtk_widget_set_no_show_all(data->gpu_combo, TRUE);

  gtk_grid_attach(GTK_GRID(settings_grid), provider_labelev, 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(settings_grid), data->provider_indicator, 1, row, 1, 1);
  // span only cols 2-3 (matching path_entry below) so col 4 stays sized
  // by the button box and doesn't steal width when gpu_combo appears
  gtk_grid_attach(GTK_GRID(settings_grid), provider_hbox, 2, row++, 2, 1);

  // ORT library path — not shown on macOS where ORT is statically linked with CoreML.
  // Developers can still use DT_ORT_LIBRARY env var to override on macOS
#if !defined(__APPLE__)
  {
    GtkWidget *path_label = gtk_label_new(_("ONNX Runtime library"));
    gtk_widget_set_halign(path_label, GTK_ALIGN_START);
    GtkWidget *path_labelev = gtk_event_box_new();
    gtk_widget_add_events(path_labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(path_labelev), path_label);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(path_labelev), FALSE);

    data->ort_path_indicator = _create_indicator("plugins/ai/ort_library_path");

    gchar *cur_path = dt_conf_get_string("plugins/ai/ort_library_path");
    data->ort_path_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(data->ort_path_entry), cur_path ? cur_path : "");
#if defined(_WIN32)
    gtk_entry_set_placeholder_text(GTK_ENTRY(data->ort_path_entry),
                                   _("bundled (DirectML)"));
#else
    gtk_entry_set_placeholder_text(GTK_ENTRY(data->ort_path_entry),
                                   _("bundled (CPU only)"));
#endif
    gtk_widget_set_tooltip_text(data->ort_path_entry,
                                _("path to a GPU-enabled ONNX Runtime library.\n"
                                  "leave empty to use the bundled library.\n"
                                  "requires restart to take effect."));
    gtk_widget_set_hexpand(data->ort_path_entry, TRUE);
    g_free(cur_path);

    GtkWidget *browse_btn = dtgtk_button_new(dtgtk_cairo_paint_directory, CPF_NONE, NULL);
    gtk_widget_set_name(browse_btn, "non-flat");
    gtk_widget_set_tooltip_text(browse_btn,
                                _("select a custom ONNX Runtime shared library"));

    GtkWidget *detect_btn = gtk_button_new_with_label(_("detect"));
    gtk_widget_set_tooltip_text(detect_btn,
                                _("search for a system-installed ONNX Runtime library"));

    GtkWidget *btn_box = dt_gui_hbox(browse_btn, detect_btn);
    gtk_widget_set_valign(btn_box, GTK_ALIGN_CENTER);

    gtk_grid_attach(GTK_GRID(settings_grid), path_labelev, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(settings_grid), data->ort_path_indicator, 1, row, 1, 1);
    gtk_grid_attach(GTK_GRID(settings_grid), data->ort_path_entry, 2, row, 2, 1);
    gtk_grid_attach(GTK_GRID(settings_grid), btn_box, 4, row++, 1, 1);

    g_signal_connect(browse_btn, "clicked", G_CALLBACK(_on_ort_browse_clicked), data);
    g_signal_connect(detect_btn, "clicked", G_CALLBACK(_on_detect_system_ort), data);
    g_signal_connect(data->ort_path_entry, "activate", G_CALLBACK(_on_ort_path_changed), data);
    dt_gui_connect_click_all(path_labelev, _reset_ort_path_click_cb, NULL, data);
  }
#endif // !__APPLE__

  dt_gui_box_add(main_box, settings_grid);

  // controls_box wraps the models section - greyed out when AI disabled
  data->controls_box = dt_gui_vbox();

  // apply initial sensitivity
  _update_controls_sensitivity(data, ai_on);
  dt_gui_box_add(main_box, data->controls_box);

  // "models" section with its own grid
  GtkWidget *models_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(models_grid), DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(GTK_GRID(models_grid), DT_PIXEL_APPLY_DPI(5));

  row = 0;

  // "models" section header
  {
    GtkWidget *seclabel = gtk_label_new(_("models"));
    GtkWidget *lbox = dt_gui_hbox(seclabel);
    gtk_widget_set_name(lbox, "pref_section");
    gtk_grid_attach(GTK_GRID(models_grid), lbox, 0, row++, 1, 1);
  }

  // create model list store
  data->model_store = gtk_list_store_new(
    NUM_COLS,
    G_TYPE_BOOLEAN, // selected
    G_TYPE_STRING,  // name
    G_TYPE_BOOLEAN, // info icon visible
    G_TYPE_STRING,  // version
    G_TYPE_STRING,  // task
    G_TYPE_BOOLEAN, // enabled
    G_TYPE_BOOLEAN, // enabled_sensitive
    G_TYPE_STRING,  // status
    G_TYPE_STRING,  // repository
    G_TYPE_STRING,  // default
    G_TYPE_STRING); // id

  // sort by task, then default, then name
  gtk_tree_sortable_set_default_sort_func(
    GTK_TREE_SORTABLE(data->model_store), _model_sort_func, NULL, NULL);
  gtk_tree_sortable_set_sort_column_id(
    GTK_TREE_SORTABLE(data->model_store),
    GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID, GTK_SORT_ASCENDING);

  // create tree view
  data->model_list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(data->model_store));
  g_object_unref(data->model_store); // Tree view takes ownership

  // selection checkbox column (no title, with select-all checkbox in header)
  GtkCellRenderer *toggle_renderer = gtk_cell_renderer_toggle_new();
  g_signal_connect(
    toggle_renderer,
    "toggled",
    G_CALLBACK(_on_model_selection_toggled),
    data);
  GtkTreeViewColumn *select_col = gtk_tree_view_column_new_with_attributes(
    "",
    toggle_renderer,
    "active",
    COL_SELECTED,
    NULL);

  // add select-all checkbox as column header widget
  data->select_all_toggle = gtk_check_button_new();
  gtk_widget_set_tooltip_text(data->select_all_toggle, _("select/deselect all"));
  g_signal_connect(
    data->select_all_toggle,
    "toggled",
    G_CALLBACK(_on_select_all_toggled),
    data);
  gtk_widget_show(data->select_all_toggle);
  gtk_tree_view_column_set_widget(select_col, data->select_all_toggle);
  gtk_tree_view_column_set_clickable(select_col, TRUE);

  gtk_tree_view_append_column(GTK_TREE_VIEW(data->model_list), select_col);

  // connect to the header button's clicked signal so the checkbox toggles
  // when clicking anywhere in the header area
  GtkWidget *select_col_button = gtk_tree_view_column_get_button(select_col);
  g_signal_connect(
    select_col_button,
    "clicked",
    G_CALLBACK(_on_select_all_header_clicked),
    data);

  // name column
  GtkCellRenderer *text_renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *name_col = gtk_tree_view_column_new_with_attributes(
    _("name"),
    text_renderer,
    "text",
    COL_NAME,
    NULL);
  gtk_tree_view_column_set_expand(name_col, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(data->model_list), name_col);

  // repository column, shown only when something not from the official
  // repository is installed — otherwise it would be empty for everyone
  data->repo_col = gtk_tree_view_column_new_with_attributes(
    _("repository"),
    text_renderer,
    "text",
    COL_REPOSITORY,
    NULL);
  gtk_tree_view_column_set_visible(data->repo_col, FALSE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(data->model_list), data->repo_col);

  // info icon column — click opens model card
  GtkCellRenderer *info_renderer
    = dtgtk_paint_cell_new(dtgtk_cairo_paint_info, 0, NULL);
  data->info_col = gtk_tree_view_column_new_with_attributes(
    "",
    info_renderer,
    "visible",
    COL_INFO,
    NULL);
  gtk_tree_view_column_set_clickable(data->info_col, FALSE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(data->model_list),
                              data->info_col);

  // version column
  GtkTreeViewColumn *version_col = gtk_tree_view_column_new_with_attributes(
    _("version"),
    text_renderer,
    "text",
    COL_VERSION,
    NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(data->model_list), version_col);

  // task column
  GtkTreeViewColumn *task_col = gtk_tree_view_column_new_with_attributes(
    _("task"),
    text_renderer,
    "text",
    COL_TASK,
    NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(data->model_list), task_col);

  // enabled checkbox column (radio-button behavior per task)
  GtkCellRenderer *enabled_renderer = gtk_cell_renderer_toggle_new();
  g_signal_connect(
    enabled_renderer,
    "toggled",
    G_CALLBACK(_on_enabled_toggled),
    data);
  GtkTreeViewColumn *enabled_col = gtk_tree_view_column_new_with_attributes(
    _("enabled"),
    enabled_renderer,
    "active", COL_ENABLED,
    "sensitive", COL_ENABLED_SENSITIVE,
    "activatable", COL_ENABLED_SENSITIVE,
    NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(data->model_list), enabled_col);

  // status column
  GtkTreeViewColumn *status_col = gtk_tree_view_column_new_with_attributes(
    _("status"),
    text_renderer,
    "text",
    COL_STATUS,
    NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(data->model_list), status_col);

  // default column
  GtkTreeViewColumn *default_col = gtk_tree_view_column_new_with_attributes(
    _("default"),
    text_renderer,
    "text",
    COL_DEFAULT,
    NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(data->model_list), default_col);

  gtk_widget_set_has_tooltip(data->model_list, TRUE);
  g_signal_connect(data->model_list, "query-tooltip",
                   G_CALLBACK(_on_query_tooltip), data);
  dt_gui_connect_motion(data->model_list, _on_tree_motion_cb, NULL, NULL, data);
  dt_gui_connect_click(data->model_list, _on_info_button_press_cb, NULL, data);

  // scrolled window for the list
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(
    GTK_SCROLLED_WINDOW(scroll),
    GTK_POLICY_AUTOMATIC,
    GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(
    GTK_SCROLLED_WINDOW(scroll),
    DT_PIXEL_APPLY_DPI(200));
  gtk_widget_set_hexpand(scroll, TRUE);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_container_add(GTK_CONTAINER(scroll), data->model_list);
  gtk_grid_attach(GTK_GRID(models_grid), scroll, 0, row++, 1, 1);

  // button box
  GtkWidget *button_box = dt_gui_hbox();
  gtk_widget_set_name(button_box, "ai-controls");

#ifdef HAVE_AI_DOWNLOAD
  // download / update default button
  data->download_default_btn
    = gtk_button_new_with_label(_("download / update default"));
  gtk_widget_set_tooltip_text(data->download_default_btn,
    _("download or update all default models"));
  g_signal_connect(
    data->download_default_btn,
    "clicked",
    G_CALLBACK(_on_download_default),
    data);
  dt_gui_box_add(button_box, data->download_default_btn);

  // download / update selected button
  data->download_selected_btn
    = gtk_button_new_with_label(_("download / update selected"));
  gtk_widget_set_tooltip_text(data->download_selected_btn,
    _("download or update the selected models"));
  g_signal_connect(
    data->download_selected_btn,
    "clicked",
    G_CALLBACK(_on_download_selected),
    data);
  dt_gui_box_add(button_box, data->download_selected_btn);

#endif // HAVE_AI_DOWNLOAD

  // import from file button
  data->install_btn = gtk_button_new_with_label(_("import from file…"));
  gtk_widget_set_tooltip_text(data->install_btn,
    _("install a model from a local .dtmodel file"));
  gtk_widget_set_margin_start(data->install_btn, DT_PIXEL_APPLY_DPI(16));
  g_signal_connect(data->install_btn, "clicked", G_CALLBACK(_on_install_model), data);
  dt_gui_box_add(button_box, data->install_btn);

#ifdef HAVE_AI_DOWNLOAD
  // pairs with "import from file…", so no margin between the two
  data->install_repo_btn
    = gtk_button_new_with_label(_("install from repository…"));
  gtk_widget_set_tooltip_text(data->install_repo_btn,
    _("browse every model the configured repository offers for this version "
      "of darktable, including any not listed above"));
  g_signal_connect(
    data->install_repo_btn,
    "clicked",
    G_CALLBACK(_on_install_from_repository),
    data);
  dt_gui_box_add(button_box, data->install_repo_btn);
#endif // HAVE_AI_DOWNLOAD

  // delete selected button
  data->delete_selected_btn = gtk_button_new_with_label(_("delete selected"));
  gtk_widget_set_tooltip_text(data->delete_selected_btn,
    _("remove the selected models from disk"));
  gtk_widget_set_margin_start(data->delete_selected_btn, DT_PIXEL_APPLY_DPI(16));
  g_signal_connect(
    data->delete_selected_btn,
    "clicked",
    G_CALLBACK(_on_delete_selected),
    data);
  dt_gui_box_add(button_box, data->delete_selected_btn);

  // help button (right-anchored, matches other prefs tabs)
  GtkWidget *help_btn = gtk_button_new_with_label(_("?"));
  gtk_widget_set_tooltip_text(help_btn, _("open help page for AI settings"));
  dt_gui_add_help_link(help_btn, "ai");
  g_signal_connect(help_btn, "clicked", G_CALLBACK(dt_gui_show_help), NULL);
  dt_gui_box_add(button_box, dt_gui_align_right(help_btn));

  dt_gui_box_add(data->controls_box, models_grid);

  // wrap in a scrolled container like other tabs
  GtkWidget *main_scroll = dt_gui_scroll_wrap(main_box);
  GtkWidget *tab_box = dt_gui_vbox(main_scroll, button_box);

  // add to stack
  gtk_stack_add_titled(GTK_STACK(stack), tab_box, "AI", _("AI"));

  // populate model list
  _refresh_model_list(data);

  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_AI_MODELS_CHANGED,
                            _ai_models_changed_cb, data);

  g_object_set_data_full(G_OBJECT(tab_box), "prefs-ai-data",
                         data, _prefs_ai_data_free);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
