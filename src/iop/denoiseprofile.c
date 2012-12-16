/*
    This file is part of darktable,
    copyright (c) 2011 johannes hanika.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "bauhaus/bauhaus.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "gui/presets.h"
#include "gui/gtk.h"
#include "common/opencl.h"
#include <gtk/gtk.h>
#include <stdlib.h>
#include <xmmintrin.h>

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE(1)

typedef struct dt_iop_nlmeans_params_t
{
  float radius;      // search radius
  float strength;    // noise level after equilization
  float a[3], b[3];  // fit for poissonian-gaussian noise per color channel.
}
dt_iop_nlmeans_params_t;

typedef struct dt_iop_nlmeans_gui_data_t
{
  GtkWidget *radius;
  GtkWidget *strength;
}
dt_iop_nlmeans_gui_data_t;

typedef dt_iop_nlmeans_params_t dt_iop_nlmeans_data_t;

const char *name()
{
  return _("denoise (profiled)");
}

int
groups ()
{
  return IOP_GROUP_CORRECT;
}

int
flags ()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

typedef struct noiseprofile_t
{
  const char *name;
  const char *maker;
  const char *model;
  int iso_min, iso_max;
  dt_iop_nlmeans_params_t params;
}
noiseprofile_t;

static const noiseprofile_t noiseprofiles[] = {
// preset name                   maker    model            iso range      rad   tune   a a a                                 b b b 
{N_("generic poissonian"),       "",      "",              0, 0,         {1.0f, 1.0f, {0.0001f, 0.0001f, 0.0001},           {0.0f, 0.0f, 0.0f}}},

  // Canon EOS 20D, contributed by Jean-Sébastien Pédron <jean-sebastien.pedron@dumbbell.fr>.
{N_("canon eos 20d iso 100"),      "Canon", "EOS_20D",  100,  100, {1.0f, 1.0f, {3.49945097782558e-06, 3.49945097782558e-06, 3.49945097782558e-06}, {-1.83310906811176e-07, -1.83310906811176e-07, -1.83310906811176e-07}}},
{N_("canon eos 20d iso 200"),      "Canon", "EOS 20D",  200,  200, {1.0f, 1.0f, {5.92880023553901e-06, 5.92880023553901e-06, 5.92880023553901e-06}, {-3.28519463926131e-07, -3.28519463926131e-07, -3.28519463926131e-07}}},
{N_("canon eos 20d iso 400"),      "Canon", "EOS 20D",  400,  400, {1.0f, 1.0f, {9.65526122541132e-06, 9.65526122541132e-06, 9.65526122541132e-06}, {-3.17639857708167e-07, -3.17639857708167e-07, -3.17639857708167e-07}}},
{N_("canon eos 20d iso 800"),      "Canon", "EOS 20D",  800,  800, {1.0f, 1.0f, {1.72686069580337e-05, 1.72686069580337e-05, 1.72686069580337e-05}, {-3.01878906022867e-07, -3.01878906022867e-07, -3.01878906022867e-07}}},
{N_("canon eos 20d iso 1600"),     "Canon", "EOS 20D", 1600, 1600, {1.0f, 1.0f, {3.27651360244258e-05, 3.27651360244258e-05, 3.27651360244258e-05}, {-4.29583314312522e-07, -4.29583314312522e-07, -4.29583314312522e-07}}},
{N_("canon eos 20d iso 3200 (H)"), "Canon", "EOS 20D", 3200, 3200, {1.0f, 1.0f, {6.24125498832736e-05, 6.24125498832736e-05, 6.24125498832736e-05}, {-1.36259041568833e-07, -1.36259041568833e-07, -1.36259041568833e-07}}},

  // jo's 5dm2 presets:
{N_("canon eos 5dm2 iso 100"),   "Canon", "EOS 5D Mark II", 0,   150,     {1.0f, 1.0f, {2.83158285671061e-06, 2.83158285671061e-06, 2.83158285671061e-06}, {-2.03690658213514e-07, -2.03690658213514e-07, -2.03690658213514e-07}}},
{N_("canon eos 5dm2 iso 200"),   "Canon", "EOS 5D Mark II", 150, 300,     {1.0f, 1.0f, {4.13958998800824e-06, 4.13958998800824e-06, 4.13958998800824e-06}, {-2.21304398662743e-07, -2.21304398662743e-07, -2.21304398662743e-07}}},
{N_("canon eos 5dm2 iso 400"),   "Canon", "EOS 5D Mark II", 300, 600,     {1.0f, 1.0f, {7.24049540870435e-06, 7.24049540870435e-06, 7.24049540870435e-06}, {-3.02238583363423e-07, -3.02238583363423e-07, -3.02238583363423e-07}}},
{N_("canon eos 5dm2 iso 800"),   "Canon", "EOS 5D Mark II", 600, 1200,    {1.0f, 1.0f, {1.31341686272862e-05, 1.31341686272862e-05, 1.31341686272862e-05}, {-4.6914485275861e-07, -4.6914485275861e-07, -4.6914485275861e-07}}},
{N_("canon eos 5dm2 iso 1600"),  "Canon", "EOS 5D Mark II", 1200, 2400,   {1.0f, 1.0f, {2.50463590614947e-05, 2.50463590614947e-05, 2.50463590614947e-05}, {-7.3061992822639e-07, -7.3061992822639e-07, -7.3061992822639e-07}}},
{N_("canon eos 5dm2 iso 3200"),  "Canon", "EOS 5D Mark II", 2400, 4800,   {1.0f, 1.0f, {4.33825185453575e-05, 4.33825185453575e-05, 4.33825185453575e-05}, {-5.10134456111927e-07, -5.10134456111927e-07, -5.10134456111927e-07}}},
{N_("canon eos 5dm2 iso 6400"),  "Canon", "EOS 5D Mark II", 4800, 9600,   {1.0f, 1.0f, {7.97916401445384e-05, 7.97916401445384e-05, 7.97916401445384e-05}, {4.19094215400523e-07, 4.19094215400523e-07, 4.19094215400523e-07}}},
{N_("canon eos 5dm2 iso 12800"), "Canon", "EOS 5D Mark II", 9600, 19600,  {1.0f, 1.0f, {0.000162588350196486, 0.000162588350196486, 0.000162588350196486}, {4.536854078923e-07, 4.536854078923e-07, 4.536854078923e-07}}},
{N_("canon eos 5dm2 iso 25600"), "Canon", "EOS 5D Mark II", 19600, 102400,{1.0f, 1.0f, {0.000315319994328854, 0.000315319994328854, 0.000315319994328854}, {9.01403346714491e-06, 9.01403346714491e-06, 9.01403346714491e-06}}},

// sony a700 contributed by wolf kuehnel
{N_("sony dslr a700 iso 100"),   "SONY", "DSLR-A700", 100, 100,         {1.0f, 1.0f, {5.33211796567828e-06, 5.33211796567828e-06, 5.33211796567828e-06}, {-8.06059627840588e-08, -8.06059627840588e-08, -8.06059627840588e-08}}},
{N_("sony dslr a700 iso 125"),   "SONY", "DSLR-A700", 125, 125,         {1.0f, 1.0f, {6.56375123512217e-06, 6.56375123512217e-06, 6.56375123512217e-06}, {-2.55641591259403e-08, -2.55641591259403e-08, -2.55641591259403e-08}}},
{N_("sony dslr a700 iso 160"),   "SONY", "DSLR-A700", 160, 160,         {1.0f, 1.0f, {7.14628696578123e-06, 7.14628696578123e-06, 7.14628696578123e-06}, {1.35993082026043e-08, 1.35993082026043e-08, 1.35993082026043e-08}}},
{N_("sony dslr a700 iso 200"),   "SONY", "DSLR-A700", 200, 200,         {1.0f, 1.0f, {6.35083595400887e-06, 6.35083595400887e-06, 6.35083595400887e-06}, {-1.62812666388806e-07, -1.62812666388806e-07, -1.62812666388806e-07}}},
{N_("sony dslr a700 iso 250"),   "SONY", "DSLR-A700", 250, 250,         {1.0f, 1.0f, {7.01839295035388e-06, 7.01839295035388e-06, 7.01839295035388e-06}, {5.3500142820507e-08, 5.3500142820507e-08, 5.3500142820507e-08}}},
{N_("sony dslr a700 iso 320"),   "SONY", "DSLR-A700", 320, 320,         {1.0f, 1.0f, {9.65036774717935e-06, 9.65036774717935e-06, 9.65036774717935e-06}, {2.45331447348584e-07, 2.45331447348584e-07, 2.45331447348584e-07}}},
{N_("sony dslr a700 iso 400"),   "SONY", "DSLR-A700", 400, 400,         {1.0f, 1.0f, {1.20060675549046e-05, 1.20060675549046e-05, 1.20060675549046e-05}, {3.96465116898102e-07, 3.96465116898102e-07, 3.96465116898102e-07}}},
{N_("sony dslr a700 iso 500"),   "SONY", "DSLR-A700", 500, 500,         {1.0f, 1.0f, {1.38033057254465e-05, 1.38033057254465e-05, 1.38033057254465e-05}, {3.55894028715277e-07, 3.55894028715277e-07, 3.55894028715277e-07}}},
{N_("sony dslr a700 iso 640"),   "SONY", "DSLR-A700", 640, 640,         {1.0f, 1.0f, {1.6784463838112e-05, 1.6784463838112e-05, 1.6784463838112e-05}, {2.28180575432325e-07, 2.28180575432325e-07, 2.28180575432325e-07}}},
{N_("sony dslr a700 iso 800"),   "SONY", "DSLR-A700", 800, 800,         {1.0f, 1.0f, {1.96000303340923e-05, 1.96000303340923e-05, 1.96000303340923e-05}, {1.84656393940007e-07, 1.84656393940007e-07, 1.84656393940007e-07}}},
{N_("sony dslr a700 iso 1000"),  "SONY", "DSLR-A700", 1000, 1000,       {1.0f, 1.0f, {2.39942978637023e-05, 2.39942978637023e-05, 2.39942978637023e-05}, {1.30423876463074e-07, 1.30423876463074e-07, 1.30423876463074e-07}}},
{N_("sony dslr a700 iso 1250"),  "SONY", "DSLR-A700", 1250, 1250,       {1.0f, 1.0f, {2.99414647800315e-05, 2.99414647800315e-05, 2.99414647800315e-05}, {1.25851775185069e-07, 1.25851775185069e-07, 1.25851775185069e-07}}},
{N_("sony dslr a700 iso 1600"),  "SONY", "DSLR-A700", 1600, 1600,       {1.0f, 1.0f, {2.79523297751563e-05, 2.79523297751563e-05, 2.79523297751563e-05}, {-6.95836889595576e-07, -6.95836889595576e-07, -6.95836889595576e-07}}},
{N_("sony dslr a700 iso 2000"),  "SONY", "DSLR-A700", 2000, 2000,       {1.0f, 1.0f, {3.43885587172716e-05, 3.43885587172716e-05, 3.43885587172716e-05}, {-1.18312709804912e-06, -1.18312709804912e-06, -1.18312709804912e-06}}},
{N_("sony dslr a700 iso 2500"),  "SONY", "DSLR-A700", 2500, 2500,       {1.0f, 1.0f, {4.243083672372e-05, 4.243083672372e-05, 4.243083672372e-05}, {-7.21932721245921e-07, -7.21932721245921e-07, -7.21932721245921e-07}}},
{N_("sony dslr a700 iso 3200"),  "SONY", "DSLR-A700", 3200, 3200,       {1.0f, 1.0f, {5.30312547388233e-05, 5.30312547388233e-05, 5.30312547388233e-05}, {-1.66425315126866e-07, -1.66425315126866e-07, -1.66425315126866e-07}}},
{N_("sony dslr a700 iso 4000"),  "SONY", "DSLR-A700", 4000, 4000,       {1.0f, 1.0f, {4.12115852399434e-05, 4.12115852399434e-05, 4.12115852399434e-05}, {-1.48982485243939e-06, -1.48982485243939e-06, -1.48982485243939e-06}}},
{N_("sony dslr a700 iso 5000"),  "SONY", "DSLR-A700", 5000, 5000,       {1.0f, 1.0f, {6.68026492277847e-05, 6.68026492277847e-05, 6.68026492277847e-05}, {-3.95313049198595e-06, -3.95313049198595e-06, -3.95313049198595e-06}}},
{N_("sony dslr a700 iso 6400"),  "SONY", "DSLR-A700", 6400, 6400,       {1.0f, 1.0f, {9.75808014924563e-05, 9.75808014924563e-05, 9.75808014924563e-05}, {-4.76703830629088e-06, -4.76703830629088e-06, -4.76703830629088e-06}}},

  // pentax k-5 contributed by elliott
{N_("pentax k-5 iso 80"),    "PENTAX", "K-5", 0, 90,         {1.0f, 1.0f, {2.03116436785254e-06, 2.03116436785254e-06, 2.03116436785254e-06}, {-3.02041906259105e-08, -3.02041906259105e-08, -3.02041906259105e-08}}},
{N_("pentax k-5 iso 100"),   "PENTAX", "K-5", 90, 150,       {1.0f, 1.0f, {2.24972405550644e-06, 2.24972405550644e-06, 2.24972405550644e-06}, {-1.73907843875546e-08, -1.73907843875546e-08, -1.73907843875546e-08}}},
{N_("pentax k-5 iso 200"),   "PENTAX", "K-5", 150, 300,      {1.0f, 1.0f, {4.35081569929625e-06, 4.35081569929625e-06, 4.35081569929625e-06}, {-3.53505222478958e-08, -3.53505222478958e-08, -3.53505222478958e-08}}},
{N_("pentax k-5 iso 400"),   "PENTAX", "K-5", 300, 600,      {1.0f, 1.0f, {8.61579206202407e-06, 8.61579206202407e-06, 8.61579206202407e-06}, {-5.77072212691143e-08, -5.77072212691143e-08, -5.77072212691143e-08}}},
{N_("pentax k-5 iso 800"),   "PENTAX", "K-5", 600, 1200,     {1.0f, 1.0f, {1.71175144229784e-05, 1.71175144229784e-05, 1.71175144229784e-05}, {-1.46590586063609e-08, -1.46590586063609e-08, -1.46590586063609e-08}}},
{N_("pentax k-5 iso 1600"),  "PENTAX", "K-5", 1200, 2400,    {1.0f, 1.0f, {3.2697044230099e-05, 3.2697044230099e-05, 3.2697044230099e-05}, {3.31216462704584e-08, 3.31216462704584e-08, 3.31216462704584e-08}}},
{N_("pentax k-5 iso 3200"),  "PENTAX", "K-5", 2400, 4800,    {1.0f, 1.0f, {4.43416642030573e-05, 4.43416642030573e-05, 4.43416642030573e-05}, {-4.24937607243593e-07, -4.24937607243593e-07, -4.24937607243593e-07}}},
{N_("pentax k-5 iso 6400"),  "PENTAX", "K-5", 4800, 9600,    {1.0f, 1.0f, {8.71489939508059e-05, 8.71489939508059e-05, 8.71489939508059e-05}, {-1.07266764686662e-06, -1.07266764686662e-06, -1.07266764686662e-06}}},
{N_("pentax k-5 iso 12800"), "PENTAX", "K-5", 9600, 19600,   {1.0f, 1.0f, {0.000164985441637024, 0.000164985441637024, 0.000164985441637024}, {-9.70017976316732e-09, -9.70017976316732e-09, -9.70017976316732e-09}}},
{N_("pentax k-5 iso 25600"), "PENTAX", "K-5", 19600, 35800,  {1.0f, 1.0f, {0.000306721894329492, 0.000306721894329492, 0.000306721894329492}, {7.01957276944239e-06, 7.01957276944239e-06, 7.01957276944239e-06}}},
{N_("pentax k-5 iso 51200"), "PENTAX", "K-5", 35800, 102400, {1.0f, 1.0f, {0.000576230159234024, 0.000576230159234024, 0.000576230159234024}, {4.78351150568581e-05, 4.78351150568581e-05, 4.78351150568581e-05}}},

  // sony nex 6, contributed by christophe vescovi (same sensor: nex-5r nex-f3)
{N_("sony nex 6 iso 100"),   "SONY", "NEX-",  0, 0,         {1.0f, 1.0f, {2.83070523378633e-06, 2.83070523378633e-06, 2.83070523378633e-06}, {-8.99579441370004e-08, -8.99579441370004e-08, -8.99579441370004e-08}}},
{N_("sony nex 6 iso 200"),   "SONY", "NEX-",  0, 0,         {1.0f, 1.0f, {4.41699217331605e-06, 4.41699217331605e-06, 4.41699217331605e-06}, {-5.73326494093182e-08, -5.73326494093182e-08, -5.73326494093182e-08}}},
{N_("sony nex 6 iso 400"),   "SONY", "NEX-",  0, 0,         {1.0f, 1.0f, {7.95016906656091e-06, 7.95016906656091e-06, 7.95016906656091e-06}, {3.77445636379495e-09, 3.77445636379495e-09, 3.77445636379495e-09}}},
{N_("sony nex 6 iso 800"),   "SONY", "NEX-",  0, 0,         {1.0f, 1.0f, {1.55977208319835e-05, 1.55977208319835e-05, 1.55977208319835e-05}, {-6.32291419219102e-08, -6.32291419219102e-08, -6.32291419219102e-08}}},
{N_("sony nex 6 iso 1600"),  "SONY", "NEX-",  0, 0,         {1.0f, 1.0f, {2.98162473611846e-05, 2.98162473611846e-05, 2.98162473611846e-05}, {4.43033418340148e-08, 4.43033418340148e-08, 4.43033418340148e-08}}},
{N_("sony nex 6 iso 3200"),  "SONY", "NEX-",  0, 0,         {1.0f, 1.0f, {5.84783565575537e-05, 5.84783565575537e-05, 5.84783565575537e-05}, {2.75198472414243e-07, 2.75198472414243e-07, 2.75198472414243e-07}}},
{N_("sony nex 6 iso 6400"),  "SONY", "NEX-",  0, 0,         {1.0f, 1.0f, {0.000117797177674452, 0.000117797177674452, 0.000117797177674452}, {9.27480448891856e-07, 9.27480448891856e-07, 9.27480448891856e-07}}},
{N_("sony nex 6 iso 12800"), "SONY", "NEX-",  0, 0,         {1.0f, 1.0f, {0.000234877764709295, 0.000234877764709295, 0.000234877764709295}, {4.23945931130405e-06, 4.23945931130405e-06, 4.23945931130405e-06}}},
{N_("sony nex 6 iso 25600"), "SONY", "NEX-",  0, 0,         {1.0f, 1.0f, {0.000427503080952746, 0.000427503080952746, 0.000427503080952746}, {2.89795299259573e-05, 2.89795299259573e-05, 2.89795299259573e-05}}},

// pentax k100d contributed by elliott
{N_("pentax k100d iso 200"),  "PENTAX", "K100D", 0, 0,         {1.0f, 1.0f, {3.95804544331784e-06, 3.95804544331784e-06, 3.95804544331784e-06}, {-1.69302519603735e-07, -1.69302519603735e-07, -1.69302519603735e-07}}},
{N_("pentax k100d iso 400"),  "PENTAX", "K100D", 0, 0,         {1.0f, 1.0f, {5.20488498776622e-06, 5.20488498776622e-06, 5.20488498776622e-06}, {-1.63022469587059e-07, -1.63022469587059e-07, -1.63022469587059e-07}}},
{N_("pentax k100d iso 800"),  "PENTAX", "K100D", 0, 0,         {1.0f, 1.0f, {9.25286082752558e-06, 9.25286082752558e-06, 9.25286082752558e-06}, {-1.28100505227757e-07, -1.28100505227757e-07, -1.28100505227757e-07}}},
{N_("pentax k100d iso 1600"), "PENTAX", "K100D", 0, 0,         {1.0f, 1.0f, {1.65249296500756e-05, 1.65249296500756e-05, 1.65249296500756e-05}, {5.07468502484445e-07, 5.07468502484445e-07, 5.07468502484445e-07}}},
{N_("pentax k100d iso 3200"), "PENTAX", "K100D", 0, 0,         {1.0f, 1.0f, {3.31313403159836e-05, 3.31313403159836e-05, 3.31313403159836e-05}, {1.92154574341619e-06, 1.92154574341619e-06, 1.92154574341619e-06}}},

// olympus e-pl1 contributed by José Carlos R. Casimiro
{N_("olympus e-pl1 iso 100"),  "Olympus", "E-PL1", 0, 0, {1.0f, 1.0f, {6.1332646664107e-06, 6.1332646664107e-06, 6.1332646664107e-06}, {-1.70427018703982e-07, -1.70427018703982e-07, -1.70427018703982e-07}}},
{N_("olympus e-pl1 iso 200"),  "Olympus", "E-PL1", 0, 0, {1.0f, 1.0f, {6.17771627349177e-06, 6.17771627349177e-06, 6.17771627349177e-06}, {-1.94243887646926e-07, -1.94243887646926e-07, -1.94243887646926e-07}}},
{N_("olympus e-pl1 iso 400"),  "Olympus", "E-PL1", 0, 0, {1.0f, 1.0f, {1.00980942726634e-05, 1.00980942726634e-05, 1.00980942726634e-05}, {-8.10099319236512e-08, -8.10099319236512e-08, -8.10099319236512e-08}}},
{N_("olympus e-pl1 iso 800"),  "Olympus", "E-PL1", 0, 0, {1.0f, 1.0f, {1.78191571180193e-05, 1.78191571180193e-05, 1.78191571180193e-05}, {3.41992783976417e-07, 3.41992783976417e-07, 3.41992783976417e-07}}},
{N_("olympus e-pl1 iso 1600"), "Olympus", "E-PL1", 0, 0, {1.0f, 1.0f, {1.78191571180193e-05, 1.78191571180193e-05, 1.78191571180193e-05}, {3.41992783976417e-07, 3.41992783976417e-07, 3.41992783976417e-07}}},
{N_("olympus e-pl1 iso 3200"), "Olympus", "E-PL1", 0, 0, {1.0f, 1.0f, {6.38579880360713e-05, 6.38579880360713e-05, 6.38579880360713e-05}, {6.71632186716273e-06, 6.71632186716273e-06, 6.71632186716273e-06}}},

// canon eos 550D, christian iuga, slightly under exposed blue channel (green should be fine)
{N_("canon eos 550d iso 100"),  "Canon", "550D", 0, 0, {1.0f, 1.0f, {1.71861838866083e-10, 1.71861838866083e-10, 1.71861838866083e-10}, {1.69241651301892e-11, 1.69241651301892e-11, 1.69241651301892e-11}}},
{N_("canon eos 550d iso 200"),  "Canon", "550D", 0, 0, {1.0f, 1.0f, {6.17851997634973e-10, 6.17851997634973e-10, 6.17851997634973e-10}, {-4.58710373121214e-11, -4.58710373121214e-11, -4.58710373121214e-11}}},
{N_("canon eos 550d iso 400"),  "Canon", "550D", 0, 0, {1.0f, 1.0f, {1.62212013335921e-09, 1.62212013335921e-09, 1.62212013335921e-09}, {-1.74334133812383e-10, -1.74334133812383e-10, -1.74334133812383e-10}}},
{N_("canon eos 550d iso 800"),  "Canon", "550D", 0, 0, {1.0f, 1.0f, {1.0571486846413e-08, 1.0571486846413e-08, 1.0571486846413e-08}, {-1.55114694067052e-09, -1.55114694067052e-09, -1.55114694067052e-09}}},
{N_("canon eos 550d iso 1600"), "Canon", "550D", 0, 0, {1.0f, 1.0f, {2.60199856635175e-07, 2.60199856635175e-07, 2.60199856635175e-07}, {-2.00646439729195e-08, -2.00646439729195e-08, -2.00646439729195e-08}}},
{N_("canon eos 550d iso 3200"), "Canon", "550D", 0, 0, {1.0f, 1.0f, {1.999492725088e-06, 1.999492725088e-06, 1.999492725088e-06}, {-4.80149993523279e-08, -4.80149993523279e-08, -4.80149993523279e-08}}},
{N_("canon eos 550d iso 6400"), "Canon", "550D", 0, 0, {1.0f, 1.0f, {1.34678479698392e-05, 1.34678479698392e-05, 1.34678479698392e-05}, {4.38558690325131e-07, 4.38558690325131e-07, 4.38558690325131e-07}}},

// nikon d5000, contributed by Raphaël Monnard
{N_("nikon d5000 iso 100"),  "NIKON", "D5000", 0, 0, {1.0f, 1.0f, {3.55434034592321e-06, 3.55434034592321e-06, 3.55434034592321e-06}, {-1.03227712861061e-07, -1.03227712861061e-07, -1.03227712861061e-07}}},
{N_("nikon d5000 iso 125"),  "NIKON", "D5000", 0, 0, {1.0f, 1.0f, {3.72492240851358e-06, 3.72492240851358e-06, 3.72492240851358e-06}, {-1.35521958369885e-07, -1.35521958369885e-07, -1.35521958369885e-07}}},
{N_("nikon d5000 iso 160"),  "NIKON", "D5000", 0, 0, {1.0f, 1.0f, {3.29225795293138e-06, 3.29225795293138e-06, 3.29225795293138e-06}, {8.48279202960177e-09, 8.48279202960177e-09, 8.48279202960177e-09}}},
{N_("nikon d5000 iso 200"),  "NIKON", "D5000", 0, 0, {1.0f, 1.0f, {3.59345364056741e-06, 3.59345364056741e-06, 3.59345364056741e-06}, {6.12980277963006e-08, 6.12980277963006e-08, 6.12980277963006e-08}}},
{N_("nikon d5000 iso 250"),  "NIKON", "D5000", 0, 0, {1.0f, 1.0f, {4.15439500131005e-06, 4.15439500131005e-06, 4.15439500131005e-06}, {1.21916079668766e-07, 1.21916079668766e-07, 1.21916079668766e-07}}},
{N_("nikon d5000 iso 320"),  "NIKON", "D5000", 0, 0, {1.0f, 1.0f, {5.21102878857756e-06, 5.21102878857756e-06, 5.21102878857756e-06}, {8.84574252141181e-08, 8.84574252141181e-08, 8.84574252141181e-08}}},
{N_("nikon d5000 iso 400"),  "NIKON", "D5000", 0, 0, {1.0f, 1.0f, {6.68065899855839e-06, 6.68065899855839e-06, 6.68065899855839e-06}, {1.50637387918195e-07, 1.50637387918195e-07, 1.50637387918195e-07}}},
{N_("nikon d5000 iso 500"),  "NIKON", "D5000", 0, 0, {1.0f, 1.0f, {8.43738532308592e-06, 8.43738532308592e-06, 8.43738532308592e-06}, {1.9887353081052e-07, 1.9887353081052e-07, 1.9887353081052e-07}}},
{N_("nikon d5000 iso 640"),  "NIKON", "D5000", 0, 0, {1.0f, 1.0f, {1.01204931421007e-05, 1.01204931421007e-05, 1.01204931421007e-05}, {3.25653266866488e-07, 3.25653266866488e-07, 3.25653266866488e-07}}},
{N_("nikon d5000 iso 800"),  "NIKON", "D5000", 0, 0, {1.0f, 1.0f, {1.3503191058199e-05, 1.3503191058199e-05, 1.3503191058199e-05}, {4.18411101301127e-07, 4.18411101301127e-07, 4.18411101301127e-07}}},
{N_("nikon d5000 iso 1000"), "NIKON", "D5000", 0, 0, {1.0f, 1.0f, {1.69052928194055e-05, 1.69052928194055e-05, 1.69052928194055e-05}, {5.44260862263629e-07, 5.44260862263629e-07, 5.44260862263629e-07}}},
{N_("nikon d5000 iso 1250"), "NIKON", "D5000", 0, 0, {1.0f, 1.0f, {2.22782069380325e-05, 2.22782069380325e-05, 2.22782069380325e-05}, {6.95430630653238e-07, 6.95430630653238e-07, 6.95430630653238e-07}}},
{N_("nikon d5000 iso 1600"), "NIKON", "D5000", 0, 0, {1.0f, 1.0f, {2.87907901583626e-05, 2.87907901583626e-05, 2.87907901583626e-05}, {9.36169085563434e-07, 9.36169085563434e-07, 9.36169085563434e-07}}},
{N_("nikon d5000 iso 2000"), "NIKON", "D5000", 0, 0, {1.0f, 1.0f, {3.39490808473876e-05, 3.39490808473876e-05, 3.39490808473876e-05}, {1.56088276996701e-06, 1.56088276996701e-06, 1.56088276996701e-06}}},
{N_("nikon d5000 iso 2500"), "NIKON", "D5000", 0, 0, {1.0f, 1.0f, {4.13170410396535e-05, 4.13170410396535e-05, 4.13170410396535e-05}, {2.45270765023671e-06, 2.45270765023671e-06, 2.45270765023671e-06}}},
{N_("nikon d5000 iso 3200"), "NIKON", "D5000", 0, 0, {1.0f, 1.0f, {4.13170410396535e-05, 4.13170410396535e-05, 4.13170410396535e-05}, {2.45270765023671e-06, 2.45270765023671e-06, 2.45270765023671e-06}}},
{N_("nikon d5000 iso 4000"), "NIKON", "D5000", 0, 0, {1.0f, 1.0f, {5.4708082446611e-05, 5.4708082446611e-05, 5.4708082446611e-05}, {6.31708282891435e-06, 6.31708282891435e-06, 6.31708282891435e-06}}},
{N_("nikon d5000 iso 5000"), "NIKON", "D5000", 0, 0, {1.0f, 1.0f, {6.49088846760222e-05, 6.49088846760222e-05, 6.49088846760222e-05}, {9.10172565593788e-06, 9.10172565593788e-06, 9.10172565593788e-06}}},
{N_("nikon d5000 iso 6400"), "NIKON", "D5000", 0, 0, {1.0f, 1.0f, {8.49174688072452e-05, 8.49174688072452e-05, 8.49174688072452e-05}, {1.20040650662599e-05, 1.20040650662599e-05, 1.20040650662599e-05}}},
};
static const int noiseprofile_cnt = sizeof(noiseprofiles)/sizeof(noiseprofile_t);

void init_presets (dt_iop_module_so_t *self)
{
  for(int i=0;i<noiseprofile_cnt;i++)
  {
    dt_gui_presets_add_generic(_(noiseprofiles[i].name), self->op, self->version(), &noiseprofiles[i].params, sizeof(dt_iop_nlmeans_params_t), 1);
    // show only for matching cameras.
    dt_gui_presets_update_mml(_(noiseprofiles[i].name), self->op, self->version(), noiseprofiles[i].maker, noiseprofiles[i].model, "");
    dt_gui_presets_update_filter(_(noiseprofiles[i].name), self->op, self->version(), 1);
    // TODO: maybe use iso setting as well.
  }
}

typedef union floatint_t
{
  float f;
  uint32_t i;
}
floatint_t;

// very fast approximation for 2^-x (returns 0 for x > 126)
static inline float
fast_mexp2f(const float x)
{
  const float i1 = (float)0x3f800000u; // 2^0
  const float i2 = (float)0x3f000000u; // 2^-1
  const float k0 = i1 + x * (i2 - i1);
  floatint_t k;
  k.i = k0 >= (float)0x800000u ? k0 : 0;
  return k.f;
}

void tiling_callback  (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, struct dt_develop_tiling_t *tiling)
{
  dt_iop_nlmeans_params_t *d = (dt_iop_nlmeans_params_t *)piece->data;
  const int P = ceilf(d->radius * roi_in->scale / piece->iscale); // pixel filter size
  const int K = ceilf(7 * roi_in->scale / piece->iscale); // nbhood

  tiling->factor = 2.5f; // in + out + tmp
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = P+K;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

static inline void
precondition(
    const float *const in,
    float *const buf,
    const int wd,
    const int ht,
    const float a[3],
    const float b[3])
{
  const float sigma2[3] = {
    (b[0]/a[0])*(b[0]/a[0]),
    (b[1]/a[1])*(b[1]/a[1]),
    (b[2]/a[1])*(b[2]/a[1])};

#ifdef _OPENMP
#  pragma omp parallel for schedule(static) default(none) shared(a)
#endif
  for(int j=0; j<ht; j++)
  {
    float *buf2 = buf + 4*j*wd;
    const float *in2 = in + 4*j*wd;
    for(int i=0;i<wd;i++)
    {
      for(int c=0;c<3;c++)
      {
        buf2[c] = in2[c] / a[c];
        const float d = fmaxf(0.0f, buf2[c] + 3./8. + sigma2[c]);
        buf2[c] = 2.0f*sqrtf(d);
      }
      buf2 += 4;
      in2 += 4;
    }
  }
}

static inline void
backtransform(
    float *const buf,
    const int wd,
    const int ht,
    const float a[3],
    const float b[3])
{
  const float sigma2[3] = {
    (b[0]/a[0])*(b[0]/a[0]),
    (b[1]/a[1])*(b[1]/a[1]),
    (b[2]/a[1])*(b[2]/a[1])};

#ifdef _OPENMP
#  pragma omp parallel for schedule(static) default(none) shared(a)
#endif
  for(int j=0; j<ht; j++)
  {
    float *buf2 = buf + 4*j*wd;
    for(int i=0;i<wd;i++)
    {
      for(int c=0;c<3;c++)
      {
        const float x = buf2[c];
        // closed form approximation to unbiased inverse (input range was 0..200 for fit, not 0..1)
        // buf2[c] = fmaxf(0.0f, 1./4.*x*x + 1./4.*sqrtf(3./2.)/x - 11./8.*1.0/(x*x) + 5./8.*sqrtf(3./2.)*1.0/(x*x*x) - 1./8. - sigma2[c]);
        // asymptotic form:
        buf2[c] = fmaxf(0.0f, 1./4.*x*x - 1./8. - sigma2[c]);
        buf2[c] *= a[c];
      }
      buf2 += 4;
    }
  }
}

#define USE_NLMEANS 1
// #define USE_WAVELETS 1
#if USE_WAVELETS == 1
// =====================================================================================
// begin wavelet code:
// =====================================================================================
#define ALIGNED(a) __attribute__((aligned(a)))
#define VEC4(a) {(a), (a), (a), (a)}

static const __m128 fone ALIGNED(16) = VEC4(0x3f800000u);
static const __m128 femo ALIGNED(16) = VEC4(0x00adf880u);
static const __m128 ooo1 ALIGNED(16) = {0.f, 0.f, 0.f, 1.f};

/* SSE intrinsics version of dt_fast_expf defined in darktable.h */
static __m128  inline
dt_fast_expf_sse(const __m128 x)
{
  __m128  f = _mm_add_ps(fone, _mm_mul_ps(x, femo)); // f(n) = i1 + x(n)*(i2-i1)
  __m128i i = _mm_cvtps_epi32(f);                    // i(n) = int(f(n))
  __m128i mask = _mm_srai_epi32(i, 31);              // mask(n) = 0xffffffff if i(n) < 0
  i = _mm_andnot_si128(mask, i);                     // i(n) = 0 if i(n) < 0
  return _mm_castsi128_ps(i);                        // return *(float*)&i
}

// FIXME: needs adjusting for this color space!
/* Computes the vector
 * (wl, wc, wc, 1)
 *
 * where:
 * wl = exp(-sharpen*SQR(c1[0] - c2[0]))
 *    = exp(-s*d1) (as noted in code comments below)
 * wc = exp(-sharpen*(SQR(c1[1] - c2[1]) + SQR(c1[2] - c2[2]))
 *    = exp(-s*(d2+d3)) (as noted in code comments below)
 */
static __m128  inline
weight_sse(const __m128 *c1, const __m128 *c2, const float sharpen)
{
  return _mm_set1_ps(1.0f);
  // TODO: expf falloff, variance aware, for [0, 1] rgb images
#if 0
  const __m128 vsharpen = _mm_set1_ps(-sharpen);  // (-s, -s, -s, -s)
  __m128 diff = _mm_sub_ps(*c1, *c2);
  __m128 square = _mm_mul_ps(diff, diff);         // (?, d3, d2, d1)
  __m128 square2 = _mm_shuffle_ps(square, square, _MM_SHUFFLE(3, 1, 2, 0)); // (?, d2, d3, d1)
  __m128 added = _mm_add_ps(square, square2);     // (?, d2+d3, d2+d3, 2*d1)
  added = _mm_sub_ss(added, square);              // (?, d2+d3, d2+d3, d1)
  __m128 sharpened = _mm_mul_ps(added, vsharpen); // (?, -s*(d2+d3), -s*(d2+d3), -s*d1)
  __m128 exp = dt_fast_expf_sse(sharpened);       // (?, wc, wc, wl)
  exp = _mm_castsi128_ps(_mm_slli_si128(_mm_castps_si128(exp), 4)); // (wc, wc, wl, 0)
  exp = _mm_castsi128_ps(_mm_srli_si128(_mm_castps_si128(exp), 4)); // (0, wc, wc, wl)
  exp = _mm_or_ps(exp, ooo1); // (1, wc, wc, wl)
  return exp;
#endif
}

#define SUM_PIXEL_CONTRIBUTION_COMMON(ii, jj) \
  do { \
    const __m128 f = _mm_set1_ps(filter[(ii)]*filter[(jj)]); \
    const __m128 wp = weight_sse(px, px2, sharpen); \
    const __m128 w = _mm_mul_ps(f, wp); \
    const __m128 pd = _mm_mul_ps(w, *px2); \
    sum = _mm_add_ps(sum, pd); \
    wgt = _mm_add_ps(wgt, w); \
  } while (0)

#define SUM_PIXEL_CONTRIBUTION_WITH_TEST(ii, jj) \
  do { \
    const int iii = (ii)-2; \
    const int jjj = (jj)-2; \
    int x = i + mult*iii; \
    int y = j + mult*jjj; \
    \
    if(x < 0)       x = 0; \
    if(x >= width)  x = width  - 1; \
    if(y < 0)       y = 0; \
    if(y >= height) y = height - 1; \
    \
    px2 = ((__m128 *)in) + x + y*width; \
    \
    SUM_PIXEL_CONTRIBUTION_COMMON(ii, jj); \
  } while (0)

#define ROW_PROLOGUE \
  const __m128 *px = ((__m128 *)in) + j*width; \
  const __m128 *px2; \
  float *pdetail = detail + 4*j*width; \
  float *pcoarse = out + 4*j*width;

#define SUM_PIXEL_PROLOGUE \
  __m128 sum = _mm_setzero_ps(); \
  __m128 wgt = _mm_setzero_ps();

#define SUM_PIXEL_EPILOGUE \
  sum = _mm_mul_ps(sum, _mm_rcp_ps(wgt)); \
  \
  _mm_stream_ps(pdetail, _mm_sub_ps(*px, sum)); \
  _mm_stream_ps(pcoarse, sum); \
  px++; \
  pdetail+=4; \
  pcoarse+=4;

static void
eaw_decompose (float *const out, const float *const in, float *const detail, const int scale,
               const float sharpen, const int32_t width, const int32_t height)
{
  const int mult = 1<<scale;
  static const float filter[5] = {1.0f/16.0f, 4.0f/16.0f, 6.0f/16.0f, 4.0f/16.0f, 1.0f/16.0f};

  /* The first "2*mult" lines use the macro with tests because the 5x5 kernel
   * requires nearest pixel interpolation for at least a pixel in the sum */
#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static)
#endif
  for (int j=0; j<2*mult; j++)
  {
    ROW_PROLOGUE

    for(int i=0; i<width; i++)
    {
      SUM_PIXEL_PROLOGUE
      for (int jj=0; jj<5; jj++)
      {
        for (int ii=0; ii<5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE
    }
  }

#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static)
#endif
  for(int j=2*mult; j<height-2*mult; j++)
  {
    ROW_PROLOGUE

    /* The first "2*mult" pixels use the macro with tests because the 5x5 kernel
     * requires nearest pixel interpolation for at least a pixel in the sum */
    for (int i=0; i<2*mult; i++)
    {
      SUM_PIXEL_PROLOGUE
      for (int jj=0; jj<5; jj++)
      {
        for (int ii=0; ii<5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE
    }

    /* For pixels [2*mult, width-2*mult], we can safely use macro w/o tests
     * to avoid uneeded branching in the inner loops */
    for(int i=2*mult; i<width-2*mult; i++)
    {
      SUM_PIXEL_PROLOGUE
      px2 = ((__m128*)in) + i-2*mult + (j-2*mult)*width;
      for (int jj=0; jj<5; jj++)
      {
        for (int ii=0; ii<5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_COMMON(ii, jj);
          px2 += mult;
        }
        px2 += (width-5)*mult;
      }
      SUM_PIXEL_EPILOGUE
    }

    /* Last two pixels in the row require a slow variant... blablabla */
    for (int i=width-2*mult; i<width; i++)
    {
      SUM_PIXEL_PROLOGUE
      for (int jj=0; jj<5; jj++)
      {
        for (int ii=0; ii<5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE
    }
  }

  /* The last "2*mult" lines use the macro with tests because the 5x5 kernel
   * requires nearest pixel interpolation for at least a pixel in the sum */
#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static)
#endif
  for (int j=height-2*mult; j<height; j++)
  {
    ROW_PROLOGUE

    for(int i=0; i<width; i++)
    {
      SUM_PIXEL_PROLOGUE
      for (int jj=0; jj<5; jj++)
      {
        for (int ii=0; ii<5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE
    }
  }

  _mm_sfence();
}

#undef SUM_PIXEL_CONTRIBUTION_COMMON
#undef SUM_PIXEL_CONTRIBUTION_WITH_TEST
#undef ROW_PROLOGUE
#undef SUM_PIXEL_PROLOGUE
#undef SUM_PIXEL_EPILOGUE

static void
eaw_synthesize (float *const out, const float *const in, const float *const detail,
                const float *thrsf, const float *boostf, const int32_t width, const int32_t height)
{
  const __m128 threshold = _mm_set_ps(thrsf[3], thrsf[2], thrsf[1], thrsf[0]);
  const __m128 boost     = _mm_set_ps(boostf[3], boostf[2], boostf[1], boostf[0]);

#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static)
#endif
  for(int j=0; j<height; j++)
  {
    // TODO: prefetch? _mm_prefetch()
    const __m128 *pin = (__m128 *)in + j*width;
    __m128 *pdetail = (__m128 *)detail + j*width;
    float *pout = out + 4*j*width;
    for(int i=0; i<width; i++)
    {
#if 1
      const __m128i maski = _mm_set1_epi32(0x80000000u);
      const __m128 *mask = (__m128*)&maski;
      const __m128 absamt = _mm_max_ps(_mm_setzero_ps(), _mm_sub_ps(_mm_andnot_ps(*mask, *pdetail), threshold));
      const __m128 amount = _mm_or_ps(_mm_and_ps(*pdetail, *mask), absamt);
      _mm_stream_ps(pout, _mm_add_ps(*pin, _mm_mul_ps(boost, amount)));
#endif
      // _mm_stream_ps(pout, _mm_add_ps(*pin, *pdetail));
      pdetail ++;
      pin ++;
      pout += 4;
    }
  }
  _mm_sfence();
}
// =====================================================================================

void process(
    struct dt_iop_module_t *self,
    dt_dev_pixelpipe_iop_t *piece,
    void *ivoid,
    void *ovoid,
    const dt_iop_roi_t *roi_in,
    const dt_iop_roi_t *roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  dt_iop_nlmeans_params_t *d = (dt_iop_nlmeans_params_t *)piece->data;

  const int max_scale = 3;

  float *buf[max_scale+2];
  for(int k=0;k<=max_scale;k++)
    buf[k] = dt_alloc_align(64, 4*sizeof(float)*roi_in->width*roi_in->height);
  buf[max_scale+1] = (float *)ovoid;
  
  const float wb[3] = {
    piece->pipe->processed_maximum[0]*d->strength,
    piece->pipe->processed_maximum[1]*d->strength,
    piece->pipe->processed_maximum[2]*d->strength};
  const float aa[3] = {
    d->a[0]*wb[0],
    d->a[1]*wb[1],
    d->a[2]*wb[2]};
  const float bb[3] = {
    d->b[0]*wb[0],
    d->b[1]*wb[1],
    d->b[2]*wb[2]};

  const int width = roi_in->width, height = roi_in->height;
  precondition((float *)ivoid, buf[max_scale], width, height, aa, bb);

  // variance stabilizing transform maps sigma to unity:
  // TODO: adjust to scale?
  const float sigma_n = 1.0f;

  float *cur = buf[max_scale];

  // buf3=in -> buf0, buf2
  // buf2    -> buf1, buf3
  // buf3    -> buf2, out

  // syn:
  // out += buf2 + buf1 + buf0

  for(int scale=0; scale<max_scale; scale++)
  {
    eaw_decompose (buf[scale+2], cur, buf[scale], scale, 0.0f, width, height);
    cur = buf[scale+2];
  }

  for(int scale=max_scale-1; scale>=0; scale--)
  {
    const float sigma = sigma_n/pow(sqrtf(2.0), scale);
    // determine thrs as bayesshrink
    // TODO: parallelize!
    float sum_y[3] = {0.0f}, sum_y2[3] = {0.0f};
    const int n = width*height;
    for(int k=0;k<n;k++)
    {
      for(int c=0;c<3;c++)
      {
        sum_y [c] += buf[scale][4*k+c];
        sum_y2[c] += buf[scale][4*k+c]*buf[scale][4*k+c];
      }
    }
    const float mean_y[3] = { sum_y[0]/n, sum_y[1]/n, sum_y[2]/n };
    const float var_y[3] = {
      sum_y2[0]/(n-1.0f) - mean_y[0]*mean_y[0],
      sum_y2[1]/(n-1.0f) - mean_y[1]*mean_y[1],
      sum_y2[2]/(n-1.0f) - mean_y[2]*mean_y[2]};
    const float std_x[3] = {
      sqrtf(MAX(0, var_y[0] - sigma*sigma)),
      sqrtf(MAX(0, var_y[1] - sigma*sigma)),
      sqrtf(MAX(0, var_y[2] - sigma*sigma))};
    // fprintf(stderr, "============level %d\n", scale);
    // fprintf(stderr, "mean y: %f %f %f\n", mean_y[0], mean_y[1], mean_y[2]);
    // fprintf(stderr, "stddev: %f %f %f\n", std_x[0], std_x[1], std_x[2]);
    const float thrs[4] = { sigma*sigma/std_x[0], sigma*sigma/std_x[1], sigma*sigma/std_x[2], 0.0f};
    // fprintf(stderr, "thrs  : %f %f %f\n", thrs[0], thrs[1], thrs[2]);
    const float boost[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    eaw_synthesize (buf[max_scale+1], buf[max_scale+1], buf[scale], thrs, boost, width, height);
  }

  backtransform((float *)ovoid, width, height, aa, bb);

  for(int k=0;k<=max_scale;k++)
    free(buf[k]);

  if(piece->pipe->mask_display)
    dt_iop_alpha_copy(ivoid, ovoid, width, height);
}
#endif

#if USE_NLMEANS == 1
void process(
    struct dt_iop_module_t *self,
    dt_dev_pixelpipe_iop_t *piece,
    void *ivoid,
    void *ovoid,
    const dt_iop_roi_t *roi_in,
    const dt_iop_roi_t *roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  dt_iop_nlmeans_params_t *d = (dt_iop_nlmeans_params_t *)piece->data;

  // TODO: fixed K to use adaptive size trading variance and bias!
  // adjust to zoom size:
  const int P = ceilf(d->radius * roi_in->scale / piece->iscale); // pixel filter size
  const int K = ceilf(7 * roi_in->scale / piece->iscale); // nbhood XXX see above comment

  // P == 0 : this will degenerate to a (fast) bilateral filter.

  float *Sa = dt_alloc_align(64, sizeof(float)*roi_out->width*dt_get_num_threads());
  // we want to sum up weights in col[3], so need to init to 0:
  memset(ovoid, 0x0, sizeof(float)*roi_out->width*roi_out->height*4);
  float *in = dt_alloc_align(64, 4*sizeof(float)*roi_in->width*roi_in->height);
  
  const float wb[3] = {
    piece->pipe->processed_maximum[0]*d->strength,
    piece->pipe->processed_maximum[1]*d->strength,
    piece->pipe->processed_maximum[2]*d->strength};
  const float aa[3] = {
    d->a[0]*wb[0],
    d->a[1]*wb[1],
    d->a[2]*wb[2]};
  const float bb[3] = {
    d->b[0]*wb[0],
    d->b[1]*wb[1],
    d->b[2]*wb[2]};
  precondition((float *)ivoid, in, roi_in->width, roi_in->height, aa, bb);

  // for each shift vector
  for(int kj=-K; kj<=K; kj++)
  {
    for(int ki=-K; ki<=K; ki++)
    {
      // TODO: adaptive K tests here!
      // TODO: expf eval for real bilateral experience :)

      int inited_slide = 0;
      // don't construct summed area tables but use sliding window! (applies to cpu version res < 1k only, or else we will add up errors)
      // do this in parallel with a little threading overhead. could parallelize the outer loops with a bit more memory
#ifdef _OPENMP
#  pragma omp parallel for schedule(static) default(none) firstprivate(inited_slide) shared(kj, ki, roi_out, roi_in, in, ovoid, Sa)
#endif
      for(int j=0; j<roi_out->height; j++)
      {
        if(j+kj < 0 || j+kj >= roi_out->height) continue;
        float *S = Sa + dt_get_thread_num() * roi_out->width;
        const float *ins = in + 4*(roi_in->width *(j+kj) + ki);
        float *out = ((float *)ovoid) + 4*roi_out->width*j;

        const int Pm = MIN(MIN(P, j+kj), j);
        const int PM = MIN(MIN(P, roi_out->height-1-j-kj), roi_out->height-1-j);
        // first line of every thread
        // TODO: also every once in a while to assert numerical precision!
        if(!inited_slide)
        {
          // sum up a line
          memset(S, 0x0, sizeof(float)*roi_out->width);
          for(int jj=-Pm; jj<=PM; jj++)
          {
            int i = MAX(0, -ki);
            float *s = S + i;
            const float *inp  = in + 4*i + 4* roi_in->width *(j+jj);
            const float *inps = in + 4*i + 4*(roi_in->width *(j+jj+kj) + ki);
            const int last = roi_out->width + MIN(0, -ki);
            for(; i<last; i++, inp+=4, inps+=4, s++)
            {
              for(int k=0; k<3; k++)
                s[0] += (inp[k] - inps[k])*(inp[k] - inps[k]);
            }
          }
          // only reuse this if we had a full stripe
          if(Pm == P && PM == P) inited_slide = 1;
        }

        // sliding window for this line:
        float *s = S;
        float slide = 0.0f;
        // sum up the first -P..P
        for(int i=0; i<2*P+1; i++) slide += s[i];
        for(int i=0; i<roi_out->width; i++)
        {
          // FIXME: the comment above is actually relevant even for 1000 px width already.
          // XXX    numerical precision will not forgive us:
          if(i-P > 0 && i+P<roi_out->width)
            slide += s[P] - s[-P-1];
          if(i+ki >= 0 && i+ki < roi_out->width)
          {
            // TODO: could put that outside the loop.
            // DEBUG XXX bring back to computable range:
            const float norm = .015f/(2*P+1);
            const __m128 iv = { ins[0], ins[1], ins[2], 1.0f };
            _mm_store_ps(out, _mm_load_ps(out) + iv * _mm_set1_ps(fast_mexp2f(fmaxf(0.0f, slide*norm-2.0f))));
            // _mm_store_ps(out, _mm_load_ps(out) + iv * _mm_set1_ps(fast_mexp2f(fmaxf(0.0f, slide*norm))));
          }
          s   ++;
          ins += 4;
          out += 4;
        }
        if(inited_slide && j+P+1+MAX(0,kj) < roi_out->height)
        {
          // sliding window in j direction:
          int i = MAX(0, -ki);
          float *s = S + i;
          const float *inp  = in + 4*i + 4* roi_in->width *(j+P+1);
          const float *inps = in + 4*i + 4*(roi_in->width *(j+P+1+kj) + ki);
          const float *inm  = in + 4*i + 4* roi_in->width *(j-P);
          const float *inms = in + 4*i + 4*(roi_in->width *(j-P+kj) + ki);
          const int last = roi_out->width + MIN(0, -ki);
          for(; ((unsigned long)s & 0xf) != 0 && i<last; i++, inp+=4, inps+=4, inm+=4, inms+=4, s++)
          {
            float stmp = s[0];
            for(int k=0; k<3; k++)
              stmp += ((inp[k] - inps[k])*(inp[k] - inps[k])
                       -  (inm[k] - inms[k])*(inm[k] - inms[k]));
            s[0] = stmp;
          }
          /* Process most of the line 4 pixels at a time */
          for(; i<last-4; i+=4, inp+=16, inps+=16, inm+=16, inms+=16, s+=4)
          {
            __m128 sv = _mm_load_ps(s);
            const __m128 inp1 = _mm_load_ps(inp)    - _mm_load_ps(inps);
            const __m128 inp2 = _mm_load_ps(inp+4)  - _mm_load_ps(inps+4);
            const __m128 inp3 = _mm_load_ps(inp+8)  - _mm_load_ps(inps+8);
            const __m128 inp4 = _mm_load_ps(inp+12) - _mm_load_ps(inps+12);

            const __m128 inp12lo = _mm_unpacklo_ps(inp1,inp2);
            const __m128 inp34lo = _mm_unpacklo_ps(inp3,inp4);
            const __m128 inp12hi = _mm_unpackhi_ps(inp1,inp2);
            const __m128 inp34hi = _mm_unpackhi_ps(inp3,inp4);

            const __m128 inpv0 = _mm_movelh_ps(inp12lo,inp34lo);
            sv += inpv0*inpv0;

            const __m128 inpv1 = _mm_movehl_ps(inp34lo,inp12lo);
            sv += inpv1*inpv1;

            const __m128 inpv2 = _mm_movelh_ps(inp12hi,inp34hi);
            sv += inpv2*inpv2;

            const __m128 inm1 = _mm_load_ps(inm)    - _mm_load_ps(inms);
            const __m128 inm2 = _mm_load_ps(inm+4)  - _mm_load_ps(inms+4);
            const __m128 inm3 = _mm_load_ps(inm+8)  - _mm_load_ps(inms+8);
            const __m128 inm4 = _mm_load_ps(inm+12) - _mm_load_ps(inms+12);

            const __m128 inm12lo = _mm_unpacklo_ps(inm1,inm2);
            const __m128 inm34lo = _mm_unpacklo_ps(inm3,inm4);
            const __m128 inm12hi = _mm_unpackhi_ps(inm1,inm2);
            const __m128 inm34hi = _mm_unpackhi_ps(inm3,inm4);

            const __m128 inmv0 = _mm_movelh_ps(inm12lo,inm34lo);
            sv -= inmv0*inmv0;

            const __m128 inmv1 = _mm_movehl_ps(inm34lo,inm12lo);
            sv -= inmv1*inmv1;

            const __m128 inmv2 = _mm_movelh_ps(inm12hi,inm34hi);
            sv -= inmv2*inmv2;

            _mm_store_ps(s, sv);
          }
          for(; i<last; i++, inp+=4, inps+=4, inm+=4, inms+=4, s++)
          {
            float stmp = s[0];
            for(int k=0; k<3; k++)
              stmp += ((inp[k] - inps[k])*(inp[k] - inps[k])
                       -  (inm[k] - inms[k])*(inm[k] - inms[k]));
            s[0] = stmp;
          }
        }
        else inited_slide = 0;
      }
    }
  }
  // normalize
#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static) shared(ovoid,roi_out,d)
#endif
  for(int j=0; j<roi_out->height; j++)
  {
    float *out = ((float *)ovoid) + 4*roi_out->width*j;
    for(int i=0; i<roi_out->width; i++)
    {
      if(out[3] > 0.0f)
        _mm_store_ps(out, _mm_mul_ps(_mm_load_ps(out), _mm_set1_ps(1.0f/out[3])));
      // DEBUG show weights
      // _mm_store_ps(out, _mm_set1_ps(1.0f/out[3]));
      out += 4;
    }
  }
  // free shared tmp memory:
  free(Sa);
  free(in);
  backtransform((float *)ovoid, roi_in->width, roi_in->height, aa, bb);

  if(piece->pipe->mask_display)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}
#endif

/** this will be called to init new defaults if a new image is loaded from film strip mode. */
void reload_defaults(dt_iop_module_t *module)
{
  // our module is disabled by default
  module->default_enabled = 0;
  // init defaults:
  memcpy(module->params, &noiseprofiles[0].params, sizeof(dt_iop_nlmeans_params_t));
  memcpy(module->default_params, &noiseprofiles[0].params, sizeof(dt_iop_nlmeans_params_t));
}

/** init, cleanup, commit to pipeline */
void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_nlmeans_params_t));
  module->default_params = malloc(sizeof(dt_iop_nlmeans_params_t));
  module->priority = 145; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_nlmeans_params_t);
  module->gui_data = NULL;
  module->data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL; // just to be sure
  free(module->params);
  module->params = NULL;
}

/** commit is the synch point between core and gui, so it copies params to pipe data. */
void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_nlmeans_params_t *p = (dt_iop_nlmeans_params_t *)params;
  dt_iop_nlmeans_data_t *d = (dt_iop_nlmeans_data_t *)piece->data;
  memcpy(d, p, sizeof(*d));
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_nlmeans_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

static void
radius_callback(GtkWidget *w, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;
  dt_iop_nlmeans_params_t *p = (dt_iop_nlmeans_params_t *)self->params;
  p->radius = (int)dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
strength_callback(GtkWidget *w, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;
  dt_iop_nlmeans_params_t *p = (dt_iop_nlmeans_params_t *)self->params;
  p->strength = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_update(dt_iop_module_t *self)
{
  // let gui slider match current parameters:
  dt_iop_nlmeans_gui_data_t *g = (dt_iop_nlmeans_gui_data_t *)self->gui_data;
  dt_iop_nlmeans_params_t *p = (dt_iop_nlmeans_params_t *)self->params;
  dt_bauhaus_slider_set(g->radius,   p->radius);
  dt_bauhaus_slider_set(g->strength, p->strength);
}

void gui_init(dt_iop_module_t *self)
{
  // init the slider (more sophisticated layouts are possible with gtk tables and boxes):
  self->gui_data = malloc(sizeof(dt_iop_nlmeans_gui_data_t));
  dt_iop_nlmeans_gui_data_t *g = (dt_iop_nlmeans_gui_data_t *)self->gui_data;
  self->widget = gtk_vbox_new(TRUE, DT_BAUHAUS_SPACE);
  g->radius   = dt_bauhaus_slider_new_with_range(self, 0.0f, 4.0f, 1., 2.f, 0);
  g->strength = dt_bauhaus_slider_new_with_range(self, 0.0f, 2.0f, .05, 1.f, 3);
  gtk_box_pack_start(GTK_BOX(self->widget), g->radius, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->strength, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->radius, _("patch size"));
  dt_bauhaus_slider_set_format(g->radius, "%.0f");
  dt_bauhaus_widget_set_label(g->strength, _("strength"));
  g_object_set (GTK_OBJECT(g->radius),   "tooltip-text", _("radius of the patches to match. increase for more sharpness"), (char *)NULL);
  g_object_set (GTK_OBJECT(g->strength), "tooltip-text", _("finetune denoising strength"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->radius),   "value-changed", G_CALLBACK (radius_callback),   self);
  g_signal_connect (G_OBJECT (g->strength), "value-changed", G_CALLBACK (strength_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  // nothing else necessary, gtk will clean up the slider.
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
