/*
    This file is part of darktable,
    Copyright (C) 2014-2021 darktable developers.

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

#include <cups/cups.h>
#include <cups/ppd.h>
#include <glib.h>
#include <stdio.h>
#ifdef __APPLE__
#include <AvailabilityMacros.h>
#endif

#include "common/file_location.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/pdf.h"
#include "control/jobs/control_jobs.h"
#include "cups_print.h"

// enable weak linking in libcups on macOS
#if defined(__APPLE__) && MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_8 && ((CUPS_VERSION_MAJOR == 1 && CUPS_VERSION_MINOR >= 6) || CUPS_VERSION_MAJOR > 1)
extern int cupsEnumDests() __attribute__((weak_import));
#endif
#if defined(__APPLE__) && MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_9 && ((CUPS_VERSION_MAJOR == 1 && CUPS_VERSION_MINOR >= 7) || CUPS_VERSION_MAJOR > 1)
extern http_t *cupsConnectDest() __attribute__((weak_import));
extern cups_dinfo_t *cupsCopyDestInfo() __attribute__((weak_import));
extern int cupsGetDestMediaCount() __attribute__((weak_import));
extern int cupsGetDestMediaByIndex() __attribute__((weak_import));
extern void cupsFreeDestInfo() __attribute__((weak_import));
#endif

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
// some platforms are starting to provide CUPS 2.2.9 and there the
// CUPS API deprecated routines ate now flagged as such and reported as
// warning preventing the compilation.
//
// this seems wrong and PPD should be removed from this unit. but there
// still one missing piece discussed with the CUPS maintainers about the
// way to get media-type using the IPP API. nothing close to working at
// this stage, so instead of breaking the compilation on platforms using
// recent CUPS version we kill the warning.

typedef struct dt_prtctl_t
{
  void (*cb)(dt_printer_info_t *, void *);
  void *user_data;
} dt_prtctl_t;

// initialize the pinfo structure
void dt_init_print_info(dt_print_info_t *pinfo)
{
  memset(&pinfo->printer, 0, sizeof(dt_printer_info_t));
  memset(&pinfo->page, 0, sizeof(dt_page_setup_t));
  memset(&pinfo->paper, 0, sizeof(dt_paper_info_t));
  pinfo->printer.intent = DT_INTENT_PERCEPTUAL;
  pinfo->printer.is_turboprint = FALSE;
  *pinfo->printer.profile = '\0';
}

void dt_get_printer_info(const char *printer_name, dt_printer_info_t *pinfo)
{
  cups_dest_t *dests;
  const int num_dests = cupsGetDests(&dests);
  cups_dest_t *dest = cupsGetDest(printer_name, NULL, num_dests, dests);

  if(dest)
  {
    const char *PPDFile = cupsGetPPD (printer_name);
    g_strlcpy(pinfo->name, dest->name, MAX_NAME);
    ppd_file_t *ppd = ppdOpenFile(PPDFile);

    if(ppd)
    {
      ppdMarkDefaults(ppd);
      cupsMarkOptions(ppd, dest->num_options, dest->options);

      // first check if this is turboprint drived printer, two solutions:
      // 1. ModelName contains TurboPrint
      // 2. zedoPrinterDriver exists
      ppd_attr_t *attr = ppdFindAttr(ppd, "ModelName", NULL);

      if(attr)
      {
        pinfo->is_turboprint = strstr(attr->value, "TurboPrint") != NULL;
      }

      // hardware margins

      attr = ppdFindAttr(ppd, "HWMargins", NULL);

      if(attr)
      {
        // scanf use local number format and PPD has en numbers
        dt_util_str_to_loc_numbers_format(attr->value);

        sscanf(attr->value, "%lf %lf %lf %lf",
               &pinfo->hw_margin_left, &pinfo->hw_margin_bottom,
               &pinfo->hw_margin_right, &pinfo->hw_margin_top);

        pinfo->hw_margin_left   = dt_pdf_point_to_mm (pinfo->hw_margin_left);
        pinfo->hw_margin_bottom = dt_pdf_point_to_mm (pinfo->hw_margin_bottom);
        pinfo->hw_margin_right  = dt_pdf_point_to_mm (pinfo->hw_margin_right);
        pinfo->hw_margin_top    = dt_pdf_point_to_mm (pinfo->hw_margin_top);
      }

      // default resolution

      attr = ppdFindAttr(ppd, "DefaultResolution", NULL);

      if(attr)
      {
        char *x = strstr(attr->value, "x");

        if(x)
          sscanf (x+1, "%ddpi", &pinfo->resolution);
        else
          sscanf (attr->value, "%ddpi", &pinfo->resolution);
      }
      else
        pinfo->resolution = 300;

      while(pinfo->resolution>360)
        pinfo->resolution /= 2.0;

      ppdClose(ppd);
      g_unlink(PPDFile);
    }
  }

  cupsFreeDests(num_dests, dests);
}

static int _dest_cb(void *user_data, unsigned flags, cups_dest_t *dest)
{
  const dt_prtctl_t *pctl = (dt_prtctl_t *)user_data;
  const char *psvalue = cupsGetOption("printer-state", dest->num_options, dest->options);

  // check that the printer is ready
  if(psvalue!=NULL && strtol(psvalue, NULL, 10) < IPP_PRINTER_STOPPED)
  {
    dt_printer_info_t pr;
    memset(&pr, 0, sizeof(pr));
    dt_get_printer_info(dest->name, &pr);
    if(pctl->cb) pctl->cb(&pr, pctl->user_data);
    dt_print(DT_DEBUG_PRINT, "[print] new printer %s found\n", dest->name);
  }
  else
    dt_print(DT_DEBUG_PRINT, "[print] skip printer %s as stopped\n", dest->name);

  return 1;
}

static int _cancel = 0;

static int _detect_printers_callback(dt_job_t *job)
{
  dt_prtctl_t *pctl = dt_control_job_get_params(job);
  int res;
#if((CUPS_VERSION_MAJOR == 1) && (CUPS_VERSION_MINOR >= 6)) || CUPS_VERSION_MAJOR > 1
#if defined(__APPLE__) && MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_8
  if(&cupsEnumDests != NULL)
#endif
    res = cupsEnumDests(CUPS_MEDIA_FLAGS_DEFAULT, 30000, &_cancel, 0, 0, _dest_cb, pctl);
#if defined(__APPLE__) && MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_8
  else
#endif
#endif
#if defined(__APPLE__) && MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_8 || !(((CUPS_VERSION_MAJOR == 1) && (CUPS_VERSION_MINOR >= 6)) || CUPS_VERSION_MAJOR > 1)
  {
    cups_dest_t *dests;
    const int num_dests = cupsGetDests(&dests);
    for(int k=0; k<num_dests; k++)
    {
      _dest_cb((void *)pctl, 0, &dests[k]);
    }
    cupsFreeDests(num_dests, dests);
    res=1;
  }
#endif
  return !res;
}

void dt_printers_abort_discovery(void)
{
  _cancel = 1;
}

void dt_printers_discovery(void (*cb)(dt_printer_info_t *pr, void *user_data), void *user_data)
{
  // asynchronously checks for available printers
  dt_job_t *job = dt_control_job_create(&_detect_printers_callback, "detect connected printers");
  if(job)
  {
    dt_prtctl_t *prtctl = g_malloc0(sizeof(dt_prtctl_t));

    prtctl->cb = cb;
    prtctl->user_data = user_data;

    dt_control_job_set_params(job, prtctl, g_free);
    dt_control_add_job(darktable.control, DT_JOB_QUEUE_SYSTEM_BG, job);
  }
}

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
  dt_paper_info_t *result = NULL;

  for(GList *p = papers; p; p = g_list_next(p))
  {
    dt_paper_info_t *pi = (dt_paper_info_t*)p->data;
    if(!strcmp(pi->name,name) || !strcmp(pi->common_name,name))
    {
      result = pi;
      break;
    }
  }
  return result;
}

static gint
sort_papers (gconstpointer p1, gconstpointer p2)
{
  const dt_paper_info_t *n1 = (dt_paper_info_t *)p1;
  const dt_paper_info_t *n2 = (dt_paper_info_t *)p2;
  const int l1 = strlen(n1->common_name);
  const int l2 = strlen(n2->common_name);
  return l1==l2 ? strcmp(n1->common_name, n2->common_name) : (l1 < l2 ? -1 : +1);
}

GList *dt_get_papers(const dt_printer_info_t *printer)
{
  const char *printer_name = printer->name;
  GList *result = NULL;

#if((CUPS_VERSION_MAJOR == 1) && (CUPS_VERSION_MINOR >= 7)) || CUPS_VERSION_MAJOR > 1
#if defined(__APPLE__) && MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_9
  if(&cupsConnectDest != NULL && &cupsCopyDestInfo != NULL && &cupsGetDestMediaCount != NULL &&
      &cupsGetDestMediaByIndex != NULL && &cupsFreeDestInfo != NULL)
#endif
  {
    cups_dest_t *dests;
    const int num_dests = cupsGetDests(&dests);
    cups_dest_t *dest = cupsGetDest(printer_name, NULL, num_dests, dests);

    int cancel = 0; // important

    char resource[1024];

    if(dest)
    {
      http_t *hcon = cupsConnectDest(dest, 0, 2000, &cancel, resource, sizeof(resource), NULL, (void *)NULL);

      if(hcon)
      {
        cups_size_t size;
        cups_dinfo_t *info = cupsCopyDestInfo (hcon, dest);
        const int count = cupsGetDestMediaCount(hcon, dest, info, CUPS_MEDIA_FLAGS_DEFAULT);
        for(int k=0; k<count; k++)
        {
          if(cupsGetDestMediaByIndex(hcon, dest, info, k, CUPS_MEDIA_FLAGS_DEFAULT, &size))
          {
            if(size.width!=0 && size.length!=0 && !paper_exists(result, size.media))
            {
              pwg_media_t *med = pwgMediaForPWG (size.media);
              char common_name[MAX_NAME] = { 0 };

              if(med->ppd)
                g_strlcpy(common_name, med->ppd, sizeof(common_name));
              else
                g_strlcpy(common_name, size.media, sizeof(common_name));

              dt_paper_info_t *paper = (dt_paper_info_t*)malloc(sizeof(dt_paper_info_t));
              g_strlcpy(paper->name, size.media, sizeof(paper->name));
              g_strlcpy(paper->common_name, common_name, sizeof(paper->common_name));
              paper->width = (double)size.width / 100.0;
              paper->height = (double)size.length / 100.0;
              result = g_list_append (result, paper);

              dt_print(DT_DEBUG_PRINT,
                       "[print] new media paper %4d %6.2f x %6.2f (%s) (%s)\n",
                       k, paper->width, paper->height, paper->name, paper->common_name);
            }
          }
        }

        cupsFreeDestInfo(info);
        httpClose(hcon);
      }
      else
        dt_print(DT_DEBUG_PRINT, "[print] cannot connect to printer %s (cancel=%d)\n", printer_name, cancel);
    }

    cupsFreeDests(num_dests, dests);
  }
#endif

  // check now PPD page sizes

  const char *PPDFile = cupsGetPPD(printer_name);
  ppd_file_t *ppd = ppdOpenFile(PPDFile);

  if(ppd)
  {
    ppd_size_t *size = ppd->sizes;

    for(int k=0; k<ppd->num_sizes; k++)
    {
      if(size->width!=0 && size->length!=0 && !paper_exists(result, size->name))
      {
        dt_paper_info_t *paper = (dt_paper_info_t*)malloc(sizeof(dt_paper_info_t));
        g_strlcpy(paper->name, size->name, MAX_NAME);
        g_strlcpy(paper->common_name, size->name, MAX_NAME);
        paper->width = (double)dt_pdf_point_to_mm(size->width);
        paper->height = (double)dt_pdf_point_to_mm(size->length);
        result = g_list_append (result, paper);

        dt_print(DT_DEBUG_PRINT,
                 "[print] new ppd paper %4d %6.2f x %6.2f (%s) (%s)\n",
                 k, paper->width, paper->height, paper->name, paper->common_name);
      }
      size++;
    }

    ppdClose(ppd);
    g_unlink(PPDFile);
  }

  result = g_list_sort_with_data (result, (GCompareDataFunc)sort_papers, NULL);
  return result;
}

GList *dt_get_media_type(const dt_printer_info_t *printer)
{
  const char *printer_name = printer->name;
  GList *result = NULL;

  // check now PPD media type

  const char *PPDFile = cupsGetPPD(printer_name);
  ppd_file_t *ppd = ppdOpenFile(PPDFile);

  if(ppd)
  {
      ppd_option_t *opt = ppdFindOption(ppd, "MediaType");

      if(opt)
      {
        ppd_choice_t *choice = opt->choices;

        for(int k=0; k<opt->num_choices; k++)
        {
          dt_medium_info_t *media = (dt_medium_info_t*)malloc(sizeof(dt_medium_info_t));
          g_strlcpy(media->name, choice->choice, MAX_NAME);
          g_strlcpy(media->common_name, choice->text, MAX_NAME);
          result = g_list_prepend (result, media);

          dt_print(DT_DEBUG_PRINT, "[print] new media %2d (%s) (%s)\n", k, media->name, media->common_name);
          choice++;
        }
      }
  }

  ppdClose(ppd);
  g_unlink(PPDFile);

  return g_list_reverse(result);  // list was built in reverse order, so un-reverse it
}

dt_medium_info_t *dt_get_medium(GList *media, const char *name)
{
  dt_medium_info_t *result = NULL;

  for(GList *m = media; m; m = g_list_next(m))
  {
    dt_medium_info_t *mi = (dt_medium_info_t*)m->data;
    if(!strcmp(mi->name, name) || !strcmp(mi->common_name, name))
    {
      result = mi;
      break;
    }
  }
  return result;
}

void dt_print_file(const int32_t imgid, const char *filename, const char *job_title, const dt_print_info_t *pinfo)
{
  // first for safety check that filename exists and is readable

  if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR))
  {
    dt_control_log(_("file `%s' to print not found for image %d on `%s'"), filename, imgid, pinfo->printer.name);
    return;
  }

  cups_option_t *options = NULL;
  int num_options = 0;

  // for turboprint drived printer, use the turboprint dialog
  if(pinfo->printer.is_turboprint)
  {
    const char *tp_intent_name[] = { "perception_0", "colorimetric-relative_1", "saturation_1", "colorimetric-absolute_1" };
    char tmpfile[PATH_MAX] = { 0 };

    dt_loc_get_tmp_dir(tmpfile, sizeof(tmpfile));
    g_strlcat(tmpfile, "/dt_cups_opts_XXXXXX", sizeof(tmpfile));

    gint fd = g_mkstemp(tmpfile);
    if(fd == -1)
    {
      dt_control_log(_("failed to create temporary file for printing options"));
      dt_print(DT_DEBUG_ALWAYS, "failed to create temporary pdf for printing options\n");
      return;
    }
    close(fd);

    // ensure that intent is in the range, may happen if at some point we add new intent in the list
    const int intent = (pinfo->printer.intent < 4) ? pinfo->printer.intent : 0;

    // spawn turboprint command
    gchar * argv[15] = { 0 };

    argv[0] = "turboprint";
    argv[1] = g_strdup_printf("--printer=%s", pinfo->printer.name);
    argv[2] = "--options";
    argv[3] = g_strdup_printf("--output=%s", tmpfile);
    argv[4] = "-o";
    argv[5] = "copies=1";
    argv[6] = "-o";
    argv[7] = g_strdup_printf("PageSize=%s", pinfo->paper.common_name);
    argv[8] = "-o";
    argv[9] = "InputSlot=AutoSelect";
    argv[10] = "-o";
    argv[11] = g_strdup_printf("zedoIntent=%s", tp_intent_name[intent]);
    argv[12] = "-o";
    argv[13] = g_strdup_printf("MediaType=%s", pinfo->medium.name);
    argv[14] = NULL;

    gint exit_status = 0;

    g_spawn_sync(NULL, argv, NULL,
                 G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                 NULL, NULL, NULL, NULL, &exit_status, NULL);

    g_free(argv[1]);
    g_free(argv[3]);
    g_free(argv[7]);
    g_free(argv[11]);
    g_free(argv[13]);

    if(exit_status==0)
    {
      FILE *stream = g_fopen(tmpfile, "rb");

      while(1)
      {
        char optname[100];
        char optvalue[100];
        const int ropt = fscanf(stream, "%*s %99[^= ]=%99s", optname, optvalue);

        // if we parsed an option name=value
        if(ropt==2)
        {
          char *v = optvalue;

          // remove possible single quote around value
          if(*v == '\'') v++;
          if(v[strlen(v)-1] == '\'') v[strlen(v)-1] = '\0';

          num_options = cupsAddOption(optname, v, num_options, &options);
        }
        else if(ropt == EOF)
          break;
      }
      fclose(stream);
      g_unlink(tmpfile);
    }
    else
    {
      dt_control_log(_("printing on `%s' cancelled"), pinfo->printer.name);
      dt_print(DT_DEBUG_PRINT, "[print]   command fails with %d, cancel printing\n", exit_status);
      return;
    }
  }
  else
  {
    cups_dest_t *dests;
    const int num_dests = cupsGetDests(&dests);
    cups_dest_t *dest = cupsGetDest(pinfo->printer.name, NULL, num_dests, dests);

    for(int j = 0; j < dest->num_options; j ++)
      if(cupsGetOption(dest->options[j].name, num_options,
                        options) == NULL)
        num_options = cupsAddOption(dest->options[j].name,
                                    dest->options[j].value,
                                    num_options, &options);

    cupsFreeDests(num_dests, dests);

    // if we have a profile, disable cm on CUPS, this is important as dt does the cm

    num_options = cupsAddOption("cm-calibration", *pinfo->printer.profile ? "true" : "false", num_options, &options);

    // media to print on

    num_options = cupsAddOption("media", pinfo->paper.name, num_options, &options);

    // the media type to print on

    num_options = cupsAddOption("MediaType", pinfo->medium.name, num_options, &options);

    // never print two-side

    num_options = cupsAddOption("sides", "one-sided", num_options, &options);

    // and a single image per page

    num_options = cupsAddOption("number-up", "1", num_options, &options);

    // if the printer has no hardware margins activate the borderless mode

    if(pinfo->printer.hw_margin_top == 0 || pinfo->printer.hw_margin_bottom == 0
        || pinfo->printer.hw_margin_left == 0 || pinfo->printer.hw_margin_right == 0)
    {
      // there is many variant for this parameter
      num_options = cupsAddOption("StpFullBleed", "true", num_options, &options);
      num_options = cupsAddOption("STP_FullBleed", "true", num_options, &options);
      num_options = cupsAddOption("Borderless", "true", num_options, &options);
    }

    // as cups-filter pdftopdf will autorotate the page, there is no
    // need to set an option in the case of landscape mode images
  }

  // print lp options

  dt_print(DT_DEBUG_PRINT, "[print] printer options (%d)\n", num_options);
  for(int k=0; k<num_options; k++)
    dt_print(DT_DEBUG_PRINT, "[print]   %2d  %s=%s\n", k+1, options[k].name, options[k].value);

  const int job_id = cupsPrintFile(pinfo->printer.name, filename, job_title, num_options, options);

  if(job_id == 0)
    dt_control_log(_("error while printing `%s' on `%s'"), job_title, pinfo->printer.name);
  else
    dt_control_log(_("printing `%s' on `%s'"), job_title, pinfo->printer.name);

  cupsFreeOptions (num_options, options);
}

void dt_get_print_layout(const dt_print_info_t *prt,
                         const int32_t area_width, const int32_t area_height,
                         float *px, float *py, float *pwidth, float *pheight,
                         float *ax, float *ay, float *awidth, float *aheight,
                         gboolean *borderless)
{
  /* this is where the layout is done for the display and for the print too. So this routine is one
     of the most critical for the print circuitry. */

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

  // page margins, note that we do not want to change those values for the landscape mode.
  // these margins are those set by the user from the GUI, and the top margin is *always*
  // at the top of the screen.

  const float border_top = prt->page.margin_top;
  const float border_left = prt->page.margin_left;
  const float border_right = prt->page.margin_right;
  const float border_bottom = prt->page.margin_bottom;

  // display picture area, that is removing the non printable areas and user's margins

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

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

