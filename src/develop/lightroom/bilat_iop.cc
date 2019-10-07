#include "develop/lightroom/bilat_iop.h"

#include "develop/lightroom/add_history.h"
#include "develop/lightroom/import_value.h"
#include "develop/lightroom/interpolate.h"

namespace lightroom
{

std::string BilatIop::operation_name() const
{
  return "bilat";
}

bool BilatIop::import(xmlDocPtr, xmlNodePtr, const xmlChar *name, const xmlChar *value)
{
  return import_value(clarity_, "Clarity2012", name, value);
}

bool BilatIop::apply(int imgid) const
{
  if(!clarity_) return false;
  if(!dev()) return false;

  static Interpolator const clarity_table{ { { -100, -.650 }, { 0, 0 }, { 100, .650 } } };

  struct params_t
  {
    float sigma_r;
    float sigma_s;
    float detail;
  };
  params_t params = { 100, 100, clarity_table((float)clarity_) };
  add_history(imgid, dev(), operation_name(), 1, &params, sizeof(params));

  return true;
}

} // namespace lightroom
