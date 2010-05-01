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
 { "Canon EOS 30D",          { 840195, 148773, -67017}, { 112915, 1104553, -369720}, {240005,  -19562, 1468338}, {827255, 873337, 715317}},   // experimental
 { "Canon EOS 40D",          { 845901, 325760, -13077}, { 110809,  960724, -213577}, { 82230, -218063, 1110229}, {837906, 868393, 705704}},
 { "Canon EOS 50D",          {1035110, 365005,  -8057}, {-192184,  930511, -477417}, {189545, -233353, 1360870}, {863983, 888763, 730026}},
 { "Canon EOS 350D DIGITAL", { 784348, 329681, -18875}, { 227249, 1001602, -115692}, { 23834, -270844, 1011185}, {861252, 886368, 721420}},
 { "Canon EOS 400D DIGITAL", { 743546, 283783, -16647}, { 256531, 1035355, -117432}, { 36560, -256836, 1013535}, {855698, 880066, 726181}},
 { "Canon EOS 450D",         { 960098, 404968,  22842}, { -85114,  855072, -310928}, {159851, -194611, 1164276}, {851379, 871506, 711823}},
 { "Canon PowerShot S90",    { 866531, 231995,  55756}, {  76965, 1067474, -461502}, {106369, -243286, 1314529}, {807449, 855270, 690750}},
 { "NIKON D60",              { 756927, 309906, -13000}, { 323959, 1038269,  -66376}, { 51697, -178635, 1044418}, {609894, 636215, 537338}},   // experimental
 { "SONY DSLR-A200",         { 846786, 366302, -22858}, { 311584, 1046249, -107056}, { 54596, -192993, 1191406}, {708405, 744507, 596771}}
 };

static const int dt_profiled_colormatrix_cnt = sizeof(dt_profiled_colormatrices)/sizeof(dt_profiled_colormatrix_t);

