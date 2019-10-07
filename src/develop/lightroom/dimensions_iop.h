#pragma once

#include <experimental/optional>

#include "develop/lightroom/import_value.h"
#include "develop/lightroom/iop.h"

namespace lightroom
{

// This is a "dummy" op, in that it doesn't ever _apply_ any changes, only imports them. The imported values are
// then available to other ops for various uses.
class DimensionsIop : public Iop
{
public:
  using Iop::Iop;
  std::string operation_name() const override
  {
    return {};
  }

  bool import(xmlDocPtr, xmlNodePtr, const xmlChar *name, const xmlChar *value) override
  {
    return import_value(width_, "ImageWidth", name, value) || import_value(height_, "ImageLength", name, value);
  }

  bool apply(int) const override
  {
    return false;
  }

  int width() const
  {
    return width_;
  }
  int height() const
  {
    return height_;
  }

private:
  int width_ = 0;
  int height_ = 0;
};

}