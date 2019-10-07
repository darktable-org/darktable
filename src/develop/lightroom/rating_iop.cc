#include "develop/lightroom/rating_iop.h"

#include "develop/lightroom/import_value.h"

extern "C"
{
#include "common/ratings.h"
}

namespace lightroom
{

std::string RatingIop::operation_name() const
{
  return "rating";
}

bool RatingIop::import(xmlDocPtr, xmlNodePtr, const xmlChar *name, const xmlChar *value)
{
  if(dev()) return false;
  return import_value(rating_, "Rating", name, value);
}

bool RatingIop::apply(int imgid) const
{
  if(!rating_) return false;

  dt_ratings_apply_to_image(imgid, rating_);

  return true;
}

} // namespace lightroom
