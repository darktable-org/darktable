#pragma once

#include "develop/lightroom/iop.h"

namespace lightroom
{

class SplitToningIop : public Iop
{
public:
  using Iop::Iop;
  std::string operation_name() const override;
  bool import(xmlDocPtr doc, xmlNodePtr node, const xmlChar *name, const xmlChar *value) override;
  bool apply(int imgid) const override;

private:
  int shadow_hue_ = 0.0f;
  int shadow_saturation_ = 0.0f;
  int highlight_hue_ = 0.0f;
  int highlight_saturation_ = 0.0f;
  float balance_ = 0.0f;
};

}