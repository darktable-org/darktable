/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_H
#define DT_MOBILE_H
#include <stddef.h>
#include <stdint.h>
#include "dt_mobile_export.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct dt_mobile_session dt_mobile_session;
typedef enum dt_mobile_status {
  DT_MOBILE_OK = 0,
  DT_MOBILE_ERROR_INVALID_ARGUMENT = 1,
  DT_MOBILE_ERROR_IO = 2,
  DT_MOBILE_ERROR_UNSUPPORTED = 3,
  DT_MOBILE_ERROR_CANCELLED = 4,
  DT_MOBILE_ERROR_OUT_OF_MEMORY = 5,
  DT_MOBILE_ERROR_ENGINE = 6,
  DT_MOBILE_ERROR_DECODE = 7,
  DT_MOBILE_ERROR_PROCESSING = 8
} dt_mobile_status;
/* Capabilities describe the engine linked into this build, not file suffixes. */
enum { DT_MOBILE_CAP_ENGINE = 1u, DT_MOBILE_CAP_GPR = 2u };
uint32_t dt_mobile_capabilities(void);
/* Configure absolute runtime directories before opening an image. Paths are
 * copied and remain configured across the last session's cleanup/reopen.
 * Configuration fails while initialization, cleanup or any session is active.
 * data_dir contains the installed desktop data tree; module_dir contains the
 * installed native modules; temp_root is a writable application-private folder.
 * Host CLI callers may omit configuration and use DT_MOBILE_DATADIR/MODULEDIR
 * with the platform temporary directory instead. No process environment changes
 * are made by this API. Failures are described by dt_mobile_runtime_error(). */
dt_mobile_status dt_mobile_configure_runtime(const char *data_dir, const char *module_dir,
                                            const char *temp_root);
const char *dt_mobile_runtime_error(void);
dt_mobile_status dt_mobile_open(const char *path, dt_mobile_session **out_session);
const char *dt_mobile_open_error(void);
/* JSON strings and preview buffers belong to the caller; free with dt_mobile_free_buffer.
 * Session operations must be serialized by the caller. Only cancel may run concurrently.
 * Close must wait until the active operation and any cancellation calls finish. */
dt_mobile_status dt_mobile_modules(dt_mobile_session *session, char **json);
/* Session edit steps, excluding imported/automatic startup history. The JSON
 * contains canUndo, canRedo, position, length and limit. At most 64 successful
 * Apply steps are retained; editing after undo discards the redo branch. */
dt_mobile_status dt_mobile_history(dt_mobile_session *session, char **json);
dt_mobile_status dt_mobile_undo(dt_mobile_session *session);
dt_mobile_status dt_mobile_redo(dt_mobile_session *session);
/* Create an enabled instance with the original module's image-specific default
 * parameters and default blending. Only modules advertising canCreateInstance
 * support this operation. This creates one undo step; undo removes the instance
 * from the active pipeline, and redo restores its stable identifier/settings.
 * Identifiers are not reused during a session, including discarded redo branches.
 * On success new_identifier is caller-owned (dt_mobile_free_buffer); all failures
 * set it to NULL and leave the active history and instances unchanged. */
dt_mobile_status dt_mobile_new_instance(dt_mobile_session *session, const char *module,
                                        char **new_identifier);
dt_mobile_status dt_mobile_set_parameter(dt_mobile_session *session, const char *module,
                                          const char *parameter, double value);
/* Read the selected Tone equalizer instance's actual filtered input mask.
 * x/y are normalized final-preview image coordinates in [0,1), excluding any
 * UI letterboxing. A successful preview and an enabled instance are required;
 * sampling uses the most recent successful preview's resolution and does not
 * modify module settings or history. Returns log2 mask luminance in EV. */
dt_mobile_status dt_mobile_tone_equalizer_sample(dt_mobile_session *session,
                                                 const char *module, double x, double y,
                                                 double *exposure);
/* Original desktop on-image smooth adjustment, applied to all nine tone gains
 * as one atomic undo step. exposure must be a finite sample from this instance
 * and unchanged input/mask/geometry state; offset is finite in [-2,2] EV.
 * The instance must already be enabled. Other settings remain unchanged. */
dt_mobile_status dt_mobile_tone_equalizer_adjust(dt_mobile_session *session,
                                                 const char *module, double exposure,
                                                 double offset);
/* Read-only selected output: original mask input EV plus the actual current
 * quantized processing-curve correction. The mask input is preserved for
 * subsequent edits; requested offsets are never substituted for this value. */
dt_mobile_status dt_mobile_tone_equalizer_output(dt_mobile_session *session,
                                                 const char *module, double exposure,
                                                 double *output_exposure);
/* Set a selected output target in [input EV-2,input EV+2] using the original
 * smooth solver, atomically. Returns the achieved output, including at limits.
 * Positive gesture IDs group consecutive updates of the same instance into
 * one undo step. Another edit or Undo/Redo ends that grouping. */
dt_mobile_status dt_mobile_tone_equalizer_set_target(dt_mobile_session *session,
                                                     const char *module, double exposure,
                                                     double target, uint64_t gesture,
                                                     double *output_exposure);
/* UTF-8 for advertised text/file controls. maxBytes excludes the terminating
 * NUL. file.import selects the original coupled filename/image fields in one
 * history step. Overlong/malformed values are rejected, never truncated. */
dt_mobile_status dt_mobile_set_text_parameter(dt_mobile_session *session, const char *module,
                                               const char *parameter, const char *value);
typedef struct dt_mobile_curve_point { float x, y; } dt_mobile_curve_point;
/* Atomic node/interpolation update for the advertised base/RGB/Lab/color-zone
 * curves. Points use normalized coordinates, with 2..maxNodes finite nodes
 * (at most 20, according to the current metadata) and at least 1e-6
 * horizontal separation after conversion to float. Interpolation is 0 (cubic
 * spline), 1 (Catmull-Rom) or 2 (monotone Hermite). Only channels advertised in
 * module JSON are editable; unrelated module parameters remain unchanged. */
dt_mobile_status dt_mobile_set_curve(dt_mobile_session *session, const char *module,
                                     int channel, int interpolation,
                                     const dt_mobile_curve_point *points, size_t count);
/* Atomic RGB curve mode/color-preservation change. mode is 0 (linked RGB) or
 * 1 (independent RGB); preserve_colors must be an advertised choice (currently
 * 0 none, 1 luminance, 2 max RGB, 3 average RGB, 4 sum RGB, 5 norm RGB, 6 power).
 * Switching to independent mode copies R to G/B only when both G/B curves are
 * identity, matching the desktop widget. Other channel edits remain intact.
 * Color preservation is stored in independent mode but only used when linked.
 * The complete change, including any channel copies, is one undo step. */
dt_mobile_status dt_mobile_set_rgb_curve_options(dt_mobile_session *session,
                                                 const char *module, int mode,
                                                 int preserve_colors);
/* Atomic manual white balance in native channel-index order. The count must
 * match the advertised three or four active channels. Each gain is finite and
 * within the reviewed manual range [0.001f,8.0f]; inactive channel bytes are
 * preserved. This selects user-defined gains and clears late adaptation.
 * This is deliberately narrower than the desktop's zero-inclusive sliders. */
dt_mobile_status dt_mobile_set_whitebalance_gains(dt_mobile_session *session,
                                                 const char *module,
                                                 const float *gains, size_t count);
/* Atomic Kelvin/tint edit using the desktop temperature module's original
 * spectral integration, camera matrices and tint convention. Finite Kelvin
 * values are in [1901,25000] and tint in [0.135,2.326]. The native metadata
 * advertises this editor only when the current image has a usable calibration.
 * Generated active gains must be positive, finite floats; they retain desktop
 * values above the manual-gain maximum. Native channel 1 (green for RGB)
 * is normalized to 1; inactive
 * channel bytes are preserved. This selects user-defined white balance and
 * records one undo step. */
dt_mobile_status dt_mobile_set_whitebalance_temperature(dt_mobile_session *session,
                                                       const char *module,
                                                       double kelvin, double tint);
/* Original built-in settings: 0 as-shot, 3 camera reference, 4 as-shot to
 * reference. Gains come from the current image's original native basis.
 * Named settings retain valid original coefficients above the manual maximum;
 * only settings advertised for the current image can be selected. */
dt_mobile_status dt_mobile_set_whitebalance_setting(dt_mobile_session *session,
                                                   const char *module, int setting);
/* Atomic freehand crop in the upstream crop module's normalized coordinate
 * space. Coordinates are finite and within [0,1]; right >= left+0.01f and
 * bottom >= top+0.01f use float addition, matching the desktop minimum edges.
 * This selects the desktop freehand ratio (0/0); it does not retain
 * an earlier fixed aspect ratio. It does not interpret an already cropped
 * preview as the original image. A full rectangle restores the uncropped area. */
dt_mobile_status dt_mobile_set_crop(dt_mobile_session *session, const char *module,
                                    float left, float top, float right, float bottom);
dt_mobile_status dt_mobile_set_enabled(dt_mobile_session *session, const char *module, int enabled);
dt_mobile_status dt_mobile_export_formats(dt_mobile_session *session, char **json);
/* Trusted native integration only: params must be a complete, validated upstream
 * parameter struct for this exact module/engine version. This low-level API checks
 * its byte size, not cross-field invariants. Do not pass untrusted/UI-supplied blobs;
 * use the reviewed numeric setter or a purpose-built structured editor instead. */
dt_mobile_status dt_mobile_set_module_params(dt_mobile_session *session, const char *module,
                                               const void *params, size_t params_size);
/* Preview pixels are byte-ordered B, G, R, A (opaque alpha). Convert these
 * explicitly when the platform bitmap's byte layout differs. */
dt_mobile_status dt_mobile_render_preview(dt_mobile_session *session, int max_width, int max_height,
                                            uint8_t **rgba, int *width, int *height);
dt_mobile_status dt_mobile_export(dt_mobile_session *session, const char *output_path,
                                    const char *format, int quality);
/* Format-specific settings are advertised by export_formats JSON. This keeps
 * the source untouched and commits an output only after successful encoding.
 * Use DT_MOBILE_EXPORT_SETTINGS_INIT before overriding advertised settings. */
dt_mobile_status dt_mobile_export_with_settings(dt_mobile_session *session,
                                                const char *output_path,
                                                const char *format,
                                                const dt_mobile_export_settings *settings);
void dt_mobile_free_buffer(void *buffer);
void dt_mobile_cancel(dt_mobile_session *session);
const char *dt_mobile_last_error(const dt_mobile_session *session);
void dt_mobile_close(dt_mobile_session *session);
#ifdef __cplusplus
}
#endif
#endif
