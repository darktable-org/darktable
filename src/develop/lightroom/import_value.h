#pragma once

#include <cstdlib>
#include <string>
#include <vector>

namespace lightroom
{

// Save an integer value from an XML node
inline bool import_value(int &target, char const *target_name, xmlChar const *name, const xmlChar *value)
{
  if(xmlStrcmp(name, (xmlChar const *)target_name)) return false;
  char *end = nullptr;
  auto v = strtol((char const *)value, &end, 0);
  if(end != (char const *)value) target = v;
  return true;
}

// Save a floating-point value from an XML node
inline bool import_value(float &target, char const *target_name, xmlChar const *name, const xmlChar *value)
{
  if(xmlStrcmp(name, (xmlChar const *)target_name)) return false;
  char *end = nullptr;
  auto v = strtof((char const *)value, &end);
  if(end != (char const *)value) target = v;
  return true;
}

// Save a string value from an XML node
inline bool import_value(std::string &target, char const *target_name, xmlChar const *name, const xmlChar *value)
{
  if(xmlStrcmp(name, (xmlChar const *)target_name)) return false;
  char *v = g_utf8_casefold((char *)value, -1);
  if(v) target = v;
  g_free(v);
  return true;
}

// Add strings from an XML list to a string vector
inline bool import_value(std::vector<std::string> &target, char const *target_name, xmlDocPtr doc, xmlNodePtr node,
                         xmlChar const *name)
{
  if(xmlStrcmp(name, (xmlChar const *)target_name)) return false;
  xmlNodePtr tagNode = node;
  while(tagNode)
  {
    if(!xmlStrcmp(tagNode->name, (const xmlChar *)"li"))
    {
      xmlChar *cvalue = xmlNodeListGetString(doc, tagNode->xmlChildrenNode, 1);
      target.emplace_back((char const *)cvalue);
      xmlFree(cvalue);
    }
    tagNode = tagNode->next;
  }
  return true;
}

}