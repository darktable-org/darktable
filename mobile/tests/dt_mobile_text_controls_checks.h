/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_TEXT_CONTROLS_CHECKS_H
#define DT_MOBILE_TEXT_CONTROLS_CHECKS_H

static int mobile_text_controls_checks(dt_mobile_session *session)
{
  char *baseline = NULL, *metadata = NULL, *history = NULL, *after_history = NULL;
  int success = 0;
  ENGINE_OK(dt_mobile_modules(session, &baseline));
  CHECK(module_has(baseline, "watermark", "\"textParameters\":[{"));
  CHECK(module_has(baseline, "watermark", "\"name\":\"text\""));
  /* Includes BMP and supplementary Unicode; the native protocol is UTF-8. */
  const char text[] = "Photo \xe7\x85\xa7\xe7\x89\x87 \xf0\x9f\x8c\x85";
  ENGINE_OK(dt_mobile_set_text_parameter(session, "watermark", "text", text));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(module_has(metadata, "watermark", text));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_history(session, &history));
  char too_long[513]; memset(too_long, 'a', sizeof(too_long) - 1); too_long[512] = '\0';
  CHECK(dt_mobile_set_text_parameter(session, "watermark", "text", too_long)
          == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_text_parameter(session, "watermark", "text", "\xed\xa0\x80")
          == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_text_parameter(session, "watermark", "filename", "../outside.svg")
          == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_text_parameter(session, "watermark", "font", NULL)
          == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_text_parameter(session, "watermark", "text[0]", "x")
          == DT_MOBILE_ERROR_UNSUPPORTED);
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(!strcmp(history, after_history));
  dt_mobile_free_buffer(after_history); after_history = NULL;
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(module_has(metadata, "watermark", text));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(baseline, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_redo(session));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(module_has(metadata, "watermark", text));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_set_text_parameter(session, "watermark", "text", ""));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(module_has(metadata, "watermark", "\"name\":\"text\",\"label\":\"watermark text\",\"value\":\"\""));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(baseline, metadata));
  success = 1;
failed:
  dt_mobile_free_buffer(baseline);
  dt_mobile_free_buffer(metadata);
  dt_mobile_free_buffer(history);
  dt_mobile_free_buffer(after_history);
  return success;
}

#endif
