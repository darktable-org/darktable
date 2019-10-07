#pragma once

#include <vector>

#include "develop/lightroom/iop.h"

namespace lightroom
{

class ToneCurveIop : public Iop
{
public:
  using Iop::Iop;
  std::string operation_name() const override;
  bool import(xmlDocPtr doc, xmlNodePtr node, const xmlChar *name, const xmlChar *value) override;
  bool apply(int imgid) const override;

private:
  static constexpr int max_nodes = 20;

  enum curve_kind_t
  {
    linear = 0,
    medium_contrast = 1,
    strong_contrast = 2,
    custom = 3
  };

  struct node_t
  {
    float x = 0;
    float y = 0;
  };

  int ptc_value_[4] = {};
  float ptc_split_[3] = {};
  curve_kind_t curve_kind_ = linear;
  std::vector<node_t> curve_pts_;
};

}