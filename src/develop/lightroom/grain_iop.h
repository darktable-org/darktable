#pragma once

#include "develop/lightroom/iop.h"

namespace lightroom
{

class GrainIop : public Iop
{
public:
  using Iop::Iop;
  std::string operation_name() const override;
  bool import(xmlDocPtr doc, xmlNodePtr node, const xmlChar *name, const xmlChar *value) override;
  bool apply(int imgid) const override;

private:
  int amount_ = 0;
  int frequency_ = 0;
};

}