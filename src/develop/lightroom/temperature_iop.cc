#include "develop/lightroom/temperature_iop.h"

#include <lcms2.h>

#include "develop/lightroom/add_history.h"
#include "develop/lightroom/import_value.h"
#include "develop/lightroom/interpolate.h"

extern "C"
{
#include "external/cie_colorimetric_tables.c"
}

namespace lightroom
{

namespace
{

static constexpr int initial_black_body_temperature = 4000;

static constexpr int temperature_min = 1901;
static constexpr int temperature_max = 25000;

/*
 * Spectral power distribution functions
 * https://en.wikipedia.org/wiki/Spectral_power_distribution
 */
typedef double((*spd)(unsigned long int wavelength, double temperature_k));

/*
 * Bruce Lindbloom, "Spectral Power Distribution of a Blackbody Radiator"
 * http://www.brucelindbloom.com/Eqn_Blackbody.html
 */
static double spd_blackbody(unsigned long int wavelength, double temperature_k)
{
  // convert wavelength from nm to m
  const long double lambda = (double)wavelength * 1e-9;

  /*
   * these 2 constants were computed using following Sage code:
   *
   * (from http://physics.nist.gov/cgi-bin/cuu/Value?h)
   * h = 6.62606957 * 10^-34 # Planck
   * c= 299792458 # speed of light in vacuum
   * k = 1.3806488 * 10^-23 # Boltzmann
   *
   * c_1 = 2 * pi * h * c^2
   * c_2 = h * c / k
   *
   * print 'c_1 = ', c_1, ' ~= ', RealField(128)(c_1)
   * print 'c_2 = ', c_2, ' ~= ', RealField(128)(c_2)
   */

  static constexpr double c1 = 3.7417715246641281639549488324352159753e-16L;
  static constexpr double c2 = 0.014387769599838156481252937624049081933L;

  return (double)(c1 / (powl(lambda, 5) * (expl(c2 / (lambda * temperature_k)) - 1.0L)));
}

/*
 * Bruce Lindbloom, "Spectral Power Distribution of a CIE D-Illuminant"
 * http://www.brucelindbloom.com/Eqn_DIlluminant.html
 * and https://en.wikipedia.org/wiki/Standard_illuminant#Illuminant_series_D
 */
static double spd_daylight(unsigned long int wavelength, double temperature_k)
{
  cmsCIExyY WhitePoint = { 0.3127, 0.3290, 1.0 };

  /*
   * Bruce Lindbloom, "TempK to xy"
   * http://www.brucelindbloom.com/Eqn_T_to_xy.html
   */
  cmsWhitePointFromTemp(&WhitePoint, temperature_k);

  const double M = (0.0241 + 0.2562 * WhitePoint.x - 0.7341 * WhitePoint.y),
               m1 = (-1.3515 - 1.7703 * WhitePoint.x + 5.9114 * WhitePoint.y) / M,
               m2 = (0.0300 - 31.4424 * WhitePoint.x + 30.0717 * WhitePoint.y) / M;

  const unsigned long int j = ((wavelength - cie_daylight_components[0].wavelength)
                               / (cie_daylight_components[1].wavelength - cie_daylight_components[0].wavelength));

  return (cie_daylight_components[j].S[0] + m1 * cie_daylight_components[j].S[1]
          + m2 * cie_daylight_components[j].S[2]);
}

/*
 * Bruce Lindbloom, "Computing XYZ From Spectral Data (Emissive Case)"
 * http://www.brucelindbloom.com/Eqn_Spect_to_XYZ.html
 */
static cmsCIEXYZ spectrum_to_XYZ(double temperature_k, spd I)
{
  cmsCIEXYZ Source = { .X = 0.0, .Y = 0.0, .Z = 0.0 };

  /*
   * Color matching functions
   * https://en.wikipedia.org/wiki/CIE_1931_color_space#Color_matching_functions
   */
  for(size_t i = 0; i < cie_1931_std_colorimetric_observer_count; i++)
  {
    const unsigned long int lambda
        = cie_1931_std_colorimetric_observer[0].wavelength
          + (cie_1931_std_colorimetric_observer[1].wavelength - cie_1931_std_colorimetric_observer[0].wavelength)
                * i;
    const double P = I(lambda, temperature_k);
    Source.X += P * cie_1931_std_colorimetric_observer[i].xyz.X;
    Source.Y += P * cie_1931_std_colorimetric_observer[i].xyz.Y;
    Source.Z += P * cie_1931_std_colorimetric_observer[i].xyz.Z;
  }

  // normalize so that each component is in [0.0, 1.0] range
  const double _max = std::max(std::max(Source.X, Source.Y), Source.Z);
  Source.X /= _max;
  Source.Y /= _max;
  Source.Z /= _max;

  return Source;
}

//
static cmsCIEXYZ temperature_to_XYZ(double temperature_k)
{
  if(temperature_k < temperature_min) temperature_k = temperature_min;
  if(temperature_k > temperature_max) temperature_k = temperature_max;

  if(temperature_k < initial_black_body_temperature)
  {
    // if temperature is less than 4000K we use blackbody,
    // because there will be no Daylight reference below 4000K...
    return spectrum_to_XYZ(temperature_k, spd_blackbody);
  }
  else
  {
    return spectrum_to_XYZ(temperature_k, spd_daylight);
  }
}

static void calc_coeffs(dt_develop_t const *dev, int temperature_k, float tint, float coeffs[4])
{
  // default to sRGB D65
  double CAM_to_XYZ[3][4] = { { 0.4124564, 0.3575761, 0.1804375, 0 },
                              { 0.2126729, 0.7151522, 0.0721750, 0 },
                              { 0.0193339, 0.1191920, 0.9503041, 0 } };
  double XYZ_to_CAM[4][3] = { { 3.2404542, -1.5371385, -0.4985314 },
                              { -0.9692660, 1.8760108, 0.0415560 },
                              { 0.0556434, -0.2040259, 1.0572252 },
                              { 0, 0, 0 } };
  if(dt_image_is_raw(&dev->image_storage))
  {
    char const *camera = dev->image_storage.camera_makermodel;
    dt_colorspaces_conversion_matrices_xyz(camera, dev->image_storage.d65_color_matrix, XYZ_to_CAM, CAM_to_XYZ);
  }

  cmsCIEXYZ xyz = temperature_to_XYZ(temperature_k);
  xyz.Y /= tint;
  double XYZ[3] = { xyz.X, xyz.Y, xyz.Z };

  double CAM[4];
  for(int k = 0; k < 4; k++)
  {
    CAM[k] = 0.0;
    for(int i = 0; i < 3; i++)
    {
      CAM[k] += XYZ_to_CAM[k][i] * XYZ[i];
    }
  }

  for(int k = 0; k < 4; k++) coeffs[k] = 1.0 / CAM[k];

  // normalize
  coeffs[0] /= coeffs[1];
  coeffs[2] /= coeffs[1];
  coeffs[3] /= coeffs[1];
  coeffs[1] = 1.0;
}

} // namespace

std::string TemperatureIop::operation_name() const
{
  return "temperature";
}

bool TemperatureIop::import(xmlDocPtr, xmlNodePtr, const xmlChar *name, const xmlChar *value)
{
  return import_value(temperature_, "Temperature", name, value) || import_value(tint_, "Tint", name, value);
}

bool TemperatureIop::apply(int imgid) const
{
  if(!temperature_) return false;
  if(!dev()) return false;

  struct params_t
  {
    float coeffs[4];
  };

  static auto const lr_tint_to_dt = [](float lr) {
    static Interpolator const lr_tint_to_lnrg{ {
        { 150, 0.4472347561f },
        { 120, 0.3614738364f },
        { 90, 0.2902822029f },
        { 60, 0.2320119442f },
        { 30, 0.1789069729f },
        { 20, 0.1613647324f },
        { 10, 0.1427758973f },
        { 0, 0.1248534423f },
        { -10, 0.1036493101f },
        { -20, 0.08258857431f },
        { -30, 0.0629298779f },
        { -60, -0.002147326005f },
        { -90, -0.06668486396f },
        { -120, -0.125869729f },
        { -150, -0.1808872982f },
    } };
    static Interpolator const lnrg_to_dt_tint{ {
        { 0.5367824658f, 0.75f },
        { 0.438005097f, 0.80f },
        { 0.3468036673f, 0.85f },
        { 0.2615436546f, 0.90f },
        { 0.1810485661f, 0.95f },
        { 0.1044953205f, 1.00f },
        { -0.1100210879f, 1.10f },
        { -0.1792844921f, 1.15f },
        { -0.2486665637f, 1.20f },
    } };
    static float const lnrg_factor = 0.1044953205f - 0.1427758973f;

    return lnrg_to_dt_tint(lr_tint_to_lnrg(lr) + lnrg_factor);
  };

  params_t params;
  calc_coeffs(dev(), temperature_, lr_tint_to_dt(tint_), params.coeffs);
  add_history(imgid, dev(), operation_name(), 3, &params, sizeof(params));

  return true;
}

} // namespace lightroom
