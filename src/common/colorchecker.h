/*
    This file is part of darktable,
    Copyright (C) 2020 darktable developers.

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

/**
 * These are the CIELab values of Color Checker reference targets
 */

// types of targets we support
typedef enum dt_color_checker_targets
{
  COLOR_CHECKER_XRITE_24 = 0,
  COLOR_CHECKER_SPYDER_48 = 1,
  COLOR_CHECKER_LAST
} dt_color_checker_targets;

// helper to deal with patch color
typedef struct dt_color_checker_patch
{
  const char *name;     // mnemonic name for the patch
  float Lab[3];         // reference color in CIE Lab

  // (x, y) position of the patch center, relatively to the guides (white dots)
  // in ratio of the grid dimension along that axis
  struct {
    float x;
    float y;
  };
} dt_color_checker_patch;

typedef struct dt_color_checker_t
{
  const char *name;
  const char *author;
  const char *date;
  const char *manufacturer;

  float ratio;                        // format ratio of the chart, guide to guide (white dots)
  float radius;                       // radius of a patch in ratio of the checker diagonal
  size_t patches;                     // number of patches in target
  size_t size[2];                     // dimension along x, y axes
  size_t middle_grey;                 // index of the closest patch to 20% neutral grey
  size_t white;                       // index of the closest patch to pure white
  size_t black;                       // index of the closest patch to pure black
  dt_color_checker_patch values[];    // array of colors
} dt_color_checker_t;

/*
const dt_color_checker_t xrite_24 = { .name = "Xrite ColorChecker 24",
                                      .author = "Graeme Gill, ArgyllCMS from Gretag Macbeth reference",
                                      .date = "Feb 18, 2008",
                                      .manufacturer = "X-Rite/Gretag Macbeth",
                                      .patches = 24,
                                      .size = { 4, 6 },
                                      .middle_grey = { "D2", { 81.26,   -0.64,   -0.34 }},
                                      .values = { { "A1", { 37.99,   13.56,   14.06 }},
                                                  { "A2", { 65.71,   18.13,   17.81 }},
                                                  { "A3", { 49.93,   -4.88,  -21.93 }},
                                                  { "A4", { 43.14,  -13.10,   21.91 }},
                                                  { "A5", { 55.11,    8.84,  -25.40 }},
                                                  { "A6", { 70.72,  -33.40,   -0.20 }},
                                                  { "B1", { 62.66,   36.07,   57.10 }},
                                                  { "B2", { 40.02,   10.41,  -45.96 }},
                                                  { "B3", { 51.12,   48.24,   16.25 }},
                                                  { "B4", { 30.33,   22.98,  -21.59 }},
                                                  { "B5", { 72.53,  -23.71,   57.26 }},
                                                  { "B6", { 71.94,   19.36,   67.86 }},
                                                  { "C1", { 28.78,   14.18,  -50.30 }},
                                                  { "C2", { 55.26,  -38.34,   31.37 }},
                                                  { "C3", { 42.10,   53.38,   28.19 }},
                                                  { "C4", { 81.73,    4.04,   79.82 }},
                                                  { "C5", { 51.94,   49.99,  -14.57 }},
                                                  { "C6", { 51.04,  -28.63,  -28.64 }},
                                                  { "D1", { 96.54,   -0.43,    1.19 }},
                                                  { "D2", { 81.26,   -0.64,   -0.34 }},
                                                  { "D3", { 66.77,   -0.73,   -0.50 }},
                                                  { "D4", { 50.87,   -0.15,   -0.27 }},
                                                  { "D5", { 35.66,   -0.42,   -1.23 }},
                                                  { "D6", { 20.46,   -0.08,   -0.97 }} } };
*/

dt_color_checker_t spyder_48 = {  .name = "Datacolor SpyderCheckr 48",
                                        .author = "Aurélien PIERRE",
                                        .date = "dec, 9 2016",
                                        .manufacturer = "DataColor",
                                        .ratio = 2.f / 3.f,
                                        .radius = 0.035,
                                        .patches = 48,
                                        .size = { 8, 6 },
                                        .middle_grey = 25,
                                        .white = 24,
                                        .black = 29,
                                        .values = { { "A1", { 61.35,  34.81,  18.38 }, { 0.085, 0.125 } },
                                                    { "A2", { 75.50 ,  5.84,  50.42 }, { 0.085, 0.274 } },
                                                    { "A3", { 66.82,	-25.1,	23.47 }, { 0.085, 0.423 } },
                                                    { "A4", { 60.53,	-22.6, -20.40 }, { 0.085, 0.572 } },
                                                    { "A5", { 59.66,	-2.03, -28.46 }, { 0.085, 0.721 } },
                                                    { "A6", { 59.15,	30.83,  -5.72 }, { 0.085, 0.870 } },
                                                    { "B1", { 82.68,	 5.03,	 3.02 }, { 0.185, 0.125 } },
                                                    { "B2", { 82.25,	-2.42,	 3.78 }, { 0.185, 0.274 } },
                                                    { "B3", { 82.29,	 2.20,	-2.04 }, { 0.185, 0.423 } },
                                                    { "B4", { 24.89,	 4.43,	 0.78 }, { 0.185, 0.572 } },
                                                    { "B5", { 25.16,	-3.88,	 2.13 }, { 0.185, 0.721 } },
                                                    { "B6", { 26.13,	 2.61,	-5.03 }, { 0.185, 0.870 } },
                                                    { "C1", { 85.42,	 9.41,	14.49 }, { 0.285, 0.125 } },
                                                    { "C2", { 74.28,	 9.05,	27.21 }, { 0.285, 0.274 } },
                                                    { "C3", { 64.57,	12.39,	37.24 }, { 0.285, 0.423 } },
                                                    { "C4", { 44.49,	17.23,	26.24 }, { 0.285, 0.572 } },
                                                    { "C5", { 25.29,	 7.95,	 8.87 }, { 0.285, 0.721 } },
                                                    { "C6", { 22.67,	 2.11,	-1.10 }, { 0.285, 0.870 } },
                                                    { "D1", { 92.72,	 1.89,	 2.76 }, { 0.385, 0.125 } },
                                                    { "D2", { 88.85,	 1.59,	 2.27 }, { 0.385, 0.274 } },
                                                    { "D3", { 73.42,	 0.99,	 1.89 }, { 0.385, 0.423 } },
                                                    { "D4", { 57.15,	 0.57,	 1.19 }, { 0.385, 0.572 } },
                                                    { "D5", { 41.57,	 0.24,	 1.45 }, { 0.385, 0.721 } },
                                                    { "D6", { 25.65,	 1.24,	 0.05 }, { 0.385, 0.870 } },
                                                    { "E1", { 96.04,	 2.16,	 2.60 }, { 0.612, 0.125 } },
                                                    { "E2", { 80.44,	 1.17,	 2.05 }, { 0.612, 0.274 } },
                                                    { "E3", { 65.52,	 0.69,	 1.86 }, { 0.612, 0.423 } },
                                                    { "E4", { 49.62,	 0.58,	 1.56 }, { 0.612, 0.572 } },
                                                    { "E5", { 33.55,	 0.35,	 1.40 }, { 0.612, 0.721 } },
                                                    { "E6", { 16.91,	 1.43,	-0.81 }, { 0.612, 0.870 } },
                                                    { "F1", { 47.12, -32.50, -28.75 }, { 0.713, 0.125 } },
                                                    { "F2", { 50.49,	53.45, -13.55 }, { 0.713, 0.274 } },
                                                    { "F3", { 83.61,	 3.36,	87.02 }, { 0.713, 0.423 } },
                                                    { "F4", { 41.05,	60.75,	31.17 }, { 0.713, 0.572 } },
                                                    { "F5", { 54.14, -40.80,	34.75 }, { 0.713, 0.721 } },
                                                    { "F6", { 24.75,	13.78, -49.48 }, { 0.713, 0.870 } },
                                                    { "G1", { 60.94,	38.21,	61.31 }, { 0.814, 0.125 } },
                                                    { "G2", { 37.80,	 7.30, -43.04 }, { 0.814, 0.274 } },
                                                    { "G3", { 49.81,	48.50,	15.76 }, { 0.814, 0.423 } },
                                                    { "G4", { 28.88,	19.36, -24.48 }, { 0.814, 0.572 } },
                                                    { "G5", { 72.45, -23.60,	60.47 }, { 0.814, 0.721 } },
                                                    { "G6", { 71.65,	23.74,	72.28 }, { 0.814, 0.870 } },
                                                    { "H1", { 70.19, -31.90,	 1.98 }, { 0.915, 0.125 } },
                                                    { "H2", { 54.38,	 8.84, -25.71 }, { 0.915, 0.274 } },
                                                    { "H3", { 42.03, -15.80,	22.93 }, { 0.915, 0.423 } },
                                                    { "H4", { 48.82,	-5.11, -23.08 }, { 0.915, 0.572 } },
                                                    { "H5", { 65.10,	18.14,	16.68 }, { 0.915, 0.721 } },
                                                    { "H6", { 36.13,	14.15,	15.78 }, { 0.915, 0.870 } } } };


dt_color_checker_t * dt_get_color_checker(const dt_color_checker_targets target_type)
{
  switch(target_type)
  {
    case COLOR_CHECKER_XRITE_24:
      return &spyder_48;

    case COLOR_CHECKER_SPYDER_48:
      return &spyder_48;

    case COLOR_CHECKER_LAST:
      return &spyder_48;
  }

  return &spyder_48;
}

/**
 * helper functions
 */

// get a patch index in the list of values from the coordinates of the patch in the checker array
static inline size_t dt_color_checker_get_index(const dt_color_checker_t *const target_checker, const size_t coordinates[2])
{
  // patches are stored column-major
  const size_t height = target_checker->size[1];
  return CLAMP(height * coordinates[0] + coordinates[1], 0, target_checker->patches - 1);
}

// get a a patch coordinates of in the checker array from the patch index in the list of values
static inline void dt_color_checker_get_coordinates(const dt_color_checker_t *const target_checker, size_t *coordinates, const size_t index)
{
  // patches are stored column-major
  const size_t idx = CLAMP(index, 0, target_checker->patches - 1);
  const size_t height = target_checker->size[1];
  const size_t num_col = idx / height;
  const size_t num_lin = idx - num_col * height;
  coordinates[0] = CLAMP(num_col, 0, target_checker->size[0] - 1);
  coordinates[1] = CLAMP(num_lin, 0, target_checker->size[1] - 1);
}

// find a patch matching a name
static inline const dt_color_checker_patch *const dt_color_checker_get_patch_by_name(const dt_color_checker_t *const target_checker,
                                                                              const char *name, size_t *index)
{
  size_t idx = -1;
  const dt_color_checker_patch *patch = NULL;

  for(size_t k = 0; k < target_checker->patches; k++)
    if(strcmp(name, target_checker->values[k].name) == 0)
    {
      idx = k;
      patch = &target_checker->values[k];
      break;
    }

  if(patch == NULL) fprintf(stderr, "No patch matching name `%s` was found in %s\n", name, target_checker->name);

  if(index ) *index = idx;
  return patch;
}
