/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
 * this is a collection of custom measured color matrices, profiled
 * for darktable (darktable.sf.net), so far all contributed by Pascal de Bruijn.
 */
typedef struct dt_profiled_colormatrix_t
{
  const char *makermodel;
  int rXYZ[3], gXYZ[3], bXYZ[3], white[3];
}
dt_profiled_colormatrix_t;

static dt_profiled_colormatrix_t dt_profiled_colormatrices[] = {
 { "Canon EOS 30D",          {8401, 1487, -670}, {1129, 11045, -3697}, {2400, -195, 14683},  {8272, 8733, 7153}},
 { "Canon EOS 50D",          {10351, 3650, -80}, {-1921, 9305, -4774}, {1895, -2333, 13608}, {8639, 8887, 7300}},
 { "Canon EOS 350D DIGITAL", {7843, 3296, -188}, {2272, 10016, -1156}, {238, -2708, 10111},  {8612, 8863, 7214}},
 { "Canon EOS 400D DIGITAL", {743546, 283783, -16647}, {256531, 1035355, -117432}, {36560, -256836, 1013535}, {855698, 880066, 726181}},
 { "Canon EOS 450D",         {9600, 4049, 228},  {-851, 8550, -3109},  {1598, -1946, 11642}, {8513, 8715, 7118}},
 { "Canon PowerShot S90",    {8665, 2319, 557},  {769, 10674, -4615},  {1063, -2432, 13145}, {8074, 8552, 6907}}
 };

static const int dt_profiled_colormatrix_cnt = sizeof(dt_profiled_colormatrices)/sizeof(dt_profiled_colormatrix_t);

