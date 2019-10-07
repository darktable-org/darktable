#pragma once

#include <vector>

#include "develop/lightroom/flip_iop.h"
#include "develop/lightroom/iop.h"

namespace lightroom
{

class SpotIop : public Iop
{
public:
  SpotIop(dt_develop_t const *dev, FlipIop const &flip) : Iop{ dev }, flip_{ flip }
  {
  }
  std::string operation_name() const override;
  bool import(xmlDocPtr doc, xmlNodePtr node, const xmlChar *name, const xmlChar *value) override;
  bool apply(int imgid) const override;

private:
  struct spot_t
  {
    // position of the spot
    float x = 0;
    float y = 0;
    // position to clone from
    float xc = 0;
    float yc = 0;
    float radius = 0;
  };

  static constexpr int max_spots = 32;

  FlipIop const &flip_;
  std::vector<spot_t> spots_;
};

}