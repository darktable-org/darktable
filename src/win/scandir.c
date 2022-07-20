/*
    This file is part of darktable,
    Copyright (C) 2022 darktable developers.

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

/*
    scandir: Scan a directory, collecting all (selected) items into a an array.

    The original implementation of scandir has been made by Richard Salz.
    The original author put this code in the public domain.

    It has been modified to simplify slightly and increae readability.
*/

#include <sys/types.h>
#include <dirent.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include "scandir.h"

int alphasort(const struct dirent** dirent1, const struct dirent** dirent2)
{
  return (strcmp((*(const struct dirent **)dirent1)->d_name,
                 (*(const struct dirent **)dirent2)->d_name));
}

/* Initial guess at directory allocated.  */
#define INITIAL_ALLOCATION 20

int scandir(const char *directory_name,
            struct dirent ***array_pointer,
            int (*select_function) (const struct dirent *),
            int (*compare_function) (const struct dirent**, const struct dirent**))
{
  struct dirent **array;
  struct dirent *entry;
  struct dirent *copy;
  int allocated = INITIAL_ALLOCATION;
  int counter = 0;

  /* Get initial list space and open directory.  */
  DIR *directory = opendir(directory_name);
  if(directory == NULL) return -1;

  array = (struct dirent **) malloc(allocated * sizeof(struct dirent *));
  if(array == NULL) return -1;

  /* Read entries in the directory.  */

  while(entry = readdir(directory), entry)
    if(select_function == NULL || (*select_function) (entry))
    {
      /* User wants them all, or he wants this one.  Copy the entry.  */

      /*
       * On some OSes the declaration of "entry->d_name" is a minimal-length
       * placeholder.  Example: Solaris:
       *         /usr/include/sys/dirent.h:
       *                "char d_name[1];"
       *        man page "dirent(3)":
       *                The field d_name is the beginning of the character array
       *                giving the name of the directory entry. This name is
       *                null terminated and may have at most MAXNAMLEN chars.
       * So our malloc length may need to be increased accordingly.
       *        sizeof(entry->d_name): space (possibly minimal) in struct.
       *         strlen(entry->d_name): actual length of the entry. 
       *
       *                        John Kavadias <john_kavadias@hotmail.com>
       *                        David Lee <t.d.lee@durham.ac.uk>
       */
      const int namelength = strlen(entry->d_name) + 1;        /* length with NULL */
      int extra = 0;

      if(sizeof(entry->d_name) < namelength)
      {
        /* allocated space <= required space */
        extra += namelength - sizeof(entry->d_name);
      }

      copy = (struct dirent *) malloc(sizeof(struct dirent) + extra);
      if(copy == NULL)
      {
        closedir(directory);
        free(array);
        return -1;
      }
      copy->d_ino = entry->d_ino;
      copy->d_reclen = entry->d_reclen;
      strcpy(copy->d_name, entry->d_name);

      /* Save the copy.  */

      if(counter + 1 == allocated)
      {
        allocated <<= 1;
        void *newarray = (struct dirent **) realloc((char *) array, allocated * sizeof(struct dirent *));
        if(newarray != NULL)
          array = newarray;
        else
        {
          closedir(directory);
          free(array);
          free(copy);
          return -1;
        }
      }
      array[counter++] = copy;
    }

  /* Close things off.  */

  array[counter] = NULL;
  *array_pointer = array;
  closedir(directory);

  /* Sort?  */

  if(counter > 1 && compare_function)
    qsort((char *) array,
          counter,
          sizeof (struct dirent *),
          (int (*)(const void *, const void *))(compare_function));

  return counter;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
