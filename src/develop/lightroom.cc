/*
    This file is part of darktable,
    copyright (c) 2013--2017 pascal obry.

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

#include <cstdlib>
#include <cstring>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <sstream>
#include <sys/stat.h>
#include <vector>

extern "C"
{
#include "common/darktable.h"
#include "common/debug.h"
#include "control/control.h"
#include "develop/lightroom.h"
}

//
// The basic structure here is fairly simple: dt_lightroom_import() is the entry point. It looks for a Lightroom
// XMP file, then if it finds one, it calls Iops::import() to pull all the interesting settings from that XMP, then
// Iops::apply() to apply the imported settings to the database.
//
// The Iops class is modular, divided into various implementations of lightroom::Iop, each of which mirrors a
// Darktable iop.
//
// To add a new import module, subclass lightroom::Iop in `develop/lightroom/iop.h` - see comments in that header
// for details and look at existing modules for examples. Add the source for your module to `src/CMakeLists.txt`
// with the others. Add the header to the list below. Add a member to the Iops class below, initialize it in the
// Iops constructor, and add it to the Iops::iops list in the appropriate order.
//

#include "develop/lightroom/bilat_iop.h"
#include "develop/lightroom/clipping_iop.h"
#include "develop/lightroom/colorin_iop.h"
#include "develop/lightroom/colorlabel_iop.h"
#include "develop/lightroom/colorzones_iop.h"
#include "develop/lightroom/dimensions_iop.h"
#include "develop/lightroom/exposure_iop.h"
#include "develop/lightroom/flip_iop.h"
#include "develop/lightroom/geotagging_iop.h"
#include "develop/lightroom/grain_iop.h"
#include "develop/lightroom/rating_iop.h"
#include "develop/lightroom/splittoning_iop.h"
#include "develop/lightroom/spot_iop.h"
#include "develop/lightroom/tags_iop.h"
#include "develop/lightroom/tonecurve_iop.h"
#include "develop/lightroom/vignette_iop.h"

class Iops
{
public:
  explicit Iops(dt_develop_t const *dev)
    : tags{ dev }
    , rating{ dev }
    , colorlabel{ dev }
    , geotagging{ dev }
    , dimensions{ dev }
    , colorin{ dev }
    , flip{ dev }
    , clipping{ dev, flip }
    , exposure{ dev }
    , bilat{ dev }
    , tonecurve{ dev }
    , colorzones{ dev }
    , splittoning{ dev }
    , grain{ dev }
    , vignette{ dev, dimensions, clipping }
    , spot{ dev, flip }
  {
  }

  void import(xmlDocPtr doc, const xmlChar *name, const xmlChar *value, xmlNodePtr node)
  {
    for(auto &&iop : iops)
      if(iop->import(doc, node, name, value)) return;
  }

  std::vector<std::string> apply(int imgid) const
  {
    std::vector<std::string> imported;
    for(auto &&iop : iops)
      if(iop->apply(imgid)) imported.push_back(iop->operation_name());
    return imported;
  }

private:
  lightroom::TagsIop tags;
  lightroom::RatingIop rating;
  lightroom::ColorLabelIop colorlabel;
  lightroom::GeotaggingIop geotagging;
  lightroom::DimensionsIop dimensions;
  lightroom::ColorInIop colorin;
  lightroom::FlipIop flip;
  lightroom::ClippingIop clipping;
  lightroom::ExposureIop exposure;
  lightroom::BilatIop bilat;
  lightroom::ToneCurveIop tonecurve;
  lightroom::ColorZonesIop colorzones;
  lightroom::SplitToningIop splittoning;
  lightroom::GrainIop grain;
  lightroom::VignetteIop vignette;
  lightroom::SpotIop spot;

  std::vector<lightroom::Iop *> iops = {
    &tags,     &rating, &colorlabel, &geotagging, &dimensions,  &colorin, &flip,     &clipping,
    &exposure, &bilat,  &tonecurve,  &colorzones, &splittoning, &grain,   &vignette, &spot,
  };
};

static char *dt_get_lightroom_xmp(int imgid)
{
  char pathname[DT_MAX_FILENAME_LEN];
  gboolean from_cache = TRUE;

  // Get full pathname
  dt_image_full_path(imgid, pathname, DT_MAX_FILENAME_LEN, &from_cache);

  // Look for extension
  char *pos = strrchr(pathname, '.');

  if(pos == nullptr) return nullptr;

  // If found, replace extension with xmp
  strncpy(pos + 1, "xmp", 4);
  if(g_file_test(pathname, G_FILE_TEST_EXISTS)) return g_strdup(pathname);

  strncpy(pos + 1, "XMP", 4);
  if(g_file_test(pathname, G_FILE_TEST_EXISTS)) return g_strdup(pathname);

  return nullptr;
}

/* _has_list returns true if the node contains a list of value */
static int _has_list(char *name)
{
  return !strcmp(name, "subject") || !strcmp(name, "hierarchicalSubject") || !strcmp(name, "RetouchInfo")
         || !strcmp(name, "ToneCurvePV2012") || !strcmp(name, "title") || !strcmp(name, "description")
         || !strcmp(name, "creator") || !strcmp(name, "publisher") || !strcmp(name, "rights");
};

/* handle a specific xpath */
static void _handle_xpath(xmlDoc *doc, xmlXPathContext *ctx, const xmlChar *xpath, Iops *iops)
{
  xmlXPathObject *xpathObj = xmlXPathEvalExpression(xpath, ctx);

  if(xpathObj != nullptr)
  {
    const xmlNodeSetPtr xnodes = xpathObj->nodesetval;
    const int n = xnodes->nodeNr;

    for(int k = 0; k < n; k++)
    {
      const xmlNode *node = xnodes->nodeTab[k];

      if(_has_list((char *)node->name))
      {
        xmlNodePtr listnode = node->xmlChildrenNode;
        if(listnode) listnode = listnode->next;
        if(listnode) listnode = listnode->xmlChildrenNode;
        if(listnode) listnode = listnode->next;
        if(listnode) iops->import(doc, node->name, nullptr, listnode);
      }
      else
      {
        const xmlChar *value = xmlNodeListGetString(doc, node->children, 1);
        iops->import(doc, node->name, value, nullptr);
      }
    }

    xmlXPathFreeObject(xpathObj);
  }
}

void dt_lightroom_import(int imgid, dt_develop_t *dev, gboolean iauto)
{
  // Get full pathname
  char *pathname = dt_get_lightroom_xmp(imgid);

  if(!pathname)
  {
    if(!iauto) dt_control_log(_("cannot find lightroom XMP!"));
    return;
  }

  // Load LR xmp

  xmlDocPtr doc;
  xmlNodePtr entryNode;

  // Parse xml document

  doc = xmlParseEntity(pathname);

  if(doc == nullptr)
  {
    g_free(pathname);
    return;
  }

  // Enter first node, xmpmeta

  entryNode = xmlDocGetRootElement(doc);

  if(entryNode == nullptr)
  {
    g_free(pathname);
    xmlFreeDoc(doc);
    return;
  }

  if(xmlStrcmp(entryNode->name, (const xmlChar *)"xmpmeta"))
  {
    if(!iauto) dt_control_log(_("`%s' not a lightroom XMP!"), pathname);
    g_free(pathname);
    return;
  }

  // Check that this is really a Lightroom document

  xmlXPathContextPtr xpathCtx = xmlXPathNewContext(doc);

  if(xpathCtx == nullptr)
  {
    g_free(pathname);
    xmlFreeDoc(doc);
    return;
  }

  xmlXPathRegisterNs(xpathCtx, BAD_CAST "stEvt", BAD_CAST "http://ns.adobe.com/xap/1.0/sType/ResourceEvent#");

  xmlXPathObjectPtr xpathObj = xmlXPathEvalExpression((const xmlChar *)"//@stEvt:softwareAgent", xpathCtx);

  if(xpathObj == nullptr)
  {
    if(!iauto) dt_control_log(_("`%s' not a lightroom XMP!"), pathname);
    xmlXPathFreeContext(xpathCtx);
    g_free(pathname);
    xmlFreeDoc(doc);
    return;
  }

  xmlNodeSetPtr xnodes = xpathObj->nodesetval;

  if(xnodes != nullptr && xnodes->nodeNr > 0)
  {
    xmlNodePtr xnode = xnodes->nodeTab[0];
    xmlChar *value = xmlNodeListGetString(doc, xnode->xmlChildrenNode, 1);

    if(!strstr((char *)value, "Lightroom"))
    {
      xmlXPathFreeContext(xpathCtx);
      xmlXPathFreeObject(xpathObj);
      xmlFreeDoc(doc);
      xmlFree(value);
      if(!iauto) dt_control_log(_("`%s' not a lightroom XMP!"), pathname);
      g_free(pathname);
      return;
    }
    xmlFree(value);
  }
  // we could bail out here if we ONLY wanted to load a file known to be from lightroom.
  // if we don't know who created it we will just import it however.
  //   else
  //   {
  //     xmlXPathFreeObject(xpathObj);
  //     xmlXPathFreeContext(xpathCtx);
  //     if(!iauto) dt_control_log(_("`%s' not a lightroom XMP!"), pathname);
  //     g_free(pathname);
  //     return;
  //   }

  // let's now parse the needed data

  Iops iops{ dev };

  // record the name-spaces needed for the parsing
  xmlXPathRegisterNs(xpathCtx, BAD_CAST "crs", BAD_CAST "http://ns.adobe.com/camera-raw-settings/1.0/");
  xmlXPathRegisterNs(xpathCtx, BAD_CAST "dc", BAD_CAST "http://purl.org/dc/elements/1.1/");
  xmlXPathRegisterNs(xpathCtx, BAD_CAST "tiff", BAD_CAST "http://ns.adobe.com/tiff/1.0/");
  xmlXPathRegisterNs(xpathCtx, BAD_CAST "xmp", BAD_CAST "http://ns.adobe.com/xap/1.0/");
  xmlXPathRegisterNs(xpathCtx, BAD_CAST "exif", BAD_CAST "http://ns.adobe.com/exif/1.0/");
  xmlXPathRegisterNs(xpathCtx, BAD_CAST "lr", BAD_CAST "http://ns.adobe.com/lightroom/1.0/");
  xmlXPathRegisterNs(xpathCtx, BAD_CAST "rdf", BAD_CAST "http://www.w3.org/1999/02/22-rdf-syntax-ns#");

  // All prefixes to parse from the XMP document
  static char const *names[] = { "crs", "dc", "tiff", "xmp", "exif", "lr", nullptr };

  for(int i = 0; names[i] != nullptr; i++)
  {
    char expr[50];

    /* Lr 7.0 CC (nodes) */
    snprintf(expr, sizeof(expr), "//%s:*", names[i]);
    _handle_xpath(doc, xpathCtx, (const xmlChar *)expr, &iops);

    /* Lr up to 6.0 (attributes) */
    snprintf(expr, sizeof(expr), "//@%s:*", names[i]);
    _handle_xpath(doc, xpathCtx, (const xmlChar *)expr, &iops);
  }

  xmlXPathFreeObject(xpathObj);
  xmlXPathFreeContext(xpathCtx);
  xmlFreeDoc(doc);

  //  Integrates into the history all the imported iop
  auto applied_names = iops.apply(imgid);

  if(dev != nullptr && dev->gui_attached && !applied_names.empty())
  {
    std::ostringstream imported;
    imported << dt_iop_get_localized_name(applied_names[0].c_str());
    for(size_t i = 1; i < applied_names.size(); ++i)
      imported << ", " << dt_iop_get_localized_name(applied_names[i].c_str());

    dt_control_log(ngettext("%s has been imported", "%s have been imported", applied_names.size()),
                   imported.str().c_str());

    if(!iauto)
    {
      /* signal history changed */
      dt_dev_reload_history_items(dev);
      dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
      /* update xmp file */
      dt_image_synch_xmp(imgid);
      dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
    }
  }
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
