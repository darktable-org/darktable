/*
    This file is part of darktable,
    Copyright (C) 2022-2023 darktable developers.

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

#include "common/color_vocabulary.h"
#include "common/utility.h"

// get a range of 2 × factor × std centered in avg
static range_t
_compute_range(const gaussian_stats_t stats, const float factor)
{
  range_t out;
  out.bottom = stats.avg - factor * stats.std;
  out.top = stats.avg + factor * stats.std;
  return out;
}

const char *Lch_to_color_name(dt_aligned_pixel_t color)
{
  // color must be Lch derivated from CIE Lab 1976 turned into polar coordinates

  // First check if we have a gray (chromacity < epsilon)

  if(color[1] < 2.0f)
    return _("gray");

  // Start with special cases : skin tones

  dt_aligned_pixel_t Lab;
  dt_LCH_2_Lab(color, Lab);

  // Human skin tones database
  // This is a racially-charged matter, tread with it carefully.

  // Usable data are : tabulated avg ± std (P < 0.05) models on skin color measurements
  // on more than 80 individuals under D65 illuminant.

  // Notice all these data are valid only under D65 illuminant and errors up to delta E = 6 have
  // been measured for A illuminant. Proper camera profiling and chromatic adaptation needs to be performed
  // or all the following is meaningless.

  // We use ranges of avg ± 2 std, giving 95 % of confidence in the prediction.

  // We use CIE Lab instead of Lch coordinates, because a and b parameters are physiologically meaningful :
  //  - a (redness) is linked to blood flow and health,
  //  - b (yellowness) is linked to melanine and sun tan.

  /* Reference :
     XIAO, Kaida, YATES, Julian M., ZARDAWI, Faraedon, et al.
     Characterising the variations in ethnic skin colours: a new calibrated data base for human skin.
     Skin Research and Technology, 2017, vol. 23, no 1, p. 21-29.
     https://onlinelibrary.wiley.com/doi/pdf/10.1111/srt.12295

     Sample : 187 caucasian, 202 chinese, 145 kurdish and 426 thai.

     DE RIGAL, Jean, DES MAZIS, Isabelle, DIRIDOLLOU, Stephane, et al.
     The effect of age on skin color and color heterogeneity in four ethnic groups.
     Skin Research and Technology, 2010, vol. 16, no 2, p. 168-178.
     https://pubmed.ncbi.nlm.nih.gov/20456097/

     Sample : 121 african-american, 64 mexican.
     Note : the data have been read on the graph and are inaccurate and std is majorated.
     The original authors have been contacted to get the tabulated, accurate data,
     but the main author is retired, co-authors have changed jobs, and the L'Oréal head of R&D
     did not respond. So the values here are given for what it's worth.
  */

  // "Forearm" is the ventral forearm skin. It is the least sun-tanned part of skin.
  // Sun tan will depend the most on lifestyle, therefore the ventral forearm
  // is the least socially-biased skin color metric.

  // "Forehead" is the most sun-tanned part of skin. This translates to high b coordinate.

  // "Cheek" is the most redish part of skin. This translates to high a coordinate.

  // L decreases with age in all ethnicities and with b/yellowness/melanine/tan.

  const ethnicity_t ethnies[ETHNIE_END] =
    { { .name = _("Chinese"),          .ethnicity = ETHNIE_CHINESE },
      { .name = _("Thai"),             .ethnicity = ETHNIE_THAI },
      { .name = _("Kurdish"),          .ethnicity = ETHNIE_KURDISH },
      { .name = _("Caucasian"),        .ethnicity = ETHNIE_CAUCASIAN },
      { .name = _("African-American"), .ethnicity = ETHNIE_AFRICAN_AM },
      { .name = _("Mexican"),          .ethnicity = ETHNIE_MEXICAN } };

  const skin_color_t skin[SKINS] = {
    { .name = _("forearm"),
      .ethnicity = ETHNIE_CHINESE,
      .L = { .avg = 60.9f, .std = 3.4f },
      .a = { .avg = 7.0f, .std = 1.7f },
      .b = { .avg = 15.0f, .std = 1.8f } },
    { .name = _("forearm"),
      .ethnicity = ETHNIE_THAI,
      .L = { .avg = 61.9f, .std = 3.7f },
      .a = { .avg = 7.1f, .std = 1.7f },
      .b = { .avg = 17.4f, .std = 2.0f } },
    { .name = _("forearm"),
      .ethnicity = ETHNIE_KURDISH,
      .L = { .avg = 60.6f, .std = 4.8f },
      .a = { .avg = 6.5f, .std = 1.6f },
      .b = { .avg = 16.4f, .std = 2.3f } },
    { .name = _("forearm"),
      .ethnicity = ETHNIE_CAUCASIAN,
      .L = { .avg = 63.0f, .std = 5.5f },
      .a = { .avg = 5.6f, .std = 1.9f },
      .b = { .avg = 14.0f, .std = 2.9f } },
    { .name = _("forehead"),
      .ethnicity = ETHNIE_CHINESE,
      .L = { .avg = 56.4f, .std = 3.2f },
      .a = { .avg = 11.7f, .std = 2.1f },
      .b = { .avg = 16.3f, .std = 1.4f } },
    { .name = _("forehead"),
      .ethnicity = ETHNIE_THAI,
      .L = { .avg = 56.8f, .std = 4.1f },
      .a = { .avg = 11.6f, .std = 2.2f },
      .b = { .avg = 17.7f, .std = 1.8f } },
    { .name = _("forehead"),
      .ethnicity = ETHNIE_KURDISH,
      .L = { .avg = 56.1f, .std = 4.5f },
      .a = { .avg = 11.3f, .std = 2.1f },
      .b = { .avg = 16.4f, .std = 2.2f } },
    { .name = _("forehead"),
      .ethnicity = ETHNIE_CAUCASIAN,
      .L = { .avg = 59.2f, .std = 5.1f },
      .a = { .avg = 11.6f, .std = 2.8f },
      .b = { .avg = 15.1f, .std = 2.3f } },
    { .name = _("forehead"),
      .ethnicity = ETHNIE_AFRICAN_AM,
      .L = { .avg = 44.0f, .std = 2.0f },
      .a = { .avg = 14.0f, .std = 1.0f },
      .b = { .avg = 19.0f, .std = 1.0f } },
    { .name = _("forehead"),
      .ethnicity = ETHNIE_MEXICAN,
      .L = { .avg = 58.0f, .std = 1.0f },
      .a = { .avg = 15.0f, .std = 1.0f },
      .b = { .avg = 21.0f, .std = 1.0f } },
    { .name = _("cheek"),
      .ethnicity = ETHNIE_CHINESE,
      .L = { .avg = 58.9f, .std = 3.1f },
      .a = { .avg = 11.4f, .std = 2.1f },
      .b = { .avg = 14.2f, .std = 1.5f } },
    { .name = _("cheek"),
      .ethnicity = ETHNIE_THAI,
      .L = { .avg = 60.7f, .std = 4.0f },
      .a = { .avg = 10.5f, .std = 2.3f },
      .b = { .avg = 17.2f, .std = 2.1f } },
    { .name = _("cheek"),
      .ethnicity = ETHNIE_KURDISH,
      .L = { .avg = 58.f,  .std = 4.4f },
      .a = { .avg = 11.7f, .std = 2.3f },
      .b = { .avg = 15.8f, .std = 2.1f } },
    { .name = _("cheek"),
      .ethnicity = ETHNIE_CAUCASIAN,
      .L = { .avg = 59.6f, .std = 5.5f },
      .a = { .avg = 11.8f, .std = 3.1f },
      .b = { .avg = 14.6f, .std = 2.6f } },
    { .name = _("cheek"),
      .ethnicity = ETHNIE_AFRICAN_AM,
      .L = { .avg = 48.0f, .std = 1.0f },
      .a = { .avg = 15.0f, .std = 1.0f },
      .b = { .avg = 20.0f, .std = 1.0f } },
    { .name = _("cheek"),
      .ethnicity = ETHNIE_MEXICAN,
      .L = { .avg = 63.0f, .std = 1.0f },
      .a = { .avg = 16.0f, .std = 1.0f },
      .b = { .avg = 21.0f, .std = 1.0f } } };

  gchar *out = NULL;
  int is_skin = FALSE;
  gboolean matches[ETHNIE_END] = { FALSE };

  // Find a match against any body part and write the associated ethnicity
  for(int elem = 0; elem < SKINS; ++elem)
  {
    range_t L = _compute_range(skin[elem].L, 1.5f);
    range_t a = _compute_range(skin[elem].a, 1.5f);
    range_t b = _compute_range(skin[elem].b, 1.5f);

    const int match = (Lab[0] > L.bottom && Lab[0] < L.top) && (Lab[1] > a.bottom && Lab[1] < a.top)
      && (Lab[2] > b.bottom && Lab[2] < b.top);

    is_skin = is_skin || match;
    if(match) matches[skin[elem].ethnicity] = TRUE;
  }

  // Write all matching ethnicities
  for(int elem = 0; elem < ETHNIE_END; ++elem)
    if(matches[elem])
      out = dt_util_dstrcat(out, _("average %s skin tone\n"), ethnies[elem].name);

  if(is_skin) return out;

  // Reference for color names : https://chromatone.center/theory/color/models/perceptual/
  // Though we ignore them sometimes when they get too lyrical for some more down-to-earth names
  // Color are read for chroma = [80 - 100].
  const float h = color[2] * 360.f; // °
  const float L = color[0];
  //const float c = color[1];

  // h in degrees - split into 15 hue sectors of 24°
  const int step_h = (int)(h) / 24;

  // L in % - split into 5 L sectors of 20 %
  const int step_L = (int)(fminf(L, 100.f)) / 20;

  if(step_h == 0)
  {
    // 0° - pinkish red
    if(step_L == 0) return _("deep purple");    // L = 10 %
    if(step_L == 1) return _("fuchsia");        // L = 30 %
    if(step_L == 2) return _("medium magenta"); // L = 50 %
    if(step_L == 3) return _("violet pink");    // L = 70 %
    if(step_L == 4) return _("plum violet");    // L = 90 %
  }
  else if(step_h == 1)
  {
    // 24° - red
    if(step_L == 0) return _("dark red");
    if(step_L == 1) return _("red");
    if(step_L == 2) return _("crimson");
    if(step_L == 3) return _("salmon");
    if(step_L == 4) return _("pink");
  }
  else if(step_h == 2)
  {
    // 48° - orangy red
    if(step_L == 0) return _("maroon");
    if(step_L == 1) return _("dark orange red");
    if(step_L == 2) return _("orange red");
    if(step_L == 3) return _("coral");
    if(step_L == 4) return _("khaki");
  }
  else if(step_h == 3)
  {
    // 72° - orange
    if(step_L == 0) return _("brown");
    if(step_L == 1) return _("chocolate");
    if(step_L == 2) return _("dark gold");
    if(step_L == 3) return _("gold");
    if(step_L == 4) return _("sandy brown");
  }
  else if(step_h == 4)
  {
    // 96° - yellow olive
    if(step_L == 0) return _("dark green");
    if(step_L == 1) return _("dark olive green");
    if(step_L == 2) return _("olive");
    if(step_L == 3) return _("khaki");
    if(step_L == 4) return _("beige");
  }
  else if(step_h == 5)
  {
    // 120° - green
    if(step_L == 0) return _("dark green");
    if(step_L == 1) return _("forest green");
    if(step_L == 2) return _("olive drab");
    if(step_L == 3) return _("yellow green");
    if(step_L == 4) return _("pale green");
  }
  else if(step_h == 6)
  {
    // 144° - blueish green
    if(step_L == 0) return _("dark green");
    if(step_L == 1) return _("green");
    if(step_L == 2) return _("forest green");
    if(step_L == 3) return _("lime green");
    if(step_L == 4) return _("pale green");
  }
  else if(step_h == 7)
  {
    // 168° - greenish cyian
    if(step_L == 0) return _("dark sea green");
    if(step_L == 1) return _("sea green");
    if(step_L == 2) return _("teal");
    if(step_L == 3) return _("light sea green");
    if(step_L == 4) return _("turquoise");
  }
  else if(step_h == 8)
  {
    // 192° - cyan
    if(step_L == 0) return _("dark slate gray");
    if(step_L == 1) return _("light slate gray");
    if(step_L == 2) return _("dark cyan");
    if(step_L == 3) return _("aqua");
    if(step_L == 4) return _("cyan");
  }
  else if(step_h == 9)
  {
    // 216° - medium blue
    if(step_L == 0) return _("navy blue");
    if(step_L == 1) return _("teal");
    if(step_L == 2) return _("dark cyan");
    if(step_L == 3) return _("deep sky blue");
    if(step_L == 4) return _("aquamarine blue");
  }
  else if(step_h == 10 || step_h == 11)
  {
    // 240° - blue and 264° - bluer than blue
    // these are collapsed because CIE Lab 1976 sucks for blues
    if(step_L == 0) return _("dark blue");
    if(step_L == 1) return _("medium blue");
    if(step_L == 2) return _("azure blue");
    if(step_L == 3) return _("deep sky blue");
    if(step_L == 4) return _("aqua");
  }
  else if(step_h == 12)
  {
    // 288° - more blue
    if(step_L == 0) return _("dark blue");
    if(step_L == 1) return _("medium blue");
    if(step_L == 2) return _("blue");
    if(step_L == 3) return _("light sky blue");
    if(step_L == 4) return _("light blue");
  }
  else if(step_h == 13)
  {
    // 312° - violet
    if(step_L == 0) return _("indigo");
    if(step_L == 1) return _("dark violet");
    if(step_L == 2) return _("blue violet");
    if(step_L == 3) return _("violet");
    if(step_L == 4) return _("plum");
  }
  else if(step_h == 14)
  {
    if(step_L == 0) return _("purple");
    if(step_L == 1) return _("dark magenta");
    if(step_L == 2) return _("magenta");
    if(step_L == 3) return _("violet");
    if(step_L == 4) return _("lavender");
  }

  return _("color not found");
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
