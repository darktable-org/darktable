#pragma once

#include "develop/lightroom/iop.h"

namespace lightroom
{

class CoLiSaIop : public Iop
{
public:
  using Iop::Iop;
  std::string operation_name() const override;
  bool import(xmlDocPtr doc, xmlNodePtr node, const xmlChar *name, const xmlChar *value) override;
  bool apply(int imgid) const override;

private:
  int contrast_ = 0;
  int saturation_ = 0;
};

}