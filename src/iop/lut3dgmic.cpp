/*
    This file is part of darktable,

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

#define cimg_verbosity 0
#include <gmic.h>
#include <iostream>
#include <string>

extern "C"
{
  // otherwise the name will be mangled and the linker won't be able to see the function ...
  void lut3d_decompress_clut(const unsigned char *const input_keypoints, const unsigned int nb_input_keypoints,
                             const unsigned int output_resolution, float *const output_clut_data,
                             const char *const filename);

  unsigned int lut3d_get_cached_clut(float *const output_clut_data, const unsigned int output_resolution,
                                     const char *const filename);

  gboolean lut3d_read_gmz(int *const nb_keypoints, unsigned char *const keypoints, const char *const filename,
                          int *const nb_lut, void *widget, const char *const lutname, const gboolean newlutname);

  void lut3d_add_lutname_to_list(void *g, const char *const lutname);

  void lut3d_clear_lutname_list(void *g);
}

void lut3d_decompress_clut(const unsigned char *const input_keypoints, const unsigned int nb_input_keypoints,
                           const unsigned int output_resolution, float *const output_clut_data,
                           const char *const filename)
{
  gmic_list<float> image_list;
  gmic_list<char> image_names;
  gmic g_instance;
  g_instance.verbosity = -1;
  char gmic_cmd[512];
  image_list.assign(1);
  gmic_image<float> &img = image_list[0];
  img.assign(1, nb_input_keypoints, 1, 6);
  // set the keypoint image
  float *ptr = img;
  for(size_t i = 0; i < nb_input_keypoints * 6; ++i) *(ptr++) = (float)input_keypoints[i];

  // decompress the keypoints to LUT
  // -cut 0,255 is added to mask GMIC 2.6.4 compressed clut issue.
  std::snprintf(gmic_cmd, sizeof(gmic_cmd), "-decompress_clut %u,%u,%u -cut 0,255", output_resolution,
                output_resolution, output_resolution);
  try
  {
    g_instance.run(gmic_cmd, image_list, image_names);
  }
  catch(gmic_exception &e) // In case something went wrong.
  {
    std::printf("[lut3d gmic] error: \"%s\"\n", e.what());
    image_list.assign(0);
    return;
  }
  // save in cache if possible - compatible G'MIC
  try
  {
    std::snprintf(gmic_cmd, sizeof(gmic_cmd), "-o \"%s\",uchar", filename);
    g_instance.run(gmic_cmd, image_list, image_names);
  }
  catch(...)
  {
    std::fprintf(stderr, "[lut3d gmic] error - saving cache LUT (does the cache folder exist?)\n");
  }
  // format for dt
  try
  {
    g_instance.run("-div 255.0 -permute cxyz", image_list, image_names); // scale to [0,1]
  }
  catch(gmic_exception &e) // In case something went wrong.
  {
    std::printf("[lut3d gmic] error: \"%s\"\n", e.what());
    image_list.assign(0);
    return;
  }
  const size_t img_size
      = image_list[0]._width * image_list[0]._height * image_list[0]._depth * image_list[0]._spectrum;
  std::memcpy(output_clut_data, image_list[0]._data, img_size * sizeof(float));
  image_list.assign(0);
}

unsigned int lut3d_get_cached_clut(float *const output_clut_data, const unsigned int output_resolution,
                                   const char *const filename)
{
  gmic_list<float> image_list;
  gmic_list<char> image_names;
  char gmic_cmd[512];
  gmic g_instance;
  g_instance.verbosity = -1;
  // get the cache file
  try
  {
    std::snprintf(gmic_cmd, sizeof(gmic_cmd), "-i \"%s\"", filename);
    g_instance.run(gmic_cmd, image_list, image_names);
  }
  catch(...)
  { // no cached lut
    image_list.assign(0);
    return 0;
  }
  // expected LUT size ?
  const unsigned int output_size = 3 * output_resolution * output_resolution * output_resolution;
  unsigned int output_res = output_resolution;
  size_t img_size = image_list[0]._width * image_list[0]._height * image_list[0]._depth * image_list[0]._spectrum;
  if(output_size < img_size) // downsize the cached lut
  {
    std::snprintf(gmic_cmd, sizeof(gmic_cmd), "-r %u,%u,%u,3,3", output_resolution, output_resolution,
                  output_resolution);
    try
    {
      g_instance.run(gmic_cmd, image_list, image_names);
    }
    catch(gmic_exception &e) // In case something went wrong.
    {
      std::printf("[lut3d gmic] error: \"%s\"\n", e.what());
      image_list.assign(0);
      return 0;
    }
    img_size = image_list[0]._width * image_list[0]._height * image_list[0]._depth * image_list[0]._spectrum;
  }
  else if(output_size > img_size) // reduce the expected lut size
  {
    output_res = image_list[0]._width;
  }
  // format for dt
  try
  {
    g_instance.run("-div 255.0 -permute cxyz", image_list, image_names); // scale to [0,1]
  }
  catch(gmic_exception &e) // In case something went wrong.
  {
    std::printf("[lut3d gmic] error: \"%s\"\n", e.what());
    image_list.assign(0);
    return 0;
  }
  std::memcpy(output_clut_data, image_list[0]._data, img_size * sizeof(float));
  image_list.assign(0);
  return output_res;
}

gboolean lut3d_read_gmz(int *const nb_keypoints, unsigned char *const keypoints, const char *const filename,
                        int *const nb_lut, void *g, const char *const lutname, const gboolean newlutname)
{
  gmic_list<float> image_list;
  gmic_list<char> image_names;
  char gmic_cmd[512];
  gmic g_instance;
  g_instance.verbosity = -1;
  gboolean lut_found = FALSE;
  // read the compressed lut file
  try
  {
    std::snprintf(gmic_cmd, sizeof(gmic_cmd), "-i \"%s\"", filename);
    g_instance.run(gmic_cmd, image_list, image_names);
  }
  catch(gmic_exception &e) // In case something went wrong.
  {
    std::printf("[lut3d gmic] error: \"%s\"\n", e.what());
    *nb_lut = 0;
    image_list.assign(0);
    image_names.assign(0);
    return lut_found;
  }
  unsigned int l = 0;
  if(lutname[0]) // find this specific lut
  {
    for(unsigned int i = 0; i < image_names._width; ++i)
    {
      if(strcmp(image_names[i]._data, lutname) == 0)
      {
        l = i;
        lut_found = TRUE;
        break;
      }
    }
  }
  *nb_lut = (int)image_names._width;
  if(!newlutname)
  { // list of luts for this new file
    lut3d_clear_lutname_list(g);
    for(unsigned int i = 0; i < image_names._width; ++i)
    {
      lut3d_add_lutname_to_list(g, image_names[i]._data);
    }
  }

  int nb_kp = *nb_keypoints = (int)image_list[l]._height;
  if(image_list[l]._width == 1 && image_list[l]._height <= 2048 && image_list[l]._depth == 1
     && image_list[l]._spectrum == 6)
  { // color lut
    gmic_image<float> &img = image_list[l];
    for(int i = 0; i < nb_kp * 6; ++i) keypoints[i] = (unsigned char)img[i];
  }
  else if(image_list[l]._width == 1 && image_list[l]._height <= 2048 && image_list[l]._depth == 1
          && image_list[l]._spectrum == 4)
  { // black & white lut
    gmic_image<float> &img = image_list[l];
    for(int i = 0; i < nb_kp * 3; ++i) keypoints[i] = (unsigned char)img[i];
    for(int i = 0; i < nb_kp; ++i)
      keypoints[nb_kp * 3 + i] = keypoints[nb_kp * 4 + i] = keypoints[nb_kp * 5 + i]
          = (unsigned char)img[nb_kp * 3 + i];
  }
  else
    std::printf("[lut3d gmic] error: incompatible compressed LUT [%u] %s\n", l, image_names[l]._data);

  image_list.assign(0);
  image_names.assign(0);
  return lut_found;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
