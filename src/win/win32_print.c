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
#include <xpsprint.h>
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
// xpsprint.dll — see CMake notes from earlier in this project.
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

// Resolves StartXpsPrintJob if not already done. Safe to call repeatedly —
// a no-op once resolved. LoadLibrary/GetProcAddress are individually
// thread-safe Win32 calls, so even if two print jobs somehow raced into
// this, worst case is a harmless redundant LoadLibrary refcount bump.
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
static void dt_sync_orientation(DEVMODEW *dm, const dt_page_setup_t *page)
{
  dm->dmFields |= DM_ORIENTATION;
  dm->dmOrientation = page->landscape ? DMORIENT_LANDSCAPE : DMORIENT_PORTRAIT;
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

  DBG_MARK("synced pinfo: orient=%d paper=%.1fx%.1fmm res=%d hw_mm: L=%.2f T=%.2f R=%.2f B=%.2f",
           pinfo->page.landscape,
           pinfo->paper.width, pinfo->paper.height,
           pinfo->printer.resolution,
           pinfo->printer.hw_margin_left,
           pinfo->printer.hw_margin_top,
           pinfo->printer.hw_margin_right,
           pinfo->printer.hw_margin_bottom);

  return TRUE;
}
/* ----------------------------------------------------------------------------
   Printer discovery 
---------------------------------------------------------------------------- */

// Initialize print info (same as your Linux version)
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
  // DBG_MARK("enter dt_get_printer_info");

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
    // DBG_MARK("exit dt_get_printer_info (no wprinter)");
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
    // DBG_MARK(buf);
  }

  // DBG_MARK("exit dt_get_printer_info");
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
  // DBG_MARK(buf);

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
  // DBG_MARK("discovery job started");

  dt_prtctl_t *pctl = dt_control_job_get_params(job);
  gboolean queued_any_default = FALSE;

  // Load saved default; NULL means not set.
  char *dt_default = dt_conf_get_string("plugins/lighttable/print/printer");
  const char *sync_target = (dt_default && dt_default[0] != '\0') ? dt_default : NULL;

  DWORD needed = 0, returned = 0;
  (void)EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2, NULL, 0, &needed, &returned);
  // DBG_MARK("EnumPrinters initial call complete");

  if(needed == 0) { g_free(dt_default); return 0; }

  BYTE *buffer = (BYTE *)malloc(needed);
  if(!buffer) { g_free(dt_default); return 0; }

  int success = 0;
  if(EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS,
                   NULL, 2, buffer, needed, &needed, &returned))
  {
    // DBG_MARK("EnumPrinters returned printers");
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
        // DBG_MARK(buf);

        if(!darktable.control->cups_started)
        {
          darktable.control->cups_started = TRUE;
          // DBG_MARK("first usable printer found, cups_started set");
        }

        // Default selection priority: DT setting, then Windows, else fallback later
        if(is_dt_default || (!sync_target && is_win_default))
        {
          // DBG_MARK("default printer branch taken");

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
      // DBG_MARK("queued detail job for FALLBACK first printer");
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
  // DBG_MARK("discovery job finished (names queued, default detail async)");
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
      // DBG_MARK("queued detail job for non-default printer");
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
  // DBG_MARK("detail job started");

  // Populate detailed capabilities (paper sizes, trays, etc.)
  dt_get_printer_info(pinfo->name, pinfo);
  params->wrap->details_valid = TRUE;

  // Persist default as soon as first ready details arrive (default or fallback)
  if(params->is_default)
  {
    dt_conf_set_string("plugins/lighttable/print/printer", pinfo->name);
    // DBG_MARK("updated default printer to first ready");
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
    // DBG_MARK("queued notify");
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
      // DBG_MARK("queued job to populate remaining printers");
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
      // DBG_MARK("abort discovery called");

    _cancel = 1;
    free_discovered_printers();

}

void dt_win_printers_discovery(void (*cb)(dt_printer_info_t *pr, void *user_data),
                            void (*ready_cb)(dt_printer_info_t *pr, void *user_data),
                           void *user_data)
{
  // DBG_MARK("starting discovery");

  // Reset cancel flag at the start of each discovery
  _cancel = 0;

  dt_job_t *job = dt_control_job_create(_detect_printers_callback,
                                        "detect connected printers");

  if(!job)
  {
    // DBG_MARK("failed to create discovery job");
    return;
  }
  
    dt_prtctl_t *prtctl = (dt_prtctl_t *)calloc(1, sizeof(dt_prtctl_t));
    prtctl->cb = cb;
    prtctl->ready_cb = ready_cb;
    prtctl->user_data = user_data;
    prtctl->refs = 1; // initial owner: the discovery pipeline

    dt_control_job_set_params(job, prtctl, NULL);
    dt_control_add_job(DT_JOB_QUEUE_SYSTEM_BG, job);
    // DBG_MARK("discovery job queued");
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
      for(int i = 0; i < sizes_count; i++)
      {
        dt_paper_info_t *p = calloc(1, sizeof(dt_paper_info_t));
        p->width  = szList[i].cx / 10.0;
        p->height = szList[i].cy / 10.0;

        // Try to get names if available
        #ifdef DC_PAPERNAMES
        wchar_t *names = (wchar_t *)calloc((size_t)papers_count, 64 * sizeof(wchar_t));
        if(names)
        {
          int got = DeviceCapabilitiesW(wprinter, NULL, DC_PAPERNAMES, names, NULL);
          if(got > i)
          {
            gchar *utf8 = g_utf16_to_utf8(&names[i * 64], -1, NULL, NULL, NULL);
            if(utf8 && *utf8)
            {
              g_strlcpy(p->name, utf8, MAX_NAME);
              g_strlcpy(p->common_name, utf8, MAX_NAME);
              g_free(utf8);
            }
          }
          free(names);
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
  // DBG_MARK("DeviceCapabilities returned %d resolutions for %s", n, printer_name_utf8);

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
      // DBG_MARK("found resolution %d: %d x %d dpi", i, q->xdpi, q->ydpi);
    }
    g_free(resolutions);
  }
  else
  {
    // Fallback: just wrap the effective resolution from dt_get_printer_info
    dt_win_quality_t *q = g_malloc(sizeof(*q));
    q->xdpi = q->ydpi = info.resolution;
    list = g_list_append(list, q);
    // DBG_MARK("fallback to single resolution: %d dpi", q->xdpi);
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
        hr = encoder->lpVtbl->Commit(encoder, NULL);
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

// Wraps box->buf as a WIC bitmap, encodes it as JPEG, and returns an XPS image
// resource ready to place on a page. Caller owns releasing the returned resource.
static IXpsOMImageResource *_win_build_image_resource(IXpsOMObjectFactory *factory,
                                                   const dt_image_box *box,
                                                   const void *icc_data,
                                                   size_t icc_size)
{
  if(!factory || !box || !box->buf || box->exp_width <= 0 || box->exp_height <= 0)
    return NULL;

  IWICImagingFactory *wic = NULL;
  IWICBitmap *bitmap = NULL;
  IWICColorContext *color_ctx = NULL;
  IXpsOMImageResource *resource = NULL;
  IStream *img_stream = NULL;

  HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                               &IID_IWICImagingFactory, (void **)&wic);
  if(FAILED(hr) || !wic) return NULL;

  const UINT stride = (UINT)box->exp_width * 3;
  hr = wic->lpVtbl->CreateBitmapFromMemory(wic, box->exp_width, box->exp_height,
                                         &GUID_WICPixelFormat24bppRGB,
                                         stride, stride * box->exp_height,
                                         (BYTE *)box->buf, &bitmap);

  if(SUCCEEDED(hr) && bitmap && icc_data && icc_size > 0)
  {
    hr = wic->lpVtbl->CreateColorContext(wic, &color_ctx);
    if(SUCCEEDED(hr) && color_ctx)
      hr = color_ctx->lpVtbl->InitializeFromMemory(color_ctx,
                                                 (const BYTE *)icc_data,
                                                 (UINT)icc_size);
    if(FAILED(hr))
    {
      color_ctx->lpVtbl->Release(color_ctx);
      color_ctx = NULL;
    }
  }

  if(SUCCEEDED(hr) && bitmap)
  {
    hr = _win_encode_bitmap_to_png_stream(wic, (IWICBitmapSource *)bitmap, &img_stream);
    if(SUCCEEDED(hr) && img_stream)
    {
      hr = factory->lpVtbl->CreateImageResource(factory, img_stream,
                                              XPS_IMAGE_TYPE_PNG, NULL,
                                              &resource);
      if(SUCCEEDED(hr) && resource && color_ctx)
        resource->lpVtbl->SetColorContext(resource, color_ctx);
      img_stream->lpVtbl->Release(img_stream);
    }
  }

  if(color_ctx) color_ctx->lpVtbl->Release(color_ctx);
  if(bitmap) bitmap->lpVtbl->Release(bitmap);
  if(wic) wic->lpVtbl->Release(wic);

  return resource;
}

// Adds one positioned image to an already-open XPS page.
static HRESULT _win_place_image_on_page(IXpsOMObjectFactory *factory,
                                       IXpsOMPage *page,
                                       IXpsOMImageResource *resource,
                                       const dt_image_box *box,
                                       int resolution)
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
  XPS_RECT viewport = { x, y, w, h };

  HRESULT hr = factory->lpVtbl->CreateImageBrush(factory, resource, &viewbox, &viewport, &brush);
  if(FAILED(hr)) return hr;

  hr = factory->lpVtbl->CreatePath(factory, &path);
  if(FAILED(hr)) goto cleanup;

  hr = factory->lpVtbl->CreateGeometry(factory, &geom);
  if(FAILED(hr)) goto cleanup;

  hr = factory->lpVtbl->CreateGeometryFigure(factory, &(XPS_POINT){x, y}, &figure);
  if(FAILED(hr)) goto cleanup;

  // Rectangle: start at (x, y), then line to (x+w, y), (x+w, y+h), (x, y+h), and close.
  static const XPS_SEGMENT_TYPE seg_types[] = {
    XPS_SEGMENT_TYPE_LINE,
    XPS_SEGMENT_TYPE_LINE,
    XPS_SEGMENT_TYPE_LINE,
    XPS_SEGMENT_TYPE_LINE
  };
  static const FLOAT seg_data[] = {
    x + w, y,
    x + w, y + h,
    x,     y + h,
    x,     y
  };
  static const WINBOOL seg_strokes[] = { TRUE, TRUE, TRUE, TRUE };

  hr = figure->lpVtbl->SetSegments(figure,
                                 4,
                                 8,
                                 seg_types,
                                 seg_data,
                                 seg_strokes);
  if(SUCCEEDED(hr))
    hr = figure->lpVtbl->SetIsClosed(figure, TRUE);
  if(SUCCEEDED(hr))
    hr = figure->lpVtbl->SetIsFilled(figure, TRUE);

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
                        float width, float height)
  {
  DBG_MARK("entering dt_win_print_file");

  if(imgs->count <= 0)
  {
    dt_control_log(_("no images to print on `%s'"), pinfo->printer.name);
    return false;
  }

  const float page_width  = _win_mm_to_diu(width);
  const float page_height = _win_mm_to_diu(height);

  if(!_win_xpsprint_ensure_loaded())
  return false;   // logged inside the helper already
  
  // This runs on the background job thread — COM must be explicitly
  // initialized here, it's not inherited from the GUI thread.
  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
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
    hr = pStartXpsPrintJob(wprinter, wtitle, NULL, NULL, completionEvent,
                          NULL, 0, &xpsJob, &docStream, &ticketStream);
    if(SUCCEEDED(hr))
    {
     if(print_ticket_data && print_ticket_size > 0)
       ticketStream->lpVtbl->Write(ticketStream, print_ticket_data,
                                  (ULONG)print_ticket_size, NULL);

     IXpsOMObjectFactory *factory = NULL;
     hr = CoCreateInstance(&CLSID_XpsOMObjectFactory, NULL, CLSCTX_INPROC_SERVER,
                           &IID_IXpsOMObjectFactory, (void **)&factory);

     if(SUCCEEDED(hr) && factory)
     {
       IXpsOMPackageWriter *writer = NULL;
       hr = factory->lpVtbl->CreatePackageWriterOnStream(factory, docStream,
                                                         FALSE,
                                                         XPS_INTERLEAVING_OFF,
                                                         NULL,
                                                         NULL,
                                                         NULL,
                                                         NULL,
                                                         NULL,
                                                         &writer);

       if(SUCCEEDED(hr) && writer)
       {
         writer->lpVtbl->StartNewDocument(writer, NULL, NULL, NULL, NULL, NULL);

         IXpsOMPage *page = NULL;
         XPS_SIZE page_size = { page_width, page_height };
         hr = factory->lpVtbl->CreatePage(factory, &page_size, L"en-US", NULL, &page);
         if(SUCCEEDED(hr) && page)
         {
           for(int i = 0; i < imgs->count; i++)
           {
             const dt_image_box *box = &imgs->box[i];
             IXpsOMImageResource *res = _win_build_image_resource(factory, box, icc_data, icc_size);
             if(res)
             {
               _win_place_image_on_page(factory, page, res, box, pinfo->printer.resolution);
               res->lpVtbl->Release(res);
             }
           }

           writer->lpVtbl->AddPage(writer, page, &page_size, NULL, NULL, NULL, NULL);
           page->lpVtbl->Release(page);
         }

         writer->lpVtbl->Close(writer);
         writer->lpVtbl->Release(writer);
         ok = true;
       }
       factory->lpVtbl->Release(factory);
     }

     if(docStream) docStream->lpVtbl->Close(docStream);
     if(ticketStream) ticketStream->lpVtbl->Close(ticketStream);

     WaitForSingleObject(completionEvent, INFINITE);
   }
   else
   {
     dt_control_log(_("could not start XPS print job on `%s'"), pinfo->printer.name);
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
    DBG_MARK("borderless printer detected");
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

    DBG_MARK("hw_mm: L=%.2f T=%.2f R=%.2f B=%.2f | user_mm: L=%.2f T=%.2f R=%.2f B=%.2f",
         np_left, np_top, np_right, np_bottom,
         prt->page.margin_left, prt->page.margin_top,
         prt->page.margin_right, prt->page.margin_bottom);

DBG_MARK("layout: px=%.2f py=%.2f pwidth=%.2f pheight=%.2f ax=%.2f ay=%.2f aw=%.2f ah=%.2f",
         *px, *py, *pwidth, *pheight,
         *ax, *ay, *awidth, *aheight);
}







// // Compute the destination rectangle in device units (pixels on printer DC)
// RECT compute_target_rect(const dt_print_info_t *pinfo,
//                          const dt_win_dib_t *dib,
//                          HDC hdc)   // pass in the printer DC so we can query caps
// {
//   RECT r = {0};

//   // Query actual device caps
//   const int dpi_x   = GetDeviceCaps(hdc, LOGPIXELSX);
//   const int dpi_y   = GetDeviceCaps(hdc, LOGPIXELSY);
//   const int horzRes = GetDeviceCaps(hdc, HORZRES);
//   const int vertRes = GetDeviceCaps(hdc, VERTRES);
//   const int physW   = GetDeviceCaps(hdc, PHYSICALWIDTH);
//   const int physH   = GetDeviceCaps(hdc, PHYSICALHEIGHT);
//   const int offX    = GetDeviceCaps(hdc, PHYSICALOFFSETX);
//   const int offY    = GetDeviceCaps(hdc, PHYSICALOFFSETY);

//   DBG_MARK("caps: dpi=(%d,%d) horzRes=%d vertRes=%d physW=%d physH=%d offX=%d offY=%d",
//             dpi_x, dpi_y, horzRes, vertRes, physW, physH, offX, offY);

//   // Borderless detection: full phys area, no offsets
//   const BOOL borderless = (offX == 0 && offY == 0 &&
//                             horzRes == physW && vertRes == physH);

//   // Hardware margins in px (from pinfo, but scaled with actual dpi)
//   const int hw_left_px   = (int)(pinfo->printer.hw_margin_left   / 25.4 * dpi_x + 0.5);
//   const int hw_top_px    = (int)(pinfo->printer.hw_margin_top    / 25.4 * dpi_y + 0.5);
//   const int hw_right_px  = (int)(pinfo->printer.hw_margin_right  / 25.4 * dpi_x + 0.5);
//   const int hw_bottom_px = (int)(pinfo->printer.hw_margin_bottom / 25.4 * dpi_y + 0.5);

//   DBG_MARK("hw_px from mm: L=%d T=%d R=%d B=%d",
//             hw_left_px, hw_top_px, hw_right_px, hw_bottom_px);

//   // User margins in px
//   const int user_left_px   = (int)(pinfo->page.margin_left   / 25.4 * dpi_x + 0.5);
//   const int user_top_px    = (int)(pinfo->page.margin_top    / 25.4 * dpi_y + 0.5);
//   const int user_right_px  = (int)(pinfo->page.margin_right  / 25.4 * dpi_x + 0.5);
//   const int user_bottom_px = (int)(pinfo->page.margin_bottom / 25.4 * dpi_y + 0.5);

//   // Effective margins = user − hw (clamped at 0)
//   const int eff_left   = (user_left_px   > hw_left_px)   ? (user_left_px   - hw_left_px)   : 0;
//   const int eff_top    = (user_top_px    > hw_top_px)    ? (user_top_px    - hw_top_px)    : 0;
//   const int eff_right  = (user_right_px  > hw_right_px)  ? (user_right_px  - hw_right_px)  : 0;
//   const int eff_bottom = (user_bottom_px > hw_bottom_px) ? (user_bottom_px - hw_bottom_px) : 0;

//   // Available area in device units
//   const int page_w = borderless ? physW : horzRes;
//   const int page_h = borderless ? physH : vertRes;

//   const int avail_w_px = page_w - eff_left - eff_right;
//   const int avail_h_px = page_h - eff_top  - eff_bottom;

// // Compute image native DPI
// double image_dpi_x = (double)dib->width  / pinfo->paper.width;   // pixels per mm
// double image_dpi_y = (double)dib->height / pinfo->paper.height;
// image_dpi_x *= 25.4; // convert to pixels per inch
// image_dpi_y *= 25.4;

// // Clamp to avoid upscaling
// double effective_dpi_x = MIN(image_dpi_x, dpi_x);
// double effective_dpi_y = MIN(image_dpi_y, dpi_y);

// // Compute target raster size in pixels
// int dst_w = (int)(pinfo->paper.width  * effective_dpi_x / 25.4);
// int dst_h = (int)(pinfo->paper.height * effective_dpi_y / 25.4);

// // Optional: oversize for borderless printing
// double oversize_factor_x = (double)physW / (double)(physW - eff_left - eff_right);
// double oversize_factor_y = (double)physH / (double)(physH - eff_top - eff_bottom);
// oversize_factor_x = MAX(1.0, oversize_factor_x);
// oversize_factor_y = MAX(1.0, oversize_factor_y);

// dst_w = (int)(dst_w * oversize_factor_x);
// dst_h = (int)(dst_h * oversize_factor_y);


//   // Center within available area
//   int dst_x = eff_left + (avail_w_px - dst_w) / 2;
//   int dst_y = eff_top  + (avail_h_px - dst_h) / 2;

//   r.left   = dst_x;
//   r.top    = dst_y;
//   r.right  = dst_x + dst_w;
//   r.bottom = dst_y + dst_h;

//       // --- Paper mismatch check ---
//   double physW_mm = (double)physW / dpi_x * 25.4;
//   double physH_mm = (double)physH / dpi_y * 25.4;

//   double reqW_mm = pinfo->paper.width;
//   double reqH_mm = pinfo->paper.height;

//   // Allow a small tolerance (drivers often round sizes)
//   const double tol = 1.0; // mm

//   if(fabs(physW_mm - reqW_mm) > tol || fabs(physH_mm - reqH_mm) > tol) {
//       dt_control_log(_("Paper mismatch: job requests %.1fx%.1f mm, "
//                         "printer reports %.1fx%.1f mm. "
//                         "Image will be centered and may be cropped."),
//                       reqW_mm, reqH_mm, physW_mm, physH_mm);

//   }
  
//   DBG_MARK("compute_target_rect: borderless=%d | hw_px L=%d T=%d R=%d B=%d | "
//             "user_px L=%d T=%d R=%d B=%d | eff_px L=%d T=%d R=%d B=%d | rect=(%d,%d %dx%d)",
//             borderless,
//             hw_left_px, hw_top_px, hw_right_px, hw_bottom_px,
//             user_left_px, user_top_px, user_right_px, user_bottom_px,
//             eff_left, eff_top, eff_right, eff_bottom,
//             r.left, r.top, r.right - r.left, r.bottom - r.top);

//   return r;
// }


//constructor for the print settings context
dt_win32_print_ctx_t *dt_win32_print_ctx_new(dt_print_info_t *pinfo)
{
  dt_win32_print_ctx_t *settings_ctx = calloc(1, sizeof(*settings_ctx));

  if(!settings_ctx) return NULL;

  settings_ctx->base = pinfo;         // <-- inject here
  settings_ctx->settings_opened = FALSE;
  settings_ctx->cached_dm = NULL;
  settings_ctx->hPrinter = NULL;

  if(!pinfo || !pinfo->printer.name[0]) 
  {
    DBG_MARK("dt_win32_print_ctx_new: invalid printer name");
    return settings_ctx; // nothing more we can do
  }

  wchar_t *wprinter = g_utf8_to_utf16(pinfo->printer.name, -1, NULL, NULL, NULL);
  // open printer handle
  if(!OpenPrinterW(wprinter, &settings_ctx->hPrinter, NULL)) 
  {
    DBG_MARK("OpenPrinterW failed for %s", wprinter);
    return settings_ctx; // leave cached_dm NULL, caller can detect
  }
  DBG_MARK("dt_win32_print_ctx_new: opened printer %s", wprinter);
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
            DBG_MARK("ctx_new: initialized defaults dmSize=%d fields=0x%x dpi=%d/%d",
                settings_ctx->cached_dm->dmSize,
                settings_ctx->cached_dm->dmFields,
                settings_ctx->cached_dm->dmPrintQuality,
                settings_ctx->cached_dm->dmYResolution);                               
      if(ret != IDOK) 
      {
        free(settings_ctx->cached_dm);
        settings_ctx->cached_dm = NULL;
      } 
      else 
      {
        // after successful DocumentPropertiesW
        dt_sync_orientation(settings_ctx->cached_dm, &pinfo->page);
        DBG_MARK("ctx_new: post popup dmSize=%d fields=0x%x dpi=%d/%d",
                settings_ctx->cached_dm->dmSize,
                settings_ctx->cached_dm->dmFields,
                settings_ctx->cached_dm->dmPrintQuality,
                settings_ctx->cached_dm->dmYResolution);
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
  DBG_MARK("entering dt_win_open_printer_settings");  
  if(!settings_ctx || !settings_ctx->base ) 
  {
      DBG_MARK("dt_win_open_printer_settings: invalid context or printer name");
      return FALSE;
  }

  const char *utf8_name = settings_ctx->base->printer.name;
  if(!utf8_name || !*utf8_name) 
  {
  DBG_MARK("printer name missing");
  return FALSE;
  }

  // Convert UTF‑8 printer name from pinfo to wide string
  wchar_t *printer_name = g_utf8_to_utf16(utf8_name, -1, NULL, NULL, NULL);
  if(!printer_name) {
      DBG_MARK("Failed to convert printer name to UTF‑16");
      return FALSE;
  }

  HANDLE hPrinter = NULL;
  if(!OpenPrinterW(printer_name, &hPrinter, NULL)) {
      DBG_MARK("OpenPrinterW failed for %S", printer_name);
      g_free(printer_name);
      return FALSE;
  }

  // Query required DEVMODE size
  LONG dm_size = DocumentPropertiesW(hwnd_owner,
                                      hPrinter,
                                      printer_name,
                                      NULL, NULL, 0);
  if(dm_size < 0) {
      DBG_MARK("DocumentPropertiesW size query failed");
      ClosePrinter(hPrinter);
      g_free(printer_name);
      return FALSE;
  }

  if(!settings_ctx->cached_dm) 
  {
    settings_ctx->cached_dm = (DEVMODEW*)malloc(dm_size);
    if(!settings_ctx->cached_dm) 
    {
        DBG_MARK("malloc failed for DEVMODE");
        ClosePrinter(hPrinter);
        g_free(printer_name);
        return FALSE;
    }
    ZeroMemory(settings_ctx->cached_dm, dm_size);
  }


    dt_sync_orientation(settings_ctx->cached_dm, &settings_ctx->base->page);

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
    DBG_MARK("Printer settings updated via dialog");

    if(settings_ctx->cached_dm)
    {
      // Sync orientation from DEVMODE back to Darktable
      if(settings_ctx->cached_dm->dmFields & DM_ORIENTATION) 
      {
          settings_ctx->base->page.landscape =
              (settings_ctx->cached_dm->dmOrientation == DMORIENT_LANDSCAPE)
                  ? TRUE
                  : FALSE;
      }
      
      // One‑stop sync: DEVMODE → pinfo (orientation, paper, resolution, hw margins)
      dt_win_sync_cached_dm_to_pinfo(settings_ctx);

      // Optional debug dump of DEVMODE
      // DEVMODEW *dm = settings_ctx->cached_dm;
      // DBG_MARK("DEVMODE: fields=0x%x size=%d orientation=%d paper=%d (%dx%d tenths mm) "
      //          "quality=%d xres=%d yres=%d copies=%d color=%d",
      //          dm->dmFields,
      //          dm->dmSize,
      //          dm->dmOrientation,
      //          dm->dmPaperSize,
      //          dm->dmPaperWidth, dm->dmPaperLength,
      //          dm->dmPrintQuality,
      //          dm->dmPrintQuality, dm->dmYResolution,
      //          dm->dmCopies,
      //          dm->dmColor);
    }

    return TRUE;
  }
  else
  {
  //   // DBG_MARK("Printer settings dialog cancelled");

  //   // if(settings_ctx->cached_dm)
  //   // {
  //   //   // DEVMODEW *dm = settings_ctx->cached_dm;

  //     // Optional debug dump of DEVMODE
  //     // DBG_MARK("DEVMODE: fields=0x%x size=%d orientation=%d paper=%d (%dx%d tenths mm) "
  //     //          "quality=%d xres=%d yres=%d copies=%d color=%d",
  //     //          dm->dmFields,
  //     //          dm->dmSize,
  //     //          dm->dmOrientation,
  //     //          dm->dmPaperSize,
  //     //          dm->dmPaperWidth, dm->dmPaperLength,
  //     //          dm->dmPrintQuality,
  //     //          dm->dmPrintQuality, dm->dmYResolution,
  //     //          dm->dmCopies,
  //     //          dm->dmColor);
  //   }

    return FALSE;
  }

}

//-----------------------------REFACTOR WORK
// uint8_t *convert_16bit_rgb_to_24bit(const uint16_t *src, int width, int height)
// {
//   int total_pixels = width * height;
//   uint8_t *dst = g_malloc(3 * total_pixels);

//   for(int i = 0; i < total_pixels; i++)
//   {
//     dst[3*i + 0] = src[3*i + 0] >> 8; // R
//     dst[3*i + 1] = src[3*i + 1] >> 8; // G
//     dst[3*i + 2] = src[3*i + 2] >> 8; // B
//   }

//   return dst;
// }


bool win_render_box_to_dib(const dt_image_box *box, dt_win_dib_t *out)
{

  if(!box || !box->buf || box->exp_width <= 0 || box->exp_height <= 0)
  {
    DBG_MARK("invalid box or buffer: imgid=%d exp=%dx%d buf=%p",
             box ? box->imgid : -1,
             box ? box->exp_width : -1,
             box ? box->exp_height : -1,
             box ? box->buf : NULL);
    return false;
  }

DBG_MARK("rendering box check 2: imgid=%d exp=%dx%d print=%.2fx%.2f px at (%.2f, %.2f) - stopping to avoid crash",
         box->imgid,
         box->exp_width, box->exp_height,
         box->print.width, box->print.height,
         box->print.x, box->print.y);

const int width  = box->exp_width;
const int height = box->exp_height;

int stride = ((width * 3 + 3) & ~3); // pad to 4-byte boundary
int bufsize = stride * height;

uint8_t *pixels = (uint8_t *)g_malloc(bufsize); // safe and aligned
  // Fill background white
  memset(pixels, 0xFF, bufsize);

  BITMAPINFO *bi = (BITMAPINFO *)g_malloc0(sizeof(BITMAPINFO));
  
  bi->bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
  bi->bmiHeader.biWidth       = width;
  bi->bmiHeader.biHeight      = -height; // top-down
  bi->bmiHeader.biPlanes      = 1;
  bi->bmiHeader.biBitCount    = 24;
  bi->bmiHeader.biCompression = BI_RGB;
  bi->bmiHeader.biSizeImage   = (DWORD)bufsize;

  const int src_w = box->exp_width;
  const int src_h = box->exp_height;
  const uint8_t *src = (const uint8_t *)box->buf;

  const int dst_x = 0; //box->print.x;
  const int dst_y = 0; //box->print.y;
  const int dst_w = box->print.width;
  const int dst_h = box->print.height;

if (src_w == dst_w && src_h == dst_h)
{
  for (int y = 0; y < dst_h; y++)
  {
    const uint8_t *srcrow = src + y * src_w * 3;
    uint8_t *dstrow = pixels + (dst_y + y) * stride + dst_x * 3; // 3 bytes per pixel

  if ((dst_y + y) >= height) {
    DBG_MARK("Y overflow: dst_y=%d y=%d height=%d", dst_y, y, height);
    continue; // or break, or return false
  }

  // 🛡️ Debug check: row width bounds
  if ((dst_x + dst_w) * 3 > stride) {
    DBG_MARK("X overflow: dst_x=%d dst_w=%d stride=%d", dst_x, dst_w, stride);
    continue; // or break, or return false
  }


    for (int x = 0; x < dst_w; x++)
    {
      const uint8_t *s = srcrow + x * 3;
      uint8_t *d = dstrow + x * 3;

      d[0] = s[2]; // B
      d[1] = s[1]; // G
      d[2] = s[0]; // R
      // no alpha
    }

    // // Optional: zero out padding bytes at end of row
    // int row_bytes = dst_w * 3;
    // int pad_bytes = stride - row_bytes;
    // if (pad_bytes > 0)
    //   memset(dstrow + row_bytes, 0, pad_bytes);
  }
}
else
{
  DBG_MARK("dimension mismatch: src=%dx%d dst=%dx%d", src_w, src_h, dst_w, dst_h);
}

  out->width  = box->exp_width;
  out->height = box->exp_height;
// buffer is guaranteed to be 24-bit RGB from export_image
  out->pixels = pixels;
  out->stride = stride;
  out->bi     = bi;
// DBG_MARK("DPI calc input: exp_width=%d print.width=%.2f", box->exp_width, box->print.width);
//   // Compute effective DPI from layout
//   double dpi_x = box->exp_width  / (box->print.width  / 25.4);
//   DBG_MARK("computed DPI: %.2f", dpi_x);
//   double dpi_y = box->exp_height / (box->print.height / 25.4);
//   out->dpi_x = dpi_x;
//   out->dpi_y = dpi_y;


  return true;
}

RECT compute_box_rect(const dt_image_box *box, HDC hdc, int dpi_x, int dpi_y, int paper_width, int paper_height)
{
RECT r;

int hw_margin_left = GetDeviceCaps(hdc, PHYSICALOFFSETX);
int hw_margin_top  = GetDeviceCaps(hdc, PHYSICALOFFSETY);
int printable_width  = GetDeviceCaps(hdc, HORZRES);
int printable_height = GetDeviceCaps(hdc, VERTRES);
  int physW   = GetDeviceCaps(hdc, PHYSICALWIDTH);
  int physH   = GetDeviceCaps(hdc, PHYSICALHEIGHT);

// Compute centering offset between Darktable paper and actual printable area
int offset_x = (physW  - (int)paper_width*dpi_x/25.4) / 2;
int offset_y = (physH - (int)paper_height*dpi_y/25.4) / 2;

// Apply offset and hardware margin correction
r.left   = box->print.x + offset_x - hw_margin_left;
r.right  = r.left + box->print.width;

r.bottom = printable_height - (box->print.y + offset_y - hw_margin_top);
r.top    = r.bottom - box->print.height;
DBG_MARK ("printable: %d %d; physical: %d %d; paper: %d %d; offset: %d %d", printable_width, printable_height, physW, physH, paper_width, paper_height, offset_x, offset_y);
return r;
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