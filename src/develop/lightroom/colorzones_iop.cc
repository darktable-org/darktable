#include "develop/lightroom/colorzones_iop.h"

#include <map>

#include "develop/lightroom/add_history.h"
#include "develop/lightroom/import_value.h"

namespace lightroom
{

std::string ColorZonesIop::operation_name() const
{
  return "colorzones";
}

bool ColorZonesIop::import(xmlDocPtr, xmlNodePtr, const xmlChar *name, const xmlChar *value)
{
  static std::map<int, std::string> const channels{
    { 0, "Luminance" },
    { 1, "Saturation" },
    { 2, "Hue" },
  };
  static std::map<int, std::string> const colors{
    { 0, "Red" },  { 1, "Orange" }, { 2, "Yellow" }, { 3, "Green" },
    { 4, "Aqua" }, { 5, "Blue" },   { 6, "Purple" }, { 7, "Magenta" },
  };
  for(auto &&channel : channels)
    for(auto &&color : colors)
    {
      auto adjustment_name = channel.second + "Adjustment" + color.second;
      if(import_value(equalizer_y_[channel.first][color.first], adjustment_name.c_str(), name, value)) return true;
    }
  return false;
}

bool ColorZonesIop::apply(int imgid) const
{
  if(!dev()) return false;
  bool has_any = false;
  for(const auto &i : equalizer_y_)
    for(int j : i)
      if(j != 0)
      {
        has_any = true;
        break;
      }
  if(!has_any) return false;

  static constexpr float factor[] = {
    4.0 / 9.0, // lightness factor adjustment (use 4 out of 9 boxes in colorzones)
    1,
    3.0 / 9.0, // hue factor adjustment (use 3 out of 9 boxes in colorzones)
  };

  enum dt_iop_colorzones_channel_t
  {
    DT_IOP_COLORZONES_L = 0,
    DT_IOP_COLORZONES_C = 1,
    DT_IOP_COLORZONES_h = 2
  };

  struct params_t
  {
    int32_t channel;
    float equalizer_x[3][8];
    float equalizer_y[3][8];
  };

  params_t params = {};
  params.channel = DT_IOP_COLORZONES_h;
  for(int i = 0; i < 3; i++)
    for(int j = 0; j < 8; j++)
    {
      params.equalizer_x[i][j] = float(j) / 7;
      params.equalizer_y[i][j] = 0.5f + factor[i] * (float)equalizer_y_[i][j] / 200.0;
    }
  add_history(imgid, dev(), operation_name(), 2, &params, sizeof(params));

  return true;
}

} // namespace lightroom
