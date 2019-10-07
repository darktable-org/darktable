#include "develop/lightroom/colorin_iop.h"

#include "develop/lightroom/add_history.h"

extern "C"
{
#include "common/colorspaces.h"
}

namespace lightroom
{

std::string ColorInIop::operation_name() const
{
  return "colorin";
}

bool ColorInIop::import(xmlDocPtr, xmlNodePtr, const xmlChar *, const xmlChar *)
{
  return false;
}

bool ColorInIop::apply(int imgid) const
{
  if(!dev()) return false;
  if(!dt_image_is_raw(&dev()->image_storage)) return false;

  static constexpr int icc_length = 100;
  struct params_t
  {
    char iccprofile[icc_length];
    dt_iop_color_intent_t intent;
  };

  // set colorin to cmatrix which is the default from Adobe (so closer to what Lightroom does)
  params_t params = { "cmatrix", DT_INTENT_PERCEPTUAL };
  add_history(imgid, dev(), operation_name(), 1, &params, sizeof(params));

  return true;
}

} // namespace lightroom
