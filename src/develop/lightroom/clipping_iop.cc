#include "develop/lightroom/clipping_iop.h"

#include "develop/lightroom/add_history.h"
#include "develop/lightroom/import_value.h"

namespace lightroom
{

std::string ClippingIop::operation_name() const
{
  return "clipping";
}

bool ClippingIop::import(xmlDocPtr, xmlNodePtr, const xmlChar *name, const xmlChar *value)
{
  if(import_value(cx_, "CropLeft", name, value)) return true;
  if(import_value(cy_, "CropTop", name, value)) return true;
  if(import_value(cw_, "CropRight", name, value)) return true;
  if(import_value(ch_, "CropBottom", name, value)) return true;
  if(import_value(angle_, "CropAngle", name, value)) return true;
  if(!xmlStrcmp(name, (const xmlChar *)"HasCrop"))
  {
    has_crop_ = !xmlStrcmp(value, (const xmlChar *)"True");
    return true;
  }
  return false;
}

bool ClippingIop::apply(int imgid) const
{
  if(!has_crop_) return false;
  if(!dev()) return false;

  struct params_t
  {
    float angle = 0;
    float cx = 0;
    float cy = 0;
    float cw = 0;
    float ch = 0;
    float k_h = 0;
    float k_v = 0;
    float kxa = 0.2f;
    float kya = 0.2f;
    float kxb = 0.8f;
    float kyb = 0.2f;
    float kxc = 0.8f;
    float kyc = 0.8f;
    float kxd = 0.2f;
    float kyd = 0.8f;
    int k_type = 0;
    int k_sym = 0;
    int k_apply = 0;
    int crop_auto = 0;
    int ratio_n = -2;
    int ratio_d = -2;
  };

  params_t params = { -angle_, cx_, cy_, cw_, ch_ };

  //  // adjust crop data according to the rotation
  //
  //  switch(dev()->image_storage.orientation)
  //  {
  //    case 5: // portrait - counter-clockwise
  //    {
  //      auto tmp = params.ch;
  //      params.ch = 1.0f - params.cx;
  //      params.cx = params.cy;
  //      params.cy = 1.0f - params.cw;
  //      params.cw = tmp;
  //      break;
  //    }
  //    case 6: // portrait - clockwise
  //    {
  //      auto tmp = params.ch;
  //      params.ch = params.cw;
  //      params.cw = 1.0f - params.cy;
  //      params.cy = params.cx;
  //      params.cx = 1.0f - tmp;
  //      break;
  //    }
  //    default:
  //      break;
  //  }

  if(params.angle != 0)
  {
    const float radian_angle = params.angle * (3.141592f / 180);

    // do the rotation (radian_angle) using center of image (0.5, 0.5)

    float const source_x_lim = dev()->image_storage.width / 2.0f;
    float const source_y_lim = dev()->image_storage.height / 2.0f;
    float const target_x_lim = (source_x_lim * abs(cos(radian_angle)) + source_y_lim * abs(sin(radian_angle)));
    float const target_y_lim = (source_x_lim * abs(sin(radian_angle)) + source_y_lim * abs(cos(radian_angle)));

    float const cx = (params.cx - 0.5f) * source_x_lim;
    float const cy = (params.cy - 0.5f) * source_y_lim;
    float const cw = (params.cw - 0.5f) * source_x_lim;
    float const ch = (params.ch - 0.5f) * source_y_lim;
    params.cx = (cx * cos(radian_angle) - cy * sin(radian_angle)) / target_x_lim + 0.5f;
    params.cy = (cx * sin(radian_angle) + cy * cos(radian_angle)) / target_y_lim + 0.5f;
    params.cw = (cw * cos(radian_angle) - ch * sin(radian_angle)) / target_x_lim + 0.5f;
    params.ch = (cw * sin(radian_angle) + ch * cos(radian_angle)) / target_y_lim + 0.5f;
  }

  auto orientation = flip_.orientation();
  if (orientation & ORIENTATION_FLIP_Y) {
    auto const cy = params.cy;
    auto const ch = params.ch;
    params.cy = 1.0f - ch;
    params.ch = 1.0f - cy;
  }
  if (orientation & ORIENTATION_FLIP_X) {
    auto const cx = params.cx;
    auto const cw = params.cw;
    params.cx = 1.0f - cw;
    params.cw = 1.0f - cx;
  }
  if (orientation & ORIENTATION_SWAP_XY) {
    std::swap(params.cx, params.cy);
    std::swap(params.cw, params.ch);
  }

  add_history(imgid, dev(), operation_name(), 5, &params, sizeof(params));

  return true;
}

} // namespace lightroom
