/*
    This file is part of darktable,
    Copyright (C) 2018-2020 darktable developers.

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

typedef struct _help_url
{
  char *name;
  char *url;
} dt_help_url;

dt_help_url db[] =
{
  {"ratings",                    "star_ratings_and_color_labels.html#star_ratings_and_color_labels"},
  {"filter",                     "filtering_and_sort_order.html#filtering_and_sort_order"},
  {"colorlabels",                "star_ratings_and_color_labels.html#star_ratings_and_color_labels"},
  {"import",                     "lighttable_panels.html#import"},
  {"select",                     "select.html#select"},
  {"image",                      "selected_images.html#selected_images"},
  {"copy_history",               "history_stack.html#history_stack"},
  {"styles",                     "styles.html#styles"},
  {"metadata",                   "metadata_editor.html#metadata_editor"},
  {"tagging",                    "tagging.html#tagging"},
  {"geotagging",                 "geotagging.html#geotagging"},
  {"collect",                    "collect_images.html#collect_images"},
  {"recentcollect",              "recently_used_collections.html#recently_used_collections"},
  {"metadata_view",              "image_information.html#image_information"},
  {"export",                     "export_selected.html#export_selected"},
  {"histogram",                  "histogram.html#histogram"},
  {"navigation",                 "darkroom_panels.html#navigation"},
  {"snapshots",                  "snapshots.html#snapshots"},
  {"modulegroups",               "module_groups.html#module_groups"},
  {"history",                    "history.html#history"},
  {"colorpicker",                "global_color_picker.html#global_color_picker"},
  {"masks",                      "mask_manager.html#mask_manager"},
  {"duplicate",                  "duplicate.html#duplicate"},
  {"location",                   "find_location.html#find_location"},
  {"map_settings",               "map_settings.html#map_settings"},
  {"print_settings",             "print_settings.html#print_settings"},
  {"global_toolbox",             NULL},
  {"global_toolbox_preferences", "preferences.html#preferences"},
  {"global_toolbox_help",        "contextual_help.html#contextual_help"},
  {"lighttable_mode",            "lighttable_chapter.html#lighttable_overview"},
  {"lighttable_filemanager",     "lighttable_chapter.html#lighttable_filemanager"},
  {"lighttable_zoomable",        "lighttable_chapter.html#lighttable_zoomable"},
  {"module_toolbox",             NULL},
  {"view_toolbox",               NULL},
  {"backgroundjobs",             NULL},
  {"hinter",                     NULL},
  {"filter",                     NULL},
  {"filmstrip",                  "filmstrip_overview.html#filmstrip_overview"},
  {"viewswitcher",               "user_interface.html#views"},
  {"favorite_presets",           "darkroom_bottom_panel.html#favorite_presets"},
  {"bottom_panel_styles",        "darkroom_bottom_panel.html#darkroom_bottom_panel_styles"},
  {"rawoverexposed",             "darkroom_bottom_panel.html#rawoverexposed"},
  {"overexposed",                "darkroom_bottom_panel.html#overexposed"},
  {"softproof",                  "darkroom_bottom_panel.html#softproof"},
  {"gamut",                      "darkroom_bottom_panel.html#gamutcheck"},

  // iop links
  {"ashift",                     "technical_group.html#perspective_correction"},
  {"atrous",                     "effects_group.html#equalizer"},
  {"basecurve",                  "technical_group.html#base_curve"},
  {"bilateral",                  "technical_group.html#denoise_bilateral"},
  {"bilat",                      "effects_group.html#local_contrast"},
  {"bloom",                      "effects_group.html#bloom"},
  {"borders",                    "effects_group.html#framing"},
  {"cacorrect",                  "technical_group.html#chromatic_aberrations"},
  {"channelmixer",               "grading_group.html#channel_mixer"},
  {"clahe",                      NULL}, // deprecated, replaced by bilat.
  {"clipping",                   "technical_group.html#crop_and_rotate"},
  {"colisa",                     "grading_group.html#contrast_brightness_saturation"},
  {"colorbalance",               "grading_group.html#color_balance"},
  {"colorchecker",               "technical_group.html#color_look_up_table"},
  {"colorcontrast",              "grading_group.html#color_contrast"},
  {"colorcorrection",            "grading_group.html#color_correction"},
  {"colorin",                    "technical_group.html#input_color_profile"},
  {"colorize",                   "grading_group.html#colorize"},
  {"colormapping",               "effects_group.html#color_mapping"},
  {"colorout",                   "technical_group.html#output_color_profile"},
  {"colorreconstruct",           "technical_group.html#color_reconstruction"},
  {"colortransfer",              NULL}, // deprecate
  {"colorzones",                 "grading_group.html#color_zones"},
  {"defringe",                   "technical_group.html#defringe"},
  {"demosaic",                   "technical_group.html#demosaic"},
  {"denoiseprofile",             "technical_group.html#denoise_profiled"},
  {"dither",                     "technical_group.html#dithering"},
  {"equalizer",                  NULL}, // deprecated, replaced by atrous
  {"exposure",                   "technical_group.html#exposure"},
  {"filmic",                     "technical_group.html#filmic"},
  {"filmicrgb",                  "technical_group.html#filmic"},
  {"flip",                       "technical_group.html#orientation"},
  {"globaltonemap",              "grading_group.html#global_tonemap"},
  {"graduatednd",                "grading_group.html#graduated_density"},
  {"grain",                      "effects_group.html#grain"},
  {"hazeremoval",                "technical_group.html#haze_removal"},
  {"highlights",                 "technical_group.html#highlight_reconstruction"},
  {"highpass",                   "effects_group.html#highpass"},
  {"hotpixels",                  "technical_group.html#hotpixels"},
  {"invert",                     "technical_group.html#invert"},
  {"lens",                       "technical_group.html#lens_correction"},
  {"levels",                     "grading_group.html#levels"},
  {"liquify",                    "effects_group.html#liquify"},
  {"lowlight",                   "effects_group.html#low_light"},
  {"lowpass",                    "effects_group.html#lowpass"},
  {"lut3d",                      "technical_group.html#lut3d"},
  {"monochrome",                 "effects_group.html#monochrome"},
  {"negadoctor",                 "technical_group.html#negadoctor"},
  {"nlmeans",                    "technical_group.html#denoise_non_local_means"},
  {"profile_gamma",              "technical_group.html#unbreak_input_profile"},
  {"rawdenoise",                 "technical_group.html#raw_denoise"},
  {"rawprepare",                 "technical_group.html#raw_black_white_point"},
  {"relight",                    "grading_group.html#fill_light"},
  {"retouch",                    "effects_group.html#retouch"},
  {"rgbcurve",                   "grading_group.html#rgbcurve"},
  {"rgblevels",                  "grading_group.html#rgblevels"},
  {"rotatepixels",               "technical_group.html#rotate_pixels"},
  {"scalepixels",                "technical_group.html#scale_pixels"},
  {"shadhi",                     "grading_group.html#shadows_and_highlights"},
  {"sharpen",                    "effects_group.html#sharpen"},
  {"soften",                     "effects_group.html#soften"},
  {"splittoning",                "grading_group.html#splittoning"},
  {"spots",                      "effects_group.html#spot_removal"},
  {"temperature",                "grading_group.html#whitebalance"},
  {"tonecurve",                  "grading_group.html#tone_curve"},
  {"toneequal",                  "grading_group.html#toneequalizer"},
  {"tonemap",                    "grading_group.html#tonemapping"},
  {"velvia",                     "grading_group.html#velvia"},
  {"vibrance",                   "grading_group.html#vibrance"},
  {"vignette",                   "effects_group.html#vignetting"},
  {"watermark",                  "effects_group.html#watermark"},
  {"zonesystem",                 "grading_group.html#zone_system"},
};

char *dt_get_help_url(char *name)
{
  if(name==NULL) return NULL;

  for(int k=0; k< sizeof(db)/2/sizeof(char *); k++)
    if(!strcmp(db[k].name, name)) return db[k].url;

  return NULL;
}
