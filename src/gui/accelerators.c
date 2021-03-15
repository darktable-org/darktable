/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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

#include "gui/accelerators.h"
#include "common/action.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/utility.h"
#include "control/control.h"
#include "develop/blend.h"
#include "gui/presets.h"

#include "bauhaus/bauhaus.h"

#include <assert.h>
#include <gtk/gtk.h>

typedef struct dt_shortcut_t
{
  dt_input_device_t key_device;
  guint key;
  guint mods;
  guint button;
  enum
  {
    DT_SHORTCUT_CLICK_NONE,
    DT_SHORTCUT_CLICK_SINGLE,
    DT_SHORTCUT_CLICK_DOUBLE,
    DT_SHORTCUT_CLICK_TRIPLE,
    DT_SHORTCUT_CLICK_LONG = 4
  } click;
  dt_input_device_t move_device;
  guint move;
  enum
  {
    DT_SHORTCUT_DIR_NONE,
    DT_SHORTCUT_DIR_UP,
    DT_SHORTCUT_DIR_DOWN,
  } direction;
  dt_view_type_flags_t views;

  dt_action_t *action;
  int instance; // 0 is from prefs, >0 counting from first, <0 counting from last
  float speed;

  enum // these will be defined in widget definition block as strings and here indexed
  {
    DT_SHORTCUT_ELEMENT_MIN,
    DT_SHORTCUT_ELEMENT_MAX,
    DT_SHORTCUT_ELEMENT_MINEST,
    DT_SHORTCUT_ELEMENT_MAXEST,
    DT_SHORTCUT_ELEMENT_NODE1, // contrast equaliser for example. Node value or node x-axis position can be moved
    DT_SHORTCUT_ELEMENT_NODE2,
    DT_SHORTCUT_ELEMENT_NODE3,
    DT_SHORTCUT_ELEMENT_NODE4,
    DT_SHORTCUT_ELEMENT_NODE5,
    DT_SHORTCUT_ELEMENT_NODE6,
    DT_SHORTCUT_ELEMENT_NODE7,
    DT_SHORTCUT_ELEMENT_NODE8,
  } element; // this should be index into widget discription structure.
  enum // these will be defined in type definition block as strings and here indexed
  {
    DT_SHORTCUT_EFFECT_CLOSURE,
    DT_SHORTCUT_EFFECT_UP,
    DT_SHORTCUT_EFFECT_DOWN,
    DT_SHORTCUT_EFFECT_NEXT,
    DT_SHORTCUT_EFFECT_PREVIOUS,
    DT_SHORTCUT_EFFECT_VALUE,
    DT_SHORTCUT_EFFECT_RESET,
    DT_SHORTCUT_EFFECT_END,
    DT_SHORTCUT_EFFECT_BEGIN,
  } effect;
} dt_shortcut_t;

static dt_shortcut_t bsc = { 0 };  // building shortcut

typedef enum dt_shortcut_move_t
{
  DT_SHORTCUT_MOVE_NONE,
  DT_SHORTCUT_MOVE_SCROLL,
  DT_SHORTCUT_MOVE_HORIZONTAL,
  DT_SHORTCUT_MOVE_VERTICAL,
  DT_SHORTCUT_MOVE_DIAGONAL,
  DT_SHORTCUT_MOVE_SKEW,
  DT_SHORTCUT_MOVE_LEFTRIGHT,
  DT_SHORTCUT_MOVE_UPDOWN,
  DT_SHORTCUT_MOVE_PGUPDOWN,
} dt_shortcut_move_t;

typedef struct dt_device_key_t
{
  dt_input_device_t key_device;
  guint key;
} dt_device_key_t;

typedef struct dt_action_widget_t
{
  dt_action_t *action;
  GtkWidget *widget;
} dt_action_widget_t;

#define DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE 0

const char *move_string[] = { "", N_("scroll"), N_("horizontal"), N_("vertical"), N_("diagonal"), N_("skew"),
                                  N_("leftright"), N_("updown"), N_("pgupdown"), NULL };
const char *click_string[] = { "", N_("single"), N_("double"), N_("triple"), NULL };

const struct _modifier_name
{
  GdkModifierType modifier;
  char           *name;
} modifier_string[]
  = { { GDK_SHIFT_MASK  , N_("shift") },
      { GDK_CONTROL_MASK, N_("ctrl" ) },
      { GDK_MOD1_MASK   , N_("alt"  ) },
      { GDK_MOD2_MASK   , N_("cmd"  ) },
      { GDK_SUPER_MASK  , N_("super") },
      { GDK_HYPER_MASK  , N_("hyper") },
      { GDK_META_MASK   , N_("meta" ) },
      { 0, NULL } };

gint shortcut_compare_func(gconstpointer shortcut_a, gconstpointer shortcut_b, gpointer user_data)
{
  const dt_shortcut_t *a = (const dt_shortcut_t *)shortcut_a;
  const dt_shortcut_t *b = (const dt_shortcut_t *)shortcut_b;

  dt_view_type_flags_t active_view = GPOINTER_TO_INT(user_data);
  int a_in_view = a->views ? a->views & active_view : -1; // put fallbacks last
  int b_in_view = b->views ? b->views & active_view : -1; // put fallbacks last

// FIXME if no views then this is fallback; sort by action first (after putting all fallbacks last)

  if(a_in_view != b_in_view)
    return b_in_view - a_in_view; // reverse order; in current view first
  if(a->key_device != b->key_device)
    return a->key_device - b->key_device;
  if(a->key != b->key)
    return a->key - b->key;
  if(a->button != b->button)
    return a->button - b->button;
  if(a->click != b->click)
    return a->click - b->click;
  if(a->move_device != b->move_device)
    return a->move_device - b->move_device;
  if(a->move != b->move)
    return a->move - b->move;
  if(a->mods != b->mods)
    return a->mods - b->mods;

  return 0;
};

static gchar *_action_full_label(dt_action_t *action)
{
  if(action->owner)
  {
    gchar *owner_label = _action_full_label(action->owner);
    gchar *full_label = g_strdup_printf("%s/%s", owner_label, action->label);
    g_free(owner_label);
    return full_label;
  }
  else
    return g_strdup(action->label);
}

static gchar *_action_full_label_translated(dt_action_t *action)
{
  if(action->owner)
  {
    gchar *owner_label = _action_full_label_translated(action->owner);
    gchar *full_label = g_strdup_printf("%s/%s", owner_label, action->label_translated);
    g_free(owner_label);
    return full_label;
  }
  else
    return g_strdup(action->label_translated);
}

static void _dump_actions(FILE *f, dt_action_t *action)
{
  while(action)
  {
    gchar *label = _action_full_label(action);
    fprintf(f, "%s %s\n", label, !action->target ? "*" : "");
    g_free(label);
    if(action->type <= DT_ACTION_TYPE_SECTION)
      _dump_actions(f, action->target);
    action = action->next;
  }
}

dt_input_device_t dt_register_input_driver(dt_lib_module_t *module, const dt_input_driver_definition_t *callbacks)
{
  dt_input_device_t id = 10;

  GSList *driver = darktable.control->input_drivers;
  while(driver)
  {
    if(((dt_input_driver_definition_t *)driver->data)->module == module) return id;
    driver = driver->next;
    id += 10;
  }

  dt_input_driver_definition_t *new_driver = calloc(1, sizeof(dt_input_driver_definition_t));
  *new_driver = *callbacks;
  new_driver->module = module;
  darktable.control->input_drivers = g_slist_append(darktable.control->input_drivers, (gpointer)new_driver);

  return id;
}

#define DT_MOVE_NAME -1
static gchar *_shortcut_key_move_name(dt_input_device_t id, guint key_or_move, guint mods, gboolean display)
{
  gchar *name = NULL, *post_name = NULL;
  if(id == DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE)
  {
    if(mods == DT_MOVE_NAME)
      return g_strdup(display && key_or_move != 0 ? _(move_string[key_or_move]) : move_string[key_or_move]);
    else
    {
      if(display)
      {
        gchar *key_name = gtk_accelerator_get_label(key_or_move, 0);
        post_name = g_utf8_strdown(key_name, -1);
        g_free(key_name);
      }
      else
        name = key_or_move ? gtk_accelerator_name(key_or_move, 0) : g_strdup("None");
    }
  }
  else
  {
    GSList *driver = darktable.control->input_drivers;
    while(driver)
    {
      if((id -= 10) < 10)
      {
        dt_input_driver_definition_t *callbacks = driver->data;
        gchar *without_device
          = mods == DT_MOVE_NAME
          ? callbacks->move_to_string(key_or_move, display)
          : callbacks->key_to_string(key_or_move, display);

        if(display)
          post_name = without_device;
        else
        {
          char id_str[2] = "\0\0";
          if(id) id_str[0] = '0' + id;

          name = g_strdup_printf("%s%s:%s", callbacks->name, id_str, without_device);
          g_free(without_device);
        }
        break;
      }
      driver = driver->next;
    }

    if(!driver) name = g_strdup(_("Unknown driver"));
  }
  if(mods != DT_MOVE_NAME)
  {
    for(const struct _modifier_name *mod_str = modifier_string;
        mod_str->modifier;
        mod_str++)
    {
      if(mods & mod_str->modifier)
      {
        gchar *save_name = name;
        name = display
             ? g_strdup_printf("%s%s+", name ? name : "", _(mod_str->name))
             : g_strdup_printf("%s;%s", name ? name : "",   mod_str->name);
        g_free(save_name);
      }
    }
  }

  if(post_name)
  {
    gchar *save_name = name;
    name = g_strdup_printf("%s%s", name ? name : "", post_name);
    g_free(save_name);
    g_free(post_name);
  }

  return name;
}

static gchar *_shortcut_description(dt_shortcut_t *s, gboolean full)
{
  static gchar hint[1024];
  int length = 0;

#define add_hint(format, ...) length += length >= sizeof(hint) ? 0 : snprintf(hint + length, sizeof(hint) - length, format, ##__VA_ARGS__)

  gchar *key_name = _shortcut_key_move_name(s->key_device, s->key, s->mods, TRUE);
  gchar *move_name = _shortcut_key_move_name(s->move_device, s->move, DT_MOVE_NAME, TRUE);

  add_hint("%s%s", key_name, s->key_device || s->key ? "" : move_name);

  if(s->button) add_hint(",");
  if(s->button & (1 << GDK_BUTTON_PRIMARY  )) add_hint(" %s", _("left"));
  if(s->button & (1 << GDK_BUTTON_SECONDARY)) add_hint(" %s", _("right"));
  if(s->button & (1 << GDK_BUTTON_MIDDLE   )) add_hint(" %s", _("middle"));

  guint clean_click = s->click & ~DT_SHORTCUT_CLICK_LONG;
  if(clean_click > DT_SHORTCUT_CLICK_SINGLE) add_hint(" %s", _(click_string[clean_click]));
  if(s->click >= DT_SHORTCUT_CLICK_LONG) add_hint(" %s", _("long"));
  if(s->button)
    add_hint(" %s", _("click"));
  else if(s->click > DT_SHORTCUT_CLICK_SINGLE)
    add_hint(" %s", _("press"));

  if(*move_name && (s->key_device || s->key))
  {
    add_hint(", %s", move_name);
  }
  g_free(key_name);
  g_free(move_name);

  if(full)
  {
    if(s->instance == 1) add_hint(", %s", _("first instance"));
    else
    if(s->instance == -1) add_hint(", %s", _("last instance"));
    else
    if(s->instance != 0) add_hint(", %s %+d", _("relative instance"), s->instance);

    if(s->speed != 1.0) add_hint(_(", %s *%g"), _("speed"), s->speed);
  }

#undef add_hint

  return hint;
}

static gboolean _shortcut_tooltip_callback(GtkWidget *widget, gint x, gint y, gboolean keyboard_mode,
                                           GtkTooltip *tooltip, gpointer user_data)
{
  gchar *description = NULL;
  dt_action_t *action = NULL;

  if(GTK_IS_TREE_VIEW(widget))
  {
    GtkTreePath *path = NULL;
    GtkTreeModel *model;
    GtkTreeIter iter;
    if(!gtk_tree_view_get_tooltip_context(GTK_TREE_VIEW(widget), &x, &y, keyboard_mode, &model, &path, &iter))
      return FALSE;

    gtk_tree_model_get(model, &iter, 0, &action, -1);
    gtk_tree_view_set_tooltip_row(GTK_TREE_VIEW(widget), tooltip, path);
    gtk_tree_path_free(path);
  }
  else
    action = g_hash_table_lookup(darktable.control->widgets, widget);

  for(GSequenceIter *iter = g_sequence_get_begin_iter(darktable.control->shortcuts);
      !g_sequence_iter_is_end(iter);
      iter = g_sequence_iter_next(iter))
  {
    dt_shortcut_t *s = g_sequence_get(iter);
    if(s->action == action)
    {
      gchar *old_description = description;
      description = g_strdup_printf("%s\n%s", description ? description : "", _shortcut_description(s, TRUE));
      g_free(old_description);
    }
  }

  if(description)
  {
    gchar *original_markup = gtk_widget_get_tooltip_markup(widget);
    gchar *desc_escaped = g_markup_escape_text(description, -1);
    gchar *markup_text = g_strdup_printf("%s<span style='italic' foreground='red'>%s</span>",
                                         original_markup ? original_markup : "Shortcuts:", desc_escaped);
    gtk_tooltip_set_markup(tooltip, markup_text);
    g_free(original_markup);
    g_free(desc_escaped);
    g_free(markup_text);
    g_free(description);

    return TRUE;
  }

  return FALSE;
}

void find_views(dt_shortcut_t *s)
{
  s->views = 0;

  dt_action_t *owner = s->action->owner;
  while(owner && owner->type == DT_ACTION_TYPE_SECTION) owner = owner->owner;
  if(owner)
  switch(owner->type)
  {
  case DT_ACTION_TYPE_IOP:
    s->views = DT_VIEW_DARKROOM;
    break;
  case DT_ACTION_TYPE_VIEW:
    {
      dt_view_t *view = (dt_view_t *)owner;

      s->views = view->view(view);
    }
    break;
  case DT_ACTION_TYPE_LIB:
    {
      dt_lib_module_t *lib = (dt_lib_module_t *)owner;

      const gchar **views = lib->views(lib);
      while (*views)
      {
        if     (strcmp(*views, "lighttable") == 0)
          s->views |= DT_VIEW_LIGHTTABLE;
        else if(strcmp(*views, "darkroom") == 0)
          s->views |= DT_VIEW_DARKROOM;
        else if(strcmp(*views, "print") == 0)
          s->views |= DT_VIEW_PRINT;
        else if(strcmp(*views, "slideshow") == 0)
          s->views |= DT_VIEW_SLIDESHOW;
        else if(strcmp(*views, "map") == 0)
          s->views |= DT_VIEW_MAP;
        else if(strcmp(*views, "tethering") == 0)
          s->views |= DT_VIEW_TETHERING;
        else if(strcmp(*views, "*") == 0)
          s->views |= DT_VIEW_DARKROOM | DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING |
                      DT_VIEW_MAP | DT_VIEW_PRINT | DT_VIEW_SLIDESHOW;
        views++;
      }
    }
    break;
  case DT_ACTION_TYPE_CATEGORY:
    if(owner == &darktable.control->actions_blend)
      s->views = DT_VIEW_DARKROOM;
    else if(owner == &darktable.control->actions_lua)
      s->views = DT_VIEW_DARKROOM | DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING |
                 DT_VIEW_MAP | DT_VIEW_PRINT | DT_VIEW_SLIDESHOW;
    else if(owner == &darktable.control->actions_thumb)
    {
      s->views = DT_VIEW_DARKROOM | DT_VIEW_MAP | DT_VIEW_TETHERING | DT_VIEW_PRINT;
      if(!strstr(s->action->label,"history"))
        s->views |= DT_VIEW_LIGHTTABLE; // lighttable has copy/paste history shortcuts in separate lib
    }
    else
      fprintf(stderr, "[find_views] views for category '%s' unknown\n", owner->label);
    break;
  case DT_ACTION_TYPE_GLOBAL:
    s->views = DT_VIEW_DARKROOM | DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING |
               DT_VIEW_MAP | DT_VIEW_PRINT | DT_VIEW_SLIDESHOW;
    break;
  default:
    break;
  }
}

static GtkTreeStore *shortcuts_store = NULL;
static GtkTreeStore *actions_store = NULL;
static GtkWidget *grab_widget = NULL;

#define NUM_CATEGORIES 3
const gchar *category_label[NUM_CATEGORIES]
  = { N_("active view"),
      N_("other views"),
      N_("fallbacks (not implemented)") };

static void shortcuts_store_category(GtkTreeIter *category, dt_shortcut_t *s, dt_view_type_flags_t view)
{
  gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(shortcuts_store), category, NULL,
                                s && s->views ? s->views & view ? 0 : 1 : 2);
}

gboolean remove_from_store(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  gpointer iter_data;
  gtk_tree_model_get(model, iter, 0, &iter_data, -1);
  if(iter_data == data)
  {
    gtk_tree_store_remove(GTK_TREE_STORE(model), iter);
    return TRUE;
  }

  return FALSE;
}

static void remove_shortcut(GSequenceIter *shortcut)
{
  if(shortcuts_store)
    gtk_tree_model_foreach(GTK_TREE_MODEL(shortcuts_store), remove_from_store, shortcut);
  g_sequence_remove(shortcut);
}

static void add_shortcut(dt_shortcut_t *shortcut, dt_view_type_flags_t view)
{
  GSequenceIter *new_shortcut = g_sequence_insert_sorted(darktable.control->shortcuts, shortcut,
                                                         shortcut_compare_func, GINT_TO_POINTER(view));

  GtkTreeModel *model = GTK_TREE_MODEL(shortcuts_store);
  if(model)
  {
    GSequenceIter *prev_shortcut = g_sequence_iter_prev(new_shortcut);
    GSequenceIter *seq_iter = NULL;
    GtkTreeIter category, child;
    shortcuts_store_category(&category, shortcut, view);

    gint position = 1, found = 0;
    if(gtk_tree_model_iter_children(model, &child, &category))
    do
    {
      gtk_tree_model_get(model, &child, 0, &seq_iter, -1);
      if(seq_iter == prev_shortcut)
      {
        found = position;
        break;
      }
      position++;
    } while(gtk_tree_model_iter_next(model, &child));

    gtk_tree_store_insert_with_values(shortcuts_store, NULL, &category, found, 0, new_shortcut, -1);
  }

  if(shortcut->action && shortcut->action->type == DT_ACTION_TYPE_KEY_PRESSED && shortcut->action->target)
  {
    GtkAccelKey *key = shortcut->action->target;
    key->accel_key = shortcut->key;
    key->accel_mods = shortcut->mods;
  }
}

static void _shortcut_row_inserted(GtkTreeModel *tree_model, GtkTreePath *path, GtkTreeIter *iter, gpointer user_data)
{
  gtk_tree_view_expand_to_path(GTK_TREE_VIEW(user_data), path);
  gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(user_data), path, NULL, TRUE, 0.5, 0);
  gtk_tree_view_set_cursor(GTK_TREE_VIEW(user_data), path, NULL, FALSE);
}

static gboolean insert_shortcut(dt_shortcut_t *shortcut, gboolean confirm)
{
  if(shortcut->action && shortcut->action && shortcut->action->type == DT_ACTION_TYPE_KEY_PRESSED &&
     (shortcut->key_device != DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE ||
      shortcut->move_device != DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE || shortcut->button != 0 ||
      shortcut->click != DT_SHORTCUT_CLICK_SINGLE || shortcut->move != DT_SHORTCUT_MOVE_NONE))
  {
    fprintf(stderr, "[insert_shortcut] only key+mods type shortcut supported for key_pressed style accelerators\n");
    dt_control_log(_("only key + ctrl/shift/alt supported for this shortcut"));
    return FALSE;
  }
  // FIXME: prevent multiple shortcuts because only the last one will work.
  // better solution; incorporate these special case accelerators into standard shortcut framework

  dt_shortcut_t *s = calloc(sizeof(dt_shortcut_t), 1);
  *s = *shortcut;
  find_views(s);
  dt_view_type_flags_t real_views = s->views;

  const dt_view_t *vw = NULL;
  if(darktable.view_manager) vw = dt_view_manager_get_current_view(darktable.view_manager);
  dt_view_type_flags_t view = vw && vw->view ? vw->view(vw) : DT_VIEW_LIGHTTABLE;

  // check (and remove if confirmed) clashes in current and other views
  gboolean remove_existing = !confirm;
  do
  {
    gchar *existing_labels = NULL;
    int active_view = 1;
    do
    {
      GSequenceIter *existing = g_sequence_lookup(darktable.control->shortcuts, s, shortcut_compare_func, GINT_TO_POINTER(view));
      if(existing) // at least one found
      {
        // go to first one that has same shortcut
        while(!g_sequence_iter_is_begin(existing) &&
              !shortcut_compare_func(s, g_sequence_get(g_sequence_iter_prev(existing)), GINT_TO_POINTER(view)))
          existing = g_sequence_iter_prev(existing);

        do
        {
          GSequenceIter *saved_next = g_sequence_iter_next(existing);

          dt_shortcut_t *e = g_sequence_get(existing);

          if(e->action == s->action /* FIXME && e->sub == s->sub */)
          {
            // there should be no other clashes because same mapping already existed
            g_free(s);
            gchar *question = g_markup_printf_escaped("\n%s\n", _("remove the shortcut?"));
            if(confirm &&
               dt_gui_show_standalone_yes_no_dialog(_("shortcut already exists"), question, _("no"), _("yes")))
            {
              remove_shortcut(existing);
            }
            g_free(question);
            return FALSE;
          }

          if(e->views & real_views) // overlap
          {
            if(remove_existing)
              remove_shortcut(existing);
            else
            {
              gchar *old_labels = existing_labels;
              gchar *new_label = _action_full_label_translated(e->action);
              existing_labels = g_strdup_printf("%s\n%s",
                                                existing_labels ? existing_labels : "",
                                                new_label);
              g_free(new_label);
              g_free(old_labels);
            }
          }

          existing = saved_next;
        } while(!g_sequence_iter_is_end(existing) && !shortcut_compare_func(s, g_sequence_get(existing), GINT_TO_POINTER(view)));
      }

      s->views ^= view; // look in the opposite selection
    } while(active_view--);

    if(existing_labels)
    {
      gchar *question = g_markup_printf_escaped("\n%s\n<i>%s</i>\n",
                                                _("remove these existing shortcuts?"),
                                                existing_labels);
      remove_existing = dt_gui_show_standalone_yes_no_dialog(_("clashing shortcuts exist"),
                                                             question, _("no"), _("yes"));

      g_free(existing_labels);
      g_free(question);

      if(!remove_existing)
      {
        g_free(s);
        return FALSE;
      }
    }
    else
    {
      remove_existing = FALSE;
    }

  } while(remove_existing);

  add_shortcut(s, view);

  return TRUE;
}

typedef enum
{
  SHORTCUT_VIEW_DESCRIPTION,
  SHORTCUT_VIEW_ACTION,
  SHORTCUT_VIEW_ELEMENT,
  SHORTCUT_VIEW_SPEED,
  SHORTCUT_VIEW_INSTANCE,
  SHORTCUT_VIEW_COLUMNS
} field_id;

#define NUM_INSTANCES 5 // or 3, but change char relative[] = "-2" to "-1"
const gchar *instance_label[/*NUM_INSTANCES*/]
  = { N_("preferred"),
      N_("first"),
      N_("last"),
      N_("second"),
      N_("last but one") };

static void _fill_tree_fields(GtkTreeViewColumn *column, GtkCellRenderer *cell, GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
  void *data_ptr = NULL;
  gtk_tree_model_get(model, iter, 0, &data_ptr, -1);
  field_id field = GPOINTER_TO_INT(data);
  gchar *field_text = NULL;
  gboolean editable = FALSE;
  if(GPOINTER_TO_UINT(data_ptr) < NUM_CATEGORIES)
  {
    if(field == SHORTCUT_VIEW_DESCRIPTION)
    {
      field_text = g_strdup(_(category_label[GPOINTER_TO_INT(data_ptr)]));
    }
    else
    {
      field_text = g_strdup("");
    }
  }
  else
  {
    dt_shortcut_t *s = g_sequence_get(data_ptr);
    switch(field)
    {
    case SHORTCUT_VIEW_DESCRIPTION:
      field_text = g_strdup(_shortcut_description(s, FALSE));
      break;
    case SHORTCUT_VIEW_ACTION:
      if(s->action) field_text = _action_full_label_translated(s->action);
      break;
    case SHORTCUT_VIEW_ELEMENT:
      field_text = g_strdup(s->element ? "reset" : ""); // FIXME just for fakes
      break;
    case SHORTCUT_VIEW_INSTANCE:
      if(s->action)
      for(dt_action_t *owner = s->action->owner; owner; owner = owner->owner)
      {
        if(owner->type == DT_ACTION_TYPE_IOP)
        {
          dt_iop_module_so_t *iop = (dt_iop_module_so_t *)owner;
//          iop -= (dt_iop_module_so_t *)&iop->actions - iop;

          if(!(iop->flags() & IOP_FLAGS_ONE_INSTANCE))
          {
            field_text = abs(s->instance) <= (NUM_INSTANCES - 1) /2
                       ? g_strdup(_(instance_label[abs(s->instance)*2 - (s->instance > 0)]))
                       : g_strdup_printf("%+d", s->instance);
            editable = TRUE;
          }
        }
      }
      break;
    case SHORTCUT_VIEW_SPEED:
      field_text = g_strdup_printf("%.3f", s->speed);
      editable = TRUE;
      break;
    default:
      break;
    }
  }
  g_object_set(cell, "text", field_text, "editable", editable, NULL);
  g_free(field_text);
}

static void _add_prefs_column(GtkWidget *tree, GtkCellRenderer *renderer, char *name, int position)
{
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(name, renderer, NULL);
  gtk_tree_view_column_set_cell_data_func(column, renderer, _fill_tree_fields, GINT_TO_POINTER(position), NULL);
  gtk_tree_view_column_set_resizable(column, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
}

static dt_shortcut_t *find_edited_shortcut(GtkTreeModel *model, const gchar *path_string)
{
  GtkTreePath *path = gtk_tree_path_new_from_string(path_string);
  GtkTreeIter iter;
  gtk_tree_model_get_iter(model, &iter, path);
  gtk_tree_path_free(path);

  void *data_ptr = NULL;
  gtk_tree_model_get(model, &iter, 0, &data_ptr, -1);

  return g_sequence_get(data_ptr);
}

static void _speed_edited(GtkCellRendererText *cell, const gchar *path_string, const gchar *new_text, gpointer data)
{
  find_edited_shortcut(data, path_string)->speed = atof(new_text);
}

static void _instance_edited(GtkCellRendererText *cell, const gchar *path_string, const gchar *new_text, gpointer data)
{
  dt_shortcut_t *s = find_edited_shortcut(data, path_string);

  if(!(s->instance = atoi(new_text)))
    for(int i = 0; i < NUM_INSTANCES; i++)
      if(!strcmp(instance_label[i], new_text))
        s->instance = (i + 1) / 2 * (i % 2 ? 1 : -1);
}

static void grab_in_tree_view(GtkTreeView *tree_view)
{
  g_set_weak_pointer(&grab_widget, gtk_widget_get_parent(gtk_widget_get_parent(GTK_WIDGET(tree_view)))); // static
  gtk_widget_set_sensitive(grab_widget, FALSE);
  g_signal_connect(gtk_widget_get_toplevel(grab_widget), "event", G_CALLBACK(dt_shortcut_dispatcher), NULL);
}

static void ungrab_grab_widget()
{
  gdk_seat_ungrab(gdk_display_get_default_seat(gdk_display_get_default()));

  if(grab_widget)
  {
    gtk_widget_set_sensitive(grab_widget, TRUE);
    g_signal_handlers_disconnect_by_func(gtk_widget_get_toplevel(grab_widget), G_CALLBACK(dt_shortcut_dispatcher), NULL);
  }
}

static void _shortcut_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data)
{
  GtkTreeIter iter;
  gtk_tree_model_get_iter(GTK_TREE_MODEL(user_data), &iter, path);

  void *data_ptr = NULL;
  gtk_tree_model_get(GTK_TREE_MODEL(user_data), &iter, 0, &data_ptr, -1);

  dt_shortcut_t *s = g_sequence_get(data_ptr);
  bsc.action = s->action;
  bsc.instance = s->instance;

  grab_in_tree_view(tree_view);
}

static gboolean _add_actions_to_tree(GtkTreeIter *parent, dt_action_t *action)
{
  gboolean any_leaves = FALSE;

  GtkTreeIter iter;
  while(action)
  {
    gtk_tree_store_insert_with_values(actions_store, &iter, parent, -1, 0, GINT_TO_POINTER(action), -1);

    if(action->type <= DT_ACTION_TYPE_SECTION &&
       !_add_actions_to_tree(&iter, action->target))
      gtk_tree_store_remove(actions_store, &iter);
    else
      any_leaves = TRUE;

    action = action->next;
  }

  return any_leaves;
}

static void _show_action_label(GtkTreeViewColumn *column, GtkCellRenderer *cell, GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
  dt_action_t *action = NULL;
  gtk_tree_model_get(model, iter, 0, &action, -1);
  g_object_set(cell, "text", action->label_translated, NULL);
}

static void _action_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data)
{
  GtkTreeIter iter;
  gtk_tree_model_get_iter(GTK_TREE_MODEL(user_data), &iter, path);

  gtk_tree_model_get(GTK_TREE_MODEL(user_data), &iter, 0, &bsc.action, -1);
  bsc.instance = 0;

  grab_in_tree_view(tree_view);
}

gboolean _search_func(GtkTreeModel *model, gint column, const gchar *key, GtkTreeIter *iter, gpointer search_data)
{
  gboolean different = TRUE;
  if(column == 1)
  {
    dt_action_t *action = NULL;
    gtk_tree_model_get(model, iter, 0, &action, -1);
    different = !strstr(action->label_translated, key);
  }
  else
  {
    GSequenceIter *seq_iter = NULL;
    gtk_tree_model_get(model, iter, 0, &seq_iter, -1);
    if(GPOINTER_TO_UINT(seq_iter) >= NUM_CATEGORIES)
    {
      dt_shortcut_t *s = g_sequence_get(seq_iter);
      if(s->action)
      {
        gchar *label = _action_full_label_translated(s->action);
        different = !strstr(label, key);
        g_free(label);
      }
    }
  }
  if(!different)
  {
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    gtk_tree_view_expand_to_path(GTK_TREE_VIEW(search_data), path);
    gtk_tree_path_free(path);

    return FALSE;
  }

  GtkTreeIter child;
  if(gtk_tree_model_iter_children(model, &child, iter))
  {
    do
    {
      if(!_search_func(model, column, key, &child, search_data)) return FALSE;
    }
    while(gtk_tree_model_iter_next(model, &child));
  }

  return TRUE;
}

GtkWidget *dt_shortcuts_prefs()
{
  GtkWidget *container = gtk_paned_new(GTK_ORIENTATION_VERTICAL);

  // Building the shortcut treeview
  g_set_weak_pointer(&shortcuts_store, gtk_tree_store_new(1, G_TYPE_POINTER)); // static

  const dt_view_t *vw = dt_view_manager_get_current_view(darktable.view_manager);
  dt_view_type_flags_t view = vw && vw->view ? vw->view(vw) : DT_VIEW_LIGHTTABLE;

  for(gint i = 0; i < NUM_CATEGORIES; i++)
    gtk_tree_store_insert_with_values(shortcuts_store, NULL, NULL, -1, 0, GINT_TO_POINTER(i), -1);

  for(GSequenceIter *iter = g_sequence_get_begin_iter(darktable.control->shortcuts);
      !g_sequence_iter_is_end(iter);
      iter = g_sequence_iter_next(iter))
  {
    dt_shortcut_t *s = g_sequence_get(iter);
    GtkTreeIter category;
    shortcuts_store_category(&category, s, view);

    gtk_tree_store_insert_with_values(shortcuts_store, NULL, &category, -1, 0, iter, -1);
  }

  // FIXME fake fallback shortcuts just for illustration
  static GSequence *fakes = NULL;
  static dt_shortcut_t s_fine = { .mods = GDK_CONTROL_MASK , .speed = .1 };
  static dt_shortcut_t s_coarse = { .mods = GDK_SHIFT_MASK , .speed = 10. };
  static dt_shortcut_t s_reset = { .button = 1 << GDK_BUTTON_PRIMARY, .click = DT_SHORTCUT_CLICK_DOUBLE , .element = 1 };
  if(!fakes)
  {
    fakes = g_sequence_new(NULL);
    g_sequence_append(fakes, &s_coarse);
    g_sequence_append(fakes, &s_fine);
    g_sequence_append(fakes, &s_reset);
  }
  GtkTreeIter category;
  shortcuts_store_category(&category, NULL, 0);
  for(GSequenceIter *i = g_sequence_get_begin_iter(fakes); !g_sequence_iter_is_end(i); i = g_sequence_iter_next(i))
    gtk_tree_store_insert_with_values(shortcuts_store, NULL, &category, -1, 0, i, -1);
  // FIXME end fake fallbacks

  GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(shortcuts_store));
  g_object_unref(G_OBJECT(shortcuts_store));
  gtk_tree_view_set_hover_expand(GTK_TREE_VIEW(tree), TRUE);
  gtk_tree_view_set_search_column(GTK_TREE_VIEW(tree), 0); // fake column for _search_func
  gtk_tree_view_set_search_equal_func(GTK_TREE_VIEW(tree), _search_func, tree, NULL);
  g_signal_connect(G_OBJECT(tree), "row-activated", G_CALLBACK(_shortcut_row_activated), shortcuts_store);
  g_signal_connect(G_OBJECT(shortcuts_store), "row-inserted", G_CALLBACK(_shortcut_row_inserted), tree);

  // Setting up the cell renderers
  _add_prefs_column(tree, gtk_cell_renderer_text_new(), _("shortcut"), SHORTCUT_VIEW_DESCRIPTION);

  _add_prefs_column(tree, gtk_cell_renderer_text_new(), _("action"), SHORTCUT_VIEW_ACTION);

  _add_prefs_column(tree, gtk_cell_renderer_text_new(), _("element"), SHORTCUT_VIEW_ELEMENT);

  GtkCellRenderer *renderer = gtk_cell_renderer_spin_new();
  g_object_set(renderer, "adjustment", gtk_adjustment_new(1, -1000, 1000, .01, 1, 10),
                         "digits", 3, "xalign", 1.0, NULL);
  g_signal_connect(renderer, "edited", G_CALLBACK(_speed_edited), shortcuts_store);
  _add_prefs_column(tree, renderer, _("speed"), SHORTCUT_VIEW_SPEED);

  renderer = gtk_cell_renderer_combo_new();
  GtkListStore *instances = gtk_list_store_new(1, G_TYPE_STRING);
  for(int i = 0; i < NUM_INSTANCES; i++)
    gtk_list_store_insert_with_values(instances, NULL, -1, 0, _(instance_label[i]), -1);
  for(char relative[] = "-2"; (relative[0] ^= '+' ^ '-') == '-' || ++relative[1] <= '9'; )
    gtk_list_store_insert_with_values(instances, NULL, -1, 0, relative, -1);
  g_object_set(renderer, "model", instances, "text-column", 0, "has-entry", FALSE, NULL);
  g_signal_connect(renderer, "edited", G_CALLBACK(_instance_edited), shortcuts_store);
  _add_prefs_column(tree, renderer, _("instance"), SHORTCUT_VIEW_INSTANCE);

  // Adding the shortcuts treeview to its containers
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_size_request(scroll, -1, 100);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(scroll), tree);
  gtk_paned_pack1(GTK_PANED(container), scroll, TRUE, FALSE);

  // Creating the action selection treeview
  g_set_weak_pointer(&actions_store, gtk_tree_store_new(1, G_TYPE_POINTER)); // static
  _add_actions_to_tree(NULL, darktable.control->actions);

  tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(actions_store));
  g_object_unref(actions_store);
  gtk_tree_view_set_hover_expand(GTK_TREE_VIEW(tree), TRUE);
  gtk_tree_view_set_search_column(GTK_TREE_VIEW(tree), 1); // fake column for _search_func
  gtk_tree_view_set_search_equal_func(GTK_TREE_VIEW(tree), _search_func, tree, NULL);
  g_object_set(tree, "has-tooltip", TRUE, NULL);
  g_signal_connect(G_OBJECT(tree), "query-tooltip", G_CALLBACK(_shortcut_tooltip_callback), actions_store);
  g_signal_connect(G_OBJECT(tree), "row-activated", G_CALLBACK(_action_row_activated), actions_store);

  renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(_("action"), renderer, NULL);
  gtk_tree_view_column_set_cell_data_func(column, renderer, _show_action_label, NULL, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  // Adding the action treeview to its containers
  scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_size_request(scroll, -1, 100);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(scroll), tree);
  gtk_paned_pack2(GTK_PANED(container), scroll, TRUE, FALSE);

  return container;
}

void dt_shortcuts_save(const gchar *file_name)
{
  FILE *f = g_fopen(file_name, "wb");
  if(f)
  {
    for(GSequenceIter *i = g_sequence_get_begin_iter(darktable.control->shortcuts);
        !g_sequence_iter_is_end(i);
        i = g_sequence_iter_next(i))
    {
      dt_shortcut_t *s = g_sequence_get(i);

      gchar *key_name = _shortcut_key_move_name(s->key_device, s->key, s->mods, FALSE);
      fprintf(f, "%s", key_name);
      g_free(key_name);

      if(s->move_device || s->move)
      {
        gchar *move_name = _shortcut_key_move_name(s->move_device, s->move, DT_MOVE_NAME, FALSE);
        fprintf(f, ";%s", move_name);
        g_free(move_name);
      }

      if(s->button & (1 << GDK_BUTTON_PRIMARY  )) fprintf(f, ";%s", "left");
      if(s->button & (1 << GDK_BUTTON_MIDDLE   )) fprintf(f, ";%s", "middle");
      if(s->button & (1 << GDK_BUTTON_SECONDARY)) fprintf(f, ";%s", "right");
      guint clean_click = s->click & ~DT_SHORTCUT_CLICK_LONG;
      if(clean_click > DT_SHORTCUT_CLICK_SINGLE) fprintf(f, ";%s", click_string[clean_click]);
      if(s->click >= DT_SHORTCUT_CLICK_LONG) fprintf(f, ";%s", "long");

      fprintf(f, "=");

      gchar *action_label = _action_full_label(s->action);
      fprintf(f, "%s", action_label);
      g_free(action_label);

      if(s->instance == -1) fprintf(f, ";last");
      if(s->instance == +1) fprintf(f, ";first");
      if(abs(s->instance) > 1) fprintf(f, ";%+d", s->instance);
      if(s->speed != 1.0) fprintf(f, ";*%g", s->speed);

      fprintf(f, "\n");
    }

    fclose(f);
  }
}

void dt_shortcuts_load(const gchar *file_name)
{
  FILE *f = g_fopen(file_name, "rb");
  if(f)
  {
    while(!feof(f))
    {
      char line[1024];
      char *read = fgets(line, sizeof(line), f);
      if(read > 0)
      {
        line[strcspn(line, "\r\n")] = '\0';

        char *act_start = strchr(line, '=');
        if(!act_start)
        {
          fprintf(stderr, "[dt_shortcuts_load] line '%s' is not an assignment\n", line);
          continue;
        }

        dt_shortcut_t s = { .speed = 1 };

        char *token = strtok(line, "=;");
        if(strcmp(token, "None"))
        {
          s.click = DT_SHORTCUT_CLICK_SINGLE;

          char *colon = strchr(token, ':');
          if(!colon)
          {
            gtk_accelerator_parse(token, &s.key, &s.mods);
            if(s.mods) fprintf(stderr, "[dt_shortcuts_load] unexpected modifiers found in %s\n", token);
            if(!s.key) fprintf(stderr, "[dt_shortcuts_load] no key name found in %s\n", token);
          }
          else
          {
            char *key_start = colon + 1;
            *colon-- = 0;
            if(colon == token)
            {
              fprintf(stderr, "[dt_shortcuts_load] missing driver name in %s\n", token);
              continue;
            }
            dt_input_device_t id = *colon - '0';
            if(id > 9 )
              id = 0;
            else
              *colon-- = 0;

            GSList *driver = darktable.control->input_drivers;
            while(driver)
            {
              id += 10;
              dt_input_driver_definition_t *callbacks = driver->data;
              if(!strcmp(token, callbacks->name))
              {
                if(!callbacks->string_to_key(key_start, &s.key))
                  fprintf(stderr, "[dt_shortcuts_load] key not recognised in %s\n", key_start);

                s.key_device = id;
                break;
              }
              driver = driver->next;
            }
            if(!driver)
            {
              fprintf(stderr, "[dt_shortcuts_load] '%s' is not a valid driver\n", token);
              continue;
            }
          }
        }

        while((token = strtok(NULL, "=;")) && token < act_start)
        {
          char *colon = strchr(token, ':');
          if(!colon)
          {
            int mod = -1;
            while(modifier_string[++mod].modifier)
              if(!strcmp(token, modifier_string[mod].name))
              {
                s.mods |= modifier_string[mod].modifier;
                break;
              }
            if(modifier_string[mod].modifier) continue;

            if(!strcmp(token, "left"  )) { s.button |= (1 << GDK_BUTTON_PRIMARY  ); continue; }
            if(!strcmp(token, "middle")) { s.button |= (1 << GDK_BUTTON_MIDDLE   ); continue; }
            if(!strcmp(token, "right" )) { s.button |= (1 << GDK_BUTTON_SECONDARY); continue; }

            int click = 0;
            while(click_string[++click])
              if(!strcmp(token, click_string[click]))
              {
                s.click = click;
                break;
              }
            if(click_string[click]) continue;
            if(!strcmp(token, "long")) { s.click |= DT_SHORTCUT_CLICK_LONG; continue; }

            int move = 0;
            while(move_string[++move])
              if(!strcmp(token, move_string[move]))
              {
                s.move = move;
                break;
              }
            if(move_string[move]) continue;

            fprintf(stderr, "[dt_shortcuts_load] token '%s' not recognised\n", token);
          }
          else
          {
            char *move_start = colon + 1;
            *colon-- = 0;
            if(colon == token)
            {
              fprintf(stderr, "[dt_shortcuts_load] missing driver name in %s\n", token);
              continue;
            }
            dt_input_device_t id = *colon - '0';
            if(id > 9 )
              id = 0;
            else
              *colon-- = 0;

            GSList *driver = darktable.control->input_drivers;
            while(driver)
            {
              id += 10;
              dt_input_driver_definition_t *callbacks = driver->data;
              if(!strcmp(token, callbacks->name))
              {
                if(!callbacks->string_to_move(move_start, &s.move))
                  fprintf(stderr, "[dt_shortcuts_load] move not recognised in %s\n", move_start);

                s.move_device = id;
                break;
              }
              driver = driver->next;
            }
            if(!driver)
            {
              fprintf(stderr, "[dt_shortcuts_load] '%s' is not a valid driver\n", token);
              continue;
            }
          }
        }

        // find action and also views along the way
        gchar **path = g_strsplit(token, "/", 0);
        s.action = dt_action_locate(NULL, path);
        g_strfreev(path);

        if(!s.action)
        {
          fprintf(stderr, "[dt_shortcuts_load] action path '%s' not found\n", token);
          continue;
        }

        while((token = strtok(NULL, ";")))
        {
          if(!strcmp(token, "first")) s.instance =  1; else
          if(!strcmp(token, "last" )) s.instance = -1; else
          if(*token == '+' || *token == '-') sscanf(token, "%d", &s.instance); else
          if(*token == '*') sscanf(token, "*%g", &s.speed); else
          fprintf(stderr, "[dt_shortcuts_load] token '%s' not recognised\n", token);
        }

        insert_shortcut(&s, FALSE);
      }
    }
    fclose(f);
  }
}

void dt_shortcuts_reinitialise()
{
  for(GSList *d = darktable.control->input_drivers; d; d = d->next)
  {
    dt_input_driver_definition_t *driver = d->data;
    driver->module->gui_cleanup(driver->module);
    driver->module->gui_init(driver->module);
  }

  // reload shortcuts
  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(datadir, sizeof(datadir));
  gchar *file_name = g_strdup_printf("%s/shortcutsrc", datadir);
  if(g_file_test(file_name, G_FILE_TEST_EXISTS))
  {
    // start with an empty shortcuts collection
    if(darktable.control->shortcuts) g_sequence_free(darktable.control->shortcuts);
    darktable.control->shortcuts = g_sequence_new(g_free);

    dt_shortcuts_load(file_name);
  }
  g_free(file_name);

  file_name = g_strdup_printf("%s/all_actions", datadir);
  FILE *f = g_fopen(file_name, "wb");
  _dump_actions(f, darktable.control->actions);
  fclose(f);
  g_free(file_name);

  dt_control_log(_("input devices reinitialised"));
}

void dt_shortcuts_select_view(dt_view_type_flags_t view)
{
  g_sequence_sort(darktable.control->shortcuts, shortcut_compare_func, GINT_TO_POINTER(view));
}

static GSList *pressed_keys = NULL; // list of currently pressed keys
static guint pressed_button = 0;
static guint last_time = 0;

static void lookup_mapping_widget()
{
  bsc.action = g_hash_table_lookup(darktable.control->widgets, darktable.control->mapping_widget);
  if(bsc.action->target != darktable.control->mapping_widget)
  {
    // find relative module instance
    dt_action_t *owner = bsc.action->owner;
    while(owner && owner->type != DT_ACTION_TYPE_IOP) owner = owner->owner;
    if(owner)
    {
      dt_iop_module_so_t *module = (dt_iop_module_so_t *)owner;

      int current_instance = 0;
      for(GList *iop_mods = darktable.develop->iop;
          iop_mods;
          iop_mods = g_list_next(iop_mods))
      {
        dt_iop_module_t *mod = (dt_iop_module_t *)iop_mods->data;

        if(mod->so == module && mod->iop_order != INT_MAX)
        {
          current_instance++;

          if(!bsc.instance)
          {
            for(GSList *w = mod->widget_list; w; w = w->next)
            {
              if(((dt_action_widget_t *)w->data)->widget == darktable.control->mapping_widget)
              {
                bsc.instance = current_instance;
                break;
              }
            }
          }
        }
      }

      if(current_instance - bsc.instance < bsc.instance) bsc.instance -= current_instance + 1;
    }
  }
}

static void define_new_mapping()
{
  if(insert_shortcut(&bsc, TRUE))
  {
    gchar *label = _action_full_label_translated(bsc.action);
    dt_control_log(_("%s assigned to %s"), _shortcut_description(&bsc, TRUE), label);
    g_free(label);
  }

  bsc.instance = 0;
  darktable.control->mapping_widget = bsc.action = NULL;

  gchar datadir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(datadir, sizeof(datadir));
  gchar *file_name = g_strdup_printf("%s/shortcutsrc", datadir);
  dt_shortcuts_save(file_name);
  g_free(file_name);
}

static gboolean _widget_invisible(GtkWidget *w)
{
  return (!gtk_widget_get_visible(w) ||
          !gtk_widget_get_visible(gtk_widget_get_parent(w)));
}

gboolean combobox_idle_value_changed(gpointer widget)
{
  g_signal_emit_by_name(G_OBJECT(widget), "value-changed");

  while(g_idle_remove_by_data(widget));

  return FALSE;
}

static float process_mapping(float move_size)
{
  float return_value = NAN;

  bsc.views = darktable.view_manager->current_view->view(darktable.view_manager->current_view);

  GSequenceIter *existing = g_sequence_lookup(darktable.control->shortcuts, &bsc,
                                              shortcut_compare_func, GINT_TO_POINTER(bsc.views));
  if(existing)
  {
    dt_shortcut_t *bac = g_sequence_get(existing);

    dt_action_t *owner = bac->action->owner;
    while(owner && owner->type == DT_ACTION_TYPE_SECTION) owner = owner->owner;

    dt_iop_module_t *mod = NULL;
    GtkWidget *widget = bac->action->target;
    if(owner && owner->type == DT_ACTION_TYPE_IOP &&
       (bac->instance || bac->action->type == DT_ACTION_TYPE_PRESET))
    {
      // find module instance
      dt_iop_module_so_t *module = (dt_iop_module_so_t *)owner;

      int current_instance = abs(bac->instance);

      for(GList *iop_mods = bac->instance > 0
                          ? darktable.develop->iop
                          : g_list_last(darktable.develop->iop);
          iop_mods;
          iop_mods = bac->instance > 0
                    ? g_list_next(iop_mods)
                    : g_list_previous(iop_mods))
      {
        mod = (dt_iop_module_t *)iop_mods->data;

        gboolean first_widget_is_preferred = FALSE;
        if(mod->widget_list)
        {
          dt_action_widget_t *referral = mod->widget_list->data;
          first_widget_is_preferred = referral->widget == referral->action->target;
        }

        if(mod->so == module &&
           mod->iop_order != INT_MAX &&
           (!--current_instance || first_widget_is_preferred))
          break;
      }

      // find module instance widget
      if(mod && bac->action->type == DT_ACTION_TYPE_WIDGET)
      {
        for(GSList *w = mod->widget_list; w; w = w->next)
        {
          dt_action_widget_t *referral = w->data;
          if(referral->action == bac->action)
          {
            widget = referral->widget;
            break;
          }
        }
      }
    }

    if(bac->action->type == DT_ACTION_TYPE_PRESET && owner)
    {
      if(owner->type == DT_ACTION_TYPE_LIB)
      {
        dt_lib_module_t *lib = (dt_lib_module_t *)owner;
        dt_lib_presets_apply(bac->action->label_translated, lib->plugin_name, lib->version());
      }
      else if(owner->type == DT_ACTION_TYPE_IOP)
      {
        dt_gui_presets_apply_preset(bac->action->label_translated, mod);
      }
    }
    else if(bac->action->type == DT_ACTION_TYPE_WIDGET &&
            GTK_IS_WIDGET(widget) && !_widget_invisible(widget))
    {
      if(DTGTK_IS_TOGGLEBUTTON(widget))
      {
        GdkEvent *event = gdk_event_new(GDK_BUTTON_PRESS);
        event->button.state = 0; // FIXME support ctrl-press
        event->button.button = GDK_BUTTON_PRIMARY;
        event->button.window = gtk_widget_get_window(widget);
        g_object_ref(event->button.window);

        // some togglebuttons connect to the clicked signal, others to toggled or button-press-event
        if(!gtk_widget_event(widget, event))
          gtk_button_clicked(GTK_BUTTON(widget));

        gdk_event_free(event);
      }
      else if(GTK_IS_BUTTON(widget)) // test DTGTK_IS_TOGGLEBUTTON first, because it is also a button
        gtk_button_clicked(GTK_BUTTON(widget));
      else if(DT_IS_BAUHAUS_WIDGET(widget))
      {
        dt_bauhaus_widget_t *bhw = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

        if(bhw->type == DT_BAUHAUS_SLIDER)
        {
          dt_bauhaus_slider_data_t *d = &bhw->data.slider;
          if(move_size != 0)
          {
            if(bac->speed == 987.)
              dt_bauhaus_slider_reset(widget);
            else
            {
              float value = dt_bauhaus_slider_get(widget);
              float step = dt_bauhaus_slider_get_step(widget);
              float multiplier = dt_accel_get_slider_scale_multiplier() * bac->speed;

              const float min_visible = powf(10.0f, -dt_bauhaus_slider_get_digits(widget));
              if(fabsf(step*multiplier) < min_visible)
                multiplier = min_visible / fabsf(step);

              d->is_dragging = 1;
              dt_bauhaus_slider_set(widget, value + move_size * step * multiplier);
              d->is_dragging = 0;
            }

            dt_accel_widget_toast(widget);
          }

          return_value = d->pos +
                      ( d->min == -d->max ? 2 :
                      ( d->min == 0 && (d->max == 1 || d->max == 100) ? 4 : 0 ));
        }
        else
        {
          int value = dt_bauhaus_combobox_get(widget);

          if(move_size != 0)
          {
            value = CLAMP(value + move_size, 0, dt_bauhaus_combobox_length(widget) - 1);

            ++darktable.gui->reset;
            dt_bauhaus_combobox_set(widget, value);
            --darktable.gui->reset;

            g_idle_add(combobox_idle_value_changed, widget);

            dt_accel_widget_toast(widget);
          }

          return_value = - 1 - value;
        }
      }
      else
        return return_value;
    }
    else if(bac->action->type == DT_ACTION_TYPE_CLOSURE && bac->action->target)
    {
      typedef gboolean (*accel_callback)(GtkAccelGroup *accel_group, GObject *acceleratable,
                                        guint keyval, GdkModifierType modifier, gpointer p);
      ((accel_callback)((GCClosure*)widget)->callback)(NULL, NULL, bac->key, bac->mods,
                                                      ((GClosure*)bac->action->target)->data);
    }
  }

  return return_value;
}

gint cmp_key(gconstpointer a, gconstpointer b)
{
  const dt_device_key_t *key_a = a;
  const dt_device_key_t *key_b = b;
  return key_a->key_device != key_b->key_device || key_a->key != key_b->key;
}

float dt_shortcut_move(dt_input_device_t id, guint time, guint move, double size)
{
  bsc.move_device = id;
  bsc.move = move;
  bsc.speed = 1.0;

  float return_value = 0;
  GdkKeymap *keymap = gdk_keymap_get_for_display(gdk_display_get_default());

  if(!pressed_keys && !bsc.key_device && !bsc.key)
    bsc.mods = gdk_keymap_get_modifier_state(keymap);

  bsc.mods &= gdk_keymap_get_modifier_mask(keymap, GDK_MODIFIER_INTENT_DEFAULT_MOD_MASK);
  gdk_keymap_add_virtual_modifiers(keymap, &bsc.mods);

  if(darktable.control->mapping_widget && !bsc.action && size != 0) lookup_mapping_widget();

  if(bsc.action)
  {
    define_new_mapping();
  }
  else
  {
    if(pressed_keys)
    {
      for(GSList *k = pressed_keys; k; k = k->next)
      {
        dt_device_key_t *device_key = k->data;
        bsc.key_device = device_key->key_device;
        bsc.key = device_key->key;

        return_value = process_mapping(size);
      }
    }
    else
      return_value = process_mapping(size);
  }

  bsc.move_device = 0;
  bsc.move = DT_SHORTCUT_MOVE_NONE;

  return return_value;
}

static guint press_timeout_source = 0;

static gboolean _key_up_delayed(gpointer do_key)
{
  if(!pressed_keys) ungrab_grab_widget();

  if(do_key) dt_shortcut_move(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, 0, DT_SHORTCUT_MOVE_NONE, 1);

  bsc.key_device = DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE;
  bsc.key = 0;
  bsc.click = DT_SHORTCUT_CLICK_NONE;
  bsc.mods = 0;

  press_timeout_source = 0;

  return FALSE;
}

static guint click_timeout_source = 0;

static gboolean _button_release_delayed(gpointer user_data)
{
  dt_shortcut_move(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, 0, DT_SHORTCUT_MOVE_NONE, 1);

  bsc.click = DT_SHORTCUT_CLICK_NONE;
  bsc.button = pressed_button;

  click_timeout_source = 0;
  return FALSE;
}

void dt_shortcut_key_press(dt_input_device_t id, guint time, guint key, guint mods)
{
  if(id == DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE)
  {
    dt_shortcut_t simple_key = { .key_device = id, .key = key, .mods = mods, .click = DT_SHORTCUT_CLICK_SINGLE,
                                 .views = darktable.view_manager->current_view->view(darktable.view_manager->current_view) };

    GSequenceIter *existing = g_sequence_lookup(darktable.control->shortcuts, &simple_key,
                                                shortcut_compare_func, GINT_TO_POINTER(simple_key.views));

    if(existing && ((dt_shortcut_t *)g_sequence_get(existing))->action->type == DT_ACTION_TYPE_KEY_PRESSED)
      return;
  }

  dt_device_key_t this_key = { id, key };
  if(!g_slist_find_custom(pressed_keys, &this_key, cmp_key))
  {
    if(press_timeout_source)
    {
      g_source_remove(press_timeout_source);
      press_timeout_source = 0;
    }

    int delay = 0;
    g_object_get(gtk_settings_get_default(), "gtk-double-click-time", &delay, NULL);

    if(!pressed_keys)
    {
      bsc.mods = mods;
      if(id == bsc.key_device && key == bsc.key &&
         time < last_time + delay && bsc.click < DT_SHORTCUT_CLICK_TRIPLE)
        bsc.click++;
      else
      {
        bsc.click = DT_SHORTCUT_CLICK_SINGLE;

        if(darktable.control->mapping_widget && !bsc.action) lookup_mapping_widget();
      }

      GdkCursor *cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "all-scroll");
      gdk_seat_grab(gdk_display_get_default_seat(gdk_display_get_default()),
                    gtk_widget_get_window(grab_widget ? gtk_widget_get_toplevel(grab_widget)
                                                      : dt_ui_main_window(darktable.gui->ui)),
                    GDK_SEAT_CAPABILITY_ALL, FALSE, cursor,
                    NULL, NULL, NULL);
      g_object_unref(cursor);
    }

    last_time = time;
    bsc.key_device = id;
    bsc.key = key;
    bsc.button = pressed_button = 0;

    dt_device_key_t *new_key = calloc(1, sizeof(dt_device_key_t));
    *new_key = this_key;
    pressed_keys = g_slist_prepend(pressed_keys, new_key);
  }
  // key hold (CTRL-W for example) should fire without key being released if shortcut is marked as "key hold" (or something)??
  // otherwise only fire when key is released (because we are expecting scroll or something)
}

void dt_shortcut_key_release(dt_input_device_t id, guint time, guint key)
{
  dt_device_key_t this_key = { id, key };

  GSList *stored_key = g_slist_find_custom(pressed_keys, &this_key, cmp_key);
  if(stored_key)
  {
    g_free(stored_key->data);
    pressed_keys = g_slist_delete_link(pressed_keys, stored_key);

    if(!pressed_keys)
    {
      if(bsc.key_device == id && bsc.key == key)
      {
        int delay = 0;
        g_object_get(gtk_settings_get_default(), "gtk-double-click-time", &delay, NULL);

        guint passed_time = time - last_time;
        if(passed_time < delay && bsc.click < DT_SHORTCUT_CLICK_TRIPLE)
          press_timeout_source = g_timeout_add(delay - passed_time, _key_up_delayed, GINT_TO_POINTER(TRUE));
        else
        {
          if(passed_time > delay) bsc.click |= DT_SHORTCUT_CLICK_LONG;
          _key_up_delayed(GINT_TO_POINTER(passed_time < 2 * delay)); // call immediately
        }
      }
      else
      {
        _key_up_delayed(NULL);
      }
    }
  }
  else
  {
    fprintf(stderr, "[dt_shortcut_key_release] released key wasn't stored\n");
  }
}

static guint _fix_keyval(GdkEvent *event)
{
  guint keyval = 0;
  GdkKeymap *keymap = gdk_keymap_get_for_display(gdk_display_get_default());
  gdk_keymap_translate_keyboard_state(keymap, event->key.hardware_keycode, 0, 0,
                                      &keyval, NULL, NULL, NULL);
  return keyval;
}

gboolean dt_shortcut_dispatcher(GtkWidget *w, GdkEvent *event, gpointer user_data)
{
  static gdouble move_start_x = 0;
  static gdouble move_start_y = 0;

//  dt_print(DT_DEBUG_INPUT, "  [shortcut_dispatcher] %d\n", event->type);

  if(GTK_IS_WINDOW(w))
  {
    GtkWidget *focused = gtk_window_get_focus(GTK_WINDOW(w));
    if((GTK_IS_ENTRY(focused) || GTK_IS_TEXT_VIEW(focused)) && gtk_widget_event(focused, event))
    {
      return TRUE;
    }
  }

  if(!darktable.control->key_accelerators_on) return FALSE; // FIXME should eventually no longer be needed

  if(pressed_keys == NULL && event->type != GDK_KEY_PRESS && event->type != GDK_FOCUS_CHANGE) return FALSE;

  switch(event->type)
  {
  case GDK_KEY_PRESS:
    if(event->key.is_modifier) return FALSE;

    dt_shortcut_key_press(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, event->key.time, _fix_keyval(event), event->key.state);
    break;
  case GDK_KEY_RELEASE:
    if(event->key.is_modifier) return FALSE;

    dt_shortcut_key_release(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, event->key.time, _fix_keyval(event));
    break;
  case GDK_GRAB_BROKEN:
    if(event->grab_broken.implicit) break;
  case GDK_WINDOW_STATE:
    event->focus_change.in = FALSE; // fall through to GDK_FOCUS_CHANGE
  case GDK_FOCUS_CHANGE: // dialog boxes and switch to other app release grab
    if(!event->focus_change.in)
    {
      ungrab_grab_widget();
      g_slist_free_full(pressed_keys, g_free);
      pressed_keys = NULL;
      bsc.click = DT_SHORTCUT_CLICK_NONE;
    }
    break;
  case GDK_SCROLL:
    {
      int delta_y;
      dt_gui_get_scroll_unit_delta((GdkEventScroll *)event, &delta_y);
      dt_shortcut_move(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, event->scroll.time, DT_SHORTCUT_MOVE_SCROLL, - delta_y);
    }
    break;
  case GDK_MOTION_NOTIFY:
    if(bsc.move == DT_SHORTCUT_MOVE_NONE)
    {
      move_start_x = event->motion.x;
      move_start_y = event->motion.y;
      bsc.move = DT_SHORTCUT_MOVE_HORIZONTAL; // set fake direction so the start position doesn't keep resetting
      break;
    }

    gdouble x_move = event->motion.x - move_start_x;
    gdouble y_move = event->motion.y - move_start_y;
    const gdouble step_size = 10; // FIXME configurable, x & y separately

    gdouble angle = x_move / (0.001 + y_move);

    gdouble size = trunc(x_move / step_size);
    if(size != 0 && fabs(angle) >= 2)
    {
      move_start_x += size * step_size;
      move_start_y = event->motion.y;
      dt_shortcut_move(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, event->motion.time, DT_SHORTCUT_MOVE_HORIZONTAL, size);
    }
    else
    {
      size = - trunc(y_move / step_size);
      if(size != 0)
      {
        move_start_y -= size * step_size;
        if(fabs(angle) < .5)
        {
          move_start_x = event->motion.x;
          dt_shortcut_move(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, event->motion.time, DT_SHORTCUT_MOVE_VERTICAL, size);
        }
        else
        {
          move_start_x -= size * step_size * angle;
          dt_shortcut_move(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, event->motion.time,
                           angle < 0 ? DT_SHORTCUT_MOVE_SKEW : DT_SHORTCUT_MOVE_DIAGONAL, size);
        }
      }
    }
    break;
  case GDK_BUTTON_PRESS:
    pressed_button |= 1 << event->button.button;
    bsc.button = pressed_button;
    bsc.click = DT_SHORTCUT_CLICK_SINGLE;
    bsc.move = DT_SHORTCUT_MOVE_NONE;
    last_time = event->button.time;
    if(click_timeout_source)
    {
      g_source_remove(click_timeout_source);
      click_timeout_source = 0;
    }
    break;
  case GDK_DOUBLE_BUTTON_PRESS:
    bsc.click = DT_SHORTCUT_CLICK_DOUBLE;
    break;
  case GDK_TRIPLE_BUTTON_PRESS:
    bsc.click = DT_SHORTCUT_CLICK_TRIPLE;
    break;
  case GDK_BUTTON_RELEASE:
    // FIXME; check if there's a shortcut defined for double/triple (could be fallback?); if not -> no delay
    // maybe even action on PRESS rather than RELEASE
    // FIXME be careful!!; we seem to be receiving presses and releases twice!?!
    pressed_button &= ~(1 << event->button.button);

    int delay = 0;
    g_object_get(gtk_settings_get_default(), "gtk-double-click-time", &delay, NULL);

    guint passed_time = event->button.time - last_time;
    if(passed_time < delay && bsc.click < DT_SHORTCUT_CLICK_TRIPLE)
    {
      if(!click_timeout_source)
        click_timeout_source = g_timeout_add(delay - passed_time, _button_release_delayed, NULL);
    }
    else
    {
      if(passed_time > delay)
        bsc.click |= DT_SHORTCUT_CLICK_LONG;
      if(passed_time < 2 * delay)
        _button_release_delayed(NULL); // call immediately
    }
    break;
  default:
    break;
  }

  return FALSE; // FIXME is return type used? doesn't seem so (maybe because of grab)
}

static void _remove_widget_from_hashtable(GtkWidget *widget, gpointer user_data)
{
  dt_action_t *action = g_hash_table_lookup(darktable.control->widgets, widget);
  if(action && action->target == widget)
  {
    action->target = NULL;
    g_hash_table_remove(darktable.control->widgets, widget);
  }
}

static inline gchar *path_without_symbols(const gchar *path)
{
  return g_strdelimit(g_strdup(path), "=,/.", '-');
}

void dt_action_insert_sorted(dt_action_t *owner, dt_action_t *new_action)
{
  dt_action_t **insertion_point = (dt_action_t **)&owner->target;
  while(*insertion_point &&
      g_utf8_collate((*insertion_point)->label_translated, new_action->label_translated) < 0)
  {
    insertion_point = &(*insertion_point)->next;
  }
  new_action->next = *insertion_point;
  *insertion_point = new_action;
}

dt_action_t *dt_action_locate(dt_action_t *owner, gchar **path)
{
//  if(!owner) return NULL;

  gchar *clean_path = NULL;

  dt_action_t *action = owner ? owner->target : darktable.control->actions;
  while(*path)
  {
    if(!clean_path) clean_path = path_without_symbols(*path);

    if(!action)
    {
      dt_action_t *new_action = calloc(1, sizeof(dt_action_t));
      new_action->label = clean_path;
      new_action->label_translated = g_strdup(Q_(*path));
      new_action->type = DT_ACTION_TYPE_SECTION;
      new_action->owner = owner;

      dt_action_insert_sorted(owner, new_action);

      owner = new_action;
      action = NULL;
    }
    else if(!strcmp(action->label, clean_path))
    {
      g_free(clean_path);
      owner = action;
      action = action->target;
    }
    else
    {
      action = action->next;
      continue;
    }
    clean_path = NULL; // now owned by action or freed
    path++;
  }

  if(owner->type <= DT_ACTION_TYPE_SECTION && owner->target)
  {
    fprintf(stderr, "[dt_action_locate] found action '%s' not leaf node \n", owner->label);
    return NULL;
  }

  return owner;
}

void dt_action_define_key_pressed_accel(dt_action_t *action, const gchar *path, GtkAccelKey *key)
{
  dt_action_t *new_action = calloc(1, sizeof(dt_action_t));
  new_action->label = path_without_symbols(path);
  new_action->label_translated = g_strdup(Q_(path));
  new_action->type = DT_ACTION_TYPE_KEY_PRESSED;
  new_action->target = key;
  new_action->owner = action;

  dt_action_insert_sorted(action, new_action);
}

dt_action_t *_action_define(dt_action_t *owner, const gchar *path, gboolean local, guint accel_key, GdkModifierType mods, GtkWidget *widget)
{
  // add to module_so actions list
  // split on `; find any sections or if not found, create (at start)
  gchar **split_path = g_strsplit(path, "`", 6);
  dt_action_t *ac = dt_action_locate(owner, split_path);
  g_strfreev(split_path);

  if(ac)
  {
    if(owner->type == DT_ACTION_TYPE_CLOSURE && owner->target)
      g_closure_unref(owner->target);

    ac->type = DT_ACTION_TYPE_WIDGET;

    if(!darktable.control->accel_initialising)
    {
      ac->target = widget;
      g_hash_table_insert(darktable.control->widgets, widget, ac);

      // in case of bauhaus widget more efficient to directly implement in dt_bauhaus_..._destroy
      g_signal_connect(G_OBJECT(widget), "query-tooltip", G_CALLBACK(_shortcut_tooltip_callback), NULL);
      g_signal_connect(G_OBJECT(widget), "destroy", G_CALLBACK(_remove_widget_from_hashtable), NULL);
    }
  }

  return ac;
}

void dt_action_define_iop(dt_iop_module_t *self, const gchar *path, gboolean local, guint accel_key, GdkModifierType mods, GtkWidget *widget)
{
  // add to module_so actions list
  dt_action_t *ac = strstr(path,"blend`") == path
                  ? _action_define(&darktable.control->actions_blend, path + strlen("blend`"), local, accel_key, mods, widget)
                  : _action_define(&self->so->actions, path, local, accel_key, mods, widget);

  // to support multi-instance, also save per instance widget list
  dt_action_widget_t *referral = g_malloc0(sizeof(dt_action_widget_t));
  referral->action = ac;
  referral->widget = widget;
  self->widget_list = g_slist_prepend(self->widget_list, referral);
}

typedef struct _accel_iop_t
{
  dt_accel_t *accel;
  GClosure *closure;
} _accel_iop_t;

void dt_accel_path_global(char *s, size_t n, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s", "global", path);
}

void dt_accel_path_view(char *s, size_t n, char *module, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s/%s", "views", module, path);
}

void dt_accel_path_iop(char *s, size_t n, char *module, const char *path)
{
  if(path)
  {

    gchar **split_paths = g_strsplit(path, "`", 4);
    gchar **used_paths = split_paths;
    // transitionally keep "preset" translated in keyboardrc to avoid breakage for now
    // this also needs to be amended in preferences
    if(!strcmp(split_paths[0], "preset"))
    {
      g_free(split_paths[0]);
      split_paths[0] = g_strdup(_("preset"));
    }
    else if(!strcmp(split_paths[0], "blend"))
    {
      module = "blending";
      used_paths++;
    }

    for(gchar **cur_path = used_paths; *cur_path; cur_path++)
    {
      gchar *after_context = strchr(*cur_path,'|');
      if(after_context) memmove(*cur_path, after_context + 1, strlen(after_context));
    }
    gchar *joined_paths = g_strjoinv("/", used_paths);
    snprintf(s, n, "<Darktable>/%s/%s/%s", "image operations", module, joined_paths);
    g_free(joined_paths);
    g_strfreev(split_paths);
  }
  else
    snprintf(s, n, "<Darktable>/%s/%s", "image operations", module);
}

void dt_accel_path_lib(char *s, size_t n, char *module, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s/%s", "modules", module, path);
}

void dt_accel_path_lua(char *s, size_t n, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s", "lua", path);
}

void dt_accel_path_manual(char *s, size_t n, const char *full_path)
{
  snprintf(s, n, "<Darktable>/%s", full_path);
}

static void dt_accel_path_global_translated(char *s, size_t n, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s", C_("accel", "global"), g_dpgettext2(NULL, "accel", path));
}

static void dt_accel_path_view_translated(char *s, size_t n, dt_view_t *module, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s/%s", C_("accel", "views"), module->name(module),
           g_dpgettext2(NULL, "accel", path));
}

static void dt_accel_path_iop_translated(char *s, size_t n, dt_iop_module_so_t *module, const char *path)
{
  gchar *module_clean = g_strdelimit(g_strdup(module->name()), "/", '-');

  if(path)
  {
    gchar **split_paths = g_strsplit(path, "`", 4);
    gchar **used_paths = split_paths;
    if(!strcmp(split_paths[0], "blend"))
    {
      g_free(module_clean);
      module_clean = g_strconcat(_("blending"), " ", NULL);
      used_paths++;
    }
    for(gchar **cur_path = used_paths; *cur_path; cur_path++)
    {
      gchar *saved_path = *cur_path;
      *cur_path = g_strdelimit(g_strconcat(Q_(*cur_path), (strcmp(*cur_path, "preset") ? NULL : " "), NULL), "/", '`');
      g_free(saved_path);
    }
    gchar *joined_paths = g_strjoinv("/", used_paths);
    snprintf(s, n, "<Darktable>/%s/%s/%s", C_("accel", "processing modules"), module_clean, joined_paths);
    g_free(joined_paths);
    g_strfreev(split_paths);
  }
  else
    snprintf(s, n, "<Darktable>/%s/%s", C_("accel", "processing modules"), module_clean);

  g_free(module_clean);
}

static void dt_accel_path_lib_translated(char *s, size_t n, dt_lib_module_t *module, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s/%s", C_("accel", "utility modules"), module->name(module),
           g_dpgettext2(NULL, "accel", path));
}

static void dt_accel_path_lua_translated(char *s, size_t n, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s", C_("accel", "lua"), g_dpgettext2(NULL, "accel", path));
}

static void dt_accel_path_manual_translated(char *s, size_t n, const char *full_path)
{
  snprintf(s, n, "<Darktable>/%s", g_dpgettext2(NULL, "accel", full_path));
}

void dt_accel_register_shortcut(dt_action_t *owner, const gchar *path_string, guint accel_key, GdkModifierType mods)
{
#ifdef SHORTCUTS_TRANSITION

  gchar **split_path = g_strsplit(path_string, "/", 0);
  gchar **split_trans = g_strsplit(g_dpgettext2(NULL, "accel", path_string), "/", g_strv_length(split_path));

  gchar **path = split_path;
  gchar **trans = split_trans;

  gchar *clean_path = NULL;

  dt_action_t *action = owner->target;
  while(*path)
  {
    if(!clean_path) clean_path = path_without_symbols(*path);

    if(!action)
    {
      dt_action_t *new_action = calloc(1, sizeof(dt_action_t));
      new_action->label = clean_path;
      new_action->label_translated = g_strdup(*trans ? *trans : *path);
      new_action->type = DT_ACTION_TYPE_SECTION;
      new_action->owner = owner;

      dt_action_insert_sorted(owner, new_action);

      owner = new_action;
      action = NULL;
    }
    else if(!strcmp(action->label, clean_path))
    {
      g_free(clean_path);
      owner = action;
      action = action->target;
    }
    else
    {
      action = action->next;
      continue;
    }
    clean_path = NULL; // now owned by action or freed
    path++;
    if(*trans) trans++;
  }

  g_strfreev(split_path);
  g_strfreev(split_trans);

  if(accel_key != 0)
  {
    GdkKeymap *keymap = gdk_keymap_get_for_display(gdk_display_get_default());

    GdkKeymapKey *keys;
    gint n_keys, i = 0;

    if(!gdk_keymap_get_entries_for_keyval(keymap, accel_key, &keys, &n_keys)) return;

    // find the first key in group 0, if any
    while(i < n_keys - 1 && (keys[i].group > 0 || keys[i].level > 1)) i++;

    if(keys[i].level > 1)
      fprintf(stderr, "[dt_accel_register_shortcut] expected to find a key in group 0 with only shift\n");

    if(keys[i].level == 1) mods |= GDK_SHIFT_MASK;

    if(mods & GDK_CONTROL_MASK)
    {
      mods = (mods & ~GDK_CONTROL_MASK) |
             gdk_keymap_get_modifier_mask(keymap, GDK_MODIFIER_INTENT_PRIMARY_ACCELERATOR);
    }

    dt_shortcut_t s = { .key_device = DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE,
                        .click = DT_SHORTCUT_CLICK_SINGLE,
                        .mods = mods,
                        .speed = 1.0,
                        .action = owner };

    gdk_keymap_translate_keyboard_state(keymap, keys[i].keycode, 0, 0, &s.key, NULL, NULL, NULL);

    insert_shortcut(&s, FALSE);

    g_free(keys);
  }
#endif // SHORTCUTS_TRANSITION
}

void dt_accel_connect_shortcut(dt_action_t *owner, const gchar *path_string, GClosure *closure)
{
#ifdef SHORTCUTS_TRANSITION

  gchar **split_path = g_strsplit(path_string, "/", 0);
  gchar **path = split_path;

  while(*path && (owner = owner->target))
  {
    gchar *clean_path = path_without_symbols(*path);

    while(owner)
    {
      if(!strcmp(owner->label, clean_path))
        break;
      else
        owner = owner->next;
    }

    g_free(clean_path);
    path++;
  }

  if(!*path && owner)
  {
    if(owner->type == DT_ACTION_TYPE_CLOSURE && owner->target)
      g_closure_unref(owner->target);

    owner->type = DT_ACTION_TYPE_CLOSURE;
    owner->target = closure;
    g_closure_ref(closure);
    g_closure_sink(closure);
  }
  else
  {
    fprintf(stderr, "[dt_accel_connect_shortcut] '%s' not found\n", path_string);
  }

  g_strfreev(split_path);

#endif // SHORTCUTS_TRANSITION
}

void dt_accel_register_global(const gchar *path, guint accel_key, GdkModifierType mods)
{
  gchar accel_path[256];
  dt_accel_t *accel = (dt_accel_t *)g_malloc0(sizeof(dt_accel_t));

  dt_accel_path_global(accel_path, sizeof(accel_path), path);
  gtk_accel_map_add_entry(accel_path, accel_key, mods);

  g_strlcpy(accel->path, accel_path, sizeof(accel->path));
  dt_accel_path_global_translated(accel_path, sizeof(accel_path), path);
  g_strlcpy(accel->translated_path, accel_path, sizeof(accel->translated_path));

  *(accel->module) = '\0';
  accel->local = FALSE;
  accel->views = DT_VIEW_DARKROOM | DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING | DT_VIEW_MAP | DT_VIEW_PRINT | DT_VIEW_SLIDESHOW;
  darktable.control->accelerator_list = g_list_prepend(darktable.control->accelerator_list, accel);

  dt_accel_register_shortcut(&darktable.control->actions_global, path, accel_key, mods);
}

void dt_accel_register_view(dt_view_t *self, const gchar *path, guint accel_key, GdkModifierType mods)
{
  gchar accel_path[256];
  dt_accel_t *accel = (dt_accel_t *)g_malloc0(sizeof(dt_accel_t));

  dt_accel_path_view(accel_path, sizeof(accel_path), self->module_name, path);
  gtk_accel_map_add_entry(accel_path, accel_key, mods);

  g_strlcpy(accel->path, accel_path, sizeof(accel->path));
  dt_accel_path_view_translated(accel_path, sizeof(accel_path), self, path);
  g_strlcpy(accel->translated_path, accel_path, sizeof(accel->translated_path));

  g_strlcpy(accel->module, self->module_name, sizeof(accel->module));
  accel->local = FALSE;
  accel->views = self->view(self);
  darktable.control->accelerator_list = g_list_prepend(darktable.control->accelerator_list, accel);

  dt_accel_register_shortcut(&self->actions, path, accel_key, mods);
}

void dt_accel_register_iop(dt_iop_module_so_t *so, gboolean local, const gchar *path, guint accel_key,
                           GdkModifierType mods)
{
  dt_accel_t *accel = (dt_accel_t *)g_malloc0(sizeof(dt_accel_t));

  dt_accel_path_iop(accel->path, sizeof(accel->path), so->op, path);
  gtk_accel_map_add_entry(accel->path, accel_key, mods);
  dt_accel_path_iop_translated(accel->translated_path, sizeof(accel->translated_path), so, path);

  g_strlcpy(accel->module, so->op, sizeof(accel->module));
  accel->local = local;
  accel->views = DT_VIEW_DARKROOM;
  darktable.control->accelerator_list = g_list_prepend(darktable.control->accelerator_list, accel);
}

void dt_action_define_preset(dt_action_t *action, const gchar *name)
{
  gchar *path[3] = { "preset", (gchar *)name, NULL };
  dt_action_t *p = dt_action_locate(action, path);
  if(p)
  {
    p->type = DT_ACTION_TYPE_PRESET;
    p->target = (gpointer)TRUE;
  }
}

void dt_action_rename_preset(dt_action_t *action, const gchar *old_name, const gchar *new_name)
{
  gchar *path[3] = { "preset", (gchar *)old_name, NULL };
  dt_action_t *p = dt_action_locate(action, path);
  if(p)
  {
    g_free((char*)p->label);
    g_free((char*)p->label_translated);

    if(new_name)
    {
      p->label = path_without_symbols(new_name);
      p->label_translated = g_strdup(_(new_name));
    }
    else
    {
      dt_action_t **previous = (dt_action_t **)&p->owner->target;
      while(*previous)
      {
        if(*previous == p)
        {
          *previous = p->next;
          break;
        }
        previous = &(*previous)->next;
      }

      if(actions_store)
        gtk_tree_model_foreach(GTK_TREE_MODEL(actions_store), remove_from_store, p);

      GSequenceIter *iter = g_sequence_get_begin_iter(darktable.control->shortcuts);
      while(!g_sequence_iter_is_end(iter))
      {
        GSequenceIter *current = iter;
        iter = g_sequence_iter_next(iter); // remove will invalidate

        dt_shortcut_t *s = g_sequence_get(current);
        if(s->action == p)
          remove_shortcut(current);
      }

      g_free(p);
    }
  }
}

void dt_accel_register_lib_as_view(gchar *view_name, const gchar *path, guint accel_key, GdkModifierType mods)
{
  //register a lib shortcut but place it in the path of a view
  gchar accel_path[256];
  dt_accel_path_view(accel_path, sizeof(accel_path), view_name, path);
  if (dt_accel_find_by_path(accel_path)) return; // return if nothing to add, to avoid multiple entries

  dt_accel_t *accel = (dt_accel_t *)g_malloc0(sizeof(dt_accel_t));
  gtk_accel_map_add_entry(accel_path, accel_key, mods);
  g_strlcpy(accel->path, accel_path, sizeof(accel->path));

  snprintf(accel_path, sizeof(accel_path), "<Darktable>/%s/%s/%s", C_("accel", "views"),
           g_dgettext(NULL, view_name),
           g_dpgettext2(NULL, "accel", path));

  g_strlcpy(accel->translated_path, accel_path, sizeof(accel->translated_path));

  g_strlcpy(accel->module, view_name, sizeof(accel->module));
  accel->local = FALSE;

  if(strcmp(view_name, "lighttable") == 0)
    accel->views = DT_VIEW_LIGHTTABLE;
  else if(strcmp(view_name, "darkroom") == 0)
    accel->views = DT_VIEW_DARKROOM;
  else if(strcmp(view_name, "print") == 0)
    accel->views = DT_VIEW_PRINT;
  else if(strcmp(view_name, "slideshow") == 0)
    accel->views = DT_VIEW_SLIDESHOW;
  else if(strcmp(view_name, "map") == 0)
    accel->views = DT_VIEW_MAP;
  else if(strcmp(view_name, "tethering") == 0)
    accel->views = DT_VIEW_TETHERING;

  darktable.control->accelerator_list = g_list_prepend(darktable.control->accelerator_list, accel);

#ifdef SHORTCUTS_TRANSITION
  dt_action_t *a = darktable.control->actions_views.target;
  while(a)
  {
    if(!strcmp(a->label, view_name))
      break;
    else
      a = a->next;
  }
  if(a)
  {
    dt_accel_register_shortcut(a, path, accel_key, mods);
  }
  else
  {
    fprintf(stderr, "[dt_accel_register_lib_as_view] '%s' not found\n", view_name);
  }
#endif // SHORTCUTS_TRANSITION
}

void dt_accel_register_lib_for_views(dt_lib_module_t *self, dt_view_type_flags_t views, const gchar *path,
                                     guint accel_key, GdkModifierType mods)
{
  gchar accel_path[256];
  dt_accel_path_lib(accel_path, sizeof(accel_path), self->plugin_name, path);
  if (dt_accel_find_by_path(accel_path)) return; // return if nothing to add, to avoid multiple entries

  dt_accel_t *accel = (dt_accel_t *)g_malloc0(sizeof(dt_accel_t));

  gtk_accel_map_add_entry(accel_path, accel_key, mods);
  g_strlcpy(accel->path, accel_path, sizeof(accel->path));
  dt_accel_path_lib_translated(accel_path, sizeof(accel_path), self, path);
  g_strlcpy(accel->translated_path, accel_path, sizeof(accel->translated_path));

  g_strlcpy(accel->module, self->plugin_name, sizeof(accel->module));
  accel->local = FALSE;
  // we get the views in which the lib will be displayed
  accel->views = views;
  darktable.control->accelerator_list = g_list_prepend(darktable.control->accelerator_list, accel);
}

void dt_accel_register_lib(dt_lib_module_t *self, const gchar *path, guint accel_key, GdkModifierType mods)
{
  dt_view_type_flags_t v = 0;
  int i=0;
  const gchar **views = self->views(self);
  while (views[i])
  {
    if(strcmp(views[i], "lighttable") == 0)
      v |= DT_VIEW_LIGHTTABLE;
    else if(strcmp(views[i], "darkroom") == 0)
      v |= DT_VIEW_DARKROOM;
    else if(strcmp(views[i], "print") == 0)
      v |= DT_VIEW_PRINT;
    else if(strcmp(views[i], "slideshow") == 0)
      v |= DT_VIEW_SLIDESHOW;
    else if(strcmp(views[i], "map") == 0)
      v |= DT_VIEW_MAP;
    else if(strcmp(views[i], "tethering") == 0)
      v |= DT_VIEW_TETHERING;
    else if(strcmp(views[i], "*") == 0)
      v |= DT_VIEW_DARKROOM | DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING | DT_VIEW_MAP | DT_VIEW_PRINT
           | DT_VIEW_SLIDESHOW;
    i++;
  }
  dt_accel_register_lib_for_views(self, v, path, accel_key, mods);

  dt_accel_register_shortcut(&self->actions, path, accel_key, mods);
}

const gchar *_common_actions[]
  = { NC_("accel", "show module"),
      NC_("accel", "enable module"),
      NC_("accel", "focus module"),
      NC_("accel", "reset module parameters"),
      NC_("accel", "show preset menu"),
      NULL };

const gchar *_slider_actions[]
  = { NC_("accel", "increase"),
      NC_("accel", "decrease"),
      NC_("accel", "reset"),
      NC_("accel", "edit"),
      NC_("accel", "dynamic"),
      NULL };

const gchar *_combobox_actions[]
  = { NC_("accel", "next"),
      NC_("accel", "previous"),
      NC_("accel", "dynamic"),
      NULL };

void _accel_register_actions_iop(dt_iop_module_so_t *so, gboolean local, const gchar *path, const char **actions)
{
  gchar accel_path[256];
  gchar accel_path_trans[256];
  dt_accel_path_iop(accel_path, sizeof(accel_path), so->op, path);
  dt_accel_path_iop_translated(accel_path_trans, sizeof(accel_path_trans), so, path);

  for(const char **action = actions; *action; action++)
  {
    dt_accel_t *accel = (dt_accel_t *)g_malloc0(sizeof(dt_accel_t));
    snprintf(accel->path, sizeof(accel->path), "%s/%s", accel_path, *action);
    gtk_accel_map_add_entry(accel->path, 0, 0);
    snprintf(accel->translated_path, sizeof(accel->translated_path), "%s/%s ", accel_path_trans,
             g_dpgettext2(NULL, "accel", *action));
    g_strlcpy(accel->module, so->op, sizeof(accel->module));
    accel->local = local;
    accel->views = DT_VIEW_DARKROOM;

    darktable.control->accelerator_list = g_list_prepend(darktable.control->accelerator_list, accel);

    if(!path) dt_accel_register_shortcut(&so->actions, *action, 0, 0);
  }
}

void dt_accel_register_common_iop(dt_iop_module_so_t *so)
{
  _accel_register_actions_iop(so, FALSE, NULL, _common_actions);
}

void dt_accel_register_combobox_iop(dt_iop_module_so_t *so, gboolean local, const gchar *path)
{
  _accel_register_actions_iop(so, local, path, _combobox_actions);
}

void dt_accel_register_slider_iop(dt_iop_module_so_t *so, gboolean local, const gchar *path)
{
  _accel_register_actions_iop(so, local, path, _slider_actions);
}

void dt_accel_register_lua(const gchar *path, guint accel_key, GdkModifierType mods)
{
  gchar accel_path[256];
  dt_accel_t *accel = (dt_accel_t *)g_malloc0(sizeof(dt_accel_t));

  dt_accel_path_lua(accel_path, sizeof(accel_path), path);
  gtk_accel_map_add_entry(accel_path, accel_key, mods);

  g_strlcpy(accel->path, accel_path, sizeof(accel->path));
  dt_accel_path_lua_translated(accel_path, sizeof(accel_path), path);
  g_strlcpy(accel->translated_path, accel_path, sizeof(accel->translated_path));

  *(accel->module) = '\0';
  accel->local = FALSE;
  accel->views = DT_VIEW_DARKROOM | DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING | DT_VIEW_MAP | DT_VIEW_PRINT | DT_VIEW_SLIDESHOW;
  darktable.control->accelerator_list = g_list_prepend(darktable.control->accelerator_list, accel);

  dt_accel_register_shortcut(&darktable.control->actions_lua, path, accel_key, mods);
}

void dt_accel_register_manual(const gchar *full_path, dt_view_type_flags_t views, guint accel_key,
                              GdkModifierType mods)
{
  gchar accel_path[256];
  dt_accel_t *accel = (dt_accel_t *)g_malloc0(sizeof(dt_accel_t));

  dt_accel_path_manual(accel_path, sizeof(accel_path), full_path);
  gtk_accel_map_add_entry(accel_path, accel_key, mods);

  g_strlcpy(accel->path, accel_path, sizeof(accel->path));
  dt_accel_path_manual_translated(accel_path, sizeof(accel_path), full_path);
  g_strlcpy(accel->translated_path, accel_path, sizeof(accel->translated_path));

  *(accel->module) = '\0';
  accel->local = FALSE;
  accel->views = views;
  darktable.control->accelerator_list = g_list_prepend(darktable.control->accelerator_list, accel);

  gchar **split_path = g_strsplit(full_path, "/", 3);
  if(!strcmp(split_path[0], "views") && !strcmp(split_path[1], "thumbtable"))
    dt_accel_register_shortcut(&darktable.control->actions_thumb, split_path[2], accel_key, mods);
  g_strfreev(split_path);
}

static dt_accel_t *_lookup_accel(const gchar *path)
{
  for(const GList *l = darktable.control->accelerator_list; l; l = g_list_next(l))
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strcmp(accel->path, path)) return accel;
  }
  return NULL;
}

void dt_accel_connect_global(const gchar *path, GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_global(accel_path, sizeof(accel_path), path);
  dt_accel_t *laccel = _lookup_accel(accel_path);
  laccel->closure = closure;
  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path, closure);

  dt_accel_connect_shortcut(&darktable.control->actions_global, path, closure);
}

void dt_accel_connect_view(dt_view_t *self, const gchar *path, GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_view(accel_path, sizeof(accel_path), self->module_name, path);
  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path, closure);
  dt_accel_t *laccel = _lookup_accel(accel_path);
  laccel->closure = closure;

  self->accel_closures = g_slist_prepend(self->accel_closures, laccel);

  dt_accel_connect_shortcut(&self->actions, path, closure);
}

dt_accel_t *dt_accel_connect_lib_as_view(dt_lib_module_t *module, gchar *view_name, const gchar *path, GClosure *closure)
{
#ifdef SHORTCUTS_TRANSITION
  dt_action_t *a = darktable.control->actions_views.target;
  while(a)
  {
    if(!strcmp(a->label, view_name))
      break;
    else
      a = a->next;
  }
  if(a)
  {
    dt_accel_connect_shortcut(a, path, closure);
  }
  else
  {
    fprintf(stderr, "[dt_accel_register_lib_as_view] '%s' not found\n", view_name);
  }
#endif // SHORTCUTS_TRANSITION

  gchar accel_path[256];
  dt_accel_path_view(accel_path, sizeof(accel_path), view_name, path);
  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path, closure);

  dt_accel_t *accel = _lookup_accel(accel_path);
  if(!accel) return NULL; // this happens when the path doesn't match any accel (typos, ...)

  accel->closure = closure;

  module->accel_closures = g_slist_prepend(module->accel_closures, accel);
  return accel;
}

dt_accel_t *dt_accel_connect_lib_as_global(dt_lib_module_t *module, const gchar *path, GClosure *closure)
{
  dt_accel_connect_shortcut(&darktable.control->actions_global, path, closure);

  gchar accel_path[256];
  dt_accel_path_global(accel_path, sizeof(accel_path), path);

  dt_accel_t *accel = _lookup_accel(accel_path);
  if(!accel) return NULL; // this happens when the path doesn't match any accel (typos, ...)

  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path, closure);

  accel->closure = closure;

  module->accel_closures = g_slist_prepend(module->accel_closures, accel);
  return accel;
}

static dt_accel_t *_store_iop_accel_closure(dt_iop_module_t *module, gchar *accel_path, GClosure *closure)
{
  // Looking up the entry in the global accelerators list
  dt_accel_t *accel = _lookup_accel(accel_path);
  if(!accel) return NULL; // this happens when the path doesn't match any accel (typos, ...)

  GSList **save_list = accel->local ? &module->accel_closures_local : &module->accel_closures;

  _accel_iop_t *stored_accel = g_malloc(sizeof(_accel_iop_t));
  stored_accel->accel = accel;
  stored_accel->closure = closure;

  g_closure_ref(closure);
  g_closure_sink(closure);
  *save_list = g_slist_prepend(*save_list, stored_accel);

  return accel;
}

dt_accel_t *dt_accel_connect_iop(dt_iop_module_t *module, const gchar *path, GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_iop(accel_path, sizeof(accel_path), module->op, path);

  return _store_iop_accel_closure(module, accel_path, closure);
}

dt_accel_t *dt_accel_connect_lib(dt_lib_module_t *module, const gchar *path, GClosure *closure)
{
  dt_accel_connect_shortcut(&module->actions, path, closure);

  gchar accel_path[256];
  dt_accel_path_lib(accel_path, sizeof(accel_path), module->plugin_name, path);
  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path, closure);

  dt_accel_t *accel = _lookup_accel(accel_path);
  if(!accel) return NULL; // this happens when the path doesn't match any accel (typos, ...)

  accel->closure = closure;

  module->accel_closures = g_slist_prepend(module->accel_closures, accel);
  return accel;
}

void dt_accel_connect_lua(const gchar *path, GClosure *closure)
{
  dt_accel_connect_shortcut(&darktable.control->actions_lua, path, closure);

  gchar accel_path[256];
  dt_accel_path_lua(accel_path, sizeof(accel_path), path);
  dt_accel_t *laccel = _lookup_accel(accel_path);
  laccel->closure = closure;
  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path, closure);
}

void dt_accel_connect_manual(GSList **list_ptr, const gchar *full_path, GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_manual(accel_path, sizeof(accel_path), full_path);
  dt_accel_t *accel = _lookup_accel(accel_path);
  accel->closure = closure;
  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path, closure);
  *list_ptr = g_slist_prepend(*list_ptr, accel);

  gchar **split_path = g_strsplit(full_path, "/", 3);
  if(!strcmp(split_path[0], "views") && !strcmp(split_path[1], "thumbtable"))
    dt_accel_connect_shortcut(&darktable.control->actions_thumb, split_path[2], closure);
  g_strfreev(split_path);

}

static gboolean _press_button_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                       GdkModifierType modifier, gpointer data)
{
  if(!(GTK_IS_BUTTON(data))) return FALSE;

  gtk_button_clicked(GTK_BUTTON(data));
  return TRUE;
}

static gboolean _tooltip_callback(GtkWidget *widget, gint x, gint y, gboolean keyboard_mode,
                                  GtkTooltip *tooltip, gpointer user_data)
{
  char *text = gtk_widget_get_tooltip_text(widget);

  GtkAccelKey key;
  dt_accel_t *accel = g_object_get_data(G_OBJECT(widget), "dt-accel");
  if(accel && gtk_accel_map_lookup_entry(accel->path, &key))
  {
    gchar *key_name = gtk_accelerator_get_label(key.accel_key, key.accel_mods);
    if(key_name && *key_name)
    {
      char *tmp = g_strdup_printf(_("%s\n(shortcut: %s)"), text, key_name);
      g_free(text);
      text = tmp;
    }
    g_free(key_name);
  }

  gtk_tooltip_set_text(tooltip, text);
  g_free(text);
  return FALSE;
}

void dt_accel_connect_button_iop(dt_iop_module_t *module, const gchar *path, GtkWidget *button)
{
  GClosure *closure = g_cclosure_new(G_CALLBACK(_press_button_callback), button, NULL);
  dt_accel_t *accel = dt_accel_connect_iop(module, path, closure);
  g_object_set_data(G_OBJECT(button), "dt-accel", accel);

  if(gtk_widget_get_has_tooltip(button))
    g_signal_connect(G_OBJECT(button), "query-tooltip", G_CALLBACK(_tooltip_callback), NULL);

  dt_action_define_iop(module, path, FALSE, 0, 0, button);
}

void dt_accel_connect_button_lib(dt_lib_module_t *module, const gchar *path, GtkWidget *button)
{
  GClosure *closure = g_cclosure_new(G_CALLBACK(_press_button_callback), button, NULL);
  dt_accel_t *accel = dt_accel_connect_lib(module, path, closure);
  g_object_set_data(G_OBJECT(button), "dt-accel", accel);

  if(gtk_widget_get_has_tooltip(button))
    g_signal_connect(G_OBJECT(button), "query-tooltip", G_CALLBACK(_tooltip_callback), NULL);

  _action_define(&module->actions, path, FALSE, 0, 0, button);
}

void dt_accel_connect_button_lib_as_global(dt_lib_module_t *module, const gchar *path, GtkWidget *button)
{
  GClosure *closure = g_cclosure_new(G_CALLBACK(_press_button_callback), button, NULL);
  dt_accel_t *accel = dt_accel_connect_lib_as_global(module, path, closure);
  g_object_set_data(G_OBJECT(button), "dt-accel", accel);

  if(gtk_widget_get_has_tooltip(button))
    g_signal_connect(G_OBJECT(button), "query-tooltip", G_CALLBACK(_tooltip_callback), NULL);

  _action_define(&darktable.control->actions_global, path, FALSE, 0, 0, button);
}

static gboolean bauhaus_slider_edit_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                             GdkModifierType modifier, gpointer data)
{
  GtkWidget *slider = GTK_WIDGET(data);

  dt_bauhaus_show_popup(DT_BAUHAUS_WIDGET(slider));

  return TRUE;
}

void dt_accel_widget_toast(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(!darktable.gui->reset)
  {
    char *text = NULL;

    switch(w->type){
      case DT_BAUHAUS_SLIDER:
      {
        text = dt_bauhaus_slider_get_text(widget);
        break;
      }
      case DT_BAUHAUS_COMBOBOX:
        text = g_strdup(dt_bauhaus_combobox_get_text(widget));
        break;
      default: //literally impossible but hey
        return;
        break;
    }

    if(w->label[0] != '\0')
    { // label is not empty
      if(w->module && w->module->multi_name[0] != '\0')
        dt_toast_log(_("%s %s / %s: %s"), w->module->name(), w->module->multi_name, w->label, text);
      else if(w->module && !strstr(w->module->name(), w->label))
        dt_toast_log(_("%s / %s: %s"), w->module->name(), w->label, text);
      else
        dt_toast_log(_("%s: %s"), w->label, text);
    }
    else
    { //label is empty
      if(w->module && w->module->multi_name[0] != '\0')
        dt_toast_log(_("%s %s / %s"), w->module->name(), w->module->multi_name, text);
      else if(w->module)
        dt_toast_log(_("%s / %s"), w->module->name(), text);
      else
        dt_toast_log("%s", text);
    }

    g_free(text);
  }

}

float dt_accel_get_slider_scale_multiplier()
{
  const int slider_precision = dt_conf_get_int("accel/slider_precision");

  if(slider_precision == DT_IOP_PRECISION_COARSE)
  {
    return dt_conf_get_float("darkroom/ui/scale_rough_step_multiplier");
  }
  else if(slider_precision == DT_IOP_PRECISION_FINE)
  {
    return dt_conf_get_float("darkroom/ui/scale_precise_step_multiplier");
  }

  return dt_conf_get_float("darkroom/ui/scale_step_multiplier");
}

static gboolean bauhaus_slider_increase_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                 guint keyval, GdkModifierType modifier, gpointer data)
{
  GtkWidget *slider = GTK_WIDGET(data);

  if(_widget_invisible(slider)) return TRUE;

  float value = dt_bauhaus_slider_get(slider);
  float step = dt_bauhaus_slider_get_step(slider);
  float multiplier = dt_accel_get_slider_scale_multiplier();

  const float min_visible = powf(10.0f, -dt_bauhaus_slider_get_digits(slider));
  if(fabsf(step*multiplier) < min_visible)
    multiplier = min_visible / fabsf(step);

  dt_bauhaus_slider_set(slider, value + step * multiplier);

  g_signal_emit_by_name(G_OBJECT(slider), "value-changed");

  dt_accel_widget_toast(slider);
  return TRUE;
}

static gboolean bauhaus_slider_decrease_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                 guint keyval, GdkModifierType modifier, gpointer data)
{
  GtkWidget *slider = GTK_WIDGET(data);

  if(_widget_invisible(slider)) return TRUE;

  float value = dt_bauhaus_slider_get(slider);
  float step = dt_bauhaus_slider_get_step(slider);
  float multiplier = dt_accel_get_slider_scale_multiplier();

  const float min_visible = powf(10.0f, -dt_bauhaus_slider_get_digits(slider));
  if(fabsf(step*multiplier) < min_visible)
    multiplier = min_visible / fabsf(step);

  dt_bauhaus_slider_set(slider, value - step * multiplier);

  g_signal_emit_by_name(G_OBJECT(slider), "value-changed");

  dt_accel_widget_toast(slider);
  return TRUE;
}

static gboolean bauhaus_slider_reset_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                              guint keyval, GdkModifierType modifier, gpointer data)
{
  GtkWidget *slider = GTK_WIDGET(data);

  if(_widget_invisible(slider)) return TRUE;

  dt_bauhaus_slider_reset(slider);

  g_signal_emit_by_name(G_OBJECT(slider), "value-changed");

  dt_accel_widget_toast(slider);
  return TRUE;
}

static gboolean bauhaus_dynamic_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                         guint keyval, GdkModifierType modifier, gpointer data)
{
  if(DT_IS_BAUHAUS_WIDGET(data))
  {
    dt_bauhaus_widget_t *widget = DT_BAUHAUS_WIDGET(data);

    if(_widget_invisible(GTK_WIDGET(widget))) return TRUE;

    darktable.view_manager->current_view->dynamic_accel_current = GTK_WIDGET(widget);

    gchar *txt = g_strdup_printf (_("scroll to change <b>%s</b> of module %s %s"),
                                  dt_bauhaus_widget_get_label(GTK_WIDGET(widget)),
                                  widget->module->name(), widget->module->multi_name);
    dt_control_hinter_message(darktable.control, txt);
    g_free(txt);
  }
  else
    dt_control_hinter_message(darktable.control, "");

  return TRUE;
}

static gboolean bauhaus_combobox_next_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                 guint keyval, GdkModifierType modifier, gpointer data)
{
  GtkWidget *combobox = GTK_WIDGET(data);

  if(_widget_invisible(combobox)) return TRUE;

  const int currentval = dt_bauhaus_combobox_get(combobox);
  const int nextval = currentval + 1 >= dt_bauhaus_combobox_length(combobox) ? 0 : currentval + 1;
  dt_bauhaus_combobox_set(combobox, nextval);

  dt_accel_widget_toast(combobox);

  return TRUE;
}

static gboolean bauhaus_combobox_prev_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                 guint keyval, GdkModifierType modifier, gpointer data)
{
  GtkWidget *combobox = GTK_WIDGET(data);

  if(_widget_invisible(combobox)) return TRUE;

  const int currentval = dt_bauhaus_combobox_get(combobox);
  const int prevval = currentval - 1 < 0 ? dt_bauhaus_combobox_length(combobox) : currentval - 1;
  dt_bauhaus_combobox_set(combobox, prevval);

  dt_accel_widget_toast(combobox);

  return TRUE;
}

void _accel_connect_actions_iop(dt_iop_module_t *module, const gchar *path,
                               GtkWidget *w, const gchar *actions[], void *callbacks[])
{
  gchar accel_path[256];
  dt_accel_path_iop(accel_path, sizeof(accel_path) - 1, module->op, path);
  size_t path_len = strlen(accel_path);
  accel_path[path_len++] = '/';

  for(const char **action = actions; *action; action++, callbacks++)
  {
    strncpy(accel_path + path_len, *action, sizeof(accel_path) - path_len);

    GClosure *closure = g_cclosure_new(G_CALLBACK(*callbacks), (gpointer)w, NULL);

    _store_iop_accel_closure(module, accel_path, closure);
  }
}

void dt_accel_connect_combobox_iop(dt_iop_module_t *module, const gchar *path, GtkWidget *combobox)
{
  assert(DT_IS_BAUHAUS_WIDGET(combobox));

  void *combobox_callbacks[]
    = { bauhaus_combobox_next_callback,
        bauhaus_combobox_prev_callback,
        bauhaus_dynamic_callback };

  _accel_connect_actions_iop(module, path, combobox, _combobox_actions, combobox_callbacks);
}

void dt_accel_connect_slider_iop(dt_iop_module_t *module, const gchar *path, GtkWidget *slider)
{
  assert(DT_IS_BAUHAUS_WIDGET(slider));

  void *slider_callbacks[]
    = { bauhaus_slider_increase_callback,
        bauhaus_slider_decrease_callback,
        bauhaus_slider_reset_callback,
        bauhaus_slider_edit_callback,
        bauhaus_dynamic_callback };

  _accel_connect_actions_iop(module, path, slider, _slider_actions, slider_callbacks);
}

void dt_accel_connect_instance_iop(dt_iop_module_t *module)
{
  for(GSList *l = module->accel_closures; l; l = g_slist_next(l))
  {
    _accel_iop_t *stored_accel = (_accel_iop_t *)l->data;
    if(stored_accel && stored_accel->accel && stored_accel->closure)
    {

      if(stored_accel->accel->closure)
        gtk_accel_group_disconnect(darktable.control->accelerators, stored_accel->accel->closure);

      stored_accel->accel->closure = stored_accel->closure;

      gtk_accel_group_connect_by_path(darktable.control->accelerators,
                                      stored_accel->accel->path, stored_accel->closure);
    }
  }

  for(GSList *w = module->widget_list; w; w = w->next)
  {
    dt_action_widget_t *referral = w->data;
    referral->action->target = referral->widget;
  }
}

void dt_accel_connect_locals_iop(dt_iop_module_t *module)
{
  for(GSList *l = module->accel_closures_local; l; l = g_slist_next(l))
  {
    _accel_iop_t *accel = (_accel_iop_t *)l->data;
    if(accel)
    {
      gtk_accel_group_connect_by_path(darktable.control->accelerators, accel->accel->path, accel->closure);
    }
  }

  module->local_closures_connected = TRUE;
}

void dt_accel_disconnect_list(GSList **list_ptr)
{
  GSList *list = *list_ptr;
  while(list)
  {
    dt_accel_t *accel = (dt_accel_t *)list->data;
    if(accel) gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);
    list = g_slist_delete_link(list, list);
  }
  *list_ptr = NULL;
}

void dt_accel_disconnect_locals_iop(dt_iop_module_t *module)
{
  if(!module->local_closures_connected) return;

  for(GSList *l = module->accel_closures_local; l; l = g_slist_next(l))
  {
    _accel_iop_t *accel = (_accel_iop_t *)l->data;
    if(accel)
    {
      gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);
    }
  }

  module->local_closures_connected = FALSE;
}

void _free_iop_accel(gpointer data)
{
  _accel_iop_t *accel = (_accel_iop_t *) data;

  if(accel->accel->closure == accel->closure)
  {
    gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);
    accel->accel->closure = NULL;
  }

  if(accel->closure->ref_count != 1)
    fprintf(stderr, "iop accel refcount %d %s\n", accel->closure->ref_count, accel->accel->path);

  g_closure_unref(accel->closure);

  g_free(accel);
}

void dt_accel_cleanup_closures_iop(dt_iop_module_t *module)
{
  dt_accel_disconnect_locals_iop(module);

  g_slist_free_full(module->accel_closures, _free_iop_accel);
  g_slist_free_full(module->accel_closures_local, _free_iop_accel);
  module->accel_closures = NULL;
  module->accel_closures_local = NULL;
}

typedef struct
{
  dt_iop_module_t *module;
  char *name;
} preset_iop_module_callback_description;

static void preset_iop_module_callback_destroyer(gpointer data, GClosure *closure)
{
  preset_iop_module_callback_description *callback_description
      = (preset_iop_module_callback_description *)data;
  g_free(callback_description->name);
  g_free(data);
}

static gboolean preset_iop_module_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  preset_iop_module_callback_description *callback_description
      = (preset_iop_module_callback_description *)data;
  dt_iop_module_t *module = callback_description->module;
  const char *name = callback_description->name;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT op_params, enabled, blendop_params, "
                                                             "blendop_version FROM data.presets "
                                                             "WHERE operation = ?1 AND name = ?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, name, -1, SQLITE_TRANSIENT);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *op_params = sqlite3_column_blob(stmt, 0);
    int op_length = sqlite3_column_bytes(stmt, 0);
    int enabled = sqlite3_column_int(stmt, 1);
    const void *blendop_params = sqlite3_column_blob(stmt, 2);
    int bl_length = sqlite3_column_bytes(stmt, 2);
    int blendop_version = sqlite3_column_int(stmt, 3);
    if(op_params && (op_length == module->params_size))
    {
      memcpy(module->params, op_params, op_length);
      module->enabled = enabled;
    }
    if(blendop_params && (blendop_version == dt_develop_blend_version())
       && (bl_length == sizeof(dt_develop_blend_params_t)))
    {
      memcpy(module->blend_params, blendop_params, sizeof(dt_develop_blend_params_t));
    }
    else if(blendop_params
            && dt_develop_blend_legacy_params(module, blendop_params, blendop_version, module->blend_params,
                                              dt_develop_blend_version(), bl_length) == 0)
    {
      // do nothing
    }
    else
    {
      memcpy(module->blend_params, module->default_blendop_params, sizeof(dt_develop_blend_params_t));
    }
  }
  sqlite3_finalize(stmt);
  dt_iop_gui_update(module);
  dt_dev_add_history_item(darktable.develop, module, FALSE);
  gtk_widget_queue_draw(module->widget);
  return TRUE;
}

void dt_accel_connect_preset_iop(dt_iop_module_t *module, const gchar *path)
{
  char build_path[1024];
  gchar *name = g_strdup(path);
  snprintf(build_path, sizeof(build_path), "%s`%s", N_("preset"), name);
  preset_iop_module_callback_description *callback_description
      = g_malloc(sizeof(preset_iop_module_callback_description));
  callback_description->module = module;
  callback_description->name = name;

  GClosure *closure = g_cclosure_new(G_CALLBACK(preset_iop_module_callback), callback_description,
                                     preset_iop_module_callback_destroyer);
  dt_accel_connect_iop(module, build_path, closure);
}



typedef struct
{
  dt_lib_module_t *module;
  char *name;
} preset_lib_module_callback_description;

static void preset_lib_module_callback_destroyer(gpointer data, GClosure *closure)
{
  preset_lib_module_callback_description *callback_description
      = (preset_lib_module_callback_description *)data;
  g_free(callback_description->name);
  g_free(data);
}
static gboolean preset_lib_module_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)

{
  preset_lib_module_callback_description *callback_description
      = (preset_lib_module_callback_description *)data;
  dt_lib_module_t *module = callback_description->module;
  const char *pn = callback_description->name;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT op_params FROM data.presets WHERE operation = ?1 AND op_version = ?2 AND name = ?3", -1, &stmt,
      NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, pn, -1, SQLITE_TRANSIENT);

  int res = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *blob = sqlite3_column_blob(stmt, 0);
    int length = sqlite3_column_bytes(stmt, 0);
    if(blob)
    {
      for(const GList *it = darktable.lib->plugins; it; it = g_list_next(it))
      {
        dt_lib_module_t *search_module = (dt_lib_module_t *)it->data;
        if(!strncmp(search_module->plugin_name, module->plugin_name, 128))
        {
          res = module->set_params(module, blob, length);
          break;
        }
      }
    }
  }
  sqlite3_finalize(stmt);
  if(res)
  {
    dt_control_log(_("deleting preset for obsolete module"));
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM data.presets WHERE operation = ?1 AND op_version = ?2 AND name = ?3",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->plugin_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, pn, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  return TRUE;
}

void dt_accel_connect_preset_lib(dt_lib_module_t *module, const gchar *path)
{
  char build_path[1024];
  gchar *name = g_strdup(path);
  snprintf(build_path, sizeof(build_path), "%s/%s", _("preset"), name);
  preset_lib_module_callback_description *callback_description
      = g_malloc(sizeof(preset_lib_module_callback_description));
  callback_description->module = module;
  callback_description->name = name;

  GClosure *closure = g_cclosure_new(G_CALLBACK(preset_lib_module_callback), callback_description,
                                     preset_lib_module_callback_destroyer);
  dt_accel_connect_lib(module, build_path, closure);
}

void dt_accel_deregister_iop(dt_iop_module_t *module, const gchar *path)
{
  char build_path[1024];
  dt_accel_path_iop(build_path, sizeof(build_path), module->op, path);

  dt_accel_t *accel = NULL;

  for(const GList *modules = darktable.develop->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;

    if(mod->so == module->so)
    {
      GSList **current_list = &mod->accel_closures;
      GSList *l = *current_list;
      while(l)
      {
        _accel_iop_t *iop_accel = (_accel_iop_t *)l->data;

        if(iop_accel && iop_accel->accel && !strncmp(iop_accel->accel->path, build_path, 1024))
        {
          accel = iop_accel->accel;

          if(iop_accel->closure == accel->closure || (accel->local && module->local_closures_connected))
            gtk_accel_group_disconnect(darktable.control->accelerators, iop_accel->closure);

          *current_list = g_slist_delete_link(*current_list, l);

          g_closure_unref(iop_accel->closure);

          g_free(iop_accel);

          break;
        }

        l = g_slist_next(l);
        // if we've run out of global accelerators, switch to processing the local accelerators
        if(!l && current_list == &mod->accel_closures) l = *(current_list = &module->accel_closures_local);
      }
    }
  }

  if(accel)
  {
    darktable.control->accelerator_list = g_list_remove(darktable.control->accelerator_list, accel);

    g_free(accel);
  }
}

void dt_accel_deregister_lib(dt_lib_module_t *module, const gchar *path)
{
  char build_path[1024];
  dt_accel_path_lib(build_path, sizeof(build_path), module->plugin_name, path);
  for(GSList *l = module->accel_closures; l; l = g_slist_next(l))
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      module->accel_closures = g_slist_delete_link(module->accel_closures, l);
      gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);
      break;
    }
  }
  for(GList *ll = darktable.control->accelerator_list; ll; ll = g_list_next(ll))
  {
    dt_accel_t *accel = (dt_accel_t *)ll->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      darktable.control->accelerator_list = g_list_delete_link(darktable.control->accelerator_list, ll);
      g_free(accel);
      break;
    }
  }
}

void dt_accel_deregister_global(const gchar *path)
{
  char build_path[1024];
  dt_accel_path_global(build_path, sizeof(build_path), path);
  for(GList *l = darktable.control->accelerator_list; l; l = g_list_next(l))
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      darktable.control->accelerator_list = g_list_delete_link(darktable.control->accelerator_list, l);
      gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);
      g_free(accel);
      break;
    }
  }
}

void dt_accel_deregister_lua(const gchar *path)
{
  char build_path[1024];
  dt_accel_path_lua(build_path, sizeof(build_path), path);
  for(GList *l = darktable.control->accelerator_list; l; l = g_list_next(l))
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      darktable.control->accelerator_list = g_list_delete_link(darktable.control->accelerator_list, l);
      gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);
      g_free(accel);
      break;
    }
  }
}

void dt_accel_deregister_manual(GSList *list, const gchar *full_path)
{
  char build_path[1024];
  dt_accel_path_manual(build_path, sizeof(build_path), full_path);
  for(GSList *l = list; l; l = g_slist_next(l))
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      list = g_slist_delete_link(list, l);
      gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);
      break;
    }
  }
  for(GList *ll = darktable.control->accelerator_list; ll; ll = g_list_next(ll))
  {
    dt_accel_t *accel = (dt_accel_t *)ll->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      darktable.control->accelerator_list = g_list_delete_link(darktable.control->accelerator_list, ll);
      g_free(accel);
      break;
    }
  }
}

gboolean find_accel_internal(GtkAccelKey *key, GClosure *closure, gpointer data)
{
  return (closure == data);
}

void dt_accel_rename_preset_iop(dt_iop_module_t *module, const gchar *path, const gchar *new_path)
{
#ifndef SHORTCUTS_TRANSITION
  char *path_preset = g_strdup_printf("%s`%s", N_("preset"), path);

  char build_path[1024];
  dt_accel_path_iop(build_path, sizeof(build_path), module->op, path_preset);

  for(GSList *l = module->accel_closures; l; l = g_slist_next(l))
  {
    _accel_iop_t *iop_accel = (_accel_iop_t *)l->data;
    if(iop_accel && iop_accel->accel && !strncmp(iop_accel->accel->path, build_path, 1024))
    {
      GtkAccelKey tmp_key
          = *(gtk_accel_group_find(darktable.control->accelerators, find_accel_internal, iop_accel->closure));
      gboolean local = iop_accel->accel->local;

      dt_accel_deregister_iop(module, path_preset);

      snprintf(build_path, sizeof(build_path), "%s`%s", N_("preset"), new_path);
      dt_accel_register_iop(module->so, local, build_path, tmp_key.accel_key, tmp_key.accel_mods);

      for(const GList *modules = darktable.develop->iop; modules; modules = g_list_next(modules))
      {
        dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
        if(mod->so == module->so)
          dt_accel_connect_preset_iop(mod, new_path);
      }

      break;
    }
  }

  g_free(path_preset);

  dt_accel_connect_instance_iop(module);
#endif // ifndef SHORTCUTS_TRANSITION
}

void dt_accel_rename_preset_lib(dt_lib_module_t *module, const gchar *path, const gchar *new_path)
{
#ifndef SHORTCUTS_TRANSITION
  char build_path[1024];
  dt_accel_path_lib(build_path, sizeof(build_path), module->plugin_name, path);
  for(GSList *l = module->accel_closures; l; l = g_slist_next(l))
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      GtkAccelKey tmp_key
          = *(gtk_accel_group_find(darktable.control->accelerators, find_accel_internal, accel->closure));
      dt_accel_deregister_lib(module, path);
      snprintf(build_path, sizeof(build_path), "%s/%s", _("preset"), new_path);
      dt_accel_register_lib(module, build_path, tmp_key.accel_key, tmp_key.accel_mods);
      dt_accel_connect_preset_lib(module, new_path);
      break;
    }
  }
#endif // ifndef SHORTCUTS_TRANSITION
}

void dt_accel_rename_global(const gchar *path, const gchar *new_path)
{
  char build_path[1024];
  dt_accel_path_global(build_path, sizeof(build_path), path);
  for(GList *l = darktable.control->accelerator_list; l; l = g_list_next(l))
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      GtkAccelKey tmp_key
          = *(gtk_accel_group_find(darktable.control->accelerators, find_accel_internal, accel->closure));
      GClosure* closure = g_closure_ref(accel->closure);
      dt_accel_deregister_global(path);
      dt_accel_register_global(new_path, tmp_key.accel_key, tmp_key.accel_mods);
      dt_accel_connect_global(new_path, closure);
      g_closure_unref(closure);
      break;
    }
  }
}

void dt_accel_rename_lua(const gchar *path, const gchar *new_path)
{
  char build_path[1024];
  dt_accel_path_lua(build_path, sizeof(build_path), path);
  for(GList *l = darktable.control->accelerator_list; l; l = g_list_next(l))
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      GtkAccelKey tmp_key
          = *(gtk_accel_group_find(darktable.control->accelerators, find_accel_internal, accel->closure));
      GClosure* closure = g_closure_ref(accel->closure);
      dt_accel_deregister_lua(path);
      dt_accel_register_lua(new_path, tmp_key.accel_key, tmp_key.accel_mods);
      dt_accel_connect_lua(new_path, closure);
      g_closure_unref(closure);
      break;
    }
  }
}

dt_accel_t *dt_accel_find_by_path(const gchar *path)
{
  return _lookup_accel(path);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
