#include "develop/lightroom/geotagging_iop.h"

#include "develop/lightroom/import_value.h"

extern "C"
{
#include "common/image.h"
}

namespace lightroom
{

std::string GeotaggingIop::operation_name() const
{
  return "geotagging";
}

bool GeotaggingIop::import(xmlDocPtr, xmlNodePtr, const xmlChar *name, const xmlChar *value)
{
  if(dev()) return false;
  return import_value(lon_, "GPSLongitude", name, value) || import_value(lat_, "GPSLatitude", name, value)
         || import_value(alt_, "GPSAltitude", name, value);
}

bool GeotaggingIop::apply(int imgid) const
{
  if(lat_.empty() || lon_.empty()) return false;

  dt_image_geoloc_t geoloc
      = { dt_util_gps_string_to_number(lon_.c_str()), dt_util_gps_string_to_number(lat_.c_str()), NAN };
  if(!alt_.empty())
  {
    double n, d;
    if(sscanf(alt_.c_str(), "%lf/%lf", &n, &d) == 2)
    {
      geoloc.elevation = n / d;
    }
  }

  if(std::isnan(geoloc.elevation))
    dt_image_set_location(imgid, &geoloc);
  else
    dt_image_set_location_and_elevation(imgid, &geoloc);

  return true;
}

} // namespace lightroom
