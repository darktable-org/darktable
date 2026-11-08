<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# App languages

Open **Settings → Language** and use the button menu to choose System default,
English, 繁體中文, 简体中文 or Español (Latinoamérica). The selection is saved.
Android 13 and newer use the platform per-app language setting, also exposed in
Android's application settings. Android 8–12 use a saved app preference and a
localized resource context. Changing languages recreates the Activity while
retaining its ViewModel, open native session and edit history.

The default English resources have complete Traditional Chinese (`zh-Hant`),
Simplified Chinese (`zh-Hans`) and Latin American Spanish (`es-419`) counterparts.
Each includes 352 translatable strings, of which 112 are additional mobile
native-control labels. Format arguments remain positional and unchanged.
Language names stay in their own language so the menu remains recognizable.

Native tool names, parameter/choice labels, curve labels and white-balance
presets use the existing desktop gettext catalogs packaged in
`darktable-runtime/locale/{zh_TW,zh_CN,es}/LC_MESSAGES/darktable.mo`. Native Spanish
labels reuse the upstream Spanish catalog; the Android UI and mobile-specific
overrides use Latin American Spanish. Unknown or untranslated desktop labels
fall back to their original text. Camera/lens/ICC profile names, file paths,
custom instance names, native addresses, values and history are not translated.

The catalog reader creates presentation copies without changing native locale,
`LC_ALL`, environment variables or JSON number formatting. Numeric editors accept
either a decimal point or decimal comma, including signed/scientific values,
without accepting grouping separators. Original finite-value, storage precision,
integer, range and cross-field validation still applies.

Host tests cover resource selection and regional fallback, the menu and stored
language, returning to System default, format arguments, gettext contexts and
malformed catalogs, native binding preservation and localized numeric inputs.
A legacy-branch test runs the pre-33 resource override under a host SDK 35
runtime; it does not establish execution on a physical Android 8–12 device.
The actual Kotlin reader also parsed all three compiled desktop MO catalogs in
a separate host probe. Physical-device language changes during RAW editing
remain unverified.
