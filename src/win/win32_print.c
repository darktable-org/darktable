/*
    This file is part of darktable,
    Copyright (C) 2014-2026 darktable developers.

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
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0602

#include "common/darktable.h"
#include <stdbool.h>
#include <stdlib.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "common/file_location.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/pdf.h"
#include "control/jobs/control_jobs.h"
#include "common/cups_print.h"   // Shared print API types
#include "common/printing.h"

#include <windows.h>
#ifdef PaletteType
#undef PaletteType
#endif
#include <winspool.h>
#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <wingdi.h>
#include <wincodec.h>
#define INITGUID
#include <initguid.h>
#include <xpsprint.h>
#include <xpsobjectmodel.h>
#include <msopc.h>
#include <strsafe.h>
#include "win/win32_print.h"


// Forward declaration
typedef struct dt_prtctl_t {
  void (*cb)(dt_printer_info_t *, void *);
  void (*ready_cb)(dt_printer_info_t *, void *);
  void *user_data;

  dt_printer_info_t *current_printer;

  // New: reference count for async lifetime
  gint refs;
} dt_prtctl_t;

typedef struct dt_win_printer_caps_t {
    int phys_w;     // full sheet width in device units
    int phys_h;     // full sheet height
    int print_w;    // printable width
    int print_h;    // printable height
    int offset_x;   // left hardware margin
    int offset_y;   // top hardware margin
} dt_win_printer_caps_t;

typedef struct notify_ctx_t {
  dt_prtctl_t *pctl;             // refcounted owner (we only dec refs here)
  dt_printer_info_t *pinfo;      // not owned; lives in wrap
} notify_ctx_t;


static GList *discovered_printers = NULL; // holds dt_printer_info_t*

void dt_populate_hw_margins(HDC hdc, dt_printer_info_t *printer);

static int _fill_printer_details_job(dt_job_t *job);

typedef struct dt_printer_wrapper_t {
  dt_printer_info_t *pinfo;   // actual printer struct (UI-owned)
  gboolean details_valid;     // TRUE once dt_get_printer_info has run
} dt_printer_wrapper_t;

typedef struct {
  dt_printer_wrapper_t *wrap;             // wrapper with pinfo + details_valid
  void (*cb)(dt_printer_info_t *, void *); // UI callback
  void (*ready_cb)(dt_printer_info_t *pr, void *user_data);
  void *user_data;
  gboolean is_default;
  dt_prtctl_t *pctl;
} printer_job_params_t;

typedef struct
{
  dt_printer_info_t *pinfo;                // UI-owned printer struct
  void (*cb)(dt_printer_info_t *, void *); // UI callback
  void *user_data;                         // UI callback user data
} printer_ui_notify_t;

// Function pointer type matching StartXpsPrintJob's real signature from
// xpsprint.h. Resolved at runtime via LoadLibrary/GetProcAddress rather
// than linked, since MinGW-w64 doesn't ship an import library for
// xpsprint.dll.
typedef HRESULT (WINAPI *PFN_StartXpsPrintJob)(
    LPCWSTR printerName,
    LPCWSTR jobName,
    LPCWSTR outputFileName,
    HANDLE progressEvent,
    HANDLE completionEvent,
    UINT8 *printablePagesOn,
    UINT32 printablePagesOnCount,
    IXpsPrintJob **xpsPrintJob,
    IXpsPrintJobStream **documentStream,
    IXpsPrintJobStream **printTicketStream);

static PFN_StartXpsPrintJob pStartXpsPrintJob = NULL;
static HMODULE hXpsPrintDll = NULL;

// Resolves StartXpsPrintJob if not already done. 
static gboolean _win_xpsprint_ensure_loaded(void)
{
  if(pStartXpsPrintJob) return TRUE;

  if(!hXpsPrintDll)
    hXpsPrintDll = LoadLibraryW(L"xpsprint.dll");

  if(!hXpsPrintDll)
  {
    dt_control_log(_("could not load xpsprint.dll"));
    return FALSE;
  }

  pStartXpsPrintJob =
    (PFN_StartXpsPrintJob)GetProcAddress(hXpsPrintDll, "StartXpsPrintJob");

  if(!pStartXpsPrintJob)
    dt_control_log(_("xpsprint.dll is missing StartXpsPrintJob"));

  return pStartXpsPrintJob != NULL;
}

/* ----------------------------------------------------------------------------
   Debug logging
---------------------------------------------------------------------------- */

#define DBG_MARK(...) \
  do { \
    FILE *f = fopen("C:/temp/winprint_debug.log", "a"); \
    if(f) { \
      fprintf(f, "%s:%d: ", __FILE__, __LINE__); \
      fprintf(f, __VA_ARGS__); \
      fprintf(f, "\n"); \
      fclose(f); \
    } \
  } while(0)

////HELPERS//////

void dt_sync_print_settings_to_dm(DEVMODEW *dm, const dt_print_info_t *pinfo)
{
  dm->dmFields |= DM_ORIENTATION | DM_PAPERSIZE |DM_PAPERWIDTH | DM_PAPERLENGTH;
  dm->dmOrientation = pinfo->page.landscape ? DMORIENT_LANDSCAPE : DMORIENT_PORTRAIT;

  // Set paper size and dimensions. If the paper has a known DEVMODE ID, use that; otherwise, use custom dimensions.
  dm->dmFields |= DM_PAPERSIZE;
  dm->dmPaperSize = (pinfo->paper.dm_paper_id != 0) ? pinfo->paper.dm_paper_id : DMPAPER_USER;
  // Set custom paper dimensions in tenths of a millimeter, these should match those queried when the DEVMODE was first obtained.
  dm->dmFields |= DM_PAPERWIDTH | DM_PAPERLENGTH;
  dm->dmPaperWidth  = (short)(pinfo->paper.width  * 10.0f);
  dm->dmPaperLength = (short)(pinfo->paper.height * 10.0f);

  // Primarily for drivers that key primarily off paper name rather than ID or dimensions
  if(pinfo->paper.common_name[0])
  {
    dm->dmFields |= DM_FORMNAME;
    wchar_t *wname = g_utf8_to_utf16(pinfo->paper.common_name, -1, NULL, NULL, NULL);
    if(wname)
    {
      wcsncpy(dm->dmFormName, wname, CCHFORMNAME - 1);
      dm->dmFormName[CCHFORMNAME - 1] = L'\0';
      g_free(wname);
    }
  }
}

void free_dib(dt_win_dib_t *dib)
{
  if(!dib) return;

  if(dib->pixels)
  {
    g_free(dib->pixels);
    dib->pixels = NULL;
  }

  if(dib->bi)
  {
    g_free(dib->bi);
    dib->bi = NULL;
  }

  dib->stride   = 0;
  dib->width    = 0;
  dib->height   = 0;
  dib->top_down = false;
}

// Helper to sync cached DM to print settings
// win32_print.c
gboolean dt_win_sync_cached_dm_to_pinfo(dt_win32_print_ctx_t *ctx)
{
  if(!ctx || !ctx->cached_dm || !ctx->base) return FALSE;

  dt_print_info_t *pinfo = ctx->base;   // full print info
  DEVMODEW *dm = ctx->cached_dm;

  // Orientation → boolean
  if(dm->dmFields & DM_ORIENTATION)
    pinfo->page.landscape = (dm->dmOrientation == DMORIENT_LANDSCAPE) ? TRUE : FALSE;

  // Paper size (tenths of mm → mm)
  if(dm->dmFields & DM_PAPERWIDTH)
    pinfo->paper.width = dm->dmPaperWidth / 10.0;
  if(dm->dmFields & DM_PAPERLENGTH)
    pinfo->paper.height = dm->dmPaperLength / 10.0;
  if(dm->dmFields & DM_PAPERSIZE)
    pinfo->paper.dm_paper_id = dm->dmPaperSize;

  // Create DC to query resolution and hardware margins
  wchar_t *wprinter = g_utf8_to_utf16(pinfo->printer.name, -1, NULL, NULL, NULL);
  if(wprinter)
  {
    HDC hdc = CreateDCW(L"WINSPOOL", wprinter, NULL, dm);
    if(hdc)
    {
      // resolution + hw margins
      int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
      int dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
      pinfo->printer.resolution = dpiX; // assume square pixels

      int offX   = GetDeviceCaps(hdc, PHYSICALOFFSETX);
      int offY   = GetDeviceCaps(hdc, PHYSICALOFFSETY);
      int physW  = GetDeviceCaps(hdc, PHYSICALWIDTH);
      int physH  = GetDeviceCaps(hdc, PHYSICALHEIGHT);
      int horzRes= GetDeviceCaps(hdc, HORZRES);
      int vertRes= GetDeviceCaps(hdc, VERTRES);

      pinfo->printer.hw_margin_left   = 25.4 * offX / dpiX;
      pinfo->printer.hw_margin_top    = 25.4 * offY / dpiY;
      pinfo->printer.hw_margin_right  = 25.4 * (physW - (offX + horzRes)) / dpiX;
      pinfo->printer.hw_margin_bottom = 25.4 * (physH - (offY + vertRes)) / dpiY;

      DeleteDC(hdc);
    }
    g_free(wprinter);
  }

  return TRUE;
}
/* ----------------------------------------------------------------------------
   Printer discovery 
---------------------------------------------------------------------------- */

// Initialize print info (same as Linux)
void dt_init_print_info(dt_print_info_t *pinfo)
{
  memset(&pinfo->printer, 0, sizeof(dt_printer_info_t));
  memset(&pinfo->page, 0, sizeof(dt_page_setup_t));
  memset(&pinfo->paper, 0, sizeof(dt_paper_info_t));
  pinfo->printer.intent = DT_INTENT_PERCEPTUAL;
  pinfo->printer.is_turboprint = FALSE;
  *pinfo->printer.profile = '\0';
  pinfo->num_printers = 0;
      // Additional initialization specific to the Windows context
  // For example, you can set default margins or resolution here
  pinfo->printer.hw_margin_left = 0;
  pinfo->printer.hw_margin_top = 0;
  pinfo->printer.hw_margin_right = 0;
  pinfo->printer.hw_margin_bottom = 0;
  pinfo->printer.resolution = 300; // Default resolution in DPI
}

// Query printer info by name (Windows equivalent of cupsGetDest + PPD parsing)

void dt_get_printer_info(const char *printer_name_utf8, dt_printer_info_t *pinfo)
{
  char saved_name[MAX_NAME] = {0};
  if(printer_name_utf8 && printer_name_utf8[0] != '\0')
    g_strlcpy(saved_name, printer_name_utf8, MAX_NAME);

  memset(pinfo, 0, sizeof(*pinfo));
  g_strlcpy(pinfo->name, saved_name, MAX_NAME);

  pinfo->is_turboprint = FALSE; // TurboPrint is CUPS-only
  pinfo->resolution = 300;
  pinfo->hw_margin_left = pinfo->hw_margin_right =
  pinfo->hw_margin_top  = pinfo->hw_margin_bottom = 0.0;

  wchar_t *wprinter = g_utf8_to_utf16(printer_name_utf8, -1, NULL, NULL, NULL);
  if(!wprinter)
  {
    return;
  }

  HANDLE hPrinter = NULL;
  DWORD needed = 0, returned = 0;
  PRINTER_INFO_2W *pi2w = NULL;
  DEVMODEW *dmW = NULL;

  if(OpenPrinterW(wprinter, &hPrinter, NULL))
  {
    GetPrinterW(hPrinter, 2, NULL, 0, &needed);
    if(needed > 0)
    {
      pi2w = (PRINTER_INFO_2W *)malloc(needed);
      if(pi2w && GetPrinterW(hPrinter, 2, (LPBYTE)pi2w, needed, &returned))
      {
        dmW = (DEVMODEW *)pi2w->pDevMode;
        if(dmW && dmW->dmPrintQuality > 0)
          pinfo->resolution = dmW->dmPrintQuality;
      }
    }

    // --- NEW: reconcile with DeviceCapabilities ---
    int n = DeviceCapabilitiesW(wprinter, NULL, DC_ENUMRESOLUTIONS, NULL, dmW);
    if(n > 0)
    {
      POINT *resolutions = g_malloc0(sizeof(POINT) * n);
      DeviceCapabilitiesW(wprinter, NULL, DC_ENUMRESOLUTIONS,
                          (LPWSTR)resolutions, dmW);

      // Pick the first as default, or match DEVMODE if possible
      pinfo->resolution = resolutions[0].x;
      g_free(resolutions);
    }

    // Hardware margins via DC
    HDC hdc = CreateDCW(L"WINSPOOL", wprinter, NULL,
                        pi2w ? (DEVMODEW*)pi2w->pDevMode : NULL);
    if(hdc)
    {
      dt_populate_hw_margins(hdc, pinfo);
      DeleteDC(hdc);
    }

    if(pi2w) free(pi2w);
    ClosePrinter(hPrinter);
  }

  g_free(wprinter);

  // Debug output of what we filled in
  {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "dt_get_printer_info: name='%s' resolution=%d "
             "margins L=%.2f R=%.2f T=%.2f B=%.2f turboprint=%d",
             pinfo->name, pinfo->resolution,
             pinfo->hw_margin_left, pinfo->hw_margin_right,
             pinfo->hw_margin_top, pinfo->hw_margin_bottom,
             pinfo->is_turboprint);
  }
}

// Enumerate printers (replacement for cupsEnumDests / cupsGetDests)
// Global cancel flag
static volatile int _cancel = 0;

//Helper to filter out unwanted printers
static gboolean
is_unwanted_printer(const char *name)
{
  if(!name) return TRUE;

  // Case-insensitive checks for common virtual devices
  if(g_strrstr(name, "Fax")) return TRUE;
  if(g_strrstr(name, "OneNote")) return TRUE;
  if(g_strrstr(name, "XPS")) return TRUE;
  //if(g_strrstr(name, "PDF")) return TRUE;

  return FALSE;
}
static gboolean _notify_ui_printer_ready(gpointer user_data)
{
  notify_ctx_t *ready_ctx = (notify_ctx_t *)user_data;
  if(!ready_ctx) return FALSE;

  dt_prtctl_t *prtctl = ready_ctx->pctl;
  dt_printer_info_t *pinfo = ready_ctx->pinfo;

  const char *name = (pinfo && pinfo->name[0] != '\0') ? pinfo->name : "(null)";

  char buf[256];
  snprintf(buf, sizeof(buf), "notify ready for %s", name);

  if(prtctl && prtctl->ready_cb && pinfo && pinfo->name[0] != '\0') 
   {prtctl->ready_cb(pinfo, prtctl->user_data);}

  // Release this notify’s ref; free prtctl if last holder
  if(prtctl && g_atomic_int_dec_and_test(&prtctl->refs)) 
  {
    free(prtctl);
  }

  g_free(ready_ctx); // free the context (does not own pinfo)
  return FALSE; // one-shot idle
}



// Background job: enumerate installed printers and invoke callback
static int _detect_printers_callback(dt_job_t *job)
{
  dt_prtctl_t *pctl = dt_control_job_get_params(job);
  gboolean queued_any_default = FALSE;

  // Load saved default; NULL means not set.
  char *dt_default = dt_conf_get_string("plugins/lighttable/print/printer");
  const char *sync_target = (dt_default && dt_default[0] != '\0') ? dt_default : NULL;

  DWORD needed = 0, returned = 0;
  (void)EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2, NULL, 0, &needed, &returned);

  if(needed == 0) { g_free(dt_default); return 0; }

  BYTE *buffer = (BYTE *)malloc(needed);
  if(!buffer) { g_free(dt_default); return 0; }

  int success = 0;
  if(EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS,
                   NULL, 2, buffer, needed, &needed, &returned))
  {
    PRINTER_INFO_2W *pi2 = (PRINTER_INFO_2W *)buffer;

    for(DWORD i = 0; i < returned; i++)
    {
      gchar *utf8 = g_utf16_to_utf8(pi2[i].pPrinterName, -1, NULL, NULL, NULL);
      if(!utf8 || utf8[0] == '\0') { g_free(utf8); continue; }

      if(!is_unwanted_printer(utf8))
      {
        gboolean is_win_default = (pi2[i].Attributes & PRINTER_ATTRIBUTE_DEFAULT) ? TRUE : FALSE;
        gboolean is_dt_default = (sync_target && g_strcmp0(utf8, sync_target) == 0);

        // Create wrapper + info, append to list, and emit name via cb for combo
        dt_printer_info_t *pinfo = calloc(1, sizeof(*pinfo));
        g_strlcpy(pinfo->name, utf8, sizeof(pinfo->name));

        dt_printer_wrapper_t *wrap = calloc(1, sizeof(*wrap));
        wrap->pinfo = pinfo;
        wrap->details_valid = FALSE;

        discovered_printers = g_list_append(discovered_printers, wrap);
        if(pctl && pctl->cb) pctl->cb(pinfo, pctl->user_data);

        char buf[256];
        snprintf(buf, sizeof(buf), "discovered printer '%s'", pinfo->name);

        if(!darktable.control->cups_started)
        {
          darktable.control->cups_started = TRUE;
        }

        // Default selection priority: DT setting, then Windows, else fallback later
        if(is_dt_default || (!sync_target && is_win_default))
        {
          if(!sync_target && is_win_default)
          {
            dt_conf_set_string("plugins/lighttable/print/printer", pinfo->name);
          }

          printer_job_params_t *params = g_new0(printer_job_params_t, 1);
          params->wrap = wrap;
          params->pctl = pctl;
          params->is_default = TRUE;

          // Hold a ref for this detail job
          g_atomic_int_inc(&pctl->refs);

          dt_job_t *detail_job = dt_control_job_create(_fill_printer_details_job,
                                                       "fill default printer details");
          if(detail_job) 
          {
            dt_control_job_set_params(detail_job, params, g_free);
            dt_control_add_job(DT_JOB_QUEUE_SYSTEM_BG, detail_job);
            queued_any_default = TRUE;
          } 
          else 
          {
            g_free(params);
            if(g_atomic_int_dec_and_test(&pctl->refs)) free(pctl);
          }
        }
      }

      g_free(utf8);
    }

    success = 1;
  }

  free(buffer);

  // Fallback if nothing matched DT config or Windows default
  if(!queued_any_default && discovered_printers)
  {
    dt_printer_wrapper_t *wrap = (dt_printer_wrapper_t *)discovered_printers->data;

    printer_job_params_t *params = g_new0(printer_job_params_t, 1);
    params->wrap = wrap;
    params->pctl = pctl;
    params->is_default = TRUE;

    // Hold a ref for this fallback detail job
    g_atomic_int_inc(&pctl->refs);

    dt_job_t *detail_job = dt_control_job_create(_fill_printer_details_job,
                                                 "fill fallback printer details");
    if(detail_job) 
    {
      dt_control_job_set_params(detail_job, params, g_free);
      dt_control_add_job(DT_JOB_QUEUE_SYSTEM_BG, detail_job);
    } 
    else 
    {
      g_free(params);
      if(g_atomic_int_dec_and_test(&pctl->refs)) free(pctl);
    }
  }

  // Drop discovery’s initial ref; detail jobs and notifies hold their own refs.
  if(pctl && g_atomic_int_dec_and_test(&pctl->refs)) {
    free(pctl);
  }

  g_free(dt_default);
  return success;
}

static int _populate_remaining_printers_job(dt_job_t *job)
{
  dt_prtctl_t *pctl = dt_control_job_get_params(job);
  if(!pctl) return 0;

  char *dt_default = dt_conf_get_string("plugins/lighttable/print/printer");

  for(GList *l = discovered_printers; l; l = l->next)
  {
    dt_printer_wrapper_t *wrap = (dt_printer_wrapper_t *)l->data;
    if(!wrap || !wrap->pinfo) continue;

    // Skip the already-selected default
    if(dt_default && g_strcmp0(wrap->pinfo->name, dt_default) == 0)
      continue;

    printer_job_params_t *params = g_new0(printer_job_params_t, 1);
    params->wrap = wrap;
    params->pctl = pctl;
    params->is_default = FALSE;

    dt_job_t *detail_job = dt_control_job_create(_fill_printer_details_job,
                                                 "fill printer details");
    if(detail_job)
    {
      dt_control_job_set_params(detail_job, params, g_free);
      dt_control_add_job(DT_JOB_QUEUE_SYSTEM_BG, detail_job);
    }
    else 
    {
      g_free(params);
      if(g_atomic_int_dec_and_test(&pctl->refs)) free(pctl);
    }
  }

  g_free(dt_default);
  return 0;
}

// Background job: fill in printer details lazily
static int _fill_printer_details_job(dt_job_t *job)
{
  printer_job_params_t *params = dt_control_job_get_params(job);
  if(!params || !params->wrap || !params->wrap->pinfo) return 0;

  dt_printer_info_t *pinfo = params->wrap->pinfo;

  // Populate detailed capabilities (paper sizes, trays, etc.)
  dt_get_printer_info(pinfo->name, pinfo);
  params->wrap->details_valid = TRUE;

  // Persist default as soon as first ready details arrive (default or fallback)
  if(params->is_default)
  {
    dt_conf_set_string("plugins/lighttable/print/printer", pinfo->name);
  }

  // Notify UI in main loop; each notify holds a ref
  if(params->pctl && params->pctl->ready_cb)
  {
    notify_ctx_t *ready_ctx = g_new0(notify_ctx_t, 1);
    ready_ctx->pctl = params->pctl;
    ready_ctx->pinfo = pinfo;

    // Hold a ref for this notify; released in the idle callback
    g_atomic_int_inc(&params->pctl->refs);
    g_idle_add(_notify_ui_printer_ready, ready_ctx);
  }

  // After default/fallback ready, enqueue details for remaining printers
  if(params->is_default && params->pctl)
  {
    // Hold a ref for the "populate remaining printers" job
    g_atomic_int_inc(&params->pctl->refs);

    dt_job_t *others_job = dt_control_job_create(_populate_remaining_printers_job,
                                                 "populate remaining printers");
    if(others_job)
    {
      dt_control_job_set_params(others_job, params->pctl, NULL); // pctl owned by refcount
      dt_control_add_job(DT_JOB_QUEUE_SYSTEM_BG, others_job);
    }
    else
    {
      // Drop the ref if job creation failed
      if(g_atomic_int_dec_and_test(&params->pctl->refs))
        free(params->pctl);
    }
  }

  return 0;
}

// Public API: request discovery abort
// Cleanup Helper
void free_discovered_printers(void)
{
  for(GList *l = discovered_printers; l; l = l->next)
  {
    dt_printer_wrapper_t *wrap = (dt_printer_wrapper_t *)l->data;
    if(wrap)
    {
      free(wrap->pinfo);
      free(wrap);
    }
  }
  g_list_free(discovered_printers);
  discovered_printers = NULL;
}
//Printer loading helper

gboolean dt_printer_details_valid(const char *name)
{
  for(GList *l = discovered_printers; l; l = l->next)
  {
    dt_printer_wrapper_t *wrap = (dt_printer_wrapper_t *)l->data;
    if(wrap && wrap->pinfo && g_strcmp0(wrap->pinfo->name, name) == 0)
      return wrap->details_valid;
  }
  return FALSE;
}


void dt_printers_abort_discovery(void)
{
    _cancel = 1;
    free_discovered_printers();

}

void dt_win_printers_discovery(void (*cb)(dt_printer_info_t *pr, void *user_data),
                            void (*ready_cb)(dt_printer_info_t *pr, void *user_data),
                           void *user_data)
{
  // Reset cancel flag at the start of each discovery
  _cancel = 0;

  dt_job_t *job = dt_control_job_create(_detect_printers_callback,
                                        "detect connected printers");

  if(!job)
  {
    return;
  }
  
    dt_prtctl_t *prtctl = (dt_prtctl_t *)calloc(1, sizeof(dt_prtctl_t));
    prtctl->cb = cb;
    prtctl->ready_cb = ready_cb;
    prtctl->user_data = user_data;
    prtctl->refs = 1; // initial owner: the discovery pipeline

    dt_control_job_set_params(job, prtctl, NULL);
    dt_control_add_job(DT_JOB_QUEUE_SYSTEM_BG, job);
}


/* Papers via DeviceCapabilitiesA: sizes are in tenths of a millimeter */
// helper: check if paper already exists in list
static gboolean paper_exists(GList *papers, const char *name)
{
  if(strstr(name,"custom_") == name)
    return TRUE;

  for(GList *p = papers; p; p = g_list_next(p))
  {
    const dt_paper_info_t *pi = (dt_paper_info_t*)p->data;
    if(!strcmp(pi->name,name) || !strcmp(pi->common_name,name))
      return TRUE;
  }
  return FALSE;
}

dt_paper_info_t *dt_get_paper(GList *papers, const char *name)
{
  for(GList *p = papers; p; p = g_list_next(p))
  {
    dt_paper_info_t *pi = (dt_paper_info_t*)p->data;
    if(!strcmp(pi->name,name) || !strcmp(pi->common_name,name))
      return pi;
  }
  return NULL;
}

static gint sort_papers (gconstpointer p1, gconstpointer p2)
{
  const dt_paper_info_t *n1 = (const dt_paper_info_t *)p1;
  const dt_paper_info_t *n2 = (const dt_paper_info_t *)p2;
  const int l1 = strlen(n1->common_name);
  const int l2 = strlen(n2->common_name);
  return l1==l2 ? strcmp(n1->common_name, n2->common_name) : (l1 < l2 ? -1 : +1);
}

GList *dt_get_papers(const dt_printer_info_t *printer)
{
  GList *list = NULL;
  wchar_t *wprinter = g_utf8_to_utf16(printer->name, -1, NULL, NULL, NULL);
  if(!wprinter) return NULL;

  int papers_count = DeviceCapabilitiesW(wprinter, NULL, DC_PAPERS, NULL, NULL);
  if(papers_count <= 0)
  {
    dt_paper_info_t *p = calloc(1, sizeof(dt_paper_info_t));
    g_strlcpy(p->name, "Letter", MAX_NAME);
    g_strlcpy(p->common_name, "Letter", MAX_NAME);
    p->width  = 215.9;
    p->height = 279.4;
    list = g_list_append(list, p);
    g_free(wprinter);
    return list;
  }

  // Query sizes
  LPSIZE szList = (LPSIZE)calloc((size_t)papers_count, sizeof(SIZE));
  if(szList)
  {
    int sizes_count = DeviceCapabilitiesW(wprinter, NULL, DC_PAPERSIZE, (LPWSTR)szList, NULL);
    if(sizes_count > 0)
    {
      #ifdef DC_PAPERS
        WORD *paper_ids = (WORD *)calloc((size_t)papers_count, sizeof(WORD));
        int got_ids = paper_ids ? DeviceCapabilitiesW(wprinter, NULL, DC_PAPERS, (LPWSTR)paper_ids, NULL) : 0;
      #endif
      
      #ifdef DC_PAPERNAMES
        wchar_t *names = (wchar_t *)calloc((size_t)papers_count, 64 * sizeof(wchar_t));
        int got = names ? DeviceCapabilitiesW(wprinter, NULL, DC_PAPERNAMES, names, NULL) : 0;
      #endif
      for(int i = 0; i < sizes_count; i++)
      {
        dt_paper_info_t *p = calloc(1, sizeof(dt_paper_info_t));
        p->width  = szList[i].cx / 10.0;
        p->height = szList[i].cy / 10.0;

        // Try to get DEVMODE paper ID if available
        #ifdef DC_PAPERS
        p->dm_paper_id = (paper_ids && got_ids > i) ? paper_ids[i] : 0;
        #endif

        // Try to get names if available
        #ifdef DC_PAPERNAMES
        if(names && got > 0)
        {
          gchar *utf8 = g_utf16_to_utf8(&names[i * 64], -1, NULL, NULL, NULL);
          if(utf8 && *utf8)
          {
            g_strlcpy(p->name, utf8, MAX_NAME);
            g_strlcpy(p->common_name, utf8, MAX_NAME);
            g_free(utf8);
          }
        }
        #endif
    
        if(p->name[0] == '\0')
        {
          snprintf(p->name, MAX_NAME, "Paper %d", i+1);
          snprintf(p->common_name, MAX_NAME, "Paper %d", i+1);
        }

        if(!paper_exists(list, p->common_name))
          list = g_list_append(list, p);
        else
          free(p);
      }
      #ifdef DC_PAPERNAMES
      free(names);   // once, after the loop — not inside it
      #endif
      #ifdef DC_PAPERS
      free(paper_ids);
      #endif  
    }
    free(szList);
  }

  if(!list)
  {
    dt_paper_info_t *p = calloc(1, sizeof(dt_paper_info_t));
    g_strlcpy(p->name, "Letter", MAX_NAME);
    g_strlcpy(p->common_name, "Letter", MAX_NAME);
    p->width  = 215.9;
    p->height = 279.4;
    list = g_list_append(list, p);
  }

  g_free(wprinter);

  // Sort alphabetically by common_name
  list = g_list_sort(list, sort_papers);

  return list;
}

// Return a GList of gchar* media names, or NULL if none.
// Caller must free the list and each string.
GList *dt_get_media_type(const dt_printer_info_t *printer)
{
  (void)printer;
  
  return NULL;
}


GList *dt_get_quality_list(const char *printer_name_utf8)
{
  GList *list = NULL;

  // Always get the effective printer info first
  dt_printer_info_t info;
  dt_get_printer_info(printer_name_utf8, &info);

  wchar_t *wprinter = g_utf8_to_utf16(printer_name_utf8, -1, NULL, NULL, NULL);
  if(!wprinter) return NULL;

  int n = DeviceCapabilitiesW(wprinter, NULL, DC_ENUMRESOLUTIONS, NULL, NULL);

  if(n > 0)
  {
    POINT *resolutions = g_malloc0(sizeof(POINT) * n);
    DeviceCapabilitiesW(wprinter, NULL, DC_ENUMRESOLUTIONS,
                        (LPWSTR)resolutions, NULL);

    for(int i = 0; i < n; i++)
    {
      dt_win_quality_t *q = g_malloc(sizeof(*q));
      q->xdpi = resolutions[i].x;
      q->ydpi = resolutions[i].y;
      list = g_list_append(list, q);
    }
    g_free(resolutions);
  }
  else
  {
    // Fallback: just wrap the effective resolution from dt_get_printer_info
    dt_win_quality_t *q = g_malloc(sizeof(*q));
    q->xdpi = q->ydpi = info.resolution;
    list = g_list_append(list, q);
  }

  g_free(wprinter);
  return list;
}



dt_medium_info_t *dt_get_medium(GList *media, const char *name)
{
  return NULL; // not found
}

// //  PRINT HELPERS // //

// mirrors dt_pdf_pixel_to_point for XPS printing: convert pixels to device-independent units (1/96 inch)
static inline float _win_pixel_to_diu(float pixels, int resolution)
{
  return pixels / (float)resolution * 96.0f;
}
  
// mirrors dt_pdf_mm_to_point — for page-level dimensions, convert millimeters to device-independent units (1/96 inch)
static inline float _win_mm_to_diu(float mm)
{
  return mm / 25.4f * 96.0f;
}

static IXpsOMColorProfileResource *_win_build_color_profile_resource(IXpsOMObjectFactory *factory,
                                                                    const void *icc_data,
                                                                    size_t icc_size,
                                                                    const wchar_t *name)
{
  if(!factory || !icc_data || icc_size == 0 || !name)
    return NULL;

  IStream *profile_stream = NULL;
  IOpcPartUri *part_uri = NULL;
  IXpsOMColorProfileResource *profile_resource = NULL;

  HRESULT hr = CreateStreamOnHGlobal(NULL, TRUE, &profile_stream);
  if(FAILED(hr)) return NULL;

  ULONG written = 0;
  hr = profile_stream->lpVtbl->Write(profile_stream, icc_data, (ULONG)icc_size, &written);
  if(SUCCEEDED(hr))
  {
    LARGE_INTEGER zero = {0};
    profile_stream->lpVtbl->Seek(profile_stream, zero, STREAM_SEEK_SET, NULL);

    /* Log the requested part name for diagnostics (convert UTF-16 to UTF-8) */
    gchar *name_utf8 = g_utf16_to_utf8(name, -1, NULL, NULL, NULL);


    hr = factory->lpVtbl->CreatePartUri(factory, name, &part_uri);
    if(SUCCEEDED(hr) && part_uri)
    {
      hr = factory->lpVtbl->CreateColorProfileResource(factory,
                                                       profile_stream,
                                                       part_uri,
                                                       &profile_resource);
      part_uri->lpVtbl->Release(part_uri);
    }
    if(name_utf8) g_free(name_utf8);
  }

  profile_stream->lpVtbl->Release(profile_stream);
  return profile_resource;
} 
static HRESULT _win_encode_bitmap_to_png_stream(IWICImagingFactory *wic,
                                                IWICBitmapSource *bitmap,
                                                IStream **stream_out)
{
  if(!wic || !bitmap || !stream_out) return E_POINTER;
  *stream_out = NULL;

  IStream *stream = NULL;
  HRESULT hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
  if(FAILED(hr)) return hr;

  IWICBitmapEncoder *encoder = NULL;
  IWICBitmapFrameEncode *frame = NULL;

  hr = wic->lpVtbl->CreateEncoder(wic, &GUID_ContainerFormatPng, NULL, &encoder);
  if(SUCCEEDED(hr))
  {
    hr = encoder->lpVtbl->Initialize(encoder, stream, WICBitmapEncoderNoCache);
    if(SUCCEEDED(hr))
    {
      hr = encoder->lpVtbl->CreateNewFrame(encoder, &frame, NULL);
      if(SUCCEEDED(hr))
      {
        hr = frame->lpVtbl->Initialize(frame, NULL);
        if(SUCCEEDED(hr))
        {
          UINT w = 0, h = 0;
          bitmap->lpVtbl->GetSize(bitmap, &w, &h);

          WICPixelFormatGUID format = GUID_WICPixelFormat24bppRGB;
          hr = frame->lpVtbl->SetSize(frame, w, h);
          if(SUCCEEDED(hr))
            hr = frame->lpVtbl->SetPixelFormat(frame, &format);
          if(SUCCEEDED(hr))
            hr = frame->lpVtbl->WriteSource(frame, bitmap, NULL);
          if(SUCCEEDED(hr))
            hr = frame->lpVtbl->Commit(frame);
        }
        if(frame) frame->lpVtbl->Release(frame);
      }
      if(SUCCEEDED(hr))
        hr = encoder->lpVtbl->Commit(encoder);
    }
    if(encoder) encoder->lpVtbl->Release(encoder);
  }

  if(SUCCEEDED(hr))
  {
    LARGE_INTEGER zero = {0};
    stream->lpVtbl->Seek(stream, zero, STREAM_SEEK_SET, NULL);
    *stream_out = stream;
    return S_OK;
  }

  stream->lpVtbl->Release(stream);
  return hr;
}

// Wraps box->buf as a WIC bitmap, encodes it as PNG, and returns an XPS image
// resource ready to place on a page. Caller owns releasing the returned resource.
static IXpsOMImageResource *_win_build_image_resource(IXpsOMObjectFactory *factory,
                                                    const dt_image_box *box,
                                                    int image_index)
{
  if(!factory || !box || !box->buf || box->exp_width <= 0 || box->exp_height <= 0)
    return NULL;

  IWICImagingFactory *wic = NULL;
  IWICBitmap *bitmap = NULL;
  IXpsOMImageResource *resource = NULL;
  IStream *img_stream = NULL;
  IOpcPartUri *part_uri = NULL;

  HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory,
                                NULL,
                                CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory,
                                (void **)&wic);
  if(FAILED(hr) || !wic)
    return NULL;

  const UINT stride = (UINT)box->exp_width * 3;
  hr = wic->lpVtbl->CreateBitmapFromMemory(wic,
                                           box->exp_width,
                                           box->exp_height,
                                           &GUID_WICPixelFormat24bppRGB,
                                           stride,
                                           stride * box->exp_height,
                                           (BYTE *)box->buf,
                                           &bitmap);

  if(SUCCEEDED(hr) && bitmap)
  {
    hr = _win_encode_bitmap_to_png_stream(wic, (IWICBitmapSource *)bitmap, &img_stream);
    if(SUCCEEDED(hr) && img_stream)
    {
      wchar_t uri[64];
      hr = StringCchPrintfW(uri, G_N_ELEMENTS(uri), L"/Resources/image_%d.png", image_index);
      if(SUCCEEDED(hr))
      {
        hr = factory->lpVtbl->CreatePartUri(factory, uri, &part_uri);
        if(SUCCEEDED(hr) && part_uri)
        {
          hr = factory->lpVtbl->CreateImageResource(factory,
                                                    img_stream,
                                                    XPS_IMAGE_TYPE_PNG,
                                                    part_uri,
                                                    &resource);
          part_uri->lpVtbl->Release(part_uri);
        }
      }
      img_stream->lpVtbl->Release(img_stream);
    }
  }

  if(bitmap) bitmap->lpVtbl->Release(bitmap);
  if(wic) wic->lpVtbl->Release(wic);

  return resource;
}

// Adds one positioned image to an already-open XPS page.
static HRESULT _win_place_image_on_page(IXpsOMObjectFactory *factory,
                                       IXpsOMPage *page,
                                       IXpsOMImageResource *resource,
                                       IXpsOMColorProfileResource *profile_resource,
                                       const dt_image_box *box,
                                       int resolution,
                                       float page_height)
{
  if(!factory || !page || !resource || !box) return E_POINTER;

  const float x = _win_pixel_to_diu(box->print.x, resolution);
  const float y = _win_pixel_to_diu(box->print.y, resolution);
  const float w = _win_pixel_to_diu(box->print.width, resolution);
  const float h = _win_pixel_to_diu(box->print.height, resolution);

  IXpsOMPath *path = NULL;
  IXpsOMImageBrush *brush = NULL;
  IXpsOMGeometry *geom = NULL;
  IXpsOMGeometryFigure *figure = NULL;
  IXpsOMGeometryFigureCollection *figures = NULL;
  IXpsOMVisualCollection *visuals = NULL;

  XPS_RECT viewbox = { 0.0f, 0.0f, (float)box->exp_width, (float)box->exp_height };

  /* XPS page origin is top-left with Y increasing downward. The printing coordinates
     in box->print.y are computed with a bottom-left origin earlier, so convert
     to page/top-left coordinates by flipping relative to the page height. */
  float adj_y = page_height - (y + h);

  XPS_RECT viewport = { x, adj_y, w, h };

  HRESULT hr = factory->lpVtbl->CreateImageBrush(factory, resource, &viewbox, &viewport, &brush);
  if(FAILED(hr)) return hr;

  if(profile_resource)
  {
    HRESULT profile_hr = brush->lpVtbl->SetColorProfileResource(brush, profile_resource);
    if(FAILED(profile_hr))
    {
    // deliberately not fatal — worth seeing whether output looks right
    // even if this call fails, rather than aborting the whole page
    }
  }

  hr = factory->lpVtbl->CreatePath(factory, &path);
  if(FAILED(hr)) goto cleanup;

  hr = factory->lpVtbl->CreateGeometry(factory, &geom);
  if(FAILED(hr)) goto cleanup;

  XPS_POINT start = { x, adj_y };
  XPS_SEGMENT_TYPE seg_types[4] = {
    XPS_SEGMENT_TYPE_LINE,
    XPS_SEGMENT_TYPE_LINE,
    XPS_SEGMENT_TYPE_LINE,
    XPS_SEGMENT_TYPE_LINE
  };

  FLOAT seg_data[8] = {
    x + w, adj_y,
    x + w, adj_y + h,
    x,     adj_y + h,
    x,     adj_y
  };

WINBOOL seg_strokes[4] = { TRUE, TRUE, TRUE, TRUE };

hr = factory->lpVtbl->CreateGeometryFigure(factory, &start, &figure);
if(FAILED(hr)) goto cleanup;

hr = figure->lpVtbl->SetSegments(figure,
                                4,
                                8,
                                seg_types,
                                seg_data,
                                seg_strokes);
if(SUCCEEDED(hr)) 
{
  hr = figure->lpVtbl->SetIsClosed(figure, TRUE);
}
if(SUCCEEDED(hr))
{
  hr = figure->lpVtbl->SetIsFilled(figure, TRUE);
}

if(SUCCEEDED(hr))
{
  hr = geom->lpVtbl->GetFigures(geom, &figures);
  if(SUCCEEDED(hr) && figures)
    hr = figures->lpVtbl->Append(figures, figure);
}

if(SUCCEEDED(hr))
{
  hr = path->lpVtbl->SetGeometryLocal(path, geom);
  if(SUCCEEDED(hr))
    hr = path->lpVtbl->SetFillBrushLocal(path, (IXpsOMBrush *)brush);
  if(SUCCEEDED(hr))
  {
    hr = page->lpVtbl->GetVisuals(page, &visuals);
    if(SUCCEEDED(hr) && visuals)
      hr = visuals->lpVtbl->Append(visuals, (IXpsOMVisual *)path);
  }
}

cleanup:
  if(figures) figures->lpVtbl->Release(figures);
  if(figure) figure->lpVtbl->Release(figure);
  if(geom) geom->lpVtbl->Release(geom);
  if(brush) brush->lpVtbl->Release(brush);
  if(path) path->lpVtbl->Release(path);
  if(visuals) visuals->lpVtbl->Release(visuals);

  return hr;
}


/*______________________________________________________________________________________

PRINT JOB MANAGEMENT

______________________________________________________________________________________  */
bool dt_win_print_file(const dt_images_box *imgs,
                       const char *job_title,
                       const dt_print_info_t *pinfo,
                       const void *print_ticket_data,
                       size_t print_ticket_size,
                       void *icc_data, size_t icc_size,
                       gboolean is_color_device,
                       float width, float height)
{
  if(!imgs || imgs->count <= 0)
  {
    dt_control_log(_("no images to print on `%s'"), pinfo->printer.name);
    return false;
  }

  const float page_width  = _win_mm_to_diu(width);
  const float page_height = _win_mm_to_diu(height);

  if(!_win_xpsprint_ensure_loaded())
    return false;

  HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  bool co_initialized = SUCCEEDED(hr) || hr == S_FALSE;
  if(!co_initialized)
  {
    dt_control_log(_("could not initialize COM for printing"));
    return false;
  }

  bool ok = false;
  wchar_t *wprinter = g_utf8_to_utf16(pinfo->printer.name, -1, NULL, NULL, NULL);
  wchar_t *wtitle   = g_utf8_to_utf16(job_title, -1, NULL, NULL, NULL);

  IXpsPrintJob *xpsJob = NULL;
  IXpsPrintJobStream *docStream = NULL;
  IXpsPrintJobStream *ticketStream = NULL;
  HANDLE completionEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

  if(wprinter && wtitle && completionEvent && pStartXpsPrintJob)
  {
    hr = pStartXpsPrintJob(wprinter,
                           wtitle,
                           NULL,
                           NULL,
                           completionEvent,
                           NULL,
                           0,
                           &xpsJob,
                           &docStream,
                           &ticketStream);

    if(SUCCEEDED(hr))
    {
      if(ticketStream && print_ticket_data && print_ticket_size > 0)
      {
        ULONG bytes_written = 0;
        hr = ticketStream->lpVtbl->Write(ticketStream,
                                        print_ticket_data,
                                        (ULONG)print_ticket_size,
                                        &bytes_written);
      }

      IXpsOMObjectFactory *factory = NULL;
      hr = CoCreateInstance(&CLSID_XpsOMObjectFactory,
                            NULL,
                            CLSCTX_INPROC_SERVER,
                            &IID_IXpsOMObjectFactory,
                            (void **)&factory);

      if(SUCCEEDED(hr) && factory)
      {
        IStream *package_stream = NULL;
        hr = CreateStreamOnHGlobal(NULL, TRUE, &package_stream);

        IOpcPartUri *doc_seq_uri = NULL;
        hr = factory->lpVtbl->CreatePartUri(factory, L"/FixedDocumentSequence.fdseq", &doc_seq_uri);

        if(SUCCEEDED(hr) && package_stream)
        {
          IXpsOMPackageWriter *writer = NULL;

          hr = factory->lpVtbl->CreatePackageWriterOnStream(
                  factory,
                  (ISequentialStream *)package_stream,
                  FALSE,
                  XPS_INTERLEAVING_OFF,
                  doc_seq_uri,
                  NULL,
                  NULL,
                  NULL,
                  NULL,
                  &writer);

          if(SUCCEEDED(hr) && writer)
          {
            IXpsOMColorProfileResource *profile_resource = NULL;
            IOpcPartUri *doc_uri = NULL;
            hr = factory->lpVtbl->CreatePartUri(factory, L"/FixedDocument.fdoc", &doc_uri);

            writer->lpVtbl->StartNewDocument(writer, doc_uri, NULL, NULL, NULL, NULL);

            doc_seq_uri->lpVtbl->Release(doc_seq_uri);
            doc_uri->lpVtbl->Release(doc_uri);

            IXpsOMPage *page = NULL;

            XPS_SIZE page_size = { page_width, page_height };

            IOpcPartUri *page_uri = NULL;
            hr = factory->lpVtbl->CreatePartUri(factory, L"/Pages/1.fpage", &page_uri);
            
            if(SUCCEEDED(hr) && page_uri)
            {
            hr = factory->lpVtbl->CreatePage(factory, &page_size, L"en-US", page_uri, &page);

            page_uri->lpVtbl->Release(page_uri);
            }

            /* Create and add the color profile resource after the page part is available */
            profile_resource = NULL;
            if(icc_data && icc_size > 0 && is_color_device)
            {
              profile_resource = _win_build_color_profile_resource(factory, icc_data, icc_size, L"/Resources/ColorProfiles/OutputProfile.icc");
            }

            if(SUCCEEDED(hr) && page)
            {
//  Iterate to place the images on the page
              for(int i = 0; i < imgs->count; i++)
              {
                const dt_image_box *box = &imgs->box[i];
                IXpsOMImageResource *res = _win_build_image_resource(factory, box, i);
                if(res)
                {
                  _win_place_image_on_page(factory, page, res, profile_resource, box, pinfo->printer.resolution, page_size.height);
                  res->lpVtbl->Release(res);
                }
              }

              hr = writer->lpVtbl->AddPage(writer, page, &page_size, NULL, NULL, NULL, NULL);

              page->lpVtbl->Release(page);
              page = NULL;
            }

            hr = writer->lpVtbl->Close(writer);

            if(profile_resource)
            {
              profile_resource->lpVtbl->Release(profile_resource);
              profile_resource = NULL;
            }
            writer->lpVtbl->Release(writer);
            writer = NULL;

            if(SUCCEEDED(hr))
            {
              LARGE_INTEGER zero = {0};
              HRESULT seek_hr = package_stream->lpVtbl->Seek(package_stream, zero, STREAM_SEEK_SET, NULL);

              if(SUCCEEDED(seek_hr))
              {
                BYTE buffer[4096];

                for(;;)
                {
                  ULONG read = 0;
                  HRESULT read_hr = package_stream->lpVtbl->Read(package_stream, buffer, sizeof(buffer), &read);

                  if(FAILED(read_hr))
                  {
                    break;
                  }

                  if(read == 0)
                  {
                    break;
                  }

                  if(docStream)
                  {
                    ULONG written = 0;
                    HRESULT whr = docStream->lpVtbl->Write(docStream, buffer, read, &written);
                    if(FAILED(whr) || written != read)
                    {
                      break; 
                    }
                  }
                }
              }
            }
          }
          package_stream->lpVtbl->Release(package_stream);
        }

        factory->lpVtbl->Release(factory);
      }

      if(docStream) docStream->lpVtbl->Close(docStream);
      if(ticketStream) ticketStream->lpVtbl->Close(ticketStream);

      WaitForSingleObject(completionEvent, INFINITE);
      ok = true;
    }
    else
    {
      dt_control_log(_("could not start print job on `%s'"), pinfo->printer.name);
    }
  }

  if(xpsJob) xpsJob->lpVtbl->Release(xpsJob);
  if(completionEvent) CloseHandle(completionEvent);
  g_free(wprinter);
  g_free(wtitle);

  if(co_initialized) CoUninitialize();

  if(ok)
    dt_control_log(_("printing `%s' on `%s'"), job_title, pinfo->printer.name);
  else
    dt_control_log(_("printing failed on `%s'"), pinfo->printer.name);

  return ok;
}

// Fill in hardware margins (in mm) for the given printer DC
void dt_populate_hw_margins(HDC hdc, dt_printer_info_t *printer)
{
  if(!hdc || !printer) return;

  int horzRes = GetDeviceCaps(hdc, HORZRES);
  int vertRes = GetDeviceCaps(hdc, VERTRES);
  int physW   = GetDeviceCaps(hdc, PHYSICALWIDTH);
  int physH   = GetDeviceCaps(hdc, PHYSICALHEIGHT);
  int offX    = GetDeviceCaps(hdc, PHYSICALOFFSETX);
  int offY    = GetDeviceCaps(hdc, PHYSICALOFFSETY);

  bool borderless = (horzRes == physW && vertRes == physH && offX == 0 && offY == 0);

  if (borderless) {
    printer->hw_margin_left = printer->hw_margin_top =
    printer->hw_margin_right = printer->hw_margin_bottom = 0.0;
    offX = 0;
    offY = 0;

    return;
  }


  int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
  int dpiY = GetDeviceCaps(hdc, LOGPIXELSY);

  // Convert device units (pixels) to millimeters
  printer->hw_margin_left   = (offX * 25.4) / dpiX;
  printer->hw_margin_top    = (offY * 25.4) / dpiY;
  printer->hw_margin_right  = ((physW - horzRes - offX) * 25.4) / dpiX;
  printer->hw_margin_bottom = ((physH - vertRes - offY) * 25.4) / dpiY;
}

void dt_get_print_layout(const dt_print_info_t *prt,
                         const int32_t area_width, const int32_t area_height,
                         float *px, float *py, float *pwidth, float *pheight,
                         float *ax, float *ay, float *awidth, float *aheight,
                         gboolean *borderless)
{
  float pg_width  = prt->paper.width;
  float pg_height = prt->paper.height;

  float np_top    = prt->printer.hw_margin_top;
  float np_left   = prt->printer.hw_margin_left;
  float np_right  = prt->printer.hw_margin_right;
  float np_bottom = prt->printer.hw_margin_bottom;

  if(prt->page.landscape)
  {
      float tmp = pg_width; pg_width = pg_height; pg_height = tmp;
      tmp = np_top; np_top = np_right; np_right = np_bottom; np_bottom = np_left; np_left = tmp;
  }

  const float a_aspect  = (float)area_width / (float)area_height;
  const float pg_aspect = pg_width / pg_height;

  float p_bottom, p_right;

  if(a_aspect > pg_aspect)
  {
      *px = (area_width - (area_height * pg_aspect)) / 2.0f;
      *py = 0;
      p_bottom = area_height;
      p_right  = area_width - *px;
  }
  else
  {
      *px = 0;
      *py = (area_height - (area_width / pg_aspect)) / 2.0f;
      p_right  = area_width;
      p_bottom = area_height - *py;
  }

  *pwidth  = p_right - *px;
  *pheight = p_bottom - *py;

  // User margins
  const float border_top    = prt->page.margin_top;
  const float border_left   = prt->page.margin_left;
  const float border_right  = prt->page.margin_right;
  const float border_bottom = prt->page.margin_bottom;

  const float bx = *px + (border_left / pg_width) * (*pwidth);
  const float by = *py + (border_top / pg_height) * (p_bottom - *py);
  const float bb = p_bottom - (border_bottom / pg_height) * (*pheight);
  const float br = p_right - (border_right / pg_width) * (p_right - *px);

  *borderless = border_left   < np_left
              || border_right  < np_right
              || border_top    < np_top
              || border_bottom < np_bottom;

  *ax      = bx;
  *ay      = by;
  *awidth  = br - bx;
  *aheight = bb - by;

}




//constructor for the print settings context
dt_win32_print_ctx_t *dt_win32_print_ctx_new(dt_print_info_t *pinfo)
{
  dt_win32_print_ctx_t *settings_ctx = calloc(1, sizeof(*settings_ctx));

  if(!settings_ctx) return NULL;

  settings_ctx->base = pinfo;         // <-- inject here
  settings_ctx->settings_opened = FALSE;
  settings_ctx->cached_dm = NULL;
  settings_ctx->hPrinter = NULL;
  settings_ctx->is_color_device = TRUE; // default to color, will be updated later if needed

  if(!pinfo || !pinfo->printer.name[0]) 
  {
    return settings_ctx; // nothing more we can do
  }

  wchar_t *wprinter = g_utf8_to_utf16(pinfo->printer.name, -1, NULL, NULL, NULL);
  // open printer handle
  if(!OpenPrinterW(wprinter, &settings_ctx->hPrinter, NULL)) 
  {
    g_free(wprinter);
    return settings_ctx; // leave cached_dm NULL, caller can detect
  }
  settings_ctx->is_color_device = DeviceCapabilitiesW(wprinter, NULL, DC_COLORDEVICE, NULL, NULL) != 0;

  // query required DEVMODE size
  LONG needed = DocumentPropertiesW(NULL, settings_ctx->hPrinter,
                                    wprinter,
                                    NULL, NULL, 0);
  if(needed > 0) 
  {
    settings_ctx->cached_dm = (DEVMODEW *)malloc(needed);
    if(settings_ctx->cached_dm) 
    {
      LONG ret = DocumentPropertiesW(NULL, settings_ctx->hPrinter,
                                    wprinter,
                                    settings_ctx->cached_dm, NULL,
                                    DM_OUT_BUFFER);
      if(ret <= 0) 
      {
        free(settings_ctx->cached_dm);
        settings_ctx->cached_dm = NULL;
      } 
    }
  }
  g_free(wprinter);
  return settings_ctx;
}

// Opens the printer settings dialog and updates ctx->cached_dm.
// Returns TRUE if the user clicked OK, FALSE if they cancelled or on error.
BOOL dt_win_open_printer_settings(dt_win32_print_ctx_t *settings_ctx, HWND hwnd_owner)
{
  if(!settings_ctx || !settings_ctx->base ) 
  {
      return FALSE;
  }

  const char *utf8_name = settings_ctx->base->printer.name;
  if(!utf8_name || !*utf8_name) 
  {
  return FALSE;
  }

  // Convert UTF‑8 printer name from pinfo to wide string
  wchar_t *printer_name = g_utf8_to_utf16(utf8_name, -1, NULL, NULL, NULL);
  if(!printer_name) {
      return FALSE;
  }

  HANDLE hPrinter = NULL;
  if(!OpenPrinterW(printer_name, &hPrinter, NULL)) {
      g_free(printer_name);
      return FALSE;
  }

  // Query required DEVMODE size
  LONG dm_size = DocumentPropertiesW(hwnd_owner,
                                      hPrinter,
                                      printer_name,
                                      NULL, NULL, 0);
  if(dm_size < 0) {
      ClosePrinter(hPrinter);
      g_free(printer_name);
      return FALSE;
  }

  if(!settings_ctx->cached_dm) 
  {
    settings_ctx->cached_dm = (DEVMODEW*)malloc(dm_size);
    if(!settings_ctx->cached_dm) 
    {
        ClosePrinter(hPrinter);
        g_free(printer_name);
        return FALSE;
    }
    ZeroMemory(settings_ctx->cached_dm, dm_size);
  }


    dt_sync_print_settings_to_dm(settings_ctx->cached_dm, settings_ctx->base);

    // Show the driver’s property sheet
    LONG ret = DocumentPropertiesW(hwnd_owner,
                                   hPrinter,
                                   printer_name,
                                   settings_ctx->cached_dm,   // output
                                   settings_ctx->cached_dm,   // input
                                   DM_IN_BUFFER | DM_OUT_BUFFER | DM_IN_PROMPT);

    ClosePrinter(hPrinter);
    g_free(printer_name);

  if(ret == IDOK)
  {
    settings_ctx->settings_opened = TRUE;

    if(settings_ctx->cached_dm)
    {
      // One‑stop sync: DEVMODE → pinfo (orientation, paper, resolution, hw margins)
      dt_win_sync_cached_dm_to_pinfo(settings_ctx);

    }
    return TRUE;
  }
  else
  {
    return FALSE;
  }

}

// windows settings destructor
void dt_win32_print_ctx_free(dt_win32_print_ctx_t *ctx)
{
    if(!ctx) return;

    if(ctx->cached_dm)
        {free(ctx->cached_dm);
        ctx->cached_dm = NULL;}


    if(ctx->hPrinter)
        {ClosePrinter(ctx->hPrinter);
        ctx->hPrinter = NULL;}

    free(ctx);
}

/* modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
   vim: shiftwidth=2 expandtab tabstop=2 cindent
   kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
   clang-format on */