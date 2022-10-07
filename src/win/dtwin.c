/*
    This file is part of darktable,
    Copyright (C) 2017-2020 darktable developers.

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
// This platform combination is needed for dt appveyor build.
#ifdef __clang__
#ifdef __MINGW32__ // 64-bit subsystem also sets this symbol
#include <errno.h>
#endif
#endif

const wchar_t *dtwin_get_locale()
{
  wchar_t *posix = NULL;
  LCID lcid;

  lcid = GetUserDefaultLCID();
  int lang_id = PRIMARYLANGID(lcid);
  int sub_id = SUBLANGID(lcid);

  switch(lang_id)
  {
    case LANG_AFRIKAANS:
      posix = L"af";
      break;
    case LANG_ARABIC:
      posix = L"ar";
      break;
    case LANG_AZERI:
      posix = L"az";
      break;
    case LANG_BENGALI:
      posix = L"bn";
      break;
    case LANG_BULGARIAN:
      posix = L"bg";
      break;
    case LANG_CATALAN:
      posix = L"ca";
      break;
    case LANG_CZECH:
      posix = L"cs";
      break;
    case LANG_DANISH:
      posix = L"da";
      break;
    case LANG_ESTONIAN:
      posix = L"et";
      break;
    case LANG_PERSIAN:
      posix = L"fa";
      break;
    case LANG_GERMAN:
      posix = L"de";
      break;
    case LANG_GREEK:
      posix = L"el";
      break;
    case LANG_ENGLISH:
      switch(sub_id)
      {
        case SUBLANG_ENGLISH_UK:
          posix = L"en_GB";
          break;
        case SUBLANG_ENGLISH_AUS:
          posix = L"en_AU";
          break;
        case SUBLANG_ENGLISH_CAN:
          posix = L"en_CA";
          break;
        default:
          posix = L"en";
          break;
      }
      break;
    case LANG_SPANISH:
      posix = L"es";
      break;
    case LANG_BASQUE:
      posix = L"eu";
      break;
    case LANG_FINNISH:
      posix = L"fi";
      break;
    case LANG_FRENCH:
      posix = L"fr";
      break;
    case LANG_GALICIAN:
      posix = L"gl";
      break;
    case LANG_GUJARATI:
      posix = L"gu";
      break;
    case LANG_HEBREW:
      posix = L"he";
      break;
    case LANG_HINDI:
      posix = L"hi";
      break;
    case LANG_HUNGARIAN:
      posix = L"hu";
      break;
    case LANG_ICELANDIC:
      break;
    case LANG_INDONESIAN:
      posix = L"id";
      break;
    case LANG_ITALIAN:
      posix = L"it";
      break;
    case LANG_JAPANESE:
      posix = L"ja";
      break;
    case LANG_GEORGIAN:
      posix = L"ka";
      break;
    case LANG_KANNADA:
      posix = L"kn";
      break;
    case LANG_KOREAN:
      posix = L"ko";
      break;
    case LANG_LITHUANIAN:
      posix = L"lt";
      break;
    case LANG_MACEDONIAN:
      posix = L"mk";
      break;
    case LANG_DUTCH:
      posix = L"nl";
      break;
    case LANG_NEPALI:
      posix = L"ne";
      break;
    case LANG_NORWEGIAN:
      switch(sub_id)
      {
        case SUBLANG_NORWEGIAN_BOKMAL:
          posix = L"nb";
          break;
        case SUBLANG_NORWEGIAN_NYNORSK:
          posix = L"nn";
          break;
      }
      break;
    case LANG_PUNJABI:
      posix = L"pa";
      break;
    case LANG_POLISH:
      posix = L"pl";
      break;
    case LANG_PASHTO:
      posix = L"ps";
      break;
    case LANG_PORTUGUESE:
      switch(sub_id)
      {
        case SUBLANG_PORTUGUESE_BRAZILIAN:
          posix = L"pt_BR";
          break;
        default:
          posix = L"pt";
          break;
      }
      break;
    case LANG_ROMANIAN:
      posix = L"ro";
      break;
    case LANG_RUSSIAN:
      posix = L"ru";
      break;
    case LANG_SLOVAK:
      posix = L"sk";
      break;
    case LANG_SLOVENIAN:
      posix = L"sl";
      break;
    case LANG_ALBANIAN:
      posix = L"sq";
      break;
    /* LANG_CROATIAN == LANG_SERBIAN == LANG_BOSNIAN */
    case LANG_SERBIAN:
      switch(sub_id)
      {
        case SUBLANG_SERBIAN_LATIN:
          posix = L"sr@Latn";
          break;
        case SUBLANG_SERBIAN_CYRILLIC:
          posix = L"sr";
          break;
        case SUBLANG_BOSNIAN_BOSNIA_HERZEGOVINA_CYRILLIC:
        case SUBLANG_BOSNIAN_BOSNIA_HERZEGOVINA_LATIN:
          posix = L"bs";
          break;
        case SUBLANG_CROATIAN_BOSNIA_HERZEGOVINA_LATIN:
          posix = L"hr";
          break;
      }
      break;
    case LANG_SWEDISH:
      posix = L"sv";
      break;
    case LANG_TAMIL:
      posix = L"ta";
      break;
    case LANG_TELUGU:
      posix = L"te";
      break;
    case LANG_THAI:
      posix = L"th";
      break;
    case LANG_TURKISH:
      posix = L"tr";
      break;
    case LANG_UKRAINIAN:
      posix = L"uk";
      break;
    case LANG_VIETNAMESE:
      posix = L"vi";
      break;
    case LANG_XHOSA:
      posix = L"xh";
      break;
    case LANG_CHINESE:
      switch(sub_id)
      {
        case SUBLANG_CHINESE_SIMPLIFIED:
          posix = L"zh_CN";
          break;
        case SUBLANG_CHINESE_TRADITIONAL:
          posix = L"zh_TW";
          break;
        default:
          posix = L"zh";
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
        posix = L"my_MM";
        break; /* Myanmar (Burmese) */
      case 9999:
        posix = L"ku";
        break; /* Kurdish (from NSIS) */
      default:
        posix = L"en";
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

  // Yes, don't get heart attack, naming of thread is done by raising a special exception on Windows
  // https://msdn.microsoft.com/en-us/library/xcb2z8hs(v=vs.71).aspx
  RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(DWORD), (const ULONG_PTR *)&info);
}

// This is taken from: https://git.gnome.org/browse/glib/tree/gio/glocalfile.c#n2269
// The glib version of this function unfortunately shows always confirmation dialog boxes
// This version does thrashing silently, without dialog boxes: FOF_SILENT | FOF_NOCONFIRMATION
// When glib version on Windows will do silent trashing we can remove this function
boolean dt_win_file_trash(GFile *file, GCancellable *cancellable, GError **error)
{
  SHFILEOPSTRUCTW op = { 0 };
  gboolean success;
  wchar_t *wfilename;
  long len;

  wfilename = g_utf8_to_utf16(g_file_get_parse_name(file), -1, NULL, &len, NULL);
  /* SHFILEOPSTRUCT.pFrom is double-zero-terminated */
  wfilename = g_renew(wchar_t, wfilename, len + 2);
  wfilename[len + 1] = 0;

  op.wFunc = FO_DELETE;
  op.pFrom = wfilename;
  op.fFlags = FOF_ALLOWUNDO | FOF_SILENT | FOF_NOCONFIRMATION;

  success = SHFileOperationW(&op) == 0;

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

  g_free(wfilename);
  return success;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

