#include "develop/lightroom/colisa_iop.h"

#include "develop/lightroom/add_history.h"
#include "develop/lightroom/import_value.h"
#include "develop/lightroom/interpolate.h"

extern "C"
{
#include "external/cie_colorimetric_tables.c"
}

namespace lightroom
{

std::string CoLiSaIop::operation_name() const
{
  return "colisa";
}

bool CoLiSaIop::import(xmlDocPtr, xmlNodePtr, const xmlChar *name, const xmlChar *value)
{
  return import_value(contrast_, "Contrast", name, value) || import_value(contrast_, "Contrast2012", name, value)
         || import_value(saturation_, "Saturation", name, value);
}

bool CoLiSaIop::apply(int imgid) const
{
  if(!dev()) return false;

  static auto const lr_contrast_to_dt = [](float lr) {
    // This mapping is from emperical measurements - a test image with various levels of contrast applied in both
    // Lightroom and Darktable, measuring the standard deviation of the luminance of each result. This gives us a
    // mapping from the LR setting to std(lum), then from there to the DT setting. This approach isn't ideal but
    // seems to work quite well in practice in my experience.
    static Interpolator const lr_to_std{ {
        { 100, 0.36458f },  { 80, 0.353671f },  { 60, 0.341999f },  { 40, 0.329606f },   { 30, 0.323169f },
        { 20, 0.3166f },    { 15, 0.313276f },  { 10, 0.309932f },  { 5, 0.306573f },    { 0, 0.303206f },
        { -5, 0.300125f },  { -10, 0.297033f }, { -15, 0.293934f }, { -20, 0.290831f },  { -30, 0.284629f },
        { -40, 0.278439f }, { -60, 0.266103f }, { -80, 0.253801f }, { -100, 0.241532f },
    } };
    static Interpolator const std_to_dt{ {
        { 0.40848f, 1.00f },
        { 0.40263f, 0.90f },
        { 0.395863f, 0.80f },
        { 0.387938f, 0.70f },
        { 0.378524f, 0.60f },
        { 0.367174f, 0.50f },
        { 0.353264f, 0.40f },
        { 0.33599f, 0.30f },
        { 0.315309f, 0.20f },
        { 0.294933f, 0.10f },
        { 0.285362f, 0.00f },
        { 0.258864f, -0.10f },
        { 0.232495f, -0.20f },
        { 0.206264f, -0.30f },
        { 0.180259f, -0.40f },
        { 0.154605f, -0.50f },
    } };

    return std_to_dt(lr_to_std(lr));
  };

  static auto const lr_saturation_to_dt = [](float lr) {
    // This is the same general approach as for contrast above, except that here the common the value is the mean
    // value of the saturation plane of the result image in HSV space.
    static Interpolator const lr_to_ms{ {
        { 100, 0.664f }, { 80, 0.632f },  { 60, 0.587f },  { 40, 0.524f },  { 30, 0.485f },
        { 20, 0.442f },  { 15, 0.418f },  { 10, 0.393f },  { 5, 0.367f },   { 0, 0.34f },
        { -5, 0.314f },  { -10, 0.291f }, { -15, 0.27f },  { -20, 0.251f }, { -30, 0.216f },
        { -40, 0.182f }, { -60, 0.118f }, { -80, 0.057f }, { -100, 0.0f },
    } };
    static Interpolator const ms_to_dt{ {
        { 0.566f, 1.00f },
        { 0.515f, 0.80f },
        { 0.459f, 0.60f },
        { 0.397f, 0.40f },
        { 0.365f, 0.30f },
        { 0.333f, 0.20f },
        { 0.301f, 0.10f },
        { 0.272f, 0.00f },
        { 0.244f, -0.10f },
        { 0.217f, -0.20f },
        { 0.19f, -0.30f },
        { 0.163f, -0.40f },
        { 0.111f, -0.60f },
        { 0.057f, -0.80f },
        { 0.0f, -1.00f },
    } };

    return ms_to_dt(lr_to_ms(lr));
  };

  struct params_t
  {
    float contrast;
    float brightness;
    float saturation;
  };
  params_t params = { lr_contrast_to_dt(contrast_), 0, lr_saturation_to_dt(saturation_) };
  add_history(imgid, dev(), operation_name(), 1, &params, sizeof(params));

  return true;
}

} // namespace lightroom
