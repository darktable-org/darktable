#pragma once

#include "develop/lightroom/iop.h"

namespace lightroom
{

class ExposureIop : public Iop
{
public:
  using Iop::Iop;
  std::string operation_name() const override;
  bool import(xmlDocPtr doc, xmlNodePtr node, const xmlChar *name, const xmlChar *value) override;
  bool apply(int imgid) const override;

private:
  float exposure_ = 0;
  int black_ = 0;
};

}