#pragma once

#include "develop/lightroom/iop.h"

namespace lightroom
{

class ColorZonesIop : public Iop
{
public:
  explicit ColorZonesIop(dt_develop_t const *dev) : Iop{ dev }
  {
    memset(&equalizer_y_, 0, sizeof(equalizer_y_));
  }
  std::string operation_name() const override;
  bool import(xmlDocPtr doc, xmlNodePtr node, const xmlChar *name, const xmlChar *value) override;
  bool apply(int imgid) const override;

private:
  int equalizer_y_[3][8] = {};
};

}