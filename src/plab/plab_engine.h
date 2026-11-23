/*
    This file is part of PhotographyLAB, a darktable-based photo editor.
    Copyright (C) 2026 darktable developers.

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

// the toolkit-neutral face of darktable's engine for PhotographyLAB. plain C
// with no darktable or GTK types, so the Qt front end (and later an Android
// one) never includes a darktable header

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct plab_field_t
{
  const char *name;   // introspection field name, e.g. "exposure"
  double value;       // numeric value, or ignored when str is set
  const char *str;    // enum symbol or text for a char array field, else NULL
  int relative;       // add value to the image's base value instead of setting it
} plab_field_t;

typedef struct plab_layer_t
{
  const char *op;       // module operation, e.g. "colorbalancergb"
  int fresh;            // 0: the image's own instance; >0: the n-th extra
                        // instance of op, created when missing (LUT layers)
  int enabled;
  const plab_field_t *fields;
  int n_fields;
  float opacity;        // 0..100 enables uniform blending at that opacity; <0 leaves it
  const char *after;    // place after the first enabled module of these
                        // comma-separated ops, or NULL
  const char *label;    // instance name shown in darktable, or NULL
} plab_layer_t;

/** boot darktable without a GUI. args are darktable core options
    (--configdir, --library, --conf ...). returns 0 on success */
int plab_init(int argc, char **argv);
void plab_shutdown(void);

/** import a file into the library (or find it there); returns imgid or -1 */
int plab_import(const char *path, char **err);

/** number of history items darktable itself applied: what edits sit on top of.
    renders once if the image was never developed */
int plab_base_history(int imgid);

/** the as-shot white balance as a correlated color temperature in kelvin,
    0 when unknown */
double plab_as_shot_temperature(int imgid);

/** chromaticity for color calibration's custom illuminant that gives a
    white balance of `kelvin` and `tint` (-100..100, positive is magenta),
    keeping the as-shot illuminant's distance from the blackbody locus */
void plab_white_balance_xy(int imgid, double kelvin, double tint,
                           double *x, double *y);

/** truncate history to base_end, then write the layers on top */
int plab_apply(int imgid, int base_end, const plab_layer_t *layers, int n_layers,
               char **err);

/** render the current history into a newly g_malloc'd 8-bit BGRx buffer fitting
    max_w x max_h; history_end -1 renders everything */
uint8_t *plab_render(int imgid, int max_w, int max_h, int history_end,
                     int *out_w, int *out_h, char **err);

/** apply a style after truncating history to base_end; returns the new base */
int plab_apply_style(int imgid, int base_end, const char *name, char **err);

/** import a .dtstyle file, or every .dtstyle in a folder; returns how many */
int plab_import_styles(const char *path);

/** newline-separated style names, g_malloc'd */
char *plab_list_styles(void);

/** export full size; format from the extension (jpg, png, tif, webp...) */
int plab_export(int imgid, const char *out_path, int quality, char **err);

/** where lut3d resolves relative LUT paths; the app keeps its LUTs there */
void plab_set_lut_folder(const char *folder);

// --- library (darktable's catalog) ---

/** import every supported image in a folder; returns how many are in the
    catalog from it afterwards, or -1 */
int plab_import_folder(const char *folder, int recursive);

/** catalog folders as "film_id\tfolder\tcount" lines, g_malloc'd */
char *plab_list_folders(void);

/** images as "imgid\tpath\trating\tlabels\tedited" lines, by capture time.
    film_id < 0 means every folder. rating is 0..5, or 6 when rejected;
    labels is a bitmask of darktable's color labels (red 1, yellow 2 ...) */
char *plab_list_images(int film_id, int min_rating, int rejected_only);

/** 0..5 stars, or 6 to reject */
void plab_set_rating(int imgid, int rating);
void plab_toggle_color_label(int imgid, int color);

/** darktable's cached thumbnail (RGBx byte order) at about the given size, g_malloc'd */
uint8_t *plab_thumbnail(int imgid, int max_w, int max_h, int *out_w, int *out_h);

void plab_free(void *p);

#ifdef __cplusplus
}
#endif
