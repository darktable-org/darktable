/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_EXTENDED_PARAMETER_POLICY_H
#define DT_MOBILE_EXTENDED_PARAMETER_POLICY_H

#include "common/image.h"
#include "develop/develop.h"

/* Desktop scalar controls reviewed in src/iop/<operation>.{c,cc} against
 * params, gui_init/gui_changed and commit_params/process.  These fields feed
 * the original processing code directly; GUI-only auto/picker drivers,
 * private versions/counts, filenames and sampled histograms are excluded.
 * Explicit fixed-capacity selections (retouch wavelet scales and harmonizer
 * nodes) use the original desktop caps and reviewed coupled checks.
 * Coupled exposure, target luminance, gamut and mask-level invariants are
 * checked below before history is modified. Fixed-array invariants live in
 * dt_mobile_array_policy.h; spline node counts use the dedicated curve API.
 */
static inline int dt_mobile_extended_parameter_allowed(const char *operation,
                                                        const char *parameter,
                                                        dt_introspection_type_t type)
{
  static const struct {
    const char *operation, *parameter;
    dt_introspection_type_t type;
  } controls[] = {
    { "sharpen", "radius", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorcorrection", "hia", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorcorrection", "hib", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorcorrection", "loa", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorcorrection", "lob", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmic", "grey_point_source", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmic", "black_point_source", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmic", "white_point_source", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmic", "grey_point_target", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmic", "black_point_target", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmic", "white_point_target", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmic", "output_power", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmic", "latitude_stops", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmic", "contrast", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmic", "saturation", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmic", "global_saturation", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmic", "balance", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmic", "interpolator", DT_INTROSPECTION_TYPE_INT },
    { "filmic", "preserve_color", DT_INTROSPECTION_TYPE_INT },
    { "retouch", "algorithm", DT_INTROSPECTION_TYPE_ENUM },
    { "retouch", "blur_type", DT_INTROSPECTION_TYPE_ENUM },
    { "retouch", "fill_mode", DT_INTROSPECTION_TYPE_ENUM },
    { "retouch", "blur_radius", DT_INTROSPECTION_TYPE_FLOAT },
    { "retouch", "fill_brightness", DT_INTROSPECTION_TYPE_FLOAT },
    { "retouch", "max_heal_iter", DT_INTROSPECTION_TYPE_INT },
    { "retouch", "num_scales", DT_INTROSPECTION_TYPE_INT },
    { "retouch", "curr_scale", DT_INTROSPECTION_TYPE_INT },
    { "retouch", "merge_from_scale", DT_INTROSPECTION_TYPE_INT },
    { "rasterfile", "mode", DT_INTROSPECTION_TYPE_ENUM },
    { "atrous", "mix", DT_INTROSPECTION_TYPE_FLOAT },
    { "basecurve", "exposure_fusion", DT_INTROSPECTION_TYPE_INT },
    { "basecurve", "exposure_stops", DT_INTROSPECTION_TYPE_FLOAT },
    { "basecurve", "exposure_bias", DT_INTROSPECTION_TYPE_FLOAT },
    { "basecurve", "preserve_colors", DT_INTROSPECTION_TYPE_ENUM },
    { "basicadj", "black_point", DT_INTROSPECTION_TYPE_FLOAT },
    { "basicadj", "exposure", DT_INTROSPECTION_TYPE_FLOAT },
    { "basicadj", "hlcompr", DT_INTROSPECTION_TYPE_FLOAT },
    { "basicadj", "contrast", DT_INTROSPECTION_TYPE_FLOAT },
    { "basicadj", "middle_grey", DT_INTROSPECTION_TYPE_FLOAT },
    { "basicadj", "brightness", DT_INTROSPECTION_TYPE_FLOAT },
    { "basicadj", "saturation", DT_INTROSPECTION_TYPE_FLOAT },
    { "basicadj", "vibrance", DT_INTROSPECTION_TYPE_FLOAT },
    { "basicadj", "preserve_colors", DT_INTROSPECTION_TYPE_ENUM },
    { "bilat", "mode", DT_INTROSPECTION_TYPE_ENUM },
    { "bilat", "sigma_r", DT_INTROSPECTION_TYPE_FLOAT },
    { "bilat", "sigma_s", DT_INTROSPECTION_TYPE_FLOAT },
    { "bilat", "detail", DT_INTROSPECTION_TYPE_FLOAT },
    { "bilat", "midtone", DT_INTROSPECTION_TYPE_FLOAT },
    { "blurs", "type", DT_INTROSPECTION_TYPE_ENUM },
    { "blurs", "radius", DT_INTROSPECTION_TYPE_INT },
    { "blurs", "blades", DT_INTROSPECTION_TYPE_INT },
    { "blurs", "concavity", DT_INTROSPECTION_TYPE_FLOAT },
    { "blurs", "linearity", DT_INTROSPECTION_TYPE_FLOAT },
    { "blurs", "rotation", DT_INTROSPECTION_TYPE_FLOAT },
    { "blurs", "angle", DT_INTROSPECTION_TYPE_FLOAT },
    { "blurs", "curvature", DT_INTROSPECTION_TYPE_FLOAT },
    { "blurs", "offset", DT_INTROSPECTION_TYPE_FLOAT },
    { "borders", "aspect_orient", DT_INTROSPECTION_TYPE_ENUM },
    { "borders", "basis", DT_INTROSPECTION_TYPE_ENUM },
    { "borders", "aspect", DT_INTROSPECTION_TYPE_FLOAT },
    { "borders", "size", DT_INTROSPECTION_TYPE_FLOAT },
    { "borders", "pos_h", DT_INTROSPECTION_TYPE_FLOAT },
    { "borders", "pos_v", DT_INTROSPECTION_TYPE_FLOAT },
    { "borders", "frame_size", DT_INTROSPECTION_TYPE_FLOAT },
    { "borders", "frame_offset", DT_INTROSPECTION_TYPE_FLOAT },
    { "cacorrect", "avoidshift", DT_INTROSPECTION_TYPE_BOOL },
    { "cacorrect", "iterations", DT_INTROSPECTION_TYPE_ENUM },
    { "cacorrectrgb", "guide_channel", DT_INTROSPECTION_TYPE_ENUM },
    { "cacorrectrgb", "mode", DT_INTROSPECTION_TYPE_ENUM },
    { "cacorrectrgb", "radius", DT_INTROSPECTION_TYPE_FLOAT },
    { "cacorrectrgb", "strength", DT_INTROSPECTION_TYPE_FLOAT },
    { "cacorrectrgb", "refine_manifolds", DT_INTROSPECTION_TYPE_BOOL },
    { "clahe", "radius", DT_INTROSPECTION_TYPE_DOUBLE },
    { "clahe", "slope", DT_INTROSPECTION_TYPE_DOUBLE },
    { "clipping", "angle", DT_INTROSPECTION_TYPE_FLOAT },
    { "clipping", "crop_auto", DT_INTROSPECTION_TYPE_BOOL },
    { "colorharmonizer", "rule", DT_INTROSPECTION_TYPE_ENUM },
    { "colorharmonizer", "num_custom_nodes", DT_INTROSPECTION_TYPE_INT },
    { "colorharmonizer", "anchor_hue", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorharmonizer", "pull_strength", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorharmonizer", "neutral_protection", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorharmonizer", "pull_width", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorharmonizer", "smoothing", DT_INTROSPECTION_TYPE_FLOAT },
    { "colormapping", "dominance", DT_INTROSPECTION_TYPE_FLOAT },
    { "colormapping", "equalization", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorreconstruct", "threshold", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorreconstruct", "spatial", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorreconstruct", "range", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorreconstruct", "hue", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorreconstruct", "precedence", DT_INTROSPECTION_TYPE_ENUM },
    { "defringe", "radius", DT_INTROSPECTION_TYPE_FLOAT },
    { "defringe", "thresh", DT_INTROSPECTION_TYPE_FLOAT },
    { "defringe", "op_mode", DT_INTROSPECTION_TYPE_ENUM },
    { "enlargecanvas", "percent_left", DT_INTROSPECTION_TYPE_FLOAT },
    { "enlargecanvas", "percent_right", DT_INTROSPECTION_TYPE_FLOAT },
    { "enlargecanvas", "percent_top", DT_INTROSPECTION_TYPE_FLOAT },
    { "enlargecanvas", "percent_bottom", DT_INTROSPECTION_TYPE_FLOAT },
    { "enlargecanvas", "color", DT_INTROSPECTION_TYPE_ENUM },
    { "flip", "orientation", DT_INTROSPECTION_TYPE_ENUM },
    { "hotpixels", "strength", DT_INTROSPECTION_TYPE_FLOAT },
    { "hotpixels", "threshold", DT_INTROSPECTION_TYPE_FLOAT },
    { "hotpixels", "permissive", DT_INTROSPECTION_TYPE_BOOL },
    { "hotpixels", "markfixed", DT_INTROSPECTION_TYPE_BOOL },
    { "levels", "mode", DT_INTROSPECTION_TYPE_ENUM },
    { "levels", "black", DT_INTROSPECTION_TYPE_FLOAT },
    { "levels", "gray", DT_INTROSPECTION_TYPE_FLOAT },
    { "levels", "white", DT_INTROSPECTION_TYPE_FLOAT },
    { "lowlight", "blueness", DT_INTROSPECTION_TYPE_FLOAT },
    { "lut3d", "colorspace", DT_INTROSPECTION_TYPE_ENUM },
    { "lut3d", "interpolation", DT_INTROSPECTION_TYPE_ENUM },
    { "overlay", "opacity", DT_INTROSPECTION_TYPE_FLOAT },
    { "overlay", "scale", DT_INTROSPECTION_TYPE_FLOAT },
    { "overlay", "xoffset", DT_INTROSPECTION_TYPE_FLOAT },
    { "overlay", "yoffset", DT_INTROSPECTION_TYPE_FLOAT },
    { "overlay", "rotate", DT_INTROSPECTION_TYPE_FLOAT },
    { "overlay", "alignment", DT_INTROSPECTION_TYPE_INT },
    { "overlay", "scale_base", DT_INTROSPECTION_TYPE_ENUM },
    { "overlay", "scale_img", DT_INTROSPECTION_TYPE_ENUM },
    { "overlay", "scale_svg", DT_INTROSPECTION_TYPE_ENUM },
    { "watermark", "opacity", DT_INTROSPECTION_TYPE_FLOAT },
    { "watermark", "scale", DT_INTROSPECTION_TYPE_FLOAT },
    { "watermark", "xoffset", DT_INTROSPECTION_TYPE_FLOAT },
    { "watermark", "yoffset", DT_INTROSPECTION_TYPE_FLOAT },
    { "watermark", "rotate", DT_INTROSPECTION_TYPE_FLOAT },
    { "watermark", "alignment", DT_INTROSPECTION_TYPE_INT },
    { "watermark", "scale_base", DT_INTROSPECTION_TYPE_ENUM },
    { "watermark", "scale_img", DT_INTROSPECTION_TYPE_ENUM },
    { "watermark", "scale_svg", DT_INTROSPECTION_TYPE_ENUM },
    { "primaries", "achromatic_tint_hue", DT_INTROSPECTION_TYPE_FLOAT },
    { "primaries", "achromatic_tint_purity", DT_INTROSPECTION_TYPE_FLOAT },
    { "primaries", "red_hue", DT_INTROSPECTION_TYPE_FLOAT },
    { "primaries", "red_purity", DT_INTROSPECTION_TYPE_FLOAT },
    { "primaries", "green_hue", DT_INTROSPECTION_TYPE_FLOAT },
    { "primaries", "green_purity", DT_INTROSPECTION_TYPE_FLOAT },
    { "primaries", "blue_hue", DT_INTROSPECTION_TYPE_FLOAT },
    { "primaries", "blue_purity", DT_INTROSPECTION_TYPE_FLOAT },
    { "profile_gamma", "mode", DT_INTROSPECTION_TYPE_ENUM },
    { "profile_gamma", "linear", DT_INTROSPECTION_TYPE_FLOAT },
    { "profile_gamma", "gamma", DT_INTROSPECTION_TYPE_FLOAT },
    { "profile_gamma", "dynamic_range", DT_INTROSPECTION_TYPE_FLOAT },
    { "profile_gamma", "grey_point", DT_INTROSPECTION_TYPE_FLOAT },
    { "profile_gamma", "shadows_range", DT_INTROSPECTION_TYPE_FLOAT },
    { "rawdenoise", "threshold", DT_INTROSPECTION_TYPE_FLOAT },
    { "relight", "ev", DT_INTROSPECTION_TYPE_FLOAT },
    { "relight", "center", DT_INTROSPECTION_TYPE_FLOAT },
    { "relight", "width", DT_INTROSPECTION_TYPE_FLOAT },
    { "rgbcurve", "compensate_middle_grey", DT_INTROSPECTION_TYPE_BOOL },
    { "rgblevels", "autoscale", DT_INTROSPECTION_TYPE_ENUM },
    { "rgblevels", "preserve_colors", DT_INTROSPECTION_TYPE_ENUM },
    { "shadhi", "radius", DT_INTROSPECTION_TYPE_FLOAT },
    { "shadhi", "shadows", DT_INTROSPECTION_TYPE_FLOAT },
    { "shadhi", "whitepoint", DT_INTROSPECTION_TYPE_FLOAT },
    { "shadhi", "highlights", DT_INTROSPECTION_TYPE_FLOAT },
    { "shadhi", "compress", DT_INTROSPECTION_TYPE_FLOAT },
    { "shadhi", "shadows_ccorrect", DT_INTROSPECTION_TYPE_FLOAT },
    { "shadhi", "highlights_ccorrect", DT_INTROSPECTION_TYPE_FLOAT },
    { "shadhi", "shadhi_algo", DT_INTROSPECTION_TYPE_ENUM },
    { "vibrance", "amount", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmicrgb", "grey_point_source", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmicrgb", "black_point_source", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmicrgb", "white_point_source", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmicrgb", "reconstruct_threshold", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmicrgb", "reconstruct_feather", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmicrgb", "reconstruct_bloom_vs_details", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmicrgb", "reconstruct_grey_vs_color", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmicrgb", "reconstruct_structure_vs_texture", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmicrgb", "grey_point_target", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmicrgb", "black_point_target", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmicrgb", "white_point_target", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmicrgb", "output_power", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmicrgb", "latitude", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmicrgb", "contrast", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmicrgb", "saturation", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmicrgb", "balance", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmicrgb", "noise_level", DT_INTROSPECTION_TYPE_FLOAT },
    { "filmicrgb", "custom_grey", DT_INTROSPECTION_TYPE_BOOL },
    { "filmicrgb", "enable_highlight_reconstruction", DT_INTROSPECTION_TYPE_BOOL },
    { "filmicrgb", "high_quality_reconstruction", DT_INTROSPECTION_TYPE_INT },
    { "filmicrgb", "preserve_color", DT_INTROSPECTION_TYPE_ENUM },
    { "filmicrgb", "noise_distribution", DT_INTROSPECTION_TYPE_ENUM },
    { "filmicrgb", "shadows", DT_INTROSPECTION_TYPE_ENUM },
    { "filmicrgb", "highlights", DT_INTROSPECTION_TYPE_ENUM },
    { "filmicrgb", "version", DT_INTROSPECTION_TYPE_ENUM },
    { "filmicrgb", "spline_version", DT_INTROSPECTION_TYPE_ENUM },
    { "agx", "look_lift", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "look_slope", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "look_brightness", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "look_saturation", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "look_original_hue_mix_ratio", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "range_black_relative_ev", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "range_white_relative_ev", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "curve_pivot_x", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "curve_pivot_y_linear_output", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "curve_contrast_around_pivot", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "curve_linear_ratio_below_pivot", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "curve_linear_ratio_above_pivot", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "curve_toe_power", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "curve_shoulder_power", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "curve_gamma", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "curve_target_display_black_ratio", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "curve_target_display_white_ratio", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "red_inset", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "red_rotation", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "green_inset", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "green_rotation", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "blue_inset", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "blue_rotation", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "master_outset_ratio", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "master_unrotation_ratio", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "red_outset", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "red_unrotation", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "green_outset", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "green_unrotation", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "blue_outset", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "blue_unrotation", DT_INTROSPECTION_TYPE_FLOAT },
    { "agx", "auto_gamma", DT_INTROSPECTION_TYPE_BOOL },
    { "agx", "disable_primaries_adjustments", DT_INTROSPECTION_TYPE_BOOL },
    { "agx", "completely_reverse_primaries", DT_INTROSPECTION_TYPE_BOOL },
    { "agx", "base_primaries", DT_INTROSPECTION_TYPE_ENUM },
    { "channelmixerrgb", "temperature", DT_INTROSPECTION_TYPE_FLOAT },
    { "channelmixerrgb", "gamut", DT_INTROSPECTION_TYPE_FLOAT },
    { "channelmixerrgb", "normalize_R", DT_INTROSPECTION_TYPE_BOOL },
    { "channelmixerrgb", "normalize_G", DT_INTROSPECTION_TYPE_BOOL },
    { "channelmixerrgb", "normalize_B", DT_INTROSPECTION_TYPE_BOOL },
    { "channelmixerrgb", "normalize_sat", DT_INTROSPECTION_TYPE_BOOL },
    { "channelmixerrgb", "normalize_light", DT_INTROSPECTION_TYPE_BOOL },
    { "channelmixerrgb", "normalize_grey", DT_INTROSPECTION_TYPE_BOOL },
    { "channelmixerrgb", "clip", DT_INTROSPECTION_TYPE_BOOL },
    { "channelmixerrgb", "illuminant", DT_INTROSPECTION_TYPE_ENUM },
    { "channelmixerrgb", "illum_fluo", DT_INTROSPECTION_TYPE_ENUM },
    { "channelmixerrgb", "illum_led", DT_INTROSPECTION_TYPE_ENUM },
    { "channelmixerrgb", "adaptation", DT_INTROSPECTION_TYPE_ENUM },
    { "channelmixerrgb", "version", DT_INTROSPECTION_TYPE_ENUM },
    { "colorequal", "threshold", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "smoothing_hue", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "contrast", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "white_level", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "chroma_size", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "param_size", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "hue_shift", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "sat_red", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "sat_orange", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "sat_yellow", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "sat_green", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "sat_cyan", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "sat_blue", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "sat_lavender", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "sat_magenta", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "hue_red", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "hue_orange", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "hue_yellow", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "hue_green", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "hue_cyan", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "hue_blue", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "hue_lavender", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "hue_magenta", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "bright_red", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "bright_orange", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "bright_yellow", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "bright_green", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "bright_cyan", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "bright_blue", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "bright_lavender", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "bright_magenta", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorequal", "use_filter", DT_INTROSPECTION_TYPE_BOOL },
    { "colorzones", "channel", DT_INTROSPECTION_TYPE_ENUM },
    { "colorzones", "mode", DT_INTROSPECTION_TYPE_ENUM },
    { "colorzones", "strength", DT_INTROSPECTION_TYPE_FLOAT },
    { "tonecurve", "tonecurve_autoscale_ab", DT_INTROSPECTION_TYPE_ENUM },
    { "tonecurve", "preserve_colors", DT_INTROSPECTION_TYPE_ENUM },
    { "toneequal", "noise", DT_INTROSPECTION_TYPE_FLOAT },
    { "toneequal", "ultra_deep_blacks", DT_INTROSPECTION_TYPE_FLOAT },
    { "toneequal", "deep_blacks", DT_INTROSPECTION_TYPE_FLOAT },
    { "toneequal", "blacks", DT_INTROSPECTION_TYPE_FLOAT },
    { "toneequal", "shadows", DT_INTROSPECTION_TYPE_FLOAT },
    { "toneequal", "midtones", DT_INTROSPECTION_TYPE_FLOAT },
    { "toneequal", "highlights", DT_INTROSPECTION_TYPE_FLOAT },
    { "toneequal", "whites", DT_INTROSPECTION_TYPE_FLOAT },
    { "toneequal", "speculars", DT_INTROSPECTION_TYPE_FLOAT },
    { "toneequal", "blending", DT_INTROSPECTION_TYPE_FLOAT },
    { "toneequal", "smoothing", DT_INTROSPECTION_TYPE_FLOAT },
    { "toneequal", "feathering", DT_INTROSPECTION_TYPE_FLOAT },
    { "toneequal", "quantization", DT_INTROSPECTION_TYPE_FLOAT },
    { "toneequal", "contrast_boost", DT_INTROSPECTION_TYPE_FLOAT },
    { "toneequal", "exposure_boost", DT_INTROSPECTION_TYPE_FLOAT },
    { "toneequal", "details", DT_INTROSPECTION_TYPE_ENUM },
    { "toneequal", "method", DT_INTROSPECTION_TYPE_ENUM },
    { "toneequal", "iterations", DT_INTROSPECTION_TYPE_INT },
    { "negadoctor", "D_max", DT_INTROSPECTION_TYPE_FLOAT },
    { "negadoctor", "offset", DT_INTROSPECTION_TYPE_FLOAT },
    { "negadoctor", "black", DT_INTROSPECTION_TYPE_FLOAT },
    { "negadoctor", "gamma", DT_INTROSPECTION_TYPE_FLOAT },
    { "negadoctor", "soft_clip", DT_INTROSPECTION_TYPE_FLOAT },
    { "negadoctor", "exposure", DT_INTROSPECTION_TYPE_FLOAT },
    { "negadoctor", "film_stock", DT_INTROSPECTION_TYPE_ENUM },
    { "colorin", "intent", DT_INTROSPECTION_TYPE_ENUM },
    { "colorin", "normalize", DT_INTROSPECTION_TYPE_ENUM },
    { "colorin", "blue_mapping", DT_INTROSPECTION_TYPE_BOOL },
    { "colorout", "intent", DT_INTROSPECTION_TYPE_ENUM },
    { "colorbalancergb", "grey_fulcrum", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "mask_grey_fulcrum", DT_INTROSPECTION_TYPE_FLOAT },
    { "colorbalancergb", "midtones_Y", DT_INTROSPECTION_TYPE_FLOAT },
    { "sigmoid", "display_white_target", DT_INTROSPECTION_TYPE_FLOAT },
    { "sigmoid", "display_black_target", DT_INTROSPECTION_TYPE_FLOAT },
    { "sigmoid", "red_inset", DT_INTROSPECTION_TYPE_FLOAT },
    { "sigmoid", "red_rotation", DT_INTROSPECTION_TYPE_FLOAT },
    { "sigmoid", "green_inset", DT_INTROSPECTION_TYPE_FLOAT },
    { "sigmoid", "green_rotation", DT_INTROSPECTION_TYPE_FLOAT },
    { "sigmoid", "blue_inset", DT_INTROSPECTION_TYPE_FLOAT },
    { "sigmoid", "blue_rotation", DT_INTROSPECTION_TYPE_FLOAT },
    { "sigmoid", "purity", DT_INTROSPECTION_TYPE_FLOAT },
    { "sigmoid", "color_processing", DT_INTROSPECTION_TYPE_ENUM },
    { "sigmoid", "base_primaries", DT_INTROSPECTION_TYPE_ENUM },
    { "denoiseprofile", "radius", DT_INTROSPECTION_TYPE_FLOAT },
    { "denoiseprofile", "nbhood", DT_INTROSPECTION_TYPE_FLOAT },
    { "denoiseprofile", "strength", DT_INTROSPECTION_TYPE_FLOAT },
    { "denoiseprofile", "shadows", DT_INTROSPECTION_TYPE_FLOAT },
    { "denoiseprofile", "bias", DT_INTROSPECTION_TYPE_FLOAT },
    { "denoiseprofile", "scattering", DT_INTROSPECTION_TYPE_FLOAT },
    { "denoiseprofile", "central_pixel_weight", DT_INTROSPECTION_TYPE_FLOAT },
    { "denoiseprofile", "overshooting", DT_INTROSPECTION_TYPE_FLOAT },
    { "denoiseprofile", "mode", DT_INTROSPECTION_TYPE_ENUM },
    { "denoiseprofile", "wavelet_color_mode", DT_INTROSPECTION_TYPE_ENUM },
    { "denoiseprofile", "wb_adaptive_anscombe", DT_INTROSPECTION_TYPE_BOOL },
    { "denoiseprofile", "fix_anscombe_and_nlmeans_norm", DT_INTROSPECTION_TYPE_BOOL },
    { "denoiseprofile", "use_new_vst", DT_INTROSPECTION_TYPE_BOOL },
    { "denoiseprofile", "compensate_hilite_pres", DT_INTROSPECTION_TYPE_BOOL },
    { "diffuse", "iterations", DT_INTROSPECTION_TYPE_INT },
    { "diffuse", "radius", DT_INTROSPECTION_TYPE_INT },
    { "diffuse", "radius_center", DT_INTROSPECTION_TYPE_INT },
    { "diffuse", "sharpness", DT_INTROSPECTION_TYPE_FLOAT },
    { "diffuse", "regularization", DT_INTROSPECTION_TYPE_FLOAT },
    { "diffuse", "variance_threshold", DT_INTROSPECTION_TYPE_FLOAT },
    { "diffuse", "anisotropy_first", DT_INTROSPECTION_TYPE_FLOAT },
    { "diffuse", "anisotropy_second", DT_INTROSPECTION_TYPE_FLOAT },
    { "diffuse", "anisotropy_third", DT_INTROSPECTION_TYPE_FLOAT },
    { "diffuse", "anisotropy_fourth", DT_INTROSPECTION_TYPE_FLOAT },
    { "diffuse", "threshold", DT_INTROSPECTION_TYPE_FLOAT },
    { "diffuse", "first", DT_INTROSPECTION_TYPE_FLOAT },
    { "diffuse", "second", DT_INTROSPECTION_TYPE_FLOAT },
    { "diffuse", "third", DT_INTROSPECTION_TYPE_FLOAT },
    { "diffuse", "fourth", DT_INTROSPECTION_TYPE_FLOAT },
    { "highlights", "mode", DT_INTROSPECTION_TYPE_ENUM },
    { "highlights", "scales", DT_INTROSPECTION_TYPE_ENUM },
    { "highlights", "recovery", DT_INTROSPECTION_TYPE_ENUM },
    { "highlights", "iterations", DT_INTROSPECTION_TYPE_INT },
    { "highlights", "strength", DT_INTROSPECTION_TYPE_FLOAT },
    { "highlights", "clip", DT_INTROSPECTION_TYPE_FLOAT },
    { "highlights", "noise_level", DT_INTROSPECTION_TYPE_FLOAT },
    { "highlights", "candidating", DT_INTROSPECTION_TYPE_FLOAT },
    { "highlights", "combine", DT_INTROSPECTION_TYPE_FLOAT },
    { "highlights", "solid_color", DT_INTROSPECTION_TYPE_FLOAT },
    { "demosaic", "green_eq", DT_INTROSPECTION_TYPE_ENUM },
    { "demosaic", "color_smoothing", DT_INTROSPECTION_TYPE_ENUM },
    { "demosaic", "demosaicing_method", DT_INTROSPECTION_TYPE_ENUM },
    { "demosaic", "lmmse_refine", DT_INTROSPECTION_TYPE_ENUM },
    { "demosaic", "median_thrs", DT_INTROSPECTION_TYPE_FLOAT },
    { "demosaic", "dual_thrs", DT_INTROSPECTION_TYPE_FLOAT },
    { "demosaic", "cs_radius", DT_INTROSPECTION_TYPE_FLOAT },
    { "demosaic", "cs_thrs", DT_INTROSPECTION_TYPE_FLOAT },
    { "demosaic", "cs_boost", DT_INTROSPECTION_TYPE_FLOAT },
    { "demosaic", "cs_center", DT_INTROSPECTION_TYPE_FLOAT },
    { "demosaic", "cs_iter", DT_INTROSPECTION_TYPE_INT },
    { "demosaic", "cs_enabled", DT_INTROSPECTION_TYPE_BOOL },
    { "hazeremoval", "strength", DT_INTROSPECTION_TYPE_FLOAT },
    { "hazeremoval", "distance", DT_INTROSPECTION_TYPE_FLOAT },
    { "scalepixels", "pixel_aspect_ratio", DT_INTROSPECTION_TYPE_FLOAT },
    { "nlmeans", "radius", DT_INTROSPECTION_TYPE_FLOAT },
    { "nlmeans", "strength", DT_INTROSPECTION_TYPE_FLOAT },
    { "nlmeans", "luma", DT_INTROSPECTION_TYPE_FLOAT },
    { "nlmeans", "chroma", DT_INTROSPECTION_TYPE_FLOAT },
    { "lens", "method", DT_INTROSPECTION_TYPE_ENUM },
    { "lens", "modify_flags", DT_INTROSPECTION_TYPE_ENUM },
    { "lens", "inverse", DT_INTROSPECTION_TYPE_ENUM },
    { "lens", "target_geom", DT_INTROSPECTION_TYPE_ENUM },
    { "lens", "tca_override", DT_INTROSPECTION_TYPE_BOOL },
    { "lens", "scale", DT_INTROSPECTION_TYPE_FLOAT },
    { "lens", "tca_r", DT_INTROSPECTION_TYPE_FLOAT },
    { "lens", "tca_b", DT_INTROSPECTION_TYPE_FLOAT },
    { "lens", "cor_dist_ft", DT_INTROSPECTION_TYPE_FLOAT },
    { "lens", "cor_vig_ft", DT_INTROSPECTION_TYPE_FLOAT },
    { "lens", "cor_ca_r_ft", DT_INTROSPECTION_TYPE_FLOAT },
    { "lens", "cor_ca_b_ft", DT_INTROSPECTION_TYPE_FLOAT },
    { "lens", "scale_md", DT_INTROSPECTION_TYPE_FLOAT },
    { "lens", "v_strength", DT_INTROSPECTION_TYPE_FLOAT },
    { "lens", "v_radius", DT_INTROSPECTION_TYPE_FLOAT },
    { "lens", "v_steepness", DT_INTROSPECTION_TYPE_FLOAT },
    { "ashift", "rotation", DT_INTROSPECTION_TYPE_FLOAT },
    { "ashift", "lensshift_v", DT_INTROSPECTION_TYPE_FLOAT },
    { "ashift", "lensshift_h", DT_INTROSPECTION_TYPE_FLOAT },
    { "ashift", "shear", DT_INTROSPECTION_TYPE_FLOAT },
    { "ashift", "f_length", DT_INTROSPECTION_TYPE_FLOAT },
    { "ashift", "crop_factor", DT_INTROSPECTION_TYPE_FLOAT },
    { "ashift", "orthocorr", DT_INTROSPECTION_TYPE_FLOAT },
    { "ashift", "aspect", DT_INTROSPECTION_TYPE_FLOAT },
    { "ashift", "mode", DT_INTROSPECTION_TYPE_ENUM },
    { "dither", "dither_type", DT_INTROSPECTION_TYPE_ENUM },
    { "dither", "random.damping", DT_INTROSPECTION_TYPE_FLOAT },
    { "bilateral", "radius", DT_INTROSPECTION_TYPE_FLOAT },
    { "bilateral", "red", DT_INTROSPECTION_TYPE_FLOAT },
    { "bilateral", "green", DT_INTROSPECTION_TYPE_FLOAT },
    { "bilateral", "blue", DT_INTROSPECTION_TYPE_FLOAT },
    { "censorize", "radius_1", DT_INTROSPECTION_TYPE_FLOAT },
    { "censorize", "pixelate", DT_INTROSPECTION_TYPE_FLOAT },
    { "censorize", "radius_2", DT_INTROSPECTION_TYPE_FLOAT },
    { "censorize", "noise", DT_INTROSPECTION_TYPE_FLOAT },
    { "rawprepare", "flat_field", DT_INTROSPECTION_TYPE_ENUM },
    { "rawprepare", "raw_white_point", DT_INTROSPECTION_TYPE_USHORT },
  };
  for(size_t k = 0; k < sizeof(controls) / sizeof(controls[0]); k++)
    if(type == controls[k].type && !strcmp(operation, controls[k].operation)
       && !strcmp(parameter, controls[k].parameter)) return 1;
  return 0;
}

static inline void dt_mobile_extended_parameter_limits(const char *operation, const char *parameter,
                                                           long double *minimum, long double *maximum)
{
  static const struct { const char *operation, *parameter; float minimum, maximum; } domains[] = {
    { "lens", "target_geom", 1.0f, 8.0f },
    { "retouch", "num_scales", 0.0f, 15.0f },
    { "retouch", "curr_scale", 0.0f, 16.0f },
    { "retouch", "merge_from_scale", 0.0f, 15.0f },
    { "retouch", "algorithm", 1.0f, 4.0f },
    { "retouch", "max_heal_iter", 1.0f, 10000.0f },
    { "colorin", "intent", 0.0f, 3.0f },
    { "colorout", "intent", 0.0f, 3.0f },
    { "enlargecanvas", "color", 0.0f, 4.0f },
    { "toneequal", "method", 0.0f, 6.0f },
    { "channelmixerrgb", "illuminant", 0.0f, 10.0f },
    { "channelmixerrgb", "illum_fluo", 0.0f, 11.0f },
    { "channelmixerrgb", "illum_led", 0.0f, 8.0f },
    { "channelmixerrgb", "adaptation", 0.0f, 4.0f },
    { "sharpen", "radius", 0.0f, 99.0f },
    { "colorcorrection", "hia", -40.0f, 40.0f },
    { "colorcorrection", "hib", -40.0f, 40.0f },
    { "colorcorrection", "loa", -40.0f, 40.0f },
    { "colorcorrection", "lob", -40.0f, 40.0f },
    { "filmic", "grey_point_source", 0.1f, 100.0f },
    { "filmic", "black_point_source", -16.0f, -0.1f },
    { "filmic", "white_point_source", 0.1f, 16.0f },
    { "filmic", "grey_point_target", 0.1f, 50.0f },
    { "filmic", "black_point_target", 0.0f, 100.0f },
    { "filmic", "white_point_target", 0.0f, 100.0f },
    { "filmic", "output_power", 1.0f, 2.4f },
    { "filmic", "latitude_stops", 0.01f, 16.0f },
    { "filmic", "contrast", 0.001f, 5.0f },
    { "filmic", "saturation", 0.0f, 195.904f },
    { "filmic", "global_saturation", 0.0f, 1000.0f },
    { "filmic", "balance", -50.0f, 50.0f },
    { "filmic", "interpolator", 0.0f, 3.0f },
    { "filmic", "preserve_color", 0.0f, 1.0f },
    { "basecurve", "exposure_fusion", 0.0f, 2.0f },
    { "clahe", "radius", 0.0f, 256.0f },
    { "clahe", "slope", 1.0f, 3.0f },
    { "overlay", "alignment", 0.0f, 8.0f },
    { "watermark", "alignment", 0.0f, 8.0f },
    { "relight", "center", 0.0f, 1.0f },
    { "toneequal", "smoothing", 0.630656f, 2.522679f },
    { "filmicrgb", "grey_point_source", 0.001f, 100.0f },
    { "colorbalancergb", "grey_fulcrum", 0.0001f, 1.0f },
    { "colorbalancergb", "mask_grey_fulcrum", 0.0001f, 1.0f },
    { "colorbalancergb", "midtones_Y", -0.999f, 1.0f },
    { "bilateral", "red", 0.001f, 1.0f },
    { "bilateral", "green", 0.001f, 1.0f },
    { "bilateral", "blue", 0.001f, 1.0f },
    { "bilat", "sigma_r", 0.001f, 100.0f },
    { "bilat", "sigma_s", 0.001f, 100.0f },
  };
  for(size_t k = 0; k < sizeof(domains) / sizeof(domains[0]); k++)
    if(!strcmp(operation, domains[k].operation) && !strcmp(parameter, domains[k].parameter))
    {
      if(*minimum < domains[k].minimum) *minimum = domains[k].minimum;
      if(*maximum > domains[k].maximum) *maximum = domains[k].maximum;
      return;
    }
}

static inline int dt_mobile_extended_parameter_slider_limits(const char *operation, const char *parameter,
                                                           double *minimum, double *maximum)
{
  static const struct { const char *operation, *parameter; float minimum, maximum; } domains[] = {
    { "sharpen", "radius", 0.0f, 8.0f },
    { "filmic", "grey_point_source", 0.1f, 36.0f },
    { "filmic", "white_point_source", 2.0f, 8.0f },
    { "filmic", "black_point_source", -14.0f, -3.0f },
    { "filmic", "contrast", 1.0f, 2.0f },
    { "filmic", "latitude_stops", 2.0f, 8.0f },
    { "filmic", "global_saturation", 0.0f, 200.0f },
    { "basecurve", "exposure_fusion", 0.0f, 2.0f },
    { "basicadj", "black_point", -0.1f, 0.1f },
    { "basicadj", "exposure", -4.0f, 4.0f },
    { "basicadj", "hlcompr", 0.0f, 100.0f },
    { "basicadj", "contrast", -1.0f, 1.0f },
    { "basicadj", "brightness", -1.0f, 1.0f },
    { "blurs", "radius", 4.0f, 64.0f },
    { "cacorrectrgb", "radius", 1.0f, 20.0f },
    { "denoiseprofile", "radius", 0.0f, 8.0f },
    { "denoiseprofile", "scattering", 0.0f, 1.0f },
    { "denoiseprofile", "central_pixel_weight", 0.0f, 1.0f },
    { "denoiseprofile", "overshooting", 0.001f, 4.0f },
    { "denoiseprofile", "strength", 0.001f, 4.0f },
    { "denoiseprofile", "bias", -10.0f, 10.0f },
    { "diffuse", "iterations", 1.0f, 128.0f },
    { "diffuse", "radius", 1.0f, 512.0f },
    { "diffuse", "radius_center", 0.0f, 512.0f },
    { "diffuse", "sharpness", -0.25f, 0.25f },
    { "toneequal", "smoothing", 1.0f, 2.0f },
    { "ashift", "rotation", -10.0f, 10.0f },
    { "ashift", "lensshift_v", -1.0f, 1.0f },
    { "ashift", "lensshift_h", -1.0f, 1.0f },
    { "ashift", "shear", -0.2f, 0.2f },
    { "ashift", "f_length", 10.0f, 1000.0f },
    { "ashift", "crop_factor", 1.0f, 2.0f },
    { "nlmeans", "radius", 0.0f, 4.0f },
    { "nlmeans", "strength", 0.0f, 100.0f },
    { "bilateral", "radius", 1.0f, 30.0f },
    { "bilateral", "red", 0.001f, 0.1f },
    { "bilateral", "green", 0.001f, 0.1f },
    { "bilateral", "blue", 0.001f, 0.1f },
    { "rawdenoise", "threshold", 0.0f, 0.1f },
    { "overlay", "scale", 1.0f, 100.0f },
    { "watermark", "scale", 1.0f, 100.0f },
    { "profile_gamma", "shadows_range", -16.0f, 0.0f },
    { "profile_gamma", "dynamic_range", 0.5f, 16.0f },
    { "rawprepare", "raw_white_point", 0.0f, 16384.0f },
    { "primaries", "red_hue", -0.34906585f, 0.34906585f },
    { "primaries", "green_hue", -0.34906585f, 0.34906585f },
    { "primaries", "blue_hue", -0.34906585f, 0.34906585f },
    { "primaries", "red_purity", 0.5f, 1.5f },
    { "primaries", "green_purity", 0.5f, 1.5f },
    { "primaries", "blue_purity", 0.5f, 1.5f },
    { "primaries", "achromatic_tint_purity", 0.0f, 0.2f },
  };
  for(size_t k = 0; k < sizeof(domains) / sizeof(domains[0]); k++)
    if(!strcmp(operation, domains[k].operation) && !strcmp(parameter, domains[k].parameter))
    {
      *minimum = domains[k].minimum;
      *maximum = domains[k].maximum;
      return 1;
    }
  return 0;
}

/* Source/target tuples are read from checked introspection locations. A
 * future incompatible storage change disables the write instead of silently
 * interpreting bytes as another type. */
static inline int dt_mobile_policy_number(dt_iop_module_t *module, const char *name,
                                          dt_introspection_type_t type, double *value)
{
  if(!module || !module->get_f || !module->params || module->params_size <= 0) return 0;
  const dt_introspection_field_t *field = module->get_f(name);
  const size_t size = type == DT_INTROSPECTION_TYPE_FLOAT ? sizeof(float)
    : type == DT_INTROSPECTION_TYPE_INT || type == DT_INTROSPECTION_TYPE_ENUM ? sizeof(int) : sizeof(gboolean);
  if(!field || field->header.type != type || field->header.size != size
     || field->header.offset > (size_t)module->params_size
     || size > (size_t)module->params_size - field->header.offset) return 0;
  const char *address = (const char *)module->params + field->header.offset;
  if(type == DT_INTROSPECTION_TYPE_FLOAT) *value = *(const float *)address;
  else if(type == DT_INTROSPECTION_TYPE_BOOL) *value = *(const gboolean *)address;
  else if(type == DT_INTROSPECTION_TYPE_INT || type == DT_INTROSPECTION_TYPE_ENUM) *value = *(const int *)address;
  else return 0;
  return isfinite(*value);
}

/* A non-RAW input has no sensor black/white preparation, and a non-mosaic
 * input has no demosaicing operation. Do not advertise sliders disabled by
 * the original commit_params or derive impossible sensor ranges from zeros. */
static inline int dt_mobile_extended_parameter_applicable(const dt_iop_module_t *module)
{
  const int demosaic = !strcmp(module->op, "demosaic");
  const int rawprepare = !strcmp(module->op, "rawprepare");
  if(!demosaic && !rawprepare) return 1;
  if(!module->dev) return 0;
  const dt_image_t *image = &module->dev->image_storage;
  if(rawprepare) return dt_image_is_rawprepare_supported(image);
  return dt_image_is_raw(image) || dt_image_is_mono_sraw(image);
}

static inline int dt_mobile_extended_parameter_value_valid(dt_iop_module_t *module,
                                                           const char *parameter, double value)
{
  long double minimum = -INFINITY, maximum = INFINITY;
  dt_mobile_extended_parameter_limits(module->op, parameter, &minimum, &maximum);
  if(!isfinite(value) || value < minimum || value > maximum) return 0;
  if(!dt_mobile_extended_parameter_applicable(module)) return 0;
  if(!strcmp(module->op, "demosaic") && !strcmp(parameter, "demosaicing_method"))
  {
    /* Exact original gui_init families: Bayer 0..6 and two dual methods,
     * X-Trans's seven methods, four-Bayer's VNG4/two passthrough choices,
     * and the true-monochrome sentinel. Commit must not silently replace a
     * user's selection with a different sensor's algorithm. */
    const dt_image_t *image = &module->dev->image_storage;
    const int method = (int)value;
    if(dt_image_is_mono_sraw(image)) return method == 7;
    if(image->flags & DT_IMAGE_4BAYER) return method == 2 || method == 3 || method == 4;
    if(image->buf_dsc.filters == 9u)
      return (method >= 1024 && method <= 1029) || method == (2048 | 1024 | 2);
    return (method >= 0 && method <= 6) || method == (2048 | 5) || method == (2048 | 1);
  }
  if(!strcmp(module->op, "highlights") && !strcmp(parameter, "mode"))
  {
    if(!module->dev) return 0;
    const dt_image_t *image = &module->dev->image_storage;
    const unsigned filters = image->buf_dsc.filters;
    if(!dt_image_is_rawprepare_supported(image)) return value == 0.0;
    if(value == 3.0 && (filters == 0u || filters == 9u)) return 0;
    if(filters == 0u && (value == 1.0 || value == 2.0 || value == 4.0)) return 0;
  }
  if(!strcmp(module->op, "retouch")
     && (!strcmp(parameter, "num_scales") || !strcmp(parameter, "merge_from_scale")))
  {
    double scales, merge;
    if(!dt_mobile_policy_number(module, "num_scales", DT_INTROSPECTION_TYPE_INT, &scales)
       || !dt_mobile_policy_number(module, "merge_from_scale", DT_INTROSPECTION_TYPE_INT, &merge)) return 0;
    if(!strcmp(parameter, "num_scales")) scales = value; else merge = value;
    if(scales < 0.0 || scales > 15.0 || merge < 0.0 || merge > scales) return 0;
  }
  if(!strcmp(module->op, "sharpen") && !strcmp(parameter, "radius"))
    return value == 0.0 || value >= (double)0.1f;
  if(!strcmp(module->op, "filmic"))
  {
    if(!strcmp(parameter, "black_point_target") || !strcmp(parameter, "grey_point_target")
       || !strcmp(parameter, "white_point_target"))
    {
      double black, grey, white;
      if(!dt_mobile_policy_number(module, "black_point_target", DT_INTROSPECTION_TYPE_FLOAT, &black)
         || !dt_mobile_policy_number(module, "grey_point_target", DT_INTROSPECTION_TYPE_FLOAT, &grey)
         || !dt_mobile_policy_number(module, "white_point_target", DT_INTROSPECTION_TYPE_FLOAT, &white)) return 0;
      if(!strcmp(parameter, "black_point_target")) black = value;
      if(!strcmp(parameter, "grey_point_target")) grey = value;
      if(!strcmp(parameter, "white_point_target")) white = value;
      if(!(black < grey && grey < white)) return 0;
    }
    if(!strcmp(parameter, "latitude_stops") || !strcmp(parameter, "black_point_source")
       || !strcmp(parameter, "white_point_source"))
    {
      double black, white, latitude;
      if(!dt_mobile_policy_number(module, "black_point_source", DT_INTROSPECTION_TYPE_FLOAT, &black)
         || !dt_mobile_policy_number(module, "white_point_source", DT_INTROSPECTION_TYPE_FLOAT, &white)
         || !dt_mobile_policy_number(module, "latitude_stops", DT_INTROSPECTION_TYPE_FLOAT, &latitude)) return 0;
      if(!strcmp(parameter, "black_point_source")) black = value;
      if(!strcmp(parameter, "white_point_source")) white = value;
      if(!strcmp(parameter, "latitude_stops")) latitude = value;
      if(!(latitude < white - black)) return 0;
    }
  }
  if(!strcmp(module->op, "censorize")
     && (!strcmp(parameter, "radius_1") || !strcmp(parameter, "radius_2")))
    return value == 0.0 || value >= (double)0.1f;
  /* Auto-detection illuminants require a desktop picker callback; LAST is
   * an enumeration sentinel rather than a processing choice. */
  if(!strcmp(module->op, "channelmixerrgb"))
  {
    if(!strcmp(parameter, "illuminant"))
    {
      if(value == 8.0 || value == 9.0 || value == 11.0) return 0;
      if(value == 10.0)
      {
        if(!module->dev) return 0;
        const dt_image_t *image = &module->dev->image_storage;
        if(!dt_image_is_matrix_correction_supported(image) || dt_image_is_monochrome(image)) return 0;
      }
    }
    if(!strcmp(parameter, "adaptation"))
    {
      double illuminant, current;
      if(!dt_mobile_policy_number(module, "illuminant", DT_INTROSPECTION_TYPE_ENUM, &illuminant)
         || !dt_mobile_policy_number(module, "adaptation", DT_INTROSPECTION_TYPE_ENUM, &current)) return 0;
      /* The original camera-white-balance path chooses its adaptation itself;
       * its desktop selector is disabled in this state. Keep the effective
       * current value visible but reject changes that processing would ignore. */
      if(illuminant == 10.0 && value != current) return 0;
    }
  }
  if(!strcmp(module->op, "basicadj")
     && (!strcmp(parameter, "black_point") || !strcmp(parameter, "exposure")))
  {
    double black, exposure;
    if(!dt_mobile_policy_number(module, "black_point", DT_INTROSPECTION_TYPE_FLOAT, &black)
       || !dt_mobile_policy_number(module, "exposure", DT_INTROSPECTION_TYPE_FLOAT, &exposure)) return 0;
    if(!strcmp(parameter, "black_point")) black = value; else exposure = value;
    return (float)black < exp2f(-(float)exposure);
  }
  if(!strcmp(module->op, "filmicrgb")
     && (!strcmp(parameter, "black_point_target") || !strcmp(parameter, "grey_point_target")
         || !strcmp(parameter, "white_point_target") || !strcmp(parameter, "custom_grey")))
  {
    double black, grey, white, custom;
    if(!dt_mobile_policy_number(module, "black_point_target", DT_INTROSPECTION_TYPE_FLOAT, &black)
       || !dt_mobile_policy_number(module, "grey_point_target", DT_INTROSPECTION_TYPE_FLOAT, &grey)
       || !dt_mobile_policy_number(module, "white_point_target", DT_INTROSPECTION_TYPE_FLOAT, &white)
       || !dt_mobile_policy_number(module, "custom_grey", DT_INTROSPECTION_TYPE_BOOL, &custom)) return 0;
    if(!strcmp(parameter, "black_point_target")) black = value;
    if(!strcmp(parameter, "grey_point_target")) grey = value;
    if(!strcmp(parameter, "white_point_target")) white = value;
    if(!strcmp(parameter, "custom_grey")) custom = value;
    if(!custom) grey = (double)18.45f;
    return black < grey && grey < white;
  }
  if(!strcmp(module->op, "agx"))
  {
    if(!strcmp(parameter, "curve_pivot_y_linear_output") || !strcmp(parameter, "auto_gamma")
       || !strcmp(parameter, "curve_target_display_black_ratio")
       || !strcmp(parameter, "curve_target_display_white_ratio"))
    {
      double black, pivot, white, automatic;
      if(!dt_mobile_policy_number(module, "curve_target_display_black_ratio", DT_INTROSPECTION_TYPE_FLOAT, &black)
         || !dt_mobile_policy_number(module, "curve_pivot_y_linear_output", DT_INTROSPECTION_TYPE_FLOAT, &pivot)
         || !dt_mobile_policy_number(module, "curve_target_display_white_ratio", DT_INTROSPECTION_TYPE_FLOAT, &white)
         || !dt_mobile_policy_number(module, "auto_gamma", DT_INTROSPECTION_TYPE_BOOL, &automatic)) return 0;
      if(!strcmp(parameter, "curve_target_display_black_ratio")) black = value;
      if(!strcmp(parameter, "curve_pivot_y_linear_output")) pivot = value;
      if(!strcmp(parameter, "curve_target_display_white_ratio")) white = value;
      if(!strcmp(parameter, "auto_gamma")) automatic = value;
      if(!(black < pivot && pivot < white) || (automatic && !(pivot > 0.0 && pivot < 1.0))) return 0;
    }
    if(!strcmp(parameter, "master_outset_ratio") || !strcmp(parameter, "red_outset")
       || !strcmp(parameter, "green_outset") || !strcmp(parameter, "blue_outset")
       || !strcmp(parameter, "disable_primaries_adjustments")
       || !strcmp(parameter, "completely_reverse_primaries"))
    {
      double master, disabled, reverse;
      if(!dt_mobile_policy_number(module, "master_outset_ratio", DT_INTROSPECTION_TYPE_FLOAT, &master)
         || !dt_mobile_policy_number(module, "disable_primaries_adjustments", DT_INTROSPECTION_TYPE_BOOL, &disabled)
         || !dt_mobile_policy_number(module, "completely_reverse_primaries", DT_INTROSPECTION_TYPE_BOOL, &reverse)) return 0;
      if(!strcmp(parameter, "master_outset_ratio")) master = value;
      if(!strcmp(parameter, "disable_primaries_adjustments")) disabled = value;
      if(!strcmp(parameter, "completely_reverse_primaries")) reverse = value;
      if(!disabled && !reverse)
      {
        const char *names[] = { "red_outset", "green_outset", "blue_outset" };
        for(size_t k = 0; k < 3; k++)
        {
          double outset;
          if(!dt_mobile_policy_number(module, names[k], DT_INTROSPECTION_TYPE_FLOAT, &outset)) return 0;
          if(!strcmp(parameter, names[k])) outset = value;
          if(!(master * outset < 1.0)) return 0;
        }
      }
    }
  }
  return 1;
}

/* The desktop notebook supplies context for repeated hue-sector names. */
static inline const char *dt_mobile_extended_parameter_label(const char *operation,
                                                            const char *parameter, const char *fallback)
{
  static const struct { const char *operation, *parameter, *label; } contextual[] = {
    { "filmic", "grey_point_source", "source middle-gray luminance" },
    { "filmic", "black_point_source", "black relative exposure" },
    { "filmic", "white_point_source", "white relative exposure" },
    { "filmic", "grey_point_target", "target middle-gray luminance" },
    { "filmic", "black_point_target", "target black luminance" },
    { "filmic", "white_point_target", "target white luminance" },
    { "filmic", "output_power", "output gamma" },
    { "filmic", "latitude_stops", "latitude (EV)" },
    { "filmic", "saturation", "extreme luminance saturation" },
    { "filmic", "global_saturation", "global saturation" },
    { "filmic", "balance", "shadows / highlights balance" },
    { "filmic", "interpolator", "curve intent" },
    { "filmic", "preserve_color", "preserve chrominance" },
    { "bilat", "sigma_r", "highlights / range" },
    { "bilat", "sigma_s", "shadows / spatial extent" },
    { "retouch", "algorithm", "algorithm for new shapes" },
    { "retouch", "num_scales", "wavelet decomposition scales" },
    { "retouch", "curr_scale", "scale for new shapes" },
    { "retouch", "merge_from_scale", "merge from scale" },
    { "retouch", "max_heal_iter", "maximum heal iterations" },
  };
  for(size_t k = 0; k < sizeof(contextual) / sizeof(contextual[0]); k++)
    if(!strcmp(operation, contextual[k].operation) && !strcmp(parameter, contextual[k].parameter))
      return contextual[k].label;
  if(!strcmp(operation, "colorcorrection"))
  {
    if(!strcmp(parameter, "hia")) return "highlights a";
    if(!strcmp(parameter, "hib")) return "highlights b";
    if(!strcmp(parameter, "loa")) return "shadows a";
    if(!strcmp(parameter, "lob")) return "shadows b";
  }
  if(!strcmp(operation, "colorequal"))
  {
    static const struct { const char *parameter, *label; } labels[] = {
      { "sat_red", "red saturation" },
      { "sat_orange", "orange saturation" },
      { "sat_yellow", "yellow saturation" },
      { "sat_green", "green saturation" },
      { "sat_cyan", "cyan saturation" },
      { "sat_blue", "blue saturation" },
      { "sat_lavender", "lavender saturation" },
      { "sat_magenta", "magenta saturation" },
      { "hue_red", "red hue" },
      { "hue_orange", "orange hue" },
      { "hue_yellow", "yellow hue" },
      { "hue_green", "green hue" },
      { "hue_cyan", "cyan hue" },
      { "hue_blue", "blue hue" },
      { "hue_lavender", "lavender hue" },
      { "hue_magenta", "magenta hue" },
      { "bright_red", "red brightness" },
      { "bright_orange", "orange brightness" },
      { "bright_yellow", "yellow brightness" },
      { "bright_green", "green brightness" },
      { "bright_cyan", "cyan brightness" },
      { "bright_blue", "blue brightness" },
      { "bright_lavender", "lavender brightness" },
      { "bright_magenta", "magenta brightness" },
    };
    for(size_t k = 0; k < sizeof(labels) / sizeof(labels[0]); k++)
      if(!strcmp(parameter, labels[k].parameter)) return labels[k].label;
  }
  return fallback;
}

#endif
