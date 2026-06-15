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

#include "gui/preferences_p2p.h"
#include "common/darktable.h"
#include "common/p2p.h"
#include "control/conf.h"
#include "gui/gtk.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <string.h>

typedef struct _p2p_prefs_t
{
  GtkWidget *passphrase_entry;
  GtkWidget *fingerprint_label;
  GtkWidget *status_label;
  GtkWidget *apply_btn;
  GtkWidget *keys_view;
  GtkWidget *peers_view;
  GtkWidget *proxy_dir_btn;
  GtkWidget *import_dir_btn;
} _p2p_prefs_t;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static void _refresh_fingerprint(_p2p_prefs_t *d)
{
  const gchar *cfg = g_get_user_config_dir();
  gchar *fp_path = g_build_filename(cfg, "darktable", "peer.fingerprint", NULL);
  gchar *fp = NULL;
  gsize len = 0;
  if(g_file_get_contents(fp_path, &fp, &len, NULL) && fp && len > 0)
  {
    g_strchomp(fp);
    gtk_label_set_text(GTK_LABEL(d->fingerprint_label), fp);
  }
  else
    gtk_label_set_text(GTK_LABEL(d->fingerprint_label), _("(unavailable — start daemon first)"));
  g_free(fp);
  g_free(fp_path);
}

static void _refresh_status(_p2p_prefs_t *d)
{
  const gboolean running = dt_p2p_is_running();
  gtk_label_set_text(GTK_LABEL(d->status_label),
                     running ? _("running") : _("stopped"));
}

static gchar *_read_text_file(const gchar *path)
{
  gchar *content = NULL;
  g_file_get_contents(path, &content, NULL, NULL);
  return content ? content : g_strdup("");
}

static void _load_text_view(GtkWidget *view, const gchar *path)
{
  gchar *content = _read_text_file(path);
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), content, -1);
  g_free(content);
}

static void _save_text_view(GtkWidget *view, const gchar *path)
{
  GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buf, &start, &end);
  gchar *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
  g_file_set_contents(path, text, -1, NULL);
  g_free(text);
}

// ---------------------------------------------------------------------------
// callbacks
// ---------------------------------------------------------------------------

static void _on_show_passphrase_toggled(GtkToggleButton *btn, gpointer user_data)
{
  GtkEntry *entry = GTK_ENTRY(user_data);
  gtk_entry_set_visibility(entry, gtk_toggle_button_get_active(btn));
}

static void _on_copy_fingerprint(GtkButton *btn, gpointer user_data)
{
  GtkLabel *label = GTK_LABEL(user_data);
  const gchar *fp = gtk_label_get_text(label);
  if(fp && *fp && *fp != '(')
  {
    GtkClipboard *clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clip, fp, -1);
  }
}

static void _on_apply_clicked(GtkButton *btn, gpointer user_data)
{
  _p2p_prefs_t *d = (_p2p_prefs_t *)user_data;

  // Save passphrase.
  const gchar *pw = gtk_entry_get_text(GTK_ENTRY(d->passphrase_entry));
  if(pw && pw[0])
  {
    dt_conf_set_string("plugins/p2p/passphrase", pw);
    const gchar *cfg = g_get_user_config_dir();
    gchar *pw_path = g_build_filename(cfg, "darktable", "peer.pw", NULL);
    g_file_set_contents(pw_path, pw, -1, NULL);
    g_free(pw_path);
  }

  // Save proxy / import directories from file-chooser buttons.
  const gchar *proxy_dir =
    gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d->proxy_dir_btn));
  if(proxy_dir)
    dt_conf_set_string("plugins/p2p/proxy_dir", proxy_dir);

  const gchar *import_dir =
    gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d->import_dir_btn));
  if(import_dir)
    dt_conf_set_string("plugins/p2p/import_dir", import_dir);

  // Save peer.keys and peers.txt.
  const gchar *cfg = g_get_user_config_dir();
  gchar *keys_path  = g_build_filename(cfg, "darktable", "peer.keys",  NULL);
  gchar *peers_path = g_build_filename(cfg, "darktable", "peers.txt",  NULL);
  _save_text_view(d->keys_view,  keys_path);
  _save_text_view(d->peers_view, peers_path);
  g_free(keys_path);
  g_free(peers_path);

  // Restart daemon with new config.
  gtk_label_set_text(GTK_LABEL(d->status_label), _("restarting…"));
  gtk_widget_set_sensitive(d->apply_btn, FALSE);

  dt_p2p_restart();

  gtk_widget_set_sensitive(d->apply_btn, TRUE);
  _refresh_status(d);
  _refresh_fingerprint(d);
}

// ---------------------------------------------------------------------------
// section header helper (matches AI tab style)
// ---------------------------------------------------------------------------

static void _add_section(GtkWidget *grid, int *row, const gchar *title)
{
  GtkWidget *label = gtk_label_new(title);
  GtkWidget *box = dt_gui_hbox(label);
  gtk_widget_set_name(box, "pref_section");
  gtk_grid_attach(GTK_GRID(grid), box, 0, (*row)++, 4, 1);
}

// ---------------------------------------------------------------------------
// init_tab_p2p
// ---------------------------------------------------------------------------

void init_tab_p2p(GtkWidget *dialog, GtkWidget *stack)
{
  _p2p_prefs_t *d = g_new0(_p2p_prefs_t, 1);

  GtkWidget *main_box = dt_gui_vbox();
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_start(grid, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_end(grid,   DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_top(grid,   DT_PIXEL_APPLY_DPI(8));
  dt_gui_box_add(main_box, grid);

  int row = 0;

  // ---- connection section ------------------------------------------------
  _add_section(grid, &row, _("connection"));

  // Passphrase row
  d->passphrase_entry = gtk_entry_new();
  gtk_entry_set_visibility(GTK_ENTRY(d->passphrase_entry), FALSE);
  gtk_entry_set_placeholder_text(GTK_ENTRY(d->passphrase_entry),
                                 _("shared passphrase"));
  gtk_widget_set_hexpand(d->passphrase_entry, TRUE);
  gtk_widget_set_tooltip_text(d->passphrase_entry,
    _("Shared passphrase — all peers on the same passphrase trust each other automatically.\n"
      "Stored in ~/.config/darktable/peer.pw and darktablerc."));

  const gchar *stored_pw = dt_conf_get_string_const("plugins/p2p/passphrase");
  if(stored_pw && stored_pw[0])
    gtk_entry_set_text(GTK_ENTRY(d->passphrase_entry), stored_pw);

  GtkWidget *show_btn = gtk_toggle_button_new_with_label(_("show"));
  g_signal_connect(show_btn, "toggled",
                   G_CALLBACK(_on_show_passphrase_toggled), d->passphrase_entry);

  GtkWidget *pw_box = dt_gui_hbox(d->passphrase_entry, show_btn);
  gtk_widget_set_hexpand(pw_box, TRUE);
  gtk_grid_attach(GTK_GRID(grid), gtk_label_new(_("passphrase")), 0, row, 1, 1);
  gtk_widget_set_halign(gtk_grid_get_child_at(GTK_GRID(grid), 0, row), GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), pw_box, 2, row++, 2, 1);

  // Fingerprint row
  d->fingerprint_label = gtk_label_new("");
  gtk_label_set_selectable(GTK_LABEL(d->fingerprint_label), TRUE);
  gtk_label_set_ellipsize(GTK_LABEL(d->fingerprint_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_hexpand(d->fingerprint_label, TRUE);
  gtk_widget_set_halign(d->fingerprint_label, GTK_ALIGN_START);
  gtk_widget_set_tooltip_text(d->fingerprint_label,
    _("Your TLS public key fingerprint (SHA256).\n"
      "Add this to peer.keys on other machines to allow connections with a different passphrase."));

  GtkWidget *copy_btn = gtk_button_new_with_label(_("copy"));
  gtk_widget_set_tooltip_text(copy_btn, _("Copy fingerprint to clipboard"));
  g_signal_connect(copy_btn, "clicked",
                   G_CALLBACK(_on_copy_fingerprint), d->fingerprint_label);

  GtkWidget *fp_box = dt_gui_hbox(d->fingerprint_label, copy_btn);
  gtk_widget_set_hexpand(fp_box, TRUE);
  GtkWidget *fp_label = gtk_label_new(_("fingerprint"));
  gtk_widget_set_halign(fp_label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), fp_label, 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), fp_box,   2, row++, 2, 1);

  // Status + restart row
  d->status_label = gtk_label_new("");
  gtk_widget_set_halign(d->status_label, GTK_ALIGN_START);

  d->apply_btn = gtk_button_new_with_label(_("Apply & restart daemon"));
  gtk_widget_set_tooltip_text(d->apply_btn,
    _("Save all settings and restart the P2P sync daemon."));
  g_signal_connect(d->apply_btn, "clicked",
                   G_CALLBACK(_on_apply_clicked), d);

  GtkWidget *status_row = dt_gui_hbox(d->status_label);
  gtk_box_pack_end(GTK_BOX(status_row), d->apply_btn, FALSE, FALSE, 0);

  GtkWidget *status_label_key = gtk_label_new(_("daemon"));
  gtk_widget_set_halign(status_label_key, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), status_label_key, 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), status_row,       2, row++, 2, 1);

  // ---- directories section -----------------------------------------------
  _add_section(grid, &row, _("directories"));

  // Proxy directory
  d->proxy_dir_btn = gtk_file_chooser_button_new(
    _("Select proxy directory"), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
  gtk_widget_set_hexpand(d->proxy_dir_btn, TRUE);
  gtk_widget_set_tooltip_text(d->proxy_dir_btn,
    _("Directory scanned for proxy AVIF files to announce to peers.\n"
      "Usually your main photo library."));
  const gchar *proxy_dir = dt_conf_get_string_const("plugins/p2p/proxy_dir");
  if(proxy_dir && proxy_dir[0])
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(d->proxy_dir_btn), proxy_dir);
  GtkWidget *pd_label = gtk_label_new(_("proxy directory"));
  gtk_widget_set_halign(pd_label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), pd_label,       0, row,   1, 1);
  gtk_grid_attach(GTK_GRID(grid), d->proxy_dir_btn, 2, row++, 2, 1);

  // Import directory
  d->import_dir_btn = gtk_file_chooser_button_new(
    _("Select import directory"), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
  gtk_widget_set_hexpand(d->import_dir_btn, TRUE);
  gtk_widget_set_tooltip_text(d->import_dir_btn,
    _("Directory where remote images that don't exist locally are placed."));
  const gchar *import_dir = dt_conf_get_string_const("plugins/p2p/import_dir");
  if(import_dir && import_dir[0])
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(d->import_dir_btn), import_dir);
  GtkWidget *id_label = gtk_label_new(_("import directory"));
  gtk_widget_set_halign(id_label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), id_label,        0, row,   1, 1);
  gtk_grid_attach(GTK_GRID(grid), d->import_dir_btn, 2, row++, 2, 1);

  // ---- trusted peer keys section -----------------------------------------
  _add_section(grid, &row, _("trusted peer keys (peer.keys)"));

  GtkWidget *keys_desc = gtk_label_new(
    _("One SHA256 fingerprint per line. Peers sharing your passphrase are trusted "
      "automatically — only add keys here for peers with a different passphrase."));
  gtk_label_set_line_wrap(GTK_LABEL(keys_desc), TRUE);
  gtk_label_set_xalign(GTK_LABEL(keys_desc), 0.0f);
  gtk_widget_set_margin_bottom(keys_desc, DT_PIXEL_APPLY_DPI(4));
  gtk_grid_attach(GTK_GRID(grid), keys_desc, 0, row++, 4, 1);

  d->keys_view = gtk_text_view_new();
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(d->keys_view), TRUE);
  gtk_widget_set_tooltip_text(d->keys_view,
    _("Trusted fingerprints from ~/.config/darktable/peer.keys"));
  GtkWidget *keys_scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(keys_scroll),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(keys_scroll),
                                              DT_PIXEL_APPLY_DPI(80));
  gtk_container_add(GTK_CONTAINER(keys_scroll), d->keys_view);
  gtk_widget_set_hexpand(keys_scroll, TRUE);
  gtk_grid_attach(GTK_GRID(grid), keys_scroll, 0, row++, 4, 1);

  // ---- static peers section -----------------------------------------------
  _add_section(grid, &row, _("static peers (peers.txt)"));

  GtkWidget *peers_desc = gtk_label_new(
    _("One https://host:port per line. Used on networks where mDNS peer discovery "
      "is blocked. Bare IP addresses are also accepted (port 17842 is assumed)."));
  gtk_label_set_line_wrap(GTK_LABEL(peers_desc), TRUE);
  gtk_label_set_xalign(GTK_LABEL(peers_desc), 0.0f);
  gtk_widget_set_margin_bottom(peers_desc, DT_PIXEL_APPLY_DPI(4));
  gtk_grid_attach(GTK_GRID(grid), peers_desc, 0, row++, 4, 1);

  d->peers_view = gtk_text_view_new();
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(d->peers_view), TRUE);
  gtk_widget_set_tooltip_text(d->peers_view,
    _("Static peer addresses from ~/.config/darktable/peers.txt"));
  GtkWidget *peers_scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(peers_scroll),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(peers_scroll),
                                              DT_PIXEL_APPLY_DPI(80));
  gtk_container_add(GTK_CONTAINER(peers_scroll), d->peers_view);
  gtk_widget_set_hexpand(peers_scroll, TRUE);
  gtk_grid_attach(GTK_GRID(grid), peers_scroll, 0, row++, 4, 1);

  // ---- load initial values -----------------------------------------------
  const gchar *cfg = g_get_user_config_dir();
  gchar *keys_path  = g_build_filename(cfg, "darktable", "peer.keys",  NULL);
  gchar *peers_path = g_build_filename(cfg, "darktable", "peers.txt",  NULL);
  _load_text_view(d->keys_view,  keys_path);
  _load_text_view(d->peers_view, peers_path);
  g_free(keys_path);
  g_free(peers_path);

  _refresh_status(d);
  _refresh_fingerprint(d);

  // ---- add to stack -------------------------------------------------------
  GtkWidget *main_scroll = dt_gui_scroll_wrap(main_box);
  GtkWidget *tab_box = dt_gui_vbox(main_scroll);

  gtk_stack_add_titled(GTK_STACK(stack), tab_box, "p2p", _("P2P sync"));

  g_object_set_data_full(G_OBJECT(tab_box), "prefs-p2p-data", d, g_free);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
