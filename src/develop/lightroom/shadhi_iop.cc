#include "develop/lightroom/shadhi_iop.h"

#include "develop/lightroom/add_history.h"
#include "develop/lightroom/import_value.h"
#include "develop/lightroom/interpolate.h"

extern "C"
{
#include "common/gaussian.h"
}

namespace lightroom
{

std::string ShadHiIop::operation_name() const
{
  return "shadhi";
}

bool ShadHiIop::import(xmlDocPtr, xmlNodePtr, const xmlChar *name, const xmlChar *value)
{
  return import_value(shadows_, "Shadows2012", name, value)
         || import_value(highlights_, "Highlights2012", name, value);
}

bool ShadHiIop::apply(int imgid) const
{
  if(!dev()) return false;

  enum dt_iop_shadhi_algo_t
  {
    SHADHI_ALGO_GAUSSIAN,
    SHADHI_ALGO_BILATERAL
  };

  struct params_t
  {
    dt_gaussian_order_t order = DT_IOP_GAUSSIAN_ZERO;
    float radius = 5;
    float shadows = 0;
    float whitepoint = 0;
    float highlights = 0;
    float reserved2 = 0;
    float compress = 50;
    float shadows_ccorrect = 100;
    float highlights_ccorrect = 50;
    unsigned int flags = 127;
    float low_approximation = 0.000001f;
    dt_iop_shadhi_algo_t shadhi_algo = SHADHI_ALGO_GAUSSIAN;
  };
  params_t params;
  // Measured values:
  //  params.shadows = 0.9776554943f * shadows_ + 0.08079243245f * highlights_ + 0.7862062122f;
  //  params.highlights = 0.2475219774f * shadows_ + 0.4042048108f * highlights_ + 2.842752419f;
  // Rounded & clamped for a more natural user experience:
  params.shadows = std::min(std::max(-60.0f, shadows_ + 0.10f * highlights_), 60.0f);
  params.highlights = std::min(std::max(-60.0f, 0.25f * shadows_ + 0.40f * highlights_), 60.0f);

  add_history(imgid, dev(), operation_name(), 5, &params, sizeof(params));

  return true;
}

} // namespace lightroom
