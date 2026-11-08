/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "common/module_layout.h"
#include <stdio.h>

static int check_name(const char *filename, const char *category, const char *expected)
{
  gchar *actual = dt_module_name_from_filename(filename, dt_module_android_prefix(category), ".so");
  const int success = !g_strcmp0(actual, expected);
  if(!success) fprintf(stderr, "%s in %s: expected %s, got %s\n", filename, category,
                      expected ? expected : "rejected", actual ? actual : "rejected");
  g_free(actual);
  return success;
}

int main(void)
{
  /* A single Android nativeLibraryDir also contains the core and dependencies.
   * The same logical name can occur in different plugin categories. */
  if(!check_name("libdt_iop_exposure.so", "/plugins", "exposure")
     || !check_name("libdt_format_jpegxl.so", "/plugins/imageio/format", "jpegxl")
     || !check_name("libdt_storage_disk.so", "/plugins/imageio/storage", "disk")
     || !check_name("libdt_view_darkroom.so", "/views", "darkroom")
     || !check_name("libdt_lib_export.so", "/plugins/lighttable", "export")
     || !check_name("libdt_iop_png.so", "/plugins/imageio/format", NULL)
     || !check_name("libdt_format_png.so", "/plugins", NULL)
     || !check_name("libpng.so", "/plugins/imageio/format", NULL)
     || !check_name("libdarktable.so", "/plugins", NULL)
     || !check_name("libdt_mobile.so", "/plugins", NULL)
     || !check_name("libdt_iop_.so", "/plugins", NULL)
     || !check_name("libdt_iop_exposure.so.old", "/plugins", NULL)
     || !check_name("libdt_iop_../exposure.so", "/plugins", NULL)
     || !check_name("libdt_iop_exposure.so", "/unknown", NULL)) return 1;

  gchar *desktop = dt_module_name_from_filename("libexposure.so", "lib", ".so");
  const int desktop_ok = !g_strcmp0(desktop, "exposure");
  g_free(desktop);
  if(!desktop_ok) return 1;
  return 0;
}
