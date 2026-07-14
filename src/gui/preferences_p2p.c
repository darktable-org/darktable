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
#include "gui/pairing_qr.h"
#include "common/darktable.h"
#include "common/p2p.h"
#include "control/conf.h"
#include "gui/gtk.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <string.h>

typedef struct _p2p_prefs_t
{
  GtkWidget *enable_check;
  GtkWidget *settings_box;     // container for everything except the enable row
  GtkWidget *passphrase_entry;
  GtkWidget *fingerprint_label;
  GtkWidget *address_label;    // own https://host:port
  GtkWidget *status_label;
  GtkWidget *apply_btn;
  GtkWidget *keys_view;
  GtkWidget *peers_view;
  GtkWidget *proxy_dir_btn;
  GtkWidget *import_dir_btn;
  GtkWidget *candidates_list; // GtkListBox showing untrusted peers
  GtkWidget *candidates_empty_label;
  GtkWidget *peer_status_list;         // known peers with live connection status
  GtkWidget *peer_status_empty_label;
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

  gchar *url_path = g_build_filename(cfg, "darktable", "peer.localurl", NULL);
  gchar *url = NULL;
  if(g_file_get_contents(url_path, &url, NULL, NULL) && url)
  {
    g_strchomp(url);
    gtk_label_set_text(GTK_LABEL(d->address_label), url);
  }
  else
    gtk_label_set_text(GTK_LABEL(d->address_label), _("(unavailable — start daemon first)"));
  g_free(url);
  g_free(url_path);
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
// peer status helpers
// ---------------------------------------------------------------------------

// Relative time string — caller must g_free().
static gchar *_relative_time(gint64 unix_ts)
{
  gint64 now  = (gint64)g_get_real_time() / 1000000;
  gint64 diff = now - unix_ts;
  if(diff < 0)              return g_strdup(_("just now"));
  if(diff < 60)             return g_strdup_printf(_("%llds ago"), (long long)diff);
  if(diff < 3600)           return g_strdup_printf(_("%lld min ago"), (long long)(diff / 60));
  if(diff < 86400)          return g_strdup_printf(_("%lld h ago"),   (long long)(diff / 3600));
  return                           g_strdup_printf(_("%lld d ago"),   (long long)(diff / 86400));
}

static void _refresh_peer_status(_p2p_prefs_t *d)
{
  // Clear old rows.
  GList *children = gtk_container_get_children(GTK_CONTAINER(d->peer_status_list));
  for(GList *l = children; l; l = l->next)
    gtk_widget_destroy(GTK_WIDGET(l->data));
  g_list_free(children);

  gchar *raw = dt_p2p_query_peer_status();
  gboolean any = FALSE;

  if(raw && *raw)
  {
    // Response is a socketMsg JSON line: {"type":"peer_status","data":[...]}
    JsonParser *parser = json_parser_new();
    if(json_parser_load_from_data(parser, raw, -1, NULL))
    {
      JsonNode   *root = json_parser_get_root(parser);
      JsonObject *msg  = root ? json_node_get_object(root) : NULL;
      JsonNode   *data_node = msg ? json_object_get_member(msg, "data") : NULL;
      if(data_node && JSON_NODE_TYPE(data_node) == JSON_NODE_ARRAY)
      {
        JsonArray *arr = json_node_get_array(data_node);
        for(guint i = 0; i < json_array_get_length(arr); i++)
        {
          JsonObject *obj = json_array_get_object_element(arr, i);
          const gchar *url     = json_object_get_string_member_with_default(obj, "url", "");
          gint64 last_seen     = (gint64)json_object_get_int_member_with_default(obj, "last_seen", 0);
          gint64 failure_count = (gint64)json_object_get_int_member_with_default(obj, "failure_count", 0);
          gboolean synced      = json_object_get_boolean_member_with_default(obj, "synced", FALSE);

          const gchar *status_icon;
          const gchar *status_color;
          if(synced)
          {
            status_icon  = "✓";
            status_color = "#4caf50"; // green
          }
          else if(failure_count > 0)
          {
            status_icon  = "✗";
            status_color = "#f44336"; // red
          }
          else
          {
            status_icon  = "?";
            status_color = "#9e9e9e"; // grey
          }

          gchar *time_str = _relative_time(last_seen);
          gchar *markup;
          if(failure_count > 0)
            markup = g_strdup_printf(
              "<b>%s</b>  <small>%s — %lld failure(s)</small>",
              url, time_str, (long long)failure_count);
          else
            markup = g_strdup_printf(
              "<b>%s</b>  <small>%s</small>", url, time_str);
          g_free(time_str);

          GtkWidget *row = gtk_list_box_row_new();
          gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);

          GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(8));
          gtk_widget_set_margin_start(box,  DT_PIXEL_APPLY_DPI(6));
          gtk_widget_set_margin_end(box,    DT_PIXEL_APPLY_DPI(6));
          gtk_widget_set_margin_top(box,    DT_PIXEL_APPLY_DPI(3));
          gtk_widget_set_margin_bottom(box, DT_PIXEL_APPLY_DPI(3));

          // Status indicator pill.
          gchar *pill_markup = g_strdup_printf(
            "<span foreground='%s' weight='bold'>%s</span>", status_color, status_icon);
          GtkWidget *icon_lbl = gtk_label_new(NULL);
          gtk_label_set_markup(GTK_LABEL(icon_lbl), pill_markup);
          g_free(pill_markup);

          GtkWidget *info_lbl = gtk_label_new(NULL);
          gtk_label_set_markup(GTK_LABEL(info_lbl), markup);
          gtk_label_set_ellipsize(GTK_LABEL(info_lbl), PANGO_ELLIPSIZE_END);
          gtk_label_set_xalign(GTK_LABEL(info_lbl), 0.0f);
          gtk_widget_set_hexpand(info_lbl, TRUE);
          g_free(markup);

          gtk_box_pack_start(GTK_BOX(box), icon_lbl, FALSE, FALSE, 0);
          gtk_box_pack_start(GTK_BOX(box), info_lbl, TRUE,  TRUE,  0);
          gtk_container_add(GTK_CONTAINER(row), box);
          gtk_container_add(GTK_CONTAINER(d->peer_status_list), row);
          any = TRUE;
        }
      }
    }
    g_object_unref(parser);
  }
  g_free(raw);

  gtk_widget_show_all(d->peer_status_list);
  gtk_widget_set_visible(d->peer_status_empty_label, !any);
}

static void _on_refresh_peer_status(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  _refresh_peer_status((_p2p_prefs_t *)user_data);
}

// ---------------------------------------------------------------------------
// callbacks
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// candidate peers helpers
// ---------------------------------------------------------------------------

static void _on_accept_peer(GtkButton *btn, gpointer user_data);

// Build one row in the candidates list box.
// Each row shows "url  fp_short" on the left and an Accept button on the right.
static GtkWidget *_make_candidate_row(const gchar *url, const gchar *fp,
                                      _p2p_prefs_t *d)
{
  GtkWidget *row = gtk_list_box_row_new();
  gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_start(box,  DT_PIXEL_APPLY_DPI(6));
  gtk_widget_set_margin_end(box,    DT_PIXEL_APPLY_DPI(6));
  gtk_widget_set_margin_top(box,    DT_PIXEL_APPLY_DPI(4));
  gtk_widget_set_margin_bottom(box, DT_PIXEL_APPLY_DPI(4));

  // Truncate fingerprint to first 16 chars for display.
  gchar fp_short[64];
  g_strlcpy(fp_short, fp ? fp : "", sizeof(fp_short));
  if(strlen(fp_short) > 16) { fp_short[16] = '\0'; }

  gchar *label_text = g_strdup_printf("<b>%s</b>\n<small>%s…</small>",
                                      url ? url : "?", fp_short);
  GtkWidget *info = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(info), label_text);
  gtk_label_set_ellipsize(GTK_LABEL(info), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(info), 0.0f);
  gtk_widget_set_hexpand(info, TRUE);
  gtk_widget_set_tooltip_text(info, fp);
  g_free(label_text);

  GtkWidget *accept_btn = gtk_button_new_with_label(_("Accept"));
  gtk_widget_set_tooltip_text(accept_btn,
    _("Trust this peer's TLS certificate and start syncing with them.\n"
      "Their fingerprint is appended to ~/.config/darktable/peer.keys."));
  g_object_set_data_full(G_OBJECT(accept_btn), "fp", g_strdup(fp), g_free);
  g_object_set_data(G_OBJECT(accept_btn), "row",  row);
  g_object_set_data(G_OBJECT(accept_btn), "prefs", d);
  g_signal_connect(accept_btn, "clicked", G_CALLBACK(_on_accept_peer), NULL);

  gtk_box_pack_start(GTK_BOX(box), info,       TRUE,  TRUE,  0);
  gtk_box_pack_start(GTK_BOX(box), accept_btn, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(row), box);
  return row;
}

static void _refresh_candidates(_p2p_prefs_t *d);

static void _on_accept_peer(GtkButton *btn, gpointer user_data)
{
  (void)user_data;
  const gchar *fp  = g_object_get_data(G_OBJECT(btn), "fp");
  GtkWidget   *row = g_object_get_data(G_OBJECT(btn), "row");
  _p2p_prefs_t *d  = g_object_get_data(G_OBJECT(btn), "prefs");

  if(fp && *fp)
    dt_p2p_accept_peer(fp);

  // Remove this row immediately so the user sees feedback before a refresh.
  if(row)
    gtk_widget_destroy(row);

  // Hide the list if now empty, show the placeholder.
  if(d)
    _refresh_candidates(d);
}

static void _refresh_candidates(_p2p_prefs_t *d)
{
  // Clear existing rows.
  GList *children = gtk_container_get_children(GTK_CONTAINER(d->candidates_list));
  for(GList *l = children; l; l = l->next)
    gtk_widget_destroy(GTK_WIDGET(l->data));
  g_list_free(children);

  // Read peer.candidates JSON written by the daemon.
  const gchar *cfg = g_get_user_config_dir();
  gchar *path = g_build_filename(cfg, "darktable", "peer.candidates", NULL);
  gchar *content = NULL;
  g_file_get_contents(path, &content, NULL, NULL);
  g_free(path);

  gboolean any = FALSE;
  if(content && *content)
  {
    JsonParser *parser = json_parser_new();
    if(json_parser_load_from_data(parser, content, -1, NULL))
    {
      JsonNode *root = json_parser_get_root(parser);
      if(root && JSON_NODE_TYPE(root) == JSON_NODE_ARRAY)
      {
        JsonArray *arr = json_node_get_array(root);
        for(guint i = 0; i < json_array_get_length(arr); i++)
        {
          JsonObject *obj = json_array_get_object_element(arr, i);
          const gchar *url = json_object_get_string_member_with_default(obj, "url", "");
          const gchar *fp  = json_object_get_string_member_with_default(obj, "fingerprint", "");
          if(!url || !*url || !fp || !*fp) continue;

          GtkWidget *row = _make_candidate_row(url, fp, d);
          gtk_container_add(GTK_CONTAINER(d->candidates_list), row);
          any = TRUE;
        }
      }
    }
    g_object_unref(parser);
  }
  g_free(content);

  gtk_widget_show_all(d->candidates_list);
  gtk_widget_set_visible(d->candidates_empty_label, !any);
}

static void _on_refresh_candidates(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  _refresh_candidates((_p2p_prefs_t *)user_data);
}

static void _on_enable_toggled(GtkToggleButton *btn, gpointer user_data)
{
  _p2p_prefs_t *d = (_p2p_prefs_t *)user_data;
  const gboolean enabled = gtk_toggle_button_get_active(btn);
  dt_conf_set_bool("plugins/p2p/enabled", enabled);
  gtk_widget_set_sensitive(d->settings_box, enabled);

  if(enabled)
    dt_p2p_restart();
  else
    dt_p2p_cleanup();

  _refresh_status(d);
  _refresh_fingerprint(d);
}

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

static void _on_show_pairing_qr(GtkButton *btn, gpointer user_data)
{
  (void)user_data;
  dt_p2p_show_pairing_qr(GTK_WIDGET(btn));
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

  // ---- enable row (always visible, outside the sensitive group) -----------
  const gboolean enabled = dt_conf_get_bool("plugins/p2p/enabled");
  d->enable_check = gtk_check_button_new_with_label(_("Enable P2P sync"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->enable_check), enabled);
  gtk_widget_set_tooltip_text(d->enable_check,
    _("Start the dt-p2p-daemon background process and enable all peer-to-peer "
      "sync features. Uncheck to stop the daemon and disable all P2P activity."));
  gtk_widget_set_margin_start(d->enable_check, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_top(d->enable_check,   DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_bottom(d->enable_check, DT_PIXEL_APPLY_DPI(4));
  dt_gui_box_add(main_box, d->enable_check);

  // ---- settings box: everything else (greyed when disabled) ---------------
  d->settings_box = dt_gui_vbox();
  gtk_widget_set_sensitive(d->settings_box, enabled);
  dt_gui_box_add(main_box, d->settings_box);

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_start(grid, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_end(grid,   DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_top(grid,   DT_PIXEL_APPLY_DPI(4));
  dt_gui_box_add(d->settings_box, grid);

  // Connect toggle after settings_box exists.
  g_signal_connect(d->enable_check, "toggled", G_CALLBACK(_on_enable_toggled), d);

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

  GtkWidget *qr_btn = gtk_button_new_with_label(_("pair QR"));
  gtk_widget_set_tooltip_text(qr_btn,
    _("Show a QR code containing the passphrase and peer addresses.\n"
      "Scan it with darktable mobile to configure sync pairing."));
  g_signal_connect(qr_btn, "clicked", G_CALLBACK(_on_show_pairing_qr), NULL);

  GtkWidget *fp_box = dt_gui_hbox(d->fingerprint_label, copy_btn, qr_btn);
  gtk_widget_set_hexpand(fp_box, TRUE);
  GtkWidget *fp_label = gtk_label_new(_("fingerprint"));
  gtk_widget_set_halign(fp_label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), fp_label, 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), fp_box,   2, row++, 2, 1);

  // Address row — own https://host:port so the user knows what to add to peers.txt
  // on another machine that can't discover this host via mDNS.
  d->address_label = gtk_label_new("");
  gtk_label_set_selectable(GTK_LABEL(d->address_label), TRUE);
  gtk_label_set_ellipsize(GTK_LABEL(d->address_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_hexpand(d->address_label, TRUE);
  gtk_widget_set_halign(d->address_label, GTK_ALIGN_START);
  gtk_widget_set_tooltip_text(d->address_label,
    _("This machine's P2P sync address.\n"
      "Add it to peers.txt on other machines that cannot discover this host via mDNS."));

  GtkWidget *copy_addr_btn = gtk_button_new_with_label(_("copy"));
  gtk_widget_set_tooltip_text(copy_addr_btn, _("Copy address to clipboard"));
  g_signal_connect(copy_addr_btn, "clicked",
                   G_CALLBACK(_on_copy_fingerprint), d->address_label);

  GtkWidget *addr_box = dt_gui_hbox(d->address_label, copy_addr_btn);
  gtk_widget_set_hexpand(addr_box, TRUE);
  GtkWidget *addr_label = gtk_label_new(_("address"));
  gtk_widget_set_halign(addr_label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), addr_label, 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), addr_box,   2, row++, 2, 1);

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

  // ---- peer connection status section -------------------------------------
  _add_section(grid, &row, _("peer connection status"));

  d->peer_status_empty_label = gtk_label_new(_("No known peers yet."));
  gtk_widget_set_halign(d->peer_status_empty_label, GTK_ALIGN_START);
  gtk_widget_set_margin_start(d->peer_status_empty_label, DT_PIXEL_APPLY_DPI(4));

  GtkWidget *ps_refresh_btn = gtk_button_new_with_label(_("Refresh"));
  gtk_widget_set_tooltip_text(ps_refresh_btn,
    _("Query the running daemon for live peer connection status."));
  g_signal_connect(ps_refresh_btn, "clicked",
                   G_CALLBACK(_on_refresh_peer_status), d);

  GtkWidget *ps_refresh_row = dt_gui_hbox(d->peer_status_empty_label);
  gtk_box_pack_end(GTK_BOX(ps_refresh_row), ps_refresh_btn, FALSE, FALSE, 0);
  gtk_grid_attach(GTK_GRID(grid), ps_refresh_row, 0, row++, 4, 1);

  d->peer_status_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(d->peer_status_list), GTK_SELECTION_NONE);
  gtk_grid_attach(GTK_GRID(grid), d->peer_status_list, 0, row++, 4, 1);

  // ---- reachable peers section --------------------------------------------
  _add_section(grid, &row, _("reachable peers"));

  GtkWidget *cand_desc = gtk_label_new(
    _("Peers discovered on your network whose TLS fingerprint is not yet trusted. "
      "Click Accept to add their key and start syncing."));
  gtk_label_set_line_wrap(GTK_LABEL(cand_desc), TRUE);
  gtk_label_set_xalign(GTK_LABEL(cand_desc), 0.0f);
  gtk_widget_set_margin_bottom(cand_desc, DT_PIXEL_APPLY_DPI(4));
  gtk_grid_attach(GTK_GRID(grid), cand_desc, 0, row++, 4, 1);

  d->candidates_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(d->candidates_list), GTK_SELECTION_NONE);

  d->candidates_empty_label = gtk_label_new(_("No untrusted peers seen yet."));
  gtk_widget_set_halign(d->candidates_empty_label, GTK_ALIGN_START);
  gtk_widget_set_margin_start(d->candidates_empty_label, DT_PIXEL_APPLY_DPI(4));

  GtkWidget *refresh_btn = gtk_button_new_with_label(_("Refresh"));
  gtk_widget_set_tooltip_text(refresh_btn,
    _("Re-read the candidates file written by the daemon."));
  g_signal_connect(refresh_btn, "clicked",
                   G_CALLBACK(_on_refresh_candidates), d);

  GtkWidget *refresh_row = dt_gui_hbox(d->candidates_empty_label);
  gtk_box_pack_end(GTK_BOX(refresh_row), refresh_btn, FALSE, FALSE, 0);
  gtk_grid_attach(GTK_GRID(grid), refresh_row, 0, row++, 4, 1);

  gtk_grid_attach(GTK_GRID(grid), d->candidates_list, 0, row++, 4, 1);

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

  // ---- load initial peer status and candidate peers -----------------------
  _refresh_peer_status(d);
  _refresh_candidates(d);

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
