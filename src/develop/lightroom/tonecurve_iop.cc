#include "develop/lightroom/tonecurve_iop.h"

#include "develop/lightroom/add_history.h"
#include "develop/lightroom/import_value.h"

extern "C"
{
#include "common/curve_tools.h"
}

namespace lightroom
{

std::string ToneCurveIop::operation_name() const
{
  return "tonecurve";
}

bool ToneCurveIop::import(xmlDocPtr doc, xmlNodePtr node, const xmlChar *name, const xmlChar *value)
{
  if(import_value(ptc_value_[0], "ParametricShadows", name, value)) return true;
  if(import_value(ptc_value_[1], "ParametricDarks", name, value)) return true;
  if(import_value(ptc_value_[2], "ParametricLights", name, value)) return true;
  if(import_value(ptc_value_[3], "ParametricHighlights", name, value)) return true;
  if(import_value(ptc_split_[0], "ParametricShadowSplit", name, value)) return true;
  if(import_value(ptc_split_[1], "ParametricMidtoneSplit", name, value)) return true;
  if(import_value(ptc_split_[2], "ParametricHighlightSplit", name, value)) return true;
  if(!xmlStrcmp(name, (const xmlChar *)"ToneCurveName2012"))
  {
    if(!xmlStrcmp(value, (const xmlChar *)"Linear"))
      curve_kind_ = linear;
    else if(!xmlStrcmp(value, (const xmlChar *)"Medium Contrast"))
      curve_kind_ = medium_contrast;
    else if(!xmlStrcmp(value, (const xmlChar *)"Strong Contrast"))
      curve_kind_ = strong_contrast;
    else if(!xmlStrcmp(value, (const xmlChar *)"Custom"))
      curve_kind_ = custom;
    return true;
  }
  if(!xmlStrcmp(name, (const xmlChar *)"ToneCurvePV2012"))
  {
    xmlNodePtr tcNode = node;

    while(tcNode)
    {
      if(!xmlStrcmp(tcNode->name, (const xmlChar *)"li"))
      {
        xmlChar *cvalue = xmlNodeListGetString(doc, tcNode->xmlChildrenNode, 1);

        node_t n;
        if(sscanf((const char *)cvalue, "%f, %f", &n.x, &n.y)) curve_pts_.push_back(n);
        xmlFree(cvalue);
      }
      if(curve_pts_.size() == max_nodes) break;
      tcNode = tcNode->next;
    }
    return true;
  }
  return false;
}

bool ToneCurveIop::apply(int imgid) const
{
  if(curve_kind_ == linear && ptc_value_[0] == 0 && ptc_value_[1] == 0 && ptc_value_[2] == 0 && ptc_value_[3] == 0)
    return false;
  if(!dev()) return false;

  enum tonecurve_channel_t
  {
    ch_L = 0,
    ch_a = 1,
    ch_b = 2,
    ch_max = 3
  };

  struct params_t
  {
    // three curves (L, a, b) with max number of nodes
    node_t tonecurve[3][max_nodes];
    int tonecurve_nodes[3]{};
    int tonecurve_type[3]{};
    int tonecurve_autoscale_ab{};
    int tonecurve_preset{};
  };

  params_t params;

  int const total_pts = (curve_kind_ == custom) ? int(curve_pts_.size()) : 6;

  params.tonecurve_nodes[ch_L] = total_pts;
  params.tonecurve_nodes[ch_a] = 7;
  params.tonecurve_nodes[ch_b] = 7;
  params.tonecurve_type[ch_L] = CUBIC_SPLINE;
  params.tonecurve_type[ch_a] = CUBIC_SPLINE;
  params.tonecurve_type[ch_b] = CUBIC_SPLINE;
  params.tonecurve_autoscale_ab = 1;
  params.tonecurve_preset = 0;

  float const linear_ab[7] = { 0.0, 0.08, 0.3, 0.5, 0.7, 0.92, 1.0 };

  // linear a, b curves
  for(int k = 0; k < 7; k++) params.tonecurve[ch_a][k].x = linear_ab[k];
  for(int k = 0; k < 7; k++) params.tonecurve[ch_a][k].y = linear_ab[k];
  for(int k = 0; k < 7; k++) params.tonecurve[ch_b][k].x = linear_ab[k];
  for(int k = 0; k < 7; k++) params.tonecurve[ch_b][k].y = linear_ab[k];

  // Set the base tonecurve

  if(curve_kind_ == linear)
  {
    params.tonecurve[ch_L][0].x = 0.0;
    params.tonecurve[ch_L][0].y = 0.0;
    params.tonecurve[ch_L][1].x = ptc_split_[0] / 2.0f / 100.0f;
    params.tonecurve[ch_L][1].y = ptc_split_[0] / 2.0f / 100.0f;
    params.tonecurve[ch_L][2].x = ptc_split_[1] - (ptc_split_[1] - ptc_split_[0]) / 2.0f / 100.0f;
    params.tonecurve[ch_L][2].y = ptc_split_[1] - (ptc_split_[1] - ptc_split_[0]) / 2.0f / 100.0f;
    params.tonecurve[ch_L][3].x = ptc_split_[1] + (ptc_split_[2] - ptc_split_[1]) / 2.0f / 100.0f;
    params.tonecurve[ch_L][3].y = ptc_split_[1] + (ptc_split_[2] - ptc_split_[1]) / 2.0f / 100.0f;
    params.tonecurve[ch_L][4].x = ptc_split_[2] + (1.0f - ptc_split_[2]) / 2.0f / 100.0f;
    params.tonecurve[ch_L][4].y = ptc_split_[2] + (1.0f - ptc_split_[2]) / 2.0f / 100.0f;
    params.tonecurve[ch_L][5].x = 1.0;
    params.tonecurve[ch_L][5].y = 1.0;
  }
  else
  {
    for(int k = 0; k < total_pts; k++)
    {
      params.tonecurve[ch_L][k].x = curve_pts_[k].x / 255.0f;
      params.tonecurve[ch_L][k].y = curve_pts_[k].y / 255.0f;
    }
  }

  if(curve_kind_ != custom)
  {
    // set shadows/darks/lights/highlight adjustments

    params.tonecurve[ch_L][1].y += params.tonecurve[ch_L][1].y * (float(ptc_value_[0]) / 100.0f);
    params.tonecurve[ch_L][2].y += params.tonecurve[ch_L][2].y * (float(ptc_value_[1]) / 100.0f);
    params.tonecurve[ch_L][3].y += params.tonecurve[ch_L][3].y * (float(ptc_value_[2]) / 100.0f);
    params.tonecurve[ch_L][4].y += params.tonecurve[ch_L][4].y * (float(ptc_value_[3]) / 100.0f);

    if(params.tonecurve[ch_L][1].y > params.tonecurve[ch_L][2].y)
      params.tonecurve[ch_L][1].y = params.tonecurve[ch_L][2].y;
    if(params.tonecurve[ch_L][3].y > params.tonecurve[ch_L][4].y)
      params.tonecurve[ch_L][4].y = params.tonecurve[ch_L][3].y;
  }

  add_history(imgid, dev(), operation_name(), 3, &params, sizeof(params));

  return true;
}

} // namespace lightroom
