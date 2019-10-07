#include "develop/lightroom/grain_iop.h"

#include "develop/lightroom/add_history.h"
#include "develop/lightroom/import_value.h"
#include "develop/lightroom/interpolate.h"

namespace lightroom
{

std::string GrainIop::operation_name() const
{
  return "grain";
}

bool GrainIop::import(xmlDocPtr, xmlNodePtr, const xmlChar *name, const xmlChar *value)
{
  return import_value(amount_, "GrainAmount", name, value)
         || import_value(frequency_, "GrainFrequency", name, value);
}

bool GrainIop::apply(int imgid) const
{
  if(!amount_) return false;
  if(!dev()) return false;

  enum _dt_iop_grain_channel_t
  {
    DT_GRAIN_CHANNEL_HUE = 0,
    DT_GRAIN_CHANNEL_SATURATION,
    DT_GRAIN_CHANNEL_LIGHTNESS,
    DT_GRAIN_CHANNEL_RGB
  };

  struct params_t
  {
    _dt_iop_grain_channel_t channel;
    float scale;
    float strength;
  };

  static Interpolator const amount_table{ { { 0, 0 }, { 25, 20 }, { 50, 40 }, { 100, 80 } } };
  static Interpolator const frequency_table{ { { 0, 100 }, { 50, 100 }, { 75, 400 }, { 100, 800 } } };

  params_t params = { DT_GRAIN_CHANNEL_HUE, frequency_table((float)frequency_), amount_table((float)amount_) };

  add_history(imgid, dev(), operation_name(), 1, &params, sizeof(params));

  return true;
}

} // namespace lightroom
