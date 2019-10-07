#include "develop/lightroom/tags_iop.h"

#include "develop/lightroom/import_value.h"

extern "C"
{
#include "common/tags.h"
}

namespace lightroom
{

std::string TagsIop::operation_name() const
{
  return "tags";
}

bool TagsIop::import(xmlDocPtr doc, xmlNodePtr node, const xmlChar *name, const xmlChar *)
{
  if(dev()) return false;
  return import_value(tags_, "subject", doc, node, name)
         || import_value(tags_, "hierarchicalSubject", doc, node, name);
}

bool TagsIop::apply(int imgid) const
{
  if(tags_.empty()) return false;

  for(auto &&tag : tags_)
  {
    guint tagid = 0;
    if(!dt_tag_exists(tag.c_str(), &tagid)) dt_tag_new(tag.c_str(), &tagid);
    dt_tag_attach_from_gui(tagid, imgid);
  }

  return true;
}

} // namespace lightroom
