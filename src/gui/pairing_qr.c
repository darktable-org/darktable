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

#define _POSIX_C_SOURCE 200809L

#include "gui/pairing_qr.h"
#include "control/conf.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <cairo.h>
#include <string.h>
#include <stdlib.h>

#ifdef HAVE_QRENCODE
#include <qrencode.h>
#endif

#ifdef HAVE_QRENCODE

#define QR_MARGIN 32
#define QR_CELL    8

typedef struct
{
  QRcode *qr;
} _qr_ctx_t;

static gboolean _on_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
  _qr_ctx_t *ctx = (_qr_ctx_t *)data;
  if(!ctx || !ctx->qr) return FALSE;

  QRcode *qr = ctx->qr;
  int n = qr->width;

  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_paint(cr);

  cairo_set_source_rgb(cr, 0, 0, 0);
  for(int y = 0; y < n; y++)
    for(int x = 0; x < n; x++)
      if(qr->data[y * n + x] & 1)
      {
        cairo_rectangle(cr,
                        QR_MARGIN + x * QR_CELL,
                        QR_MARGIN + y * QR_CELL,
                        QR_CELL, QR_CELL);
        cairo_fill(cr);
      }
  return FALSE;
}

static void _on_drawing_area_destroy(GtkWidget *w, gpointer data)
{
  _qr_ctx_t *ctx = (_qr_ctx_t *)data;
  if(ctx)
  {
    QRcode_free(ctx->qr);
    g_free(ctx);
  }
}

#endif // HAVE_QRENCODE

static gchar *_read_file_trimmed(const gchar *path)
{
  gchar *content = NULL;
  if(g_file_get_contents(path, &content, NULL, NULL) && content)
  {
    g_strchomp(content);
    return content;
  }
  g_free(content);
  return NULL;
}

static void _warn(GtkWidget *parent, const gchar *msg)
{
  GtkWidget *dlg = gtk_message_dialog_new(
      GTK_WINDOW(gtk_widget_get_toplevel(parent)),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "%s", msg);
  gtk_dialog_run(GTK_DIALOG(dlg));
  gtk_widget_destroy(dlg);
}

void dt_p2p_show_pairing_qr(GtkWidget *parent)
{
  const gchar *cfg = g_get_user_config_dir();

  // Passphrase from ~/.config/darktable/peer.pw (written by preferences_p2p.c).
  gchar *pw_path       = g_build_filename(cfg, "darktable", "peer.pw",          NULL);
  gchar *fp_path       = g_build_filename(cfg, "darktable", "peer.fingerprint",  NULL);
  gchar *peers_path    = g_build_filename(cfg, "darktable", "peers.txt",         NULL);
  // peer.localurl is written by the daemon on startup; contains this machine's
  // own HTTPS proxy URL so the mobile knows where to reach us.
  gchar *localurl_path = g_build_filename(cfg, "darktable", "peer.localurl",     NULL);

  gchar *passphrase   = _read_file_trimmed(pw_path);
  gchar *fingerprint  = _read_file_trimmed(fp_path);
  gchar *peers_raw    = _read_file_trimmed(peers_path);
  gchar *local_url    = _read_file_trimmed(localurl_path);

  g_free(pw_path);
  g_free(fp_path);
  g_free(peers_path);
  g_free(localurl_path);

  if(!passphrase || !*passphrase)
  {
    g_free(passphrase);
    g_free(fingerprint);
    g_free(peers_raw);
    g_free(local_url);
    _warn(parent, _("No passphrase configured.\n"
                    "Enter a passphrase and click \"Apply & restart daemon\" first."));
    return;
  }

  if(!fingerprint || !*fingerprint)
  {
    g_free(passphrase);
    g_free(fingerprint);
    g_free(peers_raw);
    g_free(local_url);
    _warn(parent, _("Fingerprint not yet available.\n"
                    "Start the daemon first (Apply & restart daemon)."));
    return;
  }

  // Build JSON peers array.  Own URL goes first so the mobile phone tries this
  // machine before any other static peers in the list.
  GString *peers_json = g_string_new("[");
  gboolean first = TRUE;

  // Prepend own URL (written by daemon to peer.localurl on startup).
  if(local_url && *local_url)
  {
    gchar *esc = g_strescape(local_url, NULL);
    g_string_append_printf(peers_json, "\"%s\"", esc);
    g_free(esc);
    first = FALSE;
  }
  g_free(local_url);

  if(peers_raw && *peers_raw)
  {
    gchar **lines = g_strsplit(peers_raw, "\n", -1);
    for(int i = 0; lines[i]; i++)
    {
      gchar *line = g_strstrip(lines[i]);
      if(!line[0] || line[0] == '#') continue;
      gchar *esc = g_strescape(line, NULL);
      if(!first) g_string_append_c(peers_json, ',');
      g_string_append_printf(peers_json, "\"%s\"", esc);
      g_free(esc);
      first = FALSE;
    }
    g_strfreev(lines);
  }
  g_string_append_c(peers_json, ']');
  g_free(peers_raw);

  gchar *pp_esc  = g_strescape(passphrase,  NULL);
  gchar *fpr_esc = g_strescape(fingerprint, NULL);
  g_free(passphrase);
  g_free(fingerprint);

  gchar *json = g_strdup_printf("{\"v\":1,\"pp\":\"%s\",\"fpr\":\"%s\",\"peers\":%s}",
                                pp_esc, fpr_esc, peers_json->str);
  g_free(pp_esc);
  g_free(fpr_esc);
  g_string_free(peers_json, TRUE);

  // URL-safe base64, no padding.
  gchar *b64 = g_base64_encode((const guchar *)json, strlen(json));
  g_free(json);
  for(gchar *p = b64; *p; p++)
  {
    if(*p == '+') *p = '-';
    else if(*p == '/') *p = '_';
    else if(*p == '=') { *p = '\0'; break; }
  }
  gchar *url = g_strdup_printf("darktable://pair?d=%s", b64);
  g_free(b64);

#ifdef HAVE_QRENCODE
  QRcode *qr = QRcode_encodeString(url, 0, QR_ECLEVEL_M, QR_MODE_8, 1);
  g_free(url);
  if(!qr)
  {
    _warn(parent, _("Failed to generate QR code."));
    return;
  }

  int px = qr->width * QR_CELL + 2 * QR_MARGIN;

  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      _("Pair with darktable mobile"),
      GTK_WINDOW(gtk_widget_get_toplevel(parent)),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      _("Close"), GTK_RESPONSE_CLOSE,
      NULL);

  GtkWidget *area = gtk_drawing_area_new();
  gtk_widget_set_size_request(area, px, px);

  _qr_ctx_t *ctx = g_new0(_qr_ctx_t, 1);
  ctx->qr = qr;
  g_signal_connect(area, "draw",    G_CALLBACK(_on_draw),                ctx);
  g_signal_connect(area, "destroy", G_CALLBACK(_on_drawing_area_destroy), ctx);

  GtkWidget *hint = gtk_label_new(
      _("Scan with darktable mobile to configure pairing.\n"
        "The QR code contains the passphrase and peer addresses."));
  gtk_label_set_justify(GTK_LABEL(hint), GTK_JUSTIFY_CENTER);
  gtk_widget_set_margin_bottom(hint, 8);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 16);
  gtk_box_pack_start(GTK_BOX(content), hint, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content), area, FALSE, FALSE, 0);
  gtk_widget_show_all(dialog);

  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
#else
  (void)url;
  _warn(parent, _("QR code generation is not available.\n"
                  "Build darktable with libqrencode to enable this feature."));
#endif
}
