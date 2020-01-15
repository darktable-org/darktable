/*
    This file is part of darktable,
    copyright (c) 2018 edgardo hoszowski.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/darktable.h"
#include "common/iop_order.h"
#include "common/styles.h"
#include "common/debug.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/pixelpipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DT_IOP_ORDER_VERSION 5

#define DT_IOP_ORDER_INFO (darktable.unmuted & DT_DEBUG_IOPORDER)

static void _ioppr_reset_iop_order(GList *iop_order_list);

/** Note :
 * we do not use finite-math-only and fast-math because divisions by zero are not manually avoided in the code
 * fp-contract=fast enables hardware-accelerated Fused Multiply-Add
 * the rest is loop reorganization and vectorization optimization
 **/
#if defined(__GNUC__)
#pragma GCC optimize ("unroll-loops", "tree-loop-if-convert", \
                      "tree-loop-distribution", "no-strict-aliasing", \
                      "loop-interchange", "loop-nest-optimize", "tree-loop-im", \
                      "unswitch-loops", "tree-loop-ivcanon", "ira-loop-pressure", \
                      "split-ivs-in-unroller", "variable-expansion-in-unroller", \
                      "split-loops", "ivopts", "predictive-commoning",\
                      "tree-loop-linear", "loop-block", "loop-strip-mine", \
                      "fp-contract=fast", \
                      "tree-vectorize")
#endif

// note legacy_order & recommended_order have the original iop-order double that is
// used only for the initial database migration.
//
// in the new code only the iop-order as int is used to order the module on the GUI.

// @@_NEW_MOUDLE: For new module it is required to insert the new module name in both lists below.

const dt_iop_order_entry_t legacy_order[] = {
  { { 1.0f }, "rawprepare", 0},
  { { 2.0f }, "invert", 0},
  { { 3.0f }, "temperature", 0},
  { { 4.0f }, "highlights", 0},
  { { 5.0f }, "cacorrect", 0},
  { { 6.0f }, "hotpixels", 0},
  { { 7.0f }, "rawdenoise", 0},
  { { 8.0f }, "demosaic", 0},
  { { 9.0f }, "mask_manager", 0},
  { {10.0f }, "denoiseprofile", 0},
  { {11.0f }, "tonemap", 0},
  { {12.0f }, "exposure", 0},
  { {13.0f }, "spots", 0},
  { {14.0f }, "retouch", 0},
  { {15.0f }, "lens", 0},
  { {16.0f }, "ashift", 0},
  { {17.0f }, "liquify", 0},
  { {18.0f }, "rotatepixels", 0},
  { {19.0f }, "scalepixels", 0},
  { {20.0f }, "flip", 0},
  { {21.0f }, "clipping", 0},
  { {21.5f }, "toneequal", 0},
  { {22.0f }, "graduatednd", 0},
  { {23.0f }, "basecurve", 0},
  { {24.0f }, "bilateral", 0},
  { {25.0f }, "profile_gamma", 0},
  { {26.0f }, "hazeremoval", 0},
  { {27.0f }, "colorin", 0},
  { {27.5f }, "basicadj", 0},
  { {28.0f }, "colorreconstruct", 0},
  { {29.0f }, "colorchecker", 0},
  { {30.0f }, "defringe", 0},
  { {31.0f }, "equalizer", 0},
  { {32.0f }, "vibrance", 0},
  { {33.0f }, "colorbalance", 0},
  { {34.0f }, "colorize", 0},
  { {35.0f }, "colortransfer", 0},
  { {36.0f }, "colormapping", 0},
  { {37.0f }, "bloom", 0},
  { {38.0f }, "nlmeans", 0},
  { {39.0f }, "globaltonemap", 0},
  { {40.0f }, "shadhi", 0},
  { {41.0f }, "atrous", 0},
  { {42.0f }, "bilat", 0},
  { {43.0f }, "colorzones", 0},
  { {44.0f }, "lowlight", 0},
  { {45.0f }, "monochrome", 0},
  { {46.0f }, "filmic", 0},
  { {46.5f }, "filmicrgb", 0},
  { {47.0f }, "colisa", 0},
  { {48.0f }, "zonesystem", 0},
  { {49.0f }, "tonecurve", 0},
  { {50.0f }, "levels", 0},
  { {50.2f }, "rgblevels", 0},
  { {50.5f }, "rgbcurve", 0},
  { {51.0f }, "relight", 0},
  { {52.0f }, "colorcorrection", 0},
  { {53.0f }, "sharpen", 0},
  { {54.0f }, "lowpass", 0},
  { {55.0f }, "highpass", 0},
  { {56.0f }, "grain", 0},
  { {56.5f }, "lut3d", 0},
  { {57.0f }, "colorcontrast", 0},
  { {58.0f }, "colorout", 0},
  { {59.0f }, "channelmixer", 0},
  { {60.0f }, "soften", 0},
  { {61.0f }, "vignette", 0},
  { {62.0f }, "splittoning", 0},
  { {63.0f }, "velvia", 0},
  { {64.0f }, "clahe", 0},
  { {65.0f }, "finalscale", 0},
  { {66.0f }, "overexposed", 0},
  { {67.0f }, "rawoverexposed", 0},
  { {67.5f }, "dither", 0},
  { {68.0f }, "borders", 0},
  { {69.0f }, "watermark", 0},
  { {71.0f }, "gamma", 0},
  { { 0.0f }, "", 0}
};

const dt_iop_order_entry_t recommended_order[] = {
  { { 1.0 }, "rawprepare", 0},
  { { 2.0 }, "invert", 0},
  { { 3.0f }, "temperature", 0},
  { { 4.0f }, "highlights", 0},
  { { 5.0f }, "cacorrect", 0},
  { { 6.0f }, "hotpixels", 0},
  { { 7.0f }, "rawdenoise", 0},
  { { 8.0f }, "demosaic", 0},
  { { 9.0f }, "denoiseprofile", 0},
  { {10.0f }, "bilateral", 0},
  { {11.0f }, "rotatepixels", 0},
  { {12.0f }, "scalepixels", 0},
  { {13.0f }, "lens", 0},
  { {14.0f }, "hazeremoval", 0},
  { {15.0f }, "ashift", 0},
  { {16.0f }, "flip", 0},
  { {17.0f }, "clipping", 0},
  { {18.0f }, "liquify", 0},
  { {19.0f }, "spots", 0},
  { {20.0f }, "retouch", 0},
  { {21.0f }, "exposure", 0},
  { {22.0f }, "mask_manager", 0},
  { {23.0f }, "tonemap", 0},
  { {24.0f }, "toneequal", 0},
  { {25.0f }, "graduatednd", 0},
  { {26.0f }, "profile_gamma", 0},
  { {27.0f }, "equalizer", 0},
  { {28.0f }, "colorin", 0},
  { {29.0f }, "nlmeans", 0},         // signal processing (denoising)
                                  //    -> needs a signal as scene-referred as possible (even if it works in Lab)
  { {30.0f }, "colorchecker", 0},    // calibration to "neutral" exchange colour space
                                  //    -> improve colour calibration of colorin and reproductibility
                                  //    of further edits (styles etc.)
  { {31.0f }, "defringe", 0},        // desaturate fringes in Lab, so needs properly calibrated colours
                                  //    in order for chromaticity to be meaningful,
  { {32.0f }, "atrous", 0},          // frequential operation, needs a signal as scene-referred as possible to avoid halos
  { {33.0f }, "lowpass", 0},         // same
  { {34.0f }, "highpass", 0},        // same
  { {35.0f }, "sharpen", 0},         // same, worst than atrous in same use-case, less control overall
  { {36.0f }, "lut3d", 0},           // apply a creative style or film emulation, possibly non-linear,
                                  //    so better move it after frequential ops that need L2 Hilbert spaces
                                  //    of square summable functions
  { {37.0f }, "colortransfer", 0},   // probably better if source and destination colours are neutralized in the same
                                  //    colour exchange space, hence after colorin and colorcheckr,
                                  //    but apply after frequential ops in case it does non-linear witchcraft,
                                  //    just to be safe
  { {38.0f }, "colormapping", 0},    // same
  { {39.0f }, "channelmixer", 0},    // does exactly the same thing as colorin, aka RGB to RGB matrix conversion,
                                  //    but coefs are user-defined instead of calibrated and read from ICC profile.
                                  //    Really versatile yet under-used module, doing linear ops,
                                  //    very good in scene-referred workflow
  { {40.0f }, "basicadj", 0},        // module mixing view/model/control at once, usage should be discouraged
  { {41.0f }, "colorbalance", 0},    // scene-referred color manipulation
  { {42.0f }, "rgbcurve", 0},        // really versatile way to edit colour in scene-referred and display-referred workflow
  { {43.0f }, "rgblevels", 0},       // same
  { {44.0f }, "basecurve", 0},       // conversion from scene-referred to display referred, reverse-engineered
                                  //    on camera JPEG default look
  { {45.0f }, "filmic", 0},          // same, but different (parametric) approach
  { {46.0f }, "filmicrgb", 0},       // same, upgraded
  { {47.0f }, "colisa", 0},          // edit contrast while damaging colour
  { {48.0f }, "tonecurve", 0},       // same
  { {49.0f }, "levels", 0},          // same
  { {50.0f }, "shadhi", 0},          // same
  { {51.0f }, "zonesystem", 0},      // same
  { {52.0f }, "globaltonemap", 0},   // same
  { {53.0f }, "relight", 0},         // flatten local contrast while pretending do add lightness
  { {54.0f }, "bilat", 0},           // improve clarity/local contrast after all the bad things we have done
                                  //    to it with tonemapping
  { {55.0f }, "colorcorrection", 0}, // now that the colours have been damaged by contrast manipulations,
                                  // try to recover them - global adjustment of white balance for shadows and highlights
  { {56.0f }, "colorcontrast", 0},   // adjust chrominance globally
  { {57.0f }, "velvia", 0},          // same
  { {58.0f }, "vibrance", 0},        // same, but more subtle
  { {60.0f }, "colorzones", 0},      // same, but locally
  { {61.0f }, "bloom", 0},           // creative module
  { {62.0f }, "colorize", 0},        // creative module
  { {63.0f }, "lowlight", 0},        // creative module
  { {64.0f }, "monochrome", 0},      // creative module
  { {65.0f }, "grain", 0},           // creative module
  { {66.0f }, "soften", 0},          // creative module
  { {67.0f }, "splittoning", 0},     // creative module
  { {68.0f }, "vignette", 0},        // creative module
  { {69.0f }, "colorreconstruct", 0},// try to salvage blown areas before ICC intents in LittleCMS2 do things with them.
  { {70.0f }, "colorout", 0},
  { {71.0f }, "clahe", 0},
  { {72.0f }, "finalscale", 0},
  { {73.0f }, "overexposed", 0},
  { {74.0f }, "rawoverexposed", 0},
  { {75.0f }, "dither", 0},
  { {76.0f }, "borders", 0},
  { {77.0f }, "watermark", 0},
  { {78.0f }, "gamma", 0},
  { { 0.0f }, "", 0 }
};

#if 0
static GList *_insert_before(GList *iop_order_list, const char *module, const char *new_module)
{
  gboolean exists = FALSE;

  // first check that new module is missing

  GList *l = iop_order_list;

  while(l)
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)l->data;
    if(!strcmp(entry->operation, new_module))
    {
      exists = TRUE;
      break;
    }

    l = g_list_next(l);
  }

  // the insert it if needed

  if(!exists)
  {
    l = iop_order_list;
    while(l)
    {
      dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)l->data;

      if(!strcmp(entry->operation, module))
      {
        dt_iop_order_entry_t *new_entry = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));

        strncpy(new_entry->operation, new_module, sizeof(new_entry->operation));
        new_entry->instance = 0;
        new_entry->o.iop_order = 0;

        iop_order_list = g_list_insert_before(iop_order_list, l, new_entry);
        break;
      }

      l = g_list_next(l);
    }
  }

  return iop_order_list;
}
#endif

static void* _ioppr_copy_entry(const void *entry, void *user_data)
{
  dt_iop_order_entry_t* copy = (dt_iop_order_entry_t*)malloc(sizeof(dt_iop_order_entry_t));
  memcpy(copy, entry, sizeof(dt_iop_order_entry_t));
  return (void *)copy;
}

GList *dt_ioppr_iop_order_list_duplicate(GList *iop_order_list)
{
  return g_list_copy_deep(iop_order_list, _ioppr_copy_entry, NULL);
}

dt_iop_order_t dt_ioppr_get_iop_order_version(const int32_t imgid)
{
  dt_iop_order_t iop_order_version = DT_IOP_ORDER_RECOMMENDED;

  // check current iop order version
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT version FROM main.module_order WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    iop_order_version = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);

  return iop_order_version;
}

// returns a list of dt_iop_order_rule_t
// this do not have versions
GList *dt_ioppr_get_iop_order_rules()
{
  GList *rules = NULL;

  const dt_iop_order_rule_t rule_entry[] = { { "rawprepare", "invert" },
                                                { "invert", "temperature" },
                                                { "temperature", "highlights" },
                                                { "highlights", "cacorrect" },
                                                { "cacorrect", "hotpixels" },
                                                { "hotpixels", "rawdenoise" },
                                                { "rawdenoise", "demosaic" },
                                                { "demosaic", "colorin" },
                                                { "colorin", "colorout" },
                                                { "colorout", "gamma" },
                                                /* clipping GUI broken if flip is done on top */
                                                { "flip", "clipping" },
                                                /* clipping GUI broken if ashift is done on top */
                                                { "ashift", "clipping" },
                                                { "\0", "\0" } };

  int i = 0;
  while(rule_entry[i].op_prev[0])
  {
    dt_iop_order_rule_t *rule = calloc(1, sizeof(dt_iop_order_rule_t));

    snprintf(rule->op_prev, sizeof(rule->op_prev), "%s", rule_entry[i].op_prev);
    snprintf(rule->op_next, sizeof(rule->op_next), "%s", rule_entry[i].op_next);

    rules = g_list_append(rules, rule);
    i++;
  }

  return rules;
}

GList *dt_ioppr_get_iop_order_link(GList *iop_order_list, const char *op_name, const int multi_priority)
{
  GList *link = NULL;

  GList *iops_order = g_list_first(iop_order_list);
  while(iops_order)
  {
    dt_iop_order_entry_t *order_entry = (dt_iop_order_entry_t *)iops_order->data;

    if(strcmp(order_entry->operation, op_name) == 0
       && (order_entry->instance == multi_priority || multi_priority == -1))
    {
      link = iops_order;
      break;
    }

    iops_order = g_list_next(iops_order);
  }

  return link;
}

// returns the first iop order entry that matches operation == op_name
dt_iop_order_entry_t *dt_ioppr_get_iop_order_entry(GList *iop_order_list, const char *op_name, const int multi_priority)
{
  const GList *link = dt_ioppr_get_iop_order_link(iop_order_list, op_name, multi_priority);
  if(link)
    return (dt_iop_order_entry_t *)link->data;
  else
    return NULL;
}

// returns the iop_order associated with the iop order entry that matches operation == op_name
int dt_ioppr_get_iop_order(GList *iop_order_list, const char *op_name, const int multi_priority)
{
  int iop_order = INT_MAX;
  const dt_iop_order_entry_t *order_entry = dt_ioppr_get_iop_order_entry(iop_order_list, op_name, multi_priority);

  if(order_entry)
  {
    iop_order = order_entry->o.iop_order;
  }
  else
    fprintf(stderr, "cannot get iop-order for %s instance %d\n", op_name, multi_priority);

  return iop_order;
}

gint dt_sort_iop_list_by_order(gconstpointer a, gconstpointer b)
{
  const dt_iop_order_entry_t *am = (const dt_iop_order_entry_t *)a;
  const dt_iop_order_entry_t *bm = (const dt_iop_order_entry_t *)b;
  if(am->o.iop_order > bm->o.iop_order) return 1;
  if(am->o.iop_order < bm->o.iop_order) return -1;
  return 0;
}

gint dt_sort_iop_list_by_order_f(gconstpointer a, gconstpointer b)
{
  const dt_iop_order_entry_t *am = (const dt_iop_order_entry_t *)a;
  const dt_iop_order_entry_t *bm = (const dt_iop_order_entry_t *)b;
  if(am->o.iop_order_f > bm->o.iop_order_f) return 1;
  if(am->o.iop_order_f < bm->o.iop_order_f) return -1;
  return 0;
}

dt_iop_order_t dt_ioppr_get_iop_order_list_kind(GList *iop_order_list)
{
  // first check if this is the recommended order
  int k = 0;
  GList *l = iop_order_list;
  gboolean ok = TRUE;
  while(l)
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)l->data;
    if(strcmp(recommended_order[k].operation, entry->operation)
       || recommended_order[k].instance != 0)
    {
      ok = FALSE;
      break;
    }
    k++;
    l = g_list_next(l);
  }

  if(ok) return DT_IOP_ORDER_RECOMMENDED;

  // then check if this is the legacy order
  k = 0;
  l = iop_order_list;
  ok = TRUE;
  while(l)
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)l->data;
    if(strcmp(legacy_order[k].operation, entry->operation)
       || legacy_order[k].instance != 0)
    {
      ok = FALSE;
      break;
    }
    k++;
    l = g_list_next(l);
  }

  if(ok) return DT_IOP_ORDER_LEGACY;

  return DT_IOP_ORDER_CUSTOM;
}

gboolean dt_ioppr_write_iop_order(const dt_iop_order_t kind, GList *iop_order_list, const int32_t imgid)
{
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT OR REPLACE INTO main.module_order VALUES (?1, 0, NULL)", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) != SQLITE_DONE) return FALSE;
  sqlite3_finalize(stmt);

  if(kind == DT_IOP_ORDER_CUSTOM)
  {
    gchar *iop_list_txt = dt_ioppr_serialize_text_iop_order_list(iop_order_list);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE main.module_order SET version = ?2, iop_list = ?3 WHERE imgid = ?1", -1,
                                &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, kind);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, iop_list_txt, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(stmt) != SQLITE_DONE) return FALSE;
    sqlite3_finalize(stmt);

    g_free(iop_list_txt);
  }
  else
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE main.module_order SET version = ?2, iop_list = NULL WHERE imgid = ?1", -1,
                                &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, kind);
    if(sqlite3_step(stmt) != SQLITE_DONE) return FALSE;
    sqlite3_finalize(stmt);
  }

  return TRUE;
}

gboolean dt_ioppr_write_iop_order_list(GList *iop_order_list, const int32_t imgid)
{
  const dt_iop_order_t kind = dt_ioppr_get_iop_order_list_kind(iop_order_list);
  return dt_ioppr_write_iop_order(kind, iop_order_list, imgid);
}

GList *_table_to_list(const dt_iop_order_entry_t entries[])
{
  GList *iop_order_list = NULL;
  int k = 0;
  while(entries[k].operation[0])
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));

    strncpy(entry->operation, entries[k].operation, sizeof(entry->operation) - 1);
    entry->instance = 0;
    entry->o.iop_order_f = entries[k].o.iop_order_f;
    iop_order_list = g_list_append(iop_order_list, entry);

    k++;
  }

  return iop_order_list;
}

GList *dt_ioppr_get_iop_order_list_version(dt_iop_order_t version)
{
  GList *iop_order_list = NULL;

  if(version == DT_IOP_ORDER_LEGACY)
  {
    iop_order_list = _table_to_list(legacy_order);
  }
  else if(version == DT_IOP_ORDER_RECOMMENDED)
  {
    iop_order_list = _table_to_list(recommended_order);
  }

  return iop_order_list;
}

GList *dt_ioppr_get_iop_order_list(int32_t imgid, gboolean sorted)
{
  GList *iop_order_list = NULL;

  if(imgid > 0)
  {
    sqlite3_stmt *stmt;

    // we read the iop-order-list in the preset table, the actual version is
    // the first int32_t serialized into the io_params. This is then a sequential
    // search, but there will not be many such presets and we do call this routine
    // only when loading an image and when changing the iop-order.

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT version, iop_list"
                                " FROM main.module_order"
                                " WHERE imgid=?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const dt_iop_order_t version = sqlite3_column_int(stmt, 0);

      if(version == DT_IOP_ORDER_CUSTOM)
      {
        const char *buf = (char *)sqlite3_column_text(stmt, 1);
        if(buf) iop_order_list = dt_ioppr_deserialize_text_iop_order_list(buf);

        if(!iop_order_list)
        {
          // preset not found, fall back to last built-in version, will be loaded below
          fprintf(stderr, "[dt_ioppr_get_iop_order_list] error building iop_order_list imgid %d\n", imgid);
        }
        else
        {
          // @@_NEW_MOUDLE: For new module it is required to insert the new module name in the iop-order list here.
          //                The insertion can be done depending on the current iop-order list kind.
#if 0
          _insert_before(iop_order_list, "<CURRENT_MODULE>", "<NEW_MODULE>");
#endif
        }
      }
      else if(version == DT_IOP_ORDER_LEGACY)
      {
        iop_order_list = _table_to_list(legacy_order);
      }
      else if(version == DT_IOP_ORDER_RECOMMENDED)
      {
        iop_order_list = _table_to_list(recommended_order);
      }
      else
        fprintf(stderr, "[dt_ioppr_get_iop_order_list] invalid iop order version %d for imgid %d\n", version, imgid);

      if(iop_order_list)
      {
        _ioppr_reset_iop_order(iop_order_list);
      }
    }

    sqlite3_finalize(stmt);
  }

  // fallback to last iop order list (also used to initialize the pipe when imgid = 0)
  // and new image not yet loaded or whose history has been reset.
  if(!iop_order_list)
  {
    iop_order_list = _table_to_list(recommended_order);
  }

  if(sorted) iop_order_list = g_list_sort(iop_order_list, dt_sort_iop_list_by_order);

  return iop_order_list;
}

static void _ioppr_reset_iop_order(GList *iop_order_list)
{
  int iop_order = 0;
  GList *l = iop_order_list;
  while(l)
  {
    dt_iop_order_entry_t *e = (dt_iop_order_entry_t *)l->data;
    e->o.iop_order = iop_order++;
    l = g_list_next(l);
  }
}

void dt_ioppr_resync_iop_list(dt_develop_t *dev)
{
  // make sure that the iop_order_list does not contains possibly removed modules

  GList *l = g_list_first(dev->iop_order_list);
  while(l)
  {
    dt_iop_order_entry_t *e = (dt_iop_order_entry_t *)l->data;
    dt_iop_module_t *mod = dt_iop_get_module_by_op_priority(dev->iop, e->operation, e->instance);
    if(mod == NULL)
    {
      dev->iop_order_list = g_list_remove_link(dev->iop_order_list, l);
    }

    l = g_list_next(l);
  }
}

void dt_ioppr_resync_modules_order(dt_develop_t *dev)
{
  _ioppr_reset_iop_order(dev->iop_order_list);

  // and reset all module iop_order

  GList *modules = g_list_first(dev->iop);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)(modules->data);

    mod->iop_order = dt_ioppr_get_iop_order(dev->iop_order_list, mod->op, mod->multi_priority);
    if(mod->iop_order == INT_MAX)
    {
      // module not found, probably removed instance, remote it
      dev->iop = g_list_delete_link(dev->iop, modules);
    }
    modules = g_list_next(modules);
  }

  dev->iop = g_list_sort(dev->iop, dt_sort_iop_by_order);
}

// sets the iop_order on each module of *_iop_list
// iop_order is set only for base modules, multi-instances will be flagged as unused with DBL_MAX
// if a module do not exists on iop_order_list it is flagged as unused with DBL_MAX
void dt_ioppr_set_default_iop_order(dt_develop_t *dev, const int32_t imgid)
{
  // get the iop-order for this image

  GList *iop_order_list = dt_ioppr_get_iop_order_list(imgid, FALSE);

  // we assign a single iop-order to each module

  _ioppr_reset_iop_order(iop_order_list);

  if(dev->iop_order_list) g_list_free_full(dev->iop_order_list, free);
  dev->iop_order_list = iop_order_list;

  // we now set the module list given to this iop-order

  dt_ioppr_resync_modules_order(dev);
}

void dt_ioppr_migrate_iop_order(struct dt_develop_t *dev, const int32_t imgid)
{
  dt_ioppr_set_default_iop_order(dev, imgid);
  dt_dev_reload_history_items(dev);
}

static void _count_iop_module(GList *iop, const char *operation, int *max_multi_priority, int *count,
                              int *max_multi_priority_enabled, int *count_enabled)
{
  *max_multi_priority = 0;
  *count = 0;
  *max_multi_priority_enabled = 0;
  *count_enabled = 0;

  GList *modules = iop;
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(!strcmp(mod->op, operation))
    {
      (*count)++;
      if(*max_multi_priority < mod->multi_priority) *max_multi_priority = mod->multi_priority;

      if(mod->enabled)
      {
        (*count_enabled)++;
        if(*max_multi_priority_enabled < mod->multi_priority) *max_multi_priority_enabled = mod->multi_priority;
      }
    }
    modules = g_list_next(modules);
  }

  assert(*count >= *count_enabled);
}

static int _count_entries_operation(GList *e_list, const char *operation)
{
  int count = 0;

  GList *l = g_list_first(e_list);
  while(l)
  {
    dt_iop_order_entry_t *ep = (dt_iop_order_entry_t *)l->data;
    if(!strcmp(ep->operation, operation)) count++;
    l = g_list_next(l);
  }

  return count;
}

static gboolean _operation_already_handled(GList *e_list, const char *operation)
{
  GList *l = g_list_previous(e_list);

  while(l)
  {
    dt_iop_order_entry_t *ep = (dt_iop_order_entry_t *)l->data;
    if(!strcmp(ep->operation, operation)) return TRUE;
    l = g_list_previous(l);
  }
  return FALSE;
}

// returns the nth module's priority being active or not
int _get_multi_priority(dt_develop_t *dev, const char *operation, const int n, const gboolean only_disabled)
{
  GList *l = dev->iop;
  int count = 0;
  while(l)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)l->data;
    if((!only_disabled || mod->enabled == FALSE) && !strcmp(mod->op, operation))
    {
      count++;
      if(count == n) return mod->multi_priority;
    }

    l = g_list_next(l);
  }

  return INT_MAX;
}

 void dt_ioppr_update_for_entries(dt_develop_t *dev, GList *entry_list, gboolean append)
{
  GList *e_list = entry_list;

  // for each priority list to be checked
  while(e_list)
  {
    dt_iop_order_entry_t *ep = (dt_iop_order_entry_t *)e_list->data;

    int max_multi_priority = 0, count = 0;
    int max_multi_priority_enabled = 0, count_enabled = 0;

    // is it a currently active module and if so how many active instances we have
    _count_iop_module(dev->iop, ep->operation,
                      &max_multi_priority, &count, &max_multi_priority_enabled, &count_enabled);

    // look for this operation into the target iop-order list and add there as much operation as needed

    GList *l = g_list_last(dev->iop_order_list);

    while(l)
    {
      dt_iop_order_entry_t *e = (dt_iop_order_entry_t *)l->data;
      if(!strcmp(e->operation, ep->operation) && !_operation_already_handled(e_list, ep->operation))
      {
        // how many instances of this module in the entry list, and re-number multi-priority accordingly
        const int new_active_instances = _count_entries_operation(entry_list, ep->operation);

        int add_count = 0;
        int start_multi_priority = 0;
        int nb_replace = 0;

        if(append)
        {
          nb_replace = count - count_enabled;
          add_count = MAX(0, new_active_instances - nb_replace);
          start_multi_priority = max_multi_priority + 1;
        }
        else
        {
          nb_replace = count;
          add_count = MAX(0, new_active_instances - count);
          start_multi_priority = max_multi_priority + 1;
        }

        // update multi_priority to be unique in iop list
        int multi_priority = start_multi_priority;
        int nb = 0;

        GList *s = entry_list;
        while(s)
        {
          dt_iop_order_entry_t *item = (dt_iop_order_entry_t *)s->data;
          if(!strcmp(item->operation, e->operation))
          {
            nb++;
            if(nb <= nb_replace)
            {
              // this one replaces current module, get it's multi-priority
              item->instance = _get_multi_priority(dev, item->operation, nb, append);
            }
            else
            {
              // otherwise create a new multi-priority
              item->instance = multi_priority++;
            }
          }
          s = g_list_next(s);
        }

        multi_priority = start_multi_priority;

        l = g_list_next(l);

        for(int k = 0; k<add_count; k++)
        {
          dt_iop_order_entry_t *n = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));
          strncpy(n->operation, ep->operation, sizeof(n->operation));
          n->instance = multi_priority++;
          n->o.iop_order = 0;
          dev->iop_order_list = g_list_insert_before(dev->iop_order_list, l, n);
        }
        break;
      }

      l = g_list_previous(l);
    }

    e_list = g_list_next(e_list);
  }

  _ioppr_reset_iop_order(dev->iop_order_list);

//  dt_ioppr_print_iop_order(dev->iop_order_list, "upd sitem");
}

void dt_ioppr_update_for_style_items(dt_develop_t *dev, GList *st_items, gboolean append)
{
  GList *si_list = g_list_first(st_items);
  GList *e_list = NULL;

  // for each priority list to be checked
  while(si_list)
  {
    dt_style_item_t *si = (dt_style_item_t *)si_list->data;

    dt_iop_order_entry_t *n = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));
    memcpy(n->operation, si->operation, sizeof(n->operation));
    n->instance = si->multi_priority;
    n->o.iop_order = 0;
    e_list = g_list_append(e_list, n);

    si_list = g_list_next(si_list);
  }

  dt_ioppr_update_for_entries(dev, e_list, append);

  // write back the multi-priority

  si_list = g_list_first(st_items);
  GList *el = g_list_first(e_list);
  while(si_list)
  {
    dt_style_item_t *si = (dt_style_item_t *)si_list->data;
    dt_iop_order_entry_t *e = (dt_iop_order_entry_t *)el->data;

    si->multi_priority = e->instance;
    si->iop_order = dt_ioppr_get_iop_order(dev->iop_order_list, si->operation, si->multi_priority);

    el = g_list_next(el);
    si_list = g_list_next(si_list);
  }

  g_list_free(e_list);
}

void dt_ioppr_update_for_modules(dt_develop_t *dev, GList *modules, gboolean append)
{
  GList *m_list = g_list_first(modules);
  GList *e_list = NULL;

  // for each priority list to be checked
  while(m_list)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)m_list->data;

    dt_iop_order_entry_t *n = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));
    strncpy(n->operation, mod->op, sizeof(n->operation));
    n->instance = mod->multi_priority;
    n->o.iop_order = 0;
    e_list = g_list_append(e_list, n);

    m_list = g_list_next(m_list);
  }

  dt_ioppr_update_for_entries(dev, e_list, append);

  // write back the multi-priority

  m_list = g_list_first(modules);
  GList *el = g_list_first(e_list);
  while(m_list)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)m_list->data;
    dt_iop_order_entry_t *e = (dt_iop_order_entry_t *)el->data;

    mod->multi_priority = e->instance;
    mod->iop_order = dt_ioppr_get_iop_order(dev->iop_order_list, mod->op, mod->multi_priority);

    el = g_list_next(el);
    m_list = g_list_next(m_list);
  }

  g_list_free_full(e_list, free);
}

// returns the first dt_dev_history_item_t on history_list where hist->module == mod
static dt_dev_history_item_t *_ioppr_search_history_by_module(GList *history_list, dt_iop_module_t *mod)
{
  dt_dev_history_item_t *hist_entry = NULL;

  GList *history = g_list_first(history_list);
  while(history)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);

    if(hist->module == mod)
    {
      hist_entry = hist;
      break;
    }

    history = g_list_next(history);
  }

  return hist_entry;
}

// check if there's duplicate iop_order entries in iop_list
// if so, updates the iop_order to be unique, but only if the module is disabled and not in history
void dt_ioppr_check_duplicate_iop_order(GList **_iop_list, GList *history_list)
{
  GList *iop_list = *_iop_list;
  dt_iop_module_t *mod_prev = NULL;

  // get the first module
  GList *modules = g_list_first(iop_list);
  if(modules)
  {
    mod_prev = (dt_iop_module_t *)(modules->data);
    modules = g_list_next(modules);
  }
  // check for each module if iop_order is the same as the previous one
  // if so, change it, but only if disabled and not in history
  while(modules)
  {
    int reset_list = 0;
    dt_iop_module_t *mod = (dt_iop_module_t *)(modules->data);

    if(mod->iop_order == mod_prev->iop_order && mod->iop_order != DBL_MAX)
    {
      int can_move = 0;

      if(!mod->enabled && _ioppr_search_history_by_module(history_list, mod) == NULL)
      {
        can_move = 1;

        GList *modules1 = g_list_next(modules);
        if(modules1)
        {
          dt_iop_module_t *mod_next = (dt_iop_module_t *)(modules1->data);
          if(mod->iop_order != mod_next->iop_order)
          {
            mod->iop_order += (mod_next->iop_order - mod->iop_order) / 2.0;
          }
          else
          {
            dt_ioppr_check_duplicate_iop_order(&modules, history_list);
            reset_list = 1;
          }
        }
        else
        {
          mod->iop_order += 1.0;
        }
      }
      else if(!mod_prev->enabled && _ioppr_search_history_by_module(history_list, mod_prev) == NULL)
      {
        can_move = 1;

        GList *modules1 = g_list_previous(modules);
        if(modules1) modules1 = g_list_previous(modules1);
        if(modules1)
        {
          dt_iop_module_t *mod_next = (dt_iop_module_t *)(modules1->data);
          if(mod_prev->iop_order != mod_next->iop_order)
          {
            mod_prev->iop_order -= (mod_prev->iop_order - mod_next->iop_order) / 2.0;
          }
          else
          {
            can_move = 0;
            fprintf(stderr,
                    "[dt_ioppr_check_duplicate_iop_order 1] modules %s %s(%d) and %s %s(%d) have the same iop_order\n",
                    mod_prev->op, mod_prev->multi_name, mod_prev->iop_order, mod->op, mod->multi_name, mod->iop_order);
          }
        }
        else
        {
          mod_prev->iop_order -= 0.5;
        }
      }

      if(!can_move)
      {
        fprintf(stderr,
                "[dt_ioppr_check_duplicate_iop_order] modules %s %s(%d) and %s %s(%d) have the same iop_order\n",
                mod_prev->op, mod_prev->multi_name, mod_prev->iop_order, mod->op, mod->multi_name, mod->iop_order);
      }
    }

    if(reset_list)
    {
      modules = g_list_first(iop_list);
      if(modules)
      {
        mod_prev = (dt_iop_module_t *)(modules->data);
        modules = g_list_next(modules);
      }
    }
    else
    {
      mod_prev = mod;
      modules = g_list_next(modules);
    }
  }

  *_iop_list = iop_list;
}

// check if all so modules on iop_list have a iop_order defined in iop_order_list
int dt_ioppr_check_so_iop_order(GList *iop_list, GList *iop_order_list)
{
  int iop_order_missing = 0;

  // check if all the modules have their iop_order assigned
  GList *modules = g_list_first(iop_list);
  while(modules)
  {
    dt_iop_module_so_t *mod = (dt_iop_module_so_t *)(modules->data);

    dt_iop_order_entry_t *entry = dt_ioppr_get_iop_order_entry(iop_order_list, mod->op, 0); // mod->multi_priority);
    if(entry == NULL)
    {
      iop_order_missing = 1;
      fprintf(stderr, "[dt_ioppr_check_so_iop_order] missing iop_order for module %s\n", mod->op);
    }
    modules = g_list_next(modules);
  }

  return iop_order_missing;
}

static void *_dup_iop_order_entry(const void *src, gpointer data)
{
  dt_iop_order_entry_t *scr_entry = (dt_iop_order_entry_t *)src;
  dt_iop_order_entry_t *new_entry = malloc(sizeof(dt_iop_order_entry_t));
  memcpy(new_entry, scr_entry, sizeof(dt_iop_order_entry_t));
  return (void *)new_entry;
}

// returns a duplicate of iop_order_list
GList *dt_ioppr_iop_order_copy_deep(GList *iop_order_list)
{
  return (GList *)g_list_copy_deep(iop_order_list, _dup_iop_order_entry, NULL);
}

// helper to sort a GList of dt_iop_module_t by iop_order
gint dt_sort_iop_by_order(gconstpointer a, gconstpointer b)
{
  const dt_iop_module_t *am = (const dt_iop_module_t *)a;
  const dt_iop_module_t *bm = (const dt_iop_module_t *)b;
  if(am->iop_order > bm->iop_order) return 1;
  if(am->iop_order < bm->iop_order) return -1;
  return 0;
}

// if module can be placed before than module_next on the pipe
// it returns the new iop_order
// if it cannot be placed it returns -1.0
// this assumes that the order is always positive
gboolean dt_ioppr_check_can_move_before_iop(GList *iop_list, dt_iop_module_t *module, dt_iop_module_t *module_next)
{
  if(module->flags() & IOP_FLAGS_FENCE)
  {
    return FALSE;
  }

  gboolean can_move = FALSE;

  // module is before on the pipe
  // move it up
  if(module->iop_order < module_next->iop_order)
  {
    // let's first search for module
    GList *modules = g_list_first(iop_list);
    while(modules)
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
      if(mod == module) break;
      modules = g_list_next(modules);
    }

    // we found the module
    if(modules)
    {
      dt_iop_module_t *mod1 = NULL;
      dt_iop_module_t *mod2 = NULL;

      // now search for module_next and the one previous to that, so iop_order can be calculated
      // also check the rules
      modules = g_list_next(modules);
      while(modules)
      {
        dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;

        // if we reach module_next everything is OK
        if(mod == module_next)
        {
          mod2 = mod;
          break;
        }

        // check if module can be moved around this one
        if(mod->flags() & IOP_FLAGS_FENCE)
        {
          break;
        }

        // is there a rule about swapping this two?
        int rule_found = 0;
        GList *rules = g_list_first(darktable.iop_order_rules);
        while(rules)
        {
          dt_iop_order_rule_t *rule = (dt_iop_order_rule_t *)rules->data;

          if(strcmp(module->op, rule->op_prev) == 0 && strcmp(mod->op, rule->op_next) == 0)
          {
            rule_found = 1;
            break;
          }

          rules = g_list_next(rules);
        }
        if(rule_found) break;

        mod1 = mod;
        modules = g_list_next(modules);
      }

      // we reach the module_next module
      if(mod2)
      {
        // this is already the previous module!
        if(module == mod1)
        {
          ;
        }
        else if(mod1->iop_order == mod2->iop_order)
        {
          fprintf(stderr, "[dt_ioppr_get_iop_order_before_iop] %s %s(%d) and %s %s(%d) have the same iop_order\n",
              mod1->op, mod1->multi_name, mod1->iop_order, mod2->op, mod2->multi_name, mod2->iop_order);
        }
        else
        {
          can_move = TRUE;
        }
      }
    }
    else
      fprintf(stderr, "[dt_ioppr_get_iop_order_before_iop] can't find module %s %s\n", module->op, module->multi_name);
  }
  // module is next on the pipe
  // move it down
  else if(module->iop_order > module_next->iop_order)
  {
    // let's first search for module
    GList *modules = g_list_last(iop_list);
    while(modules)
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
      if(mod == module) break;
      modules = g_list_previous(modules);
    }

    // we found the module
    if(modules)
    {
      dt_iop_module_t *mod1 = NULL;
      dt_iop_module_t *mod2 = NULL;

      // now search for module_next and the one next to that, so iop_order can be calculated
      // also check the rules
      modules = g_list_previous(modules);
      while(modules)
      {
        dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;

        // we reach the module next to module_next, everything is OK
        if(mod2 != NULL)
        {
          mod1 = mod;
          break;
        }

        // check for rules
        // check if module can be moved around this one
        if(mod->flags() & IOP_FLAGS_FENCE)
        {
          break;
        }

        // is there a rule about swapping this two?
        int rule_found = 0;
        GList *rules = g_list_first(darktable.iop_order_rules);
        while(rules)
        {
          dt_iop_order_rule_t *rule = (dt_iop_order_rule_t *)rules->data;

          if(strcmp(mod->op, rule->op_prev) == 0 && strcmp(module->op, rule->op_next) == 0)
          {
            rule_found = 1;
            break;
          }

          rules = g_list_next(rules);
        }
        if(rule_found) break;

        if(mod == module_next) mod2 = mod;
        modules = g_list_previous(modules);
      }

      // we reach the module_next module
      if(mod1)
      {
        // this is already the previous module!
        if(module == mod2)
        {
          ;
        }
        else if(mod1->iop_order == mod2->iop_order)
        {
          fprintf(stderr, "[dt_ioppr_get_iop_order_before_iop] %s %s(%d) and %s %s(%d) have the same iop_order\n",
              mod1->op, mod1->multi_name, mod1->iop_order, mod2->op, mod2->multi_name, mod2->iop_order);
        }
        else
        {
          can_move = TRUE;
        }
      }
    }
    else
      fprintf(stderr, "[dt_ioppr_get_iop_order_before_iop] can't find module %s %s\n", module->op, module->multi_name);
  }
  else
  {
    fprintf(stderr, "[dt_ioppr_get_iop_order_before_iop] modules %s %s(%d) and %s %s(%d) have the same iop_order\n",
        module->op, module->multi_name, module->iop_order, module_next->op, module_next->multi_name, module_next->iop_order);
  }

  return can_move;
}

// if module can be placed after than module_prev on the pipe
// it returns the new iop_order
// if it cannot be placed it returns -1.0
// this assumes that the order is always positive
gboolean dt_ioppr_check_can_move_after_iop(GList *iop_list, dt_iop_module_t *module, dt_iop_module_t *module_prev)
{
  gboolean can_move = FALSE;

  // moving after module_prev is the same as moving before the very next one after module_prev
  GList *modules = g_list_last(iop_list);
  dt_iop_module_t *module_next = NULL;
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module_prev) break;

    module_next = mod;
    modules = g_list_previous(modules);
  }
  if(module_next == NULL)
  {
    fprintf(
        stderr,
        "[dt_ioppr_get_iop_order_after_iop] can't find module previous to %s %s(%d) while moving %s %s(%d) after it\n",
        module_prev->op, module_prev->multi_name, module_prev->iop_order, module->op, module->multi_name,
        module->iop_order);
  }
  else
    can_move = dt_ioppr_check_can_move_before_iop(iop_list, module, module_next);

  return can_move;
}

// changes the module->iop_order so it comes before in the pipe than module_next
// sort dev->iop to reflect the changes
// return 1 if iop_order is changed, 0 otherwise
int dt_ioppr_move_iop_before(struct dt_develop_t *dev, dt_iop_module_t *module, dt_iop_module_t *module_next)
{
  GList *next = dt_ioppr_get_iop_order_link(dev->iop_order_list, module_next->op, module_next->multi_priority);
  GList *current = dt_ioppr_get_iop_order_link(dev->iop_order_list, module->op, module->multi_priority);

  if(!next || !current) return 0;

  dev->iop_order_list = g_list_remove_link(dev->iop_order_list, current);
  dev->iop_order_list = g_list_insert_before(dev->iop_order_list, next, current->data);

  g_list_free(current);

  dt_ioppr_resync_modules_order(dev);

  return 1;
}

// changes the module->iop_order so it comes after in the pipe than module_prev
// sort dev->iop to reflect the changes
// return 1 if iop_order is changed, 0 otherwise
int dt_ioppr_move_iop_after(struct dt_develop_t *dev, dt_iop_module_t *module, dt_iop_module_t *module_prev)
{
  GList *prev = dt_ioppr_get_iop_order_link(dev->iop_order_list, module_prev->op, module_prev->multi_priority);
  GList *current = dt_ioppr_get_iop_order_link(dev->iop_order_list, module->op, module->multi_priority);

  if(!prev || !current) return 0;

  dev->iop_order_list = g_list_remove_link(dev->iop_order_list, current);

  // we want insert after => so insert before the next item
  GList *next = g_list_next(prev);
  if(prev)
    dev->iop_order_list = g_list_insert_before(dev->iop_order_list, next, current->data);
  else
    dev->iop_order_list = g_list_append(dev->iop_order_list, current->data);

  g_list_free(current);

  dt_ioppr_resync_modules_order(dev);

  return 1;
}

//--------------------------------------------------------------------
// from here just for debug
//--------------------------------------------------------------------

void dt_ioppr_print_module_iop_order(GList *iop_list, const char *msg)
{
  GList *modules = g_list_first(iop_list);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)(modules->data);

    fprintf(stderr, "[%s] module %s %s multi_priority=%i, iop_order=%d\n",
            msg, mod->op, mod->multi_name, mod->multi_priority, mod->iop_order);

    modules = g_list_next(modules);
  }
}

void dt_ioppr_print_history_iop_order(GList *history_list, const char *msg)
{
  GList *history = g_list_first(history_list);
  while(history)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);

    fprintf(stderr, "[%s] module %s %s multi_priority=%i, iop_order=%d\n",
            msg, hist->op_name, hist->multi_name, hist->multi_priority, hist->iop_order);

    history = g_list_next(history);
  }
}

void dt_ioppr_print_iop_order(GList *iop_order_list, const char *msg)
{
  GList *iops_order = g_list_first(iop_order_list);
  while(iops_order)
  {
    dt_iop_order_entry_t *order_entry = (dt_iop_order_entry_t *)(iops_order->data);

    fprintf(stderr, "[%s] op %20s (inst %d) iop_order=%d\n",
            msg, order_entry->operation, order_entry->instance, order_entry->o.iop_order);

    iops_order = g_list_next(iops_order);
  }
}

static GList *_get_fence_modules_list(GList *iop_list)
{
  GList *fences = NULL;
  GList *modules = g_list_first(iop_list);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;

    if(mod->flags() & IOP_FLAGS_FENCE)
    {
      fences = g_list_append(fences, mod);
    }

    modules = g_list_next(modules);
  }
  return fences;
}

static void _ioppr_check_rules(GList *iop_list, const int imgid, const char *msg)
{
  GList *modules = NULL;

  // check for IOP_FLAGS_FENCE on each module
  // create a list of fences modules
  GList *fences = _get_fence_modules_list(iop_list);

  // check if each module is between the fences
  modules = g_list_first(iop_list);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod->iop_order == DBL_MAX)
    {
      modules = g_list_next(modules);
      continue;
    }

    dt_iop_module_t *fence_prev = NULL;
    dt_iop_module_t *fence_next = NULL;

    GList *mod_fences = g_list_first(fences);
    while(mod_fences)
    {
      dt_iop_module_t *mod_fence = (dt_iop_module_t *)mod_fences->data;

      // mod should be before this fence
      if(mod->iop_order < mod_fence->iop_order)
      {
        if(fence_next == NULL)
          fence_next = mod_fence;
        else if(mod_fence->iop_order < fence_next->iop_order)
          fence_next = mod_fence;
      }
      // mod should be after this fence
      else if(mod->iop_order > mod_fence->iop_order)
      {
        if(fence_prev == NULL)
          fence_prev = mod_fence;
        else if(mod_fence->iop_order > fence_prev->iop_order)
          fence_prev = mod_fence;
      }

      mod_fences = g_list_next(mod_fences);
    }

    // now check if mod is between the fences
    if(fence_next && mod->iop_order > fence_next->iop_order)
    {
      fprintf(stderr, "[_ioppr_check_rules] found fence %s %s module %s %s(%d) is after %s %s(%d) image %i (%s)\n",
              fence_next->op, fence_next->multi_name, mod->op, mod->multi_name, mod->iop_order, fence_next->op,
              fence_next->multi_name, fence_next->iop_order, imgid, msg);
    }
    if(fence_prev && mod->iop_order < fence_prev->iop_order)
    {
      fprintf(stderr, "[_ioppr_check_rules] found fence %s %s module %s %s(%d) is before %s %s(%d) image %i (%s)\n",
              fence_prev->op, fence_prev->multi_name, mod->op, mod->multi_name, mod->iop_order, fence_prev->op,
              fence_prev->multi_name, fence_prev->iop_order, imgid, msg);
    }


    modules = g_list_next(modules);
  }

  // for each module check if it doesn't break a rule
  modules = g_list_first(iop_list);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod->iop_order == DBL_MAX)
    {
      modules = g_list_next(modules);
      continue;
    }

    // we have a module, now check each rule
    GList *rules = g_list_first(darktable.iop_order_rules);
    while(rules)
    {
      dt_iop_order_rule_t *rule = (dt_iop_order_rule_t *)rules->data;

      // mod must be before rule->op_next
      if(strcmp(mod->op, rule->op_prev) == 0)
      {
        // check if there's a rule->op_next module before mod
        GList *modules_prev = g_list_previous(modules);
        while(modules_prev)
        {
          dt_iop_module_t *mod_prev = (dt_iop_module_t *)modules_prev->data;

          if(strcmp(mod_prev->op, rule->op_next) == 0)
          {
            fprintf(stderr, "[_ioppr_check_rules] found rule %s %s module %s %s(%d) is after %s %s(%d) image %i (%s)\n",
                    rule->op_prev, rule->op_next, mod->op, mod->multi_name, mod->iop_order, mod_prev->op,
                    mod_prev->multi_name, mod_prev->iop_order, imgid, msg);
          }

          modules_prev = g_list_previous(modules_prev);
        }
      }
      // mod must be after rule->op_prev
      else if(strcmp(mod->op, rule->op_next) == 0)
      {
        // check if there's a rule->op_prev module after mod
        GList *modules_next = g_list_next(modules);
        while(modules_next)
        {
          dt_iop_module_t *mod_next = (dt_iop_module_t *)modules_next->data;

          if(strcmp(mod_next->op, rule->op_prev) == 0)
          {
            fprintf(stderr, "[_ioppr_check_rules] found rule %s %s module %s %s(%d) is before %s %s(%d) image %i (%s)\n",
                    rule->op_prev, rule->op_next, mod->op, mod->multi_name, mod->iop_order, mod_next->op,
                    mod_next->multi_name, mod_next->iop_order, imgid, msg);
          }

          modules_next = g_list_next(modules_next);
        }
      }

      rules = g_list_next(rules);
    }

    modules = g_list_next(modules);
  }

  if(fences) g_list_free(fences);
}

void dt_ioppr_insert_module_instance(struct dt_develop_t *dev, dt_iop_module_t *module)
{
  const char *operation = module->op;
  const int32_t instance = module->multi_priority;

  dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));

  strncpy(entry->operation, operation, sizeof(entry->operation));
  entry->instance = instance;
  entry->o.iop_order = 0;

  GList *l = dev->iop_order_list;
  GList *place = NULL;

  int max_instance = -1;

  while(l)
  {
    dt_iop_order_entry_t *e = (dt_iop_order_entry_t *)l->data;
    if(!strcmp(e->operation, operation) && e->instance > max_instance)
    {
      place = l;
      max_instance = e->instance;
    }
    l = g_list_next(l);
  }

  dev->iop_order_list = g_list_insert_before(dev->iop_order_list, place, entry);
}

int dt_ioppr_check_iop_order(dt_develop_t *dev, const int imgid, const char *msg)
{
  int iop_order_ok = 1;

  // check if gamma is the last iop
  {
    GList *modules = g_list_last(dev->iop);
    while(modules)
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
      if(mod->iop_order != INT_MAX)
        break;

      modules = g_list_previous(dev->iop);
    }
    if(modules)
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;

      if(strcmp(mod->op, "gamma") != 0)
      {
        iop_order_ok = 0;
        fprintf(stderr, "[dt_ioppr_check_iop_order] gamma is not the last iop, last is %s %s(%d) image %i (%s)\n",
                mod->op, mod->multi_name, mod->iop_order,imgid, msg);
      }
    }
    else
    {
      // fprintf(stderr, "[dt_ioppr_check_iop_order] dev->iop is empty image %i (%s)\n",imgid, msg);
    }
  }

  // some other checks
  {
    GList *modules = g_list_last(dev->iop);
    while(modules)
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
      if(!mod->default_enabled && mod->iop_order != INT_MAX)
      {
        if(mod->enabled)
        {
          iop_order_ok = 0;
          fprintf(stderr, "[dt_ioppr_check_iop_order] module not used but enabled!! %s %s(%d) image %i (%s)\n",
                  mod->op, mod->multi_name, mod->iop_order,imgid, msg);
        }
        if(mod->multi_priority == 0)
        {
          iop_order_ok = 0;
          fprintf(stderr, "[dt_ioppr_check_iop_order] base module set as not used %s %s(%d) image %i (%s)\n",
                  mod->op, mod->multi_name, mod->iop_order,imgid, msg);
        }
      }

      modules = g_list_previous(dev->iop);
    }
  }

  // check if there's duplicate or out-of-order iop_order
  {
    dt_iop_module_t *mod_prev = NULL;
    GList *modules = g_list_first(dev->iop);
    while(modules)
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
      if(mod->iop_order != INT_MAX)
      {
        if(mod_prev)
        {
          if(mod->iop_order < mod_prev->iop_order)
          {
            iop_order_ok = 0;
            fprintf(stderr,
                    "[dt_ioppr_check_iop_order] module %s %s(%d) should be after %s %s(%d) image %i (%s)\n",
                    mod->op, mod->multi_name, mod->iop_order, mod_prev->op, mod_prev->multi_name,
                    mod_prev->iop_order, imgid, msg);
          }
          else if(mod->iop_order == mod_prev->iop_order)
          {
            iop_order_ok = 0;
            fprintf(
                stderr,
                "[dt_ioppr_check_iop_order] module %s %s(%i)(%d) and %s %s(%i)(%d) have the same order image %i (%s)\n",
                mod->op, mod->multi_name, mod->multi_priority, mod->iop_order, mod_prev->op,
                mod_prev->multi_name, mod_prev->multi_priority, mod_prev->iop_order, imgid, msg);
          }
        }
      }
      mod_prev = mod;
      modules = g_list_next(modules);
    }
  }

  _ioppr_check_rules(dev->iop, imgid, msg);

  GList *history = g_list_first(dev->history);
  while(history)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);

    if(hist->iop_order == INT_MAX)
    {
      if(hist->enabled)
      {
        iop_order_ok = 0;
        fprintf(stderr, "[dt_ioppr_check_iop_order] history module not used but enabled!! %s %s(%d) image %i (%s)\n",
            hist->op_name, hist->multi_name, hist->iop_order,imgid, msg);
      }
      if(hist->multi_priority == 0)
      {
        iop_order_ok = 0;
        fprintf(stderr, "[dt_ioppr_check_iop_order] history base module set as not used %s %s(%d) image %i (%s)\n",
            hist->op_name, hist->multi_name, hist->iop_order,imgid, msg);
      }
    }

    history = g_list_next(history);
  }

  return iop_order_ok;
}

void *dt_ioppr_serialize_iop_order_list(GList *iop_order_list, size_t *size)
{
  // compute size of all modules
  *size = 0;

  GList *l = iop_order_list;
  while(l)
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)l->data;
    *size += strlen(entry->operation) + sizeof(int32_t) * 2;
    l = g_list_next(l);
  }

  // allocate the parameter buffer
  char *params = (char *)malloc(*size);

  // set set preset iop-order version
  int pos = 0;

  l = iop_order_list;
  while(l)
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)l->data;
    // write the len of the module name
    const int32_t len = strlen(entry->operation);
    memcpy(params+pos, &len, sizeof(int32_t));
    pos += sizeof(int32_t);

    // write the module name
    memcpy(params+pos, entry->operation, len);
    pos += len;

    // write the instance number
    memcpy(params+pos, &(entry->instance), sizeof(int32_t));
    pos += sizeof(int32_t);

    l = g_list_next(l);
  }

  return params;
}

char *dt_ioppr_serialize_text_iop_order_list(GList *iop_order_list)
{
  gchar *text = g_strdup("");

  GList *l = iop_order_list;
  while(l)
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)l->data;
    gchar buf[64];
    snprintf(buf, sizeof(buf), "%s,%d,", entry->operation, entry->instance);
    text = g_strconcat(text, buf, NULL);
    l = g_list_next(l);
  }

  return text;
}

GList *dt_ioppr_deserialize_text_iop_order_list(const char *buf)
{
  GList *iop_order_list = NULL;
  const int len = strlen(buf);
  int pos = 0;
  int start = 0;

  while(buf[pos] && pos < len)
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));

    entry->o.iop_order = 0;

    // set module name
    start = pos;
    while(buf[pos] != ',' && pos < len) pos++;
    const int opname_len = pos-start;
    if(opname_len > 20) { free(entry); goto error; }
    memcpy(entry->operation, &buf[start], opname_len);
    entry->operation[opname_len] = '\0';

    pos++;
    start = pos;
    int inst;
    sscanf(&buf[start], "%d", &inst);
    entry->instance = inst;

    if(entry->instance < 0 || entry->instance > 1000) { free(entry); goto error; }

    while(buf[pos] != ',' && pos < len) pos++;
    pos++;

    // append to the list
    iop_order_list = g_list_append(iop_order_list, entry);
  }

  _ioppr_reset_iop_order(iop_order_list);

  return iop_order_list;

 error:
  g_list_free_full(iop_order_list, free);
  return NULL;
}

GList *dt_ioppr_deserialize_iop_order_list(const char *buf, size_t size)
{
  GList *iop_order_list = NULL;

  // parse all modules
  while(size)
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));

    entry->o.iop_order = 0;

    // get length of module name
    const int32_t len = *(int32_t *)buf;
    buf += sizeof(int32_t);

    if(len < 0 || len > 20) { free(entry); goto error; }

    // set module name
    memcpy(entry->operation, buf, len);
    *(entry->operation + len) = '\0';
    buf += len;

    // get the instance number
    entry->instance = *(int32_t *)buf;
    buf += sizeof(int32_t);

    if(entry->instance < 0 || entry->instance > 1000) { free(entry); goto error; }

    // append to the list
    iop_order_list = g_list_append(iop_order_list, entry);

    size -= (2 * sizeof(int32_t) + len);
  }

  _ioppr_reset_iop_order(iop_order_list);

  return iop_order_list;

 error:
  g_list_free_full(iop_order_list, free);
  return NULL;
}

//---------------------------------------------------------
// colorspace transforms
//---------------------------------------------------------

static void _transform_from_to_rgb_lab_lcms2(const float *const image_in, float *const image_out, const int width,
                                             const int height, const dt_colorspaces_color_profile_type_t type,
                                             const char *filename, const int intent, const int direction)
{
  const int ch = 4;
  cmsHTRANSFORM *xform = NULL;
  cmsHPROFILE *rgb_profile = NULL;
  cmsHPROFILE *lab_profile = NULL;

  if(type != DT_COLORSPACE_NONE)
  {
    const dt_colorspaces_color_profile_t *profile = dt_colorspaces_get_profile(type, filename, DT_PROFILE_DIRECTION_WORK);
    if(profile) rgb_profile = profile->profile;
  }
  else
    rgb_profile = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC2020, "", DT_PROFILE_DIRECTION_WORK)->profile;
  if(rgb_profile)
  {
    cmsColorSpaceSignature rgb_color_space = cmsGetColorSpace(rgb_profile);
    if(rgb_color_space != cmsSigRgbData)
    {
        fprintf(stderr, "working profile color space `%c%c%c%c' not supported\n",
                (char)(rgb_color_space>>24),
                (char)(rgb_color_space>>16),
                (char)(rgb_color_space>>8),
                (char)(rgb_color_space));
        rgb_profile = NULL;
    }
  }
  if(rgb_profile == NULL)
  {
    rgb_profile = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC2020, "", DT_PROFILE_DIRECTION_WORK)->profile;
    fprintf(stderr, _("unsupported working profile %s has been replaced by Rec2020 RGB!\n"), filename);
  }

  lab_profile = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;

  cmsHPROFILE *input_profile = NULL;
  cmsHPROFILE *output_profile = NULL;
  cmsUInt32Number input_format = TYPE_RGBA_FLT;
  cmsUInt32Number output_format = TYPE_LabA_FLT;

  if(direction == 1) // rgb --> lab
  {
    input_profile = rgb_profile;
    input_format = TYPE_RGBA_FLT;
    output_profile = lab_profile;
    output_format = TYPE_LabA_FLT;
  }
  else // lab -->rgb
  {
    input_profile = lab_profile;
    input_format = TYPE_LabA_FLT;
    output_profile = rgb_profile;
    output_format = TYPE_RGBA_FLT;
  }

  xform = cmsCreateTransform(input_profile, input_format, output_profile, output_format, intent, 0);
  if(xform)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(image_in, image_out, width, height, ch) \
    shared(xform) \
    schedule(static)
#endif
    for(int y = 0; y < height; y++)
    {
      const float *const in = image_in + y * width * ch;
      float *const out = image_out + y * width * ch;

      cmsDoTransform(xform, in, out, width);
    }
  }
  else
    fprintf(stderr, "[_transform_from_to_rgb_lab_lcms2] cannot create transform\n");

  if(xform) cmsDeleteTransform(xform);
}

static void _transform_rgb_to_rgb_lcms2(const float *const image_in, float *const image_out, const int width,
                                        const int height, const dt_colorspaces_color_profile_type_t type_from,
                                        const char *filename_from,
                                        const dt_colorspaces_color_profile_type_t type_to, const char *filename_to,
                                        const int intent)
{
  const int ch = 4;
  cmsHTRANSFORM *xform = NULL;
  cmsHPROFILE *from_rgb_profile = NULL;
  cmsHPROFILE *to_rgb_profile = NULL;

  if(type_from == DT_COLORSPACE_DISPLAY || type_to == DT_COLORSPACE_DISPLAY || type_from == DT_COLORSPACE_DISPLAY2
     || type_to == DT_COLORSPACE_DISPLAY2)
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);

  if(type_from != DT_COLORSPACE_NONE)
  {
    const dt_colorspaces_color_profile_t *profile_from
        = dt_colorspaces_get_profile(type_from, filename_from, DT_PROFILE_DIRECTION_ANY);
    if(profile_from) from_rgb_profile = profile_from->profile;
  }
  else
  {
    fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] invalid from profile\n");
  }

  if(type_to != DT_COLORSPACE_NONE)
  {
    const dt_colorspaces_color_profile_t *profile_to
        = dt_colorspaces_get_profile(type_to, filename_to, DT_PROFILE_DIRECTION_ANY);
    if(profile_to) to_rgb_profile = profile_to->profile;
  }
  else
  {
    fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] invalid to profile\n");
  }

  if(from_rgb_profile)
  {
    cmsColorSpaceSignature rgb_color_space = cmsGetColorSpace(from_rgb_profile);
    if(rgb_color_space != cmsSigRgbData)
    {
      fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] profile color space `%c%c%c%c' not supported\n",
              (char)(rgb_color_space >> 24), (char)(rgb_color_space >> 16), (char)(rgb_color_space >> 8),
              (char)(rgb_color_space));
      from_rgb_profile = NULL;
    }
  }
  if(to_rgb_profile)
  {
    cmsColorSpaceSignature rgb_color_space = cmsGetColorSpace(to_rgb_profile);
    if(rgb_color_space != cmsSigRgbData)
    {
      fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] profile color space `%c%c%c%c' not supported\n",
              (char)(rgb_color_space >> 24), (char)(rgb_color_space >> 16), (char)(rgb_color_space >> 8),
              (char)(rgb_color_space));
      to_rgb_profile = NULL;
    }
  }

  cmsHPROFILE *input_profile = NULL;
  cmsHPROFILE *output_profile = NULL;
  cmsUInt32Number input_format = TYPE_RGBA_FLT;
  cmsUInt32Number output_format = TYPE_RGBA_FLT;

  input_profile = from_rgb_profile;
  input_format = TYPE_RGBA_FLT;
  output_profile = to_rgb_profile;
  output_format = TYPE_RGBA_FLT;

  if(input_profile && output_profile)
    xform = cmsCreateTransform(input_profile, input_format, output_profile, output_format, intent, 0);

  if(type_from == DT_COLORSPACE_DISPLAY || type_to == DT_COLORSPACE_DISPLAY || type_from == DT_COLORSPACE_DISPLAY2
     || type_to == DT_COLORSPACE_DISPLAY2)
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

  if(xform)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(image_in, image_out, width, height, ch) \
    shared(xform) \
    schedule(static)
#endif
    for(int y = 0; y < height; y++)
    {
      const float *const in = image_in + y * width * ch;
      float *const out = image_out + y * width * ch;

      cmsDoTransform(xform, in, out, width);
    }
  }
  else
    fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] cannot create transform\n");

  if(xform) cmsDeleteTransform(xform);
}

static void _transform_lcms2(struct dt_iop_module_t *self, const float *const image_in, float *const image_out,
                             const int width, const int height, const int cst_from, const int cst_to,
                             int *converted_cst, const dt_iop_order_iccprofile_info_t *const profile_info)
{
  if(cst_from == cst_to)
  {
    *converted_cst = cst_to;
    return;
  }

  *converted_cst = cst_to;

  if(cst_from == iop_cs_rgb && cst_to == iop_cs_Lab)
  {
    printf("[_transform_lcms2] transfoming from RGB to Lab (%s %s)\n", self->op, self->multi_name);
    _transform_from_to_rgb_lab_lcms2(image_in, image_out, width, height, profile_info->type,
                                     profile_info->filename, profile_info->intent, 1);
  }
  else if(cst_from == iop_cs_Lab && cst_to == iop_cs_rgb)
  {
    printf("[_transform_lcms2] transfoming from Lab to RGB (%s %s)\n", self->op, self->multi_name);
    _transform_from_to_rgb_lab_lcms2(image_in, image_out, width, height, profile_info->type,
                                     profile_info->filename, profile_info->intent, -1);
  }
  else
  {
    *converted_cst = cst_from;
    fprintf(stderr, "[_transform_lcms2] invalid conversion from %i to %i\n", cst_from, cst_to);
  }
}

static inline void _transform_lcms2_rgb(const float *const image_in, float *const image_out, const int width,
                                        const int height,
                                        const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                        const dt_iop_order_iccprofile_info_t *const profile_info_to)
{
  _transform_rgb_to_rgb_lcms2(image_in, image_out, width, height, profile_info_from->type,
                              profile_info_from->filename, profile_info_to->type, profile_info_to->filename,
                              profile_info_to->intent);
}


static inline int _init_unbounded_coeffs(float *const lutr, float *const lutg, float *const lutb,
    float *const unbounded_coeffsr, float *const unbounded_coeffsg, float *const unbounded_coeffsb, const int lutsize)
{
  int nonlinearlut = 0;
  float *lut[3] = { lutr, lutg, lutb };
  float *unbounded_coeffs[3] = { unbounded_coeffsr, unbounded_coeffsg, unbounded_coeffsb };

  for(int k = 0; k < 3; k++)
  {
    // omit luts marked as linear (negative as marker)
    if(lut[k][0] >= 0.0f)
    {
      const float x[4] DT_ALIGNED_PIXEL = { 0.7f, 0.8f, 0.9f, 1.0f };
      const float y[4] DT_ALIGNED_PIXEL = { extrapolate_lut(lut[k], x[0], lutsize), extrapolate_lut(lut[k], x[1], lutsize), extrapolate_lut(lut[k], x[2], lutsize),
                                            extrapolate_lut(lut[k], x[3], lutsize) };
      dt_iop_estimate_exp(x, y, 4, unbounded_coeffs[k]);

      nonlinearlut++;
    }
    else
      unbounded_coeffs[k][0] = -1.0f;
  }

  return nonlinearlut;
}


static inline void _apply_tonecurves(const float *const image_in, float *const image_out,
                                     const int width, const int height,
                                     const float *const restrict lutr,
                                     const float *const restrict lutg,
                                     const float *const restrict lutb,
                                     const float *const restrict unbounded_coeffsr,
                                     const float *const restrict unbounded_coeffsg,
                                     const float *const restrict unbounded_coeffsb,
                                     const int lutsize)
{
  const int ch = 4;
  const float *const lut[3] = { lutr, lutg, lutb };
  const float *const unbounded_coeffs[3] = { unbounded_coeffsr, unbounded_coeffsg, unbounded_coeffsb };
  const size_t stride = (size_t)ch * width * height;

  // do we have any lut to apply, or is this a linear profile?
  if((lut[0][0] >= 0.0f) && (lut[1][0] >= 0.0f) && (lut[2][0] >= 0.0f))
  {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(stride, image_in, image_out, lut, lutsize, unbounded_coeffs, ch) \
    schedule(static) collapse(2) aligned(image_in, image_out:64)
#endif
    for(size_t k = 0; k < stride; k += ch)
    {
      for(int c = 0; c < 3; c++)
      {
        image_out[k + c] = (image_in[k + c] < 1.0f) ? extrapolate_lut(lut[c], image_in[k + c], lutsize)
                                                    : eval_exp(unbounded_coeffs[c], image_in[k + c]);
      }
    }
  }
  else if((lut[0][0] >= 0.0f) || (lut[1][0] >= 0.0f) || (lut[2][0] >= 0.0f))
  {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(stride, image_in, image_out, lut, lutsize, unbounded_coeffs, ch) \
    schedule(static) collapse(2) aligned(image_in, image_out:64)
#endif
    for(size_t k = 0; k < stride; k += ch)
    {
      for(int c = 0; c < 3; c++)
      {
        if(lut[c][0] >= 0.0f)
        {
          image_out[k + c] = (image_in[k + c] < 1.0f) ? extrapolate_lut(lut[c], image_in[k + c], lutsize)
                                                      : eval_exp(unbounded_coeffs[c], image_in[k + c]);
        }
      }
    }
  }
}


static inline void _transform_rgb_to_lab_matrix(const float *const restrict image_in, float *const restrict image_out,
                                                const int width, const int height,
                                                const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const int ch = 4;
  const size_t stride = (size_t)width * height * ch;
  const float *const restrict matrix = profile_info->matrix_in;

  if(profile_info->nonlinearlut)
  {
    _apply_tonecurves(image_in, image_out, width, height, profile_info->lut_in[0], profile_info->lut_in[1],
                      profile_info->lut_in[2], profile_info->unbounded_coeffs_in[0],
                      profile_info->unbounded_coeffs_in[1], profile_info->unbounded_coeffs_in[2],
                      profile_info->lutsize);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(image_out, profile_info, stride, ch, matrix) \
    schedule(static) aligned(image_out:64) aligned(matrix:16)
#endif
    for(size_t y = 0; y < stride; y += ch)
    {
      float *const in = image_out + y;
      float xyz[3] DT_ALIGNED_PIXEL = { 0.0f, 0.0f, 0.0f };
      _ioppr_linear_rgb_matrix_to_xyz(in, xyz, matrix);
      dt_XYZ_to_Lab(xyz, in);
    }
  }
  else
  {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(image_in, image_out, profile_info, stride, ch, matrix) \
    schedule(static) aligned(image_in, image_out:64) aligned(matrix:16)
#endif
    for(size_t y = 0; y < stride; y += ch)
    {
      const float *const in = image_in + y ;
      float *const out = image_out + y;

      float xyz[3] DT_ALIGNED_PIXEL = { 0.0f, 0.0f, 0.0f };

      _ioppr_linear_rgb_matrix_to_xyz(in, xyz, matrix);
      dt_XYZ_to_Lab(xyz, out);
    }
  }
}


static inline void _transform_lab_to_rgb_matrix(const float *const restrict image_in, float *const restrict image_out, const int width,
                                         const int height,
                                         const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const int ch = 4;
  const size_t stride = (size_t)width * height * ch;
  const float *const restrict matrix = profile_info->matrix_out;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(image_in, image_out, stride, profile_info, ch, matrix) \
  schedule(static) aligned(image_in, image_out:64) aligned(matrix:16)
#endif
  for(size_t y = 0; y < stride; y += ch)
  {
    const float *const in = image_in + y;
    float *const out = image_out + y;

    float xyz[3] DT_ALIGNED_PIXEL = { 0.0f, 0.0f, 0.0f };

    dt_Lab_to_XYZ(in, xyz);
    _ioppr_xyz_to_linear_rgb_matrix(xyz, out, matrix);
  }

  if(profile_info->nonlinearlut)
  {
    _apply_tonecurves(image_out, image_out, width, height, profile_info->lut_out[0], profile_info->lut_out[1],
                      profile_info->lut_out[2], profile_info->unbounded_coeffs_out[0],
                      profile_info->unbounded_coeffs_out[1], profile_info->unbounded_coeffs_out[2],
                      profile_info->lutsize);
  }
}


static inline void _transform_matrix_rgb(const float *const restrict image_in, float *const restrict image_out, const int width,
                                  const int height, const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                  const dt_iop_order_iccprofile_info_t *const profile_info_to)
{
  const int ch = 4;
  const size_t stride = (size_t)width * height * ch;
  const float *const restrict matrix_in = profile_info_from->matrix_in;
  const float *const restrict matrix_out = profile_info_to->matrix_out;

  if(profile_info_from->nonlinearlut)
  {
    _apply_tonecurves(image_in, image_out, width, height, profile_info_from->lut_in[0],
                      profile_info_from->lut_in[1], profile_info_from->lut_in[2],
                      profile_info_from->unbounded_coeffs_in[0], profile_info_from->unbounded_coeffs_in[1],
                      profile_info_from->unbounded_coeffs_in[2], profile_info_from->lutsize);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(stride, image_out, profile_info_from, profile_info_to, ch, matrix_in, matrix_out) \
    schedule(static) aligned(image_out:64) aligned(matrix_in, matrix_out:16)
#endif
    for(size_t y = 0; y < stride; y += ch)
    {
      float *const in = image_out + y;

      float xyz[3] DT_ALIGNED_PIXEL = { 0.0f, 0.0f, 0.0f };

      _ioppr_linear_rgb_matrix_to_xyz(in, xyz, matrix_in);
      _ioppr_xyz_to_linear_rgb_matrix(xyz, in, matrix_out);
    }
  }
  else
  {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(stride, image_in, image_out, profile_info_from, profile_info_to, ch, matrix_in, matrix_out) \
    schedule(static) aligned(image_in, image_out:64) aligned(matrix_in, matrix_out:16)
#endif
    for(size_t y = 0; y < stride; y += ch)
    {
      const float *const in = image_in + y;
      float *const out = image_out + y;

      float xyz[3] DT_ALIGNED_PIXEL = { 0.0f, 0.0f, 0.0f };

      _ioppr_linear_rgb_matrix_to_xyz(in, xyz, matrix_in);
      _ioppr_xyz_to_linear_rgb_matrix(xyz, out, matrix_out);
    }
  }

  if(profile_info_to->nonlinearlut)
  {
    _apply_tonecurves(image_out, image_out, width, height, profile_info_to->lut_out[0], profile_info_to->lut_out[1],
                      profile_info_to->lut_out[2], profile_info_to->unbounded_coeffs_out[0],
                      profile_info_to->unbounded_coeffs_out[1], profile_info_to->unbounded_coeffs_out[2],
                      profile_info_to->lutsize);
  }
}


static inline void _transform_matrix(struct dt_iop_module_t *self, const float *const restrict image_in, float *const restrict image_out,
                              const int width, const int height, const int cst_from, const int cst_to,
                              int *converted_cst, const dt_iop_order_iccprofile_info_t *const profile_info)
{
  if(cst_from == cst_to)
  {
    *converted_cst = cst_to;
    return;
  }

  *converted_cst = cst_to;

  if(cst_from == iop_cs_rgb && cst_to == iop_cs_Lab)
  {
    _transform_rgb_to_lab_matrix(image_in, image_out, width, height, profile_info);
  }
  else if(cst_from == iop_cs_Lab && cst_to == iop_cs_rgb)
  {
    _transform_lab_to_rgb_matrix(image_in, image_out, width, height, profile_info);
  }
  else
  {
    *converted_cst = cst_from;
    fprintf(stderr, "[_transform_matrix] invalid conversion from %i to %i\n", cst_from, cst_to);
  }
}


#define DT_IOPPR_LUT_SAMPLES 0x10000

void dt_ioppr_init_profile_info(dt_iop_order_iccprofile_info_t *profile_info, const int lutsize)
{
  profile_info->type = DT_COLORSPACE_NONE;
  profile_info->filename[0] = '\0';
  profile_info->intent = DT_INTENT_PERCEPTUAL;
  profile_info->matrix_in[0] = NAN;
  profile_info->matrix_out[0] = NAN;
  profile_info->unbounded_coeffs_in[0][0] = profile_info->unbounded_coeffs_in[1][0] = profile_info->unbounded_coeffs_in[2][0] = -1.0f;
  profile_info->unbounded_coeffs_out[0][0] = profile_info->unbounded_coeffs_out[1][0] = profile_info->unbounded_coeffs_out[2][0] = -1.0f;
  profile_info->nonlinearlut = 0;
  profile_info->grey = 0.f;
  profile_info->lutsize = (lutsize > 0) ? lutsize: DT_IOPPR_LUT_SAMPLES;
  for(int i = 0; i < 3; i++)
  {
    profile_info->lut_in[i] = dt_alloc_sse_ps(profile_info->lutsize);
    profile_info->lut_in[i][0] = -1.0f;
    profile_info->lut_out[i] = dt_alloc_sse_ps(profile_info->lutsize);
    profile_info->lut_out[i][0] = -1.0f;
  }
}

#undef DT_IOPPR_LUT_SAMPLES

void dt_ioppr_cleanup_profile_info(dt_iop_order_iccprofile_info_t *profile_info)
{
  for(int i = 0; i < 3; i++)
  {
    if(profile_info->lut_in[i]) dt_free_align(profile_info->lut_in[i]);
    if(profile_info->lut_out[i]) dt_free_align(profile_info->lut_out[i]);
  }
}

/** generate the info for the profile (type, filename) if matrix can be retrieved from lcms2
 * it can be called multiple time between init and cleanup
 * return 0 if OK, non zero otherwise
 */
static int dt_ioppr_generate_profile_info(dt_iop_order_iccprofile_info_t *profile_info, const int type, const char *filename, const int intent)
{
  int err_code = 0;
  cmsHPROFILE *rgb_profile = NULL;

  profile_info->matrix_in[0] = NAN;
  profile_info->matrix_out[0] = NAN;
  for(int i = 0; i < 3; i++)
  {
    profile_info->lut_in[i][0] = -1.0f;
    profile_info->lut_out[i][0] = -1.0f;
  }

  profile_info->nonlinearlut = 0;
  profile_info->grey = 0.1842f;

  profile_info->type = type;
  g_strlcpy(profile_info->filename, filename, sizeof(profile_info->filename));
  profile_info->intent = intent;

  if(type == DT_COLORSPACE_DISPLAY || type == DT_COLORSPACE_DISPLAY2)
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);

  const dt_colorspaces_color_profile_t *profile
      = dt_colorspaces_get_profile(type, filename, DT_PROFILE_DIRECTION_ANY);
  if(profile) rgb_profile = profile->profile;

  if(type == DT_COLORSPACE_DISPLAY || type == DT_COLORSPACE_DISPLAY2)
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

  // we only allow rgb profiles
  if(rgb_profile)
  {
    cmsColorSpaceSignature rgb_color_space = cmsGetColorSpace(rgb_profile);
    if(rgb_color_space != cmsSigRgbData)
    {
      fprintf(stderr, "working profile color space `%c%c%c%c' not supported\n",
              (char)(rgb_color_space>>24),
              (char)(rgb_color_space>>16),
              (char)(rgb_color_space>>8),
              (char)(rgb_color_space));
      rgb_profile = NULL;
    }
  }

  // get the matrix
  if(rgb_profile)
  {
    if(dt_colorspaces_get_matrix_from_input_profile(rgb_profile, profile_info->matrix_in,
        profile_info->lut_in[0], profile_info->lut_in[1], profile_info->lut_in[2],
        profile_info->lutsize, profile_info->intent) ||
        dt_colorspaces_get_matrix_from_output_profile(rgb_profile, profile_info->matrix_out,
            profile_info->lut_out[0], profile_info->lut_out[1], profile_info->lut_out[2],
            profile_info->lutsize, profile_info->intent))
    {
      profile_info->matrix_in[0] = NAN;
      profile_info->matrix_out[0] = NAN;
      for(int i = 0; i < 3; i++)
      {
        profile_info->lut_in[i][0] = -1.0f;
        profile_info->lut_out[i][0] = -1.0f;
      }
    }
    else if(isnan(profile_info->matrix_in[0]) || isnan(profile_info->matrix_out[0]))
    {
      profile_info->matrix_in[0] = NAN;
      profile_info->matrix_out[0] = NAN;
      for(int i = 0; i < 3; i++)
      {
        profile_info->lut_in[i][0] = -1.0f;
        profile_info->lut_out[i][0] = -1.0f;
      }
    }
  }

  // now try to initialize unbounded mode:
  // we do extrapolation for input values above 1.0f.
  // unfortunately we can only do this if we got the computation
  // in our hands, i.e. for the fast builtin-dt-matrix-profile path.
  if(!isnan(profile_info->matrix_in[0]) && !isnan(profile_info->matrix_out[0]))
  {
    profile_info->nonlinearlut = _init_unbounded_coeffs(profile_info->lut_in[0], profile_info->lut_in[1], profile_info->lut_in[2],
        profile_info->unbounded_coeffs_in[0], profile_info->unbounded_coeffs_in[1], profile_info->unbounded_coeffs_in[2], profile_info->lutsize);
    _init_unbounded_coeffs(profile_info->lut_out[0], profile_info->lut_out[1], profile_info->lut_out[2],
        profile_info->unbounded_coeffs_out[0], profile_info->unbounded_coeffs_out[1], profile_info->unbounded_coeffs_out[2], profile_info->lutsize);
  }

  if(!isnan(profile_info->matrix_in[0]) && !isnan(profile_info->matrix_out[0]) && profile_info->nonlinearlut)
  {
    const float rgb[3] = { 0.1842f, 0.1842f, 0.1842f };
    profile_info->grey = dt_ioppr_get_rgb_matrix_luminance(rgb, profile_info->matrix_in, profile_info->lut_in, profile_info->unbounded_coeffs_in, profile_info->lutsize, profile_info->nonlinearlut);
  }

  return err_code;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_profile_info_from_list(struct dt_develop_t *dev, const int profile_type, const char *profile_filename)
{
  dt_iop_order_iccprofile_info_t *profile_info = NULL;

  GList *profiles = g_list_first(dev->allprofile_info);
  while(profiles)
  {
    dt_iop_order_iccprofile_info_t *prof = (dt_iop_order_iccprofile_info_t *)(profiles->data);
    if(prof->type == profile_type && strcmp(prof->filename, profile_filename) == 0)
    {
      profile_info = prof;
      break;
    }
    profiles = g_list_next(profiles);
  }

  return profile_info;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_add_profile_info_to_list(struct dt_develop_t *dev, const int profile_type, const char *profile_filename, const int intent)
{
  dt_iop_order_iccprofile_info_t *profile_info = dt_ioppr_get_profile_info_from_list(dev, profile_type, profile_filename);
  if(profile_info == NULL)
  {
    profile_info = malloc(sizeof(dt_iop_order_iccprofile_info_t));
    dt_ioppr_init_profile_info(profile_info, 0);
    const int err = dt_ioppr_generate_profile_info(profile_info, profile_type, profile_filename, intent);
    if(err == 0)
    {
      dev->allprofile_info = g_list_append(dev->allprofile_info, profile_info);
    }
    else
    {
      free(profile_info);
      profile_info = NULL;
    }
  }
  return profile_info;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_iop_work_profile_info(struct dt_iop_module_t *module, GList *iop_list)
{
  dt_iop_order_iccprofile_info_t *profile = NULL;

  // first check if the module is between colorin and colorout
  gboolean in_between = FALSE;

  GList *modules = g_list_first(iop_list);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)(modules->data);

    // we reach the module, that's it
    if(strcmp(mod->op, module->op) == 0) break;

    // if we reach colorout means that the module is after it
    if(strcmp(mod->op, "colorout") == 0)
    {
      in_between = FALSE;
      break;
    }

    // we reach colorin, so far we're good
    if(strcmp(mod->op, "colorin") == 0)
    {
      in_between = TRUE;
      break;
    }

    modules = g_list_next(modules);
  }

  if(in_between)
  {
    dt_colorspaces_color_profile_type_t type = DT_COLORSPACE_NONE;
    char *filename = NULL;
    dt_develop_t *dev = module->dev;

    dt_ioppr_get_work_profile_type(dev, &type, &filename);
    if(filename) profile = dt_ioppr_add_profile_info_to_list(dev, type, filename, DT_INTENT_PERCEPTUAL);
  }

  return profile;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_set_pipe_work_profile_info(struct dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe,
    const int type, const char *filename, const int intent)
{
  dt_iop_order_iccprofile_info_t *profile_info = dt_ioppr_add_profile_info_to_list(dev, type, filename, intent);

  if(profile_info == NULL || isnan(profile_info->matrix_in[0]) || isnan(profile_info->matrix_out[0]))
  {
    fprintf(stderr, "[dt_ioppr_set_pipe_work_profile_info] unsupported working profile %i %s, it will be replaced with linear rec2020\n", type, filename);
    profile_info = dt_ioppr_add_profile_info_to_list(dev, DT_COLORSPACE_LIN_REC2020, "", intent);
  }
  pipe->dsc.work_profile_info = profile_info;

  return profile_info;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_histogram_profile_info(struct dt_develop_t *dev)
{
  dt_colorspaces_color_profile_type_t histogram_profile_type;
  char *histogram_profile_filename;
  dt_ioppr_get_histogram_profile_type(&histogram_profile_type, &histogram_profile_filename);
  return dt_ioppr_add_profile_info_to_list(dev, histogram_profile_type, histogram_profile_filename,
                                           DT_INTENT_PERCEPTUAL);
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_work_profile_info(struct dt_dev_pixelpipe_t *pipe)
{
  return pipe->dsc.work_profile_info;
}

// returns a pointer to the filename of the work profile instead of the actual string data
// pointer must not be stored
void dt_ioppr_get_work_profile_type(struct dt_develop_t *dev, int *profile_type, char **profile_filename)
{
  *profile_type = DT_COLORSPACE_NONE;
  *profile_filename = NULL;

  // use introspection to get the params values
  dt_iop_module_so_t *colorin_so = NULL;
  dt_iop_module_t *colorin = NULL;
  GList *modules = g_list_first(darktable.iop);
  while(modules)
  {
    dt_iop_module_so_t *module_so = (dt_iop_module_so_t *)(modules->data);
    if(!strcmp(module_so->op, "colorin"))
    {
      colorin_so = module_so;
      break;
    }
    modules = g_list_next(modules);
  }
  if(colorin_so && colorin_so->get_p)
  {
    modules = g_list_first(dev->iop);
    while(modules)
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      if(!strcmp(module->op, "colorin"))
      {
        colorin = module;
        break;
      }
      modules = g_list_next(modules);
    }
  }
  if(colorin)
  {
    dt_colorspaces_color_profile_type_t *_type = colorin_so->get_p(colorin->params, "type_work");
    char *_filename = colorin_so->get_p(colorin->params, "filename_work");
    if(_type && _filename)
    {
      *profile_type = *_type;
      *profile_filename = _filename;
    }
    else
      fprintf(stderr, "[dt_ioppr_get_work_profile_type] can't get colorin parameters\n");
  }
  else
    fprintf(stderr, "[dt_ioppr_get_work_profile_type] can't find colorin iop\n");
}

void dt_ioppr_get_export_profile_type(struct dt_develop_t *dev, int *profile_type, char **profile_filename)
{
  *profile_type = DT_COLORSPACE_NONE;
  *profile_filename = NULL;

  // use introspection to get the params values
  dt_iop_module_so_t *colorout_so = NULL;
  dt_iop_module_t *colorout = NULL;
  GList *modules = g_list_last(darktable.iop);
  while(modules)
  {
    dt_iop_module_so_t *module_so = (dt_iop_module_so_t *)(modules->data);
    if(!strcmp(module_so->op, "colorout"))
    {
      colorout_so = module_so;
      break;
    }
    modules = g_list_previous(modules);
  }
  if(colorout_so && colorout_so->get_p)
  {
    modules = g_list_last(dev->iop);
    while(modules)
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      if(!strcmp(module->op, "colorout"))
      {
        colorout = module;
        break;
      }
      modules = g_list_previous(modules);
    }
  }
  if(colorout)
  {
    dt_colorspaces_color_profile_type_t *_type = colorout_so->get_p(colorout->params, "type");
    char *_filename = colorout_so->get_p(colorout->params, "filename");
    if(_type && _filename)
    {
      *profile_type = *_type;
      *profile_filename = _filename;
    }
    else
      fprintf(stderr, "[dt_ioppr_get_export_profile_type] can't get colorout parameters\n");
  }
  else
    fprintf(stderr, "[dt_ioppr_get_export_profile_type] can't find colorout iop\n");
}

void dt_ioppr_get_histogram_profile_type(int *profile_type, char **profile_filename)
{
  const dt_colorspaces_color_mode_t mode = darktable.color_profiles->mode;

  // if in gamut check use soft proof
  if(mode != DT_PROFILE_NORMAL || darktable.color_profiles->histogram_type == DT_COLORSPACE_SOFTPROOF)
  {
    *profile_type = darktable.color_profiles->softproof_type;
    *profile_filename = darktable.color_profiles->softproof_filename;
  }
  else if(darktable.color_profiles->histogram_type == DT_COLORSPACE_WORK)
  {
    dt_ioppr_get_work_profile_type(darktable.develop, profile_type, profile_filename);
  }
  else if(darktable.color_profiles->histogram_type == DT_COLORSPACE_EXPORT)
  {
    dt_ioppr_get_export_profile_type(darktable.develop, profile_type, profile_filename);
  }
  else
  {
    *profile_type = darktable.color_profiles->histogram_type;
    *profile_filename = darktable.color_profiles->histogram_filename;
  }
}


#if defined(__SSE2__x) // FIXME: this is slower than the C version
static __m128 _ioppr_linear_rgb_matrix_to_xyz_sse(const __m128 rgb, const dt_iop_order_iccprofile_info_t *const profile_info)
{
/*  for(int c = 0; c < 3; c++)
  {
    xyz[c] = 0.0f;
    for(int i = 0; i < 3; i++)
    {
      xyz[c] += profile_info->matrix_in[3 * c + i] * rgb[i];
    }
  }*/
  const __m128 m0 = _mm_set_ps(0.0f, profile_info->matrix_in[6], profile_info->matrix_in[3], profile_info->matrix_in[0]);
  const __m128 m1 = _mm_set_ps(0.0f, profile_info->matrix_in[7], profile_info->matrix_in[4], profile_info->matrix_in[1]);
  const __m128 m2 = _mm_set_ps(0.0f, profile_info->matrix_in[8], profile_info->matrix_in[5], profile_info->matrix_in[2]);

  __m128 xyz
      = _mm_add_ps(_mm_mul_ps(m0, _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(0, 0, 0, 0))),
                   _mm_add_ps(_mm_mul_ps(m1, _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(1, 1, 1, 1))),
                              _mm_mul_ps(m2, _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(2, 2, 2, 2)))));
  return xyz;
}

static __m128 _ioppr_xyz_to_linear_rgb_matrix_sse(const __m128 xyz, const dt_iop_order_iccprofile_info_t *const profile_info)
{
/*  for(int c = 0; c < 3; c++)
  {
    rgb[c] = 0.0f;
    for(int i = 0; i < 3; i++)
    {
      rgb[c] += profile_info->matrix_out[3 * c + i] * xyz[i];
    }
  }*/
  const __m128 m0 = _mm_set_ps(0.0f, profile_info->matrix_out[6], profile_info->matrix_out[3], profile_info->matrix_out[0]);
  const __m128 m1 = _mm_set_ps(0.0f, profile_info->matrix_out[7], profile_info->matrix_out[4], profile_info->matrix_out[1]);
  const __m128 m2 = _mm_set_ps(0.0f, profile_info->matrix_out[8], profile_info->matrix_out[5], profile_info->matrix_out[2]);

  __m128 rgb
      = _mm_add_ps(_mm_mul_ps(m0, _mm_shuffle_ps(xyz, xyz, _MM_SHUFFLE(0, 0, 0, 0))),
                   _mm_add_ps(_mm_mul_ps(m1, _mm_shuffle_ps(xyz, xyz, _MM_SHUFFLE(1, 1, 1, 1))),
                              _mm_mul_ps(m2, _mm_shuffle_ps(xyz, xyz, _MM_SHUFFLE(2, 2, 2, 2)))));
  return rgb;
}

static void _transform_rgb_to_lab_matrix_sse(float *const image, const int width, const int height, const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const int ch = 4;
  const size_t stride = (size_t)width * height;

  _apply_tonecurves(image, width, height, profile_info->lut_in[0], profile_info->lut_in[1], profile_info->lut_in[2],
      profile_info->unbounded_coeffs_in[0], profile_info->unbounded_coeffs_in[1], profile_info->unbounded_coeffs_in[2], profile_info->lutsize);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(size_t y = 0; y < stride; y++)
  {
    float *const in = image + y * ch;

    __m128 xyz = { 0.0f };
    __m128 rgb = _mm_load_ps(in);

    xyz = _ioppr_linear_rgb_matrix_to_xyz_sse(rgb, profile_info);

    rgb = dt_XYZ_to_Lab_sse2(xyz);
    const float a = in[3];
    _mm_stream_ps(in, rgb);
    in[3] = a;
  }
}

static void _transform_lab_to_rgb_matrix_sse(float *const image, const int width, const int height, const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const int ch = 4;
  const size_t stride = (size_t)width * height;

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(size_t y = 0; y < stride; y++)
  {
    float *const in = image + y * ch;

    __m128 xyz = { 0.0f };
    __m128 lab = _mm_load_ps(in);

    xyz = dt_Lab_to_XYZ_sse2(lab);
    lab = _ioppr_xyz_to_linear_rgb_matrix_sse(xyz, profile_info);
    const float a = in[3];
    _mm_stream_ps(in, lab);
    in[3] = a;
  }

  _apply_tonecurves(image, width, height, profile_info->lut_out[0], profile_info->lut_out[1], profile_info->lut_out[2],
      profile_info->unbounded_coeffs_out[0], profile_info->unbounded_coeffs_out[1], profile_info->unbounded_coeffs_out[2], profile_info->lutsize);
}

// FIXME: this is slower than the C version
static void _transform_matrix_sse(struct dt_iop_module_t *self, float *const image, const int width, const int height,
    const int cst_from, const int cst_to, int *converted_cst, const dt_iop_order_iccprofile_info_t *const profile_info)
{
  if(cst_from == cst_to)
  {
    *converted_cst = cst_to;
    return;
  }

  *converted_cst = cst_to;

  if(cst_from == iop_cs_rgb && cst_to == iop_cs_Lab)
  {
    _transform_rgb_to_lab_matrix_sse(image, width, height, profile_info);
  }
  else if(cst_from == iop_cs_Lab && cst_to == iop_cs_rgb)
  {
    _transform_lab_to_rgb_matrix_sse(image, width, height, profile_info);
  }
  else
  {
    *converted_cst = cst_from;
    fprintf(stderr, "[_transform_matrix_sse] invalid conversion from %i to %i\n", cst_from, cst_to);
  }
}

static void _transform_matrix_rgb_sse(float *const image, const int width, const int height,
                                      const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                      const dt_iop_order_iccprofile_info_t *const profile_info_to)
{
  const int ch = 4;
  const size_t stride = (size_t)width * height;

  _apply_tonecurves(image, width, height, profile_info_from->lut_in[0], profile_info_from->lut_in[1],
                    profile_info_from->lut_in[2], profile_info_from->unbounded_coeffs_in[0],
                    profile_info_from->unbounded_coeffs_in[1], profile_info_from->unbounded_coeffs_in[2],
                    profile_info_from->lutsize);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(size_t y = 0; y < stride; y++)
  {
    float *const in = image + y * ch;

    __m128 xyz = { 0.0f };
    __m128 rgb = _mm_load_ps(in);

    xyz = _ioppr_linear_rgb_matrix_to_xyz_sse(rgb, profile_info_from);
    rgb = _ioppr_xyz_to_linear_rgb_matrix_sse(xyz, profile_info_to);

    const float a = in[3];
    _mm_stream_ps(in, rgb);
    in[3] = a;
  }

  _apply_tonecurves(image, width, height, profile_info_to->lut_out[0], profile_info_to->lut_out[1],
                    profile_info_to->lut_out[2], profile_info_to->unbounded_coeffs_out[0],
                    profile_info_to->unbounded_coeffs_out[1], profile_info_to->unbounded_coeffs_out[2],
                    profile_info_to->lutsize);
}
#endif

void dt_ioppr_transform_image_colorspace(struct dt_iop_module_t *self, const float *const image_in,
                                         float *const image_out, const int width, const int height,
                                         const int cst_from, const int cst_to, int *converted_cst,
                                         const dt_iop_order_iccprofile_info_t *const profile_info)
{
  if(cst_from == cst_to)
  {
    *converted_cst = cst_to;
    return;
  }
  if(profile_info == NULL)
  {
    // The range information below is not informative in case it's the colorin module itself.
    if (strcmp(self->op,"colorin"))
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace] module %s must be between input color profile and output color profile\n", self->op);
    *converted_cst = cst_from;
    return;
  }
  if(profile_info->type == DT_COLORSPACE_NONE)
  {
    *converted_cst = cst_from;
    return;
  }

  dt_times_t start_time = { 0 }, end_time = { 0 };
  if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

  // matrix should be never NAN, this is only to test it against lcms2!
  if(!isnan(profile_info->matrix_in[0]) && !isnan(profile_info->matrix_out[0]))
  {
    // FIXME: sse is slower than the C version
    // if(darktable.codepath.OPENMP_SIMD)
    _transform_matrix(self, image_in, image_out, width, height, cst_from, cst_to, converted_cst, profile_info);
    /*
    #if defined(__SSE2__)
        else if(darktable.codepath.SSE2)
          _transform_matrix_sse(self, image, width, height, cst_from, cst_to, converted_cst, profile_info);
    #endif
        else
          dt_unreachable_codepath();
    */
    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform %s-->%s took %.3f secs (%.3f CPU) [%s %s]\n",
          (cst_from == iop_cs_rgb) ? "RGB": "Lab", (cst_to == iop_cs_rgb) ? "RGB": "Lab",
          end_time.clock - start_time.clock, end_time.user - start_time.user, self->op, self->multi_name);
    }
  }
  else
  {
    _transform_lcms2(self, image_in, image_out, width, height, cst_from, cst_to, converted_cst, profile_info);

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform %s-->%s took %.3f secs (%.3f lcms2) [%s %s]\n",
          (cst_from == iop_cs_rgb) ? "RGB": "Lab", (cst_to == iop_cs_rgb) ? "RGB": "Lab",
          end_time.clock - start_time.clock, end_time.user - start_time.user, self->op, self->multi_name);
    }
  }

  if(*converted_cst == cst_from)
    fprintf(stderr, "[dt_ioppr_transform_image_colorspace] invalid conversion from %i to %i\n", cst_from, cst_to);
}

void dt_ioppr_transform_image_colorspace_rgb(const float *const restrict image_in, float *const restrict image_out, const int width,
                                             const int height,
                                             const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                             const dt_iop_order_iccprofile_info_t *const profile_info_to,
                                             const char *message)
{
  if(profile_info_from->type == DT_COLORSPACE_NONE || profile_info_to->type == DT_COLORSPACE_NONE)
  {
    return;
  }
  if(profile_info_from->type == profile_info_to->type
     && strcmp(profile_info_from->filename, profile_info_to->filename) == 0)
  {
    if(image_in != image_out)
      memcpy(image_out, image_in, width * height * 4 * sizeof(float));

    return;
  }

  dt_times_t start_time = { 0 }, end_time = { 0 };
  if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

  if(!isnan(profile_info_from->matrix_in[0]) && !isnan(profile_info_from->matrix_out[0])
     && !isnan(profile_info_to->matrix_in[0]) && !isnan(profile_info_to->matrix_out[0]))
  {
    // FIXME: sse is slower than the C version
    // if(darktable.codepath.OPENMP_SIMD)
    _transform_matrix_rgb(image_in, image_out, width, height, profile_info_from, profile_info_to);
    /*
    #if defined(__SSE2__)
        else if(darktable.codepath.SSE2)
          _transform_matrix_rgb_sse(self, image, width, height, profile_info_from, profile_info_to);
    #endif
        else
          dt_unreachable_codepath();
    */
    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform RGB-->RGB took %.3f secs (%.3f CPU) [%s]\n",
              end_time.clock - start_time.clock, end_time.user - start_time.user, (message) ? message : "");
    }
  }
  else
  {
    _transform_lcms2_rgb(image_in, image_out, width, height, profile_info_from, profile_info_to);

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform RGB-->RGB took %.3f secs (%.3f lcms2) [%s]\n",
              end_time.clock - start_time.clock, end_time.user - start_time.user, (message) ? message : "");
    }
  }
}

#ifdef HAVE_OPENCL
dt_colorspaces_cl_global_t *dt_colorspaces_init_cl_global()
{
  dt_colorspaces_cl_global_t *g = (dt_colorspaces_cl_global_t *)malloc(sizeof(dt_colorspaces_cl_global_t));

  const int program = 23; // colorspaces.cl, from programs.conf
  g->kernel_colorspaces_transform_lab_to_rgb_matrix = dt_opencl_create_kernel(program, "colorspaces_transform_lab_to_rgb_matrix");
  g->kernel_colorspaces_transform_rgb_matrix_to_lab = dt_opencl_create_kernel(program, "colorspaces_transform_rgb_matrix_to_lab");
  g->kernel_colorspaces_transform_rgb_matrix_to_rgb
      = dt_opencl_create_kernel(program, "colorspaces_transform_rgb_matrix_to_rgb");
  return g;
}

void dt_colorspaces_free_cl_global(dt_colorspaces_cl_global_t *g)
{
  if(!g) return;

  // destroy kernels
  dt_opencl_free_kernel(g->kernel_colorspaces_transform_lab_to_rgb_matrix);
  dt_opencl_free_kernel(g->kernel_colorspaces_transform_rgb_matrix_to_lab);
  dt_opencl_free_kernel(g->kernel_colorspaces_transform_rgb_matrix_to_rgb);

  free(g);
}

void dt_ioppr_get_profile_info_cl(const dt_iop_order_iccprofile_info_t *const profile_info, dt_colorspaces_iccprofile_info_cl_t *profile_info_cl)
{
  for(int i = 0; i < 9; i++)
  {
    profile_info_cl->matrix_in[i] = profile_info->matrix_in[i];
    profile_info_cl->matrix_out[i] = profile_info->matrix_out[i];
  }
  profile_info_cl->lutsize = profile_info->lutsize;
  for(int i = 0; i < 3; i++)
  {
    for(int j = 0; j < 3; j++)
    {
      profile_info_cl->unbounded_coeffs_in[i][j] = profile_info->unbounded_coeffs_in[i][j];
      profile_info_cl->unbounded_coeffs_out[i][j] = profile_info->unbounded_coeffs_out[i][j];
    }
  }
  profile_info_cl->nonlinearlut = profile_info->nonlinearlut;
  profile_info_cl->grey = profile_info->grey;
}

cl_float *dt_ioppr_get_trc_cl(const dt_iop_order_iccprofile_info_t *const profile_info)
{
  cl_float *trc = malloc(profile_info->lutsize * 6 * sizeof(cl_float));
  if(trc)
  {
    int x = 0;
    for(int c = 0; c < 3; c++)
      for(int y = 0; y < profile_info->lutsize; y++, x++)
        trc[x] = profile_info->lut_in[c][y];
    for(int c = 0; c < 3; c++)
      for(int y = 0; y < profile_info->lutsize; y++, x++)
        trc[x] = profile_info->lut_out[c][y];
  }
  return trc;
}

cl_int dt_ioppr_build_iccprofile_params_cl(const dt_iop_order_iccprofile_info_t *const profile_info,
                                           const int devid, dt_colorspaces_iccprofile_info_cl_t **_profile_info_cl,
                                           cl_float **_profile_lut_cl, cl_mem *_dev_profile_info,
                                           cl_mem *_dev_profile_lut)
{
  cl_int err = CL_SUCCESS;

  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl = calloc(1, sizeof(dt_colorspaces_iccprofile_info_cl_t));
  cl_float *profile_lut_cl = NULL;
  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;

  if(profile_info)
  {
    dt_ioppr_get_profile_info_cl(profile_info, profile_info_cl);
    profile_lut_cl = dt_ioppr_get_trc_cl(profile_info);

    dev_profile_info = dt_opencl_copy_host_to_device_constant(devid, sizeof(*profile_info_cl), profile_info_cl);
    if(dev_profile_info == NULL)
    {
      fprintf(stderr, "[dt_ioppr_build_iccprofile_params_cl] error allocating memory 5\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    dev_profile_lut = dt_opencl_copy_host_to_device(devid, profile_lut_cl, 256, 256 * 6, sizeof(float));
    if(dev_profile_lut == NULL)
    {
      fprintf(stderr, "[dt_ioppr_build_iccprofile_params_cl] error allocating memory 6\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
  }
  else
  {
    profile_lut_cl = malloc(1 * 6 * sizeof(cl_float));

    dev_profile_lut = dt_opencl_copy_host_to_device(devid, profile_lut_cl, 1, 1 * 6, sizeof(float));
    if(dev_profile_lut == NULL)
    {
      fprintf(stderr, "[dt_ioppr_build_iccprofile_params_cl] error allocating memory 7\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
  }

cleanup:
  *_profile_info_cl = profile_info_cl;
  *_profile_lut_cl = profile_lut_cl;
  *_dev_profile_info = dev_profile_info;
  *_dev_profile_lut = dev_profile_lut;

  return err;
}

void dt_ioppr_free_iccprofile_params_cl(dt_colorspaces_iccprofile_info_cl_t **_profile_info_cl,
                                        cl_float **_profile_lut_cl, cl_mem *_dev_profile_info,
                                        cl_mem *_dev_profile_lut)
{
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl = *_profile_info_cl;
  cl_float *profile_lut_cl = *_profile_lut_cl;
  cl_mem dev_profile_info = *_dev_profile_info;
  cl_mem dev_profile_lut = *_dev_profile_lut;

  if(profile_info_cl) free(profile_info_cl);
  if(dev_profile_info) dt_opencl_release_mem_object(dev_profile_info);
  if(dev_profile_lut) dt_opencl_release_mem_object(dev_profile_lut);
  if(profile_lut_cl) free(profile_lut_cl);

  *_profile_info_cl = NULL;
  *_profile_lut_cl = NULL;
  *_dev_profile_info = NULL;
  *_dev_profile_lut = NULL;
}

int dt_ioppr_transform_image_colorspace_cl(struct dt_iop_module_t *self, const int devid, cl_mem dev_img_in,
                                           cl_mem dev_img_out, const int width, const int height,
                                           const int cst_from, const int cst_to, int *converted_cst,
                                           const dt_iop_order_iccprofile_info_t *const profile_info)
{
  cl_int err = CL_SUCCESS;

  if(cst_from == cst_to)
  {
    *converted_cst = cst_to;
    return TRUE;
  }
  if(profile_info == NULL)
  {
    // The range information below is not informative in case it's the colorin module itself.
    if (strcmp(self->op,"colorin"))
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] module %s must be between input color profile and output color profile\n", self->op);
    *converted_cst = cst_from;
    return FALSE;
  }
  if(profile_info->type == DT_COLORSPACE_NONE)
  {
    *converted_cst = cst_from;
    return FALSE;
  }

  const int ch = 4;
  float *src_buffer = NULL;
  int in_place = (dev_img_in == dev_img_out);

  int kernel_transform = 0;
  cl_mem dev_tmp = NULL;
  cl_mem dev_profile_info = NULL;
  cl_mem dev_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t profile_info_cl;
  cl_float *lut_cl = NULL;

  *converted_cst = cst_from;

  // if we have a matrix use opencl
  if(!isnan(profile_info->matrix_in[0]) && !isnan(profile_info->matrix_out[0]))
  {
    dt_times_t start_time = { 0 }, end_time = { 0 };
    if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };

    if(cst_from == iop_cs_rgb && cst_to == iop_cs_Lab)
    {
      kernel_transform = darktable.opencl->colorspaces->kernel_colorspaces_transform_rgb_matrix_to_lab;
    }
    else if(cst_from == iop_cs_Lab && cst_to == iop_cs_rgb)
    {
      kernel_transform = darktable.opencl->colorspaces->kernel_colorspaces_transform_lab_to_rgb_matrix;
    }
    else
    {
      err = CL_INVALID_KERNEL;
      *converted_cst = cst_from;
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] invalid conversion from %i to %i\n", cst_from, cst_to);
      goto cleanup;
    }

    dt_ioppr_get_profile_info_cl(profile_info, &profile_info_cl);
    lut_cl = dt_ioppr_get_trc_cl(profile_info);

    if(in_place)
    {
      dev_tmp = dt_opencl_alloc_device(devid, width, height, 4 * sizeof(float));
      if(dev_tmp == NULL)
      {
        fprintf(stderr,
                "[dt_ioppr_transform_image_colorspace_cl] error allocating memory for color transformation 4\n");
        err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
        goto cleanup;
      }

      err = dt_opencl_enqueue_copy_image(devid, dev_img_in, dev_tmp, origin, origin, region);
      if(err != CL_SUCCESS)
      {
        fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error on copy image for color transformation\n");
        goto cleanup;
      }
    }
    else
    {
      dev_tmp = dev_img_in;
    }

    dev_profile_info = dt_opencl_copy_host_to_device_constant(devid, sizeof(profile_info_cl), &profile_info_cl);
    if(dev_profile_info == NULL)
    {
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error allocating memory for color transformation 5\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
    dev_lut = dt_opencl_copy_host_to_device(devid, lut_cl, 256, 256 * 6, sizeof(float));
    if(dev_lut == NULL)
    {
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error allocating memory for color transformation 6\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

    dt_opencl_set_kernel_arg(devid, kernel_transform, 0, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 1, sizeof(cl_mem), (void *)&dev_img_out);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 4, sizeof(cl_mem), (void *)&dev_profile_info);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 5, sizeof(cl_mem), (void *)&dev_lut);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel_transform, sizes);
    if(err != CL_SUCCESS)
    {
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error %i enqueue kernel for color transformation\n", err);
      goto cleanup;
    }

    *converted_cst = cst_to;

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform %s-->%s took %.3f secs (%.3f GPU) [%s %s]\n",
          (cst_from == iop_cs_rgb) ? "RGB": "Lab", (cst_to == iop_cs_rgb) ? "RGB": "Lab",
          end_time.clock - start_time.clock, end_time.user - start_time.user, self->op, self->multi_name);
    }
  }
  else
  {
    // no matrix, call lcms2
    src_buffer = dt_alloc_align(64, width * height * ch * sizeof(float));
    if(src_buffer == NULL)
    {
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error allocating memory for color transformation 1\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    err = dt_opencl_copy_device_to_host(devid, src_buffer, dev_img_in, width, height, ch * sizeof(float));
    if(err != CL_SUCCESS)
    {
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error allocating memory for color transformation 2\n");
      goto cleanup;
    }

    // just call the CPU version for now
    dt_ioppr_transform_image_colorspace(self, src_buffer, src_buffer, width, height, cst_from, cst_to,
                                        converted_cst, profile_info);

    err = dt_opencl_write_host_to_device(devid, src_buffer, dev_img_out, width, height, ch * sizeof(float));
    if(err != CL_SUCCESS)
    {
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error allocating memory for color transformation 3\n");
      goto cleanup;
    }
  }

cleanup:
  if(src_buffer) dt_free_align(src_buffer);
  if(dev_tmp && in_place) dt_opencl_release_mem_object(dev_tmp);
  if(dev_profile_info) dt_opencl_release_mem_object(dev_profile_info);
  if(dev_lut) dt_opencl_release_mem_object(dev_lut);
  if(lut_cl) free(lut_cl);

  return (err == CL_SUCCESS) ? TRUE : FALSE;
}

int dt_ioppr_transform_image_colorspace_rgb_cl(const int devid, cl_mem dev_img_in, cl_mem dev_img_out,
                                               const int width, const int height,
                                               const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                               const dt_iop_order_iccprofile_info_t *const profile_info_to,
                                               const char *message)
{
  cl_int err = CL_SUCCESS;

  if(profile_info_from->type == DT_COLORSPACE_NONE || profile_info_to->type == DT_COLORSPACE_NONE)
  {
    return FALSE;
  }
  if(profile_info_from->type == profile_info_to->type
     && strcmp(profile_info_from->filename, profile_info_to->filename) == 0)
  {
    if(dev_img_in != dev_img_out)
    {
      size_t origin[] = { 0, 0, 0 };
      size_t region[] = { width, height, 1 };

      err = dt_opencl_enqueue_copy_image(devid, dev_img_in, dev_img_out, origin, origin, region);
      if(err != CL_SUCCESS)
      {
        fprintf(stderr,
                "[dt_ioppr_transform_image_colorspace_rgb_cl] error on copy image for color transformation\n");
        return FALSE;
      }
    }

    return TRUE;
  }

  const int ch = 4;
  float *src_buffer_in = NULL;
  float *src_buffer_out = NULL;
  int in_place = (dev_img_in == dev_img_out);

  int kernel_transform = 0;
  cl_mem dev_tmp = NULL;

  cl_mem dev_profile_info_from = NULL;
  cl_mem dev_lut_from = NULL;
  dt_colorspaces_iccprofile_info_cl_t profile_info_from_cl;
  cl_float *lut_from_cl = NULL;

  cl_mem dev_profile_info_to = NULL;
  cl_mem dev_lut_to = NULL;
  dt_colorspaces_iccprofile_info_cl_t profile_info_to_cl;
  cl_float *lut_to_cl = NULL;

  // if we have a matrix use opencl
  if(!isnan(profile_info_from->matrix_in[0]) && !isnan(profile_info_from->matrix_out[0])
     && !isnan(profile_info_to->matrix_in[0]) && !isnan(profile_info_to->matrix_out[0]))
  {
    dt_times_t start_time = { 0 }, end_time = { 0 };
    if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };

    kernel_transform = darktable.opencl->colorspaces->kernel_colorspaces_transform_rgb_matrix_to_rgb;

    dt_ioppr_get_profile_info_cl(profile_info_from, &profile_info_from_cl);
    lut_from_cl = dt_ioppr_get_trc_cl(profile_info_from);

    dt_ioppr_get_profile_info_cl(profile_info_to, &profile_info_to_cl);
    lut_to_cl = dt_ioppr_get_trc_cl(profile_info_to);

    if(in_place)
    {
      dev_tmp = dt_opencl_alloc_device(devid, width, height, 4 * sizeof(float));
      if(dev_tmp == NULL)
      {
        fprintf(
            stderr,
            "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 4\n");
        err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
        goto cleanup;
      }

      err = dt_opencl_enqueue_copy_image(devid, dev_img_in, dev_tmp, origin, origin, region);
      if(err != CL_SUCCESS)
      {
        fprintf(stderr,
                "[dt_ioppr_transform_image_colorspace_rgb_cl] error on copy image for color transformation\n");
        goto cleanup;
      }
    }
    else
    {
      dev_tmp = dev_img_in;
    }

    dev_profile_info_from
        = dt_opencl_copy_host_to_device_constant(devid, sizeof(profile_info_from_cl), &profile_info_from_cl);
    if(dev_profile_info_from == NULL)
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 5\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
    dev_lut_from = dt_opencl_copy_host_to_device(devid, lut_from_cl, 256, 256 * 6, sizeof(float));
    if(dev_lut_from == NULL)
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 6\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    dev_profile_info_to
        = dt_opencl_copy_host_to_device_constant(devid, sizeof(profile_info_to_cl), &profile_info_to_cl);
    if(dev_profile_info_to == NULL)
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 7\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
    dev_lut_to = dt_opencl_copy_host_to_device(devid, lut_to_cl, 256, 256 * 6, sizeof(float));
    if(dev_lut_to == NULL)
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 8\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

    dt_opencl_set_kernel_arg(devid, kernel_transform, 0, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 1, sizeof(cl_mem), (void *)&dev_img_out);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 4, sizeof(cl_mem), (void *)&dev_profile_info_from);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 5, sizeof(cl_mem), (void *)&dev_lut_from);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 6, sizeof(cl_mem), (void *)&dev_profile_info_to);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 7, sizeof(cl_mem), (void *)&dev_lut_to);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel_transform, sizes);
    if(err != CL_SUCCESS)
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error %i enqueue kernel for color transformation\n",
              err);
      goto cleanup;
    }

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform RGB-->RGB took %.3f secs (%.3f GPU) [%s]\n",
              end_time.clock - start_time.clock, end_time.user - start_time.user, (message) ? message : "");
    }
  }
  else
  {
    // no matrix, call lcms2
    src_buffer_in = dt_alloc_align(64, width * height * ch * sizeof(float));
    src_buffer_out = dt_alloc_align(64, width * height * ch * sizeof(float));
    if(src_buffer_in == NULL || src_buffer_out == NULL)
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 1\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    err = dt_opencl_copy_device_to_host(devid, src_buffer_in, dev_img_in, width, height, ch * sizeof(float));
    if(err != CL_SUCCESS)
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 2\n");
      goto cleanup;
    }

    // just call the CPU version for now
    dt_ioppr_transform_image_colorspace_rgb(src_buffer_in, src_buffer_out, width, height, profile_info_from,
                                            profile_info_to, message);

    err = dt_opencl_write_host_to_device(devid, src_buffer_out, dev_img_out, width, height, ch * sizeof(float));
    if(err != CL_SUCCESS)
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 3\n");
      goto cleanup;
    }
  }

cleanup:
  if(src_buffer_in) dt_free_align(src_buffer_in);
  if(src_buffer_out) dt_free_align(src_buffer_out);
  if(dev_tmp && in_place) dt_opencl_release_mem_object(dev_tmp);

  if(dev_profile_info_from) dt_opencl_release_mem_object(dev_profile_info_from);
  if(dev_lut_from) dt_opencl_release_mem_object(dev_lut_from);
  if(lut_from_cl) free(lut_from_cl);

  if(dev_profile_info_to) dt_opencl_release_mem_object(dev_profile_info_to);
  if(dev_lut_to) dt_opencl_release_mem_object(dev_lut_to);
  if(lut_to_cl) free(lut_to_cl);

  return (err == CL_SUCCESS) ? TRUE : FALSE;
}
#endif

#undef DT_IOP_ORDER_INFO
