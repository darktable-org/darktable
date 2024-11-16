/*
    This file is part of darktable,
    Copyright (C) 2024 darktable developers.

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
The GIMP support CLI API

Added --gimp <mode> option to the cli interface. <mode> is always a string, can be "version" "file" "thumb"

Whenever we call 'darktable --gimp' the results are written to 'stdout'. All requested results are in a block encapsulated by

a start line `\n<<<gimp\n` and
a final line `\ngimp>>>\n`

for defined interpretation.

(This allows darktable to use the debug logs and avoids problems with libraries writing to output in an uncontrolled manner.)

In case of an error the <res> is 'error'

The exitcode of darktable while using any --gimp option reflects the error status.

version            Returns the current API version (for initial commit will be 1)
file <path>        Starts darktable in darkroom mode using image defined by <path>.
                   When closing the darkroom window the file is exported as an
                   xcf file to a temporary location.
                   The full path of the exported file is returned as <res>
thumb <path> <dim> Write a thumbnail jpg file to a temporary location.
                   <path> is the image file
                   <dim> (in pixels) is used for the greater of width/height and ratio is kept.
                   The returned <res> has the following format:
                   * The full path of the exported file on the first line
                   * The width and height as space-separated integers on the second line.
                     These dimensions are informational and may not be accurate.
                     For raw files this is sensor size containing valid data.
*/


#define DT_GIMP_VERSION 1

// returns TRUE in case of success
gboolean dt_export_gimp_file(const dt_imgid_t id);

dt_imgid_t dt_gimp_load_darkroom(const char *file);
dt_imgid_t dt_gimp_load_image(const char *file);
