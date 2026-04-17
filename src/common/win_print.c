/*
    This file is part of darktable,
    Copyright (C) 2025 darktable developers.

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

/* Windows native print backend, implementing the same API surface
   as cups_print.c but using GDI / WinSpool instead of CUPS.
   Color management defaults to sRGB (see issue #19856).  */

#ifdef _WIN32

#include <windows.h>
#include <shellapi.h>
#include <winspool.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "common/file_location.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/pdf.h"
#include "control/jobs/control_jobs.h"
#include "cups_print.h"

typedef struct dt_prtctl_t
{
  void (*cb)(dt_printer_info_t *, void *);
  void *user_data;
} dt_prtctl_t;

/* helper: convert a UTF-16 wide string to a UTF-8 gchar*.
   The caller must g_free() the result. */
static gchar *_wchar_to_utf8(const wchar_t *wstr)
{
  if(!wstr) return g_strdup("");
  return g_utf16_to_utf8((const gunichar2 *)wstr, -1, NULL, NULL, NULL);
}

/* helper: convert a UTF-8 string to a newly-allocated wide string.
   The caller must g_free() the result. */
static wchar_t *_utf8_to_wchar(const char *utf8)
{
  if(!utf8) return NULL;
  return (wchar_t *)g_utf8_to_utf16(utf8, -1, NULL, NULL, NULL);
}

// initialize the pinfo structure
void dt_init_print_info(dt_print_info_t *pinfo)
{
  memset(&pinfo->printer, 0, sizeof(dt_printer_info_t));
  memset(&pinfo->page, 0, sizeof(dt_page_setup_t));
  memset(&pinfo->paper, 0, sizeof(dt_paper_info_t));
  pinfo->printer.intent = DT_INTENT_PERCEPTUAL;
  pinfo->printer.is_turboprint = FALSE;
  *pinfo->printer.profile = '\0';
  pinfo->num_printers = 0;
}

void dt_get_printer_info(const char *printer_name,
                         dt_printer_info_t *pinfo)
{
  g_strlcpy(pinfo->name, printer_name, MAX_NAME);
  pinfo->is_turboprint = FALSE;

  // default resolution
  pinfo->resolution = 300;

  // try to get hardware margins via a printer DC
  // Use "WINSPOOL" as driver name for proper printer DC creation
  wchar_t *wprinter = _utf8_to_wchar(printer_name);
  HDC hdc = CreateDCW(L"WINSPOOL", wprinter, NULL, NULL);
  g_free(wprinter);

  if(hdc)
  {
    // get physical page size and printable area (in device units)
    const int phys_w  = GetDeviceCaps(hdc, PHYSICALWIDTH);
    const int phys_h  = GetDeviceCaps(hdc, PHYSICALHEIGHT);
    const int ofs_x   = GetDeviceCaps(hdc, PHYSICALOFFSETX);
    const int ofs_y   = GetDeviceCaps(hdc, PHYSICALOFFSETY);
    const int print_w = GetDeviceCaps(hdc, HORZRES);
    const int print_h = GetDeviceCaps(hdc, VERTRES);
    const int dpi_x   = GetDeviceCaps(hdc, LOGPIXELSX);
    const int dpi_y   = GetDeviceCaps(hdc, LOGPIXELSY);

    if(dpi_x > 0 && dpi_y > 0)
    {
      pinfo->resolution = dpi_x;
      while(pinfo->resolution > 360)
        pinfo->resolution /= 2;

      // margins in mm (25.4 mm per inch)
      pinfo->hw_margin_left   = (ofs_x * 25.4) / dpi_x;
      pinfo->hw_margin_top    = (ofs_y * 25.4) / dpi_y;
      pinfo->hw_margin_right  = ((phys_w - ofs_x - print_w) * 25.4) / dpi_x;
      pinfo->hw_margin_bottom = ((phys_h - ofs_y - print_h) * 25.4) / dpi_y;
    }

    DeleteDC(hdc);
  }
}

static volatile int _cancel = 0;

static int _detect_printers_callback(dt_job_t *job)
{
  dt_prtctl_t *pctl = dt_control_job_get_params(job);

  DWORD needed = 0, returned = 0;

  // first call to find out how much memory we need
  EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS,
                NULL, 2, NULL, 0, &needed, &returned);

  if(needed == 0)
  {
    darktable.control->cups_started = TRUE;
    return 0;
  }

  BYTE *buffer = (BYTE *)g_malloc(needed);
  if(!buffer)
  {
    darktable.control->cups_started = TRUE;
    return 1;
  }

  if(EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS,
                   NULL, 2, buffer, needed, &needed, &returned))
  {
    PRINTER_INFO_2W *pi = (PRINTER_INFO_2W *)buffer;

    for(DWORD i = 0; i < returned; i++)
    {
      // check for cancellation
      if(_cancel) break;

      // skip printers that are paused, offline, or have errors
      const DWORD bad_status = PRINTER_STATUS_ERROR
                             | PRINTER_STATUS_PAUSED
                             | PRINTER_STATUS_OFFLINE
                             | PRINTER_STATUS_NOT_AVAILABLE;

      if(pi[i].Status & bad_status)
      {
        gchar *name_utf8 = _wchar_to_utf8(pi[i].pPrinterName);
        dt_print(DT_DEBUG_PRINT, "[print] skip printer %s (status=%lu)",
                 name_utf8, (unsigned long)pi[i].Status);
        g_free(name_utf8);
        continue;
      }

      gchar *name_utf8 = _wchar_to_utf8(pi[i].pPrinterName);

      dt_printer_info_t pr;
      memset(&pr, 0, sizeof(pr));
      dt_get_printer_info(name_utf8, &pr);
      if(pctl->cb) pctl->cb(&pr, pctl->user_data);
      dt_print(DT_DEBUG_PRINT, "[print] new printer %s found", name_utf8);

      g_free(name_utf8);
    }
  }

  g_free(buffer);
  darktable.control->cups_started = TRUE;
  return 0;
}

void dt_printers_abort_discovery(void)
{
  _cancel = 1;
}

void dt_printers_discovery(void (*cb)(dt_printer_info_t *pr, void *user_data),
                           void *user_data)
{
  _cancel = 0;

  // asynchronously checks for available printers
  dt_job_t *job = dt_control_job_create(&_detect_printers_callback, "detect connected printers");
  if(job)
  {
    dt_prtctl_t *prtctl = g_malloc0(sizeof(dt_prtctl_t));

    prtctl->cb = cb;
    prtctl->user_data = user_data;

    dt_control_job_set_params(job, prtctl, g_free);
    dt_control_add_job(DT_JOB_QUEUE_SYSTEM_BG, job);
  }
}

dt_paper_info_t *dt_get_paper(GList *papers,
                              const char *name)
{
  dt_paper_info_t *result = NULL;

  for(GList *p = papers; p; p = g_list_next(p))
  {
    dt_paper_info_t *pi = (dt_paper_info_t *)p->data;
    if(!strcmp(pi->name, name) || !strcmp(pi->common_name, name))
    {
      result = pi;
      break;
    }
  }
  return result;
}

static gint
sort_papers(gconstpointer p1, gconstpointer p2)
{
  const dt_paper_info_t *n1 = (dt_paper_info_t *)p1;
  const dt_paper_info_t *n2 = (dt_paper_info_t *)p2;
  const int l1 = strlen(n1->common_name);
  const int l2 = strlen(n2->common_name);
  return l1 == l2 ? strcmp(n1->common_name, n2->common_name) : (l1 < l2 ? -1 : +1);
}

GList *dt_get_papers(const dt_printer_info_t *printer)
{
  GList *result = NULL;

  wchar_t *wprinter = _utf8_to_wchar(printer->name);
  if(!wprinter) return NULL;

  HANDLE hPrinter = NULL;
  if(!OpenPrinterW(wprinter, &hPrinter, NULL))
  {
    g_free(wprinter);
    return NULL;
  }

  // get the number of paper names
  const DWORD count = DeviceCapabilitiesW(wprinter, NULL, DC_PAPERNAMES, NULL, NULL);
  if(count == 0 || count == (DWORD)-1)
  {
    ClosePrinter(hPrinter);
    g_free(wprinter);
    return NULL;
  }

  // paper names are fixed 64-wchar_t blocks
  wchar_t *names = (wchar_t *)g_malloc0(count * 64 * sizeof(wchar_t));
  // paper sizes in tenths of mm
  POINT *sizes = (POINT *)g_malloc0(count * sizeof(POINT));

  DeviceCapabilitiesW(wprinter, NULL, DC_PAPERNAMES, (LPWSTR)names, NULL);
  DeviceCapabilitiesW(wprinter, NULL, DC_PAPERSIZE, (LPWSTR)sizes, NULL);

  for(DWORD k = 0; k < count; k++)
  {
    const wchar_t *wname = names + k * 64;

    // skip papers with zero dimension
    if(sizes[k].x == 0 || sizes[k].y == 0)
      continue;

    gchar *paper_name_utf8 = _wchar_to_utf8(wname);

    dt_paper_info_t *paper = malloc(sizeof(dt_paper_info_t));
    g_strlcpy(paper->name, paper_name_utf8, MAX_NAME);
    g_strlcpy(paper->common_name, paper_name_utf8, MAX_NAME);
    paper->width  = (double)sizes[k].x / 10.0;  // convert tenths of mm to mm
    paper->height = (double)sizes[k].y / 10.0;

    result = g_list_append(result, paper);

    dt_print(DT_DEBUG_PRINT,
             "[print] new paper %4lu %6.2f x %6.2f (%s)",
             (unsigned long)k, paper->width, paper->height, paper->name);

    g_free(paper_name_utf8);
  }

  g_free(names);
  g_free(sizes);
  g_free(wprinter);
  ClosePrinter(hPrinter);

  result = g_list_sort(result, sort_papers);
  return result;
}

GList *dt_get_media_type(const dt_printer_info_t *printer)
{
  // Windows does not expose media type the same way CUPS/PPD does.
  // Return an empty list — the media type combo will be hidden or
  // show only "default".
  return NULL;
}

dt_medium_info_t *dt_get_medium(GList *media,
                                const char *name)
{
  dt_medium_info_t *result = NULL;

  for(GList *m = media; m; m = g_list_next(m))
  {
    dt_medium_info_t *mi = (dt_medium_info_t *)m->data;
    if(!strcmp(mi->name, name) || !strcmp(mi->common_name, name))
    {
      result = mi;
      break;
    }
  }
  return result;
}

void dt_print_file(const dt_imgid_t imgid,
                   const char *filename,
                   const char *job_title,
                   const dt_print_info_t *pinfo)
{
  // first for safety check that filename exists and is readable

  if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR))
  {
    dt_control_log(_("file `%s' to print not found for image %d on `%s'"),
                   filename, imgid, pinfo->printer.name);
    return;
  }

  // Use the "printto" verb so we can target the specific printer
  // selected in the darktable UI, not just the system default.
  // Syntax: ShellExecute(NULL, "printto", file, "PrinterName", ...)

  wchar_t *wfilename = _utf8_to_wchar(filename);
  wchar_t *wprinter  = _utf8_to_wchar(pinfo->printer.name);

  const HINSTANCE result = ShellExecuteW(NULL, L"printto", wfilename,
                                          wprinter, NULL, SW_HIDE);

  if((intptr_t)result <= 32)
  {
    dt_control_log(_("error while printing `%s' on `%s'"), job_title, pinfo->printer.name);
    dt_print(DT_DEBUG_ALWAYS,
             "[print] ShellExecuteW('printto') failed for %s on %s, code %d",
             filename, pinfo->printer.name, (int)(intptr_t)result);
  }
  else
    dt_control_log(_("printing `%s' on `%s'"), job_title, pinfo->printer.name);

  g_free(wfilename);
  g_free(wprinter);
}

void dt_get_print_layout(const dt_print_info_t *prt,
                         const int32_t area_width,
                         const int32_t area_height,
                         float *px,
                         float *py,
                         float *pwidth,
                         float *pheight,
                         float *ax,
                         float *ay,
                         float *awidth,
                         float *aheight,
                         gboolean *borderless)
{
  /* this is where the layout is done for the display and for the
     print too. So this routine is one of the most critical for the
     print circuitry. */

  // page w/h
  float pg_width  = prt->paper.width;
  float pg_height = prt->paper.height;

  /* here, width and height correspond to the area for the picture */

  // non-printable
  float np_top = prt->printer.hw_margin_top;
  float np_left = prt->printer.hw_margin_left;
  float np_right = prt->printer.hw_margin_right;
  float np_bottom = prt->printer.hw_margin_bottom;

  /* do some arrangements for the landscape mode. */

  if(prt->page.landscape)
  {
    float tmp = pg_width;
    pg_width = pg_height;
    pg_height = tmp;

    // rotate the non-printable margins
    tmp       = np_top;
    np_top    = np_right;
    np_right  = np_bottom;
    np_bottom = np_left;
    np_left   = tmp;
  }

  // the image area aspect
  const float a_aspect = (float)area_width / (float)area_height;

  // page aspect
  const float pg_aspect = pg_width / pg_height;

  // display page
  float p_bottom, p_right;

  if(a_aspect > pg_aspect)
  {
    *px = (area_width - (area_height * pg_aspect)) / 2.0f;
    *py = 0;
    p_bottom = area_height;
    p_right = area_width - *px;
  }
  else
  {
    *px = 0;
    *py = (area_height - (area_width / pg_aspect)) / 2.0f;
    p_right = area_width;
    p_bottom = area_height - *py;
  }

  *pwidth = p_right - *px;
  *pheight = p_bottom - *py;

  // page margins, note that we do not want to change those values for
  // the landscape mode.  these margins are those set by the user from
  // the GUI, and the top margin is *always* at the top of the screen.

  const float border_top = prt->page.margin_top;
  const float border_left = prt->page.margin_left;
  const float border_right = prt->page.margin_right;
  const float border_bottom = prt->page.margin_bottom;

  // display picture area, that is removing the non printable areas
  // and user's margins

  const float bx = *px + (border_left / pg_width) * (*pwidth);
  const float by = *py + (border_top / pg_height) * (*pheight);
  const float bb = p_bottom - (border_bottom / pg_height) * (*pheight);
  const float br = p_right - (border_right / pg_width) * (*pwidth);

  *borderless = border_left   < np_left
             || border_right  < np_right
             || border_top    < np_top
             || border_bottom < np_bottom;

  // now we have the printable area (ax, ay) -> (ax + awidth, ay + aheight)

  *ax      = bx;
  *ay      = by;
  *awidth  = br - bx;
  *aheight = bb - by;
}

#endif /* _WIN32 */

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
