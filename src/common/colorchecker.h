/*
    This file is part of darktable,
    Copyright (C) 2020-2023 darktable developers.

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
  COLOR_CHECKER_XRITE_24_2000 = 0,
  COLOR_CHECKER_XRITE_24_2014 = 1,
  COLOR_CHECKER_SPYDER_24     = 2,
  COLOR_CHECKER_SPYDER_24_V2  = 3,
  COLOR_CHECKER_SPYDER_48     = 4,
  COLOR_CHECKER_SPYDER_48_V2  = 5,
  COLOR_CHECKER_SPYDER_PHOTO  = 6,
  COLOR_CHECKER_LAST
} dt_color_checker_targets;

// helper to deal with patch color
typedef struct dt_color_checker_patch
{
  const char *name;        // mnemonic name for the patch
  dt_aligned_pixel_t Lab;  // reference color in CIE Lab

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
  dt_color_checker_targets type;

  float ratio;                        // format ratio of the chart, guide to guide (white dots)
  float radius;                       // radius of a patch in ratio of the checker diagonal
  size_t patches;                     // number of patches in target
  size_t size[2];                     // dimension along x, y axes
  size_t middle_grey;                 // index of the closest patch to 20% neutral grey
  size_t white;                       // index of the closest patch to pure white
  size_t black;                       // index of the closest patch to pure black
  dt_color_checker_patch values[];    // array of colors
} dt_color_checker_t;


dt_color_checker_t xrite_24_2000 = { .name = "Xrite ColorChecker 24 before 2014",
                                    .author = "X-Rite",
                                    .date = "3/27/2000",
                                    .manufacturer = "X-Rite/Gretag Macbeth",
                                    .type = COLOR_CHECKER_XRITE_24_2000,
                                    .radius = 0.055f,
                                    .ratio = 2.f / 3.f,
                                    .patches = 24,
                                    .size = { 4, 6 },
                                    .middle_grey = 21,
                                    .white = 18,
                                    .black = 23,
                                    .values = {
                                              { "A1", { 37.986,  13.555,  14.059 }, { 0.087, 0.125}},
                                              { "A2", { 65.711,  18.13,   17.81  }, { 0.250, 0.125}},
                                              { "A3", { 49.927, -04.88,  -21.905 }, { 0.417, 0.125}},
                                              { "A4", { 43.139, -13.095,  21.905 }, { 0.584, 0.125}},
                                              { "A5", { 55.112,  08.844, -25.399 }, { 0.751, 0.125}},
                                              { "A6", { 70.719, -33.397,  -0.199 }, { 0.918, 0.125}},
                                              { "B1", { 62.661,  36.067,  57.096 }, { 0.087, 0.375}},
                                              { "B2", { 40.02,   10.41,  -45.964 }, { 0.250, 0.375}},
                                              { "B3", { 51.124,  48.239,  16.248 }, { 0.417, 0.375}},
                                              { "B4", { 30.325,  22.976, -21.587 }, { 0.584, 0.375}},
                                              { "B5", { 72.532, -23.709,  57.255 }, { 0.751, 0.375}},
                                              { "B6", { 71.941,  19.363,  67.857 }, { 0.918, 0.375}},
                                              { "C1", { 28.778,  14.179, -50.297 }, { 0.087, 0.625}},
                                              { "C2", { 55.261, -38.342,  31.37  }, { 0.250, 0.625}},
                                              { "C3", { 42.101,  53.378,  28.19  }, { 0.417, 0.625}},
                                              { "C4", { 81.733,  04.039,  79.819 }, { 0.584, 0.625}},
                                              { "C5", { 51.935,  49.986, -14.574 }, { 0.751, 0.625}},
                                              { "C6", { 51.038, -28.631, -28.638 }, { 0.918, 0.625}},
                                              { "D1", { 96.539,  -0.425,   1.186 }, { 0.087, 0.875}},
                                              { "D2", { 81.257,  -0.638,  -0.335 }, { 0.250, 0.875}},
                                              { "D3", { 66.766,  -0.734,  -0.504 }, { 0.417, 0.875}},
                                              { "D4", { 50.867,  -0.153,  -0.27  }, { 0.584, 0.875}},
                                              { "D5", { 35.656,  -0.421,  -1.231 }, { 0.751, 0.875}},
                                              { "D6", { 20.461,  -0.079,  -0.973 }, { 0.918, 0.875}} } };


dt_color_checker_t xrite_24_2014 = { .name = "Xrite ColorChecker 24 after 2014",
                                    .author = "X-Rite",
                                    .date = "3/28/2015",
                                    .manufacturer = "X-Rite/Gretag Macbeth",
                                    .type = COLOR_CHECKER_XRITE_24_2014,
                                    .radius = 0.055f,
                                    .ratio = 2.f / 3.f,
                                    .patches = 24,
                                    .size = { 4, 6 },
                                    .middle_grey = 21,
                                    .white = 18,
                                    .black = 23,
                                    .values = {
                                              { "A1", { 37.54,   14.37,   14.92 }, { 0.087, 0.125}},
                                              { "A2", { 64.66,   19.27,   17.50 }, { 0.250, 0.125}},
                                              { "A3", { 49.32,  -03.82,  -22.54 }, { 0.417, 0.125}},
                                              { "A4", { 43.46,  -12.74,   22.72 }, { 0.584, 0.125}},
                                              { "A5", { 54.94,   09.61,  -24.79 }, { 0.751, 0.125}},
                                              { "A6", { 70.48,  -32.26,  -00.37 }, { 0.918, 0.125}},
                                              { "B1", { 62.73,   35.83,   56.50 }, { 0.087, 0.375}},
                                              { "B2", { 39.43,   10.75,  -45.17 }, { 0.250, 0.375}},
                                              { "B3", { 50.57,   48.64,   16.67 }, { 0.417, 0.375}},
                                              { "B4", { 30.10,   22.54,  -20.87 }, { 0.584, 0.375}},
                                              { "B5", { 71.77,  -24.13,   58.19 }, { 0.751, 0.375}},
                                              { "B6", { 71.51,   18.24,   67.37 }, { 0.918, 0.375}},
                                              { "C1", { 28.37,   15.42,  -49.80 }, { 0.087, 0.625}},
                                              { "C2", { 54.38,  -39.72,   32.27 }, { 0.250, 0.625}},
                                              { "C3", { 42.43,   51.05,   28.62 }, { 0.417, 0.625}},
                                              { "C4", { 81.80,   02.67,   80.41 }, { 0.584, 0.625}},
                                              { "C5", { 50.63,   51.28,  -14.12 }, { 0.751, 0.625}},
                                              { "C6", { 49.57,  -29.71,  -28.32 }, { 0.918, 0.625}},
                                              { "D1", { 95.19,  -01.03,   02.93 }, { 0.087, 0.875}},
                                              { "D2", { 81.29,  -00.57,   00.44 }, { 0.250, 0.875}},
                                              { "D3", { 66.89,  -00.75,  -00.06 }, { 0.417, 0.875}},
                                              { "D4", { 50.76,  -00.13,   00.14 }, { 0.584, 0.875}},
                                              { "D5", { 35.63,  -00.46,  -00.48 }, { 0.751, 0.875}},
                                              { "D6", { 20.64,   00.07,  -00.46 }, { 0.918, 0.875}} } };


// dimensions between reference dots : 197 mm width × 135 mm height
// patch width : 26×26 mm
// outer gutter : 8 mm
// internal gutters (gap between patches) : 5 mm
dt_color_checker_t spyder_24 = {  .name = "Datacolor SpyderCheckr 24 before 2018",
                                  .author = "Aurélien PIERRE",
                                  .date = "dec, 9 2016",
                                  .manufacturer = "DataColor",
                                  .type = COLOR_CHECKER_SPYDER_24,
                                  .ratio = 2.f / 3.f,
                                  .radius = 0.035,
                                  .patches = 24,
                                  .size = { 4, 6 },
                                  .middle_grey = 03,
                                  .white = 00,
                                  .black = 05,
                                  .values = { { "A1", { 96.04,	 2.16,	 2.60 }, { 0.107, 0.844 } },
                                              { "A2", { 80.44,	 1.17,	 2.05 }, { 0.264, 0.844 } },
                                              { "A3", { 65.52,	 0.69,	 1.86 }, { 0.421, 0.844 } },
                                              { "A4", { 49.62,	 0.58,	 1.56 }, { 0.579, 0.844 } },
                                              { "A5", { 33.55,	 0.35,	 1.40 }, { 0.736, 0.844 } },
                                              { "A6", { 16.91,	 1.43,	-0.81 }, { 0.893, 0.844 } },
                                              { "B1", { 47.12, -32.50, -28.75 }, { 0.107, 0.615 } },
                                              { "B2", { 50.49,	53.45, -13.55 }, { 0.264, 0.615 } },
                                              { "B3", { 83.61,	 3.36,	87.02 }, { 0.421, 0.615 } },
                                              { "B4", { 41.05,	60.75,	31.17 }, { 0.579, 0.615 } },
                                              { "B5", { 54.14, -40.80,	34.75 }, { 0.736, 0.615 } },
                                              { "B6", { 24.75,	13.78, -49.48 }, { 0.893, 0.615 } },
                                              { "C1", { 60.94,	38.21,	61.31 }, { 0.107, 0.385 } },
                                              { "C2", { 37.80,	 7.30, -43.04 }, { 0.264, 0.385 } },
                                              { "C3", { 49.81,	48.50,	15.76 }, { 0.421, 0.385 } },
                                              { "C4", { 28.88,	19.36, -24.48 }, { 0.579, 0.385 } },
                                              { "C5", { 72.45, -23.60,	60.47 }, { 0.736, 0.385 } },
                                              { "C6", { 71.65,	23.74,	72.28 }, { 0.893, 0.385 } },
                                              { "D1", { 70.19, -31.90,	 1.98 }, { 0.107, 0.155 } },
                                              { "D2", { 54.38,	 8.84, -25.71 }, { 0.264, 0.155 } },
                                              { "D3", { 42.03, -15.80,	22.93 }, { 0.421, 0.155 } },
                                              { "D4", { 48.82,	-5.11, -23.08 }, { 0.579, 0.155 } },
                                              { "D5", { 65.10,	18.14,	18.68 }, { 0.736, 0.155 } },
                                              { "D6", { 36.13,	14.15,	15.78 }, { 0.893, 0.155 } } } };


// dimensions between reference dots : 197 mm width × 135 mm height
// patch width : 26×26 mm
// outer gutter : 8 mm
// internal gutters (gap between patches) : 5 mm
dt_color_checker_t spyder_24_v2 = {  .name = "Datacolor SpyderCheckr 24 after 2018",
                                  .author = "Aurélien PIERRE",
                                  .date = "dec, 9 2016",
                                  .manufacturer = "DataColor",
                                  .type = COLOR_CHECKER_SPYDER_24_V2,
                                  .ratio = 2.f / 3.f,
                                  .radius = 0.035,
                                  .patches = 24,
                                  .size = { 4, 6 },
                                  .middle_grey = 03,
                                  .white = 00,
                                  .black = 05,
                                  .values = { { "A1", { 96.04,   2.16,   2.60 }, { 0.107, 0.844 } },
                                              { "A2", { 80.44,   1.17,   2.05 }, { 0.264, 0.844 } },
                                              { "A3", { 65.52,   0.69,   1.86 }, { 0.421, 0.844 } },
                                              { "A4", { 49.62,   0.58,   1.56 }, { 0.579, 0.844 } },
                                              { "A5", { 33.55,   0.35,   1.40 }, { 0.736, 0.844 } },
                                              { "A6", { 16.91,   1.43,  -0.81 }, { 0.893, 0.844 } },
                                              { "B1", { 47.12, -32.50, -28.75 }, { 0.107, 0.615 } },
                                              { "B2", { 50.49,  53.45, -13.55 }, { 0.264, 0.615 } },
                                              { "B3", { 83.61,   3.36,  87.02 }, { 0.421, 0.615 } },
                                              { "B4", { 41.05,  60.75,  31.17 }, { 0.579, 0.615 } },
                                              { "B5", { 54.14, -40.80,  34.75 }, { 0.736, 0.615 } },
                                              { "B6", { 24.75,  13.78, -49.48 }, { 0.893, 0.615 } },
                                              { "C1", { 60.94,  38.21,  61.31 }, { 0.107, 0.385 } },
                                              { "C2", { 37.80,   7.30, -43.04 }, { 0.264, 0.385 } },
                                              { "C3", { 49.81,  48.50,  15.76 }, { 0.421, 0.385 } },
                                              { "C4", { 28.88,  19.36, -24.48 }, { 0.579, 0.385 } },
                                              { "C5", { 72.45, -23.57,  60.47 }, { 0.736, 0.385 } },
                                              { "C6", { 71.65,  23.74,  72.28 }, { 0.893, 0.385 } },
                                              { "D1", { 70.19, -31.85,   1.98 }, { 0.107, 0.155 } },
                                              { "D2", { 54.38,   8.84, -25.71 }, { 0.264, 0.155 } },
                                              { "D3", { 42.03, -15.78,  22.93 }, { 0.421, 0.155 } },
                                              { "D4", { 48.82,  -5.11, -23.08 }, { 0.579, 0.155 } },
                                              { "D5", { 65.10,  18.14,  18.68 }, { 0.736, 0.155 } },
                                              { "D6", { 36.13,  14.15,  15.78 }, { 0.893, 0.155 } } } };


// dimensions between reference dots : 297 mm width × 197 mm height
// patch width : 26×26 mm
// outer gutter : 8 mm
// internal gutters (gap between patches) : 5 mm
dt_color_checker_t spyder_48 = {  .name = "Datacolor SpyderCheckr 48 before 2018",
                                  .author = "Aurélien PIERRE",
                                  .date = "dec, 9 2016",
                                  .manufacturer = "DataColor",
                                  .type = COLOR_CHECKER_SPYDER_48,
                                  .ratio = 2.f / 3.f,
                                  .radius = 0.035,
                                  .patches = 48,
                                  .size = { 8, 6 },
                                  .middle_grey = 24,
                                  .white = 21,
                                  .black = 29,
                                  .values = { { "A1", { 61.35,  34.81,  18.38 }, { 0.071, 0.107 } },
                                              { "A2", { 75.50 ,  5.84,  50.42 }, { 0.071, 0.264 } },
                                              { "A3", { 66.82,	-25.1,	23.47 }, { 0.071, 0.421 } },
                                              { "A4", { 60.53,	-22.6, -20.40 }, { 0.071, 0.579 } },
                                              { "A5", { 59.66,	-2.03, -28.46 }, { 0.071, 0.736 } },
                                              { "A6", { 59.15,	30.83,  -5.72 }, { 0.071, 0.893 } },
                                              { "B1", { 82.68,	 5.03,	 3.02 }, { 0.175, 0.107 } },
                                              { "B2", { 82.25,	-2.42,	 3.78 }, { 0.175, 0.264 } },
                                              { "B3", { 82.29,	 2.20,	-2.04 }, { 0.175, 0.421 } },
                                              { "B4", { 24.89,	 4.43,	 0.78 }, { 0.175, 0.579 } },
                                              { "B5", { 25.16,	-3.88,	 2.13 }, { 0.175, 0.736 } },
                                              { "B6", { 26.13,	 2.61,	-5.03 }, { 0.175, 0.893 } },
                                              { "C1", { 85.42,	 9.41,	14.49 }, { 0.279, 0.107 } },
                                              { "C2", { 74.28,	 9.05,	27.21 }, { 0.279, 0.264 } },
                                              { "C3", { 64.57,	12.39,	37.24 }, { 0.279, 0.421 } },
                                              { "C4", { 44.49,	17.23,	26.24 }, { 0.279, 0.579 } },
                                              { "C5", { 25.29,	 7.95,	 8.87 }, { 0.279, 0.736 } },
                                              { "C6", { 22.67,	 2.11,	-1.10 }, { 0.279, 0.893 } },
                                              { "D1", { 92.72,	 1.89,	 2.76 }, { 0.384, 0.107 } },
                                              { "D2", { 88.85,	 1.59,	 2.27 }, { 0.384, 0.264 } },
                                              { "D3", { 73.42,	 0.99,	 1.89 }, { 0.384, 0.421 } },
                                              { "D4", { 57.15,	 0.57,	 1.19 }, { 0.384, 0.579 } },
                                              { "D5", { 41.57,	 0.24,	 1.45 }, { 0.384, 0.736 } },
                                              { "D6", { 25.65,	 1.24,	 0.05 }, { 0.384, 0.893 } },
                                              { "E1", { 96.04,	 2.16,	 2.60 }, { 0.616, 0.107 } },
                                              { "E2", { 80.44,	 1.17,	 2.05 }, { 0.616, 0.264 } },
                                              { "E3", { 65.52,	 0.69,	 1.86 }, { 0.616, 0.421 } },
                                              { "E4", { 49.62,	 0.58,	 1.56 }, { 0.616, 0.579 } },
                                              { "E5", { 33.55,	 0.35,	 1.40 }, { 0.616, 0.736 } },
                                              { "E6", { 16.91,	 1.43,	-0.81 }, { 0.616, 0.893 } },
                                              { "F1", { 47.12, -32.50, -28.75 }, { 0.721, 0.107 } },
                                              { "F2", { 50.49,	53.45, -13.55 }, { 0.721, 0.264 } },
                                              { "F3", { 83.61,	 3.36,	87.02 }, { 0.721, 0.421 } },
                                              { "F4", { 41.05,	60.75,	31.17 }, { 0.721, 0.579 } },
                                              { "F5", { 54.14, -40.80,	34.75 }, { 0.721, 0.736 } },
                                              { "F6", { 24.75,	13.78, -49.48 }, { 0.721, 0.893 } },
                                              { "G1", { 60.94,	38.21,	61.31 }, { 0.825, 0.107 } },
                                              { "G2", { 37.80,	 7.30, -43.04 }, { 0.825, 0.264 } },
                                              { "G3", { 49.81,	48.50,	15.76 }, { 0.825, 0.421 } },
                                              { "G4", { 28.88,	19.36, -24.48 }, { 0.825, 0.579 } },
                                              { "G5", { 72.45, -23.60,	60.47 }, { 0.825, 0.736 } },
                                              { "G6", { 71.65,	23.74,	72.28 }, { 0.825, 0.893 } },
                                              { "H1", { 70.19, -31.90,	 1.98 }, { 0.929, 0.107 } },
                                              { "H2", { 54.38,	 8.84, -25.71 }, { 0.929, 0.264 } },
                                              { "H3", { 42.03, -15.80,	22.93 }, { 0.929, 0.421 } },
                                              { "H4", { 48.82,	-5.11, -23.08 }, { 0.929, 0.579 } },
                                              { "H5", { 65.10,	18.14,	18.68 }, { 0.929, 0.736 } },
                                              { "H6", { 36.13,	14.15,	15.78 }, { 0.929, 0.893 } } } };


// dimensions between reference dots : 297 mm width × 197 mm height
// patch width : 26×26 mm
// outer gutter : 8 mm
// internal gutters (gap between patches) : 5 mm
dt_color_checker_t spyder_48_v2 = {  .name = "Datacolor SpyderCheckr 48 after 2018",
                                  .author = "Aurélien PIERRE",
                                  .date = "dec, 9 2016",
                                  .manufacturer = "DataColor",
                                  .type = COLOR_CHECKER_SPYDER_48_V2,
                                  .ratio = 2.f / 3.f,
                                  .radius = 0.035,
                                  .patches = 48,
                                  .size = { 8, 6 },
                                  .middle_grey = 24,
                                  .white = 21,
                                  .black = 29,
                                  .values = { { "A1", { 61.35,  34.81,  18.38 }, { 0.071, 0.107 } },
                                              { "A2", { 75.50 ,  5.84,  50.42 }, { 0.071, 0.264 } },
                                              { "A3", { 66.82,  -25.1,  23.47 }, { 0.071, 0.421 } },
                                              { "A4", { 60.53, -22.62, -20.40 }, { 0.071, 0.579 } },
                                              { "A5", { 59.66,  -2.03, -28.46 }, { 0.071, 0.736 } },
                                              { "A6", { 59.15,  30.83,  -5.72 }, { 0.071, 0.893 } },
                                              { "B1", { 82.68,   5.03,   3.02 }, { 0.175, 0.107 } },
                                              { "B2", { 82.25,  -2.42,   3.78 }, { 0.175, 0.264 } },
                                              { "B3", { 82.29,   2.20,  -2.04 }, { 0.175, 0.421 } },
                                              { "B4", { 24.89,   4.43,   0.78 }, { 0.175, 0.579 } },
                                              { "B5", { 25.16,  -3.88,   2.13 }, { 0.175, 0.736 } },
                                              { "B6", { 26.13,   2.61,  -5.03 }, { 0.175, 0.893 } },
                                              { "C1", { 85.42,   9.41,  14.49 }, { 0.279, 0.107 } },
                                              { "C2", { 74.28,   9.05,  27.21 }, { 0.279, 0.264 } },
                                              { "C3", { 64.57,  12.39,  37.24 }, { 0.279, 0.421 } },
                                              { "C4", { 44.49,  17.23,  26.24 }, { 0.279, 0.579 } },
                                              { "C5", { 25.29,   7.95,   8.87 }, { 0.279, 0.736 } },
                                              { "C6", { 22.67,   2.11,  -1.10 }, { 0.279, 0.893 } },
                                              { "D1", { 92.72,   1.89,   2.76 }, { 0.384, 0.107 } },
                                              { "D2", { 88.85,   1.59,   2.27 }, { 0.384, 0.264 } },
                                              { "D3", { 73.42,   0.99,   1.89 }, { 0.384, 0.421 } },
                                              { "D4", { 57.15,   0.57,   1.19 }, { 0.384, 0.579 } },
                                              { "D5", { 41.57,   0.24,   1.45 }, { 0.384, 0.736 } },
                                              { "D6", { 25.65,   1.24,   0.05 }, { 0.384, 0.893 } },
                                              { "E1", { 96.04,   2.16,   2.60 }, { 0.616, 0.107 } },
                                              { "E2", { 80.44,   1.17,   2.05 }, { 0.616, 0.264 } },
                                              { "E3", { 65.52,   0.69,   1.86 }, { 0.616, 0.421 } },
                                              { "E4", { 49.62,   0.58,   1.56 }, { 0.616, 0.579 } },
                                              { "E5", { 33.55,   0.35,   1.40 }, { 0.616, 0.736 } },
                                              { "E6", { 16.91,   1.43,  -0.81 }, { 0.616, 0.893 } },
                                              { "F1", { 47.12, -32.50, -28.75 }, { 0.721, 0.107 } },
                                              { "F2", { 50.49,  53.45, -13.55 }, { 0.721, 0.264 } },
                                              { "F3", { 83.61,   3.36,  87.02 }, { 0.721, 0.421 } },
                                              { "F4", { 41.05,  60.75,  31.17 }, { 0.721, 0.579 } },
                                              { "F5", { 54.14, -40.80,  34.75 }, { 0.721, 0.736 } },
                                              { "F6", { 24.75,  13.78, -49.48 }, { 0.721, 0.893 } },
                                              { "G1", { 60.94,  38.21,  61.31 }, { 0.825, 0.107 } },
                                              { "G2", { 37.80,   7.30, -43.04 }, { 0.825, 0.264 } },
                                              { "G3", { 49.81,  48.50,  15.76 }, { 0.825, 0.421 } },
                                              { "G4", { 28.88,  19.36, -24.48 }, { 0.825, 0.579 } },
                                              { "G5", { 72.45, -23.57,  60.47 }, { 0.825, 0.736 } },
                                              { "G6", { 71.65,  23.74,  72.28 }, { 0.825, 0.893 } },
                                              { "H1", { 70.19, -31.85,   1.98 }, { 0.929, 0.107 } },
                                              { "H2", { 54.38,   8.84, -25.71 }, { 0.929, 0.264 } },
                                              { "H3", { 42.03, -15.78,  22.93 }, { 0.929, 0.421 } },
                                              { "H4", { 48.82,  -5.11, -23.08 }, { 0.929, 0.579 } },
                                              { "H5", { 65.10,  18.14,  18.68 }, { 0.929, 0.736 } },
                                              { "H6", { 36.13,  14.15,  15.78 }, { 0.929, 0.893 } } } };


// dimensions between reference dots : 150 mm width × 116 mm height
// patch width : 12.5×12.5 mm
// outer gutter : 4 mm
// internal gutters (gap between patches) : 2.5 mm
dt_color_checker_t spyder_photo = {  .name = "Datacolor SpyderCheckr Photo",
                                  .author = "Daniel Hauck",
                                  .date = "dec, 20 2022",
                                  .manufacturer = "DataColor",
                                  .type = COLOR_CHECKER_SPYDER_PHOTO,
                                  .ratio = 106.f / 150.f,
                                  .radius = 0.059,
                                  .patches = 48,
                                  .size = { 8, 6 },
                                  .middle_grey = 24,
                                  .white = 21,
                                  .black = 29,
                                  .values = { { "A1", { 61.35,  34.81,  18.38 }, { 0.068, 0.146 } },
                                              { "A2", { 75.50 ,  5.84,  50.42 }, { 0.068, 0.288 } },
                                              { "A3", { 66.82,  -25.1,  23.47 }, { 0.068, 0.429 } },
                                              { "A4", { 60.53, -22.62, -20.40 }, { 0.068, 0.571 } },
                                              { "A5", { 59.66,  -2.03, -28.46 }, { 0.068, 0.712 } },
                                              { "A6", { 59.15,  30.83,  -5.72 }, { 0.068, 0.854 } },
                                              { "B1", { 82.68,   5.03,   3.02 }, { 0.168, 0.146 } },
                                              { "B2", { 82.25,  -2.42,   3.78 }, { 0.168, 0.288 } },
                                              { "B3", { 82.29,   2.20,  -2.04 }, { 0.168, 0.429 } },
                                              { "B4", { 24.89,   4.43,   0.78 }, { 0.168, 0.571 } },
                                              { "B5", { 25.16,  -3.88,   2.13 }, { 0.168, 0.712 } },
                                              { "B6", { 26.13,   2.61,  -5.03 }, { 0.168, 0.854 } },
                                              { "C1", { 85.42,   9.41,  14.49 }, { 0.268, 0.146 } },
                                              { "C2", { 74.28,   9.05,  27.21 }, { 0.268, 0.288 } },
                                              { "C3", { 64.57,  12.39,  37.24 }, { 0.268, 0.429 } },
                                              { "C4", { 44.49,  17.23,  26.24 }, { 0.268, 0.571 } },
                                              { "C5", { 25.29,   7.95,   8.87 }, { 0.268, 0.712 } },
                                              { "C6", { 22.67,   2.11,  -1.10 }, { 0.268, 0.854 } },
                                              { "D1", { 92.72,   1.89,   2.76 }, { 0.368, 0.146 } },
                                              { "D2", { 88.85,   1.59,   2.27 }, { 0.368, 0.288 } },
                                              { "D3", { 73.42,   0.99,   1.89 }, { 0.368, 0.429 } },
                                              { "D4", { 57.15,   0.57,   1.19 }, { 0.368, 0.571 } },
                                              { "D5", { 41.57,   0.24,   1.45 }, { 0.368, 0.712 } },
                                              { "D6", { 25.65,   1.24,   0.05 }, { 0.368, 0.854 } },
                                              { "E1", { 96.04,   2.16,   2.60 }, { 0.632, 0.146 } },
                                              { "E2", { 80.44,   1.17,   2.05 }, { 0.632, 0.288 } },
                                              { "E3", { 65.52,   0.69,   1.86 }, { 0.632, 0.429 } },
                                              { "E4", { 49.62,   0.58,   1.56 }, { 0.632, 0.571 } },
                                              { "E5", { 33.55,   0.35,   1.40 }, { 0.632, 0.712 } },
                                              { "E6", { 16.91,   1.43,  -0.81 }, { 0.632, 0.854 } },
                                              { "F1", { 47.12, -32.52, -28.75 }, { 0.732, 0.146 } },
                                              { "F2", { 50.49,  53.45, -13.55 }, { 0.732, 0.288 } },
                                              { "F3", { 83.61,   3.36,  87.02 }, { 0.732, 0.429 } },
                                              { "F4", { 41.05,  60.75,  31.17 }, { 0.732, 0.571 } },
                                              { "F5", { 54.14, -40.76,  34.75 }, { 0.732, 0.712 } },
                                              { "F6", { 24.75,  13.78, -49.48 }, { 0.732, 0.854 } },
                                              { "G1", { 60.94,  38.21,  61.31 }, { 0.832, 0.146 } },
                                              { "G2", { 37.80,   7.30, -43.04 }, { 0.832, 0.288 } },
                                              { "G3", { 49.81,  48.50,  15.76 }, { 0.832, 0.429 } },
                                              { "G4", { 28.88,  19.36, -24.48 }, { 0.832, 0.571 } },
                                              { "G5", { 72.45, -23.57,  60.47 }, { 0.832, 0.712 } },
                                              { "G6", { 71.65,  23.74,  72.28 }, { 0.832, 0.854 } },
                                              { "H1", { 70.19, -31.85,   1.98 }, { 0.932, 0.146 } },
                                              { "H2", { 54.38,   8.84, -25.71 }, { 0.932, 0.288 } },
                                              { "H3", { 42.03, -15.78,  22.93 }, { 0.932, 0.429 } },
                                              { "H4", { 48.82,  -5.11, -23.08 }, { 0.932, 0.571 } },
                                              { "H5", { 65.10,  18.14,  18.68 }, { 0.932, 0.712 } },
                                              { "H6", { 36.13,  14.15,  15.78 }, { 0.932, 0.854 } } } };

dt_color_checker_t * dt_get_color_checker(const dt_color_checker_targets target_type)
{
  switch(target_type)
  {
    case COLOR_CHECKER_XRITE_24_2000:
      return &xrite_24_2000;

    case COLOR_CHECKER_XRITE_24_2014:
      return &xrite_24_2014;

    case COLOR_CHECKER_SPYDER_24:
      return &spyder_24;

    case COLOR_CHECKER_SPYDER_24_V2:
      return &spyder_24_v2;

    case COLOR_CHECKER_SPYDER_48:
      return &spyder_48;

    case COLOR_CHECKER_SPYDER_48_V2:
      return &spyder_48_v2;

    case COLOR_CHECKER_SPYDER_PHOTO:
      return &spyder_photo;

    case COLOR_CHECKER_LAST:
      return &xrite_24_2014;
  }

  return &xrite_24_2014;
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
static inline const dt_color_checker_patch* dt_color_checker_get_patch_by_name(const dt_color_checker_t *const target_checker,
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

  if(patch == NULL)
    dt_print(DT_DEBUG_ALWAYS, "No patch matching name `%s` was found in %s\n",
             name, target_checker->name);

  if(index ) *index = idx;
  return patch;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

