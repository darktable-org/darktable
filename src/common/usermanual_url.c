/*
    This file is part of darktable,

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/usermanual_url.h"

char *dt_get_help_url(char *name)
{
  if(name==NULL) return NULL;
  if(!strcmp(name, "ratings")) return "star_ratings_and_color_labels.html#star_ratings_and_color_labels";
  if(!strcmp(name, "filter")) return "filtering_and_sort_order.html#filtering_and_sort_order";
  if(!strcmp(name, "colorlabels")) return "star_ratings_and_color_labels.html#star_ratings_and_color_labels";
  if(!strcmp(name, "import")) return "lighttable_panels.html#import";
  if(!strcmp(name, "select")) return "select.html#select";
  if(!strcmp(name, "image")) return "selected_images.html#selected_images";
  if(!strcmp(name, "copy_history")) return "history_stack.html#history_stack";
  if(!strcmp(name, "styles")) return "styles.html#styles";
  if(!strcmp(name, "metadata")) return "metadata_editor.html#metadata_editor";
  if(!strcmp(name, "tagging")) return "tagging.html#tagging";
  if(!strcmp(name, "geotagging")) return "geotagging.html#geotagging";
  if(!strcmp(name, "collect")) return "collect_images.html#collect_images";
  if(!strcmp(name, "recentcollect")) return "recently_used_collections.html#recently_used_collections";
  if(!strcmp(name, "metadata_view")) return "image_information.html#image_information";
  if(!strcmp(name, "export")) return "export_selected.html#export_selected";
  if(!strcmp(name, "histogram")) return "histogram.html#histogram";
  if(!strcmp(name, "navigation")) return "darkroom_panels.html#navigation";
  if(!strcmp(name, "snapshots")) return "snapshots.html#snapshots";
  if(!strcmp(name, "modulegroups")) return "module_groups.html#module_groups";
  if(!strcmp(name, "history")) return "history.html#history";
  if(!strcmp(name, "colorpicker")) return "global_color_picker.html#global_color_picker";
  if(!strcmp(name, "masks")) return "mask_manager.html#mask_manager";
  if(!strcmp(name, "duplicate")) return "duplicate.html#duplicate";
  if(!strcmp(name, "modulelist")) return "more_modules.html#more_modules";
  if(!strcmp(name, "location")) return "find_location.html#find_location";
  if(!strcmp(name, "map_settings")) return "map_settings.html#map_settings";
  if(!strcmp(name, "print_settings")) return "print_settings.html#print_settings";
  if(!strcmp(name, "global_toolbox")) return NULL;
  if(!strcmp(name, "global_toolbox_preferences")) return "preferences.html#preferences";
  if(!strcmp(name, "global_toolbox_help")) return "contextual_help.html#contextual_help";
  if(!strcmp(name, "lighttable_mode")) return "lighttable_chapter.html#lighttable_overview";
  if(!strcmp(name, "module_toolbox")) return NULL;
  if(!strcmp(name, "view_toolbox")) return NULL;
  if(!strcmp(name, "backgroundjobs")) return NULL;
  if(!strcmp(name, "hinter")) return NULL;
  if(!strcmp(name, "filter")) return NULL;
  if(!strcmp(name, "filmstrip")) return "filmstrip_overview.html#filmstrip_overview";
  if(!strcmp(name, "viewswitcher")) return "user_interface.html#views";
  if(!strcmp(name, "favorite_presets")) return "darkroom_bottom_panel.html#favorite_presets";
  if(!strcmp(name, "bottom_panel_styles")) return "darkroom_bottom_panel.html#darkroom_bottom_panel_styles";
  if(!strcmp(name, "rawoverexposed")) return "darkroom_bottom_panel.html#rawoverexposed";
  if(!strcmp(name, "overexposed")) return "darkroom_bottom_panel.html#overexposed";
  if(!strcmp(name, "softproof")) return "darkroom_bottom_panel.html#softproof";
  if(!strcmp(name, "gamut")) return "darkroom_bottom_panel.html#gamutcheck";
  if(!strcmp(name, "dither")) return "correction_group.html#dithering";
  if(!strcmp(name, "watermark")) return "effect_group.html#watermark";
  if(!strcmp(name, "borders")) return "effect_group.html#framing";
  if(!strcmp(name, "clahe")) return "tone_group.html#local_contrast";
  if(!strcmp(name, "velvia")) return "color_group.html#velvia";
  if(!strcmp(name, "splittoning")) return "effect_group.html#splittoning";
  if(!strcmp(name, "vignette")) return "effect_group.html#vignetting";
  if(!strcmp(name, "soften")) return "effect_group.html#soften";
  if(!strcmp(name, "channelmixer")) return "color_group.html#channel_mixer";
  if(!strcmp(name, "colorout")) return "color_group.html#output_color_profile";
  if(!strcmp(name, "colorcontrast")) return "color_group.html#color_contrast";
  if(!strcmp(name, "grain")) return "effect_group.html#grain";
  if(!strcmp(name, "highpass")) return "effect_group.html#highpass";
  if(!strcmp(name, "lowpass")) return "effect_group.html#lowpass";
  if(!strcmp(name, "sharpen")) return "correction_group.html#sharpen";
  if(!strcmp(name, "colorcorrection")) return "color_group.html#color_correction";
  if(!strcmp(name, "relight")) return "tone_group.html#fill_light";
  if(!strcmp(name, "levels")) return "tone_group.html#levels";
  if(!strcmp(name, "tonecurve")) return "tone_group.html#tone_curve";
  if(!strcmp(name, "zonesystem")) return "tone_group.html#zone_system";
  if(!strcmp(name, "colisa")) return "modules.html#contrast_brightness_saturation";
  if(!strcmp(name, "monochrome")) return "color_group.html#monochrome";
  if(!strcmp(name, "lowlight")) return "effect_group.html#low_light";
  if(!strcmp(name, "colorzones")) return "color_group.html#color_zones";
  if(!strcmp(name, "bilat")) return "tone_group.html#local_contrast";
  if(!strcmp(name, "atrous")) return "correction_group.html#equalizer";
  if(!strcmp(name, "shadhi")) return "modules.html#shadows_and_highlights";
  if(!strcmp(name, "globaltonemap")) return "tone_group.html#global_tonemap";
  if(!strcmp(name, "nlmeans")) return "correction_group.html#denoise_non_local_means";
  if(!strcmp(name, "bloom")) return "effect_group.html#bloom";
  if(!strcmp(name, "colormapping")) return "effect_group.html#color_mapping";
  if(!strcmp(name, "colortransfer")) return NULL;
  if(!strcmp(name, "colorize")) return "effect_group.html#colorize";
  if(!strcmp(name, "clipping")) return "modules.html#crop_and_rotate";
  if(!strcmp(name, "colorbalance")) return "color_group.html#color_balance";
  if(!strcmp(name, "vibrance")) return "color_group.html#vibrance";
  if(!strcmp(name, "equalizer")) return "correction_group.html#equalizer";
  if(!strcmp(name, "defringe")) return "correction_group.html#defringe";
  if(!strcmp(name, "colorchecker")) return "color_group.html#color_look_up_table";
  if(!strcmp(name, "colorreconstruct")) return "modules.html#color_reconstruction";
  if(!strcmp(name, "colorin")) return "color_group.html#input_color_profile";
  if(!strcmp(name, "hazeremoval")) return "correction_group.html#haze_removal";
  if(!strcmp(name, "profile_gamma")) return "color_group.html#unbreak_input_profile";
  if(!strcmp(name, "bilateral")) return "correction_group.html#denoise_bilateral";
  if(!strcmp(name, "basecurve")) return "modules.html#base_curve";
  if(!strcmp(name, "graduatednd")) return "effect_group.html#graduated_density";
  if(!strcmp(name, "flip")) return "modules.html#orientation";
  if(!strcmp(name, "scalepixels")) return "correction_group.html#scale_pixels";
  if(!strcmp(name, "rotatepixels")) return "correction_group.html#rotate_pixels";
  if(!strcmp(name, "liquify")) return "correction_group.html#liquify";
  if(!strcmp(name, "ashift")) return "correction_group.html#perspective_correction";
  if(!strcmp(name, "lens")) return "correction_group.html#lens_correction";
  if(!strcmp(name, "retouch")) return "correction_group.html#retouch";
  if(!strcmp(name, "spots")) return "correction_group.html#spot_removal";
  if(!strcmp(name, "exposure")) return "modules.html#exposure";
  if(!strcmp(name, "tonemap")) return "tone_group.html#tonemapping";
  if(!strcmp(name, "denoiseprofile")) return "correction_group.html#denoise_profiled";
  if(!strcmp(name, "demosaic")) return "modules.html#demosaic";
  if(!strcmp(name, "rawdenoise")) return "correction_group.html#raw_denoise";
  if(!strcmp(name, "hotpixels")) return "correction_group.html#hotpixels";
  if(!strcmp(name, "cacorrect")) return "correction_group.html#chromatic_aberrations";
  if(!strcmp(name, "highlights")) return "modules.html#highlight_reconstruction";
  if(!strcmp(name, "temperature")) return "modules.html#whitebalance";
  if(!strcmp(name, "invert")) return "modules.html#invert";
  if(!strcmp(name, "rawprepare")) return "modules.html#raw_black_white_point";
  return NULL;
}
