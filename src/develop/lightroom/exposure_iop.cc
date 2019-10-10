#include "develop/lightroom/exposure_iop.h"

#include "develop/lightroom/add_history.h"
#include "develop/lightroom/import_value.h"
#include "develop/lightroom/interpolate.h"

namespace lightroom
{

std::string ExposureIop::operation_name() const
{
  return "exposure";
}

bool ExposureIop::import(xmlDocPtr, xmlNodePtr, const xmlChar *name, const xmlChar *value)
{
  return import_value(black_, "Blacks", name, value) || import_value(black_, "Blacks2012", name, value) || import_value(black_, "Blacks2012", name, value) || import_value(exposure_, "Exposure", name, value) || import_value(exposure_, "Exposure2012", name, value);
}

bool ExposureIop::apply(int imgid) const
{
  if(exposure_ == 0 && !black_) return false;
  if(!dev()) return false;

  struct params_t
  {
    float black = 0;
    float exposure = 0;
    float gain = 0;
  };

  static Interpolator const black_table{
    { { -100, 0.020 }, { -50, 0.005 }, { 0, 0 }, { 50, -0.005 }, { 100, -0.010 } }
  };

  params_t params = { black_table((float)black_), exposure_, 0 };
  add_history(imgid, dev(), operation_name(), 2, &params, sizeof(params));

  return true;
}

} // namespace lightroom
