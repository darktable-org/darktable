/*
    contrast.cl
    OpenCL kernels for contrast management module
    
    Parity with contrast.c
*/

// Macro to avoid division by zero
// Match host-side `MIN_FLOAT` (exp2f(-16.0f) = 2^-16 = 1/65536)
#define MIN_FLOAT 1.52587890625e-5f


// Kernel 1: Modulated Luminance Calculation
// Implements L = Y + (R - B) * color_impact

__kernel void contrast_luma(
    __global const float *in,
    __global float *out,
    const int width,
    const int height,
    const float color_impact,
    const float coeff_r,
    const float coeff_g,
    const float coeff_b)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int index = (y * width + x) * 4;
  const float4 pixel = (float4)(in[index], in[index+1], in[index+2], in[index+3]);

  // Calculate Y (Standard Luminance)
  // Coefficients passed as arguments to match CPU behavior
  float Y = pixel.x * coeff_r + pixel.y * coeff_g + pixel.z * coeff_b;

  // Add colorimetric impact (R - B)
  // Corresponds to logic: L = Y + (R - B) * color_balance * 0.5
  // The passed color_impact parameter must be (params->color_balance * 0.5f)
  float L = Y + (pixel.x - pixel.z) * color_impact;

  // Clamping to avoid negative values that would break logs
  L = fmax(L, MIN_FLOAT);

  // Output is single channel float buffer
  out[y * width + x] = L;
}


// Kernel 2: Horizontal Box Blur (for Guided Filter: local mean)

__kernel void contrast_box_blur_h(
    __global const float *in,
    __global float *out,
    const int width,
    const int height,
    const int radius)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float sum = 0.0f;
  int count = 0;

  for(int i = -radius; i <= radius; i++)
  {
    int px = clamp(x + i, 0, width - 1);
    sum += in[y * width + px];
    count++;
  }

  out[y * width + x] = sum / (float)count;
}


// Kernel 3: Vertical Box Blur (for Guided Filter: local mean)

__kernel void contrast_box_blur_v(
    __global const float *in,
    __global float *out,
    const int width,
    const int height,
    const int radius)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float sum = 0.0f;
  int count = 0;

  for(int i = -radius; i <= radius; i++)
  {
    int py = clamp(y + i, 0, height - 1);
    sum += in[py * width + x];
    count++;
  }

  out[y * width + x] = sum / (float)count;
}


// Kernel: Square (I -> I^2) for variance calculation

__kernel void contrast_square(
    __global const float *in,
    __global float *out,
    const int width,
    const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  int idx = y * width + x;
  float val = in[idx];
  out[idx] = val * val;
}


// Kernel: Calculate a and b coefficients for Guided Filter
// Notes on parameters mapping from host (see commit_params in contrast.c):
// - The `feathering` parameter passed here is the host-side `d->feathering`,
//   which is computed as `1.0f / p->feathering` in `commit_params()`.
// - The `f_mult_scale` parameter must be the host-side `d->f_mult_*` value
//   (i.e. p->f_mult_* multiplied by the scale factors applied in commit_params).
// Epsilon logic: base_eps = feathering^2, then eps = base_eps * f_mult_scale
// This makes higher `f_mult_scale` values increase epsilon (more smoothing).
// a = (var_I) / (var_I + eps)
// b = mean_I - a * mean_I

__kernel void contrast_calc_ab(
    __global const float *mean_I,
    __global const float *mean_II,
    __global float *out_a,
    __global float *out_b,
    const int width,
    const int height,
    const float feathering,
    const float f_mult_scale)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  int idx = y * width + x;

  // Calculate base epsilon and scale it by the provided detail multiplier.
  float base_eps = feathering * feathering;
  float eps = base_eps * f_mult_scale;

  float m_I = mean_I[idx];
  float m_II = mean_II[idx];

  float var_I = m_II - m_I * m_I;
  float a = var_I / (var_I + eps);
  float b = m_I - a * m_I;

  out_a[idx] = a;
  out_b[idx] = b;
}


// Kernel: Apply Guided Filter (q = mean_a * I + mean_b)

__kernel void contrast_apply_guided(
    __global const float *in,
    __global const float *mean_a,
    __global const float *mean_b,
    __global float *out,
    const int width,
    const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  int idx = y * width + x;
  float I = in[idx];
  float ma = mean_a[idx];
  float mb = mean_b[idx];

  float q = ma * I + mb;
  out[idx] = q;
}


// Kernel 4: Pyramidal Reconstruction and Contrast Application
// Combines the 5 scales, applies gains and CSF.

__kernel void contrast_finalize(
  __global const float *in,              // Original RGB image
  __global float *out,                   // Output
  __global const float *lum_pixel,       // Pixel-wise luminance (unsmoothed)
  const float micro_scale,
  const float fine_scale,
  const float local_scale,
  const float broad_scale,
  const float extended_scale,
  const float noise_threshold,
  const float csf_adaptation,
  const float colorful_contrast,
  const int method,
  const int iterations,
  const float color_balance,
  const float contrast_balance,
  __global const float *lum_smoothed,    // Smoothed: Detail (local)
  __global const float *lum_extended,    // Smoothed: Extended / broadest
  __global const float *lum_broad,       // Smoothed: Broad / medium
  __global const float *lum_fine,        // Smoothed: Fine
  __global const float *lum_micro,       // Smoothed: Micro
  const float global_scale,
  const int width,
  const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int index_rgb = (y * width + x) * 4;
  const int index_luma = y * width + x;

  // Read data
  float4 pixel_in = (float4)(in[index_rgb], in[index_rgb+1], in[index_rgb+2], in[index_rgb+3]);
  float L_pixel = lum_pixel[index_luma];

  // Read smoothed scales
  // The host passes buffers. If a scale is not used, the host passes the same buffer
  // as lum_pixel, so log(pixel/pixel) = 0.
  
  float L_smoothed  = lum_smoothed[index_luma];
  float L_extended  = lum_extended[index_luma];
  float L_broad     = lum_broad[index_luma];
  float L_fine      = lum_fine[index_luma];
  float L_micro     = lum_micro[index_luma];

  // Safety to avoid log(0)
  L_pixel  = fmax(L_pixel, MIN_FLOAT);
  L_micro  = fmax(L_micro, MIN_FLOAT);
  L_fine   = fmax(L_fine, MIN_FLOAT);
  L_smoothed = fmax(L_smoothed, MIN_FLOAT);
  L_broad = fmax(L_broad, MIN_FLOAT);
  L_extended  = fmax(L_extended, MIN_FLOAT);

  // Parameters
  // gain_global is missing from user snippet, assuming 1.0f or derived
  float gain_global = 1.0f; 
  
  // Global vs local balance
  float w_local = (contrast_balance < 0.0f) ? (1.0f + contrast_balance) : 1.0f;
  float w_global = (contrast_balance > 0.0f) ? (1.0f - contrast_balance) : 1.0f;

  // --- Detail calculation (Pyramid) ---
  // Detail = log2(Pixel / Smoothed)
  // Correction = (Gain - 1) * Detail
  
  // Detail Layer (Local)
  float local_ev = native_log2(L_pixel / L_smoothed);
  float correction_ev = (local_scale - 1.0f) * local_ev;

  // Extended (broadest) Layer
  float local_extended = native_log2(L_pixel / L_extended);
  correction_ev += (extended_scale - 1.0f) * local_extended;

  // Broad (medium) Layer
  float local_broad = native_log2(L_pixel / L_broad);
  correction_ev += (broad_scale - 1.0f) * local_broad;

  // Fine Layer
  float local_fine = native_log2(L_pixel / L_fine);
  correction_ev += (fine_scale - 1.0f) * local_fine;

  // Micro Layer
  float local_micro = native_log2(L_pixel / L_micro);
  correction_ev += (micro_scale - 1.0f) * local_micro;

  // Apply local balance
  correction_ev *= w_local;

  // Noise protection (Smoothstep on detail magnitude)
  if (noise_threshold > 1e-6f) {
      float edge0 = noise_threshold * 0.5f;
      float edge1 = edge0 * 1.5f;
      float t = clamp((fabs(correction_ev) - edge0) / fmax(edge1 - edge0, 1e-6f), 0.0f, 1.0f);
      float noise_weight = t * t * (3.0f - 2.0f * t);
      correction_ev *= noise_weight;
  }

  // --- Global Contrast (CSF) ---
  // Centered on middle gray 0.1845
  float log_lum = native_log2(L_pixel / 0.1845f);
  // Gaussian curve for CSF (sigma ~= 2.5 EV -> variance ~ 12.5)
  float csf_weight = native_exp(-log_lum * log_lum / 12.5f);
  
  float global_term = (gain_global - 1.0f) * csf_adaptation * csf_weight * log_lum * w_global;

  // --- Colorimetric Factor (Color Balance) ---
  // Applied as a final gain based on R-B difference
  float factor = 1.0f;
  if (fabs(color_balance) > 0.001f)
  {
      float r = pixel_in.x;
      float g = pixel_in.y;
      float b = pixel_in.z;
      float avg = fmax((r + g + b) / 3.0f, 1e-6f);
      float mix = (color_balance * 0.5f) * (r - b);
      factor = fmax(1.0f + mix / avg, 0.0f);
  }

  // --- Recombination ---
  // Multiplier = 2^(correction + global) * factor
  float multiplier = native_exp2(correction_ev + global_term) * factor;

  float L_final = L_pixel * multiplier;

  const int use_luminance_mode = 1;
  float4 pixel_out;

  if(use_luminance_mode)
  {
      float ratio = L_final / fmax(L_pixel, 1e-6f);
      ratio = fmin(ratio, 8.0f);
      pixel_out.xyz = pixel_in.xyz * ratio;
  }
  else
  {
      float ratio = L_final / fmax(L_pixel, 1e-6f);
      float saturation_boost = 1.0f;
      if (csf_adaptation > 1.0f) {
          saturation_boost = 1.0f + (csf_adaptation - 1.0f) * csf_weight * 0.1f;
      }
      pixel_out.xyz = pixel_in.xyz * ratio * saturation_boost;
  }
  // --- Colorful Contrast (Independent) ---
  if (fabs(colorful_contrast) > 0.001f) {
      float chroma_gain = colorful_contrast * 0.15f;

      // Calculate difference between Red and Blue channels
      // Weight by luminance to protect shadows/highlights
      float chroma_diff = (pixel_out.x - pixel_out.z) * chroma_gain * csf_weight;

      // Apply opposition
      pixel_out.x += chroma_diff;
      pixel_out.z -= chroma_diff;
      pixel_out.y -= chroma_diff * 0.300f; // Luminance compensation on Green

      // Gamut Mapping (Hue Protection)
      if (pixel_out.x < 0.0f || pixel_out.y < 0.0f || pixel_out.z < 0.0f) {
          float t = 1.0f;
          if (pixel_out.x < 0.0f) t = fmin(t, L_final / (L_final - pixel_out.x));
          if (pixel_out.y < 0.0f) t = fmin(t, L_final / (L_final - pixel_out.y));
          if (pixel_out.z < 0.0f) t = fmin(t, L_final / (L_final - pixel_out.z));
          pixel_out.xyz = L_final + t * (pixel_out.xyz - L_final);
      }
  }

  // Final clamping management (positive values only, alpha preserved)
  // Note: We don't necessarily clamp to 1.0 in scene-referred, but we avoid negatives.
  pixel_out.xyz = fmax(pixel_out.xyz, 0.0f);
  pixel_out.w = pixel_in.w; // Alpha unchanged

  out[index_rgb] = pixel_out.x;
  out[index_rgb+1] = pixel_out.y;
  out[index_rgb+2] = pixel_out.z;
  out[index_rgb+3] = pixel_out.w;
}
