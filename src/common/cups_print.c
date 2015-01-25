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

gboolean is_printer_available(void)
{
  cups_dest_t *dests;
  const int num_dests = cupsGetDests(&dests);
  cupsFreeDests(num_dests, dests);
  return (gboolean) num_dests > 0;
}

GList *dt_get_printers(void)
{
  cups_dest_t *dests;
  int num_dests = cupsGetDests(&dests);
  int k;
  GList *result = NULL;

  for (k=0; k<num_dests; k++)
  {
    const cups_dest_t *dest = &dests[k];
    const char *psvalue = cupsGetOption("printer-state", dest->num_options, dest->options);

    // check that the printer is ready
    if (strtol(psvalue, NULL, 10) < IPP_PRINTER_STOPPED)
    {
      dt_printer_info_t *pr = dt_get_printer_info(dest->name);
      result = g_list_append(result,pr);
    }
    else if (darktable.unmuted & DT_DEBUG_PRINT)
    {
      fprintf(stderr, "[print] skip printer %s as stopped\n", dest->name);
    }
  }

  cupsFreeDests(num_dests, dests);

  return result;
}

static int paper_exists(GList *papers, const char *name)
{
  GList *p = papers;
  while (p)
  {
    dt_paper_info_t *pi = (dt_paper_info_t*)p->data;
    if (!strcmp(pi->name,name))
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

GList *dt_get_papers(const char *printer_name)
{
  cups_dest_t *dests;
  int num_dests = cupsGetDests(&dests);
  cups_dest_t *dest = cupsGetDest(printer_name, NULL, num_dests, dests);
  GList *result = NULL;

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
          pwg_media_t *med = pwgMediaForPWG (size.media);

          if (med->ppd && !paper_exists(result,size.media))
          {
            dt_paper_info_t *paper = (dt_paper_info_t*)malloc(sizeof(dt_paper_info_t));
            g_strlcpy(paper->name, size.media, MAX_NAME);
            g_strlcpy(paper->common_name, med->ppd, MAX_NAME);
            paper->width = (double)size.width / 100.0;
            paper->height = (double)size.length / 100.0;
            result = g_list_append (result, paper);
          }
        }
      }
    }
    else if (darktable.unmuted & DT_DEBUG_PRINT)
    {
      fprintf(stderr, "[print] cannot connect to printer %s (cancel=%d)\n", printer_name, cancel);
    }

  }
  return result;
}

void dt_print_file(const int32_t imgid, const char *filename, const dt_print_info_t *pinfo)
{
  // first for safety check that filename exists and is readable

  if (!g_file_test(filename, G_FILE_TEST_IS_REGULAR))
  {
    dt_control_log(_("file '%s' to print not found for image `%d' on %s"), filename, imgid, pinfo->printer.name);
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
    // there is many vaariant for this parameter
    num_options = cupsAddOption("StpFullBleed", "true", num_options, &options);
    num_options = cupsAddOption("STP_FullBleed", "true", num_options, &options);
    num_options = cupsAddOption("Borderless", "true", num_options, &options);
  }

  if (pinfo->page.landscape)
    num_options = cupsAddOption("landscape", "true", num_options, &options);
  else
    num_options = cupsAddOption("landscape", "false", num_options, &options);

  // print lp options

  if (darktable.unmuted & DT_DEBUG_PRINT)
  {
    fprintf (stderr, "[print] printer options (%d)\n", num_options);
    for (int k=0; k<num_options; k++)
    {
      fprintf (stderr, "[print]   %s=%s\n", options[k].name, options[k].value);
    }
  }

  const int job_id = cupsPrintFile(pinfo->printer.name, filename,  "darktable", num_options, options);

  if (job_id == 0)
    dt_control_log(_("error while printing image %d on `%s'"), imgid, pinfo->printer.name);
  else
    dt_control_log(_("printing image %d on `%s'"), imgid, pinfo->printer.name);

  cupsFreeOptions (num_options, options);
}

static void _get_image_dimension (int32_t imgid, int32_t *iwidth, int32_t *iheight)
{
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_3, DT_MIPMAP_BEST_EFFORT, 'r');

  // more than the image dimension we want the image aspect to be preserved
  *iwidth = buf.width * 4;
  *iheight = buf.height * 4;

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
}

void dt_get_print_layout(const int32_t imgid, const dt_print_info_t *prt,
                         const int32_t area_width, const int32_t area_height,
                         int32_t *px, int32_t *py, int32_t *pwidth, int32_t *pheight,
                         int32_t *ax, int32_t *ay, int32_t *awidth, int32_t *aheight,
                         int32_t *ix, int32_t *iy, int32_t *iwidth, int32_t *iheight)
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

  /* do some arangments for the landscape mode. */

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
  // at the top of the screeen.

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

  // get the image dimensions

  // if we got iwidth and iheight it is the exact size of the image, otherwise let's computer the
  // *max* image dimension (not counting the crop). This is not a problem for displaying the image
  // but for printing we really want to have here the real size of the image.

  if (*iwidth==0 || *iheight==0)
    _get_image_dimension (imgid, iwidth, iheight);

  // compute the scaling for the image to fit into the printable area

  double scale;

  if (*iwidth > *awidth)
  {
    scale =  (double)(*awidth) / (double)*iwidth;
    *iwidth = *awidth;
    *iheight *= scale;
  }

  if (*iheight > *aheight)
  {
    scale = (double)(*aheight) / (double)*iheight;
    *iheight = *aheight;
    *iwidth *= scale;
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
