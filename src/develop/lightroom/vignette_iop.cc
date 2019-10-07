#include "develop/lightroom/vignette_iop.h"

#include "develop/lightroom/add_history.h"
#include "develop/lightroom/import_value.h"
#include "develop/lightroom/interpolate.h"

namespace lightroom
{

std::string VignetteIop::operation_name() const
{
  return "vignette";
}

bool VignetteIop::import(xmlDocPtr, xmlNodePtr, const xmlChar *name, const xmlChar *value)
{
  if(import_value(midpoint_, "PostCropVignetteMidpoint", name, value)) return true;
  if(import_value(feather_, "PostCropVignetteFeather", name, value)) return true;
  if(import_value(amount_, "PostCropVignetteAmount", name, value)) return true;
  if(import_value(style_, "PostCropVignetteStyle", name, value)) return true;
  if(import_value(roundness_, "PostCropVignetteRoundness", name, value)) return true;
  return false;
}

bool VignetteIop::apply(int imgid) const
{
  if(!amount_) return false;
  if(!dev()) return false;

  const float base_ratio = 1.325 / 1.5;

  enum dt_iop_dither_t
  {
    DITHER_OFF = 0,
    DITHER_8BIT = 1,
    DITHER_16BIT = 2
  };

  struct dt_iop_vector_2d_t
  {
    float x;
    float y;
  };

  struct params_t
  {
    float scale;               // 0 - 100 Inner radius, percent of largest image dimension
    float falloff_scale;       // 0 - 100 Radius for falloff -- outer radius = inner radius + falloff_scale
    float brightness;          // -1 - 1 Strength of brightness reduction
    float saturation;          // -1 - 1 Strength of saturation reduction
    dt_iop_vector_2d_t center; // Center of vignette
    gboolean autoratio;        //
    float whratio;             // 0-1 = width/height ratio, 1-2 = height/width ratio + 1
    float shape;
    int dithering; // if and how to perform dithering
  };

  static Interpolator const lr_brightness_to_dt{
    { { -100, -1 }, { -50, -0.7 }, { 0, 0 }, { 50, 0.5 }, { 100, 1 } }
  };
  static Interpolator const lr_scale_to_dt{ { { 0, 74 }, { 4, 75 }, { 25, 85 }, { 50, 100 }, { 100, 100 } } };

  params_t params = { lr_scale_to_dt((float)midpoint_),
                      (float)feather_,
                      lr_brightness_to_dt((float)amount_),
                      (style_ == 1 ? -0.300f : -0.200f),
                      { 0, 0 },
                      false,
                      base_ratio,
                      1,
                      DITHER_8BIT };

  // defensive code, should not happen, but just in case future Lr version
  // has not ImageWidth/ImageLength XML tag.
  if(dimensions_.width() && dimensions_.height())
    params.whratio *= float(dimensions_.width()) / float(dimensions_.height());

  if(clipping_.has_crop()) params.whratio = params.whratio * clipping_.factor_ratio();

  //  Adjust scale and ratio based on the roundness. On Lightroom changing
  //  the roundness change the width and the height of the vignette.

  if(roundness_ > 0)
  {
    float newratio = params.whratio - (params.whratio - 1) * (float(roundness_) / 100);
    float dscale = (1 - (newratio / params.whratio)) / 2.0f;

    params.scale -= dscale * 100.0f;
    params.whratio = newratio;
  }

  add_history(imgid, dev(), operation_name(), 3, &params, sizeof(params));

  return true;
}

} // namespace lightroom
