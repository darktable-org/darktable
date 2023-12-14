/*
    This file is part of darktable,
    Copyright (C) 2017-2023 darktable developers.

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

#include "dtwin.h"
#include <setjmp.h>
#include <windows.h>

// Required by (at least) clang 10.0 as packaged by MSYS2 MinGW64.
#ifdef __clang__
#ifdef __MINGW32__ // 64-bit subsystem also sets this symbol
#include <errno.h>
#endif
#endif

const char *dtwin_get_locale()
{
  char *posix = NULL;
  LCID lcid;

  lcid = GetUserDefaultLCID();
  int lang_id = PRIMARYLANGID(lcid);
  int sub_id = SUBLANGID(lcid);

  switch(lang_id)
  {
    case LANG_AFRIKAANS:
      posix = "af";
      break;
    case LANG_ARABIC:
      posix = "ar";
      break;
    case LANG_AZERI:
      posix = "az";
      break;
    case LANG_BENGALI:
      posix = "bn";
      break;
    case LANG_BULGARIAN:
      posix = "bg";
      break;
    case LANG_CATALAN:
      posix = "ca";
      break;
    case LANG_CZECH:
      posix = "cs";
      break;
    case LANG_DANISH:
      posix = "da";
      break;
    case LANG_ESTONIAN:
      posix = "et";
      break;
    case LANG_PERSIAN:
      posix = "fa";
      break;
    case LANG_GERMAN:
      posix = "de";
      break;
    case LANG_GREEK:
      posix = "el";
      break;
    case LANG_ENGLISH:
      switch(sub_id)
      {
        case SUBLANG_ENGLISH_UK:
          posix = "en_GB";
          break;
        case SUBLANG_ENGLISH_AUS:
          posix = "en_AU";
          break;
        case SUBLANG_ENGLISH_CAN:
          posix = "en_CA";
          break;
        default:
          posix = "en";
          break;
      }
      break;
    case LANG_SPANISH:
      posix = "es";
      break;
    case LANG_BASQUE:
      posix = "eu";
      break;
    case LANG_FINNISH:
      posix = "fi";
      break;
    case LANG_FRENCH:
      posix = "fr";
      break;
    case LANG_GALICIAN:
      posix = "gl";
      break;
    case LANG_GUJARATI:
      posix = "gu";
      break;
    case LANG_HEBREW:
      posix = "he";
      break;
    case LANG_HINDI:
      posix = "hi";
      break;
    case LANG_HUNGARIAN:
      posix = "hu";
      break;
    case LANG_ICELANDIC:
      break;
    case LANG_INDONESIAN:
      posix = "id";
      break;
    case LANG_ITALIAN:
      posix = "it";
      break;
    case LANG_JAPANESE:
      posix = "ja";
      break;
    case LANG_GEORGIAN:
      posix = "ka";
      break;
    case LANG_KANNADA:
      posix = "kn";
      break;
    case LANG_KOREAN:
      posix = "ko";
      break;
    case LANG_LITHUANIAN:
      posix = "lt";
      break;
    case LANG_MACEDONIAN:
      posix = "mk";
      break;
    case LANG_DUTCH:
      posix = "nl";
      break;
    case LANG_NEPALI:
      posix = "ne";
      break;
    case LANG_NORWEGIAN:
      switch(sub_id)
      {
        case SUBLANG_NORWEGIAN_BOKMAL:
          posix = "nb";
          break;
        case SUBLANG_NORWEGIAN_NYNORSK:
          posix = "nn";
          break;
      }
      break;
    case LANG_PUNJABI:
      posix = "pa";
      break;
    case LANG_POLISH:
      posix = "pl";
      break;
    case LANG_PASHTO:
      posix = "ps";
      break;
    case LANG_PORTUGUESE:
      switch(sub_id)
      {
        case SUBLANG_PORTUGUESE_BRAZILIAN:
          posix = "pt_BR";
          break;
        default:
          posix = "pt";
          break;
      }
      break;
    case LANG_ROMANIAN:
      posix = "ro";
      break;
    case LANG_RUSSIAN:
      posix = "ru";
      break;
    case LANG_SLOVAK:
      posix = "sk";
      break;
    case LANG_SLOVENIAN:
      posix = "sl";
      break;
    case LANG_ALBANIAN:
      posix = "sq";
      break;
    /* LANG_CROATIAN == LANG_SERBIAN == LANG_BOSNIAN */
    case LANG_SERBIAN:
      switch(sub_id)
      {
        case SUBLANG_SERBIAN_LATIN:
        case SUBLANG_SERBIAN_BOSNIA_HERZEGOVINA_LATIN:
        case SUBLANG_SERBIAN_SERBIA_LATIN:
        case SUBLANG_SERBIAN_MONTENEGRO_LATIN:
          posix = "sr@latin";
          break;
        case SUBLANG_SERBIAN_CYRILLIC:
        case SUBLANG_SERBIAN_SERBIA_CYRILLIC:
          posix = "sr";
          break;
        case SUBLANG_SERBIAN_BOSNIA_HERZEGOVINA_CYRILLIC:
        case SUBLANG_SERBIAN_MONTENEGRO_CYRILLIC:
          posix = "sr@ije";
          break;
        case SUBLANG_BOSNIAN_BOSNIA_HERZEGOVINA_LATIN:
          posix = "bs";
          break;
        case SUBLANG_BOSNIAN_BOSNIA_HERZEGOVINA_CYRILLIC:
          posix = "bs@cyrillic";
          break;
        case SUBLANG_CROATIAN_CROATIA:
        case SUBLANG_CROATIAN_BOSNIA_HERZEGOVINA_LATIN:
          posix = "hr";
          break;
        default:
          posix = "hr";
      }
      break;
    case LANG_SWEDISH:
      posix = "sv";
      break;
    case LANG_TAMIL:
      posix = "ta";
      break;
    case LANG_TELUGU:
      posix = "te";
      break;
    case LANG_THAI:
      posix = "th";
      break;
    case LANG_TURKISH:
      posix = "tr";
      break;
    case LANG_UKRAINIAN:
      posix = "uk";
      break;
    case LANG_VIETNAMESE:
      posix = "vi";
      break;
    case LANG_XHOSA:
      posix = "xh";
      break;
    case LANG_CHINESE:
      switch(sub_id)
      {
        case SUBLANG_CHINESE_SIMPLIFIED:
          posix = "zh_CN";
          break;
        case SUBLANG_CHINESE_TRADITIONAL:
          posix = "zh_TW";
          break;
        default:
          posix = "zh";
          break;
      }
      break;
    case LANG_URDU:
      break;
    case LANG_BELARUSIAN:
      break;
    case LANG_LATVIAN:
      break;
    case LANG_ARMENIAN:
      break;
    case LANG_FAEROESE:
      break;
    case LANG_MALAY:
      break;
    case LANG_KAZAK:
      break;
    case LANG_KYRGYZ:
      break;
    case LANG_SWAHILI:
      break;
    case LANG_UZBEK:
      break;
    case LANG_TATAR:
      break;
    case LANG_ORIYA:
      break;
    case LANG_MALAYALAM:
      break;
    case LANG_ASSAMESE:
      break;
    case LANG_MARATHI:
      break;
    case LANG_SANSKRIT:
      break;
    case LANG_MONGOLIAN:
      break;
    case LANG_KONKANI:
      break;
    case LANG_MANIPURI:
      break;
    case LANG_SINDHI:
      break;
    case LANG_SYRIAC:
      break;
    case LANG_KASHMIRI:
      break;
    case LANG_DIVEHI:
      break;
  }

  /* Deal with exceptions */
  if(posix == NULL)
  {
    switch(lcid)
    {
      case 0x0455:
        posix = "my_MM";
        break; /* Myanmar (Burmese) */
      case 9999:
        posix = "ku";
        break; /* Kurdish (from NSIS) */
      default:
        posix = "en";
    }
  }

  return posix;
}

typedef struct tagTHREADNAME_INFO
{
  DWORD dwType;     // must be 0x1000
  LPCSTR szName;    // pointer to name (in user addr space)
  DWORD dwThreadID; // thread ID (-1=caller thread)
  DWORD dwFlags;    // reserved for future use, must be zero
} THREADNAME_INFO;

const DWORD MS_VC_EXCEPTION = 0x406D1388;

void dtwin_set_thread_name(DWORD dwThreadID, const char *threadName)
{
  THREADNAME_INFO info;
  info.dwType = 0x1000;
  info.szName = threadName;
  info.dwThreadID = dwThreadID;
  info.dwFlags = 0;

  // We set the thread name by raising an exception.
  // Regarding the alternative method and their comparison, see:
  // https://learn.microsoft.com/en-us/visualstudio/debugger/how-to-set-a-thread-name-in-native-code
  RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(DWORD), (const ULONG_PTR *)&info);
}

// This is taken from: https://gitlab.gnome.org/GNOME/glib/blob/main/gio/glocalfile.c#L2357
// The GLib version of this function always shows confirmation dialog boxes, unfortunately.
// This replacement does trashing without dialogs using "FOF_SILENT | FOF_NOCONFIRMATION".
// When GLib version on Windows will be able to trash silently we can remove this function.
boolean dt_win_file_trash(GFile *file, GCancellable *cancellable, GError **error)
{
  SHFILEOPSTRUCTA op = { 0 };
  gboolean success;
  char *filename;

  filename = g_file_get_parse_name(file);
  size_t len = strlen(filename);
  /* SHFILEOPSTRUCT.pFrom is double-zero-terminated */
  filename = g_renew(char, filename, len + 2);
  filename[len + 1] = 0;

  op.wFunc = FO_DELETE;
  op.pFrom = filename;
  op.fFlags = FOF_ALLOWUNDO | FOF_SILENT | FOF_NOCONFIRMATION;

  success = SHFileOperationA(&op) == 0;

  if(success && op.fAnyOperationsAborted)
  {
    if(cancellable && !g_cancellable_is_cancelled(cancellable)) g_cancellable_cancel(cancellable);
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(ECANCELED), "Unable to trash file %s: %s",
                g_file_get_parse_name(file), g_strerror(ECANCELED));
    success = FALSE;
  }
  else if(!success)
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(0), "Unable to trash file %s",
                g_file_get_parse_name(file));

  g_free(filename);
  return success;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
