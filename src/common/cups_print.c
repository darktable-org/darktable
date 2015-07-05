/*
    This file is part of darktable,
    copyright (c) 2014-2015 pascal obry.

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

#include <glib.h>
#include <stdio.h>
#include <cups/cups.h>
#include <cups/ppd.h>

#include "common/image.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/pdf.h"
#include "cups_print.h"
#include "control/jobs/control_jobs.h"

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
  *pinfo->printer.profile = '\0';
}

dt_printer_info_t *dt_get_printer_info(const char *printer_name)
{
  cups_dest_t *dests;
  int num_dests = cupsGetDests(&dests);
  cups_dest_t *dest = cupsGetDest(printer_name, NULL, num_dests, dests);
  dt_printer_info_t *result = NULL;

  if (dest)
  {
    const char *PPDFile = cupsGetPPD (printer_name);
    result = (dt_printer_info_t *)malloc(sizeof(dt_printer_info_t));
    g_strlcpy(result->name, dest->name, MAX_NAME);
    ppd_file_t *ppd = ppdOpenFile(PPDFile);

    if (ppd)
    {
      ppdMarkDefaults(ppd);
      cupsMarkOptions(ppd, dest->num_options, dest->options);

      // hardware margins

      ppd_attr_t *attr = ppdFindAttr(ppd, "HWMargins", NULL);

      if (attr)
      {
        sscanf(attr->value, "%lf %lf %lf %lf",
               &result->hw_margin_left, &result->hw_margin_bottom,
               &result->hw_margin_right, &result->hw_margin_top);

        result->hw_margin_left   = dt_pdf_point_to_mm (result->hw_margin_left);
        result->hw_margin_bottom = dt_pdf_point_to_mm (result->hw_margin_bottom);
        result->hw_margin_right  = dt_pdf_point_to_mm (result->hw_margin_right);
        result->hw_margin_top    = dt_pdf_point_to_mm (result->hw_margin_top);
      }

      // default resolution

      attr = ppdFindAttr(ppd, "DefaultResolution", NULL);

      if (attr)
      {
        char *x = strstr(attr->value, "x");

        if (x)
          sscanf (x+1, "%ddpi", &result->resolution);
        else
          sscanf (attr->value, "%ddpi", &result->resolution);
      }
      else
        result->resolution = 300;

      while(result->resolution>360)
        result->resolution /= 2.0;

      ppdClose(ppd);
      unlink(PPDFile);
    }
  }

  cupsFreeDests(num_dests, dests);
  return result;
}

static int _dest_cb(void *user_data, unsigned flags, cups_dest_t *dest)
{
  const dt_prtctl_t *pctl = (dt_prtctl_t *)user_data;
  const char *psvalue = cupsGetOption("printer-state", dest->num_options, dest->options);

  // check that the printer is ready
  if (psvalue!=NULL && strtol(psvalue, NULL, 10) < IPP_PRINTER_STOPPED)
  {
    dt_printer_info_t *pr = dt_get_printer_info(dest->name);
    if (pctl->cb) pctl->cb(pr, pctl->user_data);
    free(pr);
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
#if ((CUPS_VERSION_MAJOR == 1) && (CUPS_VERSION_MINOR >= 6)) || CUPS_VERSION_MAJOR > 1
  res = cupsEnumDests(CUPS_MEDIA_FLAGS_DEFAULT, 30000, &_cancel, 0, 0, _dest_cb, pctl);
#else
  cups_dest_t *dests;
  const int num_dests = cupsGetDests(&dests);
  for (int k=0; k<num_dests; k++)
  {
    _dest_cb((void *)pctl, 0, &dests[k]);
  }
  cupsFreeDests(num_dests, dests);
  res=1;
#endif
  g_free(pctl);
  return !res;
}

void dt_printers_abort_discovery(void)
{
  _cancel = 1;
}

void dt_printers_discovery(void (*cb)(dt_printer_info_t *pr, void *user_data), void *user_data)
{
  dt_prtctl_t *prtctl = g_malloc0(sizeof(dt_prtctl_t));

  prtctl->cb = cb;
  prtctl->user_data = user_data;

  // asynchronously checks for available printers
  dt_job_t *job = dt_control_job_create(&_detect_printers_callback, "detect connected printers");
  if(job)
  {
    dt_control_job_set_params(job, prtctl);
    dt_control_add_job(darktable.control, DT_JOB_QUEUE_SYSTEM_BG, job);
  }
}

static int paper_exists(GList *papers, const char *name)
{
  if (strstr(name,"custom_") == name)
    return 1;

  GList *p = papers;
  while (p)
  {
    const dt_paper_info_t *pi = (dt_paper_info_t*)p->data;
    if (!strcmp(pi->name,name) || !strcmp(pi->common_name,name))
      return 1;
    p = g_list_next (p);
  }
  return 0;
}

dt_paper_info_t *dt_get_paper(GList *papers, const char *name)
{
  GList *p = papers;
  dt_paper_info_t *result = NULL;

  while (p)
  {
    dt_paper_info_t *pi = (dt_paper_info_t*)p->data;
    if (!strcmp(pi->name,name) || !strcmp(pi->common_name,name))
    {
      result = pi;
      break;
    }
    p = g_list_next (p);
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

GList *dt_get_papers(const char *printer_name)
{
  GList *result = NULL;

#if ((CUPS_VERSION_MAJOR == 1) && (CUPS_VERSION_MINOR >= 6)) || CUPS_VERSION_MAJOR > 1
  cups_dest_t *dests;
  int num_dests = cupsGetDests(&dests);
  cups_dest_t *dest = cupsGetDest(printer_name, NULL, num_dests, dests);

  int cancel;
  const size_t ressize = 1024;
  char resource[ressize];

  cancel = 0; // important

  if (dest)
  {
    http_t *hcon = cupsConnectDest (dest, 0, 2000, &cancel, resource, ressize, NULL, (void *)NULL);

    if (hcon)
    {
      cups_size_t size;
      cups_dinfo_t *info = cupsCopyDestInfo (hcon, dest);
      const int count = cupsGetDestMediaCount(hcon, dest, info, CUPS_MEDIA_FLAGS_DEFAULT);
      for (int k=0; k<count; k++)
      {
        if (cupsGetDestMediaByIndex(hcon, dest, info, k, CUPS_MEDIA_FLAGS_DEFAULT, &size))
        {
          if (!paper_exists(result,size.media))
          {
            pwg_media_t *med = pwgMediaForPWG (size.media);
            char common_name[MAX_NAME] = { 0 };

            if (med->ppd)
              g_strlcpy(common_name, med->ppd, sizeof(common_name));
            else
              g_strlcpy(common_name, size.media, sizeof(common_name));

            dt_paper_info_t *paper = (dt_paper_info_t*)malloc(sizeof(dt_paper_info_t));
            g_strlcpy(paper->name, size.media, sizeof(paper->name));
            g_strlcpy(paper->common_name, common_name, sizeof(paper->common_name));
            paper->width = (double)size.width / 100.0;
            paper->height = (double)size.length / 100.0;
            result = g_list_append (result, paper);
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
#endif

  // check now PPD page sizes

  const char *PPDFile = cupsGetPPD(printer_name);
  ppd_file_t *ppd = ppdOpenFile(PPDFile);

  if (ppd)
  {
    ppd_size_t *size = ppd->sizes;

    for (int k=0; k<ppd->num_sizes; k++)
    {
      if (!paper_exists(result,size->name))
      {
        dt_paper_info_t *paper = (dt_paper_info_t*)malloc(sizeof(dt_paper_info_t));
        g_strlcpy(paper->name, size->name, MAX_NAME);
        g_strlcpy(paper->common_name, size->name, MAX_NAME);
        paper->width = (double)dt_pdf_point_to_mm(size->width);
        paper->height = (double)dt_pdf_point_to_mm(size->length);
        result = g_list_append (result, paper);
      }
      size++;
    }

    ppdClose(ppd);
    unlink(PPDFile);
  }

  result = g_list_sort_with_data (result, (GCompareDataFunc)sort_papers, NULL);
  return result;
}

void dt_print_file(const int32_t imgid, const char *filename, const dt_print_info_t *pinfo)
{
  // first for safety check that filename exists and is readable

  if (!g_file_test(filename, G_FILE_TEST_IS_REGULAR))
  {
    dt_control_log(_("file `%s' to print not found for image %d on `%s'"), filename, imgid, pinfo->printer.name);
    return;
  }

  cups_dest_t *dests;
  int num_dests = cupsGetDests(&dests);
  cups_dest_t *dest = cupsGetDest(pinfo->printer.name, NULL, num_dests, dests);

  cups_option_t *options = NULL;
  int num_options = 0;

  for (int j = 0; j < dest->num_options; j ++)
    if (cupsGetOption(dest->options[j].name, num_options,
                      options) == NULL)
      num_options = cupsAddOption(dest->options[j].name,
                                  dest->options[j].value,
                                  num_options, &options);

  cupsFreeDests(num_dests, dests);

  // disable cm on CUPS, this is important as dt does the cm

  if (*pinfo->printer.profile)
    num_options = cupsAddOption("cm-calibration", "true", num_options, &options);

  // media to print on

  num_options = cupsAddOption("media", pinfo->paper.name, num_options, &options);

  // never print two-side

  num_options = cupsAddOption("sides", "one-sided", num_options, &options);

  // and a single image per page

  num_options = cupsAddOption("number-up", "1", num_options, &options);

  // if the printer has no hardward margins activate the borderless mode

  if (pinfo->printer.hw_margin_top == 0 || pinfo->printer.hw_margin_bottom == 0
      || pinfo->printer.hw_margin_left == 0 || pinfo->printer.hw_margin_right == 0)
  {
    // there is many variant for this parameter
    num_options = cupsAddOption("StpFullBleed", "true", num_options, &options);
    num_options = cupsAddOption("STP_FullBleed", "true", num_options, &options);
    num_options = cupsAddOption("Borderless", "true", num_options, &options);
  }

  if (pinfo->page.landscape)
    num_options = cupsAddOption("landscape", "true", num_options, &options);
  else
    num_options = cupsAddOption("landscape", "false", num_options, &options);

  // print lp options

  dt_print(DT_DEBUG_PRINT, "[print] printer options (%d)\n", num_options);
  for (int k=0; k<num_options; k++)
    dt_print(DT_DEBUG_PRINT, "[print]   %s=%s\n", options[k].name, options[k].value);

  const int job_id = cupsPrintFile(pinfo->printer.name, filename,  "darktable", num_options, options);

  if (job_id == 0)
    dt_control_log(_("error while printing image %d on `%s'"), imgid, pinfo->printer.name);
  else
    dt_control_log(_("printing image %d on `%s'"), imgid, pinfo->printer.name);

  cupsFreeOptions (num_options, options);
}

static void _get_image_dimension (int32_t imgid, int32_t *iwidth, int32_t *iheight)
{
  dt_develop_t dev;
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');

  dt_dev_init(&dev, 0);
  dt_dev_load_image(&dev, imgid);
  const dt_image_t *img = &dev.image_storage;

  dt_dev_pixelpipe_t pipe;
  int wd = img->width, ht = img->height;
  int res = dt_dev_pixelpipe_init_dummy(&pipe, wd, ht);
  if(res)
  {
    // set mem pointer to 0, won't be used.
    dt_dev_pixelpipe_set_input(&pipe, &dev, (float *)buf.buf, wd, ht, 1.0f);
    dt_dev_pixelpipe_create_nodes(&pipe, &dev);
    dt_dev_pixelpipe_synch_all(&pipe, &dev);
    dt_dev_pixelpipe_get_dimensions(&pipe, &dev, pipe.iwidth, pipe.iheight, &pipe.processed_width,
                                    &pipe.processed_height);
    wd = pipe.processed_width;
    ht = pipe.processed_height;
    dt_dev_pixelpipe_cleanup(&pipe);
  }
  dt_dev_cleanup(&dev);
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  *iwidth = wd;
  *iheight = ht;
}

void dt_get_print_layout(const int32_t imgid, const dt_print_info_t *prt,
                         const int32_t area_width, const int32_t area_height,
                         int32_t *iwpix, int32_t *ihpix,
                         int32_t *px,    int32_t *py,    int32_t *pwidth, int32_t *pheight,
                         int32_t *ax,    int32_t *ay,    int32_t *awidth, int32_t *aheight,
                         int32_t *ix,    int32_t *iy,    int32_t *iwidth, int32_t *iheight)
{
  /* this is where the layout is done for the display and for the print too. So this routine is one
     of the most critical for the print circuitry. */

  double width, height;

  // page w/h
  double pg_width  = prt->paper.width;
  double pg_height = prt->paper.height;

  if (area_width==0)
    width = pg_width;
  else
    width = area_width;

  if (area_height==0)
    height = pg_height;
  else
    height = area_height;

  /* here, width and height correspond to the area for the picture */

  // non-printable
  double np_top = prt->printer.hw_margin_top;
  double np_left = prt->printer.hw_margin_left;
  double np_right = prt->printer.hw_margin_right;
  double np_bottom = prt->printer.hw_margin_bottom;

  /* do some arrangements for the landscape mode. */

  if (prt->page.landscape)
  {
    double tmp = pg_width;
    pg_width = pg_height;
    pg_height = tmp;

    //  only invert if we did not get a specific area
    if (area_width == 0 && area_height == 0)
    {
      tmp = width;
      width = height;
      height = tmp;
    }

    // rotate the non-printable margins
    tmp       = np_top;
    np_top    = np_right;
    np_right  = np_bottom;
    np_bottom = np_left;
    np_left   = tmp;
  }

  // the image area aspect
  const double a_aspect = (double)width / (double)height;

  // page aspect
  const double pg_aspect = pg_width / pg_height;

  // display page
  int32_t p_bottom, p_right;

  if (a_aspect > pg_aspect)
  {
    *px = (width - (height * pg_aspect)) / 2;
    *py = 0;
    p_bottom = height;
    p_right = width - *px;
  }
  else
  {
    *px = 0;
    *py = (height - (width / pg_aspect)) / 2;
    p_right = width;
    p_bottom = height - *py;
  }

  *pwidth = p_right - *px;
  *pheight = p_bottom - *py;

  // page margins, note that we do not want to change those values for the landscape mode.
  // these margins are those set by the user from the GUI, and the top margin is *always*
  // at the top of the screen.

  const double border_top = prt->page.margin_top;
  const double border_left = prt->page.margin_left;
  const double border_right = prt->page.margin_right;
  const double border_bottom = prt->page.margin_bottom;

  // display picture area, that is removing the non printable areas and user's margins

  const int32_t bx = *px + ((np_left + border_left) / pg_width) * (*pwidth);
  const int32_t by = *py + ((np_top + border_top)/ pg_height) * (*pheight);
  const int32_t bb = p_bottom - ((np_bottom + border_bottom) / pg_height) * (*pheight);
  const int32_t br = p_right - ((np_right + border_right) / pg_width) * (*pwidth);

  // now we have the printable area (ax, ay) -> (ax + awidth, ay + aheight)

  *ax      = bx;
  *ay      = by;
  *awidth  = br - bx;
  *aheight = bb - by;

  // get the image dimensions if needed

  if (*iwpix <= 0 || *ihpix <= 0)
    _get_image_dimension (imgid, iwpix, ihpix);

  // compute the scaling for the image to fit into the printable area

  double scale;

  *iwidth = *iwpix;
  *iheight = *ihpix;

  if (*iwidth > *awidth)
  {
    scale =  (double)(*awidth) / (double)*iwidth;
    *iwidth = *awidth;
    *iheight = (int32_t)(((double)*iheight + 0.5) * scale);
  }

  if (*iheight > *aheight)
  {
    scale = (double)(*aheight) / (double)*iheight;
    *iheight = *aheight;
    *iwidth = (int32_t)(((double)*iwidth + 0.5) * scale);
  }

  // now the image position (top-left corner coordinates) in the display area depending on the page
  // alignment set by the user.

  switch (prt->page.alignment)
  {
  case top_left:
    *ix = bx;
    *iy = by;
    break;
  case top:
    *ix = bx + (*awidth - *iwidth) / 2;
    *iy = by;
    break;
  case top_right:
    *ix = br - *iwidth;
    *iy = by;
    break;
  case left:
    *ix = bx;
    *iy = by + (*aheight - *iheight) / 2;
    break;
  case center:
    *ix = bx + (*awidth - *iwidth) / 2;
    *iy = by + (*aheight - *iheight) / 2;
    break;
  case right:
    *ix = br - *iwidth;
    *iy = by + (*aheight - *iheight) / 2;
    break;
  case bottom_left:
    *ix = bx;
    *iy = bb - *iheight;
    break;
  case bottom:
    *ix = bx + (*awidth - *iwidth) / 2;
    *iy = bb - *iheight;
    break;
  case bottom_right:
    *ix = br - *iwidth;
    *iy = bb - *iheight;
    break;
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
