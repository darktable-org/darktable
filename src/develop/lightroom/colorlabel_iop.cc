#include "develop/lightroom/colorlabel_iop.h"

#include "develop/lightroom/import_value.h"

extern "C"
{
#include "common/colorlabels.h"
}

namespace lightroom
{

std::string ColorLabelIop::operation_name() const
{
  return "color label";
}

bool ColorLabelIop::import(xmlDocPtr, xmlNodePtr, const xmlChar *name, const xmlChar *value)
{
  if(dev()) return false;
  return import_value(color_label_, "Label", name, value);
}

bool ColorLabelIop::apply(int imgid) const
{
  if(color_label_.empty()) return false;

  int color = 0;
  if(color_label_ == "red")
    color = 0;
  else if(color_label_ == "yellow")
    color = 1;
  else if(color_label_ == "green")
    color = 2;
  else if(color_label_ == "blue")
    color = 3;
  else
    // just an else here to catch all other cases as on lightroom one can
    // change the names of labels. So purple and the user's defined labels
    // will be mapped to purple on darktable.
    color = 4;

  dt_colorlabels_set_label(imgid, color);

  return true;
}

} // namespace lightroom
