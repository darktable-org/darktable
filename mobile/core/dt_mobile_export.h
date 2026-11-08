/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_EXPORT_INTERNAL_H
#define DT_MOBILE_EXPORT_INTERNAL_H
#include "dt_mobile.h"
#include "../include/dt_mobile_export.h"
#include "imageio/imageio_common.h"
#include <glib.h>
#include <stdatomic.h>

/* All functions require an initialized engine and its global work lock. */
dt_mobile_status dt_mobile_export_validate(const char *format,
                                           const dt_mobile_export_settings *settings,
                                           char *error, size_t error_size);
dt_mobile_status dt_mobile_export_write(dt_imgid_t image_id, const char *path,
                                        const char *format,
                                        const dt_mobile_export_settings *settings,
                                        dt_imageio_preview_cancel_t *cancel,
                                        const atomic_bool *cancelled,
                                        char *error, size_t error_size);
/* Append one JSON object, without a leading comma or property name. */
void dt_mobile_export_append_options(GString *json, const char *format);
#endif
