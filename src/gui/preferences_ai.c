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
  COL_INFO,     // info icon column (static "ℹ" text)
  COL_VERSION,
  COL_TASK,
  COL_ENABLED,
  COL_ENABLED_SENSITIVE, // whether the enabled checkbox is clickable
  COL_STATUS,
  COL_DEFAULT,
  COL_ID,
  NUM_COLS
};

typedef struct dt_prefs_ai_data_t
{
  GtkWidget *enable_toggle;
  GtkWidget *enable_indicator;
  GtkWidget *provider_combo;
  GtkWidget *provider_indicator;
  GtkWidget *provider_status;
  GtkWidget *model_list;
  GtkListStore *model_store;
#ifdef HAVE_AI_DOWNLOAD
  GtkWidget *download_selected_btn;
  GtkWidget *download_default_btn;
  GtkWidget *download_all_btn;
#endif
  GtkWidget *install_btn;
  GtkWidget *delete_selected_btn;
  GtkWidget *parent_dialog;
  GtkWidget *select_all_toggle;
  GtkTreeViewColumn *info_col;
  GtkWidget *controls_box;  // container for all controls below the enable toggle
  GtkWidget *ort_path_entry;
  GtkWidget *ort_path_indicator;
  GtkWidget *settings_grid;
  int controls_start_row;   // first row to grey out when AI disabled
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

  g_free(task_a); g_free(task_b);
  g_free(default_a); g_free(default_b);
  g_free(name_a); g_free(name_b);
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

static void _refresh_model_list(dt_prefs_ai_data_t *data)
{
  if(!darktable.ai_registry)
  {
    dt_print(DT_DEBUG_AI, "[preferences_ai] registry is NULL");
    return;
  }

  gtk_list_store_clear(data->model_store);

  dt_ai_models_refresh_status(darktable.ai_registry);
  dt_ai_models_check_updates(darktable.ai_registry);

  const int count = dt_ai_models_get_count(darktable.ai_registry);
  dt_print(DT_DEBUG_AI, "[preferences_ai] refreshing model list, count=%d", count);
  for(int i = 0; i < count; i++)
  {
    dt_ai_model_t *model = dt_ai_models_get_by_index(darktable.ai_registry, i);
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
      COL_DEFAULT,
      model->is_default ? _("yes") : _("no"),
      COL_VERSION,
      (model->status == DT_AI_MODEL_DOWNLOADED
       || model->status == DT_AI_MODEL_UPDATE_AVAILABLE
       || model->status == DT_AI_MODEL_UPDATE_REQUIRED)
        ? (model->version ? model->version : "0.0") : "–",
      COL_ID,
      model->id,
      COL_INFO,
      "\xe2\x84\xb9",  // ℹ (U+2139)
      -1);
    dt_ai_model_free(model);
  }

  // reset select-all toggle
  if(data->select_all_toggle)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->select_all_toggle), FALSE);
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
    g_mutex_lock(&darktable.ai_registry->lock);
    darktable.ai_registry->ai_enabled = enabled;
    g_mutex_unlock(&darktable.ai_registry->lock);

    // lazy-init directories + models on first enable
    if(enabled)
    {
      dt_ai_models_init_lazy(darktable.ai_registry);
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

// map combo box index to provider table index (skipping unavailable providers)
static int _combo_idx_to_provider(int combo_idx)
{
  int visible = -1;
  for(int i = 0; i < DT_AI_PROVIDER_COUNT; i++)
  {
    if(!dt_ai_providers[i].available) continue;
    if(++visible == combo_idx)
      return i;
  }
  return 0;  // fallback to AUTO
}

// map provider enum value to combo box index
static int _provider_to_combo_idx(dt_ai_provider_t provider)
{
  int visible = -1;
  for(int i = 0; i < DT_AI_PROVIDER_COUNT; i++)
  {
    if(!dt_ai_providers[i].available) continue;
    visible++;
    if(dt_ai_providers[i].value == provider)
      return visible;
  }
  return 0;  // fallback to first visible (AUTO)
}

static void _update_provider_status(dt_prefs_ai_data_t *data, dt_ai_provider_t provider)
{
  if(!data->provider_status) return;

  // don't probe GPU providers when AI is disabled —
  // initializing MIGraphX/ROCm on unsupported GPUs can abort()
  if(!darktable.ai_registry || !darktable.ai_registry->ai_enabled)
  {
    gtk_label_set_text(GTK_LABEL(data->provider_status), "");
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
  const int pi = _combo_idx_to_provider(combo_idx);
  dt_conf_set_string(DT_AI_CONF_PROVIDER, dt_ai_providers[pi].config_string);
  if(darktable.ai_registry)
  {
    g_mutex_lock(&darktable.ai_registry->lock);
    darktable.ai_registry->provider = dt_ai_providers[pi].value;
    g_mutex_unlock(&darktable.ai_registry->lock);
  }
  _update_string_indicator(data->provider_indicator, DT_AI_CONF_PROVIDER);
  _update_provider_status(data, dt_ai_providers[pi].value);
}

// double-click on label resets the enable toggle to default
static gboolean
_reset_enable_click(GtkWidget *label, GdkEventButton *event, GtkWidget *widget)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    const gboolean def = dt_confgen_get_bool("plugins/ai/enabled", DT_DEFAULT);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), def);
    return TRUE;
  }
  return FALSE;
}

// double-click on label resets the provider combo to default
static gboolean
_reset_provider_click(GtkWidget *label, GdkEventButton *event, GtkWidget *widget)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    const char *def = dt_confgen_get(DT_AI_CONF_PROVIDER, DT_DEFAULT);
    dt_ai_provider_t provider = dt_ai_provider_from_string(def);
    dt_bauhaus_combobox_set(widget, _provider_to_combo_idx(provider));
    return TRUE;
  }
  return FALSE;
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
}

static void _on_select_all_header_clicked(GtkWidget *button, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;
  // only toggle if the click wasn't already handled by the checkbox itself.
  // block the toggled signal to prevent double-fire, then toggle manually
  g_signal_handlers_block_by_func(data->select_all_toggle, _on_select_all_toggled, data);
  gboolean active
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
  double progress = dl->progress;
  gboolean finished = dl->finished;
  char *error = dl->error ? g_strdup(dl->error) : NULL;
  g_mutex_unlock(&dl->mutex);

  if(dl->dialog && GTK_IS_WIDGET(dl->dialog))
  {
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dl->progress_bar), progress);

    char *text = g_strdup_printf("%.0f%%", progress * 100.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(dl->progress_bar), text);
    g_free(text);
  }

  if(finished)
  {
    if(error)
    {
      // show error in dialog
      if(dl->dialog && GTK_IS_WIDGET(dl->dialog))
      {
        gtk_label_set_text(GTK_LABEL(dl->status_label), error);
        gtk_widget_show(dl->status_label);
      }
      g_free(error);
    }
    else
    {
      // success - close dialog
      if(dl->dialog && GTK_IS_WIDGET(dl->dialog))
        gtk_dialog_response(GTK_DIALOG(dl->dialog), GTK_RESPONSE_OK);
    }
    return G_SOURCE_REMOVE;
  }

  g_free(error);
  return G_SOURCE_CONTINUE;
}

// download thread function
static gpointer _download_thread_func(gpointer user_data)
{
  dt_download_dialog_t *dl = (dt_download_dialog_t *)user_data;

  char *error = dt_ai_models_download_sync(
    darktable.ai_registry,
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
  dt_ai_model_t *model = dt_ai_models_get_by_id(darktable.ai_registry, model_id);
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

  // remove the timer. Any already-dispatched idle callback will see
  // dl->finished == TRUE and return G_SOURCE_REMOVE harmlessly.
  g_source_remove(timer_id);

  // destroy the dialog before freeing dl so no straggling callback
  // can touch destroyed widgets.
  gtk_widget_destroy(dialog);
  dl->dialog = NULL;

  gboolean success = (dl->error == NULL);

  // notify modules that models have changed
  if(success)
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_AI_MODELS_CHANGED);

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
    dt_ai_model_t *model = dt_ai_models_get_by_id(darktable.ai_registry, id);
    if(model)
    {
      gboolean need_download = (model->status == DT_AI_MODEL_NOT_DOWNLOADED
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
  const int count = dt_ai_models_get_count(darktable.ai_registry);
  for(int i = 0; i < count; i++)
  {
    dt_ai_model_t *model = dt_ai_models_get_by_index(darktable.ai_registry, i);
    if(!model)
      continue;
    gboolean need_download
      = (model->is_default
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

static void _on_download_all(GtkButton *button, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;

  // download all models that need downloading
  const int count = dt_ai_models_get_count(darktable.ai_registry);
  for(int i = 0; i < count; i++)
  {
    dt_ai_model_t *model = dt_ai_models_get_by_index(darktable.ai_registry, i);
    if(!model)
      continue;
    gboolean need_download = (model->status == DT_AI_MODEL_NOT_DOWNLOADED
                              || model->status == DT_AI_MODEL_UPDATE_AVAILABLE
                              || model->status == DT_AI_MODEL_UPDATE_REQUIRED);
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
#endif // HAVE_AI_DOWNLOAD

static void _on_install_model(GtkButton *button, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;

  GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
    _("install AI model"),
    GTK_WINDOW(data->parent_dialog),
    GTK_FILE_CHOOSER_ACTION_OPEN,
    _("_open"), _("_cancel"));

  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, _("AI model packages (*.dtmodel)"));
  gtk_file_filter_add_pattern(filter, "*.dtmodel");
  gtk_file_filter_add_pattern(filter, "*.DTMODEL");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);
  gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser))
     == GTK_RESPONSE_ACCEPT)
  {
    char *filepath
      = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));

    char *error = dt_ai_models_install_local(darktable.ai_registry, filepath);
    if(error)
    {
      GtkWidget *err_dialog = gtk_message_dialog_new(
        GTK_WINDOW(data->parent_dialog),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_ERROR,
        GTK_BUTTONS_OK,
        "%s", error);
      gtk_dialog_run(GTK_DIALOG(err_dialog));
      gtk_widget_destroy(err_dialog);
      g_free(error);
    }
    else
    {
      DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_AI_MODELS_CHANGED);
      _refresh_model_list(data);
    }
    g_free(filepath);
  }
  g_object_unref(filechooser);
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
    dt_ai_model_t *model = dt_ai_models_get_by_id(darktable.ai_registry, id);
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
      if(dt_ai_models_delete(darktable.ai_registry, model_id))
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
  dt_ai_model_card_t *card = dt_ai_models_get_card(darktable.ai_registry, model_id);

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

// show tooltip and hand cursor over the info column
static gboolean _on_query_tooltip(GtkWidget *widget,
                                  gint x, gint y,
                                  gboolean keyboard_mode,
                                  GtkTooltip *tooltip,
                                  gpointer user_data)
{
  (void)keyboard_mode;
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;
  GtkTreeView *tv = GTK_TREE_VIEW(widget);
  GtkTreeViewColumn *column = NULL;
  gint bx, by;
  gtk_tree_view_convert_widget_to_bin_window_coords(
    tv, x, y, &bx, &by);

  if(!gtk_tree_view_get_path_at_pos(
       tv, bx, by, NULL, &column, NULL, NULL))
    return FALSE;

  GdkWindow *win = gtk_tree_view_get_bin_window(tv);
  if(column == data->info_col)
  {
    GdkCursor *cursor = gdk_cursor_new_from_name(gdk_window_get_display(win), "pointer");
    gdk_window_set_cursor(win, cursor);
    g_object_unref(cursor);
    gtk_tooltip_set_text(tooltip, _("click for model details"));
    return TRUE;
  }

  gdk_window_set_cursor(win, NULL);
  return FALSE;
}

// click on the ℹ info column opens the model card
static gboolean _on_info_button_press(GtkWidget *widget,
                                      GdkEventButton *event,
                                      gpointer user_data)
{
  if(event->type != GDK_BUTTON_PRESS
     || event->button != 1)
    return FALSE;

  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;
  GtkTreeView *tv = GTK_TREE_VIEW(widget);
  GtkTreePath *path = NULL;
  GtkTreeViewColumn *column = NULL;

  if(!gtk_tree_view_get_path_at_pos(tv, (gint)event->x, (gint)event->y,
                                    &path, &column, NULL, NULL))
    return FALSE;

  // only react to clicks on the info column
  if(column != data->info_col)
  {
    gtk_tree_path_free(path);
    return FALSE;
  }

  GtkTreeIter iter;
  if(gtk_tree_model_get_iter(GTK_TREE_MODEL(data->model_store), &iter, path))
  {
    gchar *model_id = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(data->model_store),
                       &iter, COL_ID, &model_id, -1);
    if(model_id)
    {
      _show_model_card(data, model_id);
      g_free(model_id);
    }
  }
  gtk_tree_path_free(path);
  return TRUE;
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
    dt_ai_ort_found_t *f = found->data;
    gtk_entry_set_text(GTK_ENTRY(data->ort_path_entry), f->path);
    dt_conf_set_string("plugins/ai/ort_library_path", f->path);
    _update_string_indicator(data->ort_path_indicator, "plugins/ai/ort_library_path");
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
      dt_ai_ort_found_t *f = l->data;
      gchar *entry = g_strdup_printf("ORT %s [%s]  %s", f->version, f->eps, f->path);
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
        dt_ai_ort_found_t *f = g_list_nth_data(found, sel);
        gtk_entry_set_text(GTK_ENTRY(data->ort_path_entry), f->path);
        dt_conf_set_string("plugins/ai/ort_library_path", f->path);
        _update_string_indicator(data->ort_path_indicator, "plugins/ai/ort_library_path");
      }
    }
    gtk_widget_destroy(dlg);
  }

  g_list_free_full(found, (GDestroyNotify)dt_ai_ort_found_free);
}

static gboolean _reset_ort_path_click(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  if(e->type != GDK_2BUTTON_PRESS) return FALSE;
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;
  gtk_entry_set_text(GTK_ENTRY(data->ort_path_entry), "");
  dt_conf_set_string("plugins/ai/ort_library_path", "");
  _update_string_indicator(data->ort_path_indicator, "plugins/ai/ort_library_path");
  return TRUE;
}
static void _on_ort_path_changed(GtkEntry *entry, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;
  const char *text = gtk_entry_get_text(GTK_ENTRY(data->ort_path_entry));

  // empty = reset to bundled
  if(!text || !text[0])
  {
    dt_conf_set_string("plugins/ai/ort_library_path", "");
    _update_string_indicator(data->ort_path_indicator, "plugins/ai/ort_library_path");
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

  dt_conf_set_string("plugins/ai/ort_library_path", text);
  _update_string_indicator(data->ort_path_indicator, "plugins/ai/ort_library_path");
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
      dt_conf_set_string("plugins/ai/ort_library_path", filename);
      _update_string_indicator(data->ort_path_indicator, "plugins/ai/ort_library_path");
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
  gtk_widget_add_events(enable_labelev, GDK_BUTTON_PRESS_MASK);
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
  g_signal_connect(
    enable_labelev,
    "button-press-event",
    G_CALLBACK(_reset_enable_click),
    data->enable_toggle);
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
  GtkWidget *provider_label = gtk_label_new(_("execution provider"));
  gtk_widget_set_halign(provider_label, GTK_ALIGN_START);
  GtkWidget *provider_labelev = gtk_event_box_new();
  gtk_widget_add_events(provider_labelev, GDK_BUTTON_PRESS_MASK);
  gtk_container_add(GTK_CONTAINER(provider_labelev), provider_label);
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(provider_labelev), FALSE);

  data->provider_indicator = _create_indicator(DT_AI_CONF_PROVIDER);
  data->provider_combo = dt_bauhaus_combobox_new(NULL);

  // populate from central provider table, skipping unavailable providers
  GString *tooltip = g_string_new(_("select hardware acceleration for AI inference:"));
  for(int i = 0; i < DT_AI_PROVIDER_COUNT; i++)
  {
    if(!dt_ai_providers[i].available) continue;
    if(dt_ai_providers[i].value == DT_AI_PROVIDER_AUTO)
      dt_bauhaus_combobox_add(data->provider_combo, _("auto"));
    else
      dt_bauhaus_combobox_add(data->provider_combo, dt_ai_providers[i].display_name);
    g_string_append_printf(tooltip, "\n- %s", dt_ai_providers[i].display_name);
  }

  char *provider_str = dt_conf_get_string(DT_AI_CONF_PROVIDER);
  dt_ai_provider_t provider = dt_ai_provider_from_string(provider_str);
  g_free(provider_str);
  dt_bauhaus_combobox_set(data->provider_combo, _provider_to_combo_idx(provider));

  g_signal_connect(
    data->provider_combo,
    "value-changed",
    G_CALLBACK(_on_provider_changed),
    data);
  g_signal_connect(
    provider_labelev,
    "button-press-event",
    G_CALLBACK(_reset_provider_click),
    data->provider_combo);
  gtk_widget_set_tooltip_text(data->provider_combo, tooltip->str);
  g_string_free(tooltip, TRUE);
  data->provider_status = gtk_label_new(NULL);
  gtk_label_set_use_markup(GTK_LABEL(data->provider_status), TRUE);
  gtk_widget_set_halign(data->provider_status, GTK_ALIGN_START);

  // put combo + status in an hbox so the combo doesn't stretch
  // when column 2 expands for the ORT path entry below
  GtkWidget *provider_hbox = dt_gui_hbox(data->provider_combo, data->provider_status);

  gtk_grid_attach(GTK_GRID(settings_grid), provider_labelev, 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(settings_grid), data->provider_indicator, 1, row, 1, 1);
  gtk_grid_attach(GTK_GRID(settings_grid), provider_hbox, 2, row++, 3, 1);

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
    g_signal_connect(path_labelev, "button-press-event", G_CALLBACK(_reset_ort_path_click), data);
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
    G_TYPE_STRING,  // info icon
    G_TYPE_STRING,  // version
    G_TYPE_STRING,  // task
    G_TYPE_BOOLEAN, // enabled
    G_TYPE_BOOLEAN, // enabled_sensitive
    G_TYPE_STRING,  // status
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

  // info icon column — click opens model card
  GtkCellRenderer *info_renderer = gtk_cell_renderer_text_new();
  data->info_col = gtk_tree_view_column_new_with_attributes(
    "",
    info_renderer,
    "text",
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
  g_signal_connect(data->model_list, "button-press-event",
                   G_CALLBACK(_on_info_button_press), data);

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
  gtk_grid_attach(GTK_GRID(models_grid), button_box, 0, row++, 1, 1);

#ifdef HAVE_AI_DOWNLOAD
  // download selected button
  data->download_selected_btn = gtk_button_new_with_label(_("download selected"));
  gtk_widget_set_tooltip_text(data->download_selected_btn,
    _("download or update the selected models"));
  g_signal_connect(
    data->download_selected_btn,
    "clicked",
    G_CALLBACK(_on_download_selected),
    data);
  dt_gui_box_add(button_box, data->download_selected_btn);

  // download default button
  data->download_default_btn = gtk_button_new_with_label(_("download default"));
  gtk_widget_set_tooltip_text(data->download_default_btn,
    _("download or update all default models"));
  g_signal_connect(
    data->download_default_btn,
    "clicked",
    G_CALLBACK(_on_download_default),
    data);
  dt_gui_box_add(button_box, data->download_default_btn);

  // download all button
  data->download_all_btn = gtk_button_new_with_label(_("download all"));
  gtk_widget_set_tooltip_text(data->download_all_btn,
    _("download or update all available models"));
  g_signal_connect(data->download_all_btn, "clicked", G_CALLBACK(_on_download_all), data);
  dt_gui_box_add(button_box, data->download_all_btn);
#endif // HAVE_AI_DOWNLOAD

  // install model button
  data->install_btn = gtk_button_new_with_label(_("install model"));
  gtk_widget_set_tooltip_text(data->install_btn,
    _("install a model from a local .dtmodel file"));
  g_signal_connect(data->install_btn, "clicked", G_CALLBACK(_on_install_model), data);
  dt_gui_box_add(button_box, data->install_btn);

  // delete selected button
  data->delete_selected_btn = gtk_button_new_with_label(_("delete selected"));
  gtk_widget_set_tooltip_text(data->delete_selected_btn,
    _("remove the selected models from disk"));
  g_signal_connect(
    data->delete_selected_btn,
    "clicked",
    G_CALLBACK(_on_delete_selected),
    data);
  dt_gui_box_add(button_box, data->delete_selected_btn);


  dt_gui_box_add(data->controls_box, models_grid);

  // wrap in a scrolled container like other tabs
  GtkWidget *main_scroll = dt_gui_scroll_wrap(main_box);
  GtkWidget *tab_box = dt_gui_vbox(main_scroll);

  // add to stack
  gtk_stack_add_titled(GTK_STACK(stack), tab_box, "AI", _("AI"));

  // populate model list
  _refresh_model_list(data);

  // store data pointer for cleanup (attach to container)
  g_object_set_data_full(G_OBJECT(tab_box), "prefs-ai-data", data, g_free);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
