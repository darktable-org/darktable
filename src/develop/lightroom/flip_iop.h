#pragma once

#include "develop/lightroom/iop.h"

extern "C"
{
#include "common/image.h"
}

namespace lightroom
{

constexpr dt_image_orientation_t dt_orientation_compose(dt_image_orientation_t a, dt_image_orientation_t b)
{
  bool ay = a & ORIENTATION_FLIP_Y;
  bool ax = a & ORIENTATION_FLIP_X;
  bool axy = a & ORIENTATION_SWAP_XY;
  bool by = b & ORIENTATION_FLIP_Y;
  bool bx = b & ORIENTATION_FLIP_X;
  bool bxy = b & ORIENTATION_SWAP_XY;
  return dt_image_orientation_t((ay ^ (axy ? bx : by) ? ORIENTATION_FLIP_Y : 0)
                                | (ax ^ (axy ? by : bx) ? ORIENTATION_FLIP_X : 0)
                                | (axy ^ bxy ? ORIENTATION_SWAP_XY : 0));
}

constexpr dt_image_orientation_t dt_orientation_inverse(dt_image_orientation_t a)
{
  bool y = a & ORIENTATION_FLIP_Y;
  bool x = a & ORIENTATION_FLIP_X;
  bool xy = a & ORIENTATION_SWAP_XY;
  return dt_image_orientation_t(((xy ? x : y) ? ORIENTATION_FLIP_Y : 0) | ((xy ? y : x) ? ORIENTATION_FLIP_X : 0)
                                | (xy ? ORIENTATION_SWAP_XY : 0));
}

class FlipIop : public Iop
{
public:
  using Iop::Iop;
  std::string operation_name() const override;
  bool import(xmlDocPtr doc, xmlNodePtr node, const xmlChar *name, const xmlChar *value) override;
  bool apply(int imgid) const override;
  dt_image_orientation_t orientation() const
  {
    return dt_image_orientation_to_flip_bits(orientation_);
  }
  dt_image_orientation_t net_orientation() const
  {
    if(!dev()) return orientation();
    return dt_orientation_compose(dt_orientation_inverse(dev()->image_storage.orientation), orientation());
  }

private:
  int orientation_ = 1;
};

}