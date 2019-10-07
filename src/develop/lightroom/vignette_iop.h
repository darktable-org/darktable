#pragma once

#include "develop/lightroom/clipping_iop.h"
#include "develop/lightroom/dimensions_iop.h"
#include "develop/lightroom/iop.h"

namespace lightroom
{

class VignetteIop : public Iop
{
public:
  VignetteIop(dt_develop_t const *dev, DimensionsIop const &dimensions, ClippingIop const &clipping)
    : Iop{ dev }, dimensions_{ dimensions }, clipping_{ clipping }
  {
  }
  std::string operation_name() const override;
  bool import(xmlDocPtr doc, xmlNodePtr node, const xmlChar *name, const xmlChar *value) override;
  bool apply(int imgid) const override;

private:
  DimensionsIop const &dimensions_;
  ClippingIop const &clipping_;
  int amount_ = 0;
  int midpoint_ = 0;
  int style_ = 0;
  int feather_ = 0;
  int roundness_ = 0;
};

}