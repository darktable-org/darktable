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

#include <lcms2.h>
#include "iop/colorout.h"
#include "control/conf.h"
#include "control/control.h"
#include "common/colormatrices.c"

static cmsToneCurve*
build_srgb_gamma(void)
{
  double Parameters[5];

  Parameters[0] = 2.4;
  Parameters[1] = 1. / 1.055;
  Parameters[2] = 0.055 / 1.055;
  Parameters[3] = 1. / 12.92;
  Parameters[4] = 0.04045;    // d

  return cmsBuildParametricToneCurve(NULL, 4, Parameters);
}

cmsHPROFILE
dt_colorspaces_create_lab_profile()
{
  return cmsCreateLab4Profile(cmsD50_xyY());
}

cmsHPROFILE
dt_colorspaces_create_srgb_profile()
{
  cmsCIExyY       D65;
  cmsCIExyYTRIPLE Rec709Primaries = {
                                   {0.6400, 0.3300, 1.0},
                                   {0.3000, 0.6000, 1.0},
                                   {0.1500, 0.0600, 1.0}
                                   };
  cmsToneCurve *Gamma22[3];
  cmsHPROFILE  hsRGB;
 
  cmsWhitePointFromTemp(&D65, 6504.0);
  Gamma22[0] = Gamma22[1] = Gamma22[2] = build_srgb_gamma();
           
  hsRGB = cmsCreateRGBProfile(&D65, &Rec709Primaries, Gamma22);
  cmsFreeToneCurve(Gamma22[0]);
  if (hsRGB == NULL) return NULL;
      
  cmsWriteRawTag(hsRGB, cmsSigDeviceMfgDescTag,   "(dt internal)", sizeof("(dt internal"));
  cmsWriteRawTag(hsRGB, cmsSigDeviceModelDescTag, "sRGB", sizeof("sRGB"));


  // This will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteRawTag(hsRGB, cmsSigProfileDescriptionTag,   "Darktable sRGB", sizeof("Darktable sRGB"));
 
  return hsRGB;
}

static cmsToneCurve*
build_adobergb_gamma(void)
{
  // this is wrong, this should be a TRC not a table gamma
  double Parameters[2];

  Parameters[0] = 2.2;
  Parameters[1] = 0;

  return cmsBuildParametricToneCurve(NULL, 1, Parameters);
}

// Create the ICC virtual profile for adobe rgb space
cmsHPROFILE
dt_colorspaces_create_adobergb_profile(void)
{
  cmsCIExyY       D65;
  cmsCIExyYTRIPLE AdobePrimaries = {
                                   {0.6400, 0.3300, 1.0},
                                   {0.2100, 0.7100, 1.0},
                                   {0.1500, 0.0600, 1.0}
                                   };
  cmsToneCurve *Gamma22[3];
  cmsHPROFILE  hAdobeRGB;

  cmsWhitePointFromTemp(&D65, 6504.0);
  Gamma22[0] = Gamma22[1] = Gamma22[2] = build_adobergb_gamma();

  hAdobeRGB = cmsCreateRGBProfile(&D65, &AdobePrimaries, Gamma22);
  cmsFreeToneCurve(Gamma22[0]);
  if (hAdobeRGB == NULL) return NULL;

  cmsWriteRawTag(hAdobeRGB, cmsSigDeviceMfgDescTag,   "(dt internal)", sizeof("(dt internal)"));
  cmsWriteRawTag(hAdobeRGB, cmsSigDeviceModelDescTag, "AdobeRGB", sizeof("AdobeRGB"));

  // This will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteRawTag(hAdobeRGB, cmsSigProfileDescriptionTag, "Darktable AdobeRGB", sizeof("Darktable AdobeRGB"));

  return hAdobeRGB;
}

static cmsToneCurve*
build_linear_gamma(void)
{
  double Parameters[2];

  Parameters[0] = 1.0;
  Parameters[1] = 0;

  return cmsBuildParametricToneCurve(0, 1, Parameters);
}


/** inverts the given 3x3 matrix */
static int
mat3inv (float * const dst, const float *const src)
{
#define A(y, x) src[(y - 1) * 3 + (x - 1)]
#define B(y, x) dst[(y - 1) * 3 + (x - 1)]

  const float det =
    A(1, 1) * (A(3, 3) * A(2, 2) - A(3, 2) * A(2, 3)) -
    A(2, 1) * (A(3, 3) * A(1, 2) - A(3, 2) * A(1, 3)) +
    A(3, 1) * (A(2, 3) * A(1, 2) - A(2, 2) * A(1, 3));

  const float epsilon = 1e-7f;
  if (fabsf(det) < epsilon) return 1;

  const float invDet = 1.f / det;

  B(1, 1) =  invDet * (A(3, 3) * A(2, 2) - A(3, 2) * A(2, 3));
  B(1, 2) = -invDet * (A(3, 3) * A(1, 2) - A(3, 2) * A(1, 3));
  B(1, 3) =  invDet * (A(2, 3) * A(1, 2) - A(2, 2) * A(1, 3));

  B(2, 1) = -invDet * (A(3, 3) * A(2, 1) - A(3, 1) * A(2, 3));
  B(2, 2) =  invDet * (A(3, 3) * A(1, 1) - A(3, 1) * A(1, 3));
  B(2, 3) = -invDet * (A(2, 3) * A(1, 1) - A(2, 1) * A(1, 3));

  B(3, 1) =  invDet * (A(3, 2) * A(2, 1) - A(3, 1) * A(2, 2));
  B(3, 2) = -invDet * (A(3, 2) * A(1, 1) - A(3, 1) * A(1, 2));
  B(3, 3) =  invDet * (A(2, 2) * A(1, 1) - A(2, 1) * A(1, 2));
#undef A
#undef B
  return 0;
}

static void
mat3mulv (float *dst, const float *const mat, const float *const v)
{
  for(int k=0;k<3;k++)
  {
    float x=0.0f;
    for(int i=0;i<3;i++) x += mat[3*k+i] * v[i];
    dst[k] = x;
  }
}

static void
mat3mul (float *dst, const float *const m1, const float *const m2)
{
  for(int k=0;k<3;k++)
  {
    for(int i=0;i<3;i++)
    {
      float x=0.0f;
      for(int j=0;j<3;j++) x += m1[3*k+j] * m2[3*j+i];
      dst[3*k+i] = x;
    }
  }
}

static float
lab_f(const float x)
{
  const float epsilon = 216.0f/24389.0f;
  const float kappa   = 24389.0f/27.0f;
  if(x > epsilon) return cbrtf(x);
  else return (kappa*x + 16.0f)/116.0f;
}

void
dt_XYZ_to_Lab(const float *XYZ, float *Lab)
{
  const float d50[3] = { 0.9642, 1.0, 0.8249 };
  const float f[3] = { lab_f(XYZ[0]/d50[0]), lab_f(XYZ[1]/d50[1]), lab_f(XYZ[2]/d50[2]) };
  Lab[0] = 116.0f * f[1] - 16.0f;
  Lab[1] = 500.0f*(f[0] - f[1]);
  Lab[2] = 200.0f*(f[1] - f[2]);
}

static float
lab_f_inv(const float x)
{
  const float epsilon = 0.20689655172413796; // cbrtf(216.0f/24389.0f);
  const float kappa   = 24389.0f/27.0f;
  if(x > epsilon) return x*x*x;
  else return (116.0f*x - 16.0f)/kappa;
}

void
dt_Lab_to_XYZ(const float *Lab, float *XYZ)
{
  const float d50[3] = { 0.9642, 1.0, 0.8249 };
  const float fy = (Lab[0] + 16.0f)/116.0f;
  const float fx = Lab[1]/500.0f + fy;
  const float fz = fy - Lab[2]/200.0f;
  float y;
  const float epsilon = 0.20689655172413796; // cbrtf(216.0f/24389.0f);
  const float kappa   = 24389.0f/27.0f;
  if(Lab[0] > kappa*epsilon)
  {
    y = (Lab[0] + 16.0f)/116.0f;
    y *= y*y;
  }
  else y = Lab[0]/kappa;

  XYZ[0] = d50[0]*lab_f_inv(fx);
  XYZ[1] = d50[1]*y;
  XYZ[2] = d50[2]*lab_f_inv(fz);
}


int
dt_colorspaces_get_darktable_matrix(const char *makermodel, float *matrix)
{
  dt_profiled_colormatrix_t *preset = NULL;
  for(int k=0;k<dt_profiled_colormatrix_cnt;k++)
  {
    if(!strcmp(makermodel, dt_profiled_colormatrices[k].makermodel))
    {
      preset = dt_profiled_colormatrices + k;
      break;
    }
  }
  if(!preset) return -1;

  const float wxyz = preset->white[0]+ preset->white[1]+ preset->white[2];
  const float rxyz = preset->rXYZ[0] + preset->rXYZ[1] + preset->rXYZ[2];
  const float gxyz = preset->gXYZ[0] + preset->gXYZ[1] + preset->gXYZ[2];
  const float bxyz = preset->bXYZ[0] + preset->bXYZ[1] + preset->bXYZ[2];

  const float xn = preset->white[0]/wxyz;
  const float yn = preset->white[1]/wxyz;
  const float xr = preset->rXYZ[0]/rxyz;
  const float yr = preset->rXYZ[1]/rxyz;
  const float xg = preset->gXYZ[0]/gxyz;
  const float yg = preset->gXYZ[1]/gxyz;
  const float xb = preset->bXYZ[0]/bxyz;
  const float yb = preset->bXYZ[1]/bxyz;

  const float primaries[9] = {xr,         xg,         xb,
                              yr,         yg,         yb,
                              1.0f-xr-yr, 1.0f-xg-yg, 1.0f-xb-yb};

  float result[9];
  if(mat3inv(result, primaries)) return -1;

#if 0
  float invcheck[9];
  mat3mul(invcheck, result, primaries);
  printf("invcheck matrix: \n");
  for(int j=0;j<3;j++)
  {
    for(int i=0;i<3;i++)
    {
      printf("%f ", invcheck[3*j+i]);
    }
    printf("\n");
  }
  // passed...
#endif


  const float whitepoint[3] = {xn/yn, 1.0f, (1.0f-xn-yn)/yn};
  float coeff[3];

  // get inverse primary whitepoint
  mat3mulv(coeff, result, whitepoint);


  float tmp[9] = { coeff[0]*xr,           coeff[1]*xg,           coeff[2]*xb,
                   coeff[0]*yr,           coeff[1]*yg,           coeff[2]*yb,
                   coeff[0]*(1.0f-xr-yr), coeff[1]*(1.0f-xg-yg), coeff[2]*(1.0f-xb-yb) };

  // input whitepoint[] in XYZ with Y normalized to 1.0f
  const float dn[3] = { preset->white[0]/(float)preset->white[1], 1.0f, preset->white[2]/(float)preset->white[1]};
  const float lam_rigg[9] = { 0.8951,  0.2664, -0.1614,
		                         -0.7502,  1.7135,  0.0367,
		                          0.0389, -0.0685,  1.0296};
  const float d50[3] = { 0.9642, 1.0, 0.8249 };


  // printf("whitepoints: %f %f %f\n and %f %f %f\n", whitepoint[0], whitepoint[1], whitepoint[2], dn[0], dn[1], dn[2]);


  // adapt to d50

  float chad_inv[9];
  if(mat3inv(chad_inv, lam_rigg)) return -1;

  float cone_src_rgb[3], cone_dst_rgb[3];
  mat3mulv(cone_src_rgb, lam_rigg, dn);
  mat3mulv(cone_dst_rgb, lam_rigg, d50);

  const float cone[9] = { cone_dst_rgb[0]/cone_src_rgb[0], 0.0f, 0.0f,
                          0.0f, cone_dst_rgb[1]/cone_src_rgb[1], 0.0f,
                          0.0f, 0.0f, cone_dst_rgb[2]/cone_src_rgb[2] };

  float tmp2[9];
  float bradford[9];
  mat3mul(tmp2, cone, lam_rigg);
  mat3mul(bradford, chad_inv, tmp2);

#if 0
  printf("bradford matrix: \n");
  for(int j=0;j<3;j++)
  {
    for(int i=0;i<3;i++)
    {
      printf("%f ", bradford[3*j+i]);
    }
    printf("\n");
  }
  printf("tmp2 matrix: \n");
  for(int j=0;j<3;j++)
  {
    for(int i=0;i<3;i++)
    {
      printf("%f ", tmp2[3*j+i]);
    }
    printf("\n");
  }
#endif

  mat3mul(matrix, bradford, tmp);
  return 0;

#if 0
tion(cmsMAT3* Conversion,  // result : bradford
                                const cmsCIEXYZ* SourceWhitePoint, // dn
                                const cmsCIEXYZ* DestWhitePoint,   // D50
                                const cmsMAT3* Chad)               // lam_rigg

{
      
    cmsMAT3 Chad_Inv;
    cmsVEC3 ConeSourceXYZ, ConeSourceRGB;
    cmsVEC3 ConeDestXYZ, ConeDestRGB;
    cmsMAT3 Cone, Tmp;


    Tmp = *Chad;
    // if (!_cmsMAT3inverse(&Tmp, &Chad_Inv)) return FALSE;

    // _cmsVEC3init(&ConeSourceXYZ, SourceWhitePoint -> X,
    //                          SourceWhitePoint -> Y,
    //                          SourceWhitePoint -> Z);

    // _cmsVEC3init(&ConeDestXYZ,   DestWhitePoint -> X,
    //                          DestWhitePoint -> Y,
    //                          DestWhitePoint -> Z);

    // _cmsMAT3eval(&ConeSourceRGB, Chad, &ConeSourceXYZ);
    // _cmsMAT3eval(&ConeDestRGB,   Chad, &ConeDestXYZ);

    // Build matrix
    _cmsVEC3init(&Cone.v[0], ConeDestRGB.n[0]/ConeSourceRGB.n[0],    0.0,  0.0);
    _cmsVEC3init(&Cone.v[1], 0.0,   ConeDestRGB.n[1]/ConeSourceRGB.n[1],   0.0);
    _cmsVEC3init(&Cone.v[2], 0.0,   0.0,   ConeDestRGB.n[2]/ConeSourceRGB.n[2]);


    // Normalize
    _cmsMAT3per(&Tmp, &Cone, Chad);
    _cmsMAT3per(Conversion, &Chad_Inv, &Tmp);

  // cmsBool _cmsBuildRGB2XYZtransferMatrix(cmsMAT3* r, const cmsCIExyY* WhitePt, const cmsCIExyYTRIPLE* Primrs)
#if 0
	cmsVEC3 WhitePoint, Coef;
	cmsMAT3 Result, Primaries;
	cmsFloat64Number xn, yn;
	cmsFloat64Number xr, yr;
	cmsFloat64Number xg, yg;
	cmsFloat64Number xb, yb;

	xn = WhitePt -> x;
	yn = WhitePt -> y;
	xr = Primrs -> Red.x;
	yr = Primrs -> Red.y;
	xg = Primrs -> Green.x;
	yg = Primrs -> Green.y;
	xb = Primrs -> Blue.x;
	yb = Primrs -> Blue.y;
#endif

	// Build Primaries matrix
  // .v[i] accesses the i-th row:
#if 0
	_cmsVEC3init(&Primaries.v[0], xr,        xg,         xb);
	_cmsVEC3init(&Primaries.v[1], yr,        yg,         yb);
	_cmsVEC3init(&Primaries.v[2], (1-xr-yr), (1-xg-yg),  (1-xb-yb));
#endif


	// Result = Primaries ^ (-1) inverse matrix
	// if (!_cmsMAT3inverse(&Primaries, &Result))
		// return FALSE;


	// _cmsVEC3init(&WhitePoint, xn/yn, 1.0, (1.0-xn-yn)/yn);

	// Across inverse primaries ...
	// _cmsMAT3eval(&Coef, &Result, &WhitePoint);

  // VX=0, VY=1, VZ=2
	// Give us the Coefs, then I build transformation matrix
	// _cmsVEC3init(&r -> v[0], Coef.n[VX]*xr,          Coef.n[VY]*xg,          Coef.n[VZ]*xb);
	// _cmsVEC3init(&r -> v[1], Coef.n[VX]*yr,          Coef.n[VY]*yg,          Coef.n[VZ]*yb);
	// _cmsVEC3init(&r -> v[2], Coef.n[VX]*(1.0-xr-yr), Coef.n[VY]*(1.0-xg-yg), Coef.n[VZ]*(1.0-xb-yb));


	// return _cmsAdaptMatrixToD50(r, WhitePt);

// Same as anterior, but assuming D50 destination. White point is given in xyY
static
cmsBool _cmsAdaptMatrixToD50(cmsMAT3* r, const cmsCIExyY* SourceWhitePt)
{
	// cmsCIEXYZ Dn;      
	cmsMAT3 Bradford;
	cmsMAT3 Tmp;

	// cmsxyY2XYZ(&Dn, SourceWhitePt);

	if (!_cmsAdaptationMatrix(&Bradford, NULL, &Dn, cmsD50_XYZ())) return FALSE;
  // Returns the final chrmatic adaptation from illuminant FromIll to Illuminant ToIll
// The cone matrix can be specified in ConeMatrix. If NULL, Bradford is assumed
cmsBool  _cmsAdaptationMatrix(cmsMAT3* r, const cmsMAT3* ConeMatrix, const cmsCIEXYZ* FromIll, const cmsCIEXYZ* ToIll)
{
	cmsMAT3 LamRigg   = {{ // Bradford matrix
		{{  0.8951,  0.2664, -0.1614 }},
		{{ -0.7502,  1.7135,  0.0367 }},
		{{  0.0389, -0.0685,  1.0296 }}
	}};

	if (ConeMatrix == NULL)
		ConeMatrix = &LamRigg;

	return ComputeChromaticAdaptation(r, FromIll, ToIll, ConeMatrix);	
}

	Tmp = *r;
  // mul matrices:
	_cmsMAT3per(r, &Bradford, &Tmp);

	return TRUE;
}
static
cmsBool ComputeChromaticAdaptation(cmsMAT3* Conversion,  // result : bradford
                                const cmsCIEXYZ* SourceWhitePoint, // dn
                                const cmsCIEXYZ* DestWhitePoint,   // D50
                                const cmsMAT3* Chad)               // lam_rigg

{
      
    cmsMAT3 Chad_Inv;
    cmsVEC3 ConeSourceXYZ, ConeSourceRGB;
    cmsVEC3 ConeDestXYZ, ConeDestRGB;
    cmsMAT3 Cone, Tmp;


    Tmp = *Chad;
    if (!_cmsMAT3inverse(&Tmp, &Chad_Inv)) return FALSE;

    _cmsVEC3init(&ConeSourceXYZ, SourceWhitePoint -> X,
                             SourceWhitePoint -> Y,
                             SourceWhitePoint -> Z);

    _cmsVEC3init(&ConeDestXYZ,   DestWhitePoint -> X,
                             DestWhitePoint -> Y,
                             DestWhitePoint -> Z);

    _cmsMAT3eval(&ConeSourceRGB, Chad, &ConeSourceXYZ);
    _cmsMAT3eval(&ConeDestRGB,   Chad, &ConeDestXYZ);

    // Build matrix
    _cmsVEC3init(&Cone.v[0], ConeDestRGB.n[0]/ConeSourceRGB.n[0],    0.0,  0.0);
    _cmsVEC3init(&Cone.v[1], 0.0,   ConeDestRGB.n[1]/ConeSourceRGB.n[1],   0.0);
    _cmsVEC3init(&Cone.v[2], 0.0,   0.0,   ConeDestRGB.n[2]/ConeSourceRGB.n[2]);


    // Normalize
    _cmsMAT3per(&Tmp, &Cone, Chad);
    _cmsMAT3per(Conversion, &Chad_Inv, &Tmp);

	return TRUE;
}
#endif
}

cmsHPROFILE
dt_colorspaces_create_darktable_profile(const char *makermodel)
{
  dt_profiled_colormatrix_t *preset = NULL;
  for(int k=0;k<dt_profiled_colormatrix_cnt;k++)
  {
    if(!strcmp(makermodel, dt_profiled_colormatrices[k].makermodel))
    {
      preset = dt_profiled_colormatrices + k;
      break;
    }
  }
  if(!preset) return NULL;

  const float wxyz = preset->white[0]+preset->white[1]+preset->white[2];
  const float rxyz = preset->rXYZ[0] +preset->rXYZ[1] +preset->rXYZ[2];
  const float gxyz = preset->gXYZ[0] +preset->gXYZ[1] +preset->gXYZ[2];
  const float bxyz = preset->bXYZ[0] +preset->bXYZ[1] +preset->bXYZ[2];
  cmsCIExyY       WP = {preset->white[0]/wxyz, preset->white[1]/wxyz, 1.0};
  cmsCIExyYTRIPLE XYZPrimaries   = {
                                   {preset->rXYZ[0]/rxyz, preset->rXYZ[1]/rxyz, 1.0},
                                   {preset->gXYZ[0]/gxyz, preset->gXYZ[1]/gxyz, 1.0},
                                   {preset->bXYZ[0]/bxyz, preset->bXYZ[1]/bxyz, 1.0}
                                   };
  cmsToneCurve *Gamma[3];
  cmsHPROFILE  hp;
 
  Gamma[0] = Gamma[1] = Gamma[2] = build_linear_gamma();
           
  hp = cmsCreateRGBProfile(&WP, &XYZPrimaries, Gamma);
  cmsFreeToneCurve(Gamma[0]);
  if (hp == NULL) return NULL;
      
  char name[512];
  snprintf(name, 512, "Darktable profiled %s", makermodel);
  cmsWriteRawTag(hp, cmsSigDeviceMfgDescTag,      "(dt internal)", sizeof("(dt internal)"));
  cmsWriteRawTag(hp, cmsSigDeviceModelDescTag,    name, strlen(name));
  cmsWriteRawTag(hp, cmsSigProfileDescriptionTag, name, strlen(name));

  return hp;
}

cmsHPROFILE
dt_colorspaces_create_xyz_profile(void)
{
  cmsHPROFILE hXYZ = cmsCreateXYZProfile();
  // revert some settings which prevent us from using XYZ as output profile:
  cmsSetDeviceClass(hXYZ,            cmsSigDisplayClass);
  cmsSetColorSpace(hXYZ,             cmsSigRgbData);
  cmsSetPCS(hXYZ,                    cmsSigXYZData);
  cmsSetHeaderRenderingIntent(hXYZ,  INTENT_PERCEPTUAL);

  if (hXYZ == NULL) return NULL;
      
  cmsWriteRawTag(hXYZ, cmsSigDeviceMfgDescTag,      "(dt internal)", sizeof("(dt internal)"));
  cmsWriteRawTag(hXYZ, cmsSigDeviceModelDescTag,    "linear XYZ", sizeof("linear XYZ"));
  // This will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteRawTag(hXYZ, cmsSigProfileDescriptionTag, "Darktable linear XYZ", sizeof("Darktable linear XYZ"));
        
  return hXYZ;
}

cmsHPROFILE
dt_colorspaces_create_linear_rgb_profile(void)
{
  cmsCIExyY       D65;
  cmsCIExyYTRIPLE Rec709Primaries = {
                                   {0.6400, 0.3300, 1.0},
                                   {0.3000, 0.6000, 1.0},
                                   {0.1500, 0.0600, 1.0}
                                   };
  cmsToneCurve *Gamma[3];
  cmsHPROFILE  hsRGB;
 
  cmsWhitePointFromTemp(&D65, 6504.0);
  Gamma[0] = Gamma[1] = Gamma[2] = build_linear_gamma();
           
  hsRGB = cmsCreateRGBProfile(&D65, &Rec709Primaries, Gamma);
  cmsFreeToneCurve(Gamma[0]);
  if (hsRGB == NULL) return NULL;
      
  cmsWriteRawTag(hsRGB, cmsSigDeviceMfgDescTag,   "(dt internal)", sizeof("(dt internal"));
  cmsWriteRawTag(hsRGB, cmsSigDeviceModelDescTag, "linear rgb", sizeof("linear rgb"));

  // This will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteRawTag(hsRGB, cmsSigProfileDescriptionTag, "Darktable linear RGB", sizeof("Darktable linear RGB"));
        
  return hsRGB;
}

cmsHPROFILE
dt_colorspaces_create_linear_infrared_profile(void)
{
  // linear rgb with r and b swapped:
  cmsCIExyY       D65;
  cmsCIExyYTRIPLE Rec709Primaries = {
                                   {0.1500, 0.0600, 1.0},
                                   {0.3000, 0.6000, 1.0},
                                   {0.6400, 0.3300, 1.0}
                                   };
  cmsToneCurve *Gamma[3];
  cmsHPROFILE  hsRGB;
 
  cmsWhitePointFromTemp(&D65, 6504.0);
  Gamma[0] = Gamma[1] = Gamma[2] = build_linear_gamma();
           
  hsRGB = cmsCreateRGBProfile(&D65, &Rec709Primaries, Gamma);
  cmsFreeToneCurve(Gamma[0]);
  if (hsRGB == NULL) return NULL;

  cmsWriteRawTag(hsRGB, cmsSigDeviceMfgDescTag,     "(dt internal)", sizeof("(dt internal)"));
  cmsWriteRawTag(hsRGB, cmsSigDeviceModelDescTag,   "linear infrared bgr", sizeof("linear infrared bgr"));

  // This will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteRawTag(hsRGB, cmsSigProfileDescriptionTag, "Darktable Linear Infrared BGR", sizeof("Darktable Linear Infrared BGR"));
        
  return hsRGB;
}

cmsHPROFILE
dt_colorspaces_create_output_profile(const int imgid)
{
  char profile[1024];
  profile[0] = '\0';
  // db lookup colorout params, and dt_conf_() for override
  gchar *overprofile = dt_conf_get_string("plugins/lighttable/export/iccprofile");
  if(!overprofile || !strcmp(overprofile, "image"))
  {
    const dt_iop_colorout_params_t *params;
    // sqlite:
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(darktable.db, "select op_params from history where imgid=?1 and operation='colorout'", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      params = sqlite3_column_blob(stmt, 0);
      strncpy(profile, params->iccprofile, 1024);
    }
    sqlite3_finalize(stmt);
  }
  if(!overprofile && profile[0] == '\0')
    strncpy(profile, "sRGB", 1024);
  if(profile[0] == '\0')
    strncpy(profile, overprofile, 1024);
  g_free(overprofile);

  cmsHPROFILE output = NULL;

  if(!strcmp(profile, "sRGB"))
    output = dt_colorspaces_create_srgb_profile();
  if(!strcmp(profile, "linear_rgb"))
    output = dt_colorspaces_create_linear_rgb_profile();
  else if(!strcmp(profile, "XYZ"))
    output = dt_colorspaces_create_xyz_profile();
  else if(!strcmp(profile, "adobergb"))
    output = dt_colorspaces_create_adobergb_profile();
  else if(!strcmp(profile, "X profile") && darktable.control->xprofile_data)
    output = cmsOpenProfileFromMem(darktable.control->xprofile_data, darktable.control->xprofile_size);
  else
  { // else: load file name
    char datadir[1024];
    char filename[1024];
    dt_get_datadir(datadir, 1024);
    snprintf(filename, 1024, "%s/color/out/%s", datadir, profile);
    output = cmsOpenProfileFromFile(filename, "r");
  }
  if(!output) output = dt_colorspaces_create_srgb_profile();
  return output;
}

cmsHPROFILE
dt_colorspaces_create_cmatrix_profile(float cmatrix[3][4])
{
  cmsCIExyY D65;
  float x[3], y[3];
  float mat[3][3];
  // sRGB D65, the linear part:
  const float rgb_to_xyz[3][3] = {
    {0.4124564, 0.3575761, 0.1804375},
    {0.2126729, 0.7151522, 0.0721750},
    {0.0193339, 0.1191920, 0.9503041}
  };

  for(int c=0;c<3;c++) for(int j=0;j<3;j++)
  {
    mat[c][j] = 0;
    for(int k=0;k<3;k++) mat[c][j] += rgb_to_xyz[c][k]*cmatrix[k][j];
  }
  for(int k=0;k<3;k++)
  {
    const float norm = mat[0][k] + mat[1][k] + mat[2][k];
    x[k] = mat[0][k] / norm;
    y[k] = mat[1][k] / norm;
  }
  cmsCIExyYTRIPLE CameraPrimaries = {
    {x[0], y[0], 1.0},
    {x[1], y[1], 1.0},
    {x[2], y[2], 1.0}
    };
  cmsHPROFILE  cmat;

  cmsWhitePointFromTemp(&D65, 6504.0);

  cmsToneCurve *Gamma[3];
  Gamma[0] = Gamma[1] = Gamma[2] = build_linear_gamma();
  cmat = cmsCreateRGBProfile(&D65, &CameraPrimaries, Gamma);
  if (cmat == NULL) return NULL;
  cmsFreeToneCurve(Gamma[0]);

  cmsWriteRawTag(cmat, cmsSigDeviceMfgDescTag,      "(dt internal)", sizeof("(dt internal)"));
  cmsWriteRawTag(cmat, cmsSigDeviceModelDescTag,    "color matrix built-in", sizeof("color matrix bulit-in"));
  cmsWriteRawTag(cmat, cmsSigProfileDescriptionTag, "color matrix built-in", sizeof("color matrix bulit-in"));

  return cmat;
}

void
dt_colorspaces_cleanup_profile(cmsHPROFILE p)
{
  cmsCloseProfile(p);
}

