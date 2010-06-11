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
 * for darktable (darktable.sf.net), so far all calculated by Pascal de Bruijn.
 */
typedef struct dt_profiled_colormatrix_t
{
  const char *makermodel;
  int rXYZ[3], gXYZ[3], bXYZ[3], white[3];
}
dt_profiled_colormatrix_t;

  // image submitter, chart type, illuminant, comments
static dt_profiled_colormatrix_t dt_profiled_colormatrices[] = {

  // Alberto Ferrante, Wolf Faust IT8, direct sunlight, well lit
  { "Canon EOS 7D",                 { 977829, 294815, -44205}, { 154175, 1238007, -325684}, {103363, -297791, 1397461}, {707291, 741760, 626251}},

  // Roy Niswanger, ColorChecker DC, direct sunlight, experimental
  { "Canon EOS 30D",                { 840195, 148773, -67017}, { 112915, 1104553, -369720}, {240005,  -19562, 1468338}, {827255, 873337, 715317}},

  // Pascal de Bruijn, CMP Digital Target 3, strobe, well lit
  { "Canon EOS 40D",                { 845901, 325760, -13077}, { 110809,  960724, -213577}, { 82230, -218063, 1110229}, {837906, 868393, 705704}},

  // Pascal de Bruijn, CMP Digital Target 3, strobe, well lit
  { "Canon EOS 50D",                {1035110, 365005,  -8057}, {-192184,  930511, -477417}, {189545, -233353, 1360870}, {863983, 888763, 730026}},

  // Pascal de Bruijn, CMP Digital Target 3, strobe, well lit
  { "Canon EOS 350D DIGITAL",       { 784348, 329681, -18875}, { 227249, 1001602, -115692}, { 23834, -270844, 1011185}, {861252, 886368, 721420}},
  { "Canon EOS DIGITAL REBEL XT",   { 784348, 329681, -18875}, { 227249, 1001602, -115692}, { 23834, -270844, 1011185}, {861252, 886368, 721420}},
  { "Canon EOS Kiss Digital N",     { 784348, 329681, -18875}, { 227249, 1001602, -115692}, { 23834, -270844, 1011185}, {861252, 886368, 721420}},

  // Pascal de Bruijn, CMP Digital Target 3, strobe, well lit
  { "Canon EOS 400D DIGITAL",       { 743546, 283783, -16647}, { 256531, 1035355, -117432}, { 36560, -256836, 1013535}, {855698, 880066, 726181}},
  { "Canon EOS DIGITAL REBEL XTi",  { 743546, 283783, -16647}, { 256531, 1035355, -117432}, { 36560, -256836, 1013535}, {855698, 880066, 726181}},
  { "Canon EOS Kiss Digital X",     { 743546, 283783, -16647}, { 256531, 1035355, -117432}, { 36560, -256836, 1013535}, {855698, 880066, 726181}},

  // Pascal de Bruijn, CMP Digital Target 3, strobe, well lit
  { "Canon EOS 450D",               { 960098, 404968,  22842}, { -85114,  855072, -310928}, {159851, -194611, 1164276}, {851379, 871506, 711823}},
  { "Canon EOS DIGITAL REBEL XSi",  { 960098, 404968,  22842}, { -85114,  855072, -310928}, {159851, -194611, 1164276}, {851379, 871506, 711823}},
  { "Canon EOS Kiss Digital X2",    { 960098, 404968,  22842}, { -85114,  855072, -310928}, {159851, -194611, 1164276}, {851379, 871506, 711823}},

  // Artis Rozentals, Wolf Faust IT8, direct sunlight, well lit
  { "Canon PowerShot S60",          { 879990, 321808,  23041}, { 272324, 1104752, -410950}, { 75500, -184097, 1373230}, {702026, 740524, 622131}},

  // Pascal de Bruijn, CMP Digital Target 3, strobe, well lit
  { "Canon PowerShot S90",          { 866531, 231995,  55756}, {  76965, 1067474, -461502}, {106369, -243286, 1314529}, {807449, 855270, 690750}},

  // Richard Hughes, Homebrew ColorChecker, strobe, experimental
  { "NIKON D60",                    { 756927, 309906, -13000}, { 323959, 1038269,  -66376}, { 51697, -178635, 1044418}, {609894, 636215, 537338}},

  // Rolf Steinort, Wolf Faust IT8, direct sunlight, well lit
  { "NIKON D200",                   { 878922, 352966,   2914}, { 273575, 1048141, -116302}, { 61661, -171021, 1126297}, {691483, 727142, 615204}},

  // Alexander Rabtchevich, Wolf Faust IT8, direct sunlight, well lit
  { "SONY DSLR-A200",               { 846786, 366302, -22858}, { 311584, 1046249, -107056}, { 54596, -192993, 1191406}, {708405, 744507, 596771}}

};

static const int dt_profiled_colormatrix_cnt = sizeof(dt_profiled_colormatrices)/sizeof(dt_profiled_colormatrix_t);

