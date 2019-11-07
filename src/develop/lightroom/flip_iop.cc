#include "develop/lightroom/flip_iop.h"

#include "develop/lightroom/add_history.h"
#include "develop/lightroom/import_value.h"

namespace lightroom
{

static_assert(dt_orientation_compose(ORIENTATION_NONE, ORIENTATION_NONE) == ORIENTATION_NONE, "");
static_assert(dt_orientation_compose(ORIENTATION_ROTATE_CW_90_DEG, ORIENTATION_ROTATE_CCW_90_DEG)
                  == ORIENTATION_NONE,
              "");
static_assert(dt_orientation_compose(ORIENTATION_ROTATE_CCW_90_DEG, ORIENTATION_ROTATE_CW_90_DEG)
                  == ORIENTATION_NONE,
              "");
static_assert(dt_orientation_compose(ORIENTATION_ROTATE_180_DEG, ORIENTATION_ROTATE_180_DEG) == ORIENTATION_NONE,
              "");
static_assert(dt_orientation_compose(ORIENTATION_ROTATE_CW_90_DEG, ORIENTATION_ROTATE_CW_90_DEG)
                  == ORIENTATION_ROTATE_180_DEG,
              "");
static_assert(dt_orientation_compose(ORIENTATION_ROTATE_CW_90_DEG, ORIENTATION_ROTATE_180_DEG)
                  == ORIENTATION_ROTATE_CCW_90_DEG,
              "");

static_assert(dt_orientation_compose(ORIENTATION_NONE, dt_orientation_inverse(ORIENTATION_NONE))
                  == ORIENTATION_NONE,
              "");
static_assert(dt_orientation_compose(ORIENTATION_ROTATE_CW_90_DEG,
                                     dt_orientation_inverse(ORIENTATION_ROTATE_CW_90_DEG))
                  == ORIENTATION_NONE,
              "");
static_assert(dt_orientation_compose(ORIENTATION_ROTATE_CCW_90_DEG,
                                     dt_orientation_inverse(ORIENTATION_ROTATE_CCW_90_DEG))
                  == ORIENTATION_NONE,
              "");
static_assert(dt_orientation_compose(ORIENTATION_ROTATE_180_DEG, dt_orientation_inverse(ORIENTATION_ROTATE_180_DEG))
                  == ORIENTATION_NONE,
              "");

std::string FlipIop::operation_name() const
{
  return "flip";
}

bool FlipIop::import(xmlDocPtr, xmlNodePtr, const xmlChar *name, const xmlChar *value)
{
  return import_value(orientation_, "Orientation", name, value);
}

bool FlipIop::apply(int imgid) const
{
  if(!dev()) return false;

  remove_history(imgid, operation_name());

  struct params_t
  {
    dt_image_orientation_t orientation;
  };

  params_t params = { net_orientation() };

  if(params.orientation == ORIENTATION_NONE) return false;

  add_history(imgid, dev(), operation_name(), 2, &params, sizeof(params));

  return true;
}

} // namespace lightroom
