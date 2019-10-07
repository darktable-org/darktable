#include "develop/lightroom/metadata_iop.h"

#include "develop/lightroom/import_value.h"

extern "C"
{
#include "common/image.h"
}

namespace lightroom
{

std::string MetadataIop::operation_name() const
{
  return "metadata";
}

bool MetadataIop::import(xmlDocPtr, xmlNodePtr, const xmlChar *name, const xmlChar *value)
{
  if(dev()) return false;
  return import_value(title_, "title", doc, node, name)
         || import_value(description_, "description", doc, node, name)
         || import_value(creator_, "creator", doc, node, name) || import_value(rights_, "rights", doc, node, name);
}

bool MetadataIop::apply(int imgid) const
{
  if(title_.empty() && description_.empty() && creator_.empty() && rights_.empty()) return false;

  for(auto &&x : title_) dt_metadata_set(imgid, "Xmp.dc.title", x.c_str());
  for(auto &&x : description_) dt_metadata_set(imgid, "Xmp.dc.description", x.c_str());
  for(auto &&x : creator_) dt_metadata_set(imgid, "Xmp.dc.creator", x.c_str());
  for(auto &&x : rights_) dt_metadata_set(imgid, "Xmp.dc.rights", x.c_str());

  return true;
}

} // namespace lightroom
