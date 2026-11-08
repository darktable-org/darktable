/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <jni.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "dt_mobile.h"
static void throw_io(JNIEnv *env, const char *message)
{
  jclass type = (*env)->FindClass(env, "java/io/IOException");
  if(type) (*env)->ThrowNew(env, type, message);
}
static void throw_status(JNIEnv *env, dt_mobile_session *session, dt_mobile_status status)
{
  if(status == DT_MOBILE_OK) return;
  const char *error = dt_mobile_last_error(session);
  throw_io(env, status == DT_MOBILE_ERROR_INVALID_ARGUMENT ? "invalid native editor arguments" :
                (error && error[0] ? error : "native editor operation failed"));
}
/* Metadata and editable text use standard UTF-8. JNI's NewStringUTF and
 * GetStringUTFChars use modified UTF-8, which corrupts supplementary characters
 * such as emoji in watermark text. Decode metadata with Java's UTF-8 codec. */
static jstring new_utf8_string(JNIEnv *env, const char *value)
{
  const size_t length = strlen(value);
  if(length > INT_MAX) { throw_io(env, "native editor text is too large"); return NULL; }
  jclass type = (*env)->FindClass(env, "java/lang/String");
  if(!type) return NULL;
  jmethodID constructor = (*env)->GetMethodID(env, type, "<init>", "([BLjava/lang/String;)V");
  if(!constructor) { (*env)->DeleteLocalRef(env, type); return NULL; }
  jbyteArray bytes = (*env)->NewByteArray(env, (jsize)length);
  if(!bytes) { (*env)->DeleteLocalRef(env, type); return NULL; }
  (*env)->SetByteArrayRegion(env, bytes, 0, (jsize)length, (const jbyte *)value);
  jstring charset = NULL, result = NULL;
  if(!(*env)->ExceptionCheck(env)) charset = (*env)->NewStringUTF(env, "UTF-8");
  if(charset) result = (*env)->NewObject(env, type, constructor, bytes, charset);
  if(charset) (*env)->DeleteLocalRef(env, charset);
  (*env)->DeleteLocalRef(env, bytes);
  (*env)->DeleteLocalRef(env, type);
  return result;
}

static char *editor_text_utf8(JNIEnv *env, jstring text)
{
  const jsize count = (*env)->GetStringLength(env, text);
  if(count > 4096) { throw_io(env, "editor text exceeds the native buffer"); return NULL; }
  const jchar *characters = (*env)->GetStringChars(env, text, NULL);
  if(!characters) return NULL;
  char *result = malloc((size_t)count * 3 + 1);
  if(!result)
  {
    (*env)->ReleaseStringChars(env, text, characters);
    throw_io(env, "unable to allocate editor text"); return NULL;
  }
  size_t used = 0;
  for(jsize k = 0; k < count; k++)
  {
    uint32_t code = characters[k];
    if(!code || (code >= 0xDC00 && code <= 0xDFFF)) goto invalid;
    if(code >= 0xD800 && code <= 0xDBFF)
    {
      if(k + 1 >= count || characters[k + 1] < 0xDC00 || characters[k + 1] > 0xDFFF) goto invalid;
      code = 0x10000 + ((code - 0xD800) << 10) + (characters[++k] - 0xDC00);
    }
    if(code < 0x80) result[used++] = (char)code;
    else if(code < 0x800)
    {
      result[used++] = (char)(0xC0 | (code >> 6));
      result[used++] = (char)(0x80 | (code & 0x3F));
    }
    else if(code < 0x10000)
    {
      result[used++] = (char)(0xE0 | (code >> 12));
      result[used++] = (char)(0x80 | ((code >> 6) & 0x3F));
      result[used++] = (char)(0x80 | (code & 0x3F));
    }
    else
    {
      result[used++] = (char)(0xF0 | (code >> 18));
      result[used++] = (char)(0x80 | ((code >> 12) & 0x3F));
      result[used++] = (char)(0x80 | ((code >> 6) & 0x3F));
      result[used++] = (char)(0x80 | (code & 0x3F));
    }
  }
  result[used] = 0;
  (*env)->ReleaseStringChars(env, text, characters);
  return result;
invalid:
  free(result);
  (*env)->ReleaseStringChars(env, text, characters);
  throw_io(env, "editor text must be valid Unicode without NUL characters");
  return NULL;
}
JNIEXPORT jint JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_capabilities(JNIEnv *env, jobject self)
{ (void)env; (void)self; return (jint)dt_mobile_capabilities(); }
JNIEXPORT void JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_configureRuntime(JNIEnv *env, jobject self,
  jstring data_dir, jstring module_dir, jstring temp_root)
{
  (void)self;
  if(!data_dir || !module_dir || !temp_root) { throw_io(env, "runtime directories are required"); return; }
  const char *data = (*env)->GetStringUTFChars(env, data_dir, NULL);
  if(!data) return;
  const char *modules = (*env)->GetStringUTFChars(env, module_dir, NULL);
  if(!modules) { (*env)->ReleaseStringUTFChars(env, data_dir, data); return; }
  const char *temporary = (*env)->GetStringUTFChars(env, temp_root, NULL);
  if(!temporary) {
    (*env)->ReleaseStringUTFChars(env, module_dir, modules);
    (*env)->ReleaseStringUTFChars(env, data_dir, data); return;
  }
  const dt_mobile_status status = dt_mobile_configure_runtime(data, modules, temporary);
  (*env)->ReleaseStringUTFChars(env, temp_root, temporary);
  (*env)->ReleaseStringUTFChars(env, module_dir, modules);
  (*env)->ReleaseStringUTFChars(env, data_dir, data);
  if(status != DT_MOBILE_OK) throw_io(env, dt_mobile_runtime_error());
}
JNIEXPORT jlong JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_open(JNIEnv *env, jobject self, jstring path)
{
  (void)self; if(!path) { throw_io(env, "a source path is required"); return 0; }
  const char *value = (*env)->GetStringUTFChars(env, path, NULL); if(!value) return 0;
  dt_mobile_session *session = NULL; dt_mobile_status status = dt_mobile_open(value, &session);
  (*env)->ReleaseStringUTFChars(env, path, value);
  if(status != DT_MOBILE_OK) { throw_io(env, dt_mobile_open_error()); return 0; }
  return (jlong)(intptr_t)session;
}
JNIEXPORT jstring JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_lastError(JNIEnv *env, jobject self, jlong handle)
{ (void)self; return (*env)->NewStringUTF(env, dt_mobile_last_error((dt_mobile_session *)(intptr_t)handle)); }
JNIEXPORT void JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_cancel(JNIEnv *env, jobject self, jlong handle)
{ (void)env; (void)self; dt_mobile_cancel((dt_mobile_session *)(intptr_t)handle); }
JNIEXPORT void JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_close(JNIEnv *env, jobject self, jlong handle)
{ (void)env; (void)self; dt_mobile_close((dt_mobile_session *)(intptr_t)handle); }
JNIEXPORT jbyteArray JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_renderPreview(JNIEnv *env, jobject self, jlong handle, jint max_width, jint max_height, jintArray dimensions)
{
  (void)self;
  if(!handle || !dimensions || (*env)->GetArrayLength(env, dimensions) < 2) { throw_io(env, "invalid preview arguments"); return NULL; }
  uint8_t *rgba = NULL; int width = 0, height = 0;
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  dt_mobile_status status = dt_mobile_render_preview(session, max_width, max_height, &rgba, &width, &height);
  if(status != DT_MOBILE_OK) { throw_io(env, dt_mobile_last_error(session)); return NULL; }
  if(!rgba || width <= 0 || height <= 0 || width > max_width || height > max_height)
  { dt_mobile_free_buffer(rgba); throw_io(env, "native preview dimensions are invalid"); return NULL; }
  size_t size = (size_t)width * (size_t)height * 4u;
  if(size > 2147483647u) { dt_mobile_free_buffer(rgba); throw_io(env, "preview buffer is too large"); return NULL; }
  jbyteArray result = (*env)->NewByteArray(env, (jsize)size);
  if(result) (*env)->SetByteArrayRegion(env, result, 0, (jsize)size, (const jbyte *)rgba);
  dt_mobile_free_buffer(rgba);
  if(!result || (*env)->ExceptionCheck(env)) return NULL;
  jint values[2] = { width, height }; (*env)->SetIntArrayRegion(env, dimensions, 0, 2, values);
  return result;
}
static jstring metadata(JNIEnv *env, jlong handle, int formats)
{
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  char *json = NULL;
  dt_mobile_status status = formats ? dt_mobile_export_formats(session, &json) : dt_mobile_modules(session, &json);
  if(status != DT_MOBILE_OK) { dt_mobile_free_buffer(json); throw_status(env, session, status); return NULL; }
  if(!json) { throw_io(env, "native editor metadata is missing"); return NULL; }
  jstring result = new_utf8_string(env, json);
  dt_mobile_free_buffer(json);
  return result;
}
JNIEXPORT jstring JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_listModules(JNIEnv *env, jobject self, jlong handle)
{ (void)self; return metadata(env, handle, 0); }
JNIEXPORT jstring JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_listExportFormats(JNIEnv *env, jobject self, jlong handle)
{ (void)self; return metadata(env, handle, 1); }
JNIEXPORT jstring JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_history(JNIEnv *env, jobject self, jlong handle)
{
  (void)self;
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  char *json = NULL;
  const dt_mobile_status status = dt_mobile_history(session, &json);
  if(status != DT_MOBILE_OK) { dt_mobile_free_buffer(json); throw_status(env, session, status); return NULL; }
  if(!json) { throw_io(env, "native edit history is missing"); return NULL; }
  jstring result = new_utf8_string(env, json);
  dt_mobile_free_buffer(json);
  return result;
}
JNIEXPORT void JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_undo(JNIEnv *env, jobject self, jlong handle)
{
  (void)self;
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  throw_status(env, session, dt_mobile_undo(session));
}
JNIEXPORT void JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_redo(JNIEnv *env, jobject self, jlong handle)
{
  (void)self;
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  throw_status(env, session, dt_mobile_redo(session));
}
JNIEXPORT jstring JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_newInstance(JNIEnv *env, jobject self,
    jlong handle, jstring module)
{
  (void)self;
  if(!handle || !module) { throw_io(env, "an image and source module are required"); return NULL; }
  const char *operation = (*env)->GetStringUTFChars(env, module, NULL);
  if(!operation) return NULL;
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  char *identifier = NULL;
  const dt_mobile_status status = dt_mobile_new_instance(session, operation, &identifier);
  (*env)->ReleaseStringUTFChars(env, module, operation);
  if(status != DT_MOBILE_OK)
  { dt_mobile_free_buffer(identifier); throw_status(env, session, status); return NULL; }
  if(!identifier || !identifier[0])
  { dt_mobile_free_buffer(identifier); throw_io(env, "the new module identifier is missing"); return NULL; }
  jstring result = (*env)->NewStringUTF(env, identifier);
  dt_mobile_free_buffer(identifier);
  return result;
}
JNIEXPORT void JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_setParameter(JNIEnv *env, jobject self, jlong handle, jstring module, jstring parameter, jdouble value)
{
  (void)self;
  if(!module || !parameter) { throw_io(env, "module and parameter are required"); return; }
  const char *operation = (*env)->GetStringUTFChars(env, module, NULL);
  if(!operation) return;
  const char *name = (*env)->GetStringUTFChars(env, parameter, NULL);
  if(!name) { (*env)->ReleaseStringUTFChars(env, module, operation); return; }
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  dt_mobile_status status = dt_mobile_set_parameter(session, operation, name, value);
  (*env)->ReleaseStringUTFChars(env, parameter, name);
  (*env)->ReleaseStringUTFChars(env, module, operation);
  throw_status(env, session, status);
}
JNIEXPORT void JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_setTextParameter(JNIEnv *env, jobject self,
  jlong handle, jstring module, jstring parameter, jstring value)
{
  (void)self;
  if(!module || !parameter || !value) { throw_io(env, "module, parameter and text are required"); return; }
  const char *operation = (*env)->GetStringUTFChars(env, module, NULL);
  if(!operation) return;
  const char *name = (*env)->GetStringUTFChars(env, parameter, NULL);
  if(!name) { (*env)->ReleaseStringUTFChars(env, module, operation); return; }
  char *text = editor_text_utf8(env, value);
  if(!text)
  {
    (*env)->ReleaseStringUTFChars(env, parameter, name);
    (*env)->ReleaseStringUTFChars(env, module, operation); return;
  }
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  const dt_mobile_status status = dt_mobile_set_text_parameter(session, operation, name, text);
  free(text);
  (*env)->ReleaseStringUTFChars(env, parameter, name);
  (*env)->ReleaseStringUTFChars(env, module, operation);
  throw_status(env, session, status);
}
JNIEXPORT jdouble JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_sampleToneEqualizer(JNIEnv *env, jobject self,
    jlong handle, jstring module, jdouble x, jdouble y)
{
  (void)self;
  if(!handle || !module) { throw_io(env, "an image and tone equalizer module are required"); return 0; }
  const char *operation = (*env)->GetStringUTFChars(env, module, NULL);
  if(!operation) return 0;
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  double exposure = 0;
  const dt_mobile_status status = dt_mobile_tone_equalizer_sample(session, operation, x, y, &exposure);
  (*env)->ReleaseStringUTFChars(env, module, operation);
  throw_status(env, session, status);
  return status == DT_MOBILE_OK ? exposure : 0;
}
JNIEXPORT void JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_adjustToneEqualizer(JNIEnv *env, jobject self,
    jlong handle, jstring module, jdouble exposure, jdouble offset)
{
  (void)self;
  if(!handle || !module) { throw_io(env, "an image and tone equalizer module are required"); return; }
  const char *operation = (*env)->GetStringUTFChars(env, module, NULL);
  if(!operation) return;
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  const dt_mobile_status status = dt_mobile_tone_equalizer_adjust(session, operation, exposure, offset);
  (*env)->ReleaseStringUTFChars(env, module, operation);
  throw_status(env, session, status);
}
JNIEXPORT jdouble JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_toneEqualizerOutput(JNIEnv *env, jobject self,
    jlong handle, jstring module, jdouble exposure)
{
  (void)self;
  if(!handle || !module) { throw_io(env, "an image and tone equalizer module are required"); return 0; }
  const char *operation = (*env)->GetStringUTFChars(env, module, NULL);
  if(!operation) return 0;
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  double output = 0;
  const dt_mobile_status status = dt_mobile_tone_equalizer_output(session, operation, exposure, &output);
  (*env)->ReleaseStringUTFChars(env, module, operation);
  throw_status(env, session, status);
  return status == DT_MOBILE_OK ? output : 0;
}
JNIEXPORT jdouble JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_setToneEqualizerTarget(JNIEnv *env, jobject self,
    jlong handle, jstring module, jdouble exposure, jdouble target, jlong gesture)
{
  (void)self;
  if(!handle || !module || gesture < 0) { throw_io(env, "an image, tone equalizer module and valid gesture are required"); return 0; }
  const char *operation = (*env)->GetStringUTFChars(env, module, NULL);
  if(!operation) return 0;
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  double output = 0;
  const dt_mobile_status status = dt_mobile_tone_equalizer_set_target(session, operation, exposure, target,
                                                                    (uint64_t)gesture, &output);
  (*env)->ReleaseStringUTFChars(env, module, operation);
  throw_status(env, session, status);
  return status == DT_MOBILE_OK ? output : 0;
}
JNIEXPORT void JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_setModuleEnabled(JNIEnv *env, jobject self, jlong handle, jstring module, jboolean enabled)
{
  (void)self;
  if(!module) { throw_io(env, "a module is required"); return; }
  const char *operation = (*env)->GetStringUTFChars(env, module, NULL);
  if(!operation) return;
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  dt_mobile_status status = dt_mobile_set_enabled(session, operation, enabled == JNI_TRUE);
  (*env)->ReleaseStringUTFChars(env, module, operation);
  throw_status(env, session, status);
}
JNIEXPORT void JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_setCurve(JNIEnv *env, jobject self,
    jlong handle, jstring module, jint channel, jint interpolation, jfloatArray coordinates)
{
  (void)self;
  if(!module || !coordinates) { throw_io(env, "a curve module and coordinates are required"); return; }
  const jsize length = (*env)->GetArrayLength(env, coordinates);
  if(length < 4 || length > 40 || length % 2)
  { throw_io(env, "a curve needs 2 to 20 coordinate pairs"); return; }
  jfloat values[40];
  (*env)->GetFloatArrayRegion(env, coordinates, 0, length, values);
  if((*env)->ExceptionCheck(env)) return;
  const char *operation = (*env)->GetStringUTFChars(env, module, NULL);
  if(!operation) return;
  dt_mobile_curve_point points[20];
  const size_t count = (size_t)length / 2;
  for(size_t k = 0; k < count; k++) points[k] = (dt_mobile_curve_point) { values[2 * k], values[2 * k + 1] };
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  const dt_mobile_status status = dt_mobile_set_curve(session, operation, channel, interpolation, points, count);
  (*env)->ReleaseStringUTFChars(env, module, operation);
  throw_status(env, session, status);
}
JNIEXPORT void JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_setRgbCurveOptions(JNIEnv *env, jobject self,
    jlong handle, jstring module, jint mode, jint preserve_colors)
{
  (void)self;
  if(!handle || !module) { throw_io(env, "an image and RGB curve module are required"); return; }
  const char *operation = (*env)->GetStringUTFChars(env, module, NULL);
  if(!operation) return;
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  const dt_mobile_status status = dt_mobile_set_rgb_curve_options(session, operation, mode, preserve_colors);
  (*env)->ReleaseStringUTFChars(env, module, operation);
  throw_status(env, session, status);
}
JNIEXPORT void JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_setWhiteBalancePreset(JNIEnv *env, jobject self,
    jlong handle, jstring module, jint preset, jint tuning)
{
  (void)self;
  if(!handle || !module) { throw_io(env, "an image and white-balance module are required"); return; }
  const char *operation = (*env)->GetStringUTFChars(env, module, NULL);
  if(!operation) return;
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  const dt_mobile_status status = dt_mobile_set_whitebalance_preset(session, operation, preset, tuning);
  (*env)->ReleaseStringUTFChars(env, module, operation);
  throw_status(env, session, status);
}
JNIEXPORT jstring JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_maskPhotoPoints(JNIEnv *env, jobject self,
    jlong handle, jstring module)
{
  (void)self;
  if(!handle || !module) { throw_io(env, "an image and mask module are required"); return NULL; }
  const char *operation = (*env)->GetStringUTFChars(env, module, NULL);
  if(!operation) return NULL;
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  char *json = NULL;
  const dt_mobile_status status = dt_mobile_mask_photo_points(session, operation, &json);
  (*env)->ReleaseStringUTFChars(env, module, operation);
  if(status != DT_MOBILE_OK)
  { dt_mobile_free_buffer(json); throw_status(env, session, status); return NULL; }
  if(!json) { throw_io(env, "native mask projection is missing"); return NULL; }
  jstring result = new_utf8_string(env, json);
  dt_mobile_free_buffer(json);
  return result;
}
JNIEXPORT void JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_placeMaskPhoto(JNIEnv *env, jobject self,
    jlong handle, jstring module, jint form_id, jboolean source, jdouble x, jdouble y)
{
  (void)self;
  if(!handle || !module) { throw_io(env, "an image and mask module are required"); return; }
  const char *operation = (*env)->GetStringUTFChars(env, module, NULL);
  if(!operation) return;
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  const dt_mobile_status status = dt_mobile_mask_place_photo(session, operation, form_id, source ? 1 : 0, x, y);
  (*env)->ReleaseStringUTFChars(env, module, operation);
  throw_status(env, session, status);
}
JNIEXPORT void JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_setWhiteBalanceGains(JNIEnv *env, jobject self,
    jlong handle, jstring module, jfloatArray gains)
{
  (void)self;
  if(!handle || !module || !gains)
  { throw_io(env, "an image, white-balance module and complete gains are required"); return; }
  const jsize count = (*env)->GetArrayLength(env, gains);
  if(count != 3 && count != 4)
  { throw_io(env, "white balance requires three or four active channel gains"); return; }
  jfloat values[4];
  (*env)->GetFloatArrayRegion(env, gains, 0, count, values);
  if((*env)->ExceptionCheck(env)) return;
  const char *operation = (*env)->GetStringUTFChars(env, module, NULL);
  if(!operation) return;
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  const dt_mobile_status status = dt_mobile_set_whitebalance_gains(session, operation, values, (size_t)count);
  (*env)->ReleaseStringUTFChars(env, module, operation);
  throw_status(env, session, status);
}
JNIEXPORT void JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_setWhiteBalanceSetting(JNIEnv *env, jobject self,
    jlong handle, jstring module, jint setting)
{
  (void)self;
  if(!handle || !module) { throw_io(env, "an image and white-balance module are required"); return; }
  const char *operation = (*env)->GetStringUTFChars(env, module, NULL);
  if(!operation) return;
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  const dt_mobile_status status = dt_mobile_set_whitebalance_setting(session, operation, setting);
  (*env)->ReleaseStringUTFChars(env, module, operation);
  throw_status(env, session, status);
}
JNIEXPORT void JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_setWhiteBalanceTemperature(JNIEnv *env, jobject self,
    jlong handle, jstring module, jdouble kelvin, jdouble tint)
{
  (void)self;
  if(!handle || !module) { throw_io(env, "an image and white-balance module are required"); return; }
  const char *operation = (*env)->GetStringUTFChars(env, module, NULL);
  if(!operation) return;
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  const dt_mobile_status status = dt_mobile_set_whitebalance_temperature(session, operation, kelvin, tint);
  (*env)->ReleaseStringUTFChars(env, module, operation);
  throw_status(env, session, status);
}
JNIEXPORT void JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_setCrop(JNIEnv *env, jobject self,
    jlong handle, jstring module, jfloat left, jfloat top, jfloat right, jfloat bottom)
{
  (void)self;
  if(!module) { throw_io(env, "a crop module is required"); return; }
  const char *operation = (*env)->GetStringUTFChars(env, module, NULL);
  if(!operation) return;
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  const dt_mobile_status status = dt_mobile_set_crop(session, operation, left, top, right, bottom);
  (*env)->ReleaseStringUTFChars(env, module, operation);
  throw_status(env, session, status);
}
JNIEXPORT void JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_exportImage(JNIEnv *env, jobject self, jlong handle, jstring path, jstring format, jint quality)
{
  (void)self;
  if(!path || !format) { throw_io(env, "output path and format are required"); return; }
  const char *output = (*env)->GetStringUTFChars(env, path, NULL);
  if(!output) return;
  const char *type = (*env)->GetStringUTFChars(env, format, NULL);
  if(!type) { (*env)->ReleaseStringUTFChars(env, path, output); return; }
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  dt_mobile_status status = dt_mobile_export(session, output, type, quality);
  (*env)->ReleaseStringUTFChars(env, format, type);
  (*env)->ReleaseStringUTFChars(env, path, output);
  throw_status(env, session, status);
}
JNIEXPORT void JNICALL Java_org_example_darktableandroid_nativecore_NativeCore_exportWithSettings(JNIEnv *env, jobject self,
    jlong handle, jstring path, jstring format, jintArray values)
{
  (void)self;
  if(!handle || !path || !format || !values || (*env)->GetArrayLength(env, values) != 12)
  { throw_io(env, "an image, output path, format and 12 export settings are required"); return; }
  jint fields[12];
  (*env)->GetIntArrayRegion(env, values, 0, 12, fields);
  if((*env)->ExceptionCheck(env)) return;
  /* Reject signed Java dimensions before converting to the public uint32_t
   * fields. All remaining ranges and format constraints use native validation. */
  if(fields[0] < 0 || fields[1] < 0)
  { throw_io(env, "export dimensions must be nonnegative"); return; }
  const dt_mobile_export_settings settings = {
    .struct_size = sizeof(dt_mobile_export_settings),
    .max_width = (uint32_t)fields[0], .max_height = (uint32_t)fields[1],
    .quality = fields[2], .pixel_format = fields[3],
    .png_compression = fields[4], .tiff_compression = fields[5],
    .tiff_compression_level = fields[6], .jpeg_subsample = fields[7],
    .dpi = fields[8], .output_profile = fields[9],
    .rendering_intent = fields[10], .metadata_policy = fields[11]
  };
  const char *output = (*env)->GetStringUTFChars(env, path, NULL);
  if(!output) return;
  const char *type = (*env)->GetStringUTFChars(env, format, NULL);
  if(!type) { (*env)->ReleaseStringUTFChars(env, path, output); return; }
  dt_mobile_session *session = (dt_mobile_session *)(intptr_t)handle;
  const dt_mobile_status status = dt_mobile_export_with_settings(session, output, type, &settings);
  (*env)->ReleaseStringUTFChars(env, format, type);
  (*env)->ReleaseStringUTFChars(env, path, output);
  throw_status(env, session, status);
}
