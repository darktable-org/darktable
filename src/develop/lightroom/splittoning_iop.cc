#include "develop/lightroom/splittoning_iop.h"

#include "develop/lightroom/add_history.h"
#include "develop/lightroom/import_value.h"
#include "develop/lightroom/interpolate.h"

namespace lightroom
{

std::string SplitToningIop::operation_name() const
{
  return "splittoning";
}

bool SplitToningIop::import(xmlDocPtr, xmlNodePtr, const xmlChar *name, const xmlChar *value)
{
  return import_value(shadow_hue_, "SplitToningShadowHue", name, value)
         || import_value(shadow_saturation_, "SplitToningShadowSaturation", name, value)
         || import_value(highlight_hue_, "SplitToningHighlightHue", name, value)
         || import_value(highlight_saturation_, "SplitToningHighlightSaturation", name, value)
         || import_value(balance_, "SplitToningBalance", name, value);
}

bool SplitToningIop::apply(int imgid) const
{
  if(!shadow_hue_ && !shadow_saturation_ && !highlight_hue_ && !highlight_saturation_) return false;
  if(!dev()) return false;

  struct params_t
  {
    float shadow_hue;
    float shadow_saturation;
    float highlight_hue;
    float highlight_saturation;
    float balance;  // center luminance of gradient
    float compress; // Compress range
  };

  static Interpolator const balance_table{ { { -100, 100 }, { 0, 0 }, { 100, 0 } } };

  params_t params
      = { float(shadow_hue_) / 255,           float(shadow_saturation_) / 100, float(highlight_hue_) / 255,
          float(highlight_saturation_) / 100, balance_table(balance_),         50 };

  add_history(imgid, dev(), operation_name(), 1, &params, sizeof(params));

  return true;
}

} // namespace lightroom
