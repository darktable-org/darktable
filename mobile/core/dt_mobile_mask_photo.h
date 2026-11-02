/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_MASK_PHOTO_H
#define DT_MOBILE_MASK_PHOTO_H

/* The adapter's develop deliberately owns no pixelpipe. Build original pieces
 * from the persisted private image history, just as preview/export do, so mask
 * coordinates remain in original image space even when the consumer is before
 * crop, flip, lens correction, perspective, pixel aspect or liquify. The caller
 * holds the engine operation and global work locks. There is intentionally no
 * geometry cache: distort-only modules also change this mapping. */
typedef struct mobile_mask_photo_geometry
{
  dt_develop_t develop;
  dt_dev_pixelpipe_t pipe;
  gboolean develop_ready, pipe_ready;
  double scale;
  int width, height;
} mobile_mask_photo_geometry;

static void mobile_mask_photo_geometry_cleanup(mobile_mask_photo_geometry *geometry)
{
  if(geometry->pipe_ready) dt_dev_pixelpipe_cleanup(&geometry->pipe);
  if(geometry->develop_ready) dt_dev_cleanup(&geometry->develop);
}

static dt_mobile_status mobile_mask_photo_geometry_init(dt_mobile_engine *engine,
    mobile_mask_photo_geometry *geometry, char *error, size_t error_size)
{
  memset(geometry, 0, sizeof(*geometry));
  if(engine->preview_max_width < 1 || engine->preview_max_height < 1)
    return engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                       "render a preview before placing a mask");
  dt_dev_init(&geometry->develop, FALSE);
  geometry->develop_ready = TRUE;
  dt_dev_load_image(&geometry->develop, engine->image_id);
  const int width = geometry->develop.image_storage.width;
  const int height = geometry->develop.image_storage.height;
  if(width <= 0 || height <= 0
     || !dt_dev_pixelpipe_init_dummy(&geometry->pipe, width, height))
    return engine_fail(error, error_size, DT_MOBILE_ERROR_PROCESSING,
                       "could not initialize mask photo geometry");
  geometry->pipe_ready = TRUE;
  dt_dev_pixelpipe_set_input(&geometry->pipe, &geometry->develop, NULL, width, height, 1.0f);
  dt_dev_pixelpipe_create_nodes(&geometry->pipe, &geometry->develop);
  dt_dev_pixelpipe_synch_all(&geometry->pipe, &geometry->develop);
  dt_dev_pixelpipe_get_dimensions(&geometry->pipe, &geometry->develop,
      geometry->pipe.iwidth, geometry->pipe.iheight,
      &geometry->pipe.processed_width, &geometry->pipe.processed_height);
  if(geometry->pipe.processed_width <= 0 || geometry->pipe.processed_height <= 0)
    return engine_fail(error, error_size, DT_MOBILE_ERROR_PROCESSING,
                       "mask photo geometry has no usable image dimensions");
  geometry->scale = fmin(1.0, fmin((double)engine->preview_max_width / geometry->pipe.processed_width,
                                 (double)engine->preview_max_height / geometry->pipe.processed_height));
  /* Preview output is floored independently on each axis. Multiplying a tap
   * by processed_width alone introduces a systematic error on the other axis. */
  geometry->width = (int)floor(geometry->scale * geometry->pipe.processed_width);
  geometry->height = (int)floor(geometry->scale * geometry->pipe.processed_height);
  if(!isfinite(geometry->scale) || geometry->scale <= 0 || geometry->width < 1 || geometry->height < 1)
    return engine_fail(error, error_size, DT_MOBILE_ERROR_PROCESSING,
                       "mask photo geometry has no usable preview dimensions");
  return DT_MOBILE_OK;
}

/* Use the original ALL traversal and committed pieces. Individual callback
 * return values are not errors in that API: for example, lens returns FALSE
 * for its identity vignette-only mode. Validate the resulting coordinates and
 * round-trip separately, using the same contract as native mask overlays. */
static gboolean mobile_mask_photo_transform(mobile_mask_photo_geometry *geometry,
    gboolean back, float point[2])
{
  const gboolean success = back
    ? dt_dev_distort_backtransform_plus(&geometry->develop, &geometry->pipe, 0,
                                       DT_DEV_TRANSFORM_DIR_ALL, point, 1)
    : dt_dev_distort_transform_plus(&geometry->develop, &geometry->pipe, 0,
                                   DT_DEV_TRANSFORM_DIR_ALL, point, 1);
  return success && isfinite(point[0]) && isfinite(point[1]);
}

static const char *mobile_mask_photo_type(const dt_masks_form_t *form)
{
  return form->type & DT_MASKS_CIRCLE ? "circle" : form->type & DT_MASKS_ELLIPSE ? "ellipse"
    : form->type & DT_MASKS_GRADIENT ? "gradient" : form->type & DT_MASKS_PATH ? "path" : "brush";
}

static float *mobile_mask_photo_center(dt_masks_form_t *form)
{
  if(form->type & DT_MASKS_CIRCLE) return ((dt_masks_point_circle_t *)form->points->data)->center;
  if(form->type & DT_MASKS_ELLIPSE) return ((dt_masks_point_ellipse_t *)form->points->data)->center;
  if(form->type & DT_MASKS_GRADIENT) return ((dt_masks_point_gradient_t *)form->points->data)->anchor;
  return NULL;
}

static gboolean mobile_mask_photo_source_supported(const dt_masks_form_t *form)
{
  /* Path/brush source fields anchor the first node before the native source
   * outline rotates around its centroid. That rotated sampled position cannot
   * be represented by forwarding this anchor alone. Keep the numeric controls,
   * but omit the photo handle until its exact geometry is supported. */
  return (form->type & DT_MASKS_CLONE)
    && (!(form->type & (DT_MASKS_PATH | DT_MASKS_BRUSH)) || form->source[2] == 0.f);
}

static void mobile_mask_photo_append_point(mobile_mask_photo_geometry *geometry,
    GString *json, const float normalized[2])
{
  float DT_ALIGNED_ARRAY point[2] = { normalized[0] * geometry->pipe.iwidth,
                                    normalized[1] * geometry->pipe.iheight };
  gboolean mapped = isfinite(normalized[0]) && isfinite(normalized[1])
    && normalized[0] >= 0 && normalized[0] <= 1 && normalized[1] >= 0 && normalized[1] <= 1
    && mobile_mask_photo_transform(geometry, FALSE, point);
  const double x = point[0] * geometry->scale / geometry->width;
  const double y = point[1] * geometry->scale / geometry->height;
  gboolean visible = mapped && normalized[0] >= 0 && normalized[0] <= 1
    && normalized[1] >= 0 && normalized[1] <= 1 && x >= 0 && x < 1 && y >= 0 && y < 1;
  if(visible)
  {
    float DT_ALIGNED_ARRAY original[2] = { point[0], point[1] };
    visible = mapped = mobile_mask_photo_transform(geometry, TRUE, original)
      && hypot(original[0] - normalized[0] * geometry->pipe.iwidth,
               original[1] - normalized[1] * geometry->pipe.iheight) <= .75 / geometry->scale;
  }
  g_string_append(json, "{\"x\":"); json_number(json, mapped ? x : NAN);
  g_string_append(json, ",\"y\":"); json_number(json, mapped ? y : NAN);
  g_string_append_printf(json, ",\"visible\":%s}", visible ? "true" : "false");
}

static dt_mobile_status mobile_mask_photo_points(dt_mobile_engine *engine, dt_iop_module_t *module,
    char **json, char *error, size_t error_size)
{
  *json = NULL;
  if(!mobile_masks_available(module))
    return engine_fail(error, error_size, DT_MOBILE_ERROR_UNSUPPORTED,
                       "this module does not support drawn masks");
  dt_masks_form_t *group = mobile_mask_group(module);
  if(!group || !group->points)
  {
    *json = g_strdup("{\"forms\":[]}");
    return DT_MOBILE_OK;
  }
  mobile_mask_photo_geometry geometry;
  const dt_mobile_status status = mobile_mask_photo_geometry_init(engine, &geometry, error, error_size);
  if(status != DT_MOBILE_OK) { mobile_mask_photo_geometry_cleanup(&geometry); return status; }
  GString *description = g_string_new("{\"forms\":[");
  gboolean first = TRUE;
  for(const GList *item = group->points; item; item = item->next)
  {
    const dt_masks_point_group_t *member = item->data;
    dt_masks_form_t *form = member ? dt_masks_get_from_id(module->dev, member->formid) : NULL;
    if(!mobile_mask_shape_supported(form)) continue;
    const float *center = mobile_mask_photo_center(form);
    const gboolean source = mobile_mask_photo_source_supported(form);
    if(!center && !source) continue;
    if(!first) g_string_append_c(description, ',');
    first = FALSE;
    const char *type = mobile_mask_photo_type(form);
    char label[96]; snprintf(label, sizeof(label), "%s %d", type, form->formid);
    g_string_append_printf(description, "{\"id\":%d,\"label\":", form->formid);
    json_string(description, label);
    g_string_append(description, ",\"type\":"); json_string(description, type);
    if(center)
    {
      g_string_append(description, ",\"center\":");
      mobile_mask_photo_append_point(&geometry, description, center);
    }
    if(source)
    {
      g_string_append(description, ",\"source\":");
      mobile_mask_photo_append_point(&geometry, description, form->source);
    }
    g_string_append_c(description, '}');
  }
  g_string_append(description, "]}");
  *json = g_string_free(description, FALSE);
  mobile_mask_photo_geometry_cleanup(&geometry);
  return DT_MOBILE_OK;
}

/* Both axes are validated and transformed before either original field changes.
 * The caller appends exactly one native mask history item after success. */
static dt_mobile_status mobile_mask_place_photo(dt_mobile_engine *engine, dt_iop_module_t *module,
    int form_id, int source, double x, double y, char *error, size_t error_size)
{
  if(!module || !module->enabled || !mobile_masks_available(module)
     || (source != 0 && source != 1) || !isfinite(x) || !isfinite(y) || x < 0 || x >= 1 || y < 0 || y >= 1)
    return engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                       "select an enabled drawn mask and a point inside the preview");
  dt_masks_form_t *group = mobile_mask_group(module);
  gboolean belongs = FALSE;
  for(const GList *item = group ? group->points : NULL; item; item = item->next)
    if(item->data && ((const dt_masks_point_group_t *)item->data)->formid == form_id)
    { belongs = TRUE; break; }
  dt_masks_form_t *form = belongs ? dt_masks_get_from_id(module->dev, form_id) : NULL;
  if(!mobile_mask_shape_supported(form) || (source && !(form->type & DT_MASKS_CLONE)))
    return engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                       "the requested mask handle does not belong to this module");
  if(source && !mobile_mask_photo_source_supported(form))
    return engine_fail(error, error_size, DT_MOBILE_ERROR_UNSUPPORTED,
                       "a rotated path or brush source has no supported photo handle");
  float *destination = source ? form->source : mobile_mask_photo_center(form);
  if(!destination)
    return engine_fail(error, error_size, DT_MOBILE_ERROR_UNSUPPORTED,
                       "this mask shape has no supported center handle");
  mobile_mask_photo_geometry geometry;
  dt_mobile_status status = mobile_mask_photo_geometry_init(engine, &geometry, error, error_size);
  if(status != DT_MOBILE_OK) { mobile_mask_photo_geometry_cleanup(&geometry); return status; }
  const double output_x = x * geometry.width / geometry.scale;
  const double output_y = y * geometry.height / geometry.scale;
  float DT_ALIGNED_ARRAY point[2] = { (float)output_x, (float)output_y };
  gboolean valid = mobile_mask_photo_transform(&geometry, TRUE, point)
    && point[0] >= 0 && point[0] < geometry.pipe.iwidth
    && point[1] >= 0 && point[1] < geometry.pipe.iheight;
  const float normalized[2] = { point[0] / geometry.pipe.iwidth, point[1] / geometry.pipe.iheight };
  if(valid)
  {
    float DT_ALIGNED_ARRAY projected[2] = { normalized[0] * geometry.pipe.iwidth,
                                          normalized[1] * geometry.pipe.iheight };
    valid = mobile_mask_photo_transform(&geometry, FALSE, projected)
      && hypot(projected[0] - output_x, projected[1] - output_y) <= .75 / geometry.scale;
  }
  mobile_mask_photo_geometry_cleanup(&geometry);
  if(!valid)
    return engine_fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT,
                       "the selected point is outside the image or cannot be mapped to a mask");
  if(atomic_load(&engine->cancelled))
    return engine_fail(error, error_size, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
  destination[0] = normalized[0]; destination[1] = normalized[1];
  if(mobile_mask_special(module)) mobile_mask_sync(module)(module);
  return DT_MOBILE_OK;
}

#endif
