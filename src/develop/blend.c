/*
    This file is part of darktable,
    Copyright (C) 2011-2026 darktable developers.

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

#include "blend.h"
#include "common/gaussian.h"
#include "common/guided_filter.h"
#include "common/imagebuf.h"
#include "common/interpolation.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/tiling.h"
#include "develop/imageop_math.h"
#include <math.h>

typedef enum _develop_mask_post_processing
{
  DEVELOP_MASK_POST_NONE = 0,
  DEVELOP_MASK_POST_BLUR = 1,
  DEVELOP_MASK_POST_FEATHER_IN = 2,
  DEVELOP_MASK_POST_FEATHER_OUT = 3,
  DEVELOP_MASK_POST_TONE_CURVE = 4,
} _develop_mask_post_processing;

static dt_develop_blend_params_t _default_blendop_params
    = { DEVELOP_MASK_DISABLED,
        DEVELOP_BLEND_CS_NONE,
        DEVELOP_BLEND_NORMAL2,
        0.0f,
        100.0f,
        DEVELOP_COMBINE_NORM_EXCL,
        0,
        0,
        0.0f,
        DEVELOP_MASK_GUIDE_IN_AFTER_BLUR,
        0.0f,
        0.0f,
        0.0f,
        0.0f, // detail mask threshold
        1, // feather_version
        0, // mask_lock
        { 0 },
        { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
          0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
          0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
          0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f },
        { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
        { 0 }, 0, INVALID_MASKID, FALSE };

static inline dt_develop_blend_colorspace_t _blend_default_module_blend_colorspace(dt_iop_module_t *module,
                                                                                   const gboolean is_scene_referred)
{
  if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
  {
    switch(module->blend_colorspace(module, NULL, NULL))
    {
      case IOP_CS_RAW:
        return DEVELOP_BLEND_CS_RAW;
      case IOP_CS_LAB:
      case IOP_CS_LCH:
        return DEVELOP_BLEND_CS_LAB;
      case IOP_CS_RGB:
        return is_scene_referred
          ? DEVELOP_BLEND_CS_RGB_SCENE
          : DEVELOP_BLEND_CS_RGB_DISPLAY;
      case IOP_CS_HSL:
        return DEVELOP_BLEND_CS_RGB_DISPLAY;
      case IOP_CS_JZCZHZ:
        return DEVELOP_BLEND_CS_RGB_SCENE;
      default:
        return DEVELOP_BLEND_CS_NONE;
    }
  }
  else
    return DEVELOP_BLEND_CS_NONE;
}

dt_develop_blend_colorspace_t dt_develop_blend_default_module_blend_colorspace(dt_iop_module_t *module)
{
  const gboolean is_scene_referred = dt_is_scene_referred();
  return _blend_default_module_blend_colorspace(module, is_scene_referred);
}

static void _blend_init_blendif_boost_parameters(dt_develop_blend_params_t *blend_params,
                                                 const dt_develop_blend_colorspace_t cst)
{
  if(cst == DEVELOP_BLEND_CS_RGB_SCENE)
  {
    // update the default boost parameters for Jz and Cz so that the
    // sRGB white is represented by a value "close" to 1.0. sRGB white
    // (R=1.0, G=1.0, B=1.0) after conversion becomes Jz=0.01758 and
    // will be shown as 1.8. In order to allow enough sensitivity in
    // the low values, the boost factor should be set to log2(0.001) =
    // -6.64385619. To keep the minimum boost factor at zero an offset
    // of that value is added in the GUI. To display the initial boost
    // factor at zero, the default value will be set to that value
    // also.
    blend_params->blendif_boost_factors[DEVELOP_BLENDIF_Jz_in] = -6.64385619f;
    blend_params->blendif_boost_factors[DEVELOP_BLENDIF_Cz_in] = -6.64385619f;
    blend_params->blendif_boost_factors[DEVELOP_BLENDIF_Jz_out] = -6.64385619f;
    blend_params->blendif_boost_factors[DEVELOP_BLENDIF_Cz_out] = -6.64385619f;
  }
}

void dt_develop_blend_init_blend_parameters(dt_develop_blend_params_t *blend_params,
                                            const dt_develop_blend_colorspace_t cst)
{
  memcpy(blend_params, &_default_blendop_params, sizeof(dt_develop_blend_params_t));
  blend_params->blend_cst = cst;
  _blend_init_blendif_boost_parameters(blend_params, cst);
}

void dt_develop_blend_init_blendif_parameters(dt_develop_blend_params_t *blend_params,
                                              const dt_develop_blend_colorspace_t cst)
{
  blend_params->blend_cst = cst;
  blend_params->blend_mode = _default_blendop_params.blend_mode;
  blend_params->blend_parameter = _default_blendop_params.blend_parameter;
  blend_params->blendif = _default_blendop_params.blendif;
  memcpy(blend_params->blendif_parameters, _default_blendop_params.blendif_parameters,
         sizeof(_default_blendop_params.blendif_parameters));
  memcpy(blend_params->blendif_boost_factors, _default_blendop_params.blendif_boost_factors,
         sizeof(_default_blendop_params.blendif_boost_factors));
  _blend_init_blendif_boost_parameters(blend_params, cst);
}

dt_iop_colorspace_type_t dt_develop_blend_colorspace(const dt_dev_pixelpipe_iop_t *const piece,
                                                     const dt_iop_colorspace_type_t cst)
{
  const dt_develop_blend_params_t *const bp = piece->blendop_data;
  if(!bp) return cst;
  switch(bp->blend_cst)
  {
    case DEVELOP_BLEND_CS_RAW:
      return IOP_CS_RAW;
    case DEVELOP_BLEND_CS_LAB:
      return IOP_CS_LAB;
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
    case DEVELOP_BLEND_CS_RGB_SCENE:
      return IOP_CS_RGB;
    default:
      return cst;
  }
}

void dt_develop_blendif_process_parameters(float *const restrict parameters,
                                           const dt_develop_blend_params_t *const params)
{
  const dt_develop_blend_colorspace_t blend_csp = params->blend_cst;
  const uint32_t blendif = params->blendif;
  const float *blendif_parameters = params->blendif_parameters;
  const float *boost_factors = params->blendif_boost_factors;
  for(size_t i = 0, j = 0;
      i < DEVELOP_BLENDIF_SIZE;
      i++, j += DEVELOP_BLENDIF_PARAMETER_ITEMS)
  {
    if(blendif & (1 << i))
    {
      float offset = 0.0f;
      if(blend_csp == DEVELOP_BLEND_CS_LAB
         && (i == DEVELOP_BLENDIF_A_in
             || i == DEVELOP_BLENDIF_A_out
             || i == DEVELOP_BLENDIF_B_in
             || i == DEVELOP_BLENDIF_B_out))
      {
        offset = 0.5f;
      }
      parameters[j + 0] =
        (blendif_parameters[i * 4 + 0] - offset) * exp2f(boost_factors[i]);
      parameters[j + 1] =
        (blendif_parameters[i * 4 + 1] - offset) * exp2f(boost_factors[i]);
      parameters[j + 2] =
        (blendif_parameters[i * 4 + 2] - offset) * exp2f(boost_factors[i]);
      parameters[j + 3] =
        (blendif_parameters[i * 4 + 3] - offset) * exp2f(boost_factors[i]);
      // pre-compute increasing slope and decreasing slope
      parameters[j + 4] = 1.0f / fmaxf(0.001f, parameters[j + 1] - parameters[j + 0]);
      parameters[j + 5] = 1.0f / fmaxf(0.001f, parameters[j + 3] - parameters[j + 2]);
      // handle the case when one end is open to avoid clipping input/output values
      if(blendif_parameters[i * 4 + 0] <= 0.0f && blendif_parameters[i * 4 + 1] <= 0.0f)
      {
        parameters[j + 0] = -FLT_MAX;
        parameters[j + 1] = -FLT_MAX;
      }
      if(blendif_parameters[i * 4 + 2] >= 1.0f && blendif_parameters[i * 4 + 3] >= 1.0f)
      {
        parameters[j + 2] = FLT_MAX;
        parameters[j + 3] = FLT_MAX;
      }
    }
    else
    {
      parameters[j + 0] = -FLT_MAX;
      parameters[j + 1] = -FLT_MAX;
      parameters[j + 2] = FLT_MAX;
      parameters[j + 3] = FLT_MAX;
      parameters[j + 4] = 0.0f;
      parameters[j + 5] = 0.0f;
    }
  }
}

// See function definition in blend.h for important information
gboolean dt_develop_blendif_init_masking_profile(dt_dev_pixelpipe_iop_t *piece,
                                                 dt_iop_order_iccprofile_info_t *blending_profile,
                                                 const dt_develop_blend_colorspace_t cst)
{
  // Bradford adaptation matrix from
  // http://www.brucelindbloom.com/index.html?Eqn_ChromAdapt.html
  const dt_colormatrix_t M = {
      {  0.9555766f, -0.0230393f,  0.0631636f, 0.0f },
      { -0.0282895f,  1.0099416f,  0.0210077f, 0.0f },
      {  0.0122982f, -0.0204830f,  1.3299098f, 0.0f } };

  const dt_iop_order_iccprofile_info_t *const profile = (cst == DEVELOP_BLEND_CS_RGB_SCENE)
      ? dt_ioppr_get_pipe_current_profile_info(piece->module, piece->pipe)
      : dt_ioppr_get_iop_work_profile_info(piece->module, piece->module->dev->iop);
  if(!profile) return FALSE;

  memcpy(blending_profile, profile, sizeof(dt_iop_order_iccprofile_info_t));
  for(size_t y = 0; y < 3; y++)
  {
    for(size_t x = 0; x < 3; x++)
    {
      float sum = 0.0f;
      for(size_t i = 0; i < 3; i++)
        sum += M[y][i] * profile->matrix_in[i][x];
      blending_profile->matrix_out[y][x] = sum;
      blending_profile->matrix_out_transposed[x][y] = sum;
    }
  }

  return TRUE;
}

static inline float _detail_mask_threshold(const float level,
                                           const gboolean detail)
{
  // this does some range calculation for smoother ui experience
  return 0.005f * (detail ? sqrf(level) : 1.0f - sqrtf(fabs(level)));
}

static void _refine_with_detail_mask(dt_iop_module_t *self,
                                     dt_dev_pixelpipe_iop_t *piece,
                                     float *mask,
                                     const dt_iop_roi_t *const roi_in,
                                     const dt_iop_roi_t *const roi_out,
                                     const float level)
{
  if(feqf(level, 0.0f, 1e-6f)) return;

  const gboolean detail = (level > 0.0f);
  const float threshold = _detail_mask_threshold(level, detail);

  dt_dev_pixelpipe_t *p = piece->pipe;
  if(p->scharr.data == NULL) goto error;

  float *lum = dt_masks_calc_detail_mask(piece, threshold, detail);
  if(lum == NULL) goto error;

  // src_hash encodes what the thresholded mask depends on (scharr data + slider value),
  // so the distortion cache is invalidated when the details slider changes.
  const dt_hash_t src_hash = dt_hash(p->scharr.hash, &level, sizeof(level));

  // here we have the slightly blurred full detail mask available
  float *warp_mask = dt_dev_distort_detail_mask(piece, lum, self, src_hash);
  dt_free_align(lum);

  if(warp_mask == NULL) goto error;

  dt_print_pipe(DT_DEBUG_PIPE,
       "refine with detail mask",
       piece->pipe, self, DT_DEVICE_CPU, roi_in, roi_out);

  const size_t msize = (size_t)roi_out->width * roi_out->height;
  DT_OMP_FOR_SIMD(aligned(mask, warp_mask : 64))
  for(size_t idx =0; idx < msize; idx++)
    mask[idx] = mask[idx] * CLIP(warp_mask[idx]);
  dt_free_align(warp_mask);

  return;

  error:
  dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_MASKS,
       "refine with detail mask",
       piece->pipe, self, DT_DEVICE_CPU, roi_in, roi_out, "no mask data available");
  dt_control_log(_("detail mask blending error"));
}

// ---------------------------------------------------------------------------
// refinement bypass: the pipe's snapshot of a preview toggle of the GUI.
//
// The set lives in blend_data and changes on the GTK thread, under bd->lock.
// dt_masks_refine_bypass_commit() copies it into the piece under that lock,
// from commit_params on a pipe worker (dev-doc/GUI_Threading.md). It is never
// stored: without a GUI the snapshot is empty, so export, CLI and thumbnails
// are unaffected

// binary search over the sorted key array
gboolean dt_masks_refine_bypass_lookup(const dt_dev_refine_bypass_t *const bypass,
                                       const guint32 key)
{
  if(!bypass || !bypass->keys) return FALSE;
  int lo = 0, hi = bypass->nkeys - 1;
  while(lo <= hi)
  {
    const int mid = lo + (hi - lo) / 2;
    if(bypass->keys[mid] == key) return TRUE;
    if(bypass->keys[mid] < key) lo = mid + 1;
    else hi = mid - 1;
  }
  return FALSE;
}

dt_hash_t dt_masks_refine_bypass_hash(const dt_dev_refine_bypass_t *const bypass)
{
  if(!bypass || !bypass->nkeys) return DT_INITHASH;
  // the array is sorted, so hashing it in order is already canonical
  return dt_hash(DT_INITHASH, bypass->keys, sizeof(guint32) * bypass->nkeys);
}

void dt_masks_refine_bypass_cleanup(dt_dev_refine_bypass_t *const bypass)
{
  if(!bypass) return;
  g_free(bypass->keys);
  bypass->keys = NULL;
  bypass->nkeys = 0;
}

static int _refine_key_compare(const void *a, const void *b)
{
  const guint32 ka = *(const guint32 *)a;
  const guint32 kb = *(const guint32 *)b;
  return (ka > kb) - (ka < kb);
}

// the bypassed keys of `list`'s members and groups into `out`, and of the lists
// they hold: nested groups and AI objects render their members through the same
// fold, which looks up the same keys (see dt_masks_group_render_roi)
static void _refine_bypass_collect(GHashTable *const set,
                                   const dt_masks_form_t *const list,
                                   GArray *const out,
                                   const int depth)
{
  if(depth > DT_MASKS_NESTING_MAX) return;
  for(const GList *l = list->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *const pt = l->data;
    const guint32 ek = dt_masks_refine_key_element(pt->formid);
    const guint32 gk = dt_masks_refine_key_group(pt->formid);
    // a group is keyed by its marker, so only a marker matches a group key;
    // testing every point costs one lookup and needs no other logic
    if(g_hash_table_lookup(set, GUINT_TO_POINTER(ek))) g_array_append_val(out, ek);
    if(g_hash_table_lookup(set, GUINT_TO_POINTER(gk))) g_array_append_val(out, gk);
    if(dt_masks_point_is_marker(pt)) continue;
    const dt_masks_form_t *const f = dt_masks_get_from_id(darktable.develop, pt->formid);
    if(f && f != list && (f->type & (DT_MASKS_GROUP | DT_MASKS_OBJECT)))
      _refine_bypass_collect(set, f, out, depth + 1);
  }
}

void dt_masks_refine_bypass_commit(const dt_iop_module_t *const module,
                                   dt_dev_pixelpipe_iop_t *const piece)
{
  dt_masks_refine_bypass_cleanup(&piece->refine_bypass);

  dt_iop_gui_blend_data_t *const bd = module ? module->blend_data : NULL;
  if(!bd) return;

  // read the params being committed, which the caller copied into the piece,
  // never module->blend_params: dt_dev_pixelpipe_synch_all commits the
  // defaults before replaying history, and dt_iop_commit_blend_params writes
  // module->blend_params after this runs. During a replay they hold the
  // defaults, without FLEXI and with no mask_id, so the snapshot would be empty
  const dt_develop_blend_params_t *const bp = piece->blendop_data;
  if(!bp || !(bp->mask_mode & DEVELOP_MASK_FLEXI)) return;

  GArray *found = g_array_new(FALSE, FALSE, sizeof(guint32));
  guint table_size = 0;

  dt_pthread_mutex_lock(&bd->lock);
  GHashTable *const set = bd->masks_refine_bypassed;
  if(set && (table_size = g_hash_table_size(set)))
  {
    // look up the keys this mask can use rather than copying the table, which
    // holds what the user bypassed in any mask
    dt_masks_form_t *const grp =
      dt_masks_get_from_id(darktable.develop, bp->mask_id);
    if(g_hash_table_lookup(set, GUINT_TO_POINTER(DT_MASKS_REFINE_KEY_GLOBAL)))
    {
      const guint32 gk = DT_MASKS_REFINE_KEY_GLOBAL;
      g_array_append_val(found, gk);
    }
    if(grp && (grp->type & DT_MASKS_GROUP)) _refine_bypass_collect(set, grp, found, 0);
  }
  dt_pthread_mutex_unlock(&bd->lock);

  const int n = found->len;
  guint32 *keys = (guint32 *)g_array_free(found, n == 0);
  if(n == 0) return;

  qsort(keys, n, sizeof(guint32), _refine_key_compare);
  piece->refine_bypass.keys = keys;
  piece->refine_bypass.nkeys = n;

  dt_print(DT_DEBUG_MASKS,
           "[masks] refine bypass '%s': %d of %u table entries apply to this"
           " mask (mask_id=%d)",
           module->op, n, table_size, bp->mask_id);
}

// is the refinement of the whole flexi mask previewed as off?
static gboolean _flexi_global_refine_bypassed(const dt_dev_pixelpipe_iop_t *const piece,
                                              const dt_develop_blend_params_t *const bp)
{
  if(!(bp->mask_mode & DEVELOP_MASK_FLEXI)) return FALSE;
  return dt_masks_refine_bypass_lookup(&piece->refine_bypass,
                                       DT_MASKS_REFINE_KEY_GLOBAL);
}

// does rendering this group need the module's input and output images? A
// parametric member evaluates its channels against them, and the guided-filter
// feathering of a member or group uses them as guide. In the OpenCL pipe they
// live on the device, and are then read back to the host; without them a
// parametric member renders opaque and feathering is skipped. Plain shapes
// keep the fast path without the readback
static gboolean _group_needs_host_guides(const dt_masks_form_t *const form,
                                         const dt_dev_pixelpipe_iop_t *const piece,
                                         const int depth)
{
  if(!form || depth > DT_MASKS_NESTING_MAX) return FALSE;
  for(const GList *l = form->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *const grpt = l->data;
    // per-shape/per-group guided-filter feathering reads the guide image
    if(grpt->refinement.enabled
       && grpt->refinement.feathering_radius > 0.1f
       && piece->colors >= 3)
      return TRUE;
    const dt_masks_form_t *const f =
      dt_masks_get_from_id_ext(piece->pipe->forms, grpt->formid);
    if(!f) continue;
    // a parametric form evaluates blendif against the guide image
    if(f->type & DT_MASKS_PARAMETRIC) return TRUE;
    // nested groups and AI objects render their members the same way
    if((f->type & (DT_MASKS_GROUP | DT_MASKS_OBJECT))
       && _group_needs_host_guides(f, piece, depth + 1))
      return TRUE;
  }
  return FALSE;
}

// what the raster members of a group read: each source's mask as the pipe made
// it, so the pipe's hash up to that source. The group's own hash only holds
// the reference, and would not change when the source's mask does
static dt_hash_t _group_raster_sources_hash(dt_hash_t hash,
                                            const dt_masks_form_t *const form,
                                            dt_dev_pixelpipe_iop_t *const piece,
                                            const int depth)
{
  if(!form || depth > DT_MASKS_NESTING_MAX) return hash;
  for(const GList *l = form->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *const grpt = l->data;
    const dt_masks_form_t *const f =
      dt_masks_get_from_id_ext(piece->pipe->forms, grpt->formid);
    if(!f) continue;
    if(f->type & (DT_MASKS_GROUP | DT_MASKS_OBJECT))
    {
      hash = _group_raster_sources_hash(hash, f, piece, depth + 1);
      continue;
    }
    if(!(f->type & DT_MASKS_RASTER) || !f->points) continue;
    const dt_masks_point_raster_t *const p = f->points->data;
    // a missing source hashes as such, so its (dis)appearance moves the key
    dt_hash_t source_hash = DT_INVALID_HASH;
    for(GList *n = piece->pipe->nodes; n; n = g_list_next(n))
    {
      dt_dev_pixelpipe_iop_t *const src = n->data;
      if(!dt_iop_module_is(src->module, p->source)
         || src->module->multi_priority != p->instance)
        continue;
      source_hash = dt_dev_pixelpipe_piece_hash(src, &src->processed_roi_out, TRUE);
      break;
    }
    hash = dt_hash(hash, &source_hash, sizeof(source_hash));
  }
  return hash;
}


/* May the blend render `mask_id`'s group as this module's blend mask?

   An IOP_FLAGS_NO_MASKS module (retouch, spots) uses its drawn forms itself in
   process() (see pixelpipe_hb.c), so the blend must not render its classic
   drawn mask: it would paint the module's own shapes as a mask.

   A flexi group is never those shapes. In such a module it holds parametric
   and raster elements only: the panel offers no shapes there
   (bd->masks_support in blend_gui.c), and migration adds none. Do not test the
   flag alone: a parametric mask of such a module lives in a flexi group once
   migrated, and would collapse to a flat opacity. */
gboolean dt_blend_may_render_group(dt_iop_module_t *self,
                                   const dt_develop_mask_mode_t mask_mode)
{
  if(!(self->flags() & IOP_FLAGS_NO_MASKS)) return TRUE;
  return (mask_mode & DEVELOP_MASK_FLEXI) != 0;
}

static size_t _get_post_operations(const dt_develop_blend_params_t *const bp,
                                   const dt_dev_pixelpipe_iop_t *const piece,
                                   _develop_mask_post_processing operations[3])
{
  const gboolean mask_feather = bp->feathering_radius > 0.1f && piece->colors >= 3;
  const gboolean mask_blur = bp->blur_radius > 0.1f;
  const gboolean mask_tone_curve = fabsf(bp->contrast) >= 0.01f || fabsf(bp->brightness) >= 0.01f;

  const gboolean mask_feather_before =
       bp->feathering_guide == DEVELOP_MASK_GUIDE_IN_BEFORE_BLUR
    || bp->feathering_guide == DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR;

  const gboolean mask_feather_out =
       bp->feathering_guide == DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR
    || bp->feathering_guide == DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR;

  const float opacity = CLIP(bp->opacity / 100.0f);

  memset(operations, 0, sizeof(_develop_mask_post_processing) * 3);
  size_t index = 0;

  if(mask_feather)
  {
    if(mask_feather_before)
    {
      operations[index++] = mask_feather_out
        ? DEVELOP_MASK_POST_FEATHER_OUT
        : DEVELOP_MASK_POST_FEATHER_IN;
      if(mask_blur)
        operations[index++] = DEVELOP_MASK_POST_BLUR;
    }
    else
    {
      if(mask_blur)
        operations[index++] = DEVELOP_MASK_POST_BLUR;
      operations[index++] = mask_feather_out
        ? DEVELOP_MASK_POST_FEATHER_OUT
        : DEVELOP_MASK_POST_FEATHER_IN;
    }
  }
  else if(mask_blur)
  {
    operations[index++] = DEVELOP_MASK_POST_BLUR;
  }

  if(mask_tone_curve && opacity > 1e-4f)
  {
    operations[index++] = DEVELOP_MASK_POST_TONE_CURVE;
  }

  return index;
}

/** Restrict the maximum window width to size of provided data.
    Anything larger is just more maths without output difference.
    Using sqrtf(scale) is a better approach to decrease perceived
    differences (both in darkroom and when exporting in non HQ mode.
*/
static inline int _required_w(const float radius, const float scale, const int maxdim)
{
  return MIN(maxdim, MAX(1, (int)ceilf(2.0f * radius * sqrtf(scale))));
}

/* Reminder: stability of the feathering guide filter depends on input data range
   and signal but also on the chose weight and eps.
*/

// feather_version comes from the piece: the module's blend params are rewritten
// with the defaults while another pipe replays history
static float _get_guide_weight(const dt_dev_pixelpipe_iop_t *piece)
{
  const uint32_t fmode = ((const dt_develop_blend_params_t *)piece->blendop_data)->feather_version;
  const dt_iop_colorspace_type_t cst = dt_develop_blend_colorspace(piece, IOP_CS_NONE);
  if(cst == IOP_CS_RGB)
    return (fmode == 0) ? 100.0f : 10.0f;
  else
    return 1.0f;
}

static float _get_feathering_eps(const dt_dev_pixelpipe_iop_t *piece)
{
  const uint32_t fmode = ((const dt_develop_blend_params_t *)piece->blendop_data)->feather_version;
  const dt_iop_colorspace_type_t cst = dt_develop_blend_colorspace(piece, IOP_CS_NONE);

  return (cst == IOP_CS_RGB && fmode) ? 0.5f : 1.0f;
}

// resolved from the piece, like feather_version: the module's raster_mask.sink
// is cleared while another pipe replays history
static const dt_iop_module_t *_raster_source(const dt_dev_pixelpipe_iop_t *piece)
{
  const dt_develop_blend_params_t *const d = piece->blendop_data;
  for(const GList *n = piece->pipe->nodes; n; n = g_list_next(n))
  {
    const dt_iop_module_t *candidate = ((dt_dev_pixelpipe_iop_t *)n->data)->module;
    if(dt_iop_module_is(candidate, d->raster_mask_source)
       && candidate->multi_priority == d->raster_mask_instance)
      return candidate;
  }
  return NULL;
}

static void _develop_blend_process_feather(const float *const guide,
                                           float *const mask,
                                           const size_t width,
                                           const size_t height,
                                           const int ch,
                                           const float guide_weight,
                                           const float feathering_radius,
                                           const float scale,
                                           const float sqrt_eps)
{
  const int w = _required_w(feathering_radius, scale, (int)MAX(width, height));

  float *const restrict mask_bak = dt_alloc_align_float(width * height);
  if(mask_bak)
  {
    dt_iop_image_copy_by_size(mask_bak, mask, width, height, 1);
    guided_filter(guide, mask_bak, mask, width, height, ch, w, sqrt_eps, guide_weight, 0.f, 1.f);
    dt_free_align(mask_bak);
  }
}


static void _develop_blend_process_mask_tone_curve(float *const restrict mask,
                                                   const size_t buffsize,
                                                   const float contrast,
                                                   const float brightness,
                                                   const float opacity)
{
  // empirical mask threshold for fully transparent masks
  const float mask_epsilon = 16.0f * FLT_EPSILON;
  const float e = expf(3.f * contrast);

  DT_OMP_FOR_SIMD(aligned(mask:64))
  for(size_t k = 0; k < buffsize; k++)
  {
    float x = 2.0f * mask[k] / opacity - 1.0f;
    if(1.f - brightness <= 0.f)
      x = mask[k] <= mask_epsilon ? -1.f : 1.f;
    else if(1.f + brightness <= 0.f)
      x = mask[k] >= 1.f - mask_epsilon ? 1.f : -1.f;
    else if(brightness > 0.f)
    {
      x = (x + brightness) / (1.f - brightness);
      x = fminf(x, 1.f);
    }
    else
    {
      x = (x + brightness) / (1.f + brightness);
      x = fmaxf(x, -1.f);
    }
    const float cval = 0.5f * (x * e / (1.f + (e - 1.f) * fabsf(x))) + 0.5f;
    /*  we don't want *very* small masking values possibly resulting from above maths
        so we make sure they above a threshold
    */
    const float mval = cval > 1e-6f ? cval : 0.0f;
    mask[k] = CLIP(mval) * opacity;
  }
}

// one guided-filter feathering pass on an element's mask, guided by the input
// or the output image from the piece's blend_refine_* context, cropped to the
// mask's roi when the input roi differs
static void _feather_form_mask(dt_dev_pixelpipe_iop_t *piece,
                               float *const mask,
                               const dt_iop_roi_t *const roi,
                               const gboolean use_out,
                               const float radius,
                               const float guide_weight,
                               const float sqrt_eps)
{
  const size_t width = roi->width;
  const size_t height = roi->height;
  const int ch = piece->colors;
  const float scale = roi->scale / piece->iscale;

  if(use_out)
  {
    if(piece->blend_refine_guide_out)
      _develop_blend_process_feather(piece->blend_refine_guide_out, mask,
                                     width, height, ch, guide_weight,
                                     radius, scale, sqrt_eps);
    return;
  }

  const float *gin = piece->blend_refine_guide_in;
  const dt_iop_roi_t *rin = piece->blend_refine_roi_in;
  if(!gin) return;

  if(rin && (rin->width != (int)width || rin->height != (int)height))
  {
    float *const restrict guide = dt_alloc_align_float(width * height * ch);
    if(guide)
    {
      dt_iop_copy_image_roi(guide, (float *)gin, ch, rin, roi);
      _develop_blend_process_feather(guide, mask, width, height, ch,
                                     guide_weight, radius, scale, sqrt_eps);
      dt_free_align(guide);
    }
  }
  else
  {
    _develop_blend_process_feather(gin, mask, width, height, ch,
                                   guide_weight, radius, scale, sqrt_eps);
  }
}

void dt_develop_blend_refine_form_mask(dt_iop_module_t *self,
                                       dt_dev_pixelpipe_iop_t *piece,
                                       float *const mask,
                                       const dt_iop_roi_t *const roi,
                                       const dt_masks_refinement_t *const r)
{
  // refine one element's raw [0, 1] mask before the group applies its opacity,
  // in the order of the whole-mask refinement (details, feathering and blur,
  // tone curve), which still runs on the finished mask
  if(!r || !r->enabled) return;

  const size_t buffsize = (size_t)roi->width * roi->height;
  const int ch = piece->colors;

  dt_print(DT_DEBUG_MASKS,
           "[masks] per-shape refine: details=%.3f feather=%.1f(guide=%u)"
           " blur=%.1f contrast=%.2f brightness=%.2f",
           r->details, r->feathering_radius, r->feathering_guide,
           r->blur_radius, r->contrast, r->brightness);

  // detail threshold (uses the pipe scharr data reachable from piece)
  _refine_with_detail_mask(self, piece, mask, roi, roi, r->details);

  const gboolean mask_feather = r->feathering_radius > 0.1f && ch >= 3;
  const gboolean mask_blur = r->blur_radius > 0.1f;
  const gboolean mask_tone_curve =
       fabsf(r->contrast) >= 0.01f || fabsf(r->brightness) >= 0.01f;

  const gboolean feather_before =
       r->feathering_guide == DEVELOP_MASK_GUIDE_IN_BEFORE_BLUR
    || r->feathering_guide == DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR;
  const gboolean feather_out =
       r->feathering_guide == DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR
    || r->feathering_guide == DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR;

  const float guide_weight = _get_guide_weight(piece);
  const float sqrt_eps = _get_feathering_eps(piece);

  if(mask_feather && feather_before)
    _feather_form_mask(piece, mask, roi, feather_out,
                       r->feathering_radius, guide_weight, sqrt_eps);

  if(mask_blur)
  {
    const float sigma = r->blur_radius * roi->scale / piece->iscale;
    const float mmax[] = { 1.0f };
    const float mmin[] = { 0.0f };
    dt_gaussian_t *g = dt_gaussian_init(roi->width, roi->height, 1, mmax, mmin, sigma, 0);
    if(g)
    {
      dt_gaussian_blur(g, mask, mask);
      dt_gaussian_free(g);
    }
  }

  if(mask_feather && !feather_before)
    _feather_form_mask(piece, mask, roi, feather_out,
                       r->feathering_radius, guide_weight, sqrt_eps);

  if(mask_tone_curve)
    _develop_blend_process_mask_tone_curve(mask, buffsize,
                                           r->contrast, r->brightness, 1.0f);
}

static const char *_develop_blend_colorspace_to_str(const dt_develop_blend_colorspace_t type)
{
  switch(type)
  {
    case DEVELOP_BLEND_CS_NONE:         return "BLEND_CS_NONE";
    case DEVELOP_BLEND_CS_RAW:          return "BLEND_CS_RAW";
    case DEVELOP_BLEND_CS_LAB:          return "BLEND_CS_LAB";
    case DEVELOP_BLEND_CS_RGB_DISPLAY:  return "BLEND_CS_RGB_DISPLAY";
    case DEVELOP_BLEND_CS_RGB_SCENE:    return "BLEND_CS_RGB_SCENE";
    default:                            return "invalid BLEND_CS";
  }
}

// Rasterize the module's drawn mask group into `mask`, reusing a previously
// rasterized result when nothing it depends on changed. This spares the (often
// expensive) group rasterization when the module reprocesses with an unchanged
// mask -- e.g. while the mask overlay is shown (pipe cache disabled downstream
// of focus) or when a non-mask slider on a masked module moves.
//
// Only safe when the group needs no host guides: guided-filter feathering and
// parametric-as-form members depend on the module in/out pixels, which have no
// cheap stable hash here. Per-shape details refinement depends on the scharr
// buffer, tracked via src_hash. Global post-ops, invert and global opacity are
// applied by the callers *after* this point, so they need not be in the key.
//
// Shared by the CPU and OpenCL blend paths: the group renderer runs on the host
// in both, so a cached buffer is valid for either, and going through one
// function is what keeps the two from drifting apart. The caller must have set
// piece->blend_refine_guide_* already.
static gboolean _render_drawn_mask_cached(dt_iop_module_t *self,
                                          dt_dev_pixelpipe_iop_t *piece,
                                          dt_masks_form_t *form,
                                          const dt_iop_roi_t *const roi_in,
                                          const dt_iop_roi_t *const roi_out,
                                          const int devid,
                                          float *const mask)
{
  const dt_develop_blend_params_t *const d = piece->blendop_data;
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;

  dt_dev_distorted_mask_cache_t *const mc = &piece->drawn_mask_cache;
  const gboolean cacheable = !_group_needs_host_guides(form, piece, 0);
  const dt_hash_t msrc = piece->pipe->scharr.hash;
  dt_hash_t mkey = DT_INVALID_HASH;
  if(cacheable)
  {
    // hash against the pipe's own form list, not darktable.develop's. The
    // rendered mask is built from piece->pipe->forms (see the lookup above), so
    // that is what the key has to describe. Resolving members through the
    // global instead is silently lossy wherever the two differ -- a headless
    // run (no develop at all), a second-window pinned dev, an export of an
    // image other than the one open in the darkroom: an unresolvable member
    // contributes NOTHING to the hash, which then collapses to the group's own
    // type/formid/version/source and stops changing when the mask does.
    mkey = dt_masks_group_hash_ext(DT_INITHASH, form, piece->pipe->forms);
    const dt_hash_t bph = dt_masks_refine_bypass_hash(&piece->refine_bypass);
    mkey = dt_hash(mkey, &bph, sizeof(dt_hash_t));
    mkey = dt_hash(mkey, roi_out, sizeof(dt_iop_roi_t));
    // mask_mode picks the fold, flexi or classic, for the same form: a cache
    // key must cover everything the result depends on
    mkey = dt_hash(mkey, &d->mask_mode, sizeof(d->mask_mode));
    mkey = _group_raster_sources_hash(mkey, form, piece, 0);
  }

  if(cacheable && mc->data && mkey != DT_INVALID_HASH
     && mc->hash == mkey && mc->src_hash == msrc
     && mc->roi.width == owidth && mc->roi.height == oheight)
  {
    memcpy(mask, mc->data, sizeof(float) * (size_t)owidth * oheight);
    dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE, "drawn mask cache hit",
                  piece->pipe, self, devid, roi_in, roi_out);
    return TRUE;
  }

  // a miss names its reason: a hit alone cannot tell a mask that is not
  // cacheable from one whose key keeps changing
  const char *why = !cacheable                       ? "needs host guides"
                    : !mc->data                      ? "empty"
                    : mc->hash != mkey               ? "mask changed"
                    : mc->src_hash != msrc           ? "details source changed"
                                                     : "size changed";
  const double start = dt_get_debug_wtime();
  const gboolean form_ok = dt_masks_group_render_roi(self, piece, form, roi_out, mask);
  dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE, "drawn mask cache miss",
                piece->pipe, self, devid, roi_in, roi_out, "%s, rendered in %.3fs", why,
                dt_get_debug_wtime() - start);
  if(cacheable && form_ok)
  {
    // through the pipe's own allocator, so this buffer is counted in
    // pipe->mask_cache_size and honours the low-memory opt-out the detail and
    // raster caches already obey: it survives synch_all, so unlike them it is
    // held for as long as the mask is unchanged
    if(dt_dev_pixelpipe_prepare_mask_cache(piece, mc, (size_t)owidth * oheight))
    {
      memcpy(mc->data, mask, sizeof(float) * (size_t)owidth * oheight);
      mc->roi = *roi_out;
      mc->hash = mkey;
      mc->src_hash = msrc;
    }
    else
      mc->hash = DT_INVALID_HASH;
  }
  else if(mc->data)
  {
    // the group needs the images now: drop the entry cached without them
    dt_dev_pixelpipe_clear_mask_cache(piece->pipe, mc);
  }

  return form_ok;
}

/* we test in pixelpipe processing if this required */
void dt_develop_blend_process(dt_iop_module_t *self,
                              dt_dev_pixelpipe_iop_t *piece,
                              const void *const ivoid,
                              void *const ovoid,
                              const dt_iop_roi_t *const roi_in,
                              const dt_iop_roi_t *const roi_out)
{
  dt_develop_blend_params_t *const d = piece->blendop_data;
  const dt_develop_mask_mode_t mask_mode = d->mask_mode;

  const gboolean raster = mask_mode & DEVELOP_MASK_RASTER;
  // a flexi mask renders through the drawn group renderer
  const gboolean mode_drawn = mask_mode & (DEVELOP_MASK_MASK | DEVELOP_MASK_FLEXI);
  const gboolean mode_parametric = mask_mode & DEVELOP_MASK_CONDITIONAL;

  const size_t ch = piece->colors;           // the number of channels in the buffer
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;
  const size_t obuffsize = (size_t)owidth * oheight;

  const int dy = roi_out->y - roi_in->y;
  const int dx = roi_out->x - roi_in->x;

  const gboolean rois_equal = (roi_in->width == owidth) && (roi_in->height == oheight);
  const gboolean inside_roi = (roi_in->width - dx >= owidth)
                           && (roi_in->height - dy >= oheight);

  /* In most cases of blending-enabled modules input and output of the module have
     the exact same dimensions.
     In some cases the module's input exceeds its output.
     Examples are the spot removal and repaint module where the source of a patch
     might lie outside the roi of the output image. Therefore:
     We can only handle blending if roi_out and roi_in have the same scale and
     if roi_out fits into the area given by roi_in.
  */
  if(!inside_roi)
  {
    dt_print_pipe(DT_DEBUG_ALWAYS,
                  "dt_develop_blend",
                  piece->pipe, self, DT_DEVICE_CPU, roi_in, roi_out,
                  "skip blending, work area mismatch");
    return;
  }

  const gboolean valid_request = dt_iop_has_focus(self) && (piece->pipe == self->dev->full.pipe);

  // does user want us to display a specific channel? The details threshold
  // carves a mask out of the image's detail even with no drawn, parametric or
  // raster mask, so it has something to show too
  const dt_dev_pixelpipe_display_mask_t request_mask_display =
      valid_request && (mode_parametric || mode_drawn || d->details != 0.0f)
        ? self->request_mask_display
        : DT_DEV_PIXELPIPE_DISPLAY_NONE;

  const dt_dev_pixelpipe_display_mask_t request_raster_display =
      valid_request && raster
        ? self->request_mask_display
        : DT_DEV_PIXELPIPE_DISPLAY_NONE;

  // get channel max values depending on colorspace
  const dt_develop_blend_colorspace_t blend_csp = d->blend_cst;
  const dt_iop_colorspace_type_t cst = dt_develop_blend_colorspace(piece, IOP_CS_NONE);

  // check if mask should be suppressed temporarily (i.e. just set to global opacity value)
  const gboolean suppress_mask = self->suppress_mask
                                 && valid_request
                                 && (mask_mode & ~DEVELOP_MASK_ENABLED);
  const gboolean uniform = mask_mode == DEVELOP_MASK_ENABLED || suppress_mask;

  // obtaining the list of mask operations to perform. Previewing the whole
  // mask's refinement as off skips them and the details threshold
  const gboolean global_refine_bypass = _flexi_global_refine_bypassed(piece, d);
  _develop_mask_post_processing post_operations[3];
  const size_t post_operations_size =
    global_refine_bypass ? 0 : _get_post_operations(d, piece, post_operations);

  // get the clipped opacity value  0 - 1
  const float opacity = CLIP(d->opacity / 100.0f);

  // allocate space for blend mask used by roi_out
  float *const restrict _mask = dt_alloc_align_float(obuffsize);
  if(!_mask)
  {
    dt_print_pipe(DT_DEBUG_PIPE,
       "dt_develop_blend",
       piece->pipe, self, DT_DEVICE_CPU, roi_in, roi_out,
       "could not allocate buffer for blending");
    return;
  }

  float *const restrict mask = _mask;

  // `mask` was filled uniformly because nothing active computes one (an empty
  // or bypassed flexi group, or a drawn mask with no form yet). Its overlay
  // would paint the whole canvas and hide where to place a shape or a picker,
  // so it is not shown
  gboolean mask_is_uniform_fallback = FALSE;

  if(uniform)
  {
    // blend uniformly (no drawn or parametric mask)
    dt_iop_image_fill(mask, opacity, owidth, oheight, 1); // mask[k] = value;
    // the details threshold carves the image's detail out of the flat mask,
    // as in the raster and drawn branches below; not for suppress_mask, which
    // shows the unrefined result
    if(!suppress_mask)
      _refine_with_detail_mask(self, piece, mask, roi_in, roi_out,
                               global_refine_bypass ? 0.0f : d->details);
  }
  else if(raster)
  {
    /* use a raster mask from another module earlier in the pipe
       dt_dev_get_raster_mask() sets a flag if the returned mask has been
       distorted and thus must be deallocated by the caller
    */
    gboolean free_mask;
    float *raster_mask = dt_dev_get_raster_mask(piece,
                                                _raster_source(piece),
                                                d->raster_mask_id,
                                                self, &free_mask);
    if(raster_mask)
    {
      dt_print_pipe(DT_DEBUG_PIPE,
         "blend raster",
         piece->pipe, self, DT_DEVICE_CPU, roi_in, roi_out, "%s%s%s",
         dt_iop_colorspace_to_name(cst),
         free_mask ? " temp" : " permanent",
         d->raster_mask_invert ? " inverted" : "");
      // invert if required
      if(d->raster_mask_invert)
      {
        DT_OMP_FOR_SIMD(aligned(mask, raster_mask:64))
        for(size_t i = 0; i < obuffsize; i++)
          mask[i] = (1.0f - raster_mask[i]) * opacity;
      }
      else
      {
        // mask[k] = opacity * raster_mask[k];
        dt_iop_image_scaled_copy(mask, raster_mask, opacity, owidth, oheight, 1);
      }
      if(free_mask) dt_free_align(raster_mask);
      _refine_with_detail_mask(self, piece, mask, roi_in, roi_out,
                             global_refine_bypass ? 0.0f : d->details);
    }
    else
    {
      // fallback if no raster mask is available
      dt_iop_image_fill(mask, 0.0f, owidth, oheight, 1);  // mask[k] = value;
    }
  }
  else
  {
    const gboolean inverted = (d->mask_combine & DEVELOP_COMBINE_MASKS_POS);
    gboolean form_ok = FALSE;

    // get the drawn mask if there is one
    dt_masks_form_t *form = dt_masks_get_from_id_ext(piece->pipe->forms, d->mask_id);

    // we blend with a drawn and/or parametric mask. An empty flexi group, which
    // emptying it in the panel leaves behind (_detach_group_members in
    // blend_gui.c), renders nothing and leaves `mask` unwritten, so it takes
    // the "no form" branch below. A classic group renders even when empty: the
    // classic fold clears the mask (see _drawn_content_t in migrate_legacy.c)
    if(form && (form->points || !(mask_mode & DEVELOP_MASK_FLEXI)) && mode_drawn
       && dt_blend_may_render_group(self, mask_mode))
    {
      // the input and output images, for the group renderer's parametric
      // members and feathering
      piece->blend_refine_guide_in = (const float *)ivoid;
      piece->blend_refine_guide_out = (const float *)ovoid;
      piece->blend_refine_roi_in = roi_in;
      piece->blend_refine_roi_out = roi_out;

      // the global post-ops and invert below run on the (cached or fresh) mask
      form_ok = _render_drawn_mask_cached(self, piece, form, roi_in, roi_out,
                                         DT_DEVICE_CPU, mask);

      piece->blend_refine_guide_in = NULL;
      piece->blend_refine_guide_out = NULL;
      piece->blend_refine_roi_in = NULL;
      piece->blend_refine_roi_out = NULL;

      if(inverted)
      {
        // if we have a mask and this flag is set -> invert the mask
        dt_iop_image_invert(mask, 1.0f, owidth, oheight, 1); // mask[k] = 1.0f - mask[k];
      }
    }
    else if(mode_drawn && dt_blend_may_render_group(self, mask_mode))
    {
      // no form defined but drawn mask active
      // we fill the buffer with 1.0f or 0.0f depending on mask_combine
      const float fill = inverted ? 0.0f : 1.0f;
      dt_iop_image_fill(mask, fill, owidth, oheight, 1); //mask[k] = fill;
    }
    else
    {
      // we fill the buffer with 1.0f or 0.0f depending on mask_combine
      const float fill = (d->mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f;
      dt_iop_image_fill(mask, fill, owidth, oheight, 1); //mask[k] = fill;
    }

    // the drawn branches above had nothing active to render and filled a
    // uniform, uninverted mask; decided here once rather than in each branch.
    // Not when the details threshold below carves detail into it, which makes
    // it worth showing
    mask_is_uniform_fallback =
      mode_drawn && dt_blend_may_render_group(self, mask_mode) && !form_ok && !inverted
      && (global_refine_bypass || feqf(d->details, 0.0f, 1e-6f));

    dt_print_pipe(DT_DEBUG_PIPE,
       form && form_ok ? "blend with form" : "blend without form",
       piece->pipe, self, DT_DEVICE_CPU, roi_in, roi_out, "%s, %s%s%s",
       dt_iop_colorspace_to_name(cst),
       _develop_blend_colorspace_to_str(blend_csp),
       inverted ? ", inverted" : "",
       rois_equal ? "" : ", roi differ");

    _refine_with_detail_mask(self, piece, mask, roi_in, roi_out,
                             global_refine_bypass ? 0.0f : d->details);

    // get parametric mask (if any) and apply global opacity
    switch(blend_csp)
    {
      case DEVELOP_BLEND_CS_LAB:
        dt_develop_blendif_lab_make_mask(piece, d,
                                         (const float *const restrict)ivoid,
                                         (const float *const restrict)ovoid,
                                         roi_in, roi_out, mask);
        break;
      case DEVELOP_BLEND_CS_RGB_DISPLAY:
        dt_develop_blendif_rgb_hsl_make_mask(piece, d, (const float *const restrict)ivoid,
                                             (const float *const restrict)ovoid,
                                             roi_in, roi_out, mask);
        break;
      case DEVELOP_BLEND_CS_RGB_SCENE:
        dt_develop_blendif_rgb_jzczhz_make_mask(piece, d, (const float *const restrict)ivoid,
                                                (const float *const restrict)ovoid,
                                                roi_in, roi_out, mask);
        break;
      case DEVELOP_BLEND_CS_RAW:
        dt_develop_blendif_raw_make_mask(piece, d, (const float *const restrict)ivoid,
                                         (const float *const restrict)ovoid,
                                         roi_in, roi_out, mask);
        break;
      default:
        break;
    }
  }

  if(!uniform)
  {
    const float guide_weight = _get_guide_weight(piece);
    const float sqrt_eps = _get_feathering_eps(piece);
    // post processing the mask
    for(size_t index = 0; index < post_operations_size; ++index)
    {
      _develop_mask_post_processing operation = post_operations[index];
      if(operation == DEVELOP_MASK_POST_FEATHER_IN)
      {
        if(rois_equal)
          _develop_blend_process_feather((float *restrict)ivoid, mask,
                                         owidth, oheight, ch, guide_weight,
                                         d->feathering_radius,
                                         roi_out->scale / piece->iscale,
                                         sqrt_eps);
        else
        {
          float *const restrict guide = dt_alloc_align_float(obuffsize * ch);
          if(guide)
          {
            dt_iop_copy_image_roi(guide, (float *restrict)ivoid, ch, roi_in, roi_out);
            _develop_blend_process_feather(guide, mask, owidth, oheight, ch, guide_weight,
                                           d->feathering_radius,
                                           roi_out->scale / piece->iscale,
                                           sqrt_eps);
            dt_free_align(guide);
          }
        }
      }
      else if(operation == DEVELOP_MASK_POST_FEATHER_OUT)
      {
        _develop_blend_process_feather((const float *const restrict)ovoid, mask,
                                       owidth, oheight, ch,
                                       guide_weight,
                                       d->feathering_radius,
                                       roi_out->scale / piece->iscale,
                                       sqrt_eps);
      }
      else if(operation == DEVELOP_MASK_POST_BLUR)
      {
        const float sigma = d->blur_radius * roi_out->scale / piece->iscale;
        const float mmax[] = { 1.0f };
        const float mmin[] = { 0.0f };

        dt_gaussian_t *g = dt_gaussian_init(owidth, oheight, 1, mmax, mmin, sigma, 0);
        if(g)
        {
          dt_gaussian_blur(g, mask, mask);
          dt_gaussian_free(g);
        }
      }
      else if(operation == DEVELOP_MASK_POST_TONE_CURVE)
      {
        _develop_blend_process_mask_tone_curve(mask, obuffsize, d->contrast, d->brightness, opacity);
      }
      else
      {
        dt_print(DT_DEBUG_PIPE, "[blendop] undefined post processing");
      }
    }
  }

  // now apply blending with per-pixel opacity value as defined in mask
  // select the blend operator
  switch(blend_csp)
  {
    case DEVELOP_BLEND_CS_LAB:
      dt_develop_blendif_lab_blend(piece, (const float *const restrict)ivoid,
                                   (float *const restrict)ovoid,
                                   roi_in, roi_out, mask, request_mask_display);
      break;
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
      dt_develop_blendif_rgb_hsl_blend(piece, (const float *const restrict)ivoid,
                                       (float *const restrict)ovoid,
                                       roi_in, roi_out, mask, request_mask_display);
      break;
    case DEVELOP_BLEND_CS_RGB_SCENE:
      dt_develop_blendif_rgb_jzczhz_blend(piece, (const float *const restrict)ivoid,
                                          (float *const restrict)ovoid,
                                          roi_in, roi_out, mask, request_mask_display);
      break;
    case DEVELOP_BLEND_CS_RAW:
      dt_develop_blendif_raw_blend(piece, (const float *const restrict)ivoid,
                                   (float *const restrict)ovoid,
                                   roi_in, roi_out, mask, request_mask_display);
      break;
    default:
      break;
  }

  // register if _this_ module should expose mask or display channel. A mask
  // that is only the uniform fallback (mask_is_uniform_fallback) still applies
  // everywhere; only its overlay is skipped. A channel display stays: the
  // blend above already wrote the channel into the output, which gamma would
  // otherwise pass through as the image
  const dt_dev_pixelpipe_display_mask_t shown_mask_display =
    mask_is_uniform_fallback ? request_mask_display & ~DT_DEV_PIXELPIPE_DISPLAY_MASK
                             : request_mask_display;
  if(shown_mask_display
     & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL))
  {
    piece->pipe->mask_display = shown_mask_display;
  }
  else if(request_raster_display
          & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL))
  {
    piece->pipe->mask_display = request_raster_display;
  }

  // check if we should store the mask for export or use in subsequent modules
  if(dt_iop_is_raster_mask_stored(piece, BLEND_RASTER_ID))
    dt_iop_piece_set_raster(piece, _mask, roi_in, roi_out);
  else
    dt_iop_piece_clear_raster(piece, _mask);
}

#ifdef HAVE_OPENCL
static void _refine_with_detail_mask_cl(dt_iop_module_t *self,
                                        dt_dev_pixelpipe_iop_t *piece,
                                        float *mask,
                                        const dt_iop_roi_t *roi_in,
                                        const dt_iop_roi_t *roi_out,
                                        const float level,
                                        const int devid)
{
  if(feqf(level, 0.0f, 1e-6f)) return;

  const gboolean detail = (level > 0.0f);
  const float threshold = _detail_mask_threshold(level, detail);
  float *lum = NULL;
  cl_mem tmp = NULL;
  cl_mem blur = NULL;
  cl_mem out = NULL;
  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;

  dt_dev_pixelpipe_t *p = piece->pipe;
  if(p->scharr.data == NULL)
  {
    dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_OPENCL,
       "no detail data available", piece->pipe, self, devid, roi_in, roi_out);
    return;
  }
  const int iwidth  = p->scharr.roi.width;
  const int iheight = p->scharr.roi.height;

  lum = dt_alloc_align_float((size_t)iwidth * iheight);
  out = dt_opencl_alloc_device_buffer(devid, sizeof(float) * iwidth * iheight);
  blur = dt_opencl_alloc_device_buffer(devid, sizeof(float) * iwidth * iheight);
  if((lum == NULL) || (out == NULL) || (blur == NULL))
    goto error;

  err = dt_opencl_write_buffer_to_device(devid, p->scharr.data, out, 0, sizeof(float) * iwidth * iheight, TRUE);
  if(err != CL_SUCCESS) goto error;

  err = dt_opencl_enqueue_kernel_2d_args(devid, darktable.opencl->blendop->kernel_calc_blend, iwidth, iheight,
          CLARG(out), CLARG(blur), CLARG(iwidth), CLARG(iheight), CLARG(threshold), CLARG(detail));
  if(err != CL_SUCCESS) goto error;

  err = dt_gaussian_fast_blur_cl_buffer(devid, blur, out, iwidth, iheight, 2.0f, 1, 0.0f, 1.0f);
  if(err != CL_SUCCESS) goto error;

  err = dt_opencl_read_buffer_from_device(devid, lum, out, 0, sizeof(float) * iwidth * iheight, TRUE);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(blur);
  dt_opencl_release_mem_object(out);
  out = NULL;
  blur = NULL;

  // src_hash encodes what the thresholded mask depends on (scharr data + slider value),
  // so the distortion cache is invalidated when the details slider changes.
  const dt_hash_t src_hash = dt_hash(p->scharr.hash, &level, sizeof(level));

  // here we have the slightly blurred full detail mask available
  float *warp_mask = dt_dev_distort_detail_mask(piece, lum, self, src_hash);
  dt_free_align(lum);
  if(warp_mask == NULL)
  {
    err = DT_OPENCL_PROCESS_CL;
    goto error;
  }
  dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE,
       "refine with detail mask", piece->pipe, self, devid, roi_in, roi_out);

  const size_t msize = (size_t)roi_out->width * roi_out->height;
  DT_OMP_FOR_SIMD(aligned(mask, warp_mask : 64))
  for(size_t idx = 0; idx < msize; idx++)
    mask[idx] = mask[idx] * CLIP(warp_mask[idx]);

  dt_free_align(warp_mask);
  return;

  error:
  dt_control_log(_("detail mask CL blending problem"));
  dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_OPENCL,
       "refine with detail_mask",
        piece->pipe, self, piece->pipe->devid, roi_in, roi_out, "OpenCL error: %s", cl_errstr(err));

  dt_opencl_release_mem_object(tmp);
  dt_opencl_release_mem_object(blur);
  dt_opencl_release_mem_object(out);
}

static inline void _blend_process_cl_exchange(cl_mem *a, cl_mem *b)
{
  cl_mem tmp = *a;
  *a = *b;
  *b = tmp;
}


/* we test in pixelpipe processing if this required */
gboolean dt_develop_blend_process_cl(dt_iop_module_t *self,
                                     dt_dev_pixelpipe_iop_t *piece,
                                     cl_mem dev_in,
                                     cl_mem dev_out,
                                     const dt_iop_roi_t *roi_in,
                                     const dt_iop_roi_t *roi_out)
{
  dt_develop_blend_params_t *const d = piece->blendop_data;
  const dt_develop_mask_mode_t mask_mode = d->mask_mode;

  const size_t ch = piece->colors;           // the number of channels in the buffer
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;
  const size_t obuffsize = owidth * oheight;

  const int dy = roi_out->y - roi_in->y;
  const int dx = roi_out->x - roi_in->x;

  const gboolean rois_equal = (roi_in->width == owidth)
                           && (roi_in->height == oheight);
  const gboolean inside_roi = (roi_in->width - dx >= owidth)
                           && (roi_in->height - dy >= oheight);

  // see comments in non-OpenCL code
  if(!inside_roi)
  {
    dt_print_pipe(DT_DEBUG_PIPE,
                  "dt_develop_blend",
                  piece->pipe, self, piece->pipe->devid, roi_in, roi_out,
                  "skip OpenCL blending, work area mismatch");
    return TRUE;
  }
  // only non-zero if mask_display was set by an _earlier_ module
  const dt_dev_pixelpipe_display_mask_t mask_display = piece->pipe->mask_display;

  const gboolean valid_request = dt_iop_has_focus(self) && (piece->pipe == self->dev->full.pipe);

  const gboolean raster = mask_mode & DEVELOP_MASK_RASTER;
  // a flexi mask renders through the drawn group renderer
  const gboolean mode_drawn = mask_mode & (DEVELOP_MASK_MASK | DEVELOP_MASK_FLEXI);
  const gboolean mode_parametric = mask_mode & DEVELOP_MASK_CONDITIONAL;

  // see the same flag in dt_develop_blend_process
  gboolean mask_is_uniform_fallback = FALSE;

  // does user want us to display a specific channel? The details threshold
  // carves a mask out of the image's detail even with no drawn, parametric or
  // raster mask, so it has something to show too
  const dt_dev_pixelpipe_display_mask_t request_mask_display =
      valid_request && (mode_parametric || mode_drawn || d->details != 0.0f)
        ? self->request_mask_display
        : DT_DEV_PIXELPIPE_DISPLAY_NONE;

  const dt_dev_pixelpipe_display_mask_t request_raster_display =
      valid_request && raster
        ? self->request_mask_display
        : DT_DEV_PIXELPIPE_DISPLAY_NONE;

  // get channel max values depending on colorspace
  const dt_develop_blend_colorspace_t blend_csp = d->blend_cst;
  const dt_iop_colorspace_type_t cst = dt_develop_blend_colorspace(piece, IOP_CS_NONE);

  // check if mask should be suppressed temporarily (i.e. just set to global opacity value)
  const gboolean suppress_mask = self->suppress_mask
                                 && valid_request
                                 && (mask_mode & ~DEVELOP_MASK_ENABLED);

  const gboolean uniform = mask_mode == DEVELOP_MASK_ENABLED || suppress_mask;

  // obtaining the list of mask operations to perform. Previewing the whole
  // mask's refinement as off skips them and the details threshold
  const gboolean global_refine_bypass = _flexi_global_refine_bypassed(piece, d);
  _develop_mask_post_processing post_operations[3];
  const size_t post_operations_size =
    global_refine_bypass ? 0 : _get_post_operations(d, piece, post_operations);

  // get the clipped opacity value  0 - 1
  const float opacity = CLIP(d->opacity / 100.0f);

  // allocate space for blend mask
  float *_mask = dt_alloc_align_float(obuffsize);
  if(!_mask)
  {
    dt_print_pipe(DT_DEBUG_ALWAYS,
       "dt_develop_blend",
       piece->pipe, self, piece->pipe->devid, roi_in, roi_out,
       "could not allocate buffer for blending");
   return FALSE;
  }

  float *const mask = _mask;

  // setup some kernels
  int kernel_mask;
  int kernel;
  switch(blend_csp)
  {
    case DEVELOP_BLEND_CS_RAW:
      kernel = ch == 1  ? darktable.opencl->blendop->kernel_blendop_RAW
                        : darktable.opencl->blendop->kernel_blendop_RAW4;
      kernel_mask = darktable.opencl->blendop->kernel_blendop_mask_RAW;
      break;

    case DEVELOP_BLEND_CS_RGB_DISPLAY:
      kernel = darktable.opencl->blendop->kernel_blendop_rgb_hsl;
      kernel_mask = darktable.opencl->blendop->kernel_blendop_mask_rgb_hsl;
      break;

    case DEVELOP_BLEND_CS_RGB_SCENE:
      kernel = darktable.opencl->blendop->kernel_blendop_rgb_jzczhz;
      kernel_mask = darktable.opencl->blendop->kernel_blendop_mask_rgb_jzczhz;
      break;

    case DEVELOP_BLEND_CS_LAB:
    default:
      kernel = darktable.opencl->blendop->kernel_blendop_Lab;
      kernel_mask = darktable.opencl->blendop->kernel_blendop_mask_Lab;
      break;
  }
  int kernel_mask_tone_curve = darktable.opencl->blendop->kernel_blendop_mask_tone_curve;
  int kernel_set_mask = darktable.opencl->blendop->kernel_blendop_set_mask;
  int kernel_display_channel = darktable.opencl->blendop->kernel_blendop_display_channel;

  const int devid = piece->pipe->devid;
  const int offs[2] = { dx, dy };

  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  cl_mem dev_blendif_params = NULL;
  cl_mem dev_boost_factors = NULL;
  cl_mem dev_mask = NULL;
  cl_mem dev_mask_2 = NULL;
  cl_mem dev_tmp = NULL;

  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl = NULL;
  cl_float *profile_lut_cl = NULL;

  cl_mem dev_work_profile_info = NULL;
  cl_mem dev_work_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *work_profile_info_cl = NULL;
  cl_float *work_profile_lut_cl = NULL;

  const size_t region[2] = { owidth, oheight };

  // parameters, for every channel the 4 limits + pre-computed
  // increasing slope and decreasing slope
  float parameters[DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_SIZE] DT_ALIGNED_ARRAY;
  dt_develop_blendif_process_parameters(parameters, d);

  // copy blend parameters to constant device memory
  dev_blendif_params = dt_opencl_copy_host_to_device_constant(devid, sizeof(parameters), parameters);
  if(dev_blendif_params == NULL) goto error;

  dev_mask = dt_opencl_alloc_device(devid, owidth, oheight, sizeof(float));
  if(dev_mask == NULL) goto error;

  const gboolean swap_mask = !uniform && (post_operations_size || !raster);
  if(swap_mask)
  {
    dev_mask_2 = dt_opencl_alloc_device(devid, owidth, oheight, sizeof(float));
    if(dev_mask_2 == NULL) goto error;
  }

  dt_iop_order_iccprofile_info_t profile;
  const gboolean use_profile = dt_develop_blendif_init_masking_profile(piece, &profile, blend_csp);

  err = dt_ioppr_build_iccprofile_params_cl(use_profile ? &profile : NULL,
                                            devid, &profile_info_cl,
                                            &profile_lut_cl,
                                            &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_blendop] profile_info_cl: %s", cl_errstr(err));
    goto error;
  }

  if(uniform)
  {
    // the details threshold, as in dt_develop_blend_process; not for
    // suppress_mask. It has no GPU kernel, so the mask is built on the host
    // and uploaded, as in the raster branch below. Without it the fill kernel
    // does the job on the GPU
    if(!suppress_mask && !global_refine_bypass && d->details != 0.0f)
    {
      dt_iop_image_fill(mask, opacity, owidth, oheight, 1);
      _refine_with_detail_mask_cl(self, piece, mask, roi_in, roi_out, d->details, devid);
      err = dt_opencl_write_host_to_image(devid, mask, dev_mask, owidth, oheight, sizeof(float));
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      // set dev_mask with global opacity value
      err = dt_opencl_enqueue_kernel_2d_args(devid, kernel_set_mask, owidth, oheight,
                                CLARG(dev_mask), CLARG(owidth), CLARG(oheight), CLARG(opacity));
      if(err != CL_SUCCESS)
      {
        dt_print(DT_DEBUG_OPENCL,
                 "[opencl_blendop] kernel_set_mask: %s", cl_errstr(err));
        goto error;
      }
    }
  }
  else if(raster)
  {
    /* use a raster mask from another module earlier in the pipe
       dt_dev_get_raster_mask() sets a flag if the returned mask has been
       distorted and thus must be deallocated by the caller
    */
    gboolean free_mask;
    float *raster_mask = dt_dev_get_raster_mask(piece,
                                                _raster_source(piece),
                                                d->raster_mask_id,
                                                self,
                                                &free_mask);
    if(raster_mask)
    {
      dt_print_pipe(DT_DEBUG_PIPE,
         "blend raster",
        piece->pipe, self, piece->pipe->devid, roi_in, roi_out, "%s%s%s",
        dt_iop_colorspace_to_name(cst),
        d->raster_mask_invert ? " inverted" : "",
        free_mask ? " temp" : " permanent");
      // invert if required
      if(d->raster_mask_invert)
      {
        DT_OMP_FOR_SIMD(aligned(mask, raster_mask:64))
        for(size_t i = 0; i < obuffsize; i++)
          mask[i] = (1.0f - raster_mask[i]) * opacity;
      }
      else
      {
        // mask[k] = opacity * raster_mask[k];
        dt_iop_image_scaled_copy(mask, raster_mask, opacity, owidth, oheight, 1);
      }
      if(free_mask) dt_free_align(raster_mask);
      _refine_with_detail_mask_cl(self, piece, mask, roi_in, roi_out,
                                global_refine_bypass ? 0.0f : d->details, devid);
    }
    else
    {
      // fallback if no raster mask is applied
      dt_iop_image_fill(mask, 0.0f, owidth, oheight, 1); //mask[k] = value;
    }

    err = dt_opencl_write_host_to_image(devid, mask, dev_mask, owidth, oheight, sizeof(float));
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    const gboolean inverted = (d->mask_combine & DEVELOP_COMBINE_MASKS_POS);
    gboolean form_ok = FALSE;
    // get the drawn mask if there is one
    dt_masks_form_t *form = dt_masks_get_from_id_ext(piece->pipe->forms, d->mask_id);

    // we blend with a drawn and/or parametric mask. An empty flexi group takes
    // the "no form" branch below, as in dt_develop_blend_process
    if(form && (form->points || !(mask_mode & DEVELOP_MASK_FLEXI)) && mode_drawn
       && dt_blend_may_render_group(self, mask_mode))
    {
      // the group renders on the CPU in this pipe too. Its parametric members
      // and feathering need the input and output images, which live on the
      // device: they are read back when the group needs them
      // (_group_needs_host_guides)
      float *guide_in = NULL;
      float *guide_out = NULL;
      if(_group_needs_host_guides(form, piece, 0))
      {
        const size_t in_sz = (size_t)roi_in->width * roi_in->height * ch;
        const size_t out_sz = (size_t)roi_out->width * roi_out->height * ch;
        guide_in = dt_alloc_align_float(in_sz);
        guide_out = dt_alloc_align_float(out_sz);
        err = guide_in && guide_out ? CL_SUCCESS : DT_OPENCL_SYSMEM_ALLOCATION;
        if(err == CL_SUCCESS)
          err = dt_opencl_copy_image_to_host(devid, guide_in, dev_in,
                                             roi_in->width, roi_in->height,
                                             ch * sizeof(float));
        if(err == CL_SUCCESS)
          err = dt_opencl_copy_image_to_host(devid, guide_out, dev_out,
                                             roi_out->width, roi_out->height,
                                             ch * sizeof(float));
        if(err != CL_SUCCESS)
        {
          // without guides a parametric member renders fully opaque and
          // per-shape feathering is skipped: let the CPU path render it instead
          dt_print(DT_DEBUG_OPENCL,
                   "[opencl_blendop] mask guide readback failed: %s",
                   cl_errstr(err));
          dt_free_align(guide_in);
          dt_free_align(guide_out);
          goto error;
        }
      }
      piece->blend_refine_guide_in = guide_in;
      piece->blend_refine_guide_out = guide_out;
      piece->blend_refine_roi_in = roi_in;
      piece->blend_refine_roi_out = roi_out;

      // same memoized rasterization as the CPU path: the group renderer runs on
      // the host here too, so a cached buffer is equally valid, and sharing the
      // function is what keeps the two paths from producing different masks
      form_ok = _render_drawn_mask_cached(self, piece, form, roi_in, roi_out,
                                         devid, mask);

      piece->blend_refine_guide_in = NULL;
      piece->blend_refine_guide_out = NULL;
      piece->blend_refine_roi_in = NULL;
      piece->blend_refine_roi_out = NULL;
      dt_free_align(guide_in);
      dt_free_align(guide_out);

      if(inverted)
      {
        // if we have a mask and this flag is set -> invert the mask
        dt_iop_image_invert(mask, 1.0f, owidth, oheight, 1); //mask[k] = 1.0f - mask[k]
      }
    }
    // mode_drawn, as in dt_develop_blend_process: a flexi mask has no
    // CONDITIONAL bit, and a flexi group that renders nothing must get the
    // CPU's `inverted ? 0 : 1` here, not the INCL fill below. The masks
    // verification tools replay the CPU path only
    else if(mode_drawn && dt_blend_may_render_group(self, mask_mode))
    {
      // no form defined but drawn mask active
      // we fill the buffer with 1.0f or 0.0f depending on mask_combine
      const float fill = inverted ? 0.0f : 1.0f;
      dt_iop_image_fill(mask, fill, owidth, oheight, 1); //mask[k] = fill;
    }
    else
    {
      // we fill the buffer with 1.0f or 0.0f depending on mask_combine
      const float fill = (d->mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f;
      dt_iop_image_fill(mask, fill, owidth, oheight, 1); //mask[k] = fill;
    }

    // as in dt_develop_blend_process: keep the two in step
    mask_is_uniform_fallback =
      mode_drawn && dt_blend_may_render_group(self, mask_mode) && !form_ok && !inverted
      && (global_refine_bypass || feqf(d->details, 0.0f, 1e-6f));

    dt_print_pipe(DT_DEBUG_PIPE,
       form && form_ok ? "blend with form" : "blend without form",
       piece->pipe, self, piece->pipe->devid, roi_in, roi_out, "%s, %s%s%s",
       dt_iop_colorspace_to_name(cst),
       _develop_blend_colorspace_to_str(blend_csp),
       inverted ? ", inverted" : "",
       rois_equal ? "" : ", roi differ");

    _refine_with_detail_mask_cl(self, piece, mask, roi_in, roi_out,
                                global_refine_bypass ? 0.0f : d->details, devid);

    err = dt_opencl_write_host_to_image(devid, mask, dev_mask_2, owidth, oheight, sizeof(float));
    if(err != CL_SUCCESS) goto error;

    // The following call to clFinish() works around a bug in some OpenCL
    // drivers (namely AMD).
    // Without this synchronization point, reads to dev_in would often not
    // return the correct value.
    // This depends on the module after which blending is called. One of the
    // affected ones is sharpen.
    dt_opencl_finish(devid);

    // get parametric mask (if any) and apply global opacity
    const uint32_t blendif = d->blendif;

    err = dt_opencl_enqueue_kernel_2d_args(devid, kernel_mask, owidth, oheight,
              CLARG(dev_in), CLARG(dev_out),
              CLARG(dev_mask_2), CLARG(dev_mask),
              CLARG(owidth), CLARG(oheight),
              CLARG(opacity),
              CLARG(blendif),
              CLARG(dev_blendif_params),
              CLARG(mask_mode), CLARG(d->mask_combine),
              CLARRAY(2, offs),
              CLARG(dev_profile_info), CLARG(dev_profile_lut), CLARG(use_profile));
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[opencl_blendop] apply global opacity: %s", cl_errstr(err));
      goto error;
    }
  }

  if(!uniform && post_operations_size)
  {
    // post processing the mask (it will always be stored in dev_mask)
    const int featherw = _required_w(d->feathering_radius,
                                     roi_out->scale / piece->iscale,
                                     MAX(owidth, oheight));
    const float sqrt_eps = _get_feathering_eps(piece);
    const float guide_weight = _get_guide_weight(piece);

    for(size_t index = 0; index < post_operations_size; index++)
    {
      _develop_mask_post_processing operation = post_operations[index];
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      if(operation == DEVELOP_MASK_POST_FEATHER_IN)
      {
        if(!rois_equal)
        {
          cl_mem dev_guide = dt_opencl_alloc_device(devid, owidth, oheight, sizeof(float) * ch);
          if(dev_guide == NULL) goto error;

          const size_t origin_1[2] = { dx, dy };
          err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_guide, CLIMG_ORIGIN, origin_1, region);
          if(err != CL_SUCCESS)
          {
            dt_opencl_release_mem_object(dev_guide);
            goto error;
          }
          err = guided_filter_cl(devid, dev_guide, dev_mask, dev_mask_2, owidth, oheight, ch,
                            featherw, sqrt_eps, guide_weight, 0.0f, 1.0f);
          dt_opencl_release_mem_object(dev_guide);
          if(err != CL_SUCCESS) goto error;
        }
        else
        {
          err = guided_filter_cl(devid, dev_in, dev_mask, dev_mask_2, owidth, oheight, ch,
                            featherw, sqrt_eps, guide_weight, 0.0f, 1.0f);
          if(err != CL_SUCCESS) goto error;
        }
      }
      else if(operation == DEVELOP_MASK_POST_FEATHER_OUT)
      {
        err = guided_filter_cl(devid, dev_out, dev_mask, dev_mask_2, owidth, oheight, ch,
                          featherw, sqrt_eps, guide_weight, 0.0f, 1.0f);
        if(err != CL_SUCCESS) goto error;
      }
      else if(operation == DEVELOP_MASK_POST_BLUR)
      {
        const float sigma = d->blur_radius * roi_out->scale / piece->iscale;
        const float mmax[] = { 1.0f };
        const float mmin[] = { 0.0f };

        dt_gaussian_cl_t *g = dt_gaussian_init_cl(devid, owidth, oheight, 1, mmax, mmin, sigma, 0);
        err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
        if(!g) goto error;
        err = dt_gaussian_blur_cl(g, dev_mask, dev_mask_2);
        dt_gaussian_free_cl(g);
        if(err != CL_SUCCESS)
        {
          dt_print(DT_DEBUG_OPENCL,
                   "[opencl_blendop] DEVELOP_MASK_POST_BLUR: %s", cl_errstr(err));
          goto error;
        }
      }
      else if(operation == DEVELOP_MASK_POST_TONE_CURVE)
      {
        const float e = expf(3.f * d->contrast);
        err = dt_opencl_enqueue_kernel_2d_args(devid, kernel_mask_tone_curve, owidth, oheight,
                                  CLARG(dev_mask), CLARG(dev_mask_2),
                                  CLARG(owidth), CLARG(oheight),
                                  CLARG(e), CLARG(d->brightness), CLARG(opacity));
        if(err != CL_SUCCESS)
        {
          dt_print(DT_DEBUG_OPENCL,
                   "[opencl_blendop] DEVELOP_MASK_POST_TONE_CURVE: %s", cl_errstr(err));
          goto error;
        }
      }
      _blend_process_cl_exchange(&dev_mask, &dev_mask_2);
    }
  }

  if(swap_mask)
  {
    dt_opencl_release_mem_object(dev_mask_2);
    dev_mask_2 = NULL;
  }

  // get temporary buffer for output image to overcome readonly/writeonly limitation
  err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  dev_tmp = dt_opencl_alloc_device(devid, owidth, oheight, sizeof(float) * ch);
  if(dev_tmp == NULL) goto error;
  err = dt_opencl_enqueue_copy_image(devid, dev_out, dev_tmp, CLIMG_ORIGIN, CLIMG_ORIGIN, region);
  if(err != CL_SUCCESS) goto error;

  if(request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY)
  {
    // load the boost factors in the device memory
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    dev_boost_factors = dt_opencl_copy_host_to_device_constant(devid, sizeof(d->blendif_boost_factors),
                                                               d->blendif_boost_factors);
    if(dev_boost_factors == NULL) goto error;

    // the display channel of Lab blending is generated in RGB and should be transformed to Lab
    // the transformation in the pipeline is currently always using the work profile
    dt_iop_order_iccprofile_info_t *work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
    const int use_work_profile = work_profile != NULL;

    err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid,
                                              &work_profile_info_cl,
                                              &work_profile_lut_cl,
                                              &dev_work_profile_info,
                                              &dev_work_profile_lut);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[opencl_blendop] work_profile_info_cl: %s", cl_errstr(err));
      goto error;
    }
    // let us display a specific channel
    err = dt_opencl_enqueue_kernel_2d_args(devid, kernel_display_channel, owidth, oheight,
                              CLARG(dev_in), CLARG(dev_tmp), CLARG(dev_mask),
                              CLARG(dev_out), CLARG(owidth), CLARG(oheight),
                              CLARRAY(2, offs), CLARG(request_mask_display),
                              CLARG(dev_boost_factors),
                              CLARG(dev_profile_info), CLARG(dev_profile_lut),
                              CLARG(use_profile), CLARG(dev_work_profile_info),
                              CLARG(dev_work_profile_lut), CLARG(use_work_profile));
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[opencl_blendop] kernel_display_channel: %s", cl_errstr(err));
      goto error;
    }
  }
  else
  {
    // apply blending with per-pixel opacity value as defined in dev_mask
    const float blend_parameter = exp2f(d->blend_parameter);
    err = dt_opencl_enqueue_kernel_2d_args(devid, kernel, owidth, oheight,
                              CLARG(dev_in), CLARG(dev_tmp), CLARG(dev_mask), CLARG(dev_out),
                              CLARG(owidth), CLARG(oheight), CLARG(d->blend_mode),
                              CLARG(blend_parameter),
                              CLARRAY(2, offs), CLARG(mask_display));
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[opencl_blendop] blend_parameter: %s", cl_errstr(err));
      goto error;
    }
  }

  // register if _this_ module should expose mask or display channel; the
  // uniform fallback drops only the mask overlay (see dt_develop_blend_process)
  const dt_dev_pixelpipe_display_mask_t shown_mask_display =
    mask_is_uniform_fallback ? request_mask_display & ~DT_DEV_PIXELPIPE_DISPLAY_MASK
                             : request_mask_display;
  if(shown_mask_display
     & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL))
  {
    piece->pipe->mask_display = shown_mask_display;
  }
  else if(request_raster_display
          & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL))
  {
    piece->pipe->mask_display = request_raster_display;
  }

  // check if we should store the mask for export or use in subsequent modules
  if(dt_iop_is_raster_mask_stored(piece, BLEND_RASTER_ID))
  {
    // Get back the final mask from the device as the raster mask.
    //
    // This must be done unconditionally to avoid presenting downstream a different
    // mask than the CPU one in the presence of refinements.
    err = dt_opencl_copy_image_to_host(devid, mask, dev_mask, owidth, oheight, sizeof(float));
    if(err != CL_SUCCESS)
    {
      dt_iop_piece_clear_raster(piece, _mask);
      goto error;
    }
    dt_iop_piece_set_raster(piece, mask, roi_in, roi_out);
  }
  else
    dt_iop_piece_clear_raster(piece, _mask);

  dt_opencl_release_mem_object(dev_blendif_params);
  dt_opencl_release_mem_object(dev_boost_factors);
  dt_opencl_release_mem_object(dev_mask);
  dt_opencl_release_mem_object(dev_mask_2);
  dt_opencl_release_mem_object(dev_tmp);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl,
                                     &dev_profile_info, &dev_profile_lut);
  dt_ioppr_free_iccprofile_params_cl(&work_profile_info_cl, &work_profile_lut_cl,
                                     &dev_work_profile_info,
                                     &dev_work_profile_lut);
  return TRUE;

error:
  // As we have not written the mask we must remove an existing one.
  dt_iop_piece_clear_raster(piece, _mask);
  dt_opencl_release_mem_object(dev_blendif_params);
  dt_opencl_release_mem_object(dev_boost_factors);
  dt_opencl_release_mem_object(dev_mask);
  dt_opencl_release_mem_object(dev_mask_2);
  dt_opencl_release_mem_object(dev_tmp);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl,
                                     &dev_profile_info, &dev_profile_lut);
  dt_ioppr_free_iccprofile_params_cl(&work_profile_info_cl, &work_profile_lut_cl,
                                     &dev_work_profile_info,
                                     &dev_work_profile_lut);
  dt_print(DT_DEBUG_OPENCL | DT_DEBUG_PIPE, "[opencl_blendop] error: %s", cl_errstr(err));
  return FALSE;
}
#endif

/** global init of blendops */
dt_blendop_cl_global_t *dt_develop_blend_init_cl_global(void)
{
#ifdef HAVE_OPENCL
  dt_blendop_cl_global_t *b = calloc(1, sizeof(dt_blendop_cl_global_t));

  const int program = 3; // blendop.cl, from programs.conf
  b->kernel_blendop_mask_Lab =
    dt_opencl_create_kernel(program, "blendop_mask_Lab");
  b->kernel_blendop_mask_RAW =
    dt_opencl_create_kernel(program, "blendop_mask_RAW");
  b->kernel_blendop_mask_rgb_hsl =
    dt_opencl_create_kernel(program, "blendop_mask_rgb_hsl");
  b->kernel_blendop_mask_rgb_jzczhz =
    dt_opencl_create_kernel(program, "blendop_mask_rgb_jzczhz");
  b->kernel_blendop_Lab =
    dt_opencl_create_kernel(program, "blendop_Lab");
  b->kernel_blendop_RAW =
    dt_opencl_create_kernel(program, "blendop_RAW");
  b->kernel_blendop_RAW4 =
    dt_opencl_create_kernel(program, "blendop_RAW4");
  b->kernel_blendop_rgb_hsl =
    dt_opencl_create_kernel(program, "blendop_rgb_hsl");
  b->kernel_blendop_rgb_jzczhz =
    dt_opencl_create_kernel(program, "blendop_rgb_jzczhz");
  b->kernel_blendop_mask_tone_curve =
    dt_opencl_create_kernel(program, "blendop_mask_tone_curve");
  b->kernel_blendop_set_mask =
    dt_opencl_create_kernel(program, "blendop_set_mask");
  b->kernel_blendop_display_channel =
    dt_opencl_create_kernel(program, "blendop_display_channel");
  b->kernel_calc_Y0_mask =
    dt_opencl_create_kernel(program, "calc_Y0_mask");
  b->kernel_calc_scharr_mask =
    dt_opencl_create_kernel(program, "calc_scharr_mask");
  b->kernel_calc_blend =
    dt_opencl_create_kernel(program, "calc_detail_blend");

  return b;
#else
  return NULL;
#endif
}

/** global cleanup of blendops */
void dt_develop_blend_free_cl_global(dt_blendop_cl_global_t *b)
{
#ifdef HAVE_OPENCL
  if(!b) return;

  dt_opencl_free_kernel(b->kernel_blendop_mask_Lab);
  dt_opencl_free_kernel(b->kernel_blendop_mask_RAW);
  dt_opencl_free_kernel(b->kernel_blendop_mask_rgb_hsl);
  dt_opencl_free_kernel(b->kernel_blendop_mask_rgb_jzczhz);
  dt_opencl_free_kernel(b->kernel_blendop_Lab);
  dt_opencl_free_kernel(b->kernel_blendop_RAW);
  dt_opencl_free_kernel(b->kernel_blendop_RAW4);
  dt_opencl_free_kernel(b->kernel_blendop_rgb_hsl);
  dt_opencl_free_kernel(b->kernel_blendop_rgb_jzczhz);
  dt_opencl_free_kernel(b->kernel_blendop_mask_tone_curve);
  dt_opencl_free_kernel(b->kernel_blendop_set_mask);
  dt_opencl_free_kernel(b->kernel_blendop_display_channel);
  dt_opencl_free_kernel(b->kernel_calc_Y0_mask);
  dt_opencl_free_kernel(b->kernel_calc_scharr_mask);
  dt_opencl_free_kernel(b->kernel_calc_blend);
  free(b);
#endif
}

/** blend version */
int dt_develop_blend_version(void)
{
  return DEVELOP_BLEND_VERSION;
}

/** report back specific memory requirements for blend step (only relevant for OpenCL path).
    We need this to calculate maximum CL mem requirements to be sure we can process the whole
    module incl blend processing on GPU as would do an early CPU fallback if requirements
    are not met.
*/
void tiling_callback_blendop(dt_iop_module_t *self,
                             dt_dev_pixelpipe_iop_t *piece,
                             const dt_iop_roi_t *roi_in,
                             const dt_iop_roi_t *roi_out,
                             dt_develop_tiling_t *tiling)
{
  tiling->factor = 0.0f;
  tiling->factor_cl = 0.0f;
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->align = 1;

  dt_develop_blend_params_t *const bldata = piece->blendop_data;
  if(bldata == NULL)
    return;

  if(bldata->details != 0.0f)
  {
    // details mask requires 2 additional quarter buffers of details data size
    // so normalize to roi_size
    dt_dev_detail_mask_t *details = &piece->pipe->scharr;
    if(details->data)
    {
      tiling->factor = 0.5f * (float)(details->roi.width * details->roi.height) / (roi_in->width * roi_in->height);
      tiling->factor_cl = tiling->factor;
    }
  }

  if(bldata->feathering_radius > 0.1f) // we don't feather below that
  {
    const int devid = piece->pipe->devid;
    if(devid > DT_DEVICE_CPU)
    {
      /* OpenCL feathering does simple internal tiling for less mem pressure,
         we still need some mem here for this.
      */
      tiling->factor_cl = MAX(tiling->factor_cl, 1.0f);
    }
    tiling->factor = MAX(tiling->factor, 18.0f * 0.25f); // we need all 18 intermediate guided filter mask buffers

    tiling->factor += 1.5f; // in + (guide, tmp) + two quarter buffers for the mask
    tiling->factor_cl += 1.5f;
  }

  const float outnorm = (float)(roi_out->width * roi_out->height) / (roi_in->width * roi_in->height);
  tiling->factor += outnorm;
  tiling->factor_cl += outnorm;
}

/** check if content of params is all zero, indicating a
   non-initialized set of blend parameters which needs special care. */
static gboolean _develop_blend_params_is_all_zero(const void *params, const size_t length)
{
  const char *data = (const char *)params;

  for(size_t k = 0; k < length; k++)
    if(data[k]) return FALSE;

  return TRUE;
}

static dt_develop_blend_mode_t _blend_legacy_blend_mode(const dt_develop_blend_mode_t legacy_blend_mode)
{
  dt_develop_blend_mode_t blend_mode = legacy_blend_mode & DEVELOP_BLEND_MODE_MASK;
  gboolean blend_reverse = FALSE;
  switch(blend_mode) {
    case DEVELOP_BLEND_NORMAL_OBSOLETE:
      blend_mode = DEVELOP_BLEND_BOUNDED;
      break;
    case DEVELOP_BLEND_INVERSE_OBSOLETE:
      blend_mode = DEVELOP_BLEND_BOUNDED;
      blend_reverse = TRUE;
      break;
    case DEVELOP_BLEND_DISABLED_OBSOLETE:
    case DEVELOP_BLEND_UNBOUNDED_OBSOLETE:
      blend_mode = DEVELOP_BLEND_NORMAL2;
      break;
    case DEVELOP_BLEND_MULTIPLY_REVERSE_OBSOLETE:
      blend_mode = DEVELOP_BLEND_MULTIPLY;
      blend_reverse = TRUE;
      break;
    default:
      break;
  }
  return (blend_reverse ? DEVELOP_BLEND_REVERSE : 0) | blend_mode;
}

static void _fix_masks_combine(dt_develop_blend_params_t *bp)
{
  // only for drawn masks where DEVELOP_COMBINE_INV has been
  // deprecated.

  if(bp->mask_mode & DEVELOP_MASK_MASK)
  {
    // if set we replace it with DEVELOP_COMBINE_MASKS_POS
    // both DEVELOP_COMBINE_INV & DEVELOP_COMBINE_MASKS_POS are giving
    // the very same result.
    const gboolean m_inv = bp->mask_combine & DEVELOP_COMBINE_INV;
    const gboolean m_pos = bp->mask_combine & DEVELOP_COMBINE_MASKS_POS;

    if(m_inv && !m_pos)
    {
      // remove INV add POS to give the same effect
      bp->mask_combine &= ~DEVELOP_COMBINE_INV;
      bp->mask_combine |= DEVELOP_COMBINE_MASKS_POS;
    }
    else if(m_inv && m_pos)
    {
      // both set, remove INV and remove POS, invert of invert is a nop
      bp->mask_combine &= ~DEVELOP_COMBINE_INV;
      bp->mask_combine &= ~DEVELOP_COMBINE_MASKS_POS;
    }
  }
}

static void _fix_raster_blend(dt_develop_blend_params_t *n)
{
  if(n->mask_mode & DEVELOP_MASK_RASTER)
  {
    n->details = 0.0f;
    n->feathering_radius = 0.0f;
    n->blur_radius = 0.0f;
    n->contrast = 0.0f;
    n->brightness = 0.0f;
    n->feathering_guide = DEVELOP_MASK_GUIDE_IN_AFTER_BLUR;
  }
}

/** update the blendop params layout to the current version.
    dt_develop_blend_legacy_params_ext() below then migrates the mask */
static gboolean _develop_blend_legacy_params_convert(dt_iop_module_t *module,
                                                      const void *const old_params,
                                                      const int old_version,
                                                      void *new_params,
                                                      const int new_version,
                                                      const int length)
{
  // edits before version 10 default to a display referred workflow
  dt_develop_blend_colorspace_t cst = _blend_default_module_blend_colorspace(module, 0);

  dt_develop_blend_params_t default_display_blend_params;
  dt_develop_blend_init_blend_parameters(&default_display_blend_params, cst);

  // first deal with all-zero parameter sets, regardless of version
  // number.  these occurred in previous darktable versions when
  // modules without blend support stored zero-initialized data in
  // history stack. that's no problem unless the module gets blend
  // support later (e.g. module exposure).  remedy: we simply
  // initialize with the current default blend params in this case.
  if(_develop_blend_params_is_all_zero(old_params, length))
  {
    dt_develop_blend_params_t *n = new_params;

    *n = default_display_blend_params;
    return FALSE;
  }

  if(old_version == 1 && new_version == DEVELOP_BLEND_VERSION)
  {
    /** blend legacy parameters version 1 */
    typedef struct dt_develop_blend_params1_t
    {
      uint32_t mode;
      float opacity;
      dt_mask_id_t mask_id;
    } dt_develop_blend_params1_t;

    if(length != sizeof(dt_develop_blend_params1_t)) return TRUE;

    dt_develop_blend_params1_t *o = (dt_develop_blend_params1_t *)old_params;
    dt_develop_blend_params_t *n = new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = (o->mode == DEVELOP_BLEND_DISABLED_OBSOLETE)
      ? DEVELOP_MASK_DISABLED
      : DEVELOP_MASK_ENABLED;
    n->blend_mode = _blend_legacy_blend_mode(o->mode);
    n->opacity = o->opacity;
    n->mask_id = o->mask_id;
    n->feather_version = 0;
    return FALSE;
  }

  if(old_version == 2 && new_version == DEVELOP_BLEND_VERSION)
  {
    /** blend legacy parameters version 2 */
    typedef struct dt_develop_blend_params2_t
    {
      /** blending mode */
      uint32_t mode;
      /** mixing opacity */
      float opacity;
      /** id of mask in current pipeline */
      dt_mask_id_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** blendif parameters */
      float blendif_parameters[4 * 8];
    } dt_develop_blend_params2_t;

    if(length != sizeof(dt_develop_blend_params2_t)) return TRUE;

    dt_develop_blend_params2_t *o = (dt_develop_blend_params2_t *)old_params;
    dt_develop_blend_params_t *n = new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = (o->mode == DEVELOP_BLEND_DISABLED_OBSOLETE)
      ? DEVELOP_MASK_DISABLED
      : DEVELOP_MASK_ENABLED;
    n->mask_mode |= ((o->blendif & (1u << DEVELOP_BLENDIF_active))
                     && (n->mask_mode == DEVELOP_MASK_ENABLED))
      ? DEVELOP_MASK_CONDITIONAL
      : 0;
    n->blend_mode = _blend_legacy_blend_mode(o->mode);
    n->opacity = o->opacity;
    n->mask_id = o->mask_id;
    n->blendif = o->blendif & 0xff; // only just in case: knock out all bits
                                    // which were undefined in version
                                    // 2; also switch off old "active" bit
    for(int i = 0; i < (4 * 8); i++) n->blendif_parameters[i] = o->blendif_parameters[i];

    n->feather_version = 0;
    return FALSE;
  }

  if(old_version == 3 && new_version == DEVELOP_BLEND_VERSION)
  {
    /** blend legacy parameters version 3 */
    typedef struct dt_develop_blend_params3_t
    {
      /** blending mode */
      uint32_t mode;
      /** mixing opacity */
      float opacity;
      /** id of mask in current pipeline */
      dt_mask_id_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
    } dt_develop_blend_params3_t;

    if(length != sizeof(dt_develop_blend_params3_t)) return TRUE;

    dt_develop_blend_params3_t *o = (dt_develop_blend_params3_t *)old_params;
    dt_develop_blend_params_t *n = new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = (o->mode == DEVELOP_BLEND_DISABLED_OBSOLETE)
      ? DEVELOP_MASK_DISABLED
      : DEVELOP_MASK_ENABLED;
    n->mask_mode |= ((o->blendif & (1u << DEVELOP_BLENDIF_active))
                     && (n->mask_mode == DEVELOP_MASK_ENABLED))
      ? DEVELOP_MASK_CONDITIONAL
      : 0;
    n->blend_mode = _blend_legacy_blend_mode(o->mode);
    n->opacity = o->opacity;
    n->mask_id = o->mask_id;
    // knock out old unused "active" flag
    n->blendif = o->blendif & ~(1u << DEVELOP_BLENDIF_active);
    memcpy(n->blendif_parameters, o->blendif_parameters,
           sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);

    n->feather_version = 0;
    return FALSE;
  }

  if(old_version == 4 && new_version == DEVELOP_BLEND_VERSION)
  {
    /** blend legacy parameters version 4 */
    typedef struct dt_develop_blend_params4_t
    {
      /** blending mode */
      uint32_t mode;
      /** mixing opacity */
      float opacity;
      /** id of mask in current pipeline */
      dt_mask_id_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** blur radius */
      float radius;
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
    } dt_develop_blend_params4_t;

    if(length != sizeof(dt_develop_blend_params4_t)) return TRUE;

    dt_develop_blend_params4_t *o = (dt_develop_blend_params4_t *)old_params;
    dt_develop_blend_params_t *n = new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = (o->mode == DEVELOP_BLEND_DISABLED_OBSOLETE)
      ? DEVELOP_MASK_DISABLED
      : DEVELOP_MASK_ENABLED;
    n->mask_mode |= ((o->blendif & (1u << DEVELOP_BLENDIF_active))
                     && (n->mask_mode == DEVELOP_MASK_ENABLED))
      ? DEVELOP_MASK_CONDITIONAL
      : 0;
    n->blend_mode = _blend_legacy_blend_mode(o->mode);
    n->opacity = o->opacity;
    n->mask_id = o->mask_id;
    n->blur_radius = o->radius;
    // knock out old unused "active" flag
    n->blendif = o->blendif & ~(1u << DEVELOP_BLENDIF_active);
    memcpy(n->blendif_parameters, o->blendif_parameters,
           sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);
    n->feather_version = 0;
    return FALSE;
  }

  if(old_version == 5 && new_version == DEVELOP_BLEND_VERSION)
  {
    /** blend legacy parameters version 5 (identical to version 6)*/
    typedef struct dt_develop_blend_params5_t
    {
      /** what kind of masking to use: off, non-mask (uniformly),
       * hand-drawn mask and/or conditional mask */
      uint32_t mask_mode;
      /** blending mode */
      uint32_t blend_mode;
      /** mixing opacity */
      float opacity;
      /** how masks are combined */
      uint32_t mask_combine;
      /** id of mask in current pipeline */
      dt_mask_id_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** blur radius */
      float radius;
      /** some reserved fields for future use */
      uint32_t reserved[4];
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
    } dt_develop_blend_params5_t;

    if(length != sizeof(dt_develop_blend_params5_t)) return TRUE;

    dt_develop_blend_params5_t *o = (dt_develop_blend_params5_t *)old_params;
    dt_develop_blend_params_t *n = new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_mode = _blend_legacy_blend_mode(o->blend_mode);
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blur_radius = o->radius;
    // this is needed as version 5 contained a bug which screwed up history
    // stacks of even older
    // versions. potentially bad history stacks can be identified by an active
    // bit no. 32 in blendif.
    n->blendif = (o->blendif & (1u << DEVELOP_BLENDIF_active)
                  ? o->blendif | 31
                  : o->blendif)
      & ~(1u << DEVELOP_BLENDIF_active);
    memcpy(n->blendif_parameters, o->blendif_parameters,
           sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);
    n->feather_version = 0;
    _fix_masks_combine(n);
    return FALSE;
  }

  if(old_version == 6 && new_version == DEVELOP_BLEND_VERSION)
  {
    /** blend legacy parameters version 6 (identical to version 7) */
    typedef struct dt_develop_blend_params6_t
    {
      /** what kind of masking to use: off, non-mask (uniformly),
       * hand-drawn mask and/or conditional mask */
      uint32_t mask_mode;
      /** blending mode */
      uint32_t blend_mode;
      /** mixing opacity */
      float opacity;
      /** how masks are combined */
      uint32_t mask_combine;
      /** id of mask in current pipeline */
      dt_mask_id_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** blur radius */
      float radius;
      /** some reserved fields for future use */
      uint32_t reserved[4];
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
    } dt_develop_blend_params6_t;

    if(length != sizeof(dt_develop_blend_params6_t)) return TRUE;

    dt_develop_blend_params6_t *o = (dt_develop_blend_params6_t *)old_params;
    dt_develop_blend_params_t *n = new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_mode = _blend_legacy_blend_mode(o->blend_mode);
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blur_radius = o->radius;
    n->blendif = o->blendif;
    memcpy(n->blendif_parameters, o->blendif_parameters,
           sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);
    n->feather_version = 0;
    _fix_masks_combine(n);
    return FALSE;
  }

  if(old_version == 7 && new_version == DEVELOP_BLEND_VERSION)
  {
    /** blend legacy parameters version 7 */
    typedef struct dt_develop_blend_params7_t
    {
      /** what kind of masking to use: off, non-mask (uniformly),
       * hand-drawn mask and/or conditional mask */
      uint32_t mask_mode;
      /** blending mode */
      uint32_t blend_mode;
      /** mixing opacity */
      float opacity;
      /** how masks are combined */
      uint32_t mask_combine;
      /** id of mask in current pipeline */
      dt_mask_id_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** blur radius */
      float radius;
      /** some reserved fields for future use */
      uint32_t reserved[4];
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
    } dt_develop_blend_params7_t;

    if(length != sizeof(dt_develop_blend_params7_t)) return TRUE;

    dt_develop_blend_params7_t *o = (dt_develop_blend_params7_t *)old_params;
    dt_develop_blend_params_t *n = new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_mode = _blend_legacy_blend_mode(o->blend_mode);
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blur_radius = o->radius;
    n->blendif = o->blendif;
    memcpy(n->blendif_parameters, o->blendif_parameters,
           sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);
    n->feather_version = 0;
    _fix_masks_combine(n);
    return FALSE;
  }

  if(old_version == 8 && new_version == DEVELOP_BLEND_VERSION)
  {
    /** blend legacy parameters version 8 */
    typedef struct dt_develop_blend_params8_t
    {
      /** what kind of masking to use: off, non-mask (uniformly),
       * hand-drawn mask and/or conditional mask */
      uint32_t mask_mode;
      /** blending mode */
      uint32_t blend_mode;
      /** mixing opacity */
      float opacity;
      /** how masks are combined */
      uint32_t mask_combine;
      /** id of mask in current pipeline */
      dt_mask_id_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** feathering radius */
      float feathering_radius;
      /** feathering guide */
      uint32_t feathering_guide;
      /** blur radius */
      float blur_radius;
      /** mask contrast enhancement */
      float contrast;
      /** mask brightness adjustment */
      float brightness;
      /** some reserved fields for future use */
      uint32_t reserved[4];
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
    } dt_develop_blend_params8_t;

    if(length != sizeof(dt_develop_blend_params8_t)) return TRUE;

    dt_develop_blend_params8_t *o = (dt_develop_blend_params8_t *)old_params;
    dt_develop_blend_params_t *n = new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_mode = _blend_legacy_blend_mode(o->blend_mode);
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blendif = o->blendif;
    n->feathering_radius = o->feathering_radius;
    n->feathering_guide = o->feathering_guide;
    n->blur_radius = o->blur_radius;
    n->contrast = o->contrast;
    n->brightness = o->brightness;
    memcpy(n->blendif_parameters, o->blendif_parameters,
           sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);
    n->feather_version = 0;
    _fix_masks_combine(n);
    return FALSE;
  }

  if(old_version == 9 && new_version == DEVELOP_BLEND_VERSION)
  {
    /** blend legacy parameters version 9 */
    typedef struct dt_develop_blend_params9_t
    {
      /** what kind of masking to use: off, non-mask (uniformly),
       *  hand-drawn mask and/or conditional mask or raster mask */
      uint32_t mask_mode;
      /** blending mode */
      uint32_t blend_mode;
      /** mixing opacity */
      float opacity;
      /** how masks are combined */
      uint32_t mask_combine;
      /** id of mask in current pipeline */
      dt_mask_id_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** feathering radius */
      float feathering_radius;
      /** feathering guide */
      uint32_t feathering_guide;
      /** blur radius */
      float blur_radius;
      /** mask contrast enhancement */
      float contrast;
      /** mask brightness adjustment */
      float brightness;
      /** some reserved fields for future use */
      uint32_t reserved[4];
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
      dt_dev_operation_t raster_mask_source;
      int raster_mask_instance;
      dt_mask_id_t raster_mask_id;
      gboolean raster_mask_invert;
    } dt_develop_blend_params9_t;

    if(length != sizeof(dt_develop_blend_params9_t)) return TRUE;

    dt_develop_blend_params9_t *o = (dt_develop_blend_params9_t *)old_params;
    dt_develop_blend_params_t *n = new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_mode = _blend_legacy_blend_mode(o->blend_mode);
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blendif = o->blendif;
    n->feathering_radius = o->feathering_radius;
    n->feathering_guide = o->feathering_guide;
    n->blur_radius = o->blur_radius;
    n->contrast = o->contrast;
    n->brightness = o->brightness;
    memcpy(n->blendif_parameters, o->blendif_parameters,
           sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);
    memcpy(n->raster_mask_source, o->raster_mask_source,
           sizeof(n->raster_mask_source));
    n->raster_mask_instance = o->raster_mask_instance;
    n->raster_mask_id = o->raster_mask_source[0] ? o->raster_mask_id : INVALID_MASKID;
    n->raster_mask_invert = o->raster_mask_invert;
    n->feather_version = 0;
    _fix_masks_combine(n);
    _fix_raster_blend(n);
    return FALSE;
  }

  if(old_version == 10 && new_version == DEVELOP_BLEND_VERSION)
  {
    /** blend legacy parameters version 10 */
    typedef struct dt_develop_blend_params10_t
    {
      /** what kind of masking to use: off, non-mask (uniformly),
       *  hand-drawn mask and/or conditional mask or raster mask */
      uint32_t mask_mode;
      /** blending color space type */
      int32_t blend_cst;
      /** blending mode */
      uint32_t blend_mode;
      /** parameter for the blending */
      float blend_parameter;
      /** mixing opacity */
      float opacity;
      /** how masks are combined */
      uint32_t mask_combine;
      /** id of mask in current pipeline */
      dt_mask_id_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** feathering radius */
      float feathering_radius;
      /** feathering guide */
      uint32_t feathering_guide;
      /** blur radius */
      float blur_radius;
      /** mask contrast enhancement */
      float contrast;
      /** mask brightness adjustment */
      float brightness;
      /** some reserved fields for future use */
      uint32_t reserved[4];
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
      float blendif_boost_factors[DEVELOP_BLENDIF_SIZE];
      dt_dev_operation_t raster_mask_source;
      int raster_mask_instance;
      dt_mask_id_t raster_mask_id;
      gboolean raster_mask_invert;
    } dt_develop_blend_params10_t;

    if(length != sizeof(dt_develop_blend_params10_t)) return TRUE;

    dt_develop_blend_params10_t *o = (dt_develop_blend_params10_t *)old_params;
    dt_develop_blend_params_t *n = new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_cst = o->blend_cst;
    n->blend_mode = _blend_legacy_blend_mode(o->blend_mode);
    n->blend_parameter = o->blend_parameter;
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blendif = o->blendif;
    n->feathering_radius = o->feathering_radius;
    n->feathering_guide = o->feathering_guide;
    n->blur_radius = o->blur_radius;
    n->contrast = o->contrast;
    n->brightness = o->brightness;
    // fix intermediate devel versions for details mask and initialize
    // n->details to proper values if something was wrong
    memcpy(&n->details, &o->reserved, sizeof(float));
    if(dt_isnan(n->details)) n->details = 0.0f;
    n->details = fminf(1.0f, fmaxf(-1.0f, n->details));

    memcpy(n->blendif_parameters, o->blendif_parameters,
           sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);
    memcpy(n->blendif_boost_factors, o->blendif_boost_factors,
           sizeof(float) * DEVELOP_BLENDIF_SIZE);
    memcpy(n->raster_mask_source, o->raster_mask_source,
           sizeof(n->raster_mask_source));
    n->raster_mask_instance = o->raster_mask_instance;
    n->raster_mask_id = o->raster_mask_source[0] ? o->raster_mask_id : INVALID_MASKID;
    n->raster_mask_invert = o->raster_mask_invert;
    n->feather_version = 0;

    _fix_masks_combine(n);
    _fix_raster_blend(n);
    return FALSE;
  }
  if(old_version == 11 && new_version == DEVELOP_BLEND_VERSION)
  {
    if(length != sizeof(dt_develop_blend_params_t)) return TRUE;

    dt_develop_blend_params_t *o = (dt_develop_blend_params_t *)old_params;
    dt_develop_blend_params_t *n = new_params;

    *n = *o;
    _fix_masks_combine(n);
    n->raster_mask_id = o->raster_mask_source[0] ? o->raster_mask_id : INVALID_MASKID;
    n->feather_version = 0;
    _fix_raster_blend(n);
    return FALSE;
  }
  if(old_version == 12 && new_version == DEVELOP_BLEND_VERSION)
  {
    if(length != sizeof(dt_develop_blend_params_t)) return TRUE;

    dt_develop_blend_params_t *o = (dt_develop_blend_params_t *)old_params;
    dt_develop_blend_params_t *n = new_params;

    *n = *o;
    n->raster_mask_id = o->raster_mask_source[0] ? o->raster_mask_id : INVALID_MASKID;
    n->feather_version = 0;
    _fix_raster_blend(n);
    return FALSE;
  }
  if(old_version == 13 && new_version == DEVELOP_BLEND_VERSION)
  {
    if(length != sizeof(dt_develop_blend_params_t)) return TRUE;

    dt_develop_blend_params_t *o = (dt_develop_blend_params_t *)old_params;
    dt_develop_blend_params_t *n = new_params;

    *n = *o;
    _fix_raster_blend(n);
    return FALSE;
  }
  if(old_version == 14 && new_version == DEVELOP_BLEND_VERSION)
  {
    if(length != sizeof(dt_develop_blend_params_t)) return TRUE;

    dt_develop_blend_params_t *o = (dt_develop_blend_params_t *)old_params;
    dt_develop_blend_params_t *n = new_params;

    *n = *o;
    return FALSE;
  }

  return TRUE;
}

gboolean dt_develop_blend_legacy_params_ext(dt_iop_module_t *module,
                                            const void *const old_params,
                                            const int old_version,
                                            void *new_params,
                                            const int new_version,
                                            const int length,
                                            const int history_num)
{
  const gboolean failed = _develop_blend_legacy_params_convert(module, old_params, old_version,
                                                                new_params, new_version, length);
  if(failed) return TRUE;

  // every conversion goes to the current version, so new_params has the
  // current layout, maybe with a classic mask_mode, whatever version it came
  // from. Migration cannot fail
  dt_develop_blend_params_t *n = new_params;
  dt_masks_migrate_classic_to_flexi(module, n, history_num);
  // every conversion is from a version older than the lock, whose reserved
  // field it took over
  n->mask_lock = 0;
  return FALSE;
}

void dt_develop_blend_keep_locked_mask(dt_develop_blend_params_t *incoming,
                                       const dt_develop_blend_params_t *const locked)
{
  if(!dt_develop_blend_mask_locked(locked))
  {
    incoming->mask_lock = 0;
    return;
  }

  const uint32_t blend_mode = incoming->blend_mode;
  const float blend_parameter = incoming->blend_parameter;
  const float opacity = incoming->opacity;
  *incoming = *locked;
  incoming->blend_mode = blend_mode;
  incoming->blend_parameter = blend_parameter;
  incoming->opacity = opacity;
}

gboolean dt_develop_blend_legacy_params(dt_iop_module_t *module,
                                        const void *const old_params,
                                        const int old_version,
                                        void *new_params,
                                        const int new_version,
                                        const int length)
{
  return dt_develop_blend_legacy_params_ext(module, old_params, old_version,
                                            new_params, new_version, length, -1);
}

gboolean dt_develop_blend_legacy_params_from_so(dt_iop_module_so_t *module_so,
                                                const void *const old_params,
                                                const int old_version,
                                                void *new_params,
                                                const int new_version,
                                                const int length)
{
  // we need a dt_iop_module_t for dt_develop_blend_legacy_params()
  dt_iop_module_t *module = calloc(1, sizeof(dt_iop_module_t));
  if(dt_iop_load_module_by_so(module, module_so, NULL))
  {
    free(module);
    return TRUE;
  }

  if(module->params_size == 0)
  {
    dt_iop_cleanup_module(module);
    free(module);
    return TRUE;
  }

  // convert the old blend params to new
  const gboolean res = dt_develop_blend_legacy_params(module, old_params, old_version,
                                                 new_params, dt_develop_blend_version(),
                                                 length);
  dt_iop_cleanup_module(module);
  free(module);
  return res;
}

// tools/update_modelines.sh
// remove-trailing-space on;
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
