/*
 *    This file is part of darktable,
 *    copyright (c) 2013 johannes hanika.
 *    copyright (c) 2015 tobias ellinghaus.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/noiseprofiles.h"
#include "common/file_location.h"

// bump this when the noiseprofiles are getting a differen layout or meaning (raw-raw data, ...)
#define DT_NOISE_PROFILE_VERSION 0

const dt_noiseprofile_t dt_noiseprofile_generic = {N_("generic poissonian"), "", "", 0, {0.0001f, 0.0001f, 0.0001}, {0.0f, 0.0f, 0.0f}};

JsonParser *dt_noiseprofile_init(const char *alternative)
{
  GError *error = NULL;
  char filename[PATH_MAX] = { 0 };

  if(alternative == NULL)
  {
    // TODO: shall we look for profiles in the user config dir?
    char datadir[PATH_MAX] = { 0 };
    dt_loc_get_datadir(datadir, sizeof(datadir));
    snprintf(filename, sizeof(filename), "%s/%s", datadir, "noiseprofiles.json");
  }
  else
    snprintf(filename, sizeof(filename), "%s", alternative);

  dt_print(DT_DEBUG_CONTROL, "[noiseprofile] loading noiseprofiles from `%s'\n", filename);
  if(!g_file_test(filename, G_FILE_TEST_EXISTS)) return NULL;

  // TODO: shall we cache the content? for now this looks fast enough(TM)
  JsonParser *parser = json_parser_new();
  if(!json_parser_load_from_file(parser, filename, &error))
  {
    fprintf(stderr, "[noiseprofile] error: parsing json from `%s' failed\n%s\n", filename, error->message);
    g_error_free(error);
    g_object_unref(parser);
    return NULL;
  }
  return parser;
}

int is_member(gchar** names, char* name)
{
  while(*names)
  {
    if(!g_strcmp0(*names, name))
      return 1;
    names++;
  }
  return 0;
}

static gint _sort_by_iso(gconstpointer a, gconstpointer b)
{
  const dt_noiseprofile_t *profile_a = (dt_noiseprofile_t *)a;
  const dt_noiseprofile_t *profile_b = (dt_noiseprofile_t *)b;

  return profile_a->iso - profile_b->iso;
}

GList *dt_noiseprofile_get_matching(const dt_image_t *cimg)
{
  JsonParser *parser = darktable.noiseprofile_parser;
  JsonReader *reader = NULL;
  GList *result = NULL;

  JsonNode *root = json_parser_get_root(parser);
  if(!root)
  {
    fprintf(stderr, "[noiseprofile] error: can't get the root node\n");
    goto end;
  }

  reader = json_reader_new(root);

  if(!json_reader_read_member(reader, "version"))
  {
    fprintf(stderr, "[noiseprofile] error: can't find file version.\n");
    goto end;
  }

  // check the file version
  const int version = json_reader_get_int_value(reader);
  json_reader_end_member(reader);

  if(version != DT_NOISE_PROFILE_VERSION)
  {
    fprintf(stderr, "[noiseprofile] error: file version is not what this code understands\n");
    goto end;
  }

  if(!json_reader_read_member(reader, "noiseprofiles"))
  {
    fprintf(stderr, "[noiseprofile] error: can't find `noiseprofiles' entry.\n");
    goto end;
  }

  if(!json_reader_is_array(reader))
  {
    fprintf(stderr, "[noiseprofile] error: `noiseprofiles' is supposed to be an array\n");
    goto end;
  }

  // go through all makers
  const int n_makers = json_reader_count_elements(reader);
  dt_print(DT_DEBUG_CONTROL, "[noiseprofile] found %d makers\n", n_makers);
  for(int i = 0; i < n_makers; i++)
  {
    if(!json_reader_read_element(reader, i))
    {
      fprintf(stderr, "[noiseprofile] error: can't access maker at position %d / %d\n", i+1, n_makers);
      goto end;
    }

    if(!json_reader_read_member(reader, "maker"))
    {
      fprintf(stderr, "[noiseprofile] error: missing `maker`\n");
      goto end;
    }

    if(g_strstr_len(cimg->camera_maker, -1, json_reader_get_string_value(reader)))
    {
      dt_print(DT_DEBUG_CONTROL, "[noiseprofile] found `%s' as `%s'\n", cimg->camera_maker, json_reader_get_string_value(reader));
      // go through all models and check those
      json_reader_end_member(reader);

      if(!json_reader_read_member(reader, "models"))
      {
        fprintf(stderr, "[noiseprofile] error: missing `models`\n");
        goto end;
      }

      const int n_models = json_reader_count_elements(reader);
      dt_print(DT_DEBUG_CONTROL, "[noiseprofile] found %d models\n", n_models);
      for(int j = 0; j < n_models; j++)
      {
        if(!json_reader_read_element(reader, j))
        {
          fprintf(stderr, "[noiseprofile] error: can't access model at position %d / %d\n", j+1, n_models);
          goto end;
        }

        if(!json_reader_read_member(reader, "model"))
        {
          fprintf(stderr, "[noiseprofile] error: missing `model`\n");
          goto end;
        }

        if(!g_strcmp0(cimg->camera_model, json_reader_get_string_value(reader)))
        {
          dt_print(DT_DEBUG_CONTROL, "[noiseprofile] found %s\n", cimg->camera_model);
          // we got a match, return at most bufsize elements
          json_reader_end_member(reader);

          if(!json_reader_read_member(reader, "profiles"))
          {
            fprintf(stderr, "[noiseprofile] error: missing `profiles`\n");
            goto end;
          }

          const int n_profiles = json_reader_count_elements(reader);
          dt_print(DT_DEBUG_CONTROL, "[noiseprofile] found %d profiles\n", n_profiles);
          for(int k = 0; k < n_profiles; k++)
          {
            dt_noiseprofile_t tmp_profile = { 0 };

            if(!json_reader_read_element(reader, k))
            {
              fprintf(stderr, "[noiseprofile] error: can't access profile at position %d / %d\n", k+1, n_profiles);
              goto end;
            }

            gchar** member_names = json_reader_list_members(reader);

            // do we want to skip this entry?
            if(is_member(member_names, "skip"))
            {
              json_reader_read_member(reader, "skip");
              gboolean skip = json_reader_get_boolean_value(reader);
              json_reader_end_member(reader);
              if(skip)
              {
                json_reader_end_element(reader);
                g_strfreev(member_names);
                continue;
              }
            }

            // maker
            tmp_profile.maker = g_strdup(cimg->camera_maker);
            // model
            tmp_profile.model = g_strdup(cimg->camera_model);

            // name
            if(!is_member(member_names, "name"))
            {
              fprintf(stderr, "[noiseprofile] error: missing `name`\n");
              g_free(tmp_profile.maker);
              g_free(tmp_profile.model);
              g_strfreev(member_names);
              goto end;
            }
            json_reader_read_member(reader, "name");
            tmp_profile.name = g_strdup(json_reader_get_string_value(reader));
            json_reader_end_member(reader);

            // iso
            if(!is_member(member_names, "iso"))
            {
              fprintf(stderr, "[noiseprofile] error: missing `iso`\n");
              g_free(tmp_profile.name);
              g_free(tmp_profile.maker);
              g_free(tmp_profile.model);
              g_strfreev(member_names);
              goto end;
            }
            json_reader_read_member(reader, "iso");
            tmp_profile.iso = json_reader_get_double_value(reader);
            json_reader_end_member(reader);

            // a
            if(!is_member(member_names, "a"))
            {
              fprintf(stderr, "[noiseprofile] error: missing `a`\n");
              g_free(tmp_profile.name);
              g_free(tmp_profile.maker);
              g_free(tmp_profile.model);
              g_strfreev(member_names);
              goto end;
            }
            json_reader_read_member(reader, "a");
            if(json_reader_count_elements(reader) != 3)
            {
              fprintf(stderr, "[noiseprofile] error: `a` with size != 3\n");
              g_free(tmp_profile.name);
              g_free(tmp_profile.maker);
              g_free(tmp_profile.model);
              g_strfreev(member_names);
              goto end;
            }

            for(int a = 0; a < 3; a++)
            {
              json_reader_read_element(reader, a);
              tmp_profile.a[a] = json_reader_get_double_value(reader);
              json_reader_end_element(reader);
            }
            json_reader_end_member(reader);

            // b
            if(!is_member(member_names, "b"))
            {
              fprintf(stderr, "[noiseprofile] error: missing `b`\n");
              g_free(tmp_profile.name);
              g_free(tmp_profile.maker);
              g_free(tmp_profile.model);
              g_strfreev(member_names);
              goto end;
            }
            json_reader_read_member(reader, "b");
            if(json_reader_count_elements(reader) != 3)
            {
              fprintf(stderr, "[noiseprofile] error: `b` with size != 3\n");
              g_free(tmp_profile.name);
              g_free(tmp_profile.maker);
              g_free(tmp_profile.model);
              g_strfreev(member_names);
              goto end;
            }

            for(int b = 0; b < 3; b++)
            {
              json_reader_read_element(reader, b);
              tmp_profile.b[b] = json_reader_get_double_value(reader);
              json_reader_end_element(reader);
            }
            json_reader_end_member(reader);

            json_reader_end_element(reader);

            // everything worked out, add tmp_profile to result
            dt_noiseprofile_t *new_profile = (dt_noiseprofile_t *)malloc(sizeof(dt_noiseprofile_t));
            *new_profile = tmp_profile;
            result = g_list_append(result, new_profile);

            g_strfreev(member_names);
          } // profiles

          goto end;
        }

        json_reader_end_member(reader);
        json_reader_end_element(reader);
      } // models
    }

    json_reader_end_member(reader);
    json_reader_end_element(reader);
  } // makers

  json_reader_end_member(reader);

end:
  if(reader) g_object_unref(reader);
  if(result) result = g_list_sort(result, _sort_by_iso);
  return result;
}

void dt_noiseprofile_free(gpointer data)
{
  dt_noiseprofile_t *profile = (dt_noiseprofile_t *)data;
  g_free(profile->name);
  g_free(profile->maker);
  g_free(profile->model);
  free(profile);
}

void dt_noiseprofile_interpolate(
  const dt_noiseprofile_t *const p1,  // the smaller iso
  const dt_noiseprofile_t *const p2,  // the larger iso (can't be == iso1)
  dt_noiseprofile_t *out)             // has iso initialized
{
  // stupid linear interpolation.
  // to be confirmed for gaussian part.
  const float t = CLAMP(
    (float)(out->iso - p1->iso) / (float)(p2->iso - p1->iso),
                        0.0f, 1.0f);
  for(int k=0; k<3; k++)
  {
    out->a[k] = (1.0f-t)*p1->a[k] + t*p2->a[k];
    out->b[k] = (1.0f-t)*p1->b[k] + t*p2->b[k];
  }
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
