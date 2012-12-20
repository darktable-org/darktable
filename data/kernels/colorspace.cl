

float4 Lab_2_LCH(float4 Lab)
{
  float H = atan2(Lab.z, Lab.y);

  H = (H > 0.0f) ? H / (2.0f*M_PI_F) : 1.0f - fabs(H) / (2.0f*M_PI_F);

  float L = Lab.x;
  float C = sqrt(Lab.y*Lab.y + Lab.z*Lab.z);

  return (float4)(L, C, H, Lab.w);
}



float4 LCH_2_Lab(float4 LCH)
{
  float L = LCH.x;
  float a = cos(2.0f*M_PI_F*LCH.z) * LCH.y;
  float b = sin(2.0f*M_PI_F*LCH.z) * LCH.y;

  return (float4)(L, a, b, LCH.w);
}


float4 XYZ_to_Lab(float4 xyz)
{
  float4 lab;

  xyz.x *= (1.0f/0.9642f);
  xyz.z *= (1.0f/0.8242f);
  xyz = (xyz > (float4)0.008856f) ? native_powr(xyz, (float4)1.0f/3.0f) : 7.787f*xyz + (float4)(16.0f/116.0f);
  lab.x = 116.0f * xyz.y - 16.0f;
  lab.y = 500.0f * (xyz.x - xyz.y);
  lab.z = 200.0f * (xyz.y - xyz.z);

  return lab;
}


float4 lab_f_inv(float4 x)
{
  const float4 epsilon = (float4)0.206896551f;
  const float4 kappa   = (float4)(24389.0f/27.0f);
  return (x > epsilon) ? x*x*x : (116.0f*x - (float4)16.0f)/kappa;
}


float4 Lab_to_XYZ(float4 Lab)
{
  const float4 d50 = (float4)(0.9642f, 1.0f, 0.8249f, 0.0f);
  float4 f, XYZ;
  f.y = (Lab.x + 16.0f)/116.0f;
  f.x = Lab.y/500.0f + f.y;
  f.z = f.y - Lab.z/200.0f;
  XYZ = d50 * lab_f_inv(f);

  return XYZ;
}


float4 RGB_2_HSL(const float4 RGB)
{
  float H, S, L;

  // assumes that each channel is scaled to [0; 1]
  float R = RGB.x;
  float G = RGB.y;
  float B = RGB.z;

  float var_Min = fmin(R, fmin(G, B));
  float var_Max = fmax(R, fmax(G, B));
  float del_Max = var_Max - var_Min;

  L = (var_Max + var_Min) / 2.0f;

  if (del_Max == 0.0f)
  {
    H = 0.0f;
    S = 0.0f;
  }
  else
  {
    if (L < 0.5f) S = del_Max / (var_Max + var_Min);
    else          S = del_Max / (2.0f - var_Max - var_Min);

    float del_R = (((var_Max - R) / 6.0f) + (del_Max / 2.0f)) / del_Max;
    float del_G = (((var_Max - G) / 6.0f) + (del_Max / 2.0f)) / del_Max;
    float del_B = (((var_Max - B) / 6.0f) + (del_Max / 2.0f)) / del_Max;

    if      (R == var_Max) H = del_B - del_G;
    else if (G == var_Max) H = (1.0f / 3.0f) + del_R - del_B;
    else if (B == var_Max) H = (2.0f / 3.0f) + del_G - del_R;

    if (H < 0.0f) H += 1.0f;
    if (H > 1.0f) H -= 1.0f;
  }

  return (float4)(H, S, L, RGB.w);
}



float Hue_2_RGB(float v1, float v2, float vH)
{
  if (vH < 0.0f) vH += 1.0f;
  if (vH > 1.0f) vH -= 1.0f;
  if ((6.0f * vH) < 1.0f) return (v1 + (v2 - v1) * 6.0f * vH);
  if ((2.0f * vH) < 1.0f) return (v2);
  if ((3.0f * vH) < 2.0f) return (v1 + (v2 - v1) * ((2.0f / 3.0f) - vH) * 6.0f);
  return (v1);
}




float4 HSL_2_RGB(const float4 HSL)
{
  float R, G, B;

  float H = HSL.x;
  float S = HSL.y;
  float L = HSL.z;

  float var_1, var_2;

  if (S == 0.0f)
  {
    R = B = G = L;
  }
  else
  {
    if (L < 0.5f) var_2 = L * (1.0f + S);
    else          var_2 = (L + S) - (S * L);

    var_1 = 2.0f * L - var_2;

    R = Hue_2_RGB(var_1, var_2, H + (1.0f / 3.0f));
    G = Hue_2_RGB(var_1, var_2, H);
    B = Hue_2_RGB(var_1, var_2, H - (1.0f / 3.0f));
  }

  // returns RGB scaled to [0; 1] for each channel
  return (float4)(R, G, B, HSL.w);
}
