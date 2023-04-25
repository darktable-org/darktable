/*
    This file is part of darktable,
    Copyright (C) 2016-2023 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/math.h"
#include "common/opencl.h"
#include "common/exif.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/openmp_maths.h"
#include "develop/tiling.h"
#include "dtgtk/drawingarea.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include "iop/gaussian_elimination.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE_INTROSPECTION(2, dt_iop_colorchecker_params_t)

static const int colorchecker_patches = 24;
static const float colorchecker_Lab[] =
{ // from argyll ColorChecker.cie
 37.99,   13.56,  14.06, // dark skin
 65.71,   18.13,  17.81, // light skin
 49.93,   -4.88, -21.93, // blue sky
 43.14,  -13.10,  21.91, // foliage
 55.11,    8.84, -25.40, // blue flower
 70.72,  -33.40, -0.20 , // bluish green
 62.66,   36.07,  57.10, // orange
 40.02,   10.41, -45.96, // purple red
 51.12,   48.24,  16.25, // moderate red
 30.33,   22.98, -21.59, // purple
 72.53,  -23.71,  57.26, // yellow green
 71.94,  19.36 ,  67.86, // orange yellow
 28.78,  14.18 , -50.30, // blue
 55.26,  -38.34,  31.37, // green
 42.10,  53.38 ,  28.19, // red
 81.73,  4.04  ,  79.82, // yellow
 51.94,  49.99 , -14.57, // magenta
 51.04,  -28.63, -28.64, // cyan
 96.54,  -0.43 ,  1.19 , // white
 81.26,  -0.64 , -0.34 , // neutral 8
 66.77,  -0.73 , -0.50 , // neutral 65
 50.87,  -0.15 , -0.27 , // neutral 5
 35.66,  -0.42 , -1.23 , // neutral 35
 20.46,  -0.08 , -0.97   // black
};

// we came to the conclusion that more than 7x7 patches will not be
// manageable in the gui. the fitting experiments show however that you
// can do significantly better with 49 than you can with 24 patches,
// especially when considering max delta E.
#define MAX_PATCHES 49
typedef struct dt_iop_colorchecker_params_t
{
  float source_L[MAX_PATCHES];
  float source_a[MAX_PATCHES];
  float source_b[MAX_PATCHES];
  float target_L[MAX_PATCHES];
  float target_a[MAX_PATCHES];
  float target_b[MAX_PATCHES];
  int32_t num_patches;
} dt_iop_colorchecker_params_t;

typedef struct dt_iop_colorchecker_gui_data_t
{
  GtkWidget *area, *combobox_patch, *scale_L, *scale_a, *scale_b, *scale_C, *combobox_target;
  int patch, drawn_patch;
  int absolute_target; // 0: show relative offsets in sliders, 1: show absolute Lab values
} dt_iop_colorchecker_gui_data_t;

typedef struct dt_iop_colorchecker_data_t
{
  int32_t num_patches;
  float source_Lab[3*MAX_PATCHES];
  float coeff_L[MAX_PATCHES+4];
  float coeff_a[MAX_PATCHES+4];
  float coeff_b[MAX_PATCHES+4];
} dt_iop_colorchecker_data_t;

typedef struct dt_iop_colorchecker_global_data_t
{
  int kernel_colorchecker;
} dt_iop_colorchecker_global_data_t;


const char *name()
{
  return _("color look up table");
}

const char *aliases()
{
  return _("profile|lut|color grading");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("perform color space corrections and apply looks"),
                                      _("corrective or creative"),
                                      _("linear or non-linear, Lab, display-referred"),
                                      _("defined by profile, Lab"),
                                      _("linear or non-linear, Lab, display-referred"));
}


int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(
    dt_iop_module_t *self,
    dt_dev_pixelpipe_t *pipe,
    dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

int legacy_params(
    dt_iop_module_t  *self,
    const void *const old_params,
    const int         old_version,
    void             *new_params,
    const int         new_version)
{
  static const float colorchecker_Lab_v1[] = {
    39.19, 13.76,  14.29,  // dark skin
    65.18, 19.00,  17.32,  // light skin
    49.46, -4.23,  -22.95, // blue sky
    42.85, -13.33, 22.12,  // foliage
    55.18, 9.44,   -24.94, // blue flower
    70.36, -32.77, -0.04,  // bluish green
    62.92, 35.49,  57.10,  // orange
    40.75, 11.41,  -46.03, // purple red
    52.10, 48.11,  16.89,  // moderate red
    30.67, 21.19,  -20.81, // purple
    73.08, -23.55, 56.97,  // yellow green
    72.43, 17.48,  68.20,  // orange yellow
    30.97, 12.67,  -46.30, // blue
    56.43, -40.66, 31.94,  // green
    43.40, 50.68,  28.84,  // red
    82.45, 2.41,   80.25,  // yellow
    51.98, 50.68,  -14.84, // magenta
    51.02, -27.63, -28.03, // cyan
    95.97, -0.40,  1.24,   // white
    81.10, -0.83,  -0.43,  // neutral 8
    66.81, -1.08,  -0.70,  // neutral 65
    50.98, -0.19,  -0.30,  // neutral 5
    35.72, -0.69,  -1.11,  // neutral 35
    21.46, 0.06,   -0.95,  // black
  };

  typedef struct dt_iop_colorchecker_params_v1_t
  {
    float target_L[24];
    float target_a[24];
    float target_b[24];
  } dt_iop_colorchecker_params_v1_t;

  if(old_version == 1 && new_version == 2)
  {
    dt_iop_colorchecker_params_v1_t *p1 = (dt_iop_colorchecker_params_v1_t *)old_params;
    dt_iop_colorchecker_params_t  *p2 = (dt_iop_colorchecker_params_t  *)new_params;

    p2->num_patches = 24;
    for(int k=0;k<24;k++)
    {
      p2->target_L[k] = p1->target_L[k];
      p2->target_a[k] = p1->target_a[k];
      p2->target_b[k] = p1->target_b[k];
      p2->source_L[k] = colorchecker_Lab_v1[3 * k + 0];
      p2->source_a[k] = colorchecker_Lab_v1[3 * k + 1];
      p2->source_b[k] = colorchecker_Lab_v1[3 * k + 2];
    }
    return 0;
  }
  return 1;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_colorchecker_params_t p;
  memset(&p, 0, sizeof(p));
  p.num_patches = 24;
  p.target_L[ 0] = p.source_L[ 0] = 17.460945129394531;
  p.target_L[ 1] = p.source_L[ 1] = 26.878498077392578;
  p.target_L[ 2] = p.source_L[ 2] = 34.900054931640625;
  p.target_L[ 3] = p.source_L[ 3] = 21.692604064941406;
  p.target_L[ 4] = p.source_L[ 4] = 32.18853759765625;
  p.target_L[ 5] = p.source_L[ 5] = 62.531227111816406;
  p.target_L[ 6] = p.source_L[ 6] = 18.933284759521484;
  p.target_L[ 7] = p.source_L[ 7] = 53.936111450195312;
  p.target_L[ 8] = p.source_L[ 8] = 69.154266357421875;
  p.target_L[ 9] = p.source_L[ 9] = 43.381229400634766;
  p.target_L[10] = p.source_L[10] = 57.797889709472656;
  p.target_L[11] = p.source_L[11] = 73.27630615234375;
  p.target_L[12] = p.source_L[12] = 53.175498962402344;
  p.target_L[13] = p.source_L[13] = 49.111373901367188;
  p.target_L[14] = p.source_L[14] = 63.169830322265625;
  p.target_L[15] = p.source_L[15] = 61.896102905273438;
  p.target_L[16] = p.source_L[16] = 67.852409362792969;
  p.target_L[17] = p.source_L[17] = 72.489517211914062;
  p.target_L[18] = p.source_L[18] = 70.935714721679688;
  p.target_L[19] = p.source_L[19] = 70.173004150390625;
  p.target_L[20] = p.source_L[20] = 77.78826904296875;
  p.target_L[21] = p.source_L[21] = 76.070747375488281;
  p.target_L[22] = p.source_L[22] = 68.645004272460938;
  p.target_L[23] = p.source_L[23] = 74.502906799316406;
  p.target_a[ 0] = p.source_a[ 0] = 8.4928874969482422;
  p.target_a[ 1] = p.source_a[ 1] = 27.94782829284668;
  p.target_a[ 2] = p.source_a[ 2] = 43.8824462890625;
  p.target_a[ 3] = p.source_a[ 3] = 16.723676681518555;
  p.target_a[ 4] = p.source_a[ 4] = 39.174972534179688;
  p.target_a[ 5] = p.source_a[ 5] = 24.966419219970703;
  p.target_a[ 6] = p.source_a[ 6] = 8.8226642608642578;
  p.target_a[ 7] = p.source_a[ 7] = 34.451812744140625;
  p.target_a[ 8] = p.source_a[ 8] = 18.39008903503418;
  p.target_a[ 9] = p.source_a[ 9] = 28.272598266601562;
  p.target_a[10] = p.source_a[10] = 10.193824768066406;
  p.target_a[11] = p.source_a[11] = 13.241470336914062;
  p.target_a[12] = p.source_a[12] = 43.655307769775391;
  p.target_a[13] = p.source_a[13] = 23.247600555419922;
  p.target_a[14] = p.source_a[14] = 23.308664321899414;
  p.target_a[15] = p.source_a[15] = 11.138319969177246;
  p.target_a[16] = p.source_a[16] = 18.200069427490234;
  p.target_a[17] = p.source_a[17] = 15.363990783691406;
  p.target_a[18] = p.source_a[18] = 11.173545837402344;
  p.target_a[19] = p.source_a[19] = 11.313735961914062;
  p.target_a[20] = p.source_a[20] = 15.059500694274902;
  p.target_a[21] = p.source_a[21] = 4.7686996459960938;
  p.target_a[22] = p.source_a[22] = 3.0603706836700439;
  p.target_a[23] = p.source_a[23] = -3.687053918838501;
  p.target_b[ 0] = p.source_b[ 0] = -0.023579597473144531;
  p.target_b[ 1] = p.source_b[ 1] = 14.991056442260742;
  p.target_b[ 2] = p.source_b[ 2] = 26.443553924560547;
  p.target_b[ 3] = p.source_b[ 3] = 7.3905587196350098;
  p.target_b[ 4] = p.source_b[ 4] = 23.309671401977539;
  p.target_b[ 5] = p.source_b[ 5] = 19.262432098388672;
  p.target_b[ 6] = p.source_b[ 6] = 3.136211633682251;
  p.target_b[ 7] = p.source_b[ 7] = 31.949621200561523;
  p.target_b[ 8] = p.source_b[ 8] = 16.144514083862305;
  p.target_b[ 9] = p.source_b[ 9] = 25.893926620483398;
  p.target_b[10] = p.source_b[10] = 12.271202087402344;
  p.target_b[11] = p.source_b[11] = 16.763805389404297;
  p.target_b[12] = p.source_b[12] = 53.904998779296875;
  p.target_b[13] = p.source_b[13] = 36.537342071533203;
  p.target_b[14] = p.source_b[14] = 32.930683135986328;
  p.target_b[15] = p.source_b[15] = 19.008804321289062;
  p.target_b[16] = p.source_b[16] = 32.259223937988281;
  p.target_b[17] = p.source_b[17] = 25.815582275390625;
  p.target_b[18] = p.source_b[18] = 26.509498596191406;
  p.target_b[19] = p.source_b[19] = 40.572704315185547;
  p.target_b[20] = p.source_b[20] = 88.354469299316406;
  p.target_b[21] = p.source_b[21] = 33.434604644775391;
  p.target_b[22] = p.source_b[22] = 9.5750093460083008;
  p.target_b[23] = p.source_b[23] = 41.285167694091797;
  dt_gui_presets_add_generic(_("it8 skin tones"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Helmholtz/Kohlrausch effect applied to black and white conversion.
  // implemented by wmader as an iop and matched as a clut for increased
  // flexibility. this was done using darktable-chart and this is copied
  // from the resulting dtstyle output file:
  const char *hk_params_input =
    "9738b84231c098426fb8814234a82d422ac41d422e3fa04100004843f7daa24257e09a422a1a984225113842f89cc9410836ca4295049542ad1c9242887370427cb32b427c512242b5a40742545bd141808740412cc6964262e484429604c44100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000ef6d3bc152c2acc1ef6566c093a522c2e7d4e4c1a87c7cc100000000b4c4dd407af09e40d060df418afc7d421dadd0413ec5124097d79041fcba2642fc9f484183eb92415d6b7040fcdcdc41b8fe2f42b64a1740fc8612c1276defc144432ec100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000d237eb4022a72842f5639742396d1442a2660d411c338b40000000006e35ca408df2054289658d4132327a4118427741d4cf08c0f8a4d5c03abed7c13fac36c23b41a6c03c2230c07d5088c26caff7c1e0e9c6bff14ecec073b028c29e0accc10000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000085f2b642a4ba9a423c9a8442a6493c428baf28425667b64100004843a836a142a84e9b4226719d421cb15d424c22ee4175fcca4211ae96426e6d9a4243878142ef45354222f82542629527420280ff416c2066417e3996420d838e424182e3410000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000fa370000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000c8b700000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000004837000000000000c8b60000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000018000000";
  int params_len = 0;
  uint8_t *hk_params = dt_exif_xmp_decode(
      hk_params_input, strlen(hk_params_input), &params_len);
  assert(params_len == sizeof(dt_iop_colorchecker_params_t));
  assert(hk_params);
  dt_gui_presets_add_generic(_("Helmholtz/Kohlrausch monochrome"), self->op,
                             self->version(), hk_params, params_len, 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  free(hk_params);

  /** The following are based on Jo's Fuji film emulations, without tonecurve which is let to user choice
   *  https://jo.dreggn.org/blog/darktable-fuji-styles.tar.xz
   * **/

  const char *astia_params_input =
  "20f59e427e278d42a2ae6f4218265742c69f4e4282bb1b4200831942eca40942d85cb641000048430000c842083a964214368d42fb258b42928b73424cad4d4231ab3e42093f3c42d38e0c42d828fb412299b841c6e7ad41b2a0a44296dd90422827874224e97c42f4606f425c795b42088b434229b7154206ff1442f61f074229a70442a620fa4120bc9b4160729b41bc109b41ce889441be73904110486e419878b940fa849142fc3c7d42e4d37442aed36f42c5b50d42877d0742e821a0411ae11341a871a4be4a1979c17d9794c18c26ebc17682e8bfec9823c1d2ae6cc03bca04c27ea111c10000000000000000bcda0b3f18478e40040b023f66ca9741097a96413c7eb14104090b41079b0b4236804a423a1624412c95ab41f8e0323f672c684136a909401fb4dc4134380e4188acfe400e6d3e425f60564040228d40b041904176f8dd41127986420bcc2a42b88bc041e7eaa9402ab50341e5f6f841a2dab840333c36426ae64fc106e5aac1a0eac5c19e42babf844ad8c139be78c198f65fc1101fa8bda089444163890b413a7f8a41c748b741979736422c2798413b18fc4024fde6414f3b73410000000000000000fcfb134234fb754246425b4140dc353f4487ce412cf53142ea844d41089ebb41bc42ed411c3d7641af131b41aea35ac0e48351c13f1a92c0b182a7c1892d8ac158c606c2406af6c1992d3ac1dd9ae2c149a950c2c608e7c0c0ff0dc268aaf3c1bf8b90c1aea004c21f564bc2db46c9c0a8a098bf5ee18cc20b3878c18de1d7c1e0c533c142ba1bc1ecd83cc106d411c20603e9c0907a30c0bea4a142fe288c42d48b6042a4c54e42ac414842f68a1542804a1442510b06429c18ac41264845435e58b24213c197428e4b8d4255e18c42ceb17542d0d64042d3293942f92f364293aa0f4296bc0c42b42fb841ceadb441ca69a542e67e984293338742c2248742a8c07c42ee3c6342923a5a429e07184213dc2042d6901f42301d0d42778a2442d6dfd74108a7b541baecc641de56e841bedfb3417a076f41ec9dc24123d19742081185424e427a427c4578424ab81942c07c224200eea94108d1134170d930bfd5e49ac143b4adc1e3180bc2248b4dbf3e6624c13e266bc034f6c6c1f5a3ecc000803bb9008890baf892bf3eb7ffc0400a16fd3f497ab04161009a416eddc941121a0d417b740d42cbf6354235603e4136ce9c41002c493eda48614199e90640ac88f64135230e41a69fac40dbb23c427bce3540a18b4d40f4ce5a41c7b0d84110816b42b4ddf741d01a98418d2510413dcc8b412331bd41efe896407578e64129fd98c1617010c2242005c23e4d85c05be37ac194fa68bf0178d2c028bacc3d46f2674121d83a413a349f416a60d141d6e0264272e8a2417c590f414c1cc241c4df634100e0f63a00b6003c1df73442b2b97442d4d78f41481be73f06bbca41d39c1642f48c674191c5a8414638b9413cc6794191c3354102e024c0262653c11276b8c07a3ad5c1d4d8c1c1e7b039c28ec129c2b5156ec1d82a26c2160a97c2626400c1bec74ac2fe5bf6c1465e87c13ab90dc2c5c47ec2581a2bc038ea0cbf06b38bc2488593c1f8140dc240a6b6c1689254c182c683c13e216cc2a03dd9c0028e10c031000000";

  uint8_t *astia_params = dt_exif_xmp_decode(
      astia_params_input, strlen(astia_params_input), &params_len);

  assert(params_len == sizeof(dt_iop_colorchecker_params_t));
  assert(astia_params);
  dt_gui_presets_add_generic(_("Fuji Astia emulation"), self->op,
                             self->version(), astia_params, params_len, 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  free(astia_params);


  const char *chrome_params_input =
  "d303b542eb5a9742ccdd7d4288707142ee9d40427af718427062d641000048430000c8420d96bc42faeaae429c32aa423a6ca9423c9ba7425993a0424e639542788d9242a722894260eb7f42d2876b420c724442dcba4042b6c02b42a8990b421276de41ac68c2410790a542393b9242a7279242a45d8f42a132864230e57e42002145426c3f44428a0b274204e62342b092fd41d68fcd41e02cbb419e07bb41ac2433413247b742a3ad9242006a924293d98142ae892e422cd42642366a26429c7ec44175d738c170f6d7c16fbc62c0116916c25d263dc13639f4c1352ac7c0000000000000000050176d3fe59a98400047863f168f2a401e8d0a41d72e8c418626bb4110dd5341c02f0e4270d9b03ef8c9fd4116fbb9411f8f6542391bfa41a0872f42815d56415e5f06420deec841b2d5b141de5f0841ee252342db21154160bd43405af34f40d5688e42624ea741f1799641242473400a34294238e8114241ee0f41383f184052f118c1724989c18c3c9ec0cf0decc138a006c29d4f65c0ef399fc1ea1696c17ba0f7405e30a741a026964231230042f235c641d6eee641aa7a5a410000000000000000b421d241467c8142ae6de741f7a0ee40a00da9423cb40742d6f24240461c864112558741c9ae1542089484423d261242e79d0a427392c240668cd341d554b241dd0ced40e72188c1091983c1e40b55c1f7b6cdc1304713c2360f12c0b8ca24c06a8319c232e36dc2a96dffc185040ac00e1ae8c1449c95c2c20370c29c0736bf6cce33c12c2200c2d0235cc177a125c2aa6f4fc11aab49c1bcb428c274a900c14babb542f2118d42489f6a42e4de5442c2153142be3202428ef2be4137584743b41ac3428d7dc042f9e4a7422c8fac425b61b04217c69a42d69e9b4255ec974210fa8c4298b687428a7a714282ef5f4292923942805242423c032d4222a90e421665d841a0dbda4154d9aa4255269e425ac99842d51a9a42a8bf8b4244637e42ea414542eac56a4280184042bb6d3542a4070042bf650242a7c111425a620642466841414be5b34248d59042e58c95422ef8814264842c423bef2542bc3f3742e63ac141fb61aac16444c7c1b455523ff40b0ec259efe8c055ec9cc166182cc00000fab800007ab97fc70fc15aec44c1c0eaa4bf4e5fe84072b9f9c0cf0a0041e0859641ac1d5241bb43b641d2a95840ce0bdb41420ca541583e2842c50aba416d47f641188f51410313b5416eec9f41b120c041284ba040a6b2e3417c0ffbbf711224407cdd2f40d2a2364219c555c0daaef1407be03240a8b5b4412e221e402cc6bcbe3067883f51cbc5c1e74603c2d25b09c188a03bc2be01abc1b07bb0c029248cc131a90ac1320d4a41a82c6e416a983f42cd15b741b8ef8941c00e88415aeaee400080ed390010d63a78ed0242dcc74f427ad0de41c023394128677642a7aecb4154458440d4f8504140563b41a9c3e64150812542f354c6414e45ba41bab6c240b6a49241c3a15c412c6e08410c168ec108f28cc1707549c18795ecc1a2b80cc2b861c2bf40480bc035b8d1c13b7a27c2875cb7c18a91acbfc9cd7ac13b382fc27eed03c2003cbe3abf62ecc03433dec17f0a69c1b58ae7c1fc0df5c09cbf17c143b7d6c124d68ac031000000";

  uint8_t *chrome_params = dt_exif_xmp_decode(
      chrome_params_input, strlen(chrome_params_input), &params_len);

  assert(params_len == sizeof(dt_iop_colorchecker_params_t));
  assert(chrome_params);
  dt_gui_presets_add_generic(_("Fuji Classic Chrome emulation"), self->op,
                             self->version(), chrome_params, params_len, 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  free(chrome_params);


  const char *mchrome_params_input =
  "287bc242632bb84226d3b54263b1a142befa904280da8942e09a88426c9d67425e6254420abc3042000048438be5aa4213ca99420d748842548c7c42d00a5942a46147422410444227060042b8bfff41348ec742c672b04293a7aa425e7f9d425e779b421a2c9a422b1f9a42fd0b87420a1e7b426e0772429e404a422a3e4a4220fc47423e8d414290c1e8412c6ddd412422cf41cce0b7419cc96441050bc4427c9fc142cebba142dbe0a04224bba04239449f4206e96e42bcec42428292e341b63ed641ca5f2dc02cfe09bfeab32cc0ca08ccc1a49ebbc1640dfcc09c6465bf7de528c2828667c19a8516c2000000002024e040c553d1419ee5594166cd9d4102e2164294636342ae0a19427699cb41a4e0de3e24a60a3fca0aa24112b99040fe569340f8adb441dc810d42aa00f740e048cc3f226070428bc677410000fa3f1053a840e46ed341aea6494144836441a2fd2f42a702824152a14142a2ea103f00e426c1c897d0c1f462f6c1fbfea9c1cb29f1c1175d1ac1efcfb9c1175407c281b891c19ced14c161f0d04192d26b42863e9a41fd251042c58c5041189b884282c51641d981fa416aa89d413b0e1e4100000000ca02b040c8fafa3ffde2b541a4fc0641c47e2e429fb2da404125b14124141a3f7c06a53fc0aae9be3817c0c16f24a8c09a8cabc1e0f6fac154eb25c2927530c2389b4fc1e97a4cc210946ec23e2934c148e702c2400ce8c1257492c2c1fe84c15e791ac2868f90c2599db5c2f66fe9c082aa61c09e38abc0585464bfcec916c2f6cfb8c16b022bc14d3275c26955a0c11a2946c146d9fac1ccf5be428046ac4247acbe4208b697427529894244c87f421ac5874230733d42722546425c5c07426aca474358f8b9421ea1a6427ee58d42e7208842d2416a426a656742fa625742012c0f4280bafb414f0ec542b457bf42a8eab14292dd9c421c95a242e5e4a54279da9942574c8842ff55914222fd7a420e9c4b42f8c44842c2da59421ae935421a45fa4126010c42ecdbd1418a2bd94140c36041ec10bf424b81a9425cfd8f421fa88b42abfb8742d9a9994298f23242ad2f12422a33bd41c8dabb41008ae3bc00b209bc8045e4bc00e87dbb0028a0ba00606aba0028a0ba0000fab700007ab900b0b3390000fa3880fdefbc00d2d7bb00c406bb00f8a7ba007014ba00b033ba0020cbb900a08c390010a43900349ebb8051e2bc003248bc0044c5bb00f6d1bb00ccd8bb00007abb0010a4ba00d004bb003072ba00803bb900007ab90060eab90000fa3700b0b3390010a4390060ea390060ea3900e8003a0007e4bc0008cfbb00a00cbb00940ebb0010a4ba00f47bbb0000fa3700803b390030f2390000fa3920a14f3e8081733de017503e0041eb3c00ec103c0060d13b0012133c0000c8b80020b23a008419bb00001639404f593e00e6433d0094723c0044133c00ec903b000c943b0068583b00040dbb005421bb001d713de0eb4e3ec097b63d00442c3d807d313d005d453d007ee53c004a123c00ca693c00d8d63b0070ad3a0070ad3a00b8533b008009b9001c22bb00e012bb00d04fbb003847bb00b86cbbc0334f3e802f3e3d004e6d3c0038793c0012133c005fe63c008009b90088dbba007c5dbb00705fbb31000000";

  uint8_t *mchrome_params = dt_exif_xmp_decode(
      mchrome_params_input, strlen(mchrome_params_input), &params_len);

  assert(params_len == sizeof(dt_iop_colorchecker_params_t));
  assert(mchrome_params);
  dt_gui_presets_add_generic(_("Fuji Monochrome emulation"), self->op,
                             self->version(), mchrome_params, params_len, 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  free(mchrome_params);


  const char *provia_params_input =
  "aa1fae42b13a98429c8997420bbc8f4264bb81424e3f76423a034642de774542b8522142000048430000c8422467bc42f123b2422c209e4282049842fc5b9342567d8b423c50704286f657424e153842deec2f4239fc0d428857de41de0aca414552bd4233bdb342973099428ddb95420af59442f7df9442f0a89442a73d874206ff75428c79704248b5484214c93e42aaee344234af074246a0d04156a284412c803b41f8d7ba4248029d42ddd3964200e884421e123142485c2c42c80e2c42ce24c441ff528ec1f8f123c14b9869c05c0bfdc18c4191bf6dc517c25d1ad6c1f2cd3ec176a711c200000000000000003242bd3fce19a2407cc67a41c7b6784152e27a41982e1142ecbd9f4142e53142f0da7d423b50ff41e574314270501140f6fad04154c232414eef50402f2ce040164c1c4184deb64190aa8f4048930a42bd5d46409d2f6642a6bd4841704e5c40e18dd441b6b79a42ca88dc41ee6e5542333e7d413cc16d3e39061ec16f90cec1c6736ac1143cefc14e0ad8c180ce9dc181d75dc0f5da2dc1b2ce4141fd67a4414d0d26427e43c6419a48664289f20042a8713f42c7dbc441c3dd52410000000000000000a1cd1242fab58242300db2427767e94004a1cd41aa56844166861442a95c5542b9287a41c117b340f682cb414e54c440fdeb76411c4c0bc1469f58c0cce3f0c1537f02c1c7768ac13a0a9ec1d151cdc1a43e47c0946b09c2e9b036c2b8de42c0a5de98c15c0722c2934588c22a7911c2ef9cddc1377a1ec072313dc18f46f2c125f1f7c0acb628c2367522c1fe682bc2c68d55c1af28ccc1ff7ab44211c69742e6f08d42e2918942b03c7842061e6c4265603b42dd9f3942ae882142cc0e48430e6dc842e4f5c2429960b942005490427ab3994210e68c4225cc86427ea6664270774a42fcf6394250a931427a111642226bce41de78d441963fc3425c07b44204ad9b42b72d9d42f9cb9f42d1f59c42bd9c9c4221488742c23a854240d87f4264c648426cb54a4264ce5642f4d92d429ef80d42accba741007f3b4154cabc42993ba44260959b422b7396421c5a3742f48a4a42397a2c429c51e14190161fc222ff73c16fe39dc0cbbd33c2e00058bffabb4bc283daf8c181095ac138a6f4c10000fa3800007a386881b1c15b5c03c24454f83f04aaa64170cd9141ca3cd641a618bc415d2c2042e1bf5542fd60054232552a42b6da20408ab1c14178bfa140f258b440c0e3ba3d66036e414efafa41aa6a3340158303424c05fe3fcbf3344231607a40a2e66440a045da4109637d425dbb6741f4002542b7c23141b018ff3d9b08fac10b2f6cc231a3c3c11e1a72c21ceed2c1b33887c1346393c0d2a38ac0c4c7b9416c71c34101e52d4208cce641b8fd5842397b14429dda1b42e4a2c841aab68d41000048b8000016b9a12f504214e69c422a9e8d42e6791241c41ed941b39a4a417a52144297102642dc4e2b41a152ca40086ac441748eb3404a6369413aac87c09cef18c1bb1805c2be0f4bc1a7bce6c1bc6701c26233f4c1b6b040c0909a26c2c2e040c290ca65c0aaa4b2c1bce85ac2df088fc2423808c2f7d5b5c1255fbcbfd0ad1cc1eef8eac10e2832c18df519c2df67f4c0accb37c26cf164c1f460a3c131000000";

  uint8_t *provia_params = dt_exif_xmp_decode(
      provia_params_input, strlen(provia_params_input), &params_len);

  assert(params_len == sizeof(dt_iop_colorchecker_params_t));
  assert(provia_params);
  dt_gui_presets_add_generic(_("Fuji Provia emulation"), self->op,
                             self->version(), provia_params, params_len, 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  free(provia_params);


  const char *velvia_params_input =
  "3f259c42b92693425c7b83420e107d42f86e4f4252a94b4293c32042db870442269da341000048430000c8427ee97f42ceca7342e81e6b42c9eb3e425514254248600f42c0fc0242ea69e941022bcd414624994222cb8d42f57d8842d77587428cea6e421c546c42b2a668429eda5e42da4a5e42242f2f42f37a1542c0fd0d42d0e30842867bab414eeca34154c46941482b5f41d08646415e552c41c512a5423390964242c7914260c07e42ea6176429c79744286010e4273310b42d6a28541fa0a4a41ca2161c0af9206c045d4f4c07ec5c3c1633ccec0d57efac17e2981c1f8449ec112a734c00000000000000000ad5fd440cb8a9441e0fab740a649a941f85d6b41387b2541888d2e42853cc241c33ad0406843c4408eb22d41c016713d7fd79541da99953f7d70c241ba600142f0d0273fd25e0541ceda4e42456b944138a29d41f76448424a941c41d0cc1642a54ba0412c030c428342874106e0e54032bfbdbfab3a48c13fe059c1d141a0c1e655c1c1ac9c49c190d038c1e3c242c094c185c0217c5ac075074e410485174251beb941c0c422412bf53c4282ada0410571a64130a5d93f584cab3e000000000000000004d88f4229c6ba4053185a41e8d51f4268579f41302c503f87e59a410806fe4085f0cf40e67992c190b1ccc0e75c45c19ee3d1c16677a1c11b6e81c1461c06c26c192cc1ef3128c2378125c29272b0c142de69c2154e7bc120564cc2d4a807c2aa6f15c12e2e82c20fa010c200327cc1fe8a4dc1502e4cc0a6debec11a4609c230e38cc112a5c5c042f01dc2b4aa7ec1fd3986c15abf8dc0282aa242f202994250707d429aed7b42604a51424c8b4e42efac1f4276070e426420a441d3d84443567fae4219ce83425a567b4214286242a8554642f1421e42c3f10d427cab1c426af6f5416221ce416de0a14206bf9242de7e8842d21d9142668d7d42465c7e42acb57c428ada5e42f4516242eaf9514232971f42c7522042028e2b42747af9410c8aef4158809141603adb4150e2a7411e1815413287a7429d2d9a420bea9c429a418d428ea5864280877f42687f3142e5cb0f42d85b9f4160000d41c30fbec0b4246fc03f0f46c19b1c1ac2f36b08c1f2513cc2b239b4c196fda7c1123632c000409cb90010a4ba349c76416a78ea410249f3404dfd00427f41974148854d4140604c42c70edc413bf6064131cc684008178941bcb2653fa9edaf4160fe4d40b8121a4222fd2a420238c03fd436d8405e0577429e85bb41f7b899419b5469426c50c541f7e217425da58e41c99c1442ef1690417ac27e416b5e56c0a5d1a5c12405f6c12c5e1bc26ab106c2c5a59ec142693dc0f43a11c082d65140698887c0efab9c41c5de6842b0e8054221f29041eeab36420440f241673fc6410201b4404822063f00e0123b001a1a3ce60f8242e6631e41ef649b41813329425bfeb741fea0973ff9f8d0419a453f41362007412eee15c128293fc18667b0c12eb0acc14bb20fc213a7ebc1281c0dc29cd587c1f61739c2f7974cc2ac6c08c2003c8fc2389bb6c119b5a2c214a74ec266f4ecc05264b6c2107819c2f476a9c17398a8c05af39dc02d6e5cc16d31cec11095f4c1fe9e20c1bfbd76c2d3adc1c12fea7fc196bf11c131000000";

  uint8_t *velvia_params = dt_exif_xmp_decode(
      velvia_params_input, strlen(velvia_params_input), &params_len);

  assert(params_len == sizeof(dt_iop_colorchecker_params_t));
  assert(velvia_params);
  dt_gui_presets_add_generic(_("Fuji Velvia emulation"), self->op,
                             self->version(), velvia_params, params_len, 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  free(velvia_params);
}

// thinplate spline kernel \phi(r) = 2 r^2 ln(r)
#if defined(_OPENMP)
#pragma omp declare simd aligned(x, y)
#endif
static inline float kernel(const dt_aligned_pixel_t x, const dt_aligned_pixel_t y)
{
  dt_aligned_pixel_t diff2;
  for_each_channel(c)
  {
    diff2[c] = (x[c] - y[c]);
    diff2[c] *= diff2[c];
  }
  const float r2 = diff2[0] + diff2[1] + diff2[2];
  return r2*fastlog(MAX(1e-8f,r2));
}

void process(struct dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                        ivoid, ovoid, roi_in, roi_out))
    return;

  const dt_iop_colorchecker_data_t *const data = (dt_iop_colorchecker_data_t *)piece->data;
  const size_t npixels = (size_t)roi_out->height * (size_t)roi_out->width;
  float *const restrict out = (float*)DT_IS_ALIGNED(ovoid);

  // convert patch data from struct of arrays to array of structs so we can vectorize operations
  const int num_patches = data->num_patches;
  dt_aligned_pixel_t *sources = dt_alloc_align(64, sizeof(dt_aligned_pixel_t) * num_patches);
  for(int i = 0; i < num_patches; i++)
  {
    sources[i][0] = data->source_Lab[3 * i];
    sources[i][1] = data->source_Lab[3 * i + 1];
    sources[i][2] = data->source_Lab[3 * i + 2];
    sources[i][3] = 0.0f;
  }
  dt_aligned_pixel_t *patches = dt_alloc_align(64, sizeof(dt_aligned_pixel_t) * (num_patches + 1));
  for(int i = 0; i <= num_patches; i++)
  {
    patches[i][0] = data->coeff_L[i];
    patches[i][1] = data->coeff_a[i];
    patches[i][2] = data->coeff_b[i];
    patches[i][3] = 0.0f;
  }
  const dt_aligned_pixel_t polynomial_L =
    { data->coeff_L[num_patches+1], data->coeff_L[num_patches+2], data->coeff_L[num_patches+3], 0.0f };
  const dt_aligned_pixel_t polynomial_a =
    { data->coeff_a[num_patches+1], data->coeff_a[num_patches+2], data->coeff_a[num_patches+3], 0.0f };
  const dt_aligned_pixel_t polynomial_b =
    { data->coeff_b[num_patches+1], data->coeff_b[num_patches+2], data->coeff_b[num_patches+3], 0.0f };

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels, num_patches, patches, sources, polynomial_L, \
                      polynomial_a, polynomial_b, ivoid, out)         \
  schedule(static)
#endif
  for(int k=0; k < npixels; k++)
  {
    dt_aligned_pixel_t inpx;
    copy_pixel(inpx, ((float *)ivoid) + 4*k);

    // polynomial part:
    dt_aligned_pixel_t poly_L, poly_a, poly_b;
    for_each_channel(c)
    {
      poly_L[c] = (polynomial_L[c] * inpx[c]);
      poly_a[c] = (polynomial_a[c] * inpx[c]);
      poly_b[c] = (polynomial_b[c] * inpx[c]);
    }
    dt_aligned_pixel_t sums = { poly_L[0] + poly_L[1] + poly_L[2],
      				poly_a[0] + poly_a[1] + poly_a[2],
                                poly_b[0] + poly_b[1] + poly_b[2],
                                0.0f };
    dt_aligned_pixel_t res;
    for_each_channel(c)
      res[c] = patches[num_patches][c] + sums[c];
    for(int p=0; p < num_patches; p++)
    {
      // rbf from thin plate spline
      const float phi = kernel(inpx, sources[p]);
      for_each_channel(c)
        res[c] += patches[p][c] * phi;
    }
    copy_pixel_nontemporal(out + 4*k, res);
  }
  dt_omploop_sfence();
  dt_free_align(patches);
  dt_free_align(sources);
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorchecker_data_t *d = (dt_iop_colorchecker_data_t *)piece->data;
  dt_iop_colorchecker_global_data_t *gd = (dt_iop_colorchecker_global_data_t *)self->global_data;

  const int devid = piece->pipe->devid;
  const int width = roi_out->width;
  const int height = roi_out->height;
  const int num_patches = d->num_patches;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem dev_params = NULL;

  const size_t params_size = (size_t)(4 * (2 * num_patches + 4)) * sizeof(float);
  float *params = malloc(params_size);
  float *idx = params;

  // re-arrange data->source_Lab and data->coeff_{L,a,b} into float4
  for(int n = 0; n < num_patches; n++, idx += 4)
  {
    idx[0] = d->source_Lab[3 * n];
    idx[1] = d->source_Lab[3 * n + 1];
    idx[2] = d->source_Lab[3 * n + 2];
    idx[3] = 0.0f;
  }

  for(int n = 0; n < num_patches + 4; n++, idx += 4)
  {
    idx[0] = d->coeff_L[n];
    idx[1] = d->coeff_a[n];
    idx[2] = d->coeff_b[n];
    idx[3] = 0.0f;
  }

  dev_params = dt_opencl_copy_host_to_device_constant(devid, params_size, params);
  if(dev_params == NULL) goto error;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_colorchecker, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(num_patches), CLARG(dev_params));
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_params);
  free(params);
  return TRUE;

error:
  free(params);
  dt_opencl_release_mem_object(dev_params);
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorchecker] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif


void commit_params(struct dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)p1;
  dt_iop_colorchecker_data_t *d = (dt_iop_colorchecker_data_t *)piece->data;

  d->num_patches = MIN(MAX_PATCHES, p->num_patches);
  const int N = d->num_patches, N4 = N + 4;
  for(int k = 0; k < N; k++)
  {
    d->source_Lab[3*k+0] = p->source_L[k];
    d->source_Lab[3*k+1] = p->source_a[k];
    d->source_Lab[3*k+2] = p->source_b[k];
  }

  // initialize coefficients with default values that will be
  // used for N<=4 and if coefficient matrix A is singular
  for(int i=0;i<4+N;i++)
  {
    d->coeff_L[i] = 0;
    d->coeff_a[i] = 0;
    d->coeff_b[i] = 0;
  }
  d->coeff_L[N + 1] = 1;
  d->coeff_a[N + 2] = 1;
  d->coeff_b[N + 3] = 1;

  /*
      Following

      K. Anjyo, J. P. Lewis, and F. Pighin, "Scattered data
      interpolation for computer graphics," ACM SIGGRAPH 2014 Courses
      on - SIGGRAPH ’14, 2014.
      http://dx.doi.org/10.1145/2614028.2615425
      http://scribblethink.org/Courses/ScatteredInterpolation/scatteredinterpcoursenotes.pdf

      construct the system matrix and the vector of function values and
      solve the set of linear equations

      / R   P \  / c \   / f \
      |       |  |   | = |   |
      \ P^t 0 /  \ d /   \ 0 /

      for the coefficient vector (c d)^t.

      By design of the interpolation scheme the interpolation
      coefficients c for radial non-linear basis functions (the kernel)
      must always vanish for N<=4.  For N<4 the (N+4)x(N+4) coefficient
      matrix A is singular, the linear system has non-unique solutions.
      Thus the cases with N<=4 need special treatment, unique solutions
      are found by setting some of the unknown coefficients to zero and
      solving a smaller linear system.
  */
  switch(N)
  {
  case 0:
    break;
  case 1:
    // interpolation via constant function
    d->coeff_L[N + 1] = p->target_L[0] / p->source_L[0];
    d->coeff_a[N + 2] = p->target_a[0] / p->source_a[0];
    d->coeff_b[N + 3] = p->target_b[0] / p->source_b[0];
    break;
  case 2:
    // interpolation via single constant function and the linear
    // function of the corresponding color channel
    {
      double A[2 * 2] = { 1, p->source_L[0],
                          1, p->source_L[1] };
      double b[2] = { p->target_L[0], p->target_L[1] };
      if(!gauss_solve(A, b, 2)) break;
      d->coeff_L[N + 0] = b[0];
      d->coeff_L[N + 1] = b[1];
    }
    {
      double A[2 * 2] = { 1, p->source_a[0],
                          1, p->source_a[1] };
      double b[2] = { p->target_a[0], p->target_a[1] };
      if(!gauss_solve(A, b, 2)) break;
      d->coeff_a[N + 0] = b[0];
      d->coeff_a[N + 2] = b[1];
    }
    {
      double A[2 * 2] = { 1, p->source_b[0],
                          1, p->source_b[1] };
      double b[2] = { p->target_b[0], p->target_b[1] };
      if(!gauss_solve(A, b, 2)) break;
      d->coeff_b[N + 0] = b[0];
      d->coeff_b[N + 3] = b[1];
    }
    break;
  case 3:
    // interpolation via single constant function, the linear function
    // of the corresponding color channel and the linear functions
    // of the other two color channels having both the same weight
    {
      double A[3 * 3] = { 1, p->source_L[0], p->source_a[0] + p->source_b[0],
                          1, p->source_L[1], p->source_a[1] + p->source_b[1],
                          1, p->source_L[2], p->source_a[2] + p->source_b[2] };
      double b[3] = { p->target_L[0], p->target_L[1], p->target_L[2] };
      if(!gauss_solve(A, b, 3)) break;
      d->coeff_L[N + 0] = b[0];
      d->coeff_L[N + 1] = b[1];
      d->coeff_L[N + 2] = b[2];
      d->coeff_L[N + 3] = b[2];
    }
    {
      double A[3 * 3] = { 1, p->source_a[0], p->source_L[0] + p->source_b[0],
                          1, p->source_a[1], p->source_L[1] + p->source_b[1],
                          1, p->source_a[2], p->source_L[2] + p->source_b[2] };
      double b[3] = { p->target_a[0], p->target_a[1], p->target_a[2] };
      if(!gauss_solve(A, b, 3)) break;
      d->coeff_a[N + 0] = b[0];
      d->coeff_a[N + 1] = b[2];
      d->coeff_a[N + 2] = b[1];
      d->coeff_a[N + 3] = b[2];
    }
    {
      double A[3 * 3] = { 1, p->source_b[0], p->source_L[0] + p->source_a[0],
                          1, p->source_b[1], p->source_L[1] + p->source_a[1],
                          1, p->source_b[2], p->source_L[2] + p->source_a[2] };
      double b[3] = { p->target_b[0], p->target_b[1], p->target_b[2] };
      if(!gauss_solve(A, b, 3)) break;
      d->coeff_b[N + 0] = b[0];
      d->coeff_b[N + 1] = b[2];
      d->coeff_b[N + 2] = b[2];
      d->coeff_b[N + 3] = b[1];
    }
    break;
  case 4:
  {
    // interpolation via constant function and 3 linear functions
    double A[4 * 4] = { 1, p->source_L[0], p->source_a[0], p->source_b[0],
                        1, p->source_L[1], p->source_a[1], p->source_b[1],
                        1, p->source_L[2], p->source_a[2], p->source_b[2],
                        1, p->source_L[3], p->source_a[3], p->source_b[3] };
    int pivot[4];
    if(!gauss_make_triangular(A, pivot, 4)) break;
    {
      double b[4] = { p->target_L[0], p->target_L[1], p->target_L[2], p->target_L[3] };
      gauss_solve_triangular(A, pivot, b, 4);
      d->coeff_L[N + 0] = b[0];
      d->coeff_L[N + 1] = b[1];
      d->coeff_L[N + 2] = b[2];
      d->coeff_L[N + 3] = b[3];
    }
    {
      double b[4] = { p->target_a[0], p->target_a[1], p->target_a[2], p->target_a[3] };
      gauss_solve_triangular(A, pivot, b, 4);
      d->coeff_a[N + 0] = b[0];
      d->coeff_a[N + 1] = b[1];
      d->coeff_a[N + 2] = b[2];
      d->coeff_a[N + 3] = b[3];
    }
    {
      double b[4] = { p->target_b[0], p->target_b[1], p->target_b[2], p->target_b[3] };
      gauss_solve_triangular(A, pivot, b, 4);
      d->coeff_b[N + 0] = b[0];
      d->coeff_b[N + 1] = b[1];
      d->coeff_b[N + 2] = b[2];
      d->coeff_b[N + 3] = b[3];
    }
    break;
  }
  default:
  {
    // setup linear system of equations
    double *A = malloc(sizeof(*A) * N4 * N4);
    double *b = malloc(sizeof(*b) * N4);
    // coefficients from nonlinear radial kernel functions
    for(int j=0;j<N;j++)
      for(int i=j;i<N;i++)
        A[j*N4+i] = A[i*N4+j] = kernel(d->source_Lab+3*i, d->source_Lab+3*j);
    // coefficients from constant and linear functions
    for(int i=0;i<N;i++) A[i*N4+N+0] = A[(N+0)*N4+i] = 1;
    for(int i=0;i<N;i++) A[i*N4+N+1] = A[(N+1)*N4+i] = d->source_Lab[3*i+0];
    for(int i=0;i<N;i++) A[i*N4+N+2] = A[(N+2)*N4+i] = d->source_Lab[3*i+1];
    for(int i=0;i<N;i++) A[i*N4+N+3] = A[(N+3)*N4+i] = d->source_Lab[3*i+2];
    // lower-right zero block
    for(int j=N;j<N4;j++)
      for(int i=N;i<N4;i++)
        A[j*N4+i] = 0;
    // make coefficient matrix triangular
    int *pivot = malloc(sizeof(*pivot) * N4);
    if(gauss_make_triangular(A, pivot, N4))
    {
      // calculate coefficients for L channel
      for(int i=0;i<N;i++) b[i] = p->target_L[i];
      for(int i=N;i<N4;i++) b[i] = 0;
      gauss_solve_triangular(A, pivot, b, N4);
      for(int i=0;i<N+4;i++) d->coeff_L[i] = b[i];
      // calculate coefficients for a channel
      for(int i=0;i<N;i++) b[i] = p->target_a[i];
      for(int i=N;i<N4;i++) b[i] = 0;
      gauss_solve_triangular(A, pivot, b, N4);
      for(int i=0;i<N+4;i++) d->coeff_a[i] = b[i];
      // calculate coefficients for b channel
      for(int i=0;i<N;i++) b[i] = p->target_b[i];
      for(int i=N;i<N4;i++) b[i] = 0;
      gauss_solve_triangular(A, pivot, b, N4);
      for(int i=0;i<N+4;i++) d->coeff_b[i] = b[i];
    }
    // free resources
    free(pivot);
    free(b);
    free(A);
  }
  }
}

void init_pipe(struct dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_colorchecker_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_reset(struct dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
}

void _colorchecker_rebuild_patch_list(struct dt_iop_module_t *self)
{
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  if(g->patch >= p->num_patches || g->patch < 0) return;

  if(dt_bauhaus_combobox_length(g->combobox_patch) != p->num_patches)
  {
    dt_bauhaus_combobox_clear(g->combobox_patch);
    char cboxentry[1024];
    for(int k=0;k<p->num_patches;k++)
    {
      snprintf(cboxentry, sizeof(cboxentry), _("patch #%d"), k);
      dt_bauhaus_combobox_add(g->combobox_patch, cboxentry);
    }
    if(p->num_patches <= 24)
      dtgtk_drawing_area_set_aspect_ratio(g->area, 2.0/3.0);
    else
      dtgtk_drawing_area_set_aspect_ratio(g->area, 1.0);
    // FIXME: why not just use g->patch for everything?
    g->drawn_patch = dt_bauhaus_combobox_get(g->combobox_patch);
  }
}

void _colorchecker_update_sliders(struct dt_iop_module_t *self)
{
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  if(g->patch >= p->num_patches || g->patch < 0) return;

  if(g->absolute_target)
  {
    dt_bauhaus_slider_set(g->scale_L, p->target_L[g->patch]);
    dt_bauhaus_slider_set(g->scale_a, p->target_a[g->patch]);
    dt_bauhaus_slider_set(g->scale_b, p->target_b[g->patch]);
    const float Cout = sqrtf(
        p->target_a[g->patch]*p->target_a[g->patch]+
        p->target_b[g->patch]*p->target_b[g->patch]);
    dt_bauhaus_slider_set(g->scale_C, Cout);
  }
  else
  {
    dt_bauhaus_slider_set(g->scale_L, p->target_L[g->patch] - p->source_L[g->patch]);
    dt_bauhaus_slider_set(g->scale_a, p->target_a[g->patch] - p->source_a[g->patch]);
    dt_bauhaus_slider_set(g->scale_b, p->target_b[g->patch] - p->source_b[g->patch]);
    const float Cin = sqrtf(
        p->source_a[g->patch]*p->source_a[g->patch] +
        p->source_b[g->patch]*p->source_b[g->patch]);
    const float Cout = sqrtf(
        p->target_a[g->patch]*p->target_a[g->patch]+
        p->target_b[g->patch]*p->target_b[g->patch]);
    dt_bauhaus_slider_set(g->scale_C, Cout-Cin);
  }
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;

  _colorchecker_rebuild_patch_list(self);
  _colorchecker_update_sliders(self);

  gtk_widget_queue_draw(g->area);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_colorchecker_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_colorchecker_params_t));
  module->default_enabled = FALSE;
  module->params_size = sizeof(dt_iop_colorchecker_params_t);
  module->gui_data = NULL;

  dt_iop_colorchecker_params_t *d = module->default_params;
  d->num_patches = colorchecker_patches;
  for(int k = 0; k < d->num_patches; k++)
  {
    d->source_L[k] = d->target_L[k] = colorchecker_Lab[3*k+0];
    d->source_a[k] = d->target_a[k] = colorchecker_Lab[3*k+1];
    d->source_b[k] = d->target_b[k] = colorchecker_Lab[3*k+2];
  }
}

void init_global(dt_iop_module_so_t *module)
{
  dt_iop_colorchecker_global_data_t *gd
      = (dt_iop_colorchecker_global_data_t *)malloc(sizeof(dt_iop_colorchecker_global_data_t));
  module->data = gd;

  const int program = 8; // extended.cl, from programs.conf
  gd->kernel_colorchecker = dt_opencl_create_kernel(program, "colorchecker");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorchecker_global_data_t *gd = (dt_iop_colorchecker_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorchecker);
  free(module->data);
  module->data = NULL;
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  if(p->num_patches <= 0) return;

  // determine patch based on color picker result
  const dt_aligned_pixel_t picked_mean = { self->picked_color[0], self->picked_color[1], self->picked_color[2] };
  int best_patch = 0;
  for(int patch = 1; patch < p->num_patches; patch++)
  {
    const dt_aligned_pixel_t Lab = { p->source_L[patch], p->source_a[patch], p->source_b[patch] };
    if((self->request_color_pick == DT_REQUEST_COLORPICK_MODULE)
       && (sqf(picked_mean[0] - Lab[0])
               + sqf(picked_mean[1] - Lab[1])
               + sqf(picked_mean[2] - Lab[2])
           < sqf(picked_mean[0] - p->source_L[best_patch])
                 + sqf(picked_mean[1] - p->source_a[best_patch])
                 + sqf(picked_mean[2] - p->source_b[best_patch])))
      best_patch = patch;
  }

  if(best_patch != g->drawn_patch)
  {
    g->patch = g->drawn_patch = best_patch;
    ++darktable.gui->reset;
    dt_bauhaus_combobox_set(g->combobox_patch, g->drawn_patch);
    _colorchecker_update_sliders(self);
    --darktable.gui->reset;
    gtk_widget_queue_draw(g->area);
  }
}

static void target_L_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  if(g->patch >= p->num_patches || g->patch < 0) return;
  if(g->absolute_target)
    p->target_L[g->patch] = dt_bauhaus_slider_get(slider);
  else
    p->target_L[g->patch] = p->source_L[g->patch] + dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void target_a_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  if(g->patch >= p->num_patches || g->patch < 0) return;
  if(g->absolute_target)
  {
    p->target_a[g->patch] = CLAMP(dt_bauhaus_slider_get(slider), -128.0, 128.0);
    const float Cout = sqrtf(
        p->target_a[g->patch]*p->target_a[g->patch]+
        p->target_b[g->patch]*p->target_b[g->patch]);
    ++darktable.gui->reset; // avoid history item
    dt_bauhaus_slider_set(g->scale_C, Cout);
    --darktable.gui->reset;
  }
  else
  {
    p->target_a[g->patch] = CLAMP(p->source_a[g->patch] + dt_bauhaus_slider_get(slider), -128.0, 128.0);
    const float Cin = sqrtf(
        p->source_a[g->patch]*p->source_a[g->patch] +
        p->source_b[g->patch]*p->source_b[g->patch]);
    const float Cout = sqrtf(
        p->target_a[g->patch]*p->target_a[g->patch]+
        p->target_b[g->patch]*p->target_b[g->patch]);
    ++darktable.gui->reset; // avoid history item
    dt_bauhaus_slider_set(g->scale_C, Cout-Cin);
    --darktable.gui->reset;
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void target_b_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  if(g->patch >= p->num_patches || g->patch < 0) return;
  if(g->absolute_target)
  {
    p->target_b[g->patch] = CLAMP(dt_bauhaus_slider_get(slider), -128.0, 128.0);
    const float Cout = sqrtf(
        p->target_a[g->patch]*p->target_a[g->patch]+
        p->target_b[g->patch]*p->target_b[g->patch]);
    ++darktable.gui->reset; // avoid history item
    dt_bauhaus_slider_set(g->scale_C, Cout);
    --darktable.gui->reset;
  }
  else
  {
    p->target_b[g->patch] = CLAMP(p->source_b[g->patch] + dt_bauhaus_slider_get(slider), -128.0, 128.0);
    const float Cin = sqrtf(
        p->source_a[g->patch]*p->source_a[g->patch] +
        p->source_b[g->patch]*p->source_b[g->patch]);
    const float Cout = sqrtf(
        p->target_a[g->patch]*p->target_a[g->patch]+
        p->target_b[g->patch]*p->target_b[g->patch]);
    ++darktable.gui->reset; // avoid history item
    dt_bauhaus_slider_set(g->scale_C, Cout-Cin);
    --darktable.gui->reset;
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void target_C_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  if(g->patch >= p->num_patches || g->patch < 0) return;
  const float Cin = sqrtf(
      p->source_a[g->patch]*p->source_a[g->patch] +
      p->source_b[g->patch]*p->source_b[g->patch]);
  const float Cout = MAX(1e-4f, sqrtf(
      p->target_a[g->patch]*p->target_a[g->patch]+
      p->target_b[g->patch]*p->target_b[g->patch]));

  if(g->absolute_target)
  {
    const float Cnew = CLAMP(dt_bauhaus_slider_get(slider), 0.01, 128.0);
    p->target_a[g->patch] = CLAMP(p->target_a[g->patch]*Cnew/Cout, -128.0, 128.0);
    p->target_b[g->patch] = CLAMP(p->target_b[g->patch]*Cnew/Cout, -128.0, 128.0);
    ++darktable.gui->reset; // avoid history item
    dt_bauhaus_slider_set(g->scale_a, p->target_a[g->patch]);
    dt_bauhaus_slider_set(g->scale_b, p->target_b[g->patch]);
    --darktable.gui->reset;
  }
  else
  {
    const float Cnew = CLAMP(Cin + dt_bauhaus_slider_get(slider), 0.01, 128.0);
    p->target_a[g->patch] = CLAMP(p->target_a[g->patch]*Cnew/Cout, -128.0, 128.0);
    p->target_b[g->patch] = CLAMP(p->target_b[g->patch]*Cnew/Cout, -128.0, 128.0);
    ++darktable.gui->reset; // avoid history item
    dt_bauhaus_slider_set(g->scale_a, p->target_a[g->patch] - p->source_a[g->patch]);
    dt_bauhaus_slider_set(g->scale_b, p->target_b[g->patch] - p->source_b[g->patch]);
    --darktable.gui->reset;
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void target_callback(GtkWidget *combo, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  g->absolute_target = dt_bauhaus_combobox_get(combo);
  ++darktable.gui->reset;
  _colorchecker_update_sliders(self);
  --darktable.gui->reset;
  // switch off colour picker, it'll interfere with other changes of the patch:
  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(g->area);
}

static void patch_callback(GtkWidget *combo, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  g->drawn_patch = g->patch = dt_bauhaus_combobox_get(combo);
  ++darktable.gui->reset;
  _colorchecker_update_sliders(self);
  --darktable.gui->reset;
  // switch off colour picker, it'll interfere with other changes of the patch:
  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(g->area);
}

static gboolean checker_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int width = allocation.width;
  const int height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg
  cairo_set_source_rgb(cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  const int cells_x = p->num_patches > 24 ? 7 : 6;
  const int cells_y = p->num_patches > 24 ? 7 : 4;
  for(int j = 0; j < cells_y; j++)
  {
    for(int i = 0; i < cells_x; i++)
    {
      const int patch = i + j*cells_x;
      if(patch >= p->num_patches) continue;

      const dt_aligned_pixel_t Lab = { p->source_L[patch], p->source_a[patch], p->source_b[patch] };
      dt_aligned_pixel_t rgb, XYZ;
      dt_Lab_to_XYZ(Lab, XYZ);
      dt_XYZ_to_sRGB(XYZ, rgb);
      cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);

      cairo_rectangle(cr, width * i / (float)cells_x, height * j / (float)cells_y,
          width / (float)cells_x - DT_PIXEL_APPLY_DPI(1),
          height / (float)cells_y - DT_PIXEL_APPLY_DPI(1));
      cairo_fill(cr);
      if(fabsf(p->target_L[patch] - p->source_L[patch]) > 1e-5f ||
         fabsf(p->target_a[patch] - p->source_a[patch]) > 1e-5f ||
         fabsf(p->target_b[patch] - p->source_b[patch]) > 1e-5f)
      {
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
        cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
        cairo_rectangle(cr,
            width * i / (float)cells_x + DT_PIXEL_APPLY_DPI(1),
            height * j / (float)cells_y + DT_PIXEL_APPLY_DPI(1),
            width / (float)cells_x - DT_PIXEL_APPLY_DPI(3),
            height / (float)cells_y - DT_PIXEL_APPLY_DPI(3));
        cairo_stroke(cr);
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
        cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
        cairo_rectangle(cr,
            width * i / (float)cells_x + DT_PIXEL_APPLY_DPI(2),
            height * j / (float)cells_y + DT_PIXEL_APPLY_DPI(2),
            width / (float)cells_x - DT_PIXEL_APPLY_DPI(5),
            height / (float)cells_y - DT_PIXEL_APPLY_DPI(5));
        cairo_stroke(cr);
      }
    }
  }

  if(g->drawn_patch != -1)
  {
    const int draw_i = g->drawn_patch % cells_x;
    const int draw_j = g->drawn_patch / cells_x;
    float color = 1.0;
    if(p->source_L[g->drawn_patch] > 80) color = 0.0;
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
    cairo_set_source_rgb(cr, color, color, color);
    cairo_rectangle(cr,
                    width * draw_i / (float) cells_x + DT_PIXEL_APPLY_DPI(5),
                    height * draw_j / (float) cells_y + DT_PIXEL_APPLY_DPI(5),
                    width / (float) cells_x - DT_PIXEL_APPLY_DPI(11),
                    height / (float) cells_y - DT_PIXEL_APPLY_DPI(11));
    cairo_stroke(cr);
  }

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean checker_motion_notify(
    GtkWidget *widget,
    GdkEventMotion *event,
    gpointer user_data)
{
  // highlight?
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  const float mouse_x = CLAMP(event->x, 0, width);
  const float mouse_y = CLAMP(event->y, 0, height);
  int cells_x = 6, cells_y = 4;
  if(p->num_patches > 24)
  {
    cells_x = 7;
    cells_y = 7;
  }
  const float mx = mouse_x * cells_x / (float)width;
  const float my = mouse_y * cells_y / (float)height;
  const int patch = (int)mx + cells_x * (int)my;
  if(patch < 0 || patch >= p->num_patches) return FALSE;
  char tooltip[1024];
  snprintf(tooltip, sizeof(tooltip),
      _("(%2.2f %2.2f %2.2f)\n"
        "altered patches are marked with an outline\n"
        "click to select\n"
        "double-click to reset\n"
        "right click to delete patch\n"
        "shift+click while color picking to replace patch"),
      p->source_L[patch], p->source_a[patch], p->source_b[patch]);
  gtk_widget_set_tooltip_text(g->area, tooltip);
  return TRUE;
}

static gboolean checker_button_press(
    GtkWidget *widget, GdkEventButton *event,
    gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  const float mouse_x = CLAMP(event->x, 0, width);
  const float mouse_y = CLAMP(event->y, 0, height);
  int cells_x = 6, cells_y = 4;
  if(p->num_patches > 24)
  {
    cells_x = 7;
    cells_y = 7;
  }
  const float mx = mouse_x * cells_x / (float)width;
  const float my = mouse_y * cells_y / (float)height;
  int patch = (int)mx + cells_x*(int)my;
  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  { // reset on double click
    if(patch < 0 || patch >= p->num_patches) return FALSE;
    p->target_L[patch] = p->source_L[patch];
    p->target_a[patch] = p->source_a[patch];
    p->target_b[patch] = p->source_b[patch];
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    ++darktable.gui->reset;
    _colorchecker_update_sliders(self);
    --darktable.gui->reset;
    gtk_widget_queue_draw(g->area);
    return TRUE;
  }
  else if(event->button == 3 && (patch < p->num_patches))
  {
    // right click: delete patch, move others up
    if(patch < 0 || patch >= p->num_patches) return FALSE;
    memmove(p->target_L+patch, p->target_L+patch+1, sizeof(float)*(p->num_patches-1-patch));
    memmove(p->target_a+patch, p->target_a+patch+1, sizeof(float)*(p->num_patches-1-patch));
    memmove(p->target_b+patch, p->target_b+patch+1, sizeof(float)*(p->num_patches-1-patch));
    memmove(p->source_L+patch, p->source_L+patch+1, sizeof(float)*(p->num_patches-1-patch));
    memmove(p->source_a+patch, p->source_a+patch+1, sizeof(float)*(p->num_patches-1-patch));
    memmove(p->source_b+patch, p->source_b+patch+1, sizeof(float)*(p->num_patches-1-patch));
    p->num_patches--;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    ++darktable.gui->reset;
    _colorchecker_rebuild_patch_list(self);
    _colorchecker_update_sliders(self);
    --darktable.gui->reset;
    gtk_widget_queue_draw(g->area);
    return TRUE;
  }
  else if((event->button == 1) &&
          dt_modifier_is(event->state, GDK_SHIFT_MASK) &&
          (self->request_color_pick == DT_REQUEST_COLORPICK_MODULE))
  {
    // shift-left while colour picking: replace source colour
    // if clicked outside the valid patches: add new one

    // color channels should be nonzero to avoid numerical issues
    int new_color_valid = fabsf(self->picked_color[0]) > 1.e-3f &&
                          fabsf(self->picked_color[1]) > 1.e-3f &&
                          fabsf(self->picked_color[2]) > 1.e-3f;
    // check if the new color is very close to some color already in the colorchecker
    for(int i=0;i<p->num_patches;++i)
    {
      float color[] = { p->source_L[i], p->source_a[i], p->source_b[i] };
      if(fabsf(self->picked_color[0] - color[0]) < 1.e-3f && fabsf(self->picked_color[1] - color[1]) < 1.e-3f
         && fabsf(self->picked_color[2] - color[2]) < 1.e-3f)
        new_color_valid = FALSE;
    }
    if(new_color_valid)
    {
      if(p->num_patches < MAX_PATCHES && (patch < 0 || patch >= p->num_patches))
      {
        p->num_patches = MIN(MAX_PATCHES, p->num_patches + 1);
        patch = p->num_patches - 1;
      }
      p->target_L[patch] = p->source_L[patch] = self->picked_color[0];
      p->target_a[patch] = p->source_a[patch] = self->picked_color[1];
      p->target_b[patch] = p->source_b[patch] = self->picked_color[2];
      dt_dev_add_history_item(darktable.develop, self, TRUE);

      ++darktable.gui->reset;
      _colorchecker_rebuild_patch_list(self);
      dt_bauhaus_combobox_set(g->combobox_patch, patch);
      _colorchecker_update_sliders(self);
      --darktable.gui->reset;
      g->patch = g->drawn_patch = patch;
      gtk_widget_queue_draw(g->area);
    }
    return TRUE;
  }
  if(patch >= p->num_patches) patch = p->num_patches-1;
  dt_bauhaus_combobox_set(g->combobox_patch, patch);
  return FALSE;
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_colorchecker_gui_data_t *g = IOP_GUI_ALLOC(colorchecker);
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->default_params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // custom 24-patch widget in addition to combo box
  g->area = dtgtk_drawing_area_new_with_aspect_ratio(4.0/6.0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->area, TRUE, TRUE, 0);

  gtk_widget_add_events(GTK_WIDGET(g->area), GDK_POINTER_MOTION_MASK
                                             | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                             | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(checker_draw), self);
  g_signal_connect(G_OBJECT(g->area), "button-press-event", G_CALLBACK(checker_button_press), self);
  g_signal_connect(G_OBJECT(g->area), "motion-notify-event", G_CALLBACK(checker_motion_notify), self);

  g->patch = 0;
  g->drawn_patch = -1;
  g->combobox_patch = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->combobox_patch, NULL, N_("patch"));
  gtk_widget_set_tooltip_text(g->combobox_patch, _("color checker patch"));
  char cboxentry[1024];
  for(int k=0;k<p->num_patches;k++)
  {
    snprintf(cboxentry, sizeof(cboxentry), _("patch #%d"), k);
    dt_bauhaus_combobox_add(g->combobox_patch, cboxentry);
  }

  dt_color_picker_new(self, DT_COLOR_PICKER_POINT_AREA, g->combobox_patch);

  g->scale_L = dt_bauhaus_slider_new_with_range(self, -100.0, 200.0, 0, 0.0f, 2);
  gtk_widget_set_tooltip_text(g->scale_L, _("adjust target color Lab 'L' channel\nlower values darken target color while higher brighten it"));
  dt_bauhaus_widget_set_label(g->scale_L, NULL, N_("lightness"));

  g->scale_a = dt_bauhaus_slider_new_with_range(self, -256.0, 256.0, 0, 0.0f, 2);
  gtk_widget_set_tooltip_text(g->scale_a, _("adjust target color Lab 'a' channel\nlower values shift target color towards greens while higher shift towards magentas"));
  dt_bauhaus_widget_set_label(g->scale_a, NULL, N_("green-magenta offset"));
  dt_bauhaus_slider_set_stop(g->scale_a, 0.0, 0.0, 1.0, 0.2);
  dt_bauhaus_slider_set_stop(g->scale_a, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->scale_a, 1.0, 1.0, 0.0, 0.2);

  g->scale_b = dt_bauhaus_slider_new_with_range(self, -256.0, 256.0, 0, 0.0f, 2);
  gtk_widget_set_tooltip_text(g->scale_b, _("adjust target color Lab 'b' channel\nlower values shift target color towards blues while higher shift towards yellows"));
  dt_bauhaus_widget_set_label(g->scale_b, NULL, N_("blue-yellow offset"));
  dt_bauhaus_slider_set_stop(g->scale_b, 0.0, 0.0, 0.0, 1.0);
  dt_bauhaus_slider_set_stop(g->scale_b, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->scale_b, 1.0, 1.0, 1.0, 0.0);

  g->scale_C = dt_bauhaus_slider_new_with_range(self, -128.0, 128.0, 0, 0.0f, 2);
  gtk_widget_set_tooltip_text(g->scale_C, _("adjust target color saturation\nadjusts 'a' and 'b' channels of target color in Lab space simultaneously\nlower values scale towards lower saturation while higher scale towards higher saturation"));
  dt_bauhaus_widget_set_label(g->scale_C, NULL, N_("saturation"));

  g->absolute_target = 0;
  g->combobox_target = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->combobox_target, 0, N_("target color"));
  gtk_widget_set_tooltip_text(g->combobox_target, _("control target color of the patches\nrelative - target color is relative from the patch original color\nabsolute - target color is absolute Lab value"));
  dt_bauhaus_combobox_add(g->combobox_target, _("relative"));
  dt_bauhaus_combobox_add(g->combobox_target, _("absolute"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->combobox_patch, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_L, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_a, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_b, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_C, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->combobox_target, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(g->combobox_patch), "value-changed", G_CALLBACK(patch_callback), self);
  g_signal_connect(G_OBJECT(g->scale_L), "value-changed", G_CALLBACK(target_L_callback), self);
  g_signal_connect(G_OBJECT(g->scale_a), "value-changed", G_CALLBACK(target_a_callback), self);
  g_signal_connect(G_OBJECT(g->scale_b), "value-changed", G_CALLBACK(target_b_callback), self);
  g_signal_connect(G_OBJECT(g->scale_C), "value-changed", G_CALLBACK(target_C_callback), self);
  g_signal_connect(G_OBJECT(g->combobox_target), "value-changed", G_CALLBACK(target_callback), self);
}

#undef MAX_PATCHES

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
