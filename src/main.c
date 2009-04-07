#include "common/darktable.h"
#include "gui/gtk.h"
#include <stdlib.h>

int main (int argc, char *argv[])
{
  if(dt_init(argc, argv)) exit(1);
  dt_gui_gtk_run(darktable.gui);
  exit(0);
}

