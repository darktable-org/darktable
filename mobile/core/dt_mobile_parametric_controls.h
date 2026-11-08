/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_PARAMETRIC_CONTROLS_H
#define DT_MOBILE_PARAMETRIC_CONTROLS_H

/* These are the original blend-if channels, not replacement mask formulas.
 * Match blend_gui.c's channel membership, four ordered gradient handles,
 * neutral unused-channel polarity and boost-coordinate transformation. */
static gboolean mobile_parametric_available(dt_iop_module_t *module)
{
  if(!mobile_blending_available(module)) return FALSE;
  const dt_iop_colorspace_type_t cst = module->blend_colorspace(module, NULL, NULL);
  return cst == IOP_CS_LAB || cst == IOP_CS_RGB;
}

static uint32_t mobile_parametric_channel_mask(dt_iop_module_t *module)
{
  return mobile_blending_colorspace(module) == DEVELOP_BLEND_CS_LAB
    ? DEVELOP_BLENDIF_Lab_MASK : DEVELOP_BLENDIF_RGB_MASK;
}

static const char *mobile_parametric_channel_name(dt_iop_module_t *module, unsigned int channel)
{
  static const char *const lab[] = {
    "input lightness", "input green/red", "input blue/yellow", NULL,
    "output lightness", "output green/red", "output blue/yellow", NULL,
    "input saturation", "input hue", NULL, NULL,
    "output saturation", "output hue", NULL
  };
  static const char *const display[] = {
    "input gray", "input red", "input green", "input blue",
    "output gray", "output red", "output green", "output blue",
    "input hue", "input chroma", "input luminance", NULL,
    "output hue", "output chroma", "output luminance"
  };
  static const char *const scene[] = {
    "input gray", "input red", "input green", "input blue",
    "output gray", "output red", "output green", "output blue",
    "input Jz luminance", "input Cz chroma", "input hue", NULL,
    "output Jz luminance", "output Cz chroma", "output hue"
  };
  if(channel > DEVELOP_BLENDIF_MAX) return NULL;
  switch(mobile_blending_colorspace(module))
  {
    case DEVELOP_BLEND_CS_LAB: return lab[channel];
    case DEVELOP_BLEND_CS_RGB_DISPLAY: return display[channel];
    case DEVELOP_BLEND_CS_RGB_SCENE: return scene[channel];
    default: return NULL;
  }
}

static gboolean mobile_parametric_boost_available(dt_iop_module_t *module, unsigned int channel)
{
  const unsigned int input_channel = channel & ~4u;
  const dt_develop_blend_colorspace_t cst = mobile_blending_colorspace(module);
  return cst == DEVELOP_BLEND_CS_LAB ? input_channel != DEVELOP_BLENDIF_h_in
    : cst == DEVELOP_BLEND_CS_RGB_DISPLAY ? input_channel <= DEVELOP_BLENDIF_BLUE_in
    : cst == DEVELOP_BLEND_CS_RGB_SCENE ? input_channel != DEVELOP_BLENDIF_hz_in : FALSE;
}

static float mobile_parametric_boost_offset(dt_iop_module_t *module, unsigned int channel)
{
  const unsigned int input_channel = channel & ~4u;
  return mobile_blending_colorspace(module) == DEVELOP_BLEND_CS_RGB_SCENE
    && (input_channel == DEVELOP_BLENDIF_Jz_in || input_channel == DEVELOP_BLENDIF_Cz_in)
      ? -6.64385619f : 0.0f;
}

static gboolean mobile_parametric_thresholds_valid(const float *values)
{
  for(size_t k = 0; k < 4; k++)
    if(!isfinite(values[k]) || values[k] < 0 || values[k] > 1 || (k && values[k] < values[k - 1])) return FALSE;
  return TRUE;
}

static gboolean mobile_parametric_active_valid(dt_iop_module_t *module, const dt_develop_blend_params_t *p)
{
  const uint32_t active = mobile_parametric_channel_mask(module) & p->blendif;
  for(unsigned int channel = 0; channel <= DEVELOP_BLENDIF_MAX; channel++)
  {
    if(!(active & (1u << channel))) continue;
    const float boost = p->blendif_boost_factors[channel];
    const float offset = mobile_parametric_boost_offset(module, channel);
    if(!mobile_parametric_thresholds_valid(p->blendif_parameters + 4 * channel)
       || !isfinite(boost) || boost < offset || boost > 18 + offset) return FALSE;
  }
  return TRUE;
}

static void mobile_parametric_update_active(dt_develop_blend_params_t *p, unsigned int channel)
{
  const float *values = p->blendif_parameters + 4 * channel;
  if(values[1] == 0.0f && values[2] == 1.0f) p->blendif &= ~(1u << channel);
  else p->blendif |= 1u << channel;
}

/* Like the original combine-mode callback, unused channels must be neutral in
 * union mode. Leaving a full-span channel with the wrong polarity cancels the
 * entire conditional mask in the original processor. */
static void mobile_parametric_neutral_channels(dt_iop_module_t *module, dt_develop_blend_params_t *p)
{
  const uint32_t unused = mobile_parametric_channel_mask(module) & ~p->blendif;
  p->blendif &= ~(unused << 16);
  if(p->mask_combine & DEVELOP_COMBINE_INCL) p->blendif |= unused << 16;
}

static void mobile_parametric_activate(dt_develop_blend_params_t *p)
{
  p->mask_mode = (p->mask_mode & ~DEVELOP_MASK_RASTER) | DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  /* Raster masks are an exclusive mode. Clear their source registration as
   * well as the mode bit; the original commit helper uses the stored source. */
  memset(p->raster_mask_source, 0, sizeof(p->raster_mask_source));
  p->raster_mask_instance = 0;
  p->raster_mask_id = INVALID_MASKID;
}

static void append_mobile_blend_enum(parameter_json *description, const char *name, const char *label,
                                     unsigned int value, unsigned int original,
                                     const unsigned int *values, const char *const *labels, size_t count)
{
  if(!description->first) g_string_append_c(description->json, ',');
  description->first = FALSE;
  GString *json = description->json;
  g_string_append(json, "{\"name\":"); json_string(json, name);
  g_string_append(json, ",\"label\":"); json_string(json, label);
  g_string_append_printf(json, ",\"type\":\"enum\",\"value\":%u,\"min\":%u,\"max\":%u",
                        value, values[0], values[count - 1]);
  for(size_t k = 0; k < count; k++)
    if(values[k] == original) { g_string_append_printf(json, ",\"default\":%u", original); break; }
  g_string_append(json, ",\"choices\":[");
  for(size_t k = 0; k < count; k++)
  {
    if(k) g_string_append_c(json, ',');
    g_string_append_printf(json, "{\"value\":%u,\"label\":", values[k]); json_string(json, labels[k]);
    g_string_append_c(json, '}');
  }
  g_string_append(json, "]}");
}

static void append_mobile_parametric_parameters(dt_iop_module_t *module, parameter_json *description)
{
  const dt_develop_blend_params_t *p = module->blend_params, *d = module->default_blendop_params;
  if(mobile_parametric_available(module))
  {
    append_mobile_blend_number(description, "blend.parametric.enabled", "parametric mask", "bool",
      !!(p->mask_mode & DEVELOP_MASK_CONDITIONAL), 0, 1, !!(d->mask_mode & DEVELOP_MASK_CONDITIONAL));
    if(p->mask_mode & DEVELOP_MASK_CONDITIONAL)
    {
      append_mobile_blend_number(description, "blend.parametric.reset", "reset all parametric channels", "action",
        0, 0, 1, NAN);
      const unsigned int combine_values[] = { 0, 1, 2, 3 };
      const char *const combine_labels[] = { "exclusive", "exclusive inverted", "inclusive", "inclusive inverted" };
      append_mobile_blend_enum(description, "blend.parametric.combine", "combine parametric and drawn masks",
        p->mask_combine & 3u, d->mask_combine & 3u, combine_values, combine_labels, 4);
      static const char *const handle_names[] = { "lower outer", "lower inner", "upper inner", "upper outer" };
      const uint32_t channels = mobile_parametric_channel_mask(module);
      for(unsigned int channel = 0; channel <= DEVELOP_BLENDIF_MAX; channel++)
      {
        if(!(channels & (1u << channel))) continue;
        const char *label = mobile_parametric_channel_name(module, channel);
        if(!label) continue;
        char name[80], title[160];
        snprintf(name, sizeof(name), "blend.parametric.%u.enabled", channel);
        snprintf(title, sizeof(title), "%s · channel enabled", label);
        append_mobile_blend_number(description, name, title, "bool", !!(p->blendif & (1u << channel)), 0, 1,
          !!(d->blendif & (1u << channel)));
        snprintf(name, sizeof(name), "blend.parametric.%u.inverted", channel);
        snprintf(title, sizeof(title), "%s · invert channel", label);
        append_mobile_blend_number(description, name, title, "bool", !!(p->blendif & (1u << (channel + 16))), 0, 1,
          !!(d->blendif & (1u << (channel + 16))) || !!(p->mask_combine & DEVELOP_COMBINE_INCL));
        for(size_t handle = 0; handle < 4; handle++)
        {
          snprintf(name, sizeof(name), "blend.parametric.%u.p%zu", channel, handle);
          snprintf(title, sizeof(title), "%s · %s", label, handle_names[handle]);
          append_mobile_blend_number(description, name, title, "float", p->blendif_parameters[4 * channel + handle],
            0, 1, d->blendif_parameters[4 * channel + handle]);
        }
        if(mobile_parametric_boost_available(module, channel))
        {
          const float offset = mobile_parametric_boost_offset(module, channel);
          snprintf(name, sizeof(name), "blend.parametric.%u.boost_ev", channel);
          snprintf(title, sizeof(title), "%s · boost (EV)", label);
          append_mobile_blend_number(description, name, title, "float", p->blendif_boost_factors[channel] - offset,
            0, 18, d->blendif_boost_factors[channel] - offset);
        }
        snprintf(name, sizeof(name), "blend.parametric.%u.reset", channel);
        snprintf(title, sizeof(title), "%s · reset channel", label);
        append_mobile_blend_number(description, name, title, "action", 0, 0, 1, NAN);
      }
    }
  }
  if(!(p->mask_mode & (DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL | DEVELOP_MASK_RASTER))) return;
  append_mobile_blend_number(description, "blend.mask.blur_radius", "mask blur radius (px)", "float",
    p->blur_radius, 0, 100, d->blur_radius);
  if(mobile_blending_colorspace(module) == DEVELOP_BLEND_CS_RAW) return;
  const unsigned int guide_values[] = { DEVELOP_MASK_GUIDE_IN_BEFORE_BLUR, DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR,
    DEVELOP_MASK_GUIDE_IN_AFTER_BLUR, DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR };
  const char *const guide_labels[] = { "input before blur", "output before blur", "input after blur", "output after blur" };
  append_mobile_blend_enum(description, "blend.mask.feather_guide", "mask feathering guide", p->feathering_guide,
    d->feathering_guide, guide_values, guide_labels, 4);
  append_mobile_blend_number(description, "blend.mask.feather_radius", "mask feathering radius (px)", "float",
    p->feathering_radius, 0, 250, d->feathering_radius);
  append_mobile_blend_number(description, "blend.mask.brightness", "mask brightness", "float",
    p->brightness, -1, 1, d->brightness);
  append_mobile_blend_number(description, "blend.mask.contrast", "mask contrast", "float",
    p->contrast, -1, 1, d->contrast);
  if(module->dev && dt_image_is_rawprepare_supported(&module->dev->image_storage))
    append_mobile_blend_number(description, "blend.mask.details", "RAW details threshold", "float",
      p->details, -1, 1, d->details);
}

/* Return 0 for unrelated fields; all validation is on a local copy so failed
 * setters cannot partly change threshold handles, registrations or history. */
static int update_mobile_parametric_parameter(dt_iop_module_t *module, const char *parameter, double value)
{
  const gboolean is_parametric = !strncmp(parameter, "blend.parametric.", 17);
  const gboolean is_refinement = !strncmp(parameter, "blend.mask.", 11);
  if(!is_parametric && !is_refinement) return 0;
  if(!isfinite(value) || !mobile_blending_available(module)) return -1;
  dt_develop_blend_params_t p = *module->blend_params;
  const dt_develop_blend_params_t *d = module->default_blendop_params;
  if(is_refinement)
  {
    if(!(p.mask_mode & (DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL | DEVELOP_MASK_RASTER))) return -1;
    const char *name = parameter + 11;
    if(!strcmp(name, "blur_radius"))
    {
      if(value < 0 || value > 100) return -1;
      p.blur_radius = (float)value;
      p.feather_version = 1;
    }
    else
    {
      if(mobile_blending_colorspace(module) == DEVELOP_BLEND_CS_RAW) return -1;
      if(!strcmp(name, "feather_radius"))
      {
        if(value < 0 || value > 250) return -1;
        p.feathering_radius = (float)value;
        p.feather_version = 1;
      }
      else if(!strcmp(name, "feather_guide"))
      {
        if(value != DEVELOP_MASK_GUIDE_IN_BEFORE_BLUR && value != DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR
           && value != DEVELOP_MASK_GUIDE_IN_AFTER_BLUR && value != DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR) return -1;
        p.feathering_guide = (uint32_t)value;
      }
      else if(!strcmp(name, "brightness"))
      {
        if(value < -1 || value > 1) return -1;
        p.brightness = (float)value;
      }
      else if(!strcmp(name, "contrast"))
      {
        if(value < -1 || value > 1) return -1;
        p.contrast = (float)value;
      }
      else if(!strcmp(name, "details"))
      {
        if(!module->dev || !dt_image_is_rawprepare_supported(&module->dev->image_storage)
           || value < -1 || value > 1) return -1;
        p.details = (float)value;
      }
      else return -1;
    }
    p.mask_mode |= DEVELOP_MASK_ENABLED;
  }
  else
  {
    if(!mobile_parametric_available(module)) return -1;
    const char *name = parameter + 17;
    if(!strcmp(name, "enabled"))
    {
      if(value != 0 && value != 1) return -1;
      if(value) mobile_parametric_activate(&p);
      else p.mask_mode &= ~DEVELOP_MASK_CONDITIONAL;
    }
    else
    {
      if(!(p.mask_mode & DEVELOP_MASK_CONDITIONAL)) return -1;
      if(!strcmp(name, "reset"))
      {
        if(value != 1) return -1;
        p.blendif = d->blendif;
        memcpy(p.blendif_parameters, d->blendif_parameters, sizeof(p.blendif_parameters));
        memcpy(p.blendif_boost_factors, d->blendif_boost_factors, sizeof(p.blendif_boost_factors));
        mobile_parametric_neutral_channels(module, &p);
      }
      else if(!strcmp(name, "combine"))
      {
        if(value < 0 || value > 3 || value != trunc(value)) return -1;
        p.mask_combine = (p.mask_combine & ~3u) | (uint32_t)value;
        mobile_parametric_neutral_channels(module, &p);
      }
      else
      {
        unsigned int channel = DEVELOP_BLENDIF_SIZE;
        const char *field = NULL;
        for(unsigned int k = 0; k <= DEVELOP_BLENDIF_MAX; k++)
        {
          char prefix[16];
          snprintf(prefix, sizeof(prefix), "%u.", k);
          if(!strncmp(name, prefix, strlen(prefix))) { channel = k; field = name + strlen(prefix); break; }
        }
        if(!field || !(mobile_parametric_channel_mask(module) & (1u << channel))) return -1;
        float *handles = p.blendif_parameters + 4 * channel;
        if(!strcmp(field, "reset"))
        {
          if(value != 1 || !mobile_parametric_thresholds_valid(d->blendif_parameters + 4 * channel)) return -1;
          memcpy(handles, d->blendif_parameters + 4 * channel, 4 * sizeof(float));
          p.blendif_boost_factors[channel] = d->blendif_boost_factors[channel];
          p.blendif = (p.blendif & ~((1u << channel) | (1u << (channel + 16))))
            | (d->blendif & ((1u << channel) | (1u << (channel + 16))));
          if(!(p.blendif & (1u << channel)) && (p.mask_combine & DEVELOP_COMBINE_INCL))
            p.blendif |= 1u << (channel + 16);
        }
        else if(!strcmp(field, "enabled"))
        {
          if(value != 0 && value != 1) return -1;
          if(value)
          {
            if(!mobile_parametric_thresholds_valid(handles)) return -1;
            p.blendif |= 1u << channel;
          }
          else
          {
            p.blendif &= ~(1u << channel);
            p.blendif &= ~(1u << (channel + 16));
            if(p.mask_combine & DEVELOP_COMBINE_INCL) p.blendif |= 1u << (channel + 16);
          }
        }
        else if(!strcmp(field, "inverted"))
        {
          if(value != 0 && value != 1) return -1;
          if(value) p.blendif |= 1u << (channel + 16);
          else p.blendif &= ~(1u << (channel + 16));
        }
        else if(field[0] == 'p' && field[1] >= '0' && field[1] <= '3' && field[2] == '\0')
        {
          if(value < 0 || value > 1) return -1;
          handles[field[1] - '0'] = (float)value;
          if(!mobile_parametric_thresholds_valid(handles)) return -1;
          mobile_parametric_update_active(&p, channel);
        }
        else if(!strcmp(field, "boost_ev"))
        {
          if(!mobile_parametric_boost_available(module, channel) || value < 0 || value > 18
             || !mobile_parametric_thresholds_valid(handles)) return -1;
          const float offset = mobile_parametric_boost_offset(module, channel);
          const float previous = p.blendif_boost_factors[channel];
          const float next = (float)value + offset;
          if(!isfinite(previous) || previous < offset || previous > 18 + offset) return -1;
          const float factor = exp2f(previous - next);
          const unsigned int input_channel = channel & ~4u;
          const float center = mobile_blending_colorspace(module) == DEVELOP_BLEND_CS_LAB
            && (input_channel == DEVELOP_BLENDIF_A_in || input_channel == DEVELOP_BLENDIF_B_in) ? 0.5f : 0.0f;
          for(size_t handle = 0; handle < 4; handle++)
            if((handle < 2 && handles[handle] > 0) || (handle >= 2 && handles[handle] < 1))
              handles[handle] = CLAMP((handles[handle] - center) * factor + center, 0.0f, 1.0f);
          p.blendif_boost_factors[channel] = next;
          if(handles[1] == 0 && handles[2] == 1) p.blendif &= ~(1u << channel);
        }
        else return -1;
      }
      mobile_parametric_activate(&p);
    }
  }
  if((p.mask_mode & DEVELOP_MASK_CONDITIONAL) && (p.mask_mode & DEVELOP_MASK_ENABLED)
     && !mobile_parametric_active_valid(module, &p)) return -1;
  dt_iop_commit_blend_params(module, &p, NULL);
  return 1;
}

#endif
