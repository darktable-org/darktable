#include "develop/lightroom/spot_iop.h"

#include <algorithm>

#include "develop/lightroom/add_history.h"

namespace lightroom
{

namespace
{

// three helper functions for parsing RetouchInfo entries. sscanf doesn't work due to floats.
gboolean _read_float(const char **startptr, const char *key, float *value)
{
  const char *iter = *startptr;
  while(*iter == ' ') iter++;
  if(!g_str_has_prefix(iter, key)) return FALSE;
  iter += strlen(key);
  while(*iter == ' ') iter++;
  if(*iter++ != '=') return FALSE;
  while(*iter == ' ') iter++;
  *value = g_ascii_strtod(iter, (char **)startptr);
  return iter != *startptr;
}

gboolean _skip_key_value_pair(const char **startptr, const char *key)
{
  const char *iter = *startptr;
  while(*iter == ' ') iter++;
  if(!g_str_has_prefix(iter, key)) return FALSE;
  iter += strlen(key);
  while(*iter == ' ') iter++;
  if(*iter++ != '=') return FALSE;
  while(*iter == ' ') iter++;
  while((*iter >= 'a' && *iter <= 'z') || (*iter >= 'A' && *iter <= 'Z')) iter++;
  *startptr = iter;
  return TRUE;
}

gboolean _skip_comma(const char **startptr)
{
  return *(*startptr)++ == ',';
}

} // namespace

std::string SpotIop::operation_name() const
{
  return "spots";
}

bool SpotIop::import(xmlDocPtr doc, xmlNodePtr node, const xmlChar *name, const xmlChar *)
{
  if(!xmlStrcmp(name, (const xmlChar *)"RetouchInfo"))
  {
    xmlNodePtr riNode = node;

    while(riNode)
    {
      if(!xmlStrcmp(riNode->name, (const xmlChar *)"li"))
      {
        xmlChar *cvalue = xmlNodeListGetString(doc, riNode->xmlChildrenNode, 1);
        spot_t p;
        const char *startptr = (const char *)cvalue;
        if(_read_float(&startptr, "centerX", &p.x) && _skip_comma(&startptr)
           && _read_float(&startptr, "centerY", &p.y) && _skip_comma(&startptr)
           && _read_float(&startptr, "radius", &p.radius) && _skip_comma(&startptr)
           && _skip_key_value_pair(&startptr, "sourceState") && _skip_comma(&startptr)
           && _read_float(&startptr, "sourceX", &p.xc) && _skip_comma(&startptr)
           && _read_float(&startptr, "sourceY", &p.yc))
        {
          spots_.push_back(p);
        }
        xmlFree(cvalue);
      }
      if(spots_.size() == max_spots) break;
      riNode = riNode->next;
    }
    return true;
  }
  return false;
}

bool SpotIop::apply(int imgid) const
{
  if(spots_.empty()) return false;
  if(!dev()) return false;

  struct params_t
  {
    int num_spots = 0;
    spot_t spot[max_spots];
  };

  params_t params = { int(spots_.size()) };
  std::copy(spots_.begin(), spots_.end(), &params.spot[0]);

  // Check for orientation, rotate when in portrait mode
  if(flip_.orientation() > 4)
    for(int k = 0; k < params.num_spots; k++)
    {
      auto tmp = params.spot[k].y;
      params.spot[k].y = 1.0f - params.spot[k].x;
      params.spot[k].x = tmp;
      auto tmpc = params.spot[k].yc;
      params.spot[k].yc = 1.0f - params.spot[k].xc;
      params.spot[k].xc = tmpc;
    }

  add_history(imgid, dev(), operation_name(), 1, &params, sizeof(params));

  return true;
}

} // namespace lightroom
