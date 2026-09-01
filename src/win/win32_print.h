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

#pragma once

#include <glib.h>
#include <wincodec.h>
#include <xpsprint.h>

G_BEGIN_DECLS

#define PRINT_CONFIG_PREFIX "plugins/print/print/"
typedef struct dt_win32_print_ctx_t {
    dt_print_info_t *base;   // cross-platform pinfo
    HANDLE           hPrinter;
    DEVMODEW        *cached_dm;
    BOOL             settings_opened;
    HWND             hwnd_owner; // optional, for property sheet
    BOOL             is_color_device; // TRUE if printer is color-capable
    // room for future Windows-only fields
} dt_win32_print_ctx_t;

/*typedef struct dt_win_dib_t
{
  BITMAPINFO *bi;     // pointer to BITMAPINFO header
  uint8_t    *pixels; // pixel buffer
  int         stride; // bytes per row
  int         width;  // in pixels
  int         height; // in pixels
  gboolean    top_down;
  int         dpi_x;
  int         dpi_y;
} dt_win_dib_t;*/
/**
 * Check if printer details have been populated by the background job.
 *
 * @param name  Printer name (UTF‑8)
 * @return      TRUE if details are valid and ready, FALSE otherwise
 */
gboolean dt_printer_details_valid(const char *name);

void free_discovered_printers(void);

void dt_win_printers_discovery(void (*cb)(dt_printer_info_t *pr, void *user_data),
                           void (*ready_cb)(dt_printer_info_t *pr, void *user_data),
                           void *user_data);



bool dt_win_print_file(const dt_images_box *imgs,
                        const char *job_title,
                        const dt_print_info_t *pinfo,
                        const void *print_ticket_data,
                        size_t print_ticket_size, 
                        void *icc_data, size_t icc_size,
                        gboolean is_color_device,
                        float width, float height);

typedef enum {
  QUALITY_SRC_CAPS,
  QUALITY_SRC_DEVMODE_NUMERIC,
  QUALITY_SRC_DEVMODE_SYMBOLIC,
  QUALITY_SRC_DEFAULT
} dt_quality_source_t;


typedef struct dt_win_quality_t
{
  int xdpi;
  int ydpi;
  dt_quality_source_t source; // <-- add this
} dt_win_quality_t;

// void free_dib(dt_win_dib_t *dib);

GList *dt_get_quality_list(const char *printer_name);

BOOL dt_win_open_printer_settings(dt_win32_print_ctx_t *ctx, HWND hwnd_owner);

void dt_win32_print_ctx_free(dt_win32_print_ctx_t *ctx);

dt_win32_print_ctx_t *dt_win32_print_ctx_new(dt_print_info_t *pinfo);

gboolean dt_win_sync_cached_dm_to_pinfo(dt_win32_print_ctx_t *ctx);

void dt_sync_print_settings_to_dm(DEVMODEW *dm, const dt_print_info_t *pinfo);

G_END_DECLS
