#pragma once

#include "develop/lightroom/flip_iop.h"
#include "develop/lightroom/iop.h"

namespace lightroom
{

class ClippingIop : public Iop
{
public:
  ClippingIop(dt_develop_t const *dev, FlipIop const &flip) : Iop{ dev }, flip_{ flip }
  {
  }
  std::string operation_name() const override;
  bool import(xmlDocPtr doc, xmlNodePtr node, const xmlChar *name, const xmlChar *value) override;
  bool apply(int imgid) const override;
  bool has_crop() const
  {
    return has_crop_;
  }
  float factor_ratio() const
  {
    return (cw_ - cx_) / (ch_ - cy_);
  }

private:
  FlipIop const &flip_;
  float angle_ = 0;
  float cx_ = 0;
  float cy_ = 0;
  float cw_ = 0;
  float ch_ = 0;
  bool has_crop_ = false;
};

}