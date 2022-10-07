/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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
 * for darktable (https://www.darktable.org), so far all calculated by Pascal de Bruijn.
 */
typedef struct dt_profiled_colormatrix_t
{
  const char *makermodel;
  int rXYZ[3], gXYZ[3], bXYZ[3], white[3];
}
dt_profiled_colormatrix_t;

// image submitter, chart type, illuminant, comments
static dt_profiled_colormatrix_t dt_profiled_colormatrices[] =
{
  // clang-format off

  // Robert Park, ColorChecker Passport, strobe, well lit
  { "Pentax K-x",                   { 821548, 337357,  42923}, { 247818, 1042969, -218735}, { -4105, -293045, 1085129}, {792206, 821823, 668640}},

  // Alessandro Miliucci, Wolf Faust IT8, direct sunlight, well lit
  { "Pentax K-r",                   { 960464, 390625,  16312}, { 295563, 1230850, -255936}, {-11536, -339279, 1276337}, {688797, 717697, 605698}},

  // Florian Franzmann, Wolf Faust IT8, strobe, well lit
  { "Pentax K20D",                  {1008652, 388794, -36346}, { 162323, 1113815, -341446}, { 81863, -214325, 1431107}, {664963, 685287, 527252}},

  // Denis Cheremisov, CMP Digital Target 4, strobe, well lit
  { "Pentax K-5",                   { 795456, 343674,  70389}, { 137650,  907654, -299805}, { 31097, -251328, 1054321}, {663452, 689972, 517853}},

  // Scott A. Miller, Wolf Faust IT8, strobe, well lit
  { "Pentax K-5 II",                { 883331, 353348,  24261}, { 323563, 1268616, -214432}, { -5951, -390045, 1241409}, {664520, 695984, 564148}},
  { "Pentax K-5 II s",              { 883331, 353348,  24261}, { 323563, 1268616, -214432}, { -5951, -390045, 1241409}, {664520, 695984, 564148}},

  // Robert Park, ColorChecker Passport, strobe, well lit
  { "Pentax K-7",                   { 738541, 294037,  28061}, { 316025,  984482, -189682}, { 12543, -185852, 1075027}, {812683, 843994, 682587}},

  // Pascal de Bruijn, Homebrew ColorChecker, strobe, well lit (this is not a joke)
  { "Pentax 645D",                  { 814209, 295822,  76019}, { 194641, 1101898, -541473}, { 83664, -313370, 1450531}, {740036, 767288, 629959}},

  // Sven Lindahl, Wolf Faust IT8, direct sunlight, well lit
  { "Canon EOS-1Ds Mark II",        {1078033, 378601, -31113}, { -15396, 1112045, -245743}, {166794, -252411, 1284531}, {681213, 705048, 590790}},

  // Xavier Besse, CMP Digital Target 3, direct sunlight, well lit
  { "Canon EOS 5D Mark II",         { 967590, 399139,  36026}, { -52094,  819046, -232071}, {144455, -143158, 1069305}, {864227, 899139, 741547}},

  // Russell Harrison, Wolf Faust IT8, direct sunlight, well lit
  { "Canon EOS 5D Mark III",        { 947891, 312958,  -7126}, { 163071, 1301834, -276596}, { 75928, -363388, 1272232}, {741272, 757050, 662430}},

  // Deacon MacMillan, Kodak Q60 (IT8), strobe, well lit
  { "Canon EOS 5D",                 { 971420, 386429,   5753}, { 176849, 1141586, -137955}, { 81909, -284790, 1198090}, {753662, 783997, 645142}},

  // Alberto Ferrante, Wolf Faust IT8, direct sunlight, well lit
  { "Canon EOS 7D",                 { 977829, 294815, -44205}, { 154175, 1238007, -325684}, {103363, -297791, 1397461}, {707291, 741760, 626251}},

  // Wim Koorenneef, Wolf Faust IT8, direct sunlight, well lit
  { "Canon EOS 20D",                { 885468, 342117,  20798}, { 278702, 1194733, -164246}, { 42389, -302963, 1147125}, {741379, 771881, 664261}},

  // Martin Fahrendorf, Wolf Faust IT8, direct sunlight, well lit
  { "Canon EOS 30D",                { 955612, 353485, -33371}, { 220200, 1250488, -146393}, { 51956, -361450, 1201355}, {680405, 707977, 597366}},

  // Roy Niswanger, ColorChecker DC, direct sunlight, experimental
  // { "Canon EOS 30D",             { 840195, 148773, -67017}, { 112915, 1104553, -369720}, {240005,  -19562, 1468338}, {827255, 873337, 715317}},

  // Pascal de Bruijn, CMP Digital Target 3, strobe, well lit
  { "Canon EOS 40D",                { 845901, 325760, -13077}, { 110809,  960724, -213577}, { 82230, -218063, 1110229}, {837906, 868393, 705704}},

  // Pascal de Bruijn, CMP Digital Target 3, strobe, well lit
  { "Canon EOS 50D",                {1035110, 365005,  -8057}, {-192184,  930511, -477417}, {189545, -233353, 1360870}, {863983, 888763, 730026}},

  // Robert Park, ColorChecker Passport, strobe, well lit
  { "Canon EOS 60D",                { 811844, 271149,  -2258}, { 233673, 1232880, -165558}, {  9354, -396515, 1055908}, {820908, 814270, 703735}},

  // Pascal de Bruijn, CMP Digital Target 3, strobe, well lit
  { "Canon EOS 350D",       { 784348, 329681, -18875}, { 227249, 1001602, -115692}, { 23834, -270844, 1011185}, {861252, 886368, 721420}},

  // Pascal de Bruijn, CMP Digital Target 3, strobe, well lit
  { "Canon EOS 400D",       { 743546, 283783, -16647}, { 256531, 1035355, -117432}, { 36560, -256836, 1013535}, {855698, 880066, 726181}},

  // Pascal de Bruijn, CMP Digital Target 3, strobe, well lit
  { "Canon EOS 450D",               { 960098, 404968,  22842}, { -85114,  855072, -310928}, {159851, -194611, 1164276}, {851379, 871506, 711823}},

  // Robert Park, ColorChecker Passport, strobe, well lit
  { "Canon EOS 500D",          { 956711, 314590,   1236}, {  27405, 1158569, -346283}, { 95444, -376572, 1260895}, {870087, 898087, 734146}},

  // Robert Park, ColorChecker Passport, strobe, well lit
  { "Canon EOS 550D",               { 864960, 319305,  36880}, { 160904, 1113586, -251587}, { 68832, -334290, 1143463}, {848404, 883118, 718628}},

  // M. Emre Meydan, Wolf Faust IT8, direct sunlight, well lit
  { "Canon EOS 600D",               { 998352, 349960,  -2716}, {  48340, 1270676, -315140}, {114716, -360596, 1265518}, {671249, 670547, 606339}},

  // Christian Carlsson, Wolf Faust IT8, direct sunlight, well lit
  { "Canon EOS 650D",               {1098572, 401901,  -6561}, { -33066, 1257919, -374954}, {190125, -352509, 1469009}, {731064, 752655, 594757}},

  // Copied from EOS 650D
  { "Canon EOS 700D",               {1098572, 401901,  -6561}, { -33066, 1257919, -374954}, {190125, -352509, 1469009}, {731064, 752655, 594757}},

  // Copied from EOS 650D
  { "Canon EOS 100D",               {1098572, 401901,  -6561}, { -33066, 1257919, -374954}, {190125, -352509, 1469009}, {731064, 752655, 594757}},

  // M. Emre Meydan, Wolf Faust IT8, direct sunlight, well lit
  { "Canon EOS 1000D",              { 875580, 325546,   -912}, { 298859, 1301361, -153580}, { 26108, -378876, 1150177}, {675369, 697647, 606659}},

  // Artis Rozentals, Wolf Faust IT8, direct sunlight, well lit
  { "Canon PowerShot S60",          { 879990, 321808,  23041}, { 272324, 1104752, -410950}, { 75500, -184097, 1373230}, {702026, 740524, 622131}},

  // Pascal de Bruijn, CMP Digital Target 3, camera strobe, well lit
  { "Canon PowerShot S90",          { 866531, 231995,  55756}, {  76965, 1067474, -461502}, {106369, -243286, 1314529}, {807449, 855270, 690750}},

  // Robert Park, ColorChecker Passport, strobe, well lit
  { "Canon PowerShot G12",          { 738434, 188904,  71182}, { 318008, 1222260, -338455}, { 13290, -324036, 1207855}, {803146, 841522, 676529}},

  // Henrik Andersson, Homebrew ColorChecker, strobe, well lit
  { "Nikon D40X",                   { 801178, 365555,  13702}, { 276398,  988342,  -84167}, { 21378, -264755, 1052521}, {859116, 893936, 739807}},

  // Henrik Andersson, Homebrew ColorChecker, strobe, well lit
  { "Nikon D60",                    { 746475, 318924,   9277}, { 254776,  946991, -130447}, { 63171, -166458, 1029190}, {753220, 787949, 652695}},

  // Robert Park, ColorChecker Passport, strobe, well lit
  { "Nikon D3000",                  { 778854, 333221,  21927}, { 292007, 1031448,  -88516}, { 27664, -245956,  997391}, {714828, 740387, 601334}},

  // Robert Park, ColorChecker Passport, strobe, well lit
  { "Nikon D3100",                  { 856476, 350891,  48691}, { 221741, 1049164, -218933}, { 12115, -297424, 1083755}, {807373, 841156, 682846}},

  // Robert Park, ColorChecker Passport, strobe, well lit
  { "Nikon D5000",                  { 852386, 356232,  42389}, { 205353, 1026688, -220184}, {  6348, -292526, 1083313}, {822647, 849106, 688538}},

  // Isaac Chanin, Wolf Faust IT8, direct sunlight, well lit
  { "Nikon D5100",                  { 994339, 388123,  37186}, { 226578, 1268478, -310028}, {  1404, -393173, 1285812}, {705582, 733917, 623779}},

  // Torsten Wortwein, Wolf Faust IT8, direct sunlight, well lit
  { "Nikon D5300",                  { 977005, 388763,  42267}, { 144699, 1161331, -312805}, { 61615, -333832, 1270767}, {702164, 733490, 600052}},

  // Robert Park, ColorChecker Passport, strobe, well lit
  { "Nikon D7000",                  { 744919, 228027, -46982}, { 454605, 1326797,  -33585}, {-132294, -467194, 985611}, {609375, 629852, 515625}},

  // Jessica Smith, Wolf Faust IT8, direct sunlight, well lit
  { "Nikon D80",                    { 893585, 348816, -39719}, { 363037, 1246628,  -80994}, { 11658, -286819, 1169052}, {694489, 710114, 562363}},

  // Henrik Andersson, Homebrew ColorChecker, strobe, well lit
  { "Nikon D90",                    { 855072, 361176,  22751}, { 177414,  963577, -241501}, { 28931, -229019, 1123062}, {751816, 781677, 650024}},

  // Rolf Steinort, Wolf Faust IT8, direct sunlight, well lit
  { "Nikon D200",                   { 878922, 352966,   2914}, { 273575, 1048141, -116302}, { 61661, -171021, 1126297}, {691483, 727142, 615204}},

  // Robert Park, ColorChecker Passport, strobe, well lit
  { "Nikon D300S",                  { 813202, 327667,  31067}, { 248810, 1047043, -203049}, { -1160, -284607, 1075790}, {774872, 800415, 648727}},

  // Michael Below, Wolf Faust IT8, direct sunlight, well lit
  { "Nikon D600",                   { 871414, 304840, -22202}, { 284576, 1209747, -302277}, { 34256, -289551, 1375656}, {702774, 726685, 540054}},

  // Robert Park, ColorChecker Passport, strobe, well lit
  { "Nikon D700",                   { 789261, 332016,  34149}, { 270386,  985748, -129135}, {  4074, -230209,  999008}, {798172, 826721, 673126}},

  // Edouard Gomez, ColorChecker Passport, direct sunlight, well lit
  { "Nikon D750",                   { 749283, 264481,  28961}, { 291855, 1096207, -304520}, { 12680, -252914, 1194870}, {783035, 813507, 650787}},

  // Mauro Fuentes, ColorChecker Passport, direct sunlight, well lit
  { "Nikon D800",                   { 792038, 268860,  33951}, { 289093, 1169876, -251740}, {-32654, -340393, 1127960}, {782806, 804443, 659058}},
  { "Nikon D800E",                  { 792038, 268860,  33951}, { 289093, 1169876, -251740}, {-32654, -340393, 1127960}, {782806, 804443, 659058}},

  // Robert Park, ColorChecker Passport, strobe, well lit
  { "Nikon Coolpix P7000",          { 804947, 229630,  97717}, { 178146, 1138763, -395233}, { 88699, -282013, 1234650}, {809998, 842819, 682144}},

  // Wolfgang Kuehnel, Wolf Faust IT8, strobe, well lit
  { "Minolta Dynax 5D",             { 910599, 389618,  20218}, { 330353, 1223724, -116943}, { 24384, -307190, 1156891}, {604309, 629196, 525848}},

  // copied from Pentax K20d
  { "Samsung GX20",                  {1008652, 388794, -36346}, { 162323, 1113815, -341446}, { 81863, -214325, 1431107}, {664963, 685287, 527252}},

  // Karl Mikaelsson, Homebrew ColorChecker, strobe, well lit
  { "Sony DSLR-A100",               { 823853, 374588,  28259}, { 220200,  934509, -108643}, { 48141, -226440, 1062881}, {689651, 715225, 602127}},

  // Alexander Rabtchevich, Wolf Faust IT8, direct sunlight, well lit
  { "Sony DSLR-A200",               { 846786, 366302, -22858}, { 311584, 1046249, -107056}, { 54596, -192993, 1191406}, {708405, 744507, 596771}},

  // Wolfgang Kuehnel, Wolf Faust IT8, strobe, well lit
  { "Sony DSLR-A230",               { 890442, 398560,  24979}, { 376419, 1215424,  -86807}, {  7294, -299591, 1116592}, {578903, 597946, 494522}},

  // Stephane Chauveau, Wolf Faust IT8, direct sunlight, well lit
  { "Sony DSLR-A550",               {1031235, 405899,   1572}, { 185623, 1122162, -272659}, {-25528, -329514, 1249969}, {729797, 753586, 633530}},

  // Karl Mikaelsson, Homebrew ColorChecker, strobe, well lit
  { "Sony DSLR-A700",               { 895737, 374771, -10330}, { 251389, 1076294, -176910}, {-33203, -356445, 1182465}, {742783, 773407, 637604}},

  // Alexander Rabtchevich, Wolf Faust IT8, direct sunlight, well lit
  { "Sony DSLR-A850",               { 968216, 463638,  -4883}, { 279083, 1156906, -230194}, {-21851, -379623, 1297455}, {749298, 799271, 638580}},

  // Copied from A850
  { "Sony DSLR-A900",               { 968216, 463638,  -4883}, { 279083, 1156906, -230194}, {-21851, -379623, 1297455}, {749298, 799271, 638580}},

  // David Meier, Wolf Faust IT8, direct sunlight, well lit
  { "Sony SLT-A55",                 { 969696, 407043,  40268}, { 218201, 1182556, -285400}, { 21042, -342819, 1260223}, {762085, 793961, 670151}},

  // Wolfgang Kuehnel, Wolf Faust IT8, strobe, well lit
  { "Sony SLT-A77",                 {1165085, 503036,  24246}, { 137390, 1265869, -243912}, {-22995, -451843, 1282257}, {645264, 669464, 562073}},

  // Alexander Rabtchevich, Wolf Faust IT8, strobe, well lit
  { "Sony SLT-A99",                 {1059296, 441162,  17807}, { 108673, 1104355, -235931}, { 38605, -302109, 1242004}, {820969, 859192, 715988}},

  // Denis Cheremisov, CMP Digital Target 4, strobe, well lit
  { "Sony ILCE-7",                  { 913254, 376358,  21606}, { 120987, 1024490, -251312}, {  5142, -318573, 1100876}, {849228, 881241, 717255}},

  // Wolfgang Kuehnel, Wolf Faust IT8, strobe, well lit
  { "Sony NEX-3",                   {1157837, 503723,  40894}, { 194550, 1279465, -297058}, {-80719, -471252, 1316238}, {669724, 694839, 586731}},

  // Denis Cheremisov, CMP Digital Target 4, strobe, well lit
  { "Sony NEX-5N",                  { 913406, 394043,   3237}, { 206253, 1085022,  -19917}, {-69138, -377472, 1038483}, {800079, 824112, 674850}},

  // Thorsten Bronger, Wolf Faust IT8, direct sunlight, well lit
  { "Sony NEX-7",                   {1057144, 441849,  -6378}, { 165604, 1224503, -218262}, { 36285, -367065, 1292053}, {752670, 779327, 631165}},

  // Josef Wells, Wolf Faust IT8, strobe, well lit
  { "Sony DSC-RX100",               { 862366, 283417,  42526}, { 302124, 1254868, -333084}, { 84610, -236816, 1327515}, {681137, 699600, 590942}},

  // Mark Haun, Wolf Faust IT8, direct sunlight, well lit
  { "Olympus E-PL1",                { 824387, 288086,  -7355}, { 299500, 1148865, -308929}, { 91858, -198425, 1346603}, {720139, 750717, 619751}},

  // Eugene Kraf, Wolf Faust IT8, direct sunlight, well lit
  { "Olympus E-PL2",                { 785522, 280624,  28503}, { 322266, 1211975, -305984}, { 82550, -246841, 1278198}, {731506, 752808, 645309}},

  // Frederic Crozat, Wolf Faust IT8, direct sunlight, well lit
  { "Olympus E-M5",                 { 937775, 279129,  75378}, { 232697, 1345169, -493317}, { 62012, -354202, 1458389}, {722229, 755142, 623749}},

  // Copied from E-M5
  { "Olympus E-M10",                { 937775, 279129,  75378}, { 232697, 1345169, -493317}, { 62012, -354202, 1458389}, {722229, 755142, 623749}},
  { "Olympus E-PM2",                { 937775, 279129,  75378}, { 232697, 1345169, -493317}, { 62012, -354202, 1458389}, {722229, 755142, 623749}},
  { "Olympus E-PL6",                { 937775, 279129,  75378}, { 232697, 1345169, -493317}, { 62012, -354202, 1458389}, {722229, 755142, 623749}},
  { "Olympus E-PL5",                { 937775, 279129,  75378}, { 232697, 1345169, -493317}, { 62012, -354202, 1458389}, {722229, 755142, 623749}},
  { "Olympus E-P5",                 { 937775, 279129,  75378}, { 232697, 1345169, -493317}, { 62012, -354202, 1458389}, {722229, 755142, 623749}},

  // Sebastian Haaf, Wolf Faust IT8, direct sunlight, well lit
  { "Olympus E-M1",                 { 774292, 245407,  30823}, { 433823, 1410355, -453156}, {-18448, -431107, 1479370}, {758911, 788452, 600266}},

  // Karl Mikaelsson, Homebrew ColorChecker, strobe, well lit
  { "Olympus E-500",                { 925171, 247681,  26367}, { 257187, 1270187, -455826}, {-87784, -426529, 1383041}, {790421, 812775, 708054}},

  // Henrik Andersson, Homebrew ColorChecker, camera strobe, well lit
  { "Olympus SP570UZ",              { 780991, 262283,  27969}, { 147522, 1135239, -422974}, {142731, -293610, 1316803}, {769669, 804474, 676895}},

  // Robert Park, ColorChecker Passport, camera strobe, well lit
  { "Panasonic DMC-FZ45",           { 833542, 259720,  35721}, { 129517, 1239594, -525848}, {117340, -405273, 1440384}, {825226, 863846, 688431}},

  // Robert Park, ColorChecker Passport, camera strobe, well lit
  { "Panasonic DMC-FZ100",          { 700119, 181885, -50354}, { 355804, 1326492, -441132}, {   244, -424149, 1415451}, {734222, 767410, 619049}},

  // Robert Park, ColorChecker Passport, strobe, well lit
  { "Panasonic DMC-G1",             { 747467, 300064,  74265}, { 225922, 1028946, -310913}, { 91782, -229019, 1153793}, {846222, 864502, 694458}},

  // Deacon MacMillan, Kodak Q60 (IT8), strobe, well lit
  { "Panasonic DMC-GF1",            { 802048, 330963,   7477}, { 194519, 968170,  -270004}, { 47211, -246552, 1177536}, {719223, 750900, 614120}},

  // Robert Park, ColorChecker Passport, strobe, well lit
  { "Panasonic DMC-G2",             { 753250, 303024,  75287}, { 225540, 1036041, -320923}, { 90927, -233749, 1170151}, {837860, 857056, 687210}},

  // Martin Schitter, Wolf Faust IT8, direct sunlight, well lit
  { "Panasonic DMC-GH4",            { 937286, 310822,  37857}, { 196823, 1184341, -338242}, { 59952, -267319, 1340836}, {703812, 738983, 594162}},

  // Robert Park, ColorChecker Passport, strobe, well lit
  { "Panasonic DMC-LX3",            { 779907, 298859,  94101}, { 239655, 1167938, -489197}, { 53589, -371368, 1317261}, {796707, 825119, 668030}},

  // Robert Park, ColorChecker Passport, strobe, well lit
  { "Panasonic DMC-LX5",            { 845215, 228226,  59219}, { 190109, 1297211, -543121}, { 42511, -433456, 1414032}, {761322, 790985, 642044}},

  // Pascal de Bruijn, CMP Digital Target 3, strobe (PSEF15A), well lit
  { "Samsung NX100",                { 859955, 369919,  17136}, { 127045,  869888, -258362}, { 69351, -149155, 1121475}, {854538, 897888, 691147}},

  // Copied from NX100
  { "Samsung NX5",                  { 859955, 369919,  17136}, { 127045,  869888, -258362}, { 69351, -149155, 1121475}, {854538, 897888, 691147}},
  { "Samsung NX10",                 { 859955, 369919,  17136}, { 127045,  869888, -258362}, { 69351, -149155, 1121475}, {854538, 897888, 691147}},
  { "Samsung NX11",                 { 859955, 369919,  17136}, { 127045,  869888, -258362}, { 69351, -149155, 1121475}, {854538, 897888, 691147}},

  // Pascal de Bruijn, ColorChecker Classic, direct sunlight, well lit
  { "Samsung NX300",                { 852844, 342072,  35950}, { 201965, 1022202, -298492}, { 37766, -234436, 1215851}, {754166, 791092, 635132}},

  // Pieter de Boer, CMP Digital Target 3, camera strobe, well lit
  { "Kodak Z1015 IS",     { 716446, 157928, -39536}, { 288498, 1234573, -412460}, { 43045, -337677, 1385773}, {774048, 823563, 644012}},

  // Rolf Steinort, Wolf Faust IT8, direct sunlight, well lit
  { "Fujifilm FinePix X100",                { 734619, 274628,  -6302}, { 325272, 1076035, -198608}, {-15366, -280670, 1061050}, {637207, 668228, 578690}},

  // Oleg Dzhimiev, ColorChecker Classic, office lighting, well lit
  { "Elphel 353E",                  {782623, 147903, -272369}, { 110016, 1115250, -729172}, {175949, -157227, 1930222}, {821899, 860794, 671768}}

  // clang-format on
};

static const int dt_profiled_colormatrix_cnt = sizeof(dt_profiled_colormatrices)/sizeof(dt_profiled_colormatrix_t);

static dt_profiled_colormatrix_t dt_vendor_colormatrices[] =
{
  // clang-format off

  // Pascal de Bruijn, DIY ColorChecker, daylight, well lit
  { "Canon EOS 50D",                { 665588, 259155, -37750}, {  61172,  790497, -117310}, {237442,  -49667,  979965}, {946487, 1000000, 1082657}},

  // Pascal de Bruijn, ColorChecker Classic, daylight, well lit
  { "Canon EOS 400D",       { 561768, 248581,  21408}, { 211548,  774429,  -57526}, {190887,  -22995,  861008}, {961594, 1000000, 1086395}},

  // Pascal de Bruijn, ColorChecker Classic, daylight, well lit
  { "Samsung NX100",                { 590607, 279297,  29831}, { 245789,  745789,  -84747}, {127808,  -25101,  879822}, {955185, 1000000, 1089981}},

  // Copied from NX100
  { "Samsung NX5",                  { 590607, 279297,  29831}, { 245789,  745789,  -84747}, {127808,  -25101,  879822}, {955185, 1000000, 1089981}},
  { "Samsung NX10",                 { 590607, 279297,  29831}, { 245789,  745789,  -84747}, {127808,  -25101,  879822}, {955185, 1000000, 1089981}},

  // clang-format on
};

static const int dt_vendor_colormatrix_cnt = sizeof(dt_vendor_colormatrices)/sizeof(dt_profiled_colormatrix_t);

static dt_profiled_colormatrix_t dt_alternate_colormatrices[] =
{
  // clang-format off

  // Pascal de Bruijn, ColorChecker Classic, daylight, well lit
  { "Canon EOS 400D",       { 773514, 302612,  25558}, { 244278, 1107727, -177689}, { 55725, -289902, 1080765}, {822388, 847488, 696823}},

  // Pascal de Bruijn, ColorChecker Classic, daylight, well lit
  { "Samsung NX100",                { 773254, 310013,  12573}, { 299774, 1003143, -150620}, {  4715, -192886, 1070877}, {817657, 850372, 693924}},

  // Copied from NX100
  { "Samsung NX5",                  { 773254, 310013,  12573}, { 299774, 1003143, -150620}, {  4715, -192886, 1070877}, {817657, 850372, 693924}},
  { "Samsung NX10",                 { 773254, 310013,  12573}, { 299774, 1003143, -150620}, {  4715, -192886, 1070877}, {817657, 850372, 693924}},

  // clang-format on
};

static const int dt_alternate_colormatrix_cnt = sizeof(dt_alternate_colormatrices)/sizeof(dt_profiled_colormatrix_t);
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

