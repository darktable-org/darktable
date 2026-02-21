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
#include "common/ai_models.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "control/signal.h"
#include "gui/gtk.h"

#include <glib/gi18n.h>

// Non-default indicator (same as other preference tabs)
#define NON_DEF_CHAR "\xe2\x97\x8f"

// Update the non-default indicator dot for a boolean preference
static void _update_bool_indicator(GtkWidget *indicator, const char *confkey)
{
  const gboolean current = dt_conf_get_bool(confkey);
  const gboolean def = dt_confgen_get_bool(confkey, DT_DEFAULT);
  if(current == def)
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

// Update the non-default indicator dot for a string preference
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

// Create the indicator label widget
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

// Column indices for model list store
enum
{
  COL_SELECTED,
  COL_NAME,
  COL_TASK,
  COL_DESCRIPTION,
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
  GtkWidget *provider_combo;
  GtkWidget *provider_indicator;
  GtkWidget *provider_status;
  GtkWidget *model_list;
  GtkListStore *model_store;
  GtkWidget *download_selected_btn;
  GtkWidget *download_default_btn;
  GtkWidget *download_all_btn;
  GtkWidget *delete_selected_btn;
  GtkWidget *parent_dialog;
  GtkWidget *select_all_toggle;
} dt_prefs_ai_data_t;

// Download dialog data
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

// Sort by task, then default (yes before no), then name
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
    dt_print(DT_DEBUG_AI, "[preferences_ai] Registry is NULL");
    return;
  }

  gtk_list_store_clear(data->model_store);

  dt_ai_models_refresh_status(darktable.ai_registry);

  const int count = dt_ai_models_get_count(darktable.ai_registry);
  dt_print(DT_DEBUG_AI, "[preferences_ai] Refreshing model list, count=%d", count);
  for(int i = 0; i < count; i++)
  {
    dt_ai_model_t *model = dt_ai_models_get_by_index(darktable.ai_registry, i);
    if(!model)
    {
      dt_print(DT_DEBUG_AI, "[preferences_ai] Model at index %d is NULL", i);
      continue;
    }
    dt_print(
      DT_DEBUG_AI,
      "[preferences_ai] Adding model: %s",
      model->id ? model->id : "(null)");

    // Check if this model is the active one for its task
    const gboolean is_downloaded = (model->status == DT_AI_MODEL_DOWNLOADED);
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
      COL_DESCRIPTION,
      model->description ? model->description : "",
      COL_STATUS,
      _status_to_string(model->status),
      COL_DEFAULT,
      model->is_default ? _("yes") : _("no"),
      COL_ID,
      model->id,
      -1);
    dt_ai_model_free(model);
  }

  // Reset select-all toggle
  if(data->select_all_toggle)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->select_all_toggle), FALSE);
}

static void _on_enable_toggled(GtkWidget *widget, gpointer user_data)
{
  GtkWidget *indicator = GTK_WIDGET(user_data);
  const gboolean enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  dt_conf_set_bool("plugins/ai/enabled", enabled);
  if(darktable.ai_registry)
  {
    g_mutex_lock(&darktable.ai_registry->lock);
    darktable.ai_registry->ai_enabled = enabled;
    g_mutex_unlock(&darktable.ai_registry->lock);
  }
  _update_bool_indicator(indicator, "plugins/ai/enabled");
}

// Map combo box index to provider table index (skipping unavailable providers)
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

// Map provider enum value to combo box index
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

// Double-click on label resets the enable toggle to default
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

// Double-click on label resets the provider combo to default
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

static void _on_model_selection_toggled(
  GtkCellRendererToggle *cell,
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

  // Toggle the value
  gtk_list_store_set(data->model_store, &iter, COL_SELECTED, !selected, -1);
}

static void _on_enabled_toggled(
  GtkCellRendererToggle *cell,
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

  // Refresh to update all checkboxes (previous active model unchecks)
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
  // Only toggle if the click wasn't already handled by the checkbox itself.
  // Block the toggled signal to prevent double-fire, then toggle manually.
  g_signal_handlers_block_by_func(data->select_all_toggle, _on_select_all_toggled, data);
  gboolean active
    = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->select_all_toggle));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->select_all_toggle), !active);
  g_signal_handlers_unblock_by_func(
    data->select_all_toggle,
    _on_select_all_toggled,
    data);

  // Now manually apply the selection since we blocked the signal
  _on_select_all_toggled(GTK_TOGGLE_BUTTON(data->select_all_toggle), data);
}

// Collect selected model IDs from the list store
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

// Progress callback called from download thread
static void
_download_progress_callback(const char *model_id, double progress, gpointer user_data)
{
  dt_download_dialog_t *dl = (dt_download_dialog_t *)user_data;
  g_mutex_lock(&dl->mutex);
  dl->progress = progress;
  g_mutex_unlock(&dl->mutex);
}

// Idle callback to update progress bar from main thread
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
      // Show error in dialog
      if(dl->dialog && GTK_IS_WIDGET(dl->dialog))
      {
        gtk_label_set_text(GTK_LABEL(dl->status_label), error);
        gtk_widget_show(dl->status_label);
      }
      g_free(error);
    }
    else
    {
      // Success - close dialog
      if(dl->dialog && GTK_IS_WIDGET(dl->dialog))
        gtk_dialog_response(GTK_DIALOG(dl->dialog), GTK_RESPONSE_OK);
    }
    return G_SOURCE_REMOVE;
  }

  g_free(error);
  return G_SOURCE_CONTINUE;
}

// Download thread function
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

// Show modal download dialog for a single model
static gboolean
_download_model_with_dialog(dt_prefs_ai_data_t *data, const char *model_id)
{
  dt_ai_model_t *model = dt_ai_models_get_by_id(darktable.ai_registry, model_id);
  if(!model)
    return FALSE;

  // Create dialog
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

  // Model name label (use fields from copy, then free it)
  char *title
    = g_strdup_printf(_("Downloading: %s"), model->name ? model->name : model->id);
  dt_ai_model_free(model);
  GtkWidget *title_label = gtk_label_new(title);
  g_free(title);
  gtk_box_pack_start(GTK_BOX(content), title_label, FALSE, FALSE, 0);

  // Progress bar
  GtkWidget *progress_bar = gtk_progress_bar_new();
  gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(progress_bar), TRUE);
  gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), "0%");
  gtk_box_pack_start(GTK_BOX(content), progress_bar, FALSE, FALSE, 0);

  // Status label (for errors)
  GtkWidget *status_label = gtk_label_new("");
  gtk_widget_set_no_show_all(status_label, TRUE);
  gtk_box_pack_start(GTK_BOX(content), status_label, FALSE, FALSE, 0);

  gtk_widget_show_all(dialog);

  // Set up download data (heap-allocated for thread safety)
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

  // Start download thread
  GThread *thread = g_thread_new("ai-download", _download_thread_func, dl);

  // Start progress update timer
  guint timer_id = g_timeout_add(100, _update_progress_idle, dl);

  // Run dialog
  gint response = gtk_dialog_run(GTK_DIALOG(dialog));

  // Handle cancellation (atomic so curl progress callback can read it safely)
  if(response == GTK_RESPONSE_CANCEL)
    g_atomic_int_set(&dl->cancelled, TRUE);

  // Wait for thread to finish — after this, dl->finished is TRUE
  g_thread_join(thread);

  // Remove the timer. Any already-dispatched idle callback will see
  // dl->finished == TRUE and return G_SOURCE_REMOVE harmlessly.
  g_source_remove(timer_id);

  // Destroy the dialog before freeing dl so no straggling callback
  // can touch destroyed widgets.
  gtk_widget_destroy(dialog);
  dl->dialog = NULL;

  gboolean success = (dl->error == NULL);

  // Notify modules that models have changed
  if(success)
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_AI_MODELS_CHANGED);

  // Clean up
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
      gboolean need_download = (model->status == DT_AI_MODEL_NOT_DOWNLOADED);
      dt_ai_model_free(model);
      if(need_download && !_download_model_with_dialog(data, id))
        break; // Stop on error or cancel
    }
  }
  g_list_free_full(ids, g_free);
  _refresh_model_list(data);
}

static void _on_download_default(GtkButton *button, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;

  // Download default models that need downloading
  const int count = dt_ai_models_get_count(darktable.ai_registry);
  for(int i = 0; i < count; i++)
  {
    dt_ai_model_t *model = dt_ai_models_get_by_index(darktable.ai_registry, i);
    if(!model)
      continue;
    gboolean need_download
      = (model->is_default && model->status == DT_AI_MODEL_NOT_DOWNLOADED);
    char *id = need_download ? g_strdup(model->id) : NULL;
    dt_ai_model_free(model);
    if(need_download)
    {
      if(!_download_model_with_dialog(data, id))
      {
        g_free(id);
        break; // Stop on error or cancel
      }
      g_free(id);
    }
  }
  _refresh_model_list(data);
}

static void _on_download_all(GtkButton *button, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;

  // Download all models that need downloading
  const int count = dt_ai_models_get_count(darktable.ai_registry);
  for(int i = 0; i < count; i++)
  {
    dt_ai_model_t *model = dt_ai_models_get_by_index(darktable.ai_registry, i);
    if(!model)
      continue;
    gboolean need_download = (model->status == DT_AI_MODEL_NOT_DOWNLOADED);
    char *id = need_download ? g_strdup(model->id) : NULL;
    dt_ai_model_free(model);
    if(need_download)
    {
      if(!_download_model_with_dialog(data, id))
      {
        g_free(id);
        break; // Stop on error or cancel
      }
      g_free(id);
    }
  }
  _refresh_model_list(data);
}

static void _on_delete_selected(GtkButton *button, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;

  // Collect selected models that are downloaded
  GList *ids = _get_selected_model_ids(data);
  GList *to_delete = NULL;
  int delete_count = 0;

  for(GList *l = ids; l; l = g_list_next(l))
  {
    const char *id = (const char *)l->data;
    dt_ai_model_t *model = dt_ai_models_get_by_id(darktable.ai_registry, id);
    if(model)
    {
      if(model->status == DT_AI_MODEL_DOWNLOADED)
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

  // Confirm deletion
  GtkWidget *confirm = gtk_message_dialog_new(
    GTK_WINDOW(data->parent_dialog),
    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
    GTK_MESSAGE_QUESTION,
    GTK_BUTTONS_YES_NO,
    ngettext("delete %d selected model?", "delete %d selected models?", delete_count),
    delete_count);

  gint response = gtk_dialog_run(GTK_DIALOG(confirm));
  gtk_widget_destroy(confirm);

  if(response == GTK_RESPONSE_YES)
  {
    gboolean any_deleted = FALSE;
    for(GList *l = to_delete; l; l = g_list_next(l))
    {
      const char *model_id = (const char *)l->data;
      if(dt_ai_models_delete(darktable.ai_registry, model_id))
      {
        dt_print(DT_DEBUG_AI, "[preferences_ai] Deleted model: %s", model_id);
        any_deleted = TRUE;
      }
    }

    if(any_deleted)
      DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_AI_MODELS_CHANGED);

    _refresh_model_list(data);
  }

  g_list_free_full(to_delete, g_free);
}

static void _on_refresh(GtkButton *button, gpointer user_data)
{
  dt_prefs_ai_data_t *data = (dt_prefs_ai_data_t *)user_data;
  _refresh_model_list(data);
}

void init_tab_ai(GtkWidget *dialog, GtkWidget *stack)
{
  dt_prefs_ai_data_t *data = g_new0(dt_prefs_ai_data_t, 1);
  data->parent_dialog = dialog;

  // Main vertical box holds two independent sections
  GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  // === "general" section with its own grid ===
  GtkWidget *general_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(general_grid), DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(GTK_GRID(general_grid), DT_PIXEL_APPLY_DPI(5));

  int row = 0;

  // --- "general" section header ---
  {
    GtkWidget *seclabel = gtk_label_new(_("general"));
    GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(lbox), seclabel, FALSE, FALSE, 0);
    gtk_widget_set_name(lbox, "pref_section");
    gtk_grid_attach(GTK_GRID(general_grid), lbox, 0, row++, 3, 1);
  }

  // Enable AI toggle
  GtkWidget *enable_label = gtk_label_new(_("enable AI features"));
  gtk_widget_set_halign(enable_label, GTK_ALIGN_START);
  GtkWidget *enable_labelev = gtk_event_box_new();
  gtk_widget_add_events(enable_labelev, GDK_BUTTON_PRESS_MASK);
  gtk_container_add(GTK_CONTAINER(enable_labelev), enable_label);
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(enable_labelev), FALSE);

  GtkWidget *enable_indicator = _create_indicator("plugins/ai/enabled");
  data->enable_toggle = gtk_check_button_new();
  gtk_toggle_button_set_active(
    GTK_TOGGLE_BUTTON(data->enable_toggle),
    dt_conf_get_bool("plugins/ai/enabled"));
  g_signal_connect(
    data->enable_toggle,
    "toggled",
    G_CALLBACK(_on_enable_toggled),
    enable_indicator);
  g_signal_connect(
    enable_labelev,
    "button-press-event",
    G_CALLBACK(_reset_enable_click),
    data->enable_toggle);
  gtk_grid_attach(GTK_GRID(general_grid), enable_labelev, 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(general_grid), enable_indicator, 1, row, 1, 1);
  gtk_grid_attach(GTK_GRID(general_grid), data->enable_toggle, 2, row++, 1, 1);

  // Provider dropdown
  GtkWidget *provider_label = gtk_label_new(_("execution provider"));
  gtk_widget_set_halign(provider_label, GTK_ALIGN_START);
  GtkWidget *provider_labelev = gtk_event_box_new();
  gtk_widget_add_events(provider_labelev, GDK_BUTTON_PRESS_MASK);
  gtk_container_add(GTK_CONTAINER(provider_labelev), provider_label);
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(provider_labelev), FALSE);

  data->provider_indicator = _create_indicator(DT_AI_CONF_PROVIDER);
  data->provider_combo = dt_bauhaus_combobox_new(NULL);

  // Populate from central provider table, skipping unavailable providers
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

  gtk_grid_attach(GTK_GRID(general_grid), provider_labelev, 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(general_grid), data->provider_indicator, 1, row, 1, 1);
  gtk_grid_attach(GTK_GRID(general_grid), data->provider_combo, 2, row, 1, 1);
  gtk_grid_attach(GTK_GRID(general_grid), data->provider_status, 3, row++, 1, 1);

  gtk_box_pack_start(GTK_BOX(main_box), general_grid, FALSE, FALSE, 0);

  // === "models" section with its own grid ===
  GtkWidget *models_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(models_grid), DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(GTK_GRID(models_grid), DT_PIXEL_APPLY_DPI(5));

  row = 0;

  // --- "models" section header ---
  {
    GtkWidget *seclabel = gtk_label_new(_("models"));
    GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(lbox), seclabel, FALSE, FALSE, 0);
    gtk_widget_set_name(lbox, "pref_section");
    gtk_grid_attach(GTK_GRID(models_grid), lbox, 0, row++, 1, 1);
  }

  // Create model list store
  data->model_store = gtk_list_store_new(
    NUM_COLS,
    G_TYPE_BOOLEAN, // selected
    G_TYPE_STRING,  // name
    G_TYPE_STRING,  // task
    G_TYPE_STRING,  // description
    G_TYPE_BOOLEAN, // enabled
    G_TYPE_BOOLEAN, // enabled_sensitive
    G_TYPE_STRING,  // status
    G_TYPE_STRING,  // default
    G_TYPE_STRING); // id

  // Sort by task, then default, then name
  gtk_tree_sortable_set_default_sort_func(
    GTK_TREE_SORTABLE(data->model_store), _model_sort_func, NULL, NULL);
  gtk_tree_sortable_set_sort_column_id(
    GTK_TREE_SORTABLE(data->model_store),
    GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID, GTK_SORT_ASCENDING);

  // Create tree view
  data->model_list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(data->model_store));
  g_object_unref(data->model_store); // Tree view takes ownership

  // Selection checkbox column (no title, with select-all checkbox in header)
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

  // Add select-all checkbox as column header widget
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

  // Connect to the header button's clicked signal so the checkbox toggles
  // when clicking anywhere in the header area
  GtkWidget *select_col_button = gtk_tree_view_column_get_button(select_col);
  g_signal_connect(
    select_col_button,
    "clicked",
    G_CALLBACK(_on_select_all_header_clicked),
    data);

  // Name column
  GtkCellRenderer *text_renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *name_col = gtk_tree_view_column_new_with_attributes(
    _("name"),
    text_renderer,
    "text",
    COL_NAME,
    NULL);
  gtk_tree_view_column_set_expand(name_col, FALSE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(data->model_list), name_col);

  // Task column
  GtkTreeViewColumn *task_col = gtk_tree_view_column_new_with_attributes(
    _("task"),
    text_renderer,
    "text",
    COL_TASK,
    NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(data->model_list), task_col);

  // Description column
  GtkTreeViewColumn *desc_col = gtk_tree_view_column_new_with_attributes(
    _("description"),
    text_renderer,
    "text",
    COL_DESCRIPTION,
    NULL);
  gtk_tree_view_column_set_expand(desc_col, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(data->model_list), desc_col);

  // Enabled checkbox column (radio-button behavior per task)
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

  // Status column
  GtkTreeViewColumn *status_col = gtk_tree_view_column_new_with_attributes(
    _("status"),
    text_renderer,
    "text",
    COL_STATUS,
    NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(data->model_list), status_col);

  // Default column
  GtkTreeViewColumn *default_col = gtk_tree_view_column_new_with_attributes(
    _("default"),
    text_renderer,
    "text",
    COL_DEFAULT,
    NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(data->model_list), default_col);

  // Scrolled window for the list
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

  // Button box
  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(5));
  gtk_grid_attach(GTK_GRID(models_grid), button_box, 0, row++, 1, 1);

  // Download selected button
  data->download_selected_btn = gtk_button_new_with_label(_("download selected"));
  g_signal_connect(
    data->download_selected_btn,
    "clicked",
    G_CALLBACK(_on_download_selected),
    data);
  gtk_box_pack_start(GTK_BOX(button_box), data->download_selected_btn, FALSE, FALSE, 0);

  // Download default button
  data->download_default_btn = gtk_button_new_with_label(_("download default"));
  g_signal_connect(
    data->download_default_btn,
    "clicked",
    G_CALLBACK(_on_download_default),
    data);
  gtk_box_pack_start(GTK_BOX(button_box), data->download_default_btn, FALSE, FALSE, 0);

  // Download all button
  data->download_all_btn = gtk_button_new_with_label(_("download all"));
  g_signal_connect(data->download_all_btn, "clicked", G_CALLBACK(_on_download_all), data);
  gtk_box_pack_start(GTK_BOX(button_box), data->download_all_btn, FALSE, FALSE, 0);

  // Delete selected button
  data->delete_selected_btn = gtk_button_new_with_label(_("delete selected"));
  g_signal_connect(
    data->delete_selected_btn,
    "clicked",
    G_CALLBACK(_on_delete_selected),
    data);
  gtk_box_pack_start(GTK_BOX(button_box), data->delete_selected_btn, FALSE, FALSE, 0);

  // Refresh button
  GtkWidget *refresh_btn = gtk_button_new_with_label(_("refresh"));
  g_signal_connect(refresh_btn, "clicked", G_CALLBACK(_on_refresh), data);
  gtk_box_pack_end(GTK_BOX(button_box), refresh_btn, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(main_box), models_grid, TRUE, TRUE, 0);

  // Wrap in a scrolled container like other tabs
  GtkWidget *tab_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *main_scroll = dt_gui_scroll_wrap(main_box);
  gtk_box_pack_start(GTK_BOX(tab_box), main_scroll, TRUE, TRUE, 0);

  // Add to stack
  gtk_stack_add_titled(GTK_STACK(stack), tab_box, "AI", _("AI"));

  // Populate model list
  _refresh_model_list(data);

  // Store data pointer for cleanup (attach to container)
  g_object_set_data_full(G_OBJECT(tab_box), "prefs-ai-data", data, g_free);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
