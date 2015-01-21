
/*
    This file is part of darktable,
    copyright (c) 2013 jeremy Rosen

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
#ifndef DT_IMAGEIO_FORMAT_H
#define DT_IMAGEIO_FORMAT_H
#ifdef __cplusplus
extern "C" {
#endif
/* early definition of modules to do type checking */
void init(struct dt_imageio_module_format_t *self);
void cleanup(struct dt_imageio_module_format_t *self);
void gui_reset(struct dt_imageio_module_format_t *self);
void gui_init(struct dt_imageio_module_format_t *self);
void gui_cleanup(struct dt_imageio_module_format_t *self);
const char *mime(dt_imageio_module_data_t *data);
const char *extension(dt_imageio_module_data_t *data);
int dimension(struct dt_imageio_module_format_t *self, dt_imageio_module_data_t *data, uint32_t *width, uint32_t *height);
size_t params_size(struct dt_imageio_module_format_t *self);
void *get_params(struct dt_imageio_module_format_t *self);
void free_params(struct dt_imageio_module_format_t *self, dt_imageio_module_data_t *data);
int set_params(struct dt_imageio_module_format_t *self, const void *params, const int size);
int write_image(dt_imageio_module_data_t *data, const char *filename, const void *in, void *exif,
                int exif_len, int imgid, int num, int total);
int bpp(dt_imageio_module_data_t *data);
int flags(dt_imageio_module_data_t *data);
int levels(dt_imageio_module_data_t *data);
int read_image(dt_imageio_module_data_t *data, uint8_t *out);
const char *name();
#ifdef __cplusplus
}
#endif
#endif
