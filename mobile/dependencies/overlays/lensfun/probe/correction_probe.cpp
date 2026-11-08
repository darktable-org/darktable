/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <lensfun.h>
#include <cmath>
#include <cstdio>
#include <cstring>

/* Parse a calibration through the real XML/database API, then verify its
 * closed-form POLY3 mapping and the inverse iteration. Square sensor and image
 * make Lensfun's normalized half-width exactly 150 pixels. */
static int correction_fixture()
{
  static const char xml[] =
    "<lensdatabase version=\"1\">"
    "<mount><name>Fixture Mount</name></mount>"
    "<lens><maker>Fixture</maker><model>Scalar Poly3</model>"
    "<mount>Fixture Mount</mount><cropfactor>1</cropfactor>"
    "<aspect-ratio>1:1</aspect-ratio><type>rectilinear</type>"
    "<calibration><distortion model=\"poly3\" focal=\"12\" k1=\"0.1\"/>"
    "</calibration></lens></lensdatabase>";
  lfDatabase database;
  if(database.Load("Android scalar fixture", xml, sizeof(xml) - 1) != LF_NO_ERROR) return 1;
  const lfLens **lenses = database.FindLenses(nullptr, "Fixture", "Scalar Poly3");
  if(!lenses) return 2;
  const lfLens *lens = lenses[0];
  const bool valid = lens && !lenses[1] && std::strcmp(lens->Model, "Scalar Poly3") == 0;
  lf_free(lenses);
  if(!valid) return 3;
  lfModifier forward(lens, 1.0f, 301, 301), reverse(lens, 1.0f, 301, 301);
  const int flags = LF_MODIFY_DISTORTION;
  if(forward.Initialize(lens, LF_PF_F32, 12, 4, 1000, 1, LF_RECTILINEAR, flags, false) != flags
     || reverse.Initialize(lens, LF_PF_F32, 12, 4, 1000, 1, LF_RECTILINEAR, flags, true) != flags)
    return 4;
  float center[2], point[2], inverse[2];
  if(!forward.ApplyGeometryDistortion(150, 150, 1, 1, center)
     || !forward.ApplyGeometryDistortion(225, 150, 1, 1, point)) return 5;
  if(!std::isfinite(center[0]) || !std::isfinite(center[1])
     || std::fabs(center[0] - 150) > 1e-3 || std::fabs(center[1] - 150) > 1e-3
     || !std::isfinite(point[0]) || !std::isfinite(point[1])
     || std::fabs(point[0] - 219.375) > 1e-3 || std::fabs(point[1] - 150) > 1e-3)
    return 6;
  if(!reverse.ApplyGeometryDistortion(point[0], point[1], 1, 1, inverse)
     || !std::isfinite(inverse[0]) || !std::isfinite(inverse[1])
     || std::fabs(inverse[0] - 225) > 1e-2 || std::fabs(inverse[1] - 150) > 1e-2)
    return 7;
  return 0;
}

#ifdef LENSFUN_ANDROID_LINK_PROBE
extern "C" __attribute__((visibility("default"))) int lensfun_android_correction_probe()
{
  return correction_fixture();
}
#else
int main()
{
  const int result = correction_fixture();
  if(result) std::fprintf(stderr, "Lensfun XML/POLY3 correction fixture failed: %d\n", result);
  return result;
}
#endif
