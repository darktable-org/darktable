#pragma once

#include <libxml/parser.h>
#include <string>

extern "C"
{
#include "develop/develop.h"
}

namespace lightroom
{

// Base class for Lightroom import modules, each of which imports, transforms (if needed), and applies settings for
// a corresponding Darktable iop. Note that each Lightroom Iop will duplicate (rather than reuse) any needed logic
// from its Darktable correspondant. This locks the import logic at a specific version of the operation, and avoids
// the need to update the import logic when the operation logic is updated. (In those cases, the legacy logic will
// do its thing to update the imported operations.
class Iop
{
public:
  explicit Iop(dt_develop_t const *dev) : dev_{ dev }
  {
  }
  dt_develop_t const *dev() const
  {
    return dev_;
  }

  // Override this to return the name of the Darktable iop your implementation will import
  virtual std::string operation_name() const = 0;

  // This will be called for each XML node scanned in from the XMP. Override it to capture any values your
  // implementation will need. Return true to indicate that your operation has imported the node and that the
  // importer doesn't need to keep offering it to other operations. Note: When simply capturing values directly,
  // `import_value()` in `import_value.h` reduces the boilerplate
  virtual bool import(xmlDocPtr doc, xmlNodePtr node, const xmlChar *name, const xmlChar *value) = 0;

  // This will be called to apply your operation to the image; override it to do so. This is also a good place to
  // do any sort of transformations needed (e.g. mapping settings with different ranges) since it is called only
  // once, after all available settings have been imported. Note: Operations which add to the development history
  // should use the helper function `add_history()` in `add_history.h`.
  virtual bool apply(int imgid) const = 0;

private:
  dt_develop_t const *dev_;
};

}