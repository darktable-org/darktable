#pragma once

#include <map>

namespace lightroom
{

// Simple linear interpolator function. Construct with a map of input->output values. The return value of
// operator() will linearly interpolate between the given values.
class Interpolator
{
public:
  explicit Interpolator(std::map<float, float> p) : p_{ std::move(p) }
  {
  }

  float operator()(float x) const
  {
    auto i = p_.lower_bound(x);
    if(i == p_.end())
    {
      --i;
      return i->second;
    }
    if(i == p_.begin()) return i->second;
    if(x == i->first) return i->second;
    auto x1 = i->first;
    auto y1 = i->second;
    --i;
    auto x0 = i->first;
    auto y0 = i->second;
    return y0 + ((x - x0) / (x1 - x0)) * (y1 - y0);
  }

private:
  std::map<float, float> p_;
};

} // namespace lightroom
