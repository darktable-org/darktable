/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_PARAMETER_POLICY_H
#define DT_MOBILE_PARAMETER_POLICY_H

#include "common/introspection.h"
#include "develop/imageop.h"
#include <math.h>
#include <string.h>
#include "dt_mobile_array_policy.h"
#include "dt_mobile_extended_parameter_policy.h"

/* Introspection describes storage, not the invariants maintained by a desktop
 * widget. In particular, curve counts, indexes and coordinates cannot safely be
 * edited independently, even when they have declared numeric bounds. New
 * controls must opt in here after their upstream processing code is reviewed.
 * Both metadata and the numeric setter use this policy; hiding a field in the
 * Android UI alone would leave unsafe writes reachable through the native API.
 *
 * The entries below are direct scalar fields, never structural counts/indexes:
 * - src/iop/exposure.c: params, commit_params and process use these as brightness
 *   coefficients/boolean compensation options, without changing buffer sizes.
 *   The value validator below also preserves its desktop GUI black/white relation.
 * - src/iop/velvia.c: params, commit_params and process use strength/bias only as
 *   saturation weights; the declared ranges match its desktop sliders.
 * - src/iop/colorcontrast.c: params, commit_params and process use the two
 *   steepness values only as Lab channel multipliers.
 * - src/iop/colorbalancergb.c: params and commit_params map these six independent
 *   grading coefficients to fixed-size processing data. They do not select a
 *   curve, mask, array element or allocation size.
 * - src/iop/sigmoid.c: contrast/skew produce fixed-size sigmoid coefficients
 *   with positive powers throughout their declared ranges. Hue preservation is
 *   clamped to [0, 1] after percentage conversion. Display targets, primaries
 *   and processing modes remain outside this reviewed subset.
 * - src/iop/sharpen.c: amount/threshold only weight the fixed unsharp-mask
 *   result. Radius remains private: arbitrary tiny positive values can make
 *   its Gaussian variance underflow, unlike the desktop widget's normal steps.
 * - src/iop/colisa.c: contrast/brightness/saturation use the original fixed-size
 *   LUTs and channel weights. Declared [-1,1] bounds keep brightness's gamma
 *   positive and bounded; these fields do not alter indexes or allocations.
 * - src/iop/colorize.c: the four original HSL/mix sliders feed the original
 *   HSL/XYZ/Lab conversion and a fixed-size pixel operation. The internal
 *   version selector remains private so imported legacy behavior is retained.
 * - src/iop/tonemap.cc: contrast >= 1 keeps its reciprocal finite; Fsize is
 *   clamped to a minimum spatial sigma of three pixels in the original code.
 * - src/iop/highpass.c and soften.c: blur radii are bounded by MAX_RADIUS or
 *   image dimensions; strength/saturation/brightness are bounded pixel weights.
 * - src/iop/lowpass.c: its native radius minimum is 0.1, LUT sizes are fixed,
 *   and lowpass_algo selects only the two original Gaussian/bilateral paths.
 *   Gaussian order and legacy unbound are not desktop widgets and stay private.
 * - src/iop/monochrome.c: a/b and size are the original virtual filter's grid
 *   and scroll controls. Explicit bounds below retain the 256-unit grid and
 *   [0.5,3] wheel clamp, preventing zero filter variance or unbounded inputs.
 * - src/iop/graduatednd.c: declared density/hardness/rotation/HSL ranges feed
 *   bounded per-pixel coefficients. Canvas offset is limited to [0,100].
 * - src/iop/vignette.c: declared scale/falloff/shape ranges use the upstream
 *   minimum falloff/shape guards. Center paths are individually reviewed
 *   scalar members. whratio is restricted to [0.001,1.999] because its native
 *   endpoints divide by zero; autoratio and dithering select original paths.
 *   The private legacy unbound flag stays unchanged.
 * - src/iop/splittoning.c, bloom.c and grain.c: fixed HSL/pixel/LUT operations
 *   and bounded original blur radius. Grain scale is strictly positive; its
 *   unused channel enum has no desktop widget and stays private.
 * - src/iop/colorcorrection.c: saturation alone is a bounded Lab multiplier;
 *   its unbounded color-grid fields require a separate canvas editor.
 * - src/iop/colorbalance.c: mode selects three original fixed-size branches;
 *   saturation/contrast and strictly positive grey have native slider bounds.
 *   Coupled lift/gamma/gain channel arrays remain private.
 * - src/iop/globaltonemap.c: operator selects only the original three paths;
 *   the two exact nested Drago controls have positive native bounds and the
 *   original logarithm guards. Detail uses the original bounded bilateral.
 * - src/iop/colorbalancergb.c: added grading coefficients use fixed-size RGB
 *   math; tonal weights produce [2,8], white fulcrum uses positive exp2, and
 *   saturation_formula selects the two original LUT paths. Coupled
 *   midtones_Y/C/H and gray fulcrums remain private due to reciprocal/zero-
 *   denominator hazards, including when importing a nonzero midtones chroma.
 *
 * Match the type as well as the name so an upstream schema change cannot turn
 * an approved float control into an unreviewed count or enum. Continue checking
 * upstream numeric bounds before writing; this policy does not replace them.
 */
static inline int dt_mobile_parameter_allowed(const char *operation, const char *parameter,
                                              dt_introspection_type_t type)
{
  static const struct {
    const char *operation;
    const char *parameter;
    dt_introspection_type_t type;
  } approved[] = {
    { "exposure", "exposure", DT_INTROSPECTION_TYPE_FLOAT },
    { "exposure", "black", DT_INTROSPECTION_TYPE_FLOAT },
    { "exposure", "compensate_exposure_bias", DT_INTROSPECTION_TYPE_BOOL },
    { "exposure", "compensate_hilite_pres", DT_INTROSPECTION_TYPE_BOOL },
    { "velvia", "strength", DT_INTROSPECTION_TYPE_FLOAT },
    { "velvia", "bias", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorcontrast", "a_steepness", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorcontrast", "b_steepness", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "vibrance", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "contrast", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "chroma_global", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "saturation_global", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "brilliance_global", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "hue_angle", DT_INTROSPECTION_TYPE_FLOAT },
    { "sigmoid", "middle_grey_contrast", DT_INTROSPECTION_TYPE_FLOAT },
    { "sigmoid", "contrast_skewness", DT_INTROSPECTION_TYPE_FLOAT },
    { "sigmoid", "hue_preservation", DT_INTROSPECTION_TYPE_FLOAT },
    { "sharpen", "amount", DT_INTROSPECTION_TYPE_FLOAT },
    { "sharpen", "threshold", DT_INTROSPECTION_TYPE_FLOAT },
    { "colisa", "contrast", DT_INTROSPECTION_TYPE_FLOAT },
    { "colisa", "brightness", DT_INTROSPECTION_TYPE_FLOAT },
    { "colisa", "saturation", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorize", "hue", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorize", "saturation", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorize", "source_lightness_mix", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorize", "lightness", DT_INTROSPECTION_TYPE_FLOAT },
    { "tonemap", "contrast", DT_INTROSPECTION_TYPE_FLOAT },
    { "tonemap", "Fsize", DT_INTROSPECTION_TYPE_FLOAT },
    { "highpass", "sharpness", DT_INTROSPECTION_TYPE_FLOAT },
    { "highpass", "contrast", DT_INTROSPECTION_TYPE_FLOAT },
    { "soften", "size", DT_INTROSPECTION_TYPE_FLOAT },
    { "soften", "saturation", DT_INTROSPECTION_TYPE_FLOAT },
    { "soften", "brightness", DT_INTROSPECTION_TYPE_FLOAT },
    { "soften", "amount", DT_INTROSPECTION_TYPE_FLOAT },
    { "lowpass", "radius", DT_INTROSPECTION_TYPE_FLOAT },
    { "lowpass", "contrast", DT_INTROSPECTION_TYPE_FLOAT },
    { "lowpass", "brightness", DT_INTROSPECTION_TYPE_FLOAT },
    { "lowpass", "saturation", DT_INTROSPECTION_TYPE_FLOAT },
    { "lowpass", "lowpass_algo", DT_INTROSPECTION_TYPE_ENUM },
    { "monochrome", "a", DT_INTROSPECTION_TYPE_FLOAT },
    { "monochrome", "b", DT_INTROSPECTION_TYPE_FLOAT },
    { "monochrome", "size", DT_INTROSPECTION_TYPE_FLOAT },
    { "monochrome", "highlights", DT_INTROSPECTION_TYPE_FLOAT },
    { "graduatednd", "density", DT_INTROSPECTION_TYPE_FLOAT },
    { "graduatednd", "hardness", DT_INTROSPECTION_TYPE_FLOAT },
    { "graduatednd", "rotation", DT_INTROSPECTION_TYPE_FLOAT },
    { "graduatednd", "offset", DT_INTROSPECTION_TYPE_FLOAT },
    { "graduatednd", "hue", DT_INTROSPECTION_TYPE_FLOAT },
    { "graduatednd", "saturation", DT_INTROSPECTION_TYPE_FLOAT },
    { "vignette", "scale", DT_INTROSPECTION_TYPE_FLOAT },
    { "vignette", "falloff_scale", DT_INTROSPECTION_TYPE_FLOAT },
    { "vignette", "brightness", DT_INTROSPECTION_TYPE_FLOAT },
    { "vignette", "saturation", DT_INTROSPECTION_TYPE_FLOAT },
    { "vignette", "center.x", DT_INTROSPECTION_TYPE_FLOAT },
    { "vignette", "center.y", DT_INTROSPECTION_TYPE_FLOAT },
    { "vignette", "whratio", DT_INTROSPECTION_TYPE_FLOAT },
    { "vignette", "shape", DT_INTROSPECTION_TYPE_FLOAT },
    { "vignette", "autoratio", DT_INTROSPECTION_TYPE_BOOL },
    { "vignette", "dithering", DT_INTROSPECTION_TYPE_ENUM },
    { "splittoning", "shadow_hue", DT_INTROSPECTION_TYPE_FLOAT },
    { "splittoning", "shadow_saturation", DT_INTROSPECTION_TYPE_FLOAT },
    { "splittoning", "highlight_hue", DT_INTROSPECTION_TYPE_FLOAT },
    { "splittoning", "highlight_saturation", DT_INTROSPECTION_TYPE_FLOAT },
    { "splittoning", "balance", DT_INTROSPECTION_TYPE_FLOAT },
    { "splittoning", "compress", DT_INTROSPECTION_TYPE_FLOAT },
    { "bloom", "size", DT_INTROSPECTION_TYPE_FLOAT },
    { "bloom", "threshold", DT_INTROSPECTION_TYPE_FLOAT },
    { "bloom", "strength", DT_INTROSPECTION_TYPE_FLOAT },
    { "grain", "scale", DT_INTROSPECTION_TYPE_FLOAT },
    { "grain", "strength", DT_INTROSPECTION_TYPE_FLOAT },
    { "grain", "midtones_bias", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorcorrection", "saturation", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalance", "saturation", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalance", "contrast", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalance", "grey", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalance", "saturation_out", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalance", "mode", DT_INTROSPECTION_TYPE_ENUM },
    { "globaltonemap", "drago.bias", DT_INTROSPECTION_TYPE_FLOAT },
    { "globaltonemap", "drago.max_light", DT_INTROSPECTION_TYPE_FLOAT },
    { "globaltonemap", "detail", DT_INTROSPECTION_TYPE_FLOAT },
    { "globaltonemap", "operator", DT_INTROSPECTION_TYPE_ENUM },
    { "colorbalancergb", "global_Y", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "global_C", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "global_H", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "shadows_Y", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "shadows_C", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "shadows_H", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "highlights_Y", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "highlights_C", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "highlights_H", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "shadows_weight", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "highlights_weight", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "white_fulcrum", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "chroma_shadows", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "chroma_midtones", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "chroma_highlights", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "saturation_shadows", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "saturation_midtones", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "saturation_highlights", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "brilliance_shadows", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "brilliance_midtones", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "brilliance_highlights", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "saturation_formula", DT_INTROSPECTION_TYPE_ENUM },
  };
  /* Scalars and fixed-array components require exact reviewed identities and
   * storage types. Structural counts and arbitrary nested fields stay private. */
  if(!operation || !parameter) return 0;
  if(dt_mobile_array_parameter_allowed(operation, parameter, type)
     || dt_mobile_extended_parameter_allowed(operation, parameter, type)) return 1;
  for(size_t k = 0; k < sizeof(approved) / sizeof(approved[0]); k++)
    if(type == approved[k].type && !strcmp(operation, approved[k].operation)
       && !strcmp(parameter, approved[k].parameter)) return 1;
  return 0;
}

/* Narrow missing/unsafe introspection bounds to the reviewed desktop control
 * domain. Both JSON metadata and the native setter intersect these with the
 * original field bounds; clients cannot bypass them by sending numeric input.
 * Float constants deliberately match the value ultimately stored by the iop. */
static inline void dt_mobile_parameter_limits(const char *operation, const char *parameter,
                                              long double *minimum, long double *maximum)
{
  dt_mobile_array_parameter_limits(operation, parameter, minimum, maximum);
  dt_mobile_extended_parameter_limits(operation, parameter, minimum, maximum);
  static const struct {
    const char *operation, *parameter;
    float minimum, maximum;
  } limits[] = {
    { "monochrome", "a", -128.0f, 128.0f },
    { "monochrome", "b", -128.0f, 128.0f },
    { "monochrome", "size", 0.5f, 3.0f },
    { "graduatednd", "offset", 0.0f, 100.0f },
    { "vignette", "whratio", 0.001f, 1.999f },
  };
  for(size_t k = 0; k < sizeof(limits) / sizeof(limits[0]); k++)
    if(!strcmp(operation, limits[k].operation) && !strcmp(parameter, limits[k].parameter))
    {
      if(*minimum < limits[k].minimum) *minimum = limits[k].minimum;
      if(*maximum > limits[k].maximum) *maximum = limits[k].maximum;
      return;
    }
}

/* Initial touch-slider extents copied from upstream gui_init soft ranges.
 * The full reviewed numeric domain is still available through exact input. */
static inline int dt_mobile_parameter_slider_limits(const char *operation, const char *parameter,
                                                    double *minimum, double *maximum)
{
  if(dt_mobile_extended_parameter_slider_limits(operation, parameter, minimum, maximum)) return 1;
  static const struct {
    const char *operation, *parameter;
    double minimum, maximum;
  } limits[] = {
    { "exposure", "exposure", -3.0, 4.0 },
    { "exposure", "black", -0.1, 0.1 },
    { "sigmoid", "middle_grey_contrast", 0.7, 3.0 },
    { "colorbalance", "saturation", 0.5, 1.5 },
    { "colorbalance", "saturation_out", 0.5, 1.5 },
    { "colorbalance", "contrast", 0.5, 1.5 },
    { "colorbalancergb", "vibrance", -0.5, 0.5 },
    { "colorbalancergb", "contrast", -0.5, 0.5 },
    { "colorbalancergb", "chroma_global", -0.5, 0.5 },
    { "colorbalancergb", "global_Y", -0.05, 0.05 },
    { "colorbalancergb", "global_C", 0.0, 0.01 },
    { "colorbalancergb", "shadows_C", 0.0, 0.5 },
    { "colorbalancergb", "highlights_Y", -0.5, 0.5 },
    { "colorbalancergb", "highlights_C", 0.0, 0.2 },
    { "colorbalancergb", "white_fulcrum", -2.0, 2.0 },
  };
  for(size_t k = 0; k < sizeof(limits) / sizeof(limits[0]); k++)
    if(!strcmp(operation, limits[k].operation) && !strcmp(parameter, limits[k].parameter))
    {
      *minimum = limits[k].minimum;
      *maximum = limits[k].maximum;
      return 1;
    }
  return 0;
}

/* Desktop notebook sections disambiguate several identical short labels.
 * Preserve that context when presenting the controls in a single mobile list. */
static inline const char *dt_mobile_parameter_label(const char *operation, const char *parameter,
                                                   const char *fallback)
{
  static const struct {
    const char *operation, *parameter, *label;
  } labels[] = {
    { "monochrome", "a", "filter a" },
    { "monochrome", "b", "filter b" },
    { "monochrome", "size", "filter size" },
    { "splittoning", "shadow_hue", "shadows hue" },
    { "splittoning", "shadow_saturation", "shadows saturation" },
    { "splittoning", "highlight_hue", "highlights hue" },
    { "splittoning", "highlight_saturation", "highlights saturation" },
    { "globaltonemap", "drago.bias", "Drago bias" },
    { "globaltonemap", "drago.max_light", "Drago target" },
    { "colorbalancergb", "global_Y", "global offset luminance" },
    { "colorbalancergb", "global_C", "global offset chroma" },
    { "colorbalancergb", "global_H", "global offset hue" },
    { "colorbalancergb", "shadows_Y", "shadows lift luminance" },
    { "colorbalancergb", "shadows_C", "shadows lift chroma" },
    { "colorbalancergb", "shadows_H", "shadows lift hue" },
    { "colorbalancergb", "highlights_Y", "highlights gain luminance" },
    { "colorbalancergb", "highlights_C", "highlights gain chroma" },
    { "colorbalancergb", "highlights_H", "highlights gain hue" },
    { "colorbalancergb", "chroma_shadows", "shadows chroma" },
    { "colorbalancergb", "chroma_midtones", "mid-tones chroma" },
    { "colorbalancergb", "chroma_highlights", "highlights chroma" },
    { "colorbalancergb", "saturation_shadows", "shadows saturation" },
    { "colorbalancergb", "saturation_midtones", "mid-tones saturation" },
    { "colorbalancergb", "saturation_highlights", "highlights saturation" },
    { "colorbalancergb", "brilliance_shadows", "shadows brilliance" },
    { "colorbalancergb", "brilliance_midtones", "mid-tones brilliance" },
    { "colorbalancergb", "brilliance_highlights", "highlights brilliance" },
  };
  for(size_t k = 0; k < sizeof(labels) / sizeof(labels[0]); k++)
    if(!strcmp(operation, labels[k].operation) && !strcmp(parameter, labels[k].parameter))
      return labels[k].label;
  return dt_mobile_array_parameter_label(operation, parameter,
      dt_mobile_extended_parameter_label(operation, parameter, fallback));
}

static inline int dt_mobile_parameter_value_valid(dt_iop_module_t *module,
                                                  const char *parameter, double value)
{
  if(!dt_mobile_array_parameter_value_valid(module, parameter, value)
     || !dt_mobile_extended_parameter_value_valid(module, parameter, value)) return 0;
  if(strcmp(module->op, "exposure")
     || (strcmp(parameter, "black") && strcmp(parameter, "exposure"))) return 1;
  if(!module->get_f || !module->get_p) return 0;
  const dt_introspection_field_t *black_field = module->get_f("black");
  const dt_introspection_field_t *exposure_field = module->get_f("exposure");
  if(!black_field || !exposure_field
     || black_field->header.type != DT_INTROSPECTION_TYPE_FLOAT
     || exposure_field->header.type != DT_INTROSPECTION_TYPE_FLOAT) return 0;
  const float *black_value = module->get_p(module->params, "black");
  const float *exposure_value = module->get_p(module->params, "exposure");
  if(!black_value || !exposure_value) return 0;
  const float black = !strcmp(parameter, "black") ? (float)value : *black_value;
  const float exposure = !strcmp(parameter, "exposure") ? (float)value : *exposure_value;
  /* src/iop/exposure.c gui_changed/_exposure_set_black/_exposure_set_white
   * maintain this relation, and _process_common_setup divides by white-black.
   * Reject a conflicting edit rather than silently changing a second control. */
  return isfinite(black) && isfinite(exposure) && black < exp2f(-exposure);
}

#endif
