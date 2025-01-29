/*
    This file is part of darktable,
    Copyright (C) 2010-2024 darktable developers.

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

#ifdef HAVE_OPENCL

#include "common/opencl.h"
#include "common/bilateralcl.h"
#include "common/darktable.h"
#include "common/dlopencl.h"
#include "common/dwt.h"
#include "common/file_location.h"
#include "common/gaussian.h"
#include "common/guided_filter.h"
#include "common/heal.h"
#include "common/interpolation.h"
#include "common/locallaplaciancl.h"
#include "common/nvidia_gpus.h"
#include "common/opencl_drivers_blacklist.h"
#include "common/tea.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/pixelpipe.h"

#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <sys/stat.h>
#include <zlib.h>

static const char *_opencl_get_vendor_by_id(unsigned int id);

static gboolean _opencl_load_program(const int dev,
                                     const int prog,
                                     const char *programname,
                                     const char *filename,
                                     const char *binname,
                                     const char *cachedir,
                                     char *md5sum,
                                     char **includemd5,
                                     int *loaded_cached);

static gboolean _opencl_build_program(const int dev,
                                      const int prog,
                                      const char *binname,
                                      const char *cachedir,
                                      char *md5sum,
                                      int loaded_cached);

static char *_ascii_str_canonical(const char *in, char *out, int maxlen);

static char *_strsep(char **stringp, const char *delim);

/** read scheduling profile for config variables */
static dt_opencl_scheduling_profile_t _opencl_get_scheduling_profile(void);

/** adjust opencl subsystem according to scheduling profile */
static void _opencl_apply_scheduling_profile(dt_opencl_scheduling_profile_t profile);

static cl_event *_opencl_events_get_slot(const int devid, const char *tag);

static void _opencl_md5sum(const char **files, char **md5sums);

const char *cl_errstr(cl_int error)
{
  switch(error)
  {
    case CL_SUCCESS:
      return "CL_SUCCESS";
    case CL_DEVICE_NOT_FOUND:
      return "CL_DEVICE_NOT_FOUND";
    case CL_DEVICE_NOT_AVAILABLE:
      return "CL_DEVICE_NOT_AVAILABLE";
    case CL_COMPILER_NOT_AVAILABLE:
      return "CL_COMPILER_NOT_AVAILABLE";
    case CL_MEM_OBJECT_ALLOCATION_FAILURE:
      return "CL_MEM_OBJECT_ALLOCATION_FAILURE";
    case CL_OUT_OF_RESOURCES:
      return "CL_OUT_OF_RESOURCES";
    case CL_OUT_OF_HOST_MEMORY:
      return "CL_OUT_OF_HOST_MEMORY";
    case CL_PROFILING_INFO_NOT_AVAILABLE:
      return "CL_PROFILING_INFO_NOT_AVAILABLE";
    case CL_MEM_COPY_OVERLAP:
      return "CL_MEM_COPY_OVERLAP";
    case CL_IMAGE_FORMAT_MISMATCH:
      return "CL_IMAGE_FORMAT_MISMATCH";
    case CL_IMAGE_FORMAT_NOT_SUPPORTED:
      return "CL_IMAGE_FORMAT_NOT_SUPPORTED";
    case CL_BUILD_PROGRAM_FAILURE:
      return "CL_BUILD_PROGRAM_FAILURE";
    case CL_MAP_FAILURE:
      return "CL_MAP_FAILURE";
    case CL_MISALIGNED_SUB_BUFFER_OFFSET:
      return "CL_MISALIGNED_SUB_BUFFER_OFFSET";
    case CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST:
      return "CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST";
    case CL_COMPILE_PROGRAM_FAILURE:
      return "CL_COMPILE_PROGRAM_FAILURE";
    case CL_LINKER_NOT_AVAILABLE:
      return "CL_LINKER_NOT_AVAILABLE";
    case CL_LINK_PROGRAM_FAILURE:
      return "CL_LINK_PROGRAM_FAILURE";
    case CL_DEVICE_PARTITION_FAILED:
      return "CL_DEVICE_PARTITION_FAILED";
    case CL_KERNEL_ARG_INFO_NOT_AVAILABLE:
      return "CL_KERNEL_ARG_INFO_NOT_AVAILABLE";
    case CL_INVALID_VALUE:
      return "CL_INVALID_VALUE";
    case CL_INVALID_DEVICE_TYPE:
      return "CL_INVALID_DEVICE_TYPE";
    case CL_INVALID_PLATFORM:
      return "CL_INVALID_PLATFORM";
    case CL_INVALID_DEVICE:
      return "CL_INVALID_DEVICE";
    case CL_INVALID_CONTEXT:
      return "CL_INVALID_CONTEXT";
    case CL_INVALID_QUEUE_PROPERTIES:
      return "CL_INVALID_QUEUE_PROPERTIES";
    case CL_INVALID_COMMAND_QUEUE:
      return "CL_INVALID_COMMAND_QUEUE";
    case CL_INVALID_HOST_PTR:
      return "CL_INVALID_HOST_PTR";
    case CL_INVALID_MEM_OBJECT:
      return "CL_INVALID_MEM_OBJECT";
    case CL_INVALID_IMAGE_FORMAT_DESCRIPTOR:
      return "CL_INVALID_IMAGE_FORMAT_DESCRIPTOR";
    case CL_INVALID_IMAGE_SIZE:
      return "CL_INVALID_IMAGE_SIZE";
    case CL_INVALID_SAMPLER:
      return "CL_INVALID_SAMPLER";
    case CL_INVALID_BINARY:
      return "CL_INVALID_BINARY";
    case CL_INVALID_BUILD_OPTIONS:
      return "CL_INVALID_BUILD_OPTIONS";
    case CL_INVALID_PROGRAM:
      return "CL_INVALID_PROGRAM";
    case CL_INVALID_PROGRAM_EXECUTABLE:
      return "CL_INVALID_PROGRAM_EXECUTABLE";
    case CL_INVALID_KERNEL_NAME:
      return "CL_INVALID_KERNEL_NAME";
    case CL_INVALID_KERNEL_DEFINITION:
      return "CL_INVALID_KERNEL_DEFINITION";
    case CL_INVALID_KERNEL:
      return "CL_INVALID_KERNEL";
    case CL_INVALID_ARG_INDEX:
      return "CL_INVALID_ARG_INDEX";
    case CL_INVALID_ARG_VALUE:
      return "CL_INVALID_ARG_VALUE";
    case CL_INVALID_ARG_SIZE:
      return "CL_INVALID_ARG_SIZE";
    case CL_INVALID_KERNEL_ARGS:
      return "CL_INVALID_KERNEL_ARGS";
    case CL_INVALID_WORK_DIMENSION:
      return "CL_INVALID_WORK_DIMENSION";
    case CL_INVALID_WORK_GROUP_SIZE:
      return"CL_INVALID_WORK_GROUP_SIZE";
    case CL_INVALID_WORK_ITEM_SIZE:
      return"CL_INVALID_WORK_ITEM_SIZE";
    case CL_INVALID_GLOBAL_OFFSET:
      return"CL_INVALID_GLOBAL_OFFSET";
    case CL_INVALID_EVENT_WAIT_LIST:
      return"CL_INVALID_EVENT_WAIT_LIST";
    case CL_INVALID_EVENT:
      return"CL_INVALID_EVENT";
    case CL_INVALID_OPERATION:
      return"CL_INVALID_OPERATION";
    case CL_INVALID_GL_OBJECT:
      return"CL_INVALID_GL_OBJECT";
    case CL_INVALID_BUFFER_SIZE:
      return"CL_INVALID_BUFFER_SIZE";
    case CL_INVALID_MIP_LEVEL:
      return"CL_INVALID_MIP_LEVEL";
    case CL_INVALID_GLOBAL_WORK_SIZE:
      return"CL_INVALID_GLOBAL_WORK_SIZE";
    case CL_INVALID_PROPERTY:
      return"CL_INVALID_PROPERTY";
    case CL_INVALID_IMAGE_DESCRIPTOR:
      return"CL_INVALID_IMAGE_DESCRIPTOR";
    case CL_INVALID_COMPILER_OPTIONS:
      return"CL_INVALID_COMPILER_OPTIONS";
    case CL_INVALID_LINKER_OPTIONS:
      return"CL_INVALID_LINKER_OPTIONS";
    case CL_INVALID_DEVICE_PARTITION_COUNT:
      return"CL_INVALID_DEVICE_PARTITION_COUNT";
    case DT_OPENCL_DEFAULT_ERROR:
      return "DT_OPENCL_DEFAULT_ERROR";
    case DT_OPENCL_SYSMEM_ALLOCATION:
      return "DT_OPENCL_SYSMEM_ALLOCATION";
    case DT_OPENCL_PROCESS_CL:
      return "DT_OPENCL_PROCESS_CL";
    case DT_OPENCL_NODEVICE:
      return "DT_OPENCL_NODEVICE";
    default:
      return "Unknown OpenCL error";
  }
}

static inline void _check_clmem_err(const int devid, const cl_int err)
{
  if((err == CL_MEM_OBJECT_ALLOCATION_FAILURE) || (err == CL_OUT_OF_RESOURCES))
  darktable.opencl->dev[devid].clmem_error |= TRUE;
}

static inline gboolean _cl_running(void)
{
  dt_opencl_t *cl = darktable.opencl;
  return cl->inited && cl->enabled && !cl->stopped;
}

static inline gboolean _cldev_running(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  return cl->inited && cl->enabled && !cl->stopped && (devid>=0);
}

int dt_opencl_get_device_info(dt_opencl_t *cl,
                              cl_device_id device,
                              cl_device_info param_name,
                              void **param_value,
                              size_t *param_value_size)
{
  *param_value_size = SIZE_MAX;

  // 1. figure out how much memory is needed
  cl_int err = (cl->dlocl->symbols->dt_clGetDeviceInfo)
    (device, param_name, 0, NULL, param_value_size);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[dt_opencl_get_device_info] could not query the actual size"
             " in bytes of info %d: %s", param_name, cl_errstr(err));
    goto error;
  }

  // 2. did we /actually/ get the size?
  if(*param_value_size == SIZE_MAX
     || *param_value_size == 0)
  {
    // both of these sizes make no sense. either i failed to parse
    // spec, or opencl implementation bug?
    dt_print(DT_DEBUG_OPENCL,
             "[dt_opencl_get_device_info] ERROR: no size returned,"
             " or zero size returned for data %d: %zu",
             param_name, *param_value_size);
    err = CL_INVALID_VALUE; // FIXME: anything better?
    goto error;
  }

  // 3. make sure that *param_value points to big-enough memory block
  {
    void *ptr = realloc(*param_value, *param_value_size);
    if(!ptr)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[dt_opencl_get_device_info] memory allocation failed!"
               " tried to allocate %zu bytes for data %d: %s",
               *param_value_size, param_name, cl_errstr(err));
      err = CL_OUT_OF_HOST_MEMORY;
      goto error;
    }

    // allocation succeeded, update pointer.
    *param_value = ptr;
  }

  // 4. actually get the value
  err = (cl->dlocl->symbols->dt_clGetDeviceInfo)
    (device, param_name, *param_value_size, *param_value, NULL);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[dt_opencl_get_device_info] could not query info %d: %s",
             param_name, cl_errstr(err));
    goto error;
  }

  return CL_SUCCESS;

error:
  free(*param_value);
  *param_value = NULL;
  *param_value_size = 0;
  return err;
}

gboolean dt_opencl_avoid_atomics(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  return (!_cldev_running(devid)) ? FALSE : cl->dev[devid].avoid_atomics;
}

void dt_opencl_micro_nap(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  if(_cldev_running(devid))
    dt_iop_nap(cl->dev[devid].micro_nap);
}

gboolean dt_opencl_use_pinned_memory(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  return (!_cldev_running(devid)) ? FALSE : cl->dev[devid].pinned_memory;
}

void dt_opencl_write_device_config(const int devid)
{
  if(devid < 0) return;
  dt_opencl_t *cl = darktable.opencl;
  gchar key[256] = { 0 };
  gchar dat[512] = { 0 };
  g_snprintf(key, 254, "%s%s", DT_CLDEVICE_HEAD, cl->dev[devid].cname);
  g_snprintf(dat, 510, "%i %i %i %i %i %i %i %i %.3f %.3f %.3f",
    cl->dev[devid].avoid_atomics,
    cl->dev[devid].micro_nap,
    cl->dev[devid].pinned_memory,
    cl->dev[devid].clroundup_wd,
    cl->dev[devid].clroundup_ht,
    cl->dev[devid].event_handles,
    cl->dev[devid].asyncmode,
    cl->dev[devid].disabled,
    0.0f, // dummy for now as we don't have the benching any more
    cl->dev[devid].advantage,
    cl->dev[devid].unified_fraction);
  dt_print(DT_DEBUG_OPENCL | DT_DEBUG_VERBOSE,
           "[dt_opencl_write_device_config] writing data '%s' for '%s'", dat, key);
  dt_conf_set_string(key, dat);

  // Also take care of extended device data, these are not only device
  // specific but also depend on the devid to support systems with two
  // similar cards.
  g_snprintf(key, 254, "%s%s_id%i", DT_CLDEVICE_HEAD, cl->dev[devid].cname, devid);
  g_snprintf(dat, 510, "%i", cl->dev[devid].headroom);
  dt_print(DT_DEBUG_OPENCL | DT_DEBUG_VERBOSE,
           "[dt_opencl_write_device_config] writing data '%s' for '%s'", dat, key);
  dt_conf_set_string(key, dat);
}

gboolean dt_opencl_read_device_config(const int devid)
{
  if(devid < 0) return FALSE;
  dt_opencl_t *cl = darktable.opencl;
  dt_opencl_device_t *cldid = &cl->dev[devid];
  gchar key[256] = { 0 };
  g_snprintf(key, 254, "%s%s", DT_CLDEVICE_HEAD, cl->dev[devid].cname);

  const gboolean existing_device = dt_conf_key_not_empty(key);
  gboolean safety_ok = TRUE;
  if(existing_device)
  {
    const gchar *dat = dt_conf_get_string_const(key);
    int avoid_atomics;
    int micro_nap;
    int pinned_memory;
    int wd;
    int ht;
    int event_handles;
    int asyncmode;
    int disabled;
    float dummy; // unused
    float advantage;
    float unified_fraction;
    sscanf(dat, "%i %i %i %i %i %i %i %i %f %f %f",
           &avoid_atomics, &micro_nap, &pinned_memory, &wd, &ht,
           &event_handles, &asyncmode, &disabled, &dummy, &advantage, &unified_fraction);

    // some rudimentary safety checking if string seems to be ok
    safety_ok = (wd > 1) && (wd < 513) && (ht > 1) && (ht < 513);

    if(safety_ok)
    {
      cldid->avoid_atomics = avoid_atomics;
      cldid->micro_nap = micro_nap;
      cldid->pinned_memory = pinned_memory;
      cldid->clroundup_wd = wd;
      cldid->clroundup_ht = ht;
      cldid->event_handles = event_handles;
      cldid->asyncmode = asyncmode;
      cldid->disabled = disabled;
      cldid->advantage = advantage;
      cldid->unified_fraction = unified_fraction;
    }
    else // if there is something wrong with the found conf key reset to defaults
    {
      dt_print(DT_DEBUG_OPENCL,
               "[dt_opencl_read_device_config] malformed data '%s' for '%s'", dat, key);
    }
  }
  // do some safety housekeeping
  if((cldid->unified_fraction < 0.05f) || (cldid->unified_fraction > 0.5f))
    cldid->unified_fraction = 0.25f;
  if((cldid->micro_nap < 0) || (cldid->micro_nap > 1000000))
    cldid->micro_nap = 250;
  if((cldid->clroundup_wd < 2) || (cldid->clroundup_wd > 512))
    cldid->clroundup_wd = 16;
  if((cldid->clroundup_ht < 2) || (cldid->clroundup_ht > 512))
    cldid->clroundup_ht = 16;
  if(cldid->event_handles < 0)
    cldid->event_handles = 0x40000000;

  cldid->use_events = cldid->event_handles ? TRUE : FALSE;
  cldid->asyncmode =  cldid->asyncmode ? TRUE : FALSE;
  cldid->disabled = cldid->disabled ? TRUE : FALSE;
  cldid->avoid_atomics = cldid->avoid_atomics ? TRUE : FALSE;
  cldid->pinned_memory = cldid->pinned_memory ? TRUE : FALSE;

  cldid->advantage = fmaxf(0.0f, cldid->advantage);

  // Also take care of extended device data, these are not only device
  // specific but also depend on the devid
  g_snprintf(key, 254, "%s%s_id%i", DT_CLDEVICE_HEAD, cldid->cname, devid);
  if(dt_conf_key_not_empty(key))
  {
    const gchar *dat = dt_conf_get_string_const(key);
    int headroom;
    sscanf(dat, "%i", &headroom);
    if(headroom > 0) cldid->headroom = headroom;
  }
  else // this is used if updating to 4.0 or fresh installs; see
       // commenting _opencl_get_unused_device_mem()
    cldid->headroom = DT_OPENCL_DEFAULT_HEADROOM;
  dt_opencl_write_device_config(devid);
  return !existing_device || !safety_ok;
}

// returns 0 if all ok or an error if we failed to init this device
static gboolean _opencl_device_init(dt_opencl_t *cl,
                                    const int dev,
                                    cl_device_id *devices,
                                    const int k)
{
  gboolean res = FALSE;
  cl_int err;

  memset(cl->dev[dev].program, 0x0, sizeof(cl_program) * DT_OPENCL_MAX_PROGRAMS);
  memset(cl->dev[dev].program_used, 0x0, sizeof(int) * DT_OPENCL_MAX_PROGRAMS);
  memset(cl->dev[dev].kernel, 0x0, sizeof(cl_kernel) * DT_OPENCL_MAX_KERNELS);
  memset(cl->dev[dev].kernel_used, 0x0, sizeof(int) * DT_OPENCL_MAX_KERNELS);
  cl->dev[dev].eventlist = NULL;
  cl->dev[dev].eventtags = NULL;
  cl->dev[dev].numevents = 0;
  cl->dev[dev].eventsconsolidated = 0;
  cl->dev[dev].maxevents = 0;
  cl->dev[dev].maxeventslot = 0;
  cl->dev[dev].lostevents = 0;
  cl->dev[dev].totalevents = 0;
  cl->dev[dev].totalsuccess = 0;
  cl->dev[dev].totallost = 0;
  cl->dev[dev].summary = CL_COMPLETE;
  cl->dev[dev].used_global_mem = 0;
  cl->dev[dev].nvidia_sm_20 = FALSE;
  cl->dev[dev].fullname = NULL;
  cl->dev[dev].cname = NULL;
  cl->dev[dev].options = NULL;
  cl->dev[dev].cflags = NULL;
  cl->dev[dev].memory_in_use = 0;
  cl->dev[dev].peak_memory = 0;
  cl->dev[dev].used_available = 0;
  // setting sane/conservative defaults at first
  cl->dev[dev].unified_fraction = 0.25f;
  cl->dev[dev].avoid_atomics = FALSE;
  cl->dev[dev].micro_nap = 250;
  cl->dev[dev].pinned_memory = FALSE;
  cl->dev[dev].unified_memory = FALSE;
  cl->dev[dev].pinned_error = FALSE;
  cl->dev[dev].clmem_error = FALSE;
  cl->dev[dev].clroundup_wd = 16;
  cl->dev[dev].clroundup_ht = 16;
  cl->dev[dev].advantage = 0.0f;
  cl->dev[dev].use_events = TRUE;
  cl->dev[dev].event_handles = 128;
  cl->dev[dev].asyncmode = FALSE;
  cl->dev[dev].disabled = FALSE;
  cl->dev[dev].headroom = 0;
  cl->dev[dev].tunehead = FALSE;
  cl_device_id devid = cl->dev[dev].devid = devices[k];

  char *device_name = NULL;
  size_t device_name_size;

  char *device_name_cleaned = NULL;

  char *fullname = NULL;

  char *cname = NULL;
  char *pname = NULL;

  char *vendor = NULL;
  size_t vendor_size;

  char *driverversion = NULL;
  size_t driverversion_size;

  char *deviceversion = NULL;
  size_t deviceversion_size;

  size_t infoint;
  size_t *infointtab = NULL;
  cl_device_type type;
  cl_bool image_support = 0;
  cl_bool device_available = 0;
  cl_uint vendor_id = 0;
  cl_bool little_endian = 0;
  cl_platform_id platform_id = 0;
  cl_bool unified_memory = 0;

  char *dtcache = calloc(PATH_MAX, sizeof(char));
  char *cachedir = calloc(PATH_MAX, sizeof(char));
  char *alnum_fullname = calloc(DT_OPENCL_CBUFFSIZE, sizeof(char));
  char *drvversion = calloc(DT_OPENCL_CBUFFSIZE, sizeof(char));
  char *platform_display_name = calloc(DT_OPENCL_CBUFFSIZE, sizeof(char));
  char *platform_name = calloc(DT_OPENCL_CBUFFSIZE, sizeof(char));
  char *platform_vendor = calloc(DT_OPENCL_CBUFFSIZE, sizeof(char));

  char kerneldir[PATH_MAX] = { 0 };
  char *filename = calloc(PATH_MAX, sizeof(char));
  char *confentry = calloc(PATH_MAX, sizeof(char));
  char *binname = calloc(PATH_MAX, sizeof(char));
  dt_print_nts(DT_DEBUG_OPENCL, "\n[dt_opencl_device_init]\n");

  // test GPU availability, vendor, memory, image support etc:
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_AVAILABLE,
                                           sizeof(cl_bool), &device_available, NULL);

  err = dt_opencl_get_device_info(cl, devid, CL_DEVICE_VENDOR,
                                  (void **)&vendor, &vendor_size);
  if(err != CL_SUCCESS)
  {
    dt_print_nts(DT_DEBUG_OPENCL,
                 "  *** could not get vendor name of device %d: %s\n", k, cl_errstr(err));
    res = TRUE;
    goto end;
  }

  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_VENDOR_ID,
                                           sizeof(cl_uint), &vendor_id, NULL);

  err = dt_opencl_get_device_info(cl, devid, CL_DEVICE_NAME,
                                  (void **)&device_name, &device_name_size);
  if(err != CL_SUCCESS)
  {
    dt_print_nts(DT_DEBUG_OPENCL,
                 "  *** could not get device name of device %d: %s\n",
                 k, cl_errstr(err));
    res = TRUE;
    goto end;
  }

  gboolean has_platform_name = TRUE;

  err = (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_PLATFORM,
                                                 sizeof(cl_platform_id), &platform_id, NULL);
  if(err != CL_SUCCESS)
  {
    has_platform_name = FALSE;
    g_strlcpy(platform_vendor, "no platform id", DT_OPENCL_CBUFFSIZE);
    g_strlcpy(platform_display_name, "no platform id", DT_OPENCL_CBUFFSIZE);
    dt_print_nts(DT_DEBUG_OPENCL,
                 "  *** could not get platform id for device `%s' : %s\n",
                 device_name, cl_errstr(err));
  }
  else
  {
    err = (cl->dlocl->symbols->dt_clGetPlatformInfo)
      (platform_id, CL_PLATFORM_NAME, DT_OPENCL_CBUFFSIZE, platform_name, NULL);
    if(err != CL_SUCCESS)
    {
      has_platform_name = FALSE;
      g_strlcpy(platform_display_name, "???", DT_OPENCL_CBUFFSIZE);
      dt_print_nts(DT_DEBUG_OPENCL,
                   "  *** could not get platform name for device `%s' : %s\n",
                   device_name, cl_errstr(err));
    }

    err = (cl->dlocl->symbols->dt_clGetPlatformInfo)
      (platform_id, CL_PLATFORM_VENDOR, DT_OPENCL_CBUFFSIZE, platform_vendor, NULL);
    if(err != CL_SUCCESS)
    {
      dt_print_nts(DT_DEBUG_OPENCL,
                   "  *** could not get platform vendor for device `%s' : %s\n",
                   device_name, cl_errstr(err));
      g_strlcpy(platform_vendor, "???", DT_OPENCL_CBUFFSIZE);
    }
  }

  if(has_platform_name)
    g_strlcpy(platform_display_name, platform_name, DT_OPENCL_CBUFFSIZE);
  else
    g_strlcpy(platform_name, "unknownplatform", DT_OPENCL_CBUFFSIZE);


  gboolean is_mesa = FALSE;
  device_name_cleaned = strdup(device_name);
  // If platform_vendor starts with Mesa (matches also Mesa/X.org) remove
  // everything starting with the first open parenthesis and removes trailing
  // white spaces.
  if(!strncasecmp(platform_vendor, "Mesa", 4))
  {
    device_name_cleaned = g_strchomp(_strsep(&device_name_cleaned, "("));
    dt_print_nts(DT_DEBUG_OPENCL,
                   "OpenCL Mesa platform `%s' --> `%s'\n",
                   platform_vendor, device_name_cleaned);
    is_mesa = TRUE;
  }

  // get the fullname
  fullname = g_strdup_printf("%s %s", platform_name, device_name_cleaned);

  // get the canonical fullname
  cname = _ascii_str_canonical(fullname, NULL , 0);
  pname = _ascii_str_canonical(platform_name, NULL , 0);
  // take every detected platform and device into account of checksum
  cl->crc = crc32(cl->crc, (const unsigned char *)platform_name, strlen(platform_name));
  cl->crc = crc32(cl->crc, (const unsigned char *)device_name, strlen(device_name));

  cl->dev[dev].fullname = strdup(fullname);
  cl->dev[dev].cname = strdup(cname);

  const gboolean newdevice = dt_opencl_read_device_config(dev);
  dt_print_nts(DT_DEBUG_OPENCL,
               "   DEVICE:                   %d: '%s'%s\n",
               k, device_name, (newdevice) ? ", NEW" : "" );
  dt_print_nts(DT_DEBUG_OPENCL,
               "   CONF KEY:                 %s%s\n",
               DT_CLDEVICE_HEAD, cl->dev[dev].cname);
  dt_print_nts(DT_DEBUG_OPENCL,
               "   PLATFORM, VENDOR & ID:    %s, %s%s, ID=%d\n",
               platform_display_name, is_mesa ? "Mesa:" : "", platform_vendor, vendor_id);
  dt_print_nts(DT_DEBUG_OPENCL,
               "   CANONICAL NAME:           %s\n", cl->dev[dev].cname);

  err = dt_opencl_get_device_info(cl, devid, CL_DRIVER_VERSION,
                                  (void **)&driverversion, &driverversion_size);
  if(err != CL_SUCCESS)
  {
    dt_print_nts(DT_DEBUG_OPENCL,
                 "   *** driver version not available *** %s\n", cl_errstr(err));
    res = TRUE;
    cl->dev[dev].disabled |= TRUE;
    goto end;
  }

  err = dt_opencl_get_device_info(cl, devid, CL_DEVICE_VERSION,
                                  (void **)&deviceversion, &deviceversion_size);
  if(err != CL_SUCCESS)
  {
    dt_print_nts(DT_DEBUG_OPENCL,
                 "   *** device version not available *** %s\n", cl_errstr(err));
    res = TRUE;
    cl->dev[dev].disabled |= TRUE;
    goto end;
  }

  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_TYPE,
                                           sizeof(cl_device_type), &type, NULL);
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_IMAGE_SUPPORT,
                                           sizeof(cl_bool), &image_support, NULL);
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_IMAGE2D_MAX_HEIGHT,
                                           sizeof(size_t),
                                           &(cl->dev[dev].max_image_height), NULL);
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_IMAGE2D_MAX_WIDTH,
                                           sizeof(size_t),
                                           &(cl->dev[dev].max_image_width), NULL);
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_MAX_MEM_ALLOC_SIZE,
                                           sizeof(cl_ulong),
                                           &(cl->dev[dev].max_mem_alloc), NULL);
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_ENDIAN_LITTLE,
                                           sizeof(cl_bool), &little_endian, NULL);
  // FIXME This test is deprecated for post 1.2 versions so if we do some cl version bump
  // we would want to use CL_DEVICE_SVM_CAPABILITIES instead
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_HOST_UNIFIED_MEMORY,
                                           sizeof(cl_bool), &unified_memory, NULL);
  cl->dev[dev].unified_memory = unified_memory ? TRUE : FALSE;

  if(!strncasecmp(platform_display_name, "NVIDIA CUDA", 11))
  {
    // very lame attempt to detect support for atomic float add in global memory.
    // we need compute model sm_20, but let's try for all nvidia devices :(
    cl->dev[dev].nvidia_sm_20 = dt_nvidia_gpu_supports_sm_20(device_name);
  }

  const gboolean is_cpu_device = (type & CL_DEVICE_TYPE_CPU) == CL_DEVICE_TYPE_CPU;

  // micro_nap can be made less conservative on current systems at least if not on-CPU
  if(newdevice)
    cl->dev[dev].micro_nap = (is_cpu_device) ? 1000 : 250;

  dt_print_nts(DT_DEBUG_OPENCL, "   DRIVER VERSION:           %s\n", driverversion);
  dt_print_nts(DT_DEBUG_OPENCL, "   DEVICE VERSION:           %s%s\n", deviceversion,
     cl->dev[dev].nvidia_sm_20 ? ", SM_20 SUPPORT" : "");
  dt_print_nts(DT_DEBUG_OPENCL, "   DEVICE_TYPE:              %s%s%s%s\n",
      ((type & CL_DEVICE_TYPE_CPU) == CL_DEVICE_TYPE_CPU) ? "CPU" : "",
      ((type & CL_DEVICE_TYPE_GPU) == CL_DEVICE_TYPE_GPU) ? "GPU" : "",
      (type & CL_DEVICE_TYPE_ACCELERATOR)                 ? ", Accelerator" : "",
      unified_memory ? ", unified mem" : ", dedicated mem" );

  if(is_cpu_device && newdevice)
  {
    dt_print_nts(DT_DEBUG_OPENCL,
                 "   *** discarding new device as emulated by CPU ***\n");
    cl->dev[dev].disabled |= TRUE;
    res = TRUE;
    goto end;
  }

  if(strlen(deviceversion) < 9)
  {
     dt_print_nts(DT_DEBUG_OPENCL,
                 "   *** no proper device version ***\n");
    res = TRUE;
    goto end;
  }
  else
  {
    if((strncmp(deviceversion+7, "1.0", 3) == 0)
      || (strncmp(deviceversion+7, "1.1", 3) == 0))
    {
      dt_print_nts(DT_DEBUG_OPENCL,
                 "   *** insufficient device version ***\n");
      res = TRUE;
      goto end;
    }
  }

  if(!device_available)
  {
    dt_print_nts(DT_DEBUG_OPENCL,
                 "   *** device is not available ***\n");
    res = TRUE;
    goto end;
  }

  if(!image_support)
  {
    dt_print_nts(DT_DEBUG_OPENCL,
                 "   *** The OpenCL driver doesn't provide image support."
                 " See also 'clinfo' output ***\n");
    res = TRUE;
    cl->dev[dev].disabled |= TRUE;
    goto end;
  }

  if(!little_endian)
  {
    dt_print_nts(DT_DEBUG_OPENCL,
                 "   *** device is not little endian ***\n");
    res = TRUE;
    cl->dev[dev].disabled |= TRUE;
    goto end;
  }

  (cl->dlocl->symbols->dt_clGetDeviceInfo)
    (devid, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(cl_ulong),
     &(cl->dev[dev].max_global_mem), NULL);
  if(cl->dev[dev].max_global_mem < (uint64_t)800ul * 1024ul * 1024ul)
  {
    dt_print_nts(DT_DEBUG_OPENCL,
                 "   *** insufficient global memory (%" PRIu64 "MB) ***\n",
                 cl->dev[dev].max_global_mem / 1024 / 1024);
    res = TRUE;
    cl->dev[dev].disabled |= TRUE;
    goto end;
  }

  const gboolean is_blacklisted = dt_opencl_check_driver_blacklist(deviceversion);

  // disable device for now if this is the first time detected and blacklisted too.
  if(newdevice && is_blacklisted)
  {
    // To keep installations we look for the old blacklist conf key
    const gboolean old_blacklist = dt_conf_get_bool("opencl_disable_drivers_blacklist");
    cl->dev[dev].disabled |= (old_blacklist) ? FALSE : TRUE;
    if(cl->dev[dev].disabled)
      dt_print_nts(DT_DEBUG_OPENCL, "   *** new device is blacklisted ***\n");
    res = TRUE;
    goto end;
  }

  dt_print_nts(DT_DEBUG_OPENCL,
               "   GLOBAL MEM SIZE:          %.0f MB\n",
               (double)cl->dev[dev].max_global_mem / 1024.0 / 1024.0);
  dt_print_nts(DT_DEBUG_OPENCL,
               "   MAX MEM ALLOC:            %.0f MB\n",
               (double)cl->dev[dev].max_mem_alloc / 1024.0 / 1024.0);
  dt_print_nts(DT_DEBUG_OPENCL,
               "   MAX IMAGE SIZE:           %zd x %zd\n",
               cl->dev[dev].max_image_width, cl->dev[dev].max_image_height);
  (cl->dlocl->symbols->dt_clGetDeviceInfo)
    (devid, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(infoint), &infoint, NULL);
  dt_print_nts(DT_DEBUG_OPENCL,
               "   MAX WORK GROUP SIZE:      %zu\n", infoint);
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS,
                                           sizeof(infoint), &infoint, NULL);
  dt_print_nts(DT_DEBUG_OPENCL,
               "   MAX WORK ITEM DIMENSIONS: %zu\n", infoint);

  size_t infointtab_size;
  err = dt_opencl_get_device_info(cl, devid, CL_DEVICE_MAX_WORK_ITEM_SIZES,
                                  (void **)&infointtab, &infointtab_size);
  if(err == CL_SUCCESS)
  {
    dt_print_nts(DT_DEBUG_OPENCL,
                 "   MAX WORK ITEM SIZES:      [ ");
    for(size_t i = 0; i < infoint; i++)
      dt_print_nts(DT_DEBUG_OPENCL, "%zu ", infointtab[i]);
    free(infointtab);
    infointtab = NULL;
    dt_print_nts(DT_DEBUG_OPENCL, "]\n");
  }
  else
  {
    dt_print_nts(DT_DEBUG_OPENCL,
                 "   *** could not get maximum work item sizes ***\n");
    res = TRUE;
    cl->dev[dev].disabled |= TRUE;
    goto end;
  }

  dt_print_nts(DT_DEBUG_OPENCL,
               "   ASYNC PIXELPIPE:          %s\n", cl->dev[dev].asyncmode ? "YES" : "NO");
  dt_print_nts(DT_DEBUG_OPENCL,
               "   PINNED MEMORY TRANSFER:   %s\n", cl->dev[dev].pinned_memory ? "YES" : "NO");
  dt_print_nts(DT_DEBUG_OPENCL,
               "   AVOID ATOMICS:            %s\n", cl->dev[dev].avoid_atomics ? "YES" : "NO");
  dt_print_nts(DT_DEBUG_OPENCL,
               "   MICRO NAP:                %i\n", cl->dev[dev].micro_nap);
  dt_print_nts(DT_DEBUG_OPENCL,
               "   ROUNDUP WIDTH & HEIGHT    %ix%i\n", cl->dev[dev].clroundup_wd, cl->dev[dev].clroundup_ht);
  dt_print_nts(DT_DEBUG_OPENCL,
               "   CHECK EVENT HANDLES:      %i\n", cl->dev[dev].event_handles);
  dt_print_nts(DT_DEBUG_OPENCL,
               "   TILING ADVANTAGE:         %.3f\n", cl->dev[dev].advantage);
  dt_print_nts(DT_DEBUG_OPENCL,
               "   DEFAULT DEVICE:           %s\n", (type & CL_DEVICE_TYPE_DEFAULT) ? "YES" : "NO");

  if(cl->dev[dev].disabled)
  {
    dt_print_nts(DT_DEBUG_OPENCL, "   *** marked as disabled ***\n");
    res = TRUE;
    goto end;
  }

  dt_pthread_mutex_init(&cl->dev[dev].lock, NULL);

  cl->dev[dev].context = (cl->dlocl->symbols->dt_clCreateContext)
    (0, 1, &devid, NULL, NULL, &err);
  if(err != CL_SUCCESS)
  {
    dt_print_nts(DT_DEBUG_OPENCL,
                 "   *** could not create context *** %s\n", cl_errstr(err));
    res = TRUE;
    goto end;
  }
  // create a command queue for first device the context reported
  cl->dev[dev].cmd_queue = (cl->dlocl->symbols->dt_clCreateCommandQueue)(
      cl->dev[dev].context, devid,
      (darktable.unmuted & DT_DEBUG_PERF)
      ? CL_QUEUE_PROFILING_ENABLE
      : 0,
      &err);
  if(err != CL_SUCCESS)
  {
    dt_print_nts(DT_DEBUG_OPENCL,
                 "   *** could not create command queue *** %s\n", cl_errstr(err));
    res = TRUE;
    goto end;
  }

  dt_loc_get_user_cache_dir(dtcache, PATH_MAX * sizeof(char));

  int len = MIN(strlen(fullname),1024 * sizeof(char));;
  int j = 0;
  // remove non-alphanumeric chars from device name
  for(int i = 0; i < len; i++)
    if(isalnum(fullname[i])) alnum_fullname[j++] = fullname[i];
  alnum_fullname[j] = 0;

  len = MIN(strlen(driverversion), 1024 * sizeof(char));
  j = 0;
  // remove non-alphanumeric chars from driver version
  for(int i = 0; i < len; i++)
    if(isalnum(driverversion[i])) drvversion[j++] = driverversion[i];
  drvversion[j] = 0;
  snprintf(cachedir, PATH_MAX * sizeof(char),
           "%s" G_DIR_SEPARATOR_S "cached_v%d_kernels_for_%s_%s",
    dtcache, DT_OPENCL_KERNELS, alnum_fullname, drvversion);
  if(g_mkdir_with_parents(cachedir, 0700) == -1)
  {
    dt_print_nts(DT_DEBUG_OPENCL,
                 "   *** failed to create kernel directory `%s' ***\n", cachedir);
    res = TRUE;
    goto end;
  }

  dt_loc_get_kerneldir(kerneldir, sizeof(kerneldir));
  dt_print_nts(DT_DEBUG_OPENCL, "   KERNEL BUILD DIRECTORY:   %s\n", kerneldir);
  dt_print_nts(DT_DEBUG_OPENCL, "   KERNEL DIRECTORY:         %s\n", cachedir);

  snprintf(filename, PATH_MAX * sizeof(char),
           "%s" G_DIR_SEPARATOR_S "programs.conf", kerneldir);

  char *escapedkerneldir = NULL;
#if !defined(__APPLE__) && !(defined(__linux__) && defined(__aarch64__))
  escapedkerneldir = g_strdup_printf("\"%s\"", kerneldir);
#else
  escapedkerneldir = dt_util_str_replace(kerneldir, " ", "\\ ");
#endif

  gchar* compile_option_name_cname =
    g_strdup_printf("%s%s_building", DT_CLDEVICE_HEAD, cl->dev[dev].cname);
  const char* compile_opt = NULL;

  if(dt_conf_key_exists(compile_option_name_cname)
     && (dt_conf_get_int("performance_configuration_version_completed") > 15))
    compile_opt = dt_conf_get_string_const(compile_option_name_cname);
  else
  {
    if(!strcmp("nvidiacuda", pname))
      compile_opt = DT_OPENCL_DEFAULT_COMPILE_OPTI;
    else if(!strcmp("apple", pname))
      compile_opt = DT_OPENCL_DEFAULT_COMPILE_OPTI;
    else if(!strcmp("amdacceleratedparallelprocessing", pname))
      compile_opt = DT_OPENCL_DEFAULT_COMPILE_OPTI;
    else
      compile_opt = DT_OPENCL_DEFAULT_COMPILE_DEFAULT;
  }

  gchar *my_option = g_strdup(compile_opt);
  dt_conf_set_string(compile_option_name_cname, my_option);

  cl->dev[dev].cflags = g_strdup_printf("-w %s%s -D%s=1",
                                  my_option,
                                  cl->dev[dev].nvidia_sm_20 ? " -DNVIDIA_SM_20=1" : "",
                                  _opencl_get_vendor_by_id(vendor_id));
  cl->dev[dev].options = g_strdup_printf("%s -I%s",
                             cl->dev[dev].cflags, escapedkerneldir);

  dt_print_nts(DT_DEBUG_OPENCL, "   CL COMPILER OPTION:       %s\n", my_option);
  dt_print_nts(DT_DEBUG_OPENCL, "   CL COMPILER COMMAND:      %s\n", cl->dev[dev].options);

  g_free(compile_option_name_cname);
  g_free(my_option);
  g_free(escapedkerneldir);
  escapedkerneldir = NULL;

  const char *clincludes[DT_OPENCL_MAX_INCLUDES] = { "rgb_norms.h",
                                                     "noise_generator.h",
                                                     "color_conversion.h",
                                                     "colorspaces.cl",
                                                     "colorspace.h",
                                                     "common.h",
                                                     NULL };

  char *includemd5[DT_OPENCL_MAX_INCLUDES] = { NULL };
  _opencl_md5sum(clincludes, includemd5);

  if(newdevice) // so far the device seems to be ok. Make sure to
                // write&export the conf database to
  {
    dt_opencl_write_device_config(dev);
    dt_conf_save(darktable.conf);
  }

  // now load all darktable cl kernels.
  // TODO: compile as a job?
  double tstart = dt_get_debug_wtime();
  FILE *f = g_fopen(filename, "rb");
  if(f)
  {
    while(!feof(f))
    {
      int prog = -1;
      gchar *confline_pattern =
        g_strdup_printf("%%%zu[^\n]\n", PATH_MAX * sizeof(char) - 1);
      int rd = fscanf(f, confline_pattern, confentry);
      g_free(confline_pattern);
      if(rd != 1) continue;
      // remove comments:
      size_t end = strlen(confentry);
      for(size_t pos = 0; pos < end; pos++)
        if(confentry[pos] == '#')
        {
          confentry[pos] = '\0';
          for(int l = pos - 1; l >= 0; l--)
          {
            if(confentry[l] == ' ')
              confentry[l] = '\0';
            else
              break;
          }
          break;
        }
      if(confentry[0] == '\0') continue;

      const char *programname = NULL, *programnumber = NULL;
      gchar **tokens = g_strsplit_set(confentry, " \t", 2);
      if(tokens)
      {
        programname = tokens[0];
        if(tokens[0])
          programnumber = tokens[1]; // if the 0st wasn't NULL then we
                                     // have at least the terminating
                                     // NULL in [1]
      }

      prog = programnumber ? strtol(programnumber, NULL, 10) : -1;

      if(!programname || programname[0] == '\0' || prog < 0)
      {
        dt_print(DT_DEBUG_OPENCL,
                 "[dt_opencl_device_init] malformed entry in programs.conf `%s';"
                 " ignoring it!", confentry);
        continue;
      }

      snprintf(filename, PATH_MAX * sizeof(char),
               "%s" G_DIR_SEPARATOR_S "%s", kerneldir, programname);
      snprintf(binname, PATH_MAX * sizeof(char),
               "%s" G_DIR_SEPARATOR_S "%s.bin", cachedir, programname);
      dt_print(DT_DEBUG_OPENCL | DT_DEBUG_VERBOSE,
               "[dt_opencl_device_init] testing program `%s' ..", programname);
      int loaded_cached;
      char md5sum[33];
      if(_opencl_load_program(dev, prog, programname, filename, binname, cachedir,
                              md5sum, includemd5, &loaded_cached)
         && _opencl_build_program(dev, prog, binname, cachedir, md5sum, loaded_cached))
      {
        dt_print(DT_DEBUG_OPENCL,
                 "[dt_opencl_device_init] failed to compile program `%s'!",
                 programname);
        fclose(f);
        g_strfreev(tokens);
        res = TRUE;
        goto end;
      }

      g_strfreev(tokens);
    }

    fclose(f);
    dt_print_nts(DT_DEBUG_OPENCL,
                 "   KERNEL LOADING TIME:       %2.4lf sec\n", dt_get_lap_time(&tstart));
  }
  else
  {
    dt_print_nts(DT_DEBUG_OPENCL,
                 "[dt_opencl_device_init] could not open `%s'!\n", filename);
    res = TRUE;
    goto end;
  }
  for(int n = 0; n < DT_OPENCL_MAX_INCLUDES; n++) g_free(includemd5[n]);
  res = FALSE;

end:
  // we always write the device config to keep track of disabled devices
  dt_opencl_write_device_config(dev);

  free(device_name);
  free(device_name_cleaned);
  free(fullname);
  free(cname);
  free(pname);
  free(vendor);
  free(driverversion);
  free(deviceversion);

  free(dtcache);
  free(cachedir);
  free(alnum_fullname);
  free(drvversion);
  free(platform_display_name);
  free(platform_name);
  free(platform_vendor);

  free(filename);
  free(confentry);
  free(binname);

  return res;
}

void dt_opencl_init(
        dt_opencl_t *cl,
        const gboolean exclude_opencl,
        const gboolean print_statistics)
{
  dt_pthread_mutex_init(&cl->lock, NULL);
  cl->inited = FALSE;
  cl->enabled = FALSE;
  cl->stopped = FALSE;
  cl->error_count = 0;
  cl->print_statistics = print_statistics;

  // work-around to fix a bug in some AMD OpenCL compilers, which
  // would fail parsing certain numerical constants if locale is
  // different from "C".  we save the current locale, set locale to
  // "C", and restore the previous setting after OpenCL is initialized
  char *locale = strdup(setlocale(LC_ALL, NULL));
  setlocale(LC_ALL, "C");

  // we might want to show an opencl error
  char *logerror = NULL;
  const gboolean opencl_requested = dt_conf_get_bool("opencl");

  cl->crc = 5781;
  cl->dlocl = NULL;
  cl->dev_priority_image = NULL;
  cl->dev_priority_preview = NULL;
  cl->dev_priority_preview2 = NULL;
  cl->dev_priority_export = NULL;
  cl->dev_priority_thumbnail = NULL;

  cl_platform_id *all_platforms = NULL;
  cl_uint *all_num_devices = NULL;

  char *platform_name = calloc(DT_OPENCL_CBUFFSIZE, sizeof(char));
  char *platform_vendor = calloc(DT_OPENCL_CBUFFSIZE, sizeof(char));
  char *platform_key = calloc(DT_OPENCL_CBUFFSIZE, sizeof(char));

  if(exclude_opencl)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_init] opencl disabled due to explicit user request");
    goto finally;
  }

  if(!opencl_requested)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_init] opencl disabled via darktable preferences");

  // look for explicit definition of opencl_runtime library in preferences
  const char *library = dt_conf_get_string_const("opencl_library");

  // dynamically load opencl runtime and bind required symbols and test it
  cl->dlocl = dt_dlopencl_init(library);
  if(cl->dlocl == NULL)
  {
    logerror = _("no working OpenCL library found");
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_init] no working opencl '%s' library found."
             " Continue with opencl disabled",
             (strlen(library) == 0) ? "default path" : library);
    goto finally;
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_init] opencl library '%s' found on your system and loaded,"
             " preference '%s'",
             cl->dlocl->library,
             (strlen(library) == 0) ? "default path" : library);
  }

  all_platforms = malloc(sizeof(cl_platform_id) * DT_OPENCL_MAX_PLATFORMS);
  all_num_devices = malloc(sizeof(cl_uint) * DT_OPENCL_MAX_PLATFORMS);

  cl_uint num_platforms = 0;
  logerror= _("platform detection failed. some possible causes:\n"
              "  - OpenCL ICD (ocl-icd) missing,\n"
              "  - previous OpenCL errors leading to blocked devices,\n"
              "  - power management problems,\n"
              "  - buggy drivers,\n"
              "  - no OpenCL driver installed,\n"
              "  - multiple drivers installed per platform\n");

  cl_int err = (cl->dlocl->symbols->dt_clGetPlatformIDs)(0, NULL, &num_platforms);
  if((err != CL_SUCCESS) || (num_platforms == 0))
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_init] %i platforms detected, error: %s",
             num_platforms, cl_errstr(err));
    goto finally;
  }

  num_platforms = 0;
  err = (cl->dlocl->symbols->dt_clGetPlatformIDs)
    (DT_OPENCL_MAX_PLATFORMS, all_platforms, &num_platforms);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_init] could not get platforms IDs: %s", cl_errstr(err));
    goto finally;
  }
  if(num_platforms == 0)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] no opencl platform available");
    goto finally;
  }

  logerror = NULL;
  dt_print(DT_DEBUG_OPENCL,
           "[opencl_init] found %d platform%s",
           num_platforms, num_platforms > 1 ? "s" : "");

  // safety check for platforms; we must not have several versions for the same platform
  {
    char *platforms = calloc(num_platforms, sizeof(char) * DT_OPENCL_CBUFFSIZE);
    for(int n = 0; n < num_platforms; n++)
    {
      cl_platform_id platform = all_platforms[n];
      if((cl->dlocl->symbols->dt_clGetPlatformInfo)
        (platform, CL_PLATFORM_NAME, DT_OPENCL_CBUFFSIZE, platforms + n * DT_OPENCL_CBUFFSIZE, NULL) != CL_SUCCESS)
        break;

      for(int k = 0; k < n; k++)
      {
        if(!strcmp(platforms + n * DT_OPENCL_CBUFFSIZE, platforms + k * DT_OPENCL_CBUFFSIZE))
        dt_print(DT_DEBUG_OPENCL,
           "[opencl_init] possibly a multiple platform problem for `%s'",
           platforms + n * DT_OPENCL_CBUFFSIZE);
      }
    }
    free(platforms);
  }

  for(int n = 0; n < num_platforms; n++)
  {
    cl_platform_id platform = all_platforms[n];
    // get the number of GPU devices available to the platforms the
    // other common option is CL_DEVICE_TYPE_GPU/CPU (but the latter
    // doesn't work with the nvidia drivers)

    const cl_int errn = (cl->dlocl->symbols->dt_clGetPlatformInfo)
        (platform, CL_PLATFORM_NAME, DT_OPENCL_CBUFFSIZE, platform_name, NULL);
    const cl_int errv = (cl->dlocl->symbols->dt_clGetPlatformInfo)
        (platform, CL_PLATFORM_VENDOR, DT_OPENCL_CBUFFSIZE, platform_vendor, NULL);

    gboolean valid_platform = FALSE;
    if(errn == CL_SUCCESS)
    {
      snprintf(platform_key, DT_OPENCL_CBUFFSIZE, "%s", "clplatform_");
      const int len = MIN(strlen(platform_name), DT_OPENCL_CBUFFSIZE);
      int j = strlen(platform_key);
      // remove non-alphanumeric chars from platform name
      for(int i = 0; i < len; i++)
        if(isalnum(platform_name[i])) platform_key[j++] = tolower(platform_name[i]);
      platform_key[j] = 0;

      if(dt_conf_key_exists(platform_key))
        valid_platform = dt_conf_get_bool(platform_key);
      /* In some cases it is safe to assume platform aliases instead of adding
          an additional conf key or falling back to clplatform_other
      */
      else if((strcmp(platform_key, "clplatform_intelropenclgraphics") == 0)
           || (strcmp(platform_key, "clplatform_intelropencluhdgraphics") == 0)
           || (strcmp(platform_key, "clplatform_intelropenclirisgraphics") == 0)
           || (strcmp(platform_key, "clplatform_intelropenclirisprographics") == 0))
        valid_platform = dt_conf_get_bool("clplatform_intelropenclhdgraphics");
      else
        valid_platform = dt_conf_get_bool("clplatform_other");
    }

    err = (cl->dlocl->symbols->dt_clGetDeviceIDs)
      (platform, CL_DEVICE_TYPE_ALL, 0, NULL, &(all_num_devices[n]));

    if(err != CL_SUCCESS || !valid_platform)
    {
      if(!valid_platform)
      {
        dt_print(DT_DEBUG_OPENCL,
                 "[check platform] platform '%s' with key '%s' is NOT active",
                 platform_name, platform_key);
      }
      else if((errn == CL_SUCCESS) && (errv == CL_SUCCESS))
        dt_print(DT_DEBUG_OPENCL,
                 "[opencl_init] no devices found for %s (vendor) - %s (name)",
                 platform_vendor, platform_name);
      else
      {
        dt_print(DT_DEBUG_OPENCL,
                 "[opencl_init] no devices found for unknown platform");
        logerror = _("no devices found for unknown platform");
      }
      all_num_devices[n] = 0;
    }
    else
    {
      char profile[64] = { 0 };
      size_t profile_size;
      err = (cl->dlocl->symbols->dt_clGetPlatformInfo)
        (platform, CL_PLATFORM_PROFILE, 64, profile, &profile_size);
      if(err != CL_SUCCESS)
      {
        all_num_devices[n] = 0;
        dt_print(DT_DEBUG_OPENCL,
                 "[opencl_init] could not get profile for platform '%s': %s",
                 platform_name, cl_errstr(err));
      }
      else
      {
        if(strcmp("FULL_PROFILE", profile) != 0)
        {
          all_num_devices[n] = 0;
          dt_print(DT_DEBUG_OPENCL,
                   "[opencl_init] platform '%s' is not FULL_PROFILE",
                   platform_name);
        }
      }
    }
  }

  cl_uint num_devices = 0;
  for(int n = 0; n < num_platforms; n++) num_devices += all_num_devices[n];

  // create the device list
  cl_device_id *devices = 0;
  if(num_devices)
  {
    cl->dev = (dt_opencl_device_t *)malloc(sizeof(dt_opencl_device_t) * num_devices);
    devices = (cl_device_id *)malloc(sizeof(cl_device_id) * num_devices);
    if(!cl->dev || !devices)
    {
      free(cl->dev);
      cl->dev = NULL;
      free(devices);
      dt_print(DT_DEBUG_OPENCL,
               "[opencl_init] could not allocate memory for device resources");
      logerror = _("not enough memory for OpenCL devices");
      goto finally;
    }
  }

  cl_device_id *devs = devices;
  for(int n = 0; n < num_platforms; n++)
  {
    if(all_num_devices[n])
    {
      cl_platform_id platform = all_platforms[n];
      err = (cl->dlocl->symbols->dt_clGetDeviceIDs)(platform,
                                                    CL_DEVICE_TYPE_ALL,
                                                    all_num_devices[n],
                                                    devs,
                                                    NULL);
      if(err != CL_SUCCESS)
      {
        num_devices -= all_num_devices[n];
        dt_print(DT_DEBUG_OPENCL,
                 "[opencl_init] could not get devices list: %s",
                 cl_errstr(err));
      }
      devs += all_num_devices[n];
    }
  }
  devs = NULL;

  dt_print_nts(DT_DEBUG_OPENCL,
               "[opencl_init] found %d device%s\n",
               num_devices, num_devices > 1 ? "s" : "");
  if(num_devices == 0)
  {
    if(devices)
      free(devices);
    logerror = _("no OpenCL devices found");
    goto finally;
  }

  int dev = 0;
  for(int k = 0; k < num_devices; k++)
  {
    if(_opencl_device_init(cl, dev, devices, k))
      continue;
    // increase dev only if _opencl_device_init was successful
    ++dev;
  }
  free(devices);
  devices = NULL;

  if(dev > 0)
  {
    cl->num_devs = dev;
    cl->inited = TRUE;
    cl->enabled = opencl_requested;
    memset(cl->mandatory, 0, sizeof(cl->mandatory));
    cl->dev_priority_image = (int *)malloc(sizeof(int) * (dev + 1));
    cl->dev_priority_preview = (int *)malloc(sizeof(int) * (dev + 1));
    cl->dev_priority_preview2 = (int *)malloc(sizeof(int) * (dev + 1));
    cl->dev_priority_export = (int *)malloc(sizeof(int) * (dev + 1));
    cl->dev_priority_thumbnail = (int *)malloc(sizeof(int) * (dev + 1));

    // only check successful malloc in debug mode; darktable will
    // crash anyhow sooner or later if mallocs that small would fail
    assert(cl->dev_priority_image != NULL
           && cl->dev_priority_preview != NULL
           && cl->dev_priority_preview2 != NULL
           && cl->dev_priority_export != NULL
           && cl->dev_priority_thumbnail != NULL);

    dt_print_nts(DT_DEBUG_OPENCL,
                 "[opencl_init] OpenCL successfully initialized."
                 " internal numbers and names of available devices:\n");
    for(int i = 0; i < dev; i++)
      dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init]\t\t%d\t'%s'\n", i, cl->dev[i].fullname);
  }
  else
  {
    logerror = _("no suitable OpenCL devices found");
    dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] no suitable devices found.\n");
  }

finally:
  dt_print(DT_DEBUG_OPENCL,
           "[opencl_init] FINALLY: opencl PREFERENCE=%s is %sAVAILABLE and %sENABLED.",
           opencl_requested ? "ON" : "OFF",
           cl->inited ? "" : "NOT ",
           cl->enabled ? "" : "NOT ");
  if(cl->inited && cl->enabled)
  {
    // report some global settings if up & running
    dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] opencl_scheduling_profile: '%s'\n",
                dt_conf_get_string_const("opencl_scheduling_profile"));
    dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] opencl_device_priority: '%s'\n",
                dt_conf_get_string_const("opencl_device_priority"));
    dt_print_nts(DT_DEBUG_OPENCL,
               "[opencl_init] opencl_mandatory_timeout: %d\n",
               dt_conf_get_int("opencl_mandatory_timeout"));
  }

  if(logerror && opencl_requested)
  {
    dt_control_log(_("OpenCL initializing problem:\n%s\ndisabling OpenCL for now"), logerror);
    dt_conf_set_bool("opencl", FALSE);
  }

  if(cl->inited)
  {
    dt_capabilities_add("opencl");
    if(cl->num_devs > 1)
      dt_capabilities_add("multiopencl");
    cl->blendop = dt_develop_blend_init_cl_global();
    cl->bilateral = dt_bilateral_init_cl_global();
    cl->gaussian = dt_gaussian_init_cl_global();
    cl->interpolation = dt_interpolation_init_cl_global();
    cl->local_laplacian = dt_local_laplacian_init_cl_global();
    cl->dwt = dt_dwt_init_cl_global();
    cl->heal = dt_heal_init_cl_global();
    cl->colorspaces = dt_colorspaces_init_cl_global();
    cl->guided_filter = dt_guided_filter_init_cl_global();

    char checksum[64];
    snprintf(checksum, sizeof(checksum), "%u", cl->crc);
    const char *oldchecksum = dt_conf_get_string_const("opencl_checksum");

    const gboolean manually = strcasecmp(oldchecksum, "OFF") == 0;
    const gboolean newcheck = ((strcmp(oldchecksum, checksum) != 0)
                               || (strlen(oldchecksum) < 1));

    // check if the list of existing OpenCL devices (indicated by
    // checksum != oldchecksum) has changed
    if(newcheck && !manually)
    {
      dt_conf_set_string("opencl_checksum", checksum);
      // set scheduling profile to "default"
      dt_conf_set_string("opencl_scheduling_profile", "default");
      dt_print(DT_DEBUG_OPENCL,
               "[opencl_init] set scheduling profile to default, setup has changed.");
      dt_control_log(_("OpenCL scheduling profile set to default, setup has changed"));
    }
    // apply config settings for scheduling profile: sets device
    // priorities and pixelpipe synchronization timeout
    dt_opencl_scheduling_profile_t profile = _opencl_get_scheduling_profile();
    _opencl_apply_scheduling_profile(profile);

    // let's keep track on unified memory devices
    dt_sys_resources_t *res = &darktable.dtresources;
    for(int i = 0; i < cl->num_devs; i++)
    {
      if(cl->dev[i].unified_memory)
      {
        const size_t reserved = MIN(cl->dev[i].max_global_mem, res->total_memory * cl->dev[i].unified_fraction);
        cl->dev[i].max_global_mem = reserved;
        cl->dev[i].max_mem_alloc = MIN(cl->dev[i].max_mem_alloc, reserved);
        dt_print_nts(DT_DEBUG_OPENCL,
               "   UNIFIED MEM SIZE:         %.0f MB reserved for '%s' id=%d",
               (double)reserved / 1024.0 / 1024.0, cl->dev[i].cname, i);
        res->total_memory -= reserved;
      }
    }
  }
  else // initialization failed
  {
    for(int i = 0; cl->dev && i < cl->num_devs; i++)
    {
      dt_pthread_mutex_destroy(&cl->dev[i].lock);
      for(int k = 0; k < DT_OPENCL_MAX_KERNELS; k++)
        if(cl->dev[i].kernel_used[k])
          (cl->dlocl->symbols->dt_clReleaseKernel)(cl->dev[i].kernel[k]);
      for(int k = 0; k < DT_OPENCL_MAX_PROGRAMS; k++)
        if(cl->dev[i].program_used[k])
          (cl->dlocl->symbols->dt_clReleaseProgram)(cl->dev[i].program[k]);
      (cl->dlocl->symbols->dt_clReleaseCommandQueue)(cl->dev[i].cmd_queue);
      (cl->dlocl->symbols->dt_clReleaseContext)(cl->dev[i].context);
      if(cl->dev[i].use_events)
      {
        dt_opencl_events_reset(i);
        free(cl->dev[i].eventlist);
        free(cl->dev[i].eventtags);
      }
      free((void *)(cl->dev[i].fullname));
      free((void *)(cl->dev[i].cname));
      free((void *)(cl->dev[i].options));
      free((void *)(cl->dev[i].cflags));
    }
  }

  free(all_num_devices);
  free(all_platforms);
  free(platform_name);
  free(platform_vendor);
  free(platform_key);

  if(locale)
  {
    setlocale(LC_ALL, locale);
    free(locale);
  }

  dt_opencl_update_settings();
}

void dt_opencl_cleanup(dt_opencl_t *cl)
{
  if(cl->inited)
  {
    dt_develop_blend_free_cl_global(cl->blendop);
    dt_bilateral_free_cl_global(cl->bilateral);
    dt_gaussian_free_cl_global(cl->gaussian);
    dt_interpolation_free_cl_global(cl->interpolation);
    dt_dwt_free_cl_global(cl->dwt);
    dt_heal_free_cl_global(cl->heal);
    dt_colorspaces_free_cl_global(cl->colorspaces);
    dt_guided_filter_free_cl_global(cl->guided_filter);

    for(int i = 0; i < cl->num_devs; i++)
    {
      dt_pthread_mutex_destroy(&cl->dev[i].lock);
      for(int k = 0; k < DT_OPENCL_MAX_KERNELS; k++)
        if(cl->dev[i].kernel_used[k])
          (cl->dlocl->symbols->dt_clReleaseKernel)(cl->dev[i].kernel[k]);
      for(int k = 0; k < DT_OPENCL_MAX_PROGRAMS; k++)
        if(cl->dev[i].program_used[k])
          (cl->dlocl->symbols->dt_clReleaseProgram)(cl->dev[i].program[k]);
      (cl->dlocl->symbols->dt_clReleaseCommandQueue)(cl->dev[i].cmd_queue);
      (cl->dlocl->symbols->dt_clReleaseContext)(cl->dev[i].context);

      if(cl->print_statistics && (darktable.unmuted & DT_DEBUG_MEMORY))
      {
        dt_print_nts(DT_DEBUG_OPENCL,
                     " [opencl_summary_statistics] device '%s' id=%d:"
                     " peak memory usage %.1f MB%s\n",
                     cl->dev[i].fullname, i,
                     (float)cl->dev[i].peak_memory/(1024*1024),
                     cl->dev[i].clmem_error
                       ? ", clmem runtime problem"
                       : "");
      }

      if(cl->print_statistics && cl->dev[i].use_events)
      {
        if(cl->dev[i].totalevents)
        {
          dt_print_nts(DT_DEBUG_OPENCL,
                       " [opencl_summary_statistics] device '%s' id=%d: %d"
                       " out of %d events were "
                       "successful and %d events lost. max event=%d%s%s\n",
                       cl->dev[i].fullname, i, cl->dev[i].totalsuccess,
                       cl->dev[i].totalevents, cl->dev[i].totallost,
                       cl->dev[i].maxeventslot,
                       (cl->dev[i].maxeventslot > 1024)
                         ? "\n *** Warning, slots > 1024"
                         : "",
                       cl->dev[i].clmem_error
                         ? ", clmem runtime problem"
                         : "");
        }
        else
        {
          dt_print_nts(DT_DEBUG_OPENCL,
                       " [opencl_summary_statistics] device '%s' id=%d: NOT utilized\n",
                       cl->dev[i].fullname, i);
        }
      }

      if(cl->dev[i].use_events)
      {
        dt_opencl_events_reset(i);

        free(cl->dev[i].eventlist);
        free(cl->dev[i].eventtags);
      }

      free((void *)(cl->dev[i].fullname));
      free((void *)(cl->dev[i].cname));
      free((void *)(cl->dev[i].options));
      free((void *)(cl->dev[i].cflags));
    }
    free(cl->dev_priority_image);
    free(cl->dev_priority_preview);
    free(cl->dev_priority_preview2);
    free(cl->dev_priority_export);
    free(cl->dev_priority_thumbnail);
  }

  if(cl->dlocl)
  {
    free(cl->dlocl->symbols);
    g_free(cl->dlocl->library);
    free(cl->dlocl);
  }

  free(cl->dev);
  dt_pthread_mutex_destroy(&cl->lock);
}

static const char *_opencl_get_vendor_by_id(const unsigned int id)
{
  const char *vendor;

  switch(id)
  {
    case DT_OPENCL_VENDOR_AMD:
      vendor = "AMD";
      break;
    case DT_OPENCL_VENDOR_NVIDIA:
      vendor = "NVIDIA";
      break;
    case DT_OPENCL_VENDOR_INTEL:
      vendor = "INTEL";
      break;
    case DT_OPENCL_VENDOR_APPLE:
      vendor = "APPLE";
      break;
    default:
      vendor = "UNKNOWN";
  }

  return vendor;
}

gboolean dt_opencl_finish(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return FALSE;

  const cl_int err = (cl->dlocl->symbols->dt_clFinish)(cl->dev[devid].cmd_queue);

  // take the opportunity to release some event handles, but without printing
  // summary statistics
  cl_int success = dt_opencl_events_flush(devid, FALSE);

  return (err == CL_SUCCESS && success == CL_SUCCESS);
}

gboolean dt_opencl_finish_sync_pipe(const int devid,
                                    const int pipetype)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return FALSE;

  const gboolean exporting = pipetype & DT_DEV_PIXELPIPE_EXPORT;
  const gboolean asyncmode = cl->dev[devid].asyncmode;

  if(!asyncmode || exporting)
    return dt_opencl_finish(devid);
  else
    return TRUE;
}

static int _take_from_list(int *list,
                           const int value)
{
  int result = -1;

  while(*list != -1 && *list != value) list++;
  result = *list;

  while(*list != -1)
  {
    *list = *(list + 1);
    list++;
  }

  return result;
}


static int _device_by_cname(const char *name)
{
  dt_opencl_t *cl = darktable.opencl;
  char tmp[2048] = { 0 };
  int result = -1;

  _ascii_str_canonical(name, tmp, sizeof(tmp));

  for(int i = 0; i < cl->num_devs; i++)
  {
    if(!strcmp(tmp, cl->dev[i].cname))
    {
      result = i;
      break;
    }
  }

  return result;
}


static char *_ascii_str_canonical(const char *in,
                                  char *out,
                                  int maxlen)
{
  if(out == NULL)
  {
    maxlen = strlen(in) + 1;
    out = malloc(maxlen);
    if(out == NULL) return NULL;
  }

  int len = 0;

  while(*in != '\0' && len < maxlen - 1)
  {
    int n = strcspn(in, "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
    in += n;
    if(n != 0) continue;
    out[len] = tolower(*in);
    len++;
    in++;
  }
  out[len] = '\0';

  return out;
}


static char *_strsep(char **stringp,
                     const char *delim)
{
  char *begin, *end;

  begin = *stringp;
  if(begin == NULL) return NULL;

  if(delim[0] == '\0' || delim[1] == '\0')
  {
    char ch = delim[0];

    if(ch == '\0')
      end = NULL;
    else
    {
      if(*begin == ch)
        end = begin;
      else if(*begin == '\0')
        end = NULL;
      else
        end = strchr(begin + 1, ch);
    }
  }
  else
    end = strpbrk(begin, delim);

  if(end)
  {
    *end++ = '\0';
    *stringp = end;
  }
  else
    *stringp = NULL;

  return begin;
}


// parse a single token of priority string and store priorities in priority_list
static void _opencl_priority_parse(dt_opencl_t *cl,
                                   char *configstr,
                                   int *priority_list,
                                   int *mandatory)
{
  const int devs = cl->num_devs;
  int count = 0;
  int *full = malloc(sizeof(int) * (devs + 1));
  int mnd = 0;

  // NULL or empty configstring?
  if(configstr == NULL || *configstr == '\0')
  {
    priority_list[0] = -1;
    *mandatory = 0;
    free(full);
    return;
  }

  // check if user wants us to force-use opencl device(s)
  if(configstr[0] == '+')
  {
    mnd = 1;
    configstr++;
  }

  // first start with a full list of devices to take from
  for(int i = 0; i < devs; i++) full[i] = i;
  full[devs] = -1;

  gchar **tokens = g_strsplit(configstr, ",", 0);
  gchar **tokens_ptr = tokens;

  while(tokens != NULL && *tokens_ptr != NULL && count < devs + 1 && full[0] != -1)
  {
    gchar *str = *tokens_ptr;
    int not = 0;
    int all = 0;

    switch(*str)
    {
      case '*':
        all = 1;
        break;
      case '!':
        not = 1;
        while(*str == '!') str++;
        break;
    }

    if(all)
    {
      // copy all remaining device numbers from full to priority list
      for(int i = 0; i < devs && full[i] != -1; i++)
      {
        priority_list[count] = full[i];
        count++;
      }
      full[0] = -1; // mark full list as empty
    }
    else if(*str != '\0')
    {
      char *endptr = NULL;

      // first check if str corresponds to an existing canonical device name
      long number = _device_by_cname(str);

      // if not try to convert string into decimal device number
      if(number < 0) number = strtol(str, &endptr, 10);

      // still not found or negative number given? set number to -1
      if(number < 0 || (number == 0 && endptr == str)) number = -1;

      // try to take number out of remaining device list
      int dev_number = _take_from_list(full, number);

      if(!not&&dev_number != -1)
      {
        priority_list[count] = dev_number;
        count++;
      }
    }

    tokens_ptr++;
  }

  g_strfreev(tokens);

  // terminate priority list with -1
  while(count < devs + 1) priority_list[count++] = -1;

  // opencl use can only be mandatory if at least one opencl device is given
  *mandatory = (priority_list[0] != -1) ? mnd : 0;

  free(full);
}

// parse a complete priority string
static void _opencl_priorities_parse(dt_opencl_t *cl,
                                     const char *configstr)
{
  char tmp[2048];
  int len = 0;

  // first get rid of all invalid characters
  while(*configstr != '\0' && len < sizeof(tmp) - 1)
  {
    const int n =
      strcspn(configstr,
              "/!,*+0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
    configstr += n;
    if(n != 0) continue;
    tmp[len] = *configstr;
    len++;
    configstr++;
  }
  tmp[len] = '\0';

  char *str = tmp;

  // now split config string into tokens, separated by '/' and parse
  // them one after the other
  char *prio = _strsep(&str, "/");
  _opencl_priority_parse(cl, prio, cl->dev_priority_image, &cl->mandatory[0]);

  prio = _strsep(&str, "/");
  _opencl_priority_parse(cl, prio, cl->dev_priority_preview, &cl->mandatory[1]);

  prio = _strsep(&str, "/");
  _opencl_priority_parse(cl, prio, cl->dev_priority_export, &cl->mandatory[2]);

  prio = _strsep(&str, "/");
  _opencl_priority_parse(cl, prio, cl->dev_priority_thumbnail, &cl->mandatory[3]);

  prio = _strsep(&str, "/");
  _opencl_priority_parse(cl, prio, cl->dev_priority_preview2, &cl->mandatory[4]);
}

// set device priorities according to config string
static void _opencl_update_priorities(const char *configstr)
{
  dt_opencl_t *cl = darktable.opencl;
  _opencl_priorities_parse(cl, configstr);

  dt_print_nts(DT_DEBUG_OPENCL,
               "[opencl_update_priorities] these are your device priorities:\n");
  dt_print_nts(DT_DEBUG_OPENCL,
               "[opencl_update_priorities]"
               " \t\timage\tpreview\texport\tthumbs\tpreview2\n");
  for(int i = 0; i < cl->num_devs; i++)
    dt_print_nts(DT_DEBUG_OPENCL,
                 "[dt_opencl_update_priorities]\t\t%d\t%d\t%d\t%d\t%d\n",
                 cl->dev_priority_image[i],
                 cl->dev_priority_preview[i], cl->dev_priority_export[i],
                 cl->dev_priority_thumbnail[i], cl->dev_priority_preview2[i]);
  dt_print_nts(DT_DEBUG_OPENCL,
               "[opencl_update_priorities] show if opencl use is"
               " mandatory for a given pixelpipe:\n");
  dt_print_nts(DT_DEBUG_OPENCL,
               "[opencl_update_priorities]"
               " \t\timage\tpreview\texport\tthumbs\tpreview2\n");
  dt_print_nts(DT_DEBUG_OPENCL,
               "[opencl_update_priorities]\t\t%d\t%d\t%d\t%d\t%d\n",
               cl->mandatory[0], cl->mandatory[1], cl->mandatory[2],
               cl->mandatory[3], cl->mandatory[4]);
}

int dt_opencl_lock_device(const int pipetype)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited) return DT_DEVICE_CPU;

  dt_pthread_mutex_lock(&cl->lock);

  const size_t prio_size = sizeof(int) * (cl->num_devs + 1);
  int *priority = malloc(prio_size);
  int mandatory;
  gboolean heavy = FALSE;

  switch(pipetype & DT_DEV_PIXELPIPE_ANY)
  {
    case DT_DEV_PIXELPIPE_FULL:
      memcpy(priority, cl->dev_priority_image, prio_size);
      mandatory = cl->mandatory[0];
      heavy = darktable.develop->late_scaling.enabled;
      break;
    case DT_DEV_PIXELPIPE_PREVIEW:
      memcpy(priority, cl->dev_priority_preview, prio_size);
      mandatory = cl->mandatory[1];
      break;
    case DT_DEV_PIXELPIPE_EXPORT:
      memcpy(priority, cl->dev_priority_export, prio_size);
      mandatory = cl->mandatory[2];
      heavy = TRUE;
      break;
    case DT_DEV_PIXELPIPE_THUMBNAIL:
      memcpy(priority, cl->dev_priority_thumbnail, prio_size);
      mandatory = cl->mandatory[3];
      break;
    case DT_DEV_PIXELPIPE_PREVIEW2:
      memcpy(priority, cl->dev_priority_preview2, prio_size);
      mandatory = cl->mandatory[4];
      heavy = darktable.develop->late_scaling.enabled;
      break;
    default:
      free(priority);
      priority = NULL;
      mandatory = 0;
  }

  dt_pthread_mutex_unlock(&cl->lock);

  if(priority)
  {
    const int usec = 5000;
    const int nloop = (heavy ? 10 : 1) * MAX(0, dt_conf_get_int("opencl_mandatory_timeout"));

    // check for free opencl device repeatedly if mandatory is TRUE,
    // else give up after first try
    for(int n = 0; n < nloop; n++)
    {
      const int *prio = priority;

      while(*prio != DT_DEVICE_CPU)
      {
        if(!dt_pthread_mutex_BAD_trylock(&cl->dev[*prio].lock))
        {
          const int devid = *prio;
          free(priority);
          return devid;
        }
        prio++;
      }

      if(!mandatory)
      {
        free(priority);
        return DT_DEVICE_CPU;
      }

      dt_iop_nap(usec);
    }
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_lock_device] reached opencl_mandatory_timeout trying"
             " to lock mandatory device, fallback to CPU\n");
  }
  else
  {
    // only a fallback if a new pipe type would be added and we forget
    // to take care of it in opencl.c
    for(int try_dev = 0; try_dev < cl->num_devs; try_dev++)
    {
      // get first currently unused processor
      if(!dt_pthread_mutex_BAD_trylock(&cl->dev[try_dev].lock)) return try_dev;
    }
  }

  free(priority);

  // use CPU processing, if no free device:
  return DT_DEVICE_CPU;
}

void dt_opencl_unlock_device(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited) return;
  if(devid > DT_DEVICE_CPU && devid < cl->num_devs)
    dt_pthread_mutex_BAD_unlock(&cl->dev[devid].lock);
}

static FILE *_fopen_stat(const char *filename, struct stat *st)
{
  FILE *f = g_fopen(filename, "rb");
  if(!f)
  {
    dt_print(DT_DEBUG_OPENCL | DT_DEBUG_VERBOSE,
             "[opencl_fopen_stat] could not open file `%s'!", filename);
    return NULL;
  }
  const int fd = fileno(f);
  if(fstat(fd, st) < 0)
  {
    dt_print(DT_DEBUG_OPENCL | DT_DEBUG_VERBOSE,
             "[opencl_fopen_stat] could not stat file `%s'!", filename);
    return NULL;
  }
  return f;
}

static void _opencl_md5sum(const char **files,
                           char **md5sums)
{
  char kerneldir[PATH_MAX] = { 0 };
  char filename[PATH_MAX] = { 0 };
  dt_loc_get_kerneldir(kerneldir, sizeof(kerneldir));

  for(int n = 0; n < DT_OPENCL_MAX_INCLUDES; n++, files++, md5sums++)
  {
    if(!*files)
    {
      *md5sums = NULL;
      continue;
    }

    snprintf(filename, sizeof(filename), "%s" G_DIR_SEPARATOR_S "%s", kerneldir, *files);

    struct stat filestat;
    FILE *f = _fopen_stat(filename, &filestat);

    if(!f)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[opencl_md5sums] could not open file `%s'!", filename);
      *md5sums = NULL;
      continue;
    }

    const size_t filesize = filestat.st_size;
    char *file = malloc(filesize);

    if(!file)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[opencl_md5sums] could not allocate buffer for file `%s'!", filename);
      *md5sums = NULL;
      fclose(f);
      continue;
    }

    const size_t rd = fread(file, sizeof(char), filesize, f);
    fclose(f);

    if(rd != filesize)
    {
      free(file);
      dt_print(DT_DEBUG_OPENCL,
               "[opencl_md5sums] could not read all of file `%s'!", filename);
      *md5sums = NULL;
      continue;
    }

    *md5sums = g_compute_checksum_for_data(G_CHECKSUM_MD5, (guchar *)file, filesize);

    free(file);
  }
}

// returns TRUE in case of an success
static gboolean _opencl_load_program(const int dev,
                                     const int prog,
                                     const char *programname,
                                     const char *filename,
                                     const char *binname,
                                     const char *cachedir,
                                     char *md5sum,
                                     char **includemd5,
                                     int *loaded_cached)
{
  cl_int err;
  dt_opencl_t *cl = darktable.opencl;

  struct stat filestat, cachedstat;
  *loaded_cached = 0;

  if(prog < 0 || prog >= DT_OPENCL_MAX_PROGRAMS)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_load_source] invalid program number `%d' of file `%s'!", prog,
             filename);
    return FALSE;
  }

  if(cl->dev[dev].program_used[prog])
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_load_source] program number `%d' already in use"
             " when loading file `%s'!", prog,
             filename);
    return FALSE;
  }

  FILE *f = _fopen_stat(filename, &filestat);
  if(!f) return FALSE;

  const size_t filesize = filestat.st_size;
  char *file = malloc(filesize + 2048);
  size_t rd = 0;
  if(file)
    rd = fread(file, sizeof(char), filesize, f);
  fclose(f);
  if(rd != filesize)
  {
    free(file);
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_load_source] could not read all"
             " of file `%s' for program number %d!",
      filename, prog);
    return FALSE;
  }

  char *start = file + filesize;
  char *end = start + 2048;
  size_t len;

  cl_device_id devid = cl->dev[dev].devid;

  // We include driver & platform version in checksum
  (cl->dlocl->symbols->dt_clGetDeviceInfo)
    (devid, CL_DRIVER_VERSION, end - start, start, &len);
  start += len;

  cl_platform_id platform;
  (cl->dlocl->symbols->dt_clGetDeviceInfo)
    (devid, CL_DEVICE_PLATFORM, sizeof(cl_platform_id), &platform, NULL);

  (cl->dlocl->symbols->dt_clGetPlatformInfo)
    (platform, CL_PLATFORM_VERSION, end - start, start, &len);
  start += len;

  // Include compiler flags for checksum
  len = g_strlcpy(start, cl->dev[dev].cflags, end - start);
  start += len;

  /* make sure that the md5sums of all the includes are applied as well */
  for(int n = 0; n < DT_OPENCL_MAX_INCLUDES; n++)
  {
    if(!includemd5[n]) continue;
    len = g_strlcpy(start, includemd5[n], end - start);
    start += len;
  }

  char *source_md5 =
      g_compute_checksum_for_data(G_CHECKSUM_MD5, (guchar *)file, start - file);
  g_strlcpy(md5sum, source_md5, 33);
  g_free(source_md5);

  file[filesize] = '\0';

  char linkedfile[PATH_MAX] = { 0 };
  ssize_t linkedfile_len = 0;

#if defined(_WIN32)
  // No symlinks on Windows
  // Have to figure out the name using the filename + md5sum
  char dup[PATH_MAX] = { 0 };
  snprintf(dup, sizeof(dup), "%s.%s", binname, md5sum);
  FILE *cached = _fopen_stat(dup, &cachedstat);
  g_strlcpy(linkedfile, md5sum, sizeof(linkedfile));
  linkedfile_len = strlen(md5sum);
#else
  FILE *cached = _fopen_stat(binname, &cachedstat);
#endif

  if(cached)
  {
#if !defined(_WIN32)
    linkedfile_len = readlink(binname, linkedfile, sizeof(linkedfile) - 1);
#endif // !defined(_WIN32)
    if(linkedfile_len > 0)
    {
      linkedfile[linkedfile_len] = '\0';

      if(strncmp(linkedfile, md5sum, 33) == 0)
      {
        // md5sum matches, load cached binary
        size_t cached_filesize = cachedstat.st_size;

        unsigned char *cached_content = malloc(cached_filesize + 1);
        if(cached_content)
          rd = fread(cached_content, sizeof(char), cached_filesize, cached);
        else
          rd = 0;
        if(rd != cached_filesize)
        {
          dt_print(DT_DEBUG_OPENCL,
                   "[opencl_load_program] could not read all of file '%s' MD5: %s!",
                   binname, md5sum);
        }
        else
        {
          cl->dev[dev].program[prog] = (cl->dlocl->symbols->dt_clCreateProgramWithBinary)(
              cl->dev[dev].context, 1, &(cl->dev[dev].devid), &cached_filesize,
                (const unsigned char **)&cached_content, NULL, &err);
          if(err != CL_SUCCESS)
          {
            dt_print(DT_DEBUG_OPENCL,
                     "[opencl_load_program] could not load cached binary"
                     " program from file '%s' MD5: '%s'! (%s)",
                     binname, md5sum, cl_errstr(err));
          }
          else
          {
            cl->dev[dev].program_used[prog] = 1;
            *loaded_cached = 1;
          }
        }
        free(cached_content);
      }
    }
    fclose(cached);
  }


  if(*loaded_cached == 0)
  {
    // if loading cached was unsuccessful for whatever reason,
    // try to remove cached binary & link
#if !defined(_WIN32)
    if(linkedfile_len > 0)
    {
      char link_dest[PATH_MAX] = { 0 };
      snprintf(link_dest, sizeof(link_dest), "%s" G_DIR_SEPARATOR_S "%s",
               cachedir, linkedfile);
      g_unlink(link_dest);
    }
    g_unlink(binname);
#else
    // delete the file which contains the MD5 name
    g_unlink(dup);
#endif //!defined(_WIN32)

    dt_print(DT_DEBUG_OPENCL | DT_DEBUG_VERBOSE,
             "[opencl_load_program] could not load cached binary program,"
             " trying to compile source\n");

    dt_control_log(_("building OpenCL program %s for %s"),
                  programname, cl->dev[dev].fullname);
    cl->dev[dev].program[prog] = (cl->dlocl->symbols->dt_clCreateProgramWithSource)(
        cl->dev[dev].context, 1, (const char **)&file, &filesize, &err);
    free(file);
    if((err != CL_SUCCESS) || (cl->dev[dev].program[prog] == NULL))
    {
      dt_print(DT_DEBUG_OPENCL,
               "[opencl_load_source] could not create program from file `%s'! (%s)",
               filename, cl_errstr(err));
      return FALSE;
    }
    else
    {
      cl->dev[dev].program_used[prog] = 1;
    }
  }
  else
  {
    free(file);
    dt_print(DT_DEBUG_OPENCL | DT_DEBUG_VERBOSE,
             "[opencl_load_program] loaded cached"
             " binary program from file '%s' MD5: '%s' ", binname, md5sum);
  }

  dt_print(DT_DEBUG_OPENCL | DT_DEBUG_VERBOSE,
           "[opencl_load_program] successfully loaded program from '%s' MD5: '%s'",
           filename, md5sum);

  return TRUE;
}

// return TRUE in case of an error condition
static gboolean _opencl_build_program(const int dev,
                                      const int prog,
                                      const char *binname,
                                      const char *cachedir,
                                      char *md5sum,
                                      int loaded_cached)
{
  if(prog < 0 || prog > DT_OPENCL_MAX_PROGRAMS) return TRUE;
  dt_opencl_t *cl = darktable.opencl;
  cl_program program = cl->dev[dev].program[prog];
  cl_int err = (cl->dlocl->symbols->dt_clBuildProgram)
    (program, 1, &(cl->dev[dev].devid), cl->dev[dev].options, NULL, NULL);

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_build_program] could not build program: %s", cl_errstr(err));
  else
    dt_print(DT_DEBUG_OPENCL | DT_DEBUG_VERBOSE,
             "[opencl_build_program] successfully built program");

  cl_build_status build_status;
  (cl->dlocl->symbols->dt_clGetProgramBuildInfo)(program, cl->dev[dev].devid,
                                                 CL_PROGRAM_BUILD_STATUS,
                                                 sizeof(cl_build_status),
                                                 &build_status, NULL);
  dt_print(DT_DEBUG_OPENCL | DT_DEBUG_VERBOSE,
           "[opencl_build_program] BUILD STATUS: %d", build_status);

  char *build_log;
  size_t ret_val_size;
  (cl->dlocl->symbols->dt_clGetProgramBuildInfo)(program, cl->dev[dev].devid,
                                                 CL_PROGRAM_BUILD_LOG, 0, NULL,
                                                 &ret_val_size);
  if(ret_val_size != SIZE_MAX)
  {
    build_log = malloc(sizeof(char) * (ret_val_size + 1));
    if(build_log)
    {
      (cl->dlocl->symbols->dt_clGetProgramBuildInfo)(program, cl->dev[dev].devid,
                                                     CL_PROGRAM_BUILD_LOG,
                                                     ret_val_size, build_log, NULL);

      build_log[ret_val_size] = '\0';

      dt_print(DT_DEBUG_OPENCL | DT_DEBUG_VERBOSE, "BUILD LOG:");
      dt_print(DT_DEBUG_OPENCL | DT_DEBUG_VERBOSE, "%s", build_log);

      free(build_log);
    }
  }

  if(err != CL_SUCCESS)
    return TRUE;

  if(!loaded_cached)
  {
    dt_print(DT_DEBUG_OPENCL | DT_DEBUG_VERBOSE,
             "[opencl_build_program] saving binary");

    cl_uint numdev = 0;
    err = (cl->dlocl->symbols->dt_clGetProgramInfo)(program, CL_PROGRAM_NUM_DEVICES,
                                                    sizeof(cl_uint),
                                                    &numdev, NULL);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[opencl_build_program] CL_PROGRAM_NUM_DEVICES failed: %s",
               cl_errstr(err));
      return TRUE;
    }

    cl_device_id *devices = malloc(sizeof(cl_device_id) * numdev);
    err = (cl->dlocl->symbols->dt_clGetProgramInfo)(program, CL_PROGRAM_DEVICES,
                                                    sizeof(cl_device_id) * numdev,
                                                    devices, NULL);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[opencl_build_program] CL_PROGRAM_DEVICES failed: %s", cl_errstr(err));
      free(devices);
      return TRUE;
    }

    size_t *binary_sizes = malloc(sizeof(size_t) * numdev);
    err = (cl->dlocl->symbols->dt_clGetProgramInfo)(program, CL_PROGRAM_BINARY_SIZES,
                                                    sizeof(size_t) * numdev,
                                                    binary_sizes, NULL);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[opencl_build_program] CL_PROGRAM_BINARY_SIZES failed: %s",
               cl_errstr(err));
      free(binary_sizes);
      free(devices);
      return TRUE;
    }

    unsigned char **binaries = malloc(sizeof(unsigned char *) * numdev);
    for(int i = 0; i < numdev; i++)
      binaries[i] = (unsigned char *)malloc(binary_sizes[i]);
    err = (cl->dlocl->symbols->dt_clGetProgramInfo)(program, CL_PROGRAM_BINARIES,
                                                    sizeof(unsigned char *) * numdev,
                                                    binaries, NULL);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[opencl_build_program] CL_PROGRAM_BINARIES failed: %s",
               cl_errstr(err));
      goto ret;
    }

    err = DT_OPENCL_DEFAULT_ERROR; // in case of a writing error we have this error
    for(int i = 0; i < numdev; i++)
    {
      if(cl->dev[dev].devid == devices[i])
      {
        char dup[PATH_MAX] = { 0 };
        g_strlcpy(dup, binname, sizeof(dup));
        char *bname = basename(dup);

        // save opencl compiled binary as md5sum-named file
        char filename[PATH_MAX] = { 0 };
#if defined(_WIN32)
        snprintf(filename, sizeof(filename), "%s" G_DIR_SEPARATOR_S "%s.%s",
                 cachedir, bname, md5sum);
#else
        snprintf(filename, sizeof(filename), "%s" G_DIR_SEPARATOR_S "%s",
                 cachedir, md5sum);
#endif //defined(_WIN32)
        FILE *f = g_fopen(filename, "wb");
        if(!f) goto ret;
        size_t bytes_written = fwrite(binaries[i], sizeof(char), binary_sizes[i], f);
        if(bytes_written != binary_sizes[i]) goto ret;
        fclose(f);

#if !defined(_WIN32)
        // create link (e.g. basic.cl.bin -> f1430102c53867c162bb60af6c163328)
        char cwd[PATH_MAX] = { 0 };
        if(!getcwd(cwd, sizeof(cwd))) goto ret;
        if(chdir(cachedir) != 0) goto ret;
        if(symlink(md5sum, bname) != 0) goto ret;
        if(chdir(cwd) != 0) goto ret;
#endif //!defined(_WIN32)
      }
    }
    err = CL_SUCCESS; // all writings done without an error

    ret:

    for(int i = 0; i < numdev; i++)
      free(binaries[i]);
    free(binaries);
    free(binary_sizes);
    free(devices);
    if(err != CL_SUCCESS)
      dt_print(DT_DEBUG_OPENCL,
               "[dt_opencl_build_program] problems while writing OpenCL kernel files");
  }

  return err != CL_SUCCESS;
}

int dt_opencl_create_kernel(const int prog,
                            const char *name)
{
  dt_opencl_t *cl = darktable.opencl;

  static int k = 0;
  cl->name_saved[k] = name;
  cl->program_saved[k] = prog;

  if(k >= DT_OPENCL_MAX_KERNELS)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_create_kernel] too many kernels! can't create kernel `%s'",
              name);
    return -1;
  }
  return k++;
}


static gboolean _check_kernel(const int dev,
                              const int kernel)
{
  dt_opencl_t *cl = darktable.opencl;

  if(!cl->inited || dev < 0) return FALSE;
  if(kernel < 0 || kernel >= DT_OPENCL_MAX_KERNELS) return FALSE;

  if(cl->dev[dev].kernel_used[kernel]) return TRUE;

  const int prog = cl->program_saved[kernel];
  if(prog < 0 || prog >= DT_OPENCL_MAX_PROGRAMS) return FALSE;
  dt_pthread_mutex_lock(&cl->lock);

  cl_int err;
  if(!cl->dev[dev].kernel_used[kernel]
     && cl->name_saved[kernel])
  {
    cl->dev[dev].kernel_used[kernel] = 1;
    cl->dev[dev].kernel[kernel] =
      (cl->dlocl->symbols->dt_clCreateKernel)
        (cl->dev[dev].program[prog], cl->name_saved[kernel], &err);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[opencl_create_kernel] could not create kernel `%s' for '%s' id=%d: (%s)",
               cl->name_saved[kernel], cl->dev[dev].fullname, dev, cl_errstr(err));
      cl->dev[dev].kernel_used[kernel] = 0;
      cl->name_saved[kernel] = NULL; // don't try again
      dt_pthread_mutex_unlock(&cl->lock);
      return FALSE;
    }
  }
  dt_pthread_mutex_unlock(&cl->lock);
  return TRUE;
}

void dt_opencl_free_kernel(const int kernel)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited) return;
  if(kernel < 0 || kernel >= DT_OPENCL_MAX_KERNELS) return;
  dt_pthread_mutex_lock(&cl->lock);
  for(int dev = 0; dev < cl->num_devs; dev++)
  {
    cl->dev[dev].kernel_used[kernel] = 0;
    (cl->dlocl->symbols->dt_clReleaseKernel)(cl->dev[dev].kernel[kernel]);
  }
  dt_pthread_mutex_unlock(&cl->lock);
}

/** return max size in sizes[3]. */
int dt_opencl_get_max_work_item_sizes(const int dev,
                                      size_t *sizes)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || dev < 0) return -1;
  return (cl->dlocl->symbols->dt_clGetDeviceInfo)(cl->dev[dev].devid,
                                                  CL_DEVICE_MAX_WORK_ITEM_SIZES,
                                                  sizeof(size_t) * 3, sizes, NULL);
}

/** return max size per dimension in sizes[3] and max total size in workgroupsize */
int dt_opencl_get_work_group_limits(const int dev,
                                    size_t *sizes,
                                    size_t *workgroupsize,
                                    unsigned long *localmemsize)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || dev < 0) return -1;
  cl_ulong lmemsize;
  cl_int err = (cl->dlocl->symbols->dt_clGetDeviceInfo)(cl->dev[dev].devid,
                                                        CL_DEVICE_LOCAL_MEM_SIZE,
                                                        sizeof(cl_ulong), &lmemsize,
                                                        NULL);
  if(err != CL_SUCCESS) return err;

  *localmemsize = lmemsize;

  err = (cl->dlocl->symbols->dt_clGetDeviceInfo)(cl->dev[dev].devid,
                                                 CL_DEVICE_MAX_WORK_GROUP_SIZE,
                                                 sizeof(size_t), workgroupsize, NULL);
  if(err != CL_SUCCESS) return err;

  return dt_opencl_get_max_work_item_sizes(dev, sizes);
}

/** return max workgroup size for a specific kernel */
int dt_opencl_get_kernel_work_group_size(const int dev,
                                         const int kernel,
                                         size_t *kernelworkgroupsize)
{
  if(!_check_kernel(dev, kernel)) return -1;

  dt_opencl_t *cl = darktable.opencl;
  return (cl->dlocl->symbols->dt_clGetKernelWorkGroupInfo)(cl->dev[dev].kernel[kernel],
                                                           cl->dev[dev].devid,
                                                           CL_KERNEL_WORK_GROUP_SIZE,
                                                           sizeof(size_t),
                                                           kernelworkgroupsize, NULL);
}

cl_int _opencl_set_kernel_arg(const int dev,
                              const int kernel,
                              const int num,
                              const size_t size,
                              const void *arg)
{
  if(!_check_kernel(dev, kernel)) return CL_INVALID_KERNEL;

  dt_opencl_t *cl = darktable.opencl;
  const cl_int err = (cl->dlocl->symbols->dt_clSetKernelArg)
    (cl->dev[dev].kernel[kernel], num, size, arg);

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_set_kernel_arg] error kernel `%s' (%i) on device %d: %s",
              darktable.opencl->name_saved[kernel], kernel, dev, cl_errstr(err));
  return err;
}

static int _opencl_set_kernel_args(const int dev,
                                   const int kernel,
                                   int num,
                                   va_list ap)
{
  static struct { const size_t marker; const size_t size; const void *ptr; }
    test = { CLWRAP(0, 0) };

  cl_int err = CL_SUCCESS;
  do
  {
    size_t marker = va_arg(ap, size_t);
    if(marker != test.marker)
    {
      dt_print(DT_DEBUG_OPENCL,
               "opencl parameters passed to opencl_set_kernel_args or "
               "dt_opencl_enqueue_kernel_2d_with_args must be wrapped with"
               " CLARG or CLLOCAL\n");
      err = CL_INVALID_KERNEL_ARGS;
      break;
    }

    size_t size = va_arg(ap, size_t);
    if(size == SIZE_MAX) break;

    void *ptr = va_arg(ap, void *);

    err = _opencl_set_kernel_arg(dev, kernel, num++, size, ptr);
  } while(err == CL_SUCCESS);

  return err;
}

int dt_opencl_set_kernel_args_internal(const int dev,
                                       const int kernel,
                                       const int num, ...)
{
  va_list ap;
  va_start(ap, num);
  const cl_int err = _opencl_set_kernel_args(dev, kernel, num, ap);
  va_end(ap);
  return err;
}

int dt_opencl_enqueue_kernel_2d(const int dev,
                                const int kernel,
                                const size_t *sizes)
{
  return dt_opencl_enqueue_kernel_2d_with_local(dev, kernel, sizes, NULL);
}

/** launch kernel with specified dimension and defined local size! */
int dt_opencl_enqueue_kernel_ndim_with_local(const int dev,
                                             const int kernel,
                                             const size_t *sizes,
                                             const size_t *local,
                                             const int dimensions)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!_cldev_running(dev)) return DT_OPENCL_NODEVICE;
  if(kernel < 0 || kernel >= DT_OPENCL_MAX_KERNELS) return CL_INVALID_KERNEL;

  char buf[256] = { 0 };
  if(darktable.unmuted & DT_DEBUG_OPENCL)
    (cl->dlocl->symbols->dt_clGetKernelInfo)(cl->dev[dev].kernel[kernel],
                                             CL_KERNEL_FUNCTION_NAME, sizeof(buf), buf, NULL);
  cl_event *eventp = _opencl_events_get_slot(dev, buf);
  cl_int err = (cl->dlocl->symbols->dt_clEnqueueNDRangeKernel)(cl->dev[dev].cmd_queue,
                                                               cl->dev[dev].kernel[kernel],
                                                               dimensions, NULL, sizes,
                                                               local, 0, NULL, eventp);

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[dt_opencl_enqueue_kernel_%id%s] kernel `%s' (%i) on device '%s' id=%d: %s",
              dimensions, local ? "_with_local" : "",
              cl->name_saved[kernel], kernel, cl->dev[dev].fullname, dev, cl_errstr(err));
  _check_clmem_err(dev, err);
  return err;
}

int dt_opencl_enqueue_kernel_2d_with_local(const int dev,
                                           const int kernel,
                                           const size_t *sizes,
                                           const size_t *local)
{
  return dt_opencl_enqueue_kernel_ndim_with_local(dev, kernel, sizes, local, 2);
}

int dt_opencl_enqueue_kernel_2d_args_internal(const int dev,
                                              const int kernel,
                                              const size_t w,
                                              const size_t h, ...)
{
  va_list ap;
  va_start(ap, h);
  const cl_int err = _opencl_set_kernel_args(dev, kernel, 0, ap);
  va_end(ap);
  if(err != CL_SUCCESS)
  {
    dt_opencl_t *cl = darktable.opencl;
    dt_print(DT_DEBUG_OPENCL,
             "[dt_opencl_enqueue_kernel_2d_args_internal] kernel `%s' (%i) on device '%s' id=%d: %s",
              cl->name_saved[kernel], kernel, cl->dev[dev].fullname, dev, cl_errstr(err));
    return err;
  }
  const size_t sizes[] = { ROUNDUPDWD(w, dev), ROUNDUPDHT(h, dev), 1 };

  return dt_opencl_enqueue_kernel_2d_with_local(dev, kernel, sizes, NULL);
}

int dt_opencl_enqueue_kernel_1d_args_internal(const int dev,
                                              const int kernel,
                                              const size_t x,
                                              ...)
{
  va_list ap;
  va_start(ap, x);
  const cl_int err = _opencl_set_kernel_args(dev, kernel, 0, ap);
  va_end(ap);
  if(err != CL_SUCCESS)
  {
    dt_opencl_t *cl = darktable.opencl;
    dt_print(DT_DEBUG_OPENCL,
             "[dt_opencl_enqueue_kernel_1d_args_internal] kernel `%s' (%i) on device '%s' id=%d: %s",
              cl->name_saved[kernel], kernel, cl->dev[dev].fullname, dev, cl_errstr(err));
    return err;
  }
  const size_t sizes[] = { ROUNDUPDWD(x, dev), 1, 1 };

  return dt_opencl_enqueue_kernel_ndim_with_local(dev, kernel, sizes, NULL, 1);
}

int dt_opencl_copy_device_to_host(const int devid,
                                  void *host,
                                  void *device,
                                  const int width,
                                  const int height,
                                  const int bpp)
{
  return dt_opencl_read_host_from_device(devid, host, device, width, height, bpp);
}

int dt_opencl_read_host_from_device(const int devid,
                                    void *host,
                                    void *device,
                                    const int width,
                                    const int height,
                                    const int bpp)
{
  return dt_opencl_read_host_from_device_rowpitch(devid, host, device,
                                                  width, height, bpp * width);
}

int dt_opencl_read_host_from_device_rowpitch(const int devid,
                                             void *host,
                                             void *device,
                                             const int width,
                                             const int height,
                                             const int rowpitch)
{
  if(!_cldev_running(devid))
    return DT_OPENCL_NODEVICE;
  const size_t origin[] = { 0, 0, 0 };
  const size_t region[] = { width, height, 1 };
  // blocking.
  return dt_opencl_read_host_from_device_raw(devid, host, device, origin,
                                             region, rowpitch, CL_TRUE);
}

int dt_opencl_read_host_from_device_non_blocking(const int devid,
                                                 void *host,
                                                 void *device,
                                                 const int width,
                                                 const int height,
                                                 const int bpp)
{
  return dt_opencl_read_host_from_device_rowpitch_non_blocking(devid, host, device,
                                                               width, height,
                                                               bpp * width);
}

int dt_opencl_read_host_from_device_rowpitch_non_blocking(const int devid,
                                                          void *host,
                                                          void *device,
                                                          const int width,
                                                          const int height,
                                                          const int rowpitch)
{
  if(!_cldev_running(devid))
    return DT_OPENCL_NODEVICE;
  const size_t origin[] = { 0, 0, 0 };
  const size_t region[] = { width, height, 1 };
  // non-blocking.
  return dt_opencl_read_host_from_device_raw(devid, host, device, origin,
                                             region, rowpitch, CL_FALSE);
}


int dt_opencl_read_host_from_device_raw(const int devid,
                                        void *host,
                                        void *device,
                                        const size_t *origin,
                                        const size_t *region,
                                        const int rowpitch,
                                        const int blocking)
{
  if(!_cldev_running(devid))
    return DT_OPENCL_NODEVICE;

  cl_event *eventp = _opencl_events_get_slot(devid,
                                               "[Read Image (from device to host)]");

  return (darktable.opencl->dlocl->symbols->dt_clEnqueueReadImage)
    (darktable.opencl->dev[devid].cmd_queue,
     device,
     blocking ? CL_TRUE : CL_FALSE,
     origin, region, rowpitch,
     0, host, 0, NULL, eventp);
}

int dt_opencl_write_host_to_device(const int devid,
                                   void *host,
                                   void *device,
                                   const int width,
                                   const int height,
                                   const int bpp)
{
  return dt_opencl_write_host_to_device_rowpitch(devid, host, device,
                                                 width, height, width * bpp);
}

int dt_opencl_write_host_to_device_rowpitch(const int devid,
                                            void *host,
                                            void *device,
                                            const int width,
                                            const int height,
                                            const int rowpitch)
{
  if(!_cldev_running(devid))
    return DT_OPENCL_NODEVICE;

  const size_t origin[] = { 0, 0, 0 };
  const size_t region[] = { width, height, 1 };
  // blocking.
  return dt_opencl_write_host_to_device_raw(devid, host, device, origin,
                                            region, rowpitch, CL_TRUE);
}

int dt_opencl_write_host_to_device_non_blocking(const int devid,
                                                void *host,
                                                void *device,
                                                const int width,
                                                const int height,
                                                const int bpp)
{
  return dt_opencl_write_host_to_device_rowpitch_non_blocking(devid, host, device,
                                                              width, height, width * bpp);
}

int dt_opencl_write_host_to_device_rowpitch_non_blocking(const int devid,
                                                         void *host,
                                                         void *device,
                                                         const int width,
                                                         const int height,
                                                         const int rowpitch)
{
  if(!_cldev_running(devid))
    return DT_OPENCL_NODEVICE;

  const size_t origin[] = { 0, 0, 0 };
  const size_t region[] = { width, height, 1 };
  // non-blocking.

  const cl_int err = dt_opencl_write_host_to_device_raw(devid, host, device,
                                                        origin, region, rowpitch, CL_FALSE);
  _check_clmem_err(devid, err);
  return err;
}

int dt_opencl_write_host_to_device_raw(const int devid,
                                       void *host,
                                       void *device,
                                       const size_t *origin,
                                       const size_t *region,
                                       const int rowpitch,
                                       const int blocking)
{
  if(!_cldev_running(devid))
    return DT_OPENCL_NODEVICE;

  cl_event *eventp = _opencl_events_get_slot(devid, "[Write Image (from host to device)]");
  const cl_int err = (darktable.opencl->dlocl->symbols->dt_clEnqueueWriteImage)
    (darktable.opencl->dev[devid].cmd_queue,
     device, blocking ? CL_TRUE : CL_FALSE,
     origin, region,
     rowpitch, 0, host, 0, NULL, eventp);
  _check_clmem_err(devid, err);
  return err;
}

int dt_opencl_enqueue_copy_image(const int devid,
                                 cl_mem src,
                                 cl_mem dst,
                                 size_t *orig_src,
                                 size_t *orig_dst,
                                 size_t *region)
{
  if(!_cldev_running(devid))
    return DT_OPENCL_NODEVICE;

  cl_event *eventp = _opencl_events_get_slot(devid, "[Copy Image (on device)]");
  const cl_int err = (darktable.opencl->dlocl->symbols->dt_clEnqueueCopyImage)
    (darktable.opencl->dev[devid].cmd_queue, src, dst, orig_src, orig_dst,
     region, 0, NULL, eventp);

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl copy_image] could not copy on device '%s' id=%d: %s",
             darktable.opencl->dev[devid].fullname, devid, cl_errstr(err));
  _check_clmem_err(devid, err);
  return err;
}

int dt_opencl_enqueue_copy_image_to_buffer(const int devid,
                                           cl_mem src_image,
                                           cl_mem dst_buffer,
                                           size_t *origin,
                                           size_t *region,
                                           size_t offset)
{
  if(!_cldev_running(devid))
    return DT_OPENCL_NODEVICE;

  cl_event *eventp = _opencl_events_get_slot(devid, "[Copy Image to Buffer (on device)]");
  const cl_int err = (darktable.opencl->dlocl->symbols->dt_clEnqueueCopyImageToBuffer)
    (darktable.opencl->dev[devid].cmd_queue, src_image, dst_buffer, origin,
     region, offset, 0, NULL, eventp);

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl copy_image_to_buffer] could not copy on device '%s' id=%d: %s",
             darktable.opencl->dev[devid].fullname, devid, cl_errstr(err));
  _check_clmem_err(devid, err);
  return err;
}

int dt_opencl_enqueue_copy_buffer_to_image(const int devid,
                                           cl_mem src_buffer,
                                           cl_mem dst_image,
                                           size_t offset,
                                           size_t *origin,
                                           size_t *region)
{
  if(!_cldev_running(devid))
    return DT_OPENCL_NODEVICE;

  cl_event *eventp = _opencl_events_get_slot(devid, "[Copy Buffer to Image (on device)]");
  const cl_int err = (darktable.opencl->dlocl->symbols->dt_clEnqueueCopyBufferToImage)
    (darktable.opencl->dev[devid].cmd_queue, src_buffer, dst_image, offset,
     origin, region, 0, NULL, eventp);

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl copy_buffer_to_image] could not copy on device '%s' id=%d: %s",
             darktable.opencl->dev[devid].fullname, devid, cl_errstr(err));
  _check_clmem_err(devid, err);
  return err;
}

int dt_opencl_enqueue_copy_buffer_to_buffer(const int devid,
                                            cl_mem src_buffer,
                                            cl_mem dst_buffer,
                                            size_t srcoffset,
                                            size_t dstoffset,
                                            size_t size)
{
  if(!_cldev_running(devid))
    return DT_OPENCL_NODEVICE;

  cl_event *eventp = _opencl_events_get_slot(devid, "[Copy Buffer to Buffer (on device)]");
  const cl_int err = (darktable.opencl->dlocl->symbols->dt_clEnqueueCopyBuffer)
    (darktable.opencl->dev[devid].cmd_queue,
     src_buffer, dst_buffer, srcoffset,
     dstoffset, size, 0, NULL, eventp);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl copy_buffer_to_buffer] could not copy on device '%s' id=%d: %s",
             darktable.opencl->dev[devid].fullname, devid, cl_errstr(err));
  _check_clmem_err(devid, err);
  return err;
}

int dt_opencl_read_buffer_from_device(const int devid,
                                      void *host,
                                      void *device,
                                      const size_t offset,
                                      const size_t size,
                                      const int blocking)
{
  if(!_cldev_running(devid))
    return DT_OPENCL_NODEVICE;

  cl_event *eventp = _opencl_events_get_slot
    (devid, "[Read Buffer (from device to host)]");

  const cl_int err = (darktable.opencl->dlocl->symbols->dt_clEnqueueReadBuffer)
    (darktable.opencl->dev[devid].cmd_queue, device,
     blocking ? CL_TRUE : CL_FALSE,
     offset, size, host, 0, NULL, eventp);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl read_buffer_from_device] could not read from device '%s' id=%d: %s",
             darktable.opencl->dev[devid].fullname, devid, cl_errstr(err));
  return err;
}

int dt_opencl_write_buffer_to_device(const int devid,
                                     void *host,
                                     void *device,
                                     const size_t offset,
                                     const size_t size,
                                     const int blocking)
{
  if(!_cldev_running(devid))
    return DT_OPENCL_NODEVICE;

  cl_event *eventp = _opencl_events_get_slot
    (devid, "[Write Buffer (from host to device)]");

  const cl_int err = (darktable.opencl->dlocl->symbols->dt_clEnqueueWriteBuffer)
    (darktable.opencl->dev[devid].cmd_queue, device,
     blocking ? CL_TRUE : CL_FALSE,
     offset, size, host, 0, NULL, eventp);

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl write_buffer_to_device] could not write to device '%s' id=%d: %s",
             darktable.opencl->dev[devid].fullname, devid, cl_errstr(err));
  return err;
}


void *dt_opencl_copy_host_to_device_constant(const int devid,
                                             const size_t size,
                                             void *host)
{
  if(!_cldev_running(devid))
    return NULL;

  cl_int err = CL_SUCCESS;
  cl_mem dev = (darktable.opencl->dlocl->symbols->dt_clCreateBuffer)
    (darktable.opencl->dev[devid].context,
     CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, size, host, &err);

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl copy_host_to_device_constant]"
             " could not alloc buffer on device '%s' id=%d: %s",
             darktable.opencl->dev[devid].fullname, devid, cl_errstr(err));

  dt_opencl_memory_statistics(devid, dev, OPENCL_MEMORY_ADD);

  return dev;
}

void *dt_opencl_copy_host_to_device(const int devid,
                                    void *host,
                                    const int width,
                                    const int height,
                                    const int bpp)
{
  return dt_opencl_copy_host_to_device_rowpitch(devid, host, width, height, bpp, 0);
}

void *dt_opencl_copy_host_to_device_rowpitch(const int devid,
                                             void *host,
                                             const int width,
                                             const int height,
                                             const int bpp,
                                             const int rowpitch)
{
  if(!_cldev_running(devid))
    return NULL;

  cl_int err = CL_SUCCESS;
  cl_image_format fmt;
  // guess pixel format from bytes per pixel
  if(bpp == 4 * sizeof(float))
    fmt = (cl_image_format){ CL_RGBA, CL_FLOAT };
  else if(bpp == 2 * sizeof(float))
    fmt = (cl_image_format){ CL_RG, CL_FLOAT };
  else if(bpp == sizeof(float))
    fmt = (cl_image_format){ CL_R, CL_FLOAT };
  else if(bpp == sizeof(uint16_t))
    fmt = (cl_image_format){ CL_R, CL_UNSIGNED_INT16 };
  else
    return NULL;

  const cl_image_desc desc = (cl_image_desc)
        {CL_MEM_OBJECT_IMAGE2D, width, height, 0, 0, rowpitch, 0, 0, 0, NULL};

  cl_mem dev = (darktable.opencl->dlocl->symbols->dt_clCreateImage)
    (darktable.opencl->dev[devid].context, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, &fmt, &desc, host, &err);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl copy_host_to_device]"
             " could not alloc/copy img buffer on device '%s' id=%d: %s",
             darktable.opencl->dev[devid].fullname, devid, cl_errstr(err));

  _check_clmem_err(devid, err);
  dt_opencl_memory_statistics(devid, dev, OPENCL_MEMORY_ADD);

  return dev;
}


void dt_opencl_release_mem_object(cl_mem mem)
{
  if(!darktable.opencl->inited)
    return;

  // the OpenCL specs are not absolutely clear if
  // clReleaseMemObject(NULL) is a no-op. we take care of the case in
  // a centralized way at this place
  if(mem == NULL)
    return;

  dt_opencl_memory_statistics(DT_DEVICE_CPU, mem, OPENCL_MEMORY_SUB);

  (darktable.opencl->dlocl->symbols->dt_clReleaseMemObject)(mem);
}

void *dt_opencl_map_buffer(const int devid,
                           cl_mem buffer,
                           const int blocking,
                           const int flags,
                           size_t offset,
                           size_t size)
{
  if(!_cldev_running(devid))
    return NULL;

  cl_int err = CL_SUCCESS;
  void *ptr;
  cl_event *eventp = _opencl_events_get_slot(devid, "[Map Buffer]");
  ptr = (darktable.opencl->dlocl->symbols->dt_clEnqueueMapBuffer)
    (darktable.opencl->dev[devid].cmd_queue, buffer,
     blocking ? CL_TRUE : CL_FALSE,
     flags, offset, size, 0, NULL, eventp, &err);

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl map buffer] could not map buffer on device '%s' id=%d: %s",
             darktable.opencl->dev[devid].fullname, devid, cl_errstr(err));
  _check_clmem_err(devid, err);
  return ptr;
}

int dt_opencl_unmap_mem_object(const int devid,
                               cl_mem mem_object,
                               void *mapped_ptr)
{
  if(!darktable.opencl->inited)
    return DT_OPENCL_NODEVICE;

  cl_event *eventp = _opencl_events_get_slot(devid, "[Unmap Mem Object]");
  cl_int err = (darktable.opencl->dlocl->symbols->dt_clEnqueueUnmapMemObject)
    (darktable.opencl->dev[devid].cmd_queue, mem_object, mapped_ptr, 0, NULL, eventp);

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl unmap mem object] could not unmap mem object on device '%s' id=%d: %s",
             darktable.opencl->dev[devid].fullname, devid, cl_errstr(err));
  return err;
}

void *dt_opencl_alloc_device(const int devid,
                             const int width,
                             const int height,
                             const int bpp)
{
  if(!_cldev_running(devid))
    return NULL;

  dt_opencl_t *cl = darktable.opencl;
  if(cl->dev[devid].max_image_width < width || cl->dev[devid].max_image_height < height)
    return NULL;

  cl_int err = CL_SUCCESS;
  cl_image_format fmt;
  // guess pixel format from bytes per pixel
  if(bpp == 4 * sizeof(float))
    fmt = (cl_image_format){ CL_RGBA, CL_FLOAT };
  else if(bpp == 2 * sizeof(float))
    fmt = (cl_image_format){ CL_RG, CL_FLOAT };
  else if(bpp == sizeof(float))
    fmt = (cl_image_format){ CL_R, CL_FLOAT };
  else if(bpp == sizeof(uint16_t))
    fmt = (cl_image_format){ CL_R, CL_UNSIGNED_INT16 };
  else if(bpp == sizeof(uint8_t))
    fmt = (cl_image_format){ CL_R, CL_UNSIGNED_INT8 };
  else
    return NULL;

  const cl_image_desc desc = (cl_image_desc)
        {CL_MEM_OBJECT_IMAGE2D, width, height, 0, 0, 0, 0, 0, 0, NULL};

  cl_mem dev = (cl->dlocl->symbols->dt_clCreateImage)
    (cl->dev[devid].context, CL_MEM_READ_WRITE, &fmt, &desc, NULL, &err);

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl alloc_device] could not alloc img buffer on device '%s' id=%d: %s",
             cl->dev[devid].fullname, devid, cl_errstr(err));

  _check_clmem_err(devid, err);
  dt_opencl_memory_statistics(devid, dev, OPENCL_MEMORY_ADD);

  return dev;
}


void *dt_opencl_alloc_device_use_host_pointer(const int devid,
                                              const int width,
                                              const int height,
                                              const int bpp,
                                              const int rowpitch,
                                              void *host)
{
  if(!_cldev_running(devid))
    return NULL;

  dt_opencl_t *cl = darktable.opencl;
  if(cl->dev[devid].max_image_width < width || cl->dev[devid].max_image_height < height)
    return NULL;

  cl_int err = CL_SUCCESS;
  cl_image_format fmt;
  // guess pixel format from bytes per pixel
  if(bpp == 4 * sizeof(float))
    fmt = (cl_image_format){ CL_RGBA, CL_FLOAT };
  else if(bpp == 2 * sizeof(float))
    fmt = (cl_image_format){ CL_RG, CL_FLOAT };
  else if(bpp == sizeof(float))
    fmt = (cl_image_format){ CL_R, CL_FLOAT };
  else if(bpp == sizeof(uint16_t))
    fmt = (cl_image_format){ CL_R, CL_UNSIGNED_INT16 };
  else
    return NULL;

  const cl_image_desc desc = (cl_image_desc)
        {CL_MEM_OBJECT_IMAGE2D, width, height, 0, 0, rowpitch, 0, 0, 0, NULL};

  cl_mem dev = (cl->dlocl->symbols->dt_clCreateImage)
    (cl->dev[devid].context,
     CL_MEM_READ_WRITE | ((host == NULL) ? CL_MEM_ALLOC_HOST_PTR : CL_MEM_USE_HOST_PTR),
     &fmt, &desc, host, &err);

  if(err != CL_SUCCESS || dev == NULL)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl alloc_device_use_host_pointer]"
             " could not allocate cl image on device '%s' id=%d: %s",
             cl->dev[devid].fullname, devid, cl_errstr(err));

  _check_clmem_err(devid, err);
  dt_opencl_memory_statistics(devid, dev, OPENCL_MEMORY_ADD);

  return dev;
}


void *dt_opencl_alloc_device_buffer(const int devid,
                                    const size_t size)
{
  if(!_cldev_running(devid))
    return NULL;
  dt_opencl_t *cl = darktable.opencl;
  if(cl->dev[devid].max_mem_alloc < size)
    return NULL;
  cl_int err = CL_SUCCESS;

  cl_mem buf = (cl->dlocl->symbols->dt_clCreateBuffer)
    (cl->dev[devid].context,
     CL_MEM_READ_WRITE, size, NULL, &err);
  if(err != CL_SUCCESS || buf == NULL)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl alloc_device_buffer] could not allocate cl buffer on device '%s' id=%d: %s",
             cl->dev[devid].fullname, devid, cl_errstr(err));

  _check_clmem_err(devid, err);
  dt_opencl_memory_statistics(devid, buf, OPENCL_MEMORY_ADD);

  return buf;
}

void *dt_opencl_alloc_device_buffer_with_flags(const int devid,
                                               const size_t size,
                                               const int flags)
{
  if(!_cldev_running(devid))
    return NULL;
  dt_opencl_t *cl = darktable.opencl;
  if(cl->dev[devid].max_mem_alloc < size)
    return NULL;

  cl_int err = CL_SUCCESS;

  cl_mem buf = (cl->dlocl->symbols->dt_clCreateBuffer)
    (cl->dev[devid].context,
     flags, size, NULL, &err);
  if(err != CL_SUCCESS || buf == NULL)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl alloc_device_buffer_with_flags] could not allocate cl buffer on device '%s' id=%d: %s",
             cl->dev[devid].fullname, devid, cl_errstr(err));

  _check_clmem_err(devid, err);
  dt_opencl_memory_statistics(devid, buf, OPENCL_MEMORY_ADD);

  return buf;
}

size_t dt_opencl_get_mem_object_size(const cl_mem mem)
{
  size_t size;
  if(mem == NULL) return 0;

  const cl_int err = (darktable.opencl->dlocl->symbols->dt_clGetMemObjectInfo)
    (mem, CL_MEM_SIZE, sizeof(size), &size, NULL);

  return (err == CL_SUCCESS) ? size : 0;
}

static int _opencl_get_mem_context_id(const cl_mem mem)
{
  cl_context context;
  if(mem == NULL) return -1;

  const cl_int err = (darktable.opencl->dlocl->symbols->dt_clGetMemObjectInfo)
    (mem, CL_MEM_CONTEXT, sizeof(context), &context, NULL);
  if(err != CL_SUCCESS)
    return -1;

  for(int devid = 0; devid < darktable.opencl->num_devs; devid++)
  {
    if(darktable.opencl->dev[devid].context == context)
      return devid;
  }

  return -1;
}

int dt_opencl_get_image_width(const cl_mem mem)
{
  size_t size;
  if(mem == NULL) return 0;

  const cl_int err = (darktable.opencl->dlocl->symbols->dt_clGetImageInfo)
    (mem, CL_IMAGE_WIDTH, sizeof(size), &size, NULL);
  if(size > INT_MAX) size = 0;

  return (err == CL_SUCCESS) ? (int)size : 0;
}

int dt_opencl_get_image_height(const cl_mem mem)
{
  size_t size;
  if(mem == NULL) return 0;

  const cl_int err = (darktable.opencl->dlocl->symbols->dt_clGetImageInfo)
    (mem, CL_IMAGE_HEIGHT, sizeof(size), &size, NULL);
  if(size > INT_MAX) size = 0;

  return (err == CL_SUCCESS) ? (int)size : 0;
}

int dt_opencl_get_image_element_size(const cl_mem mem)
{
  size_t size;
  if(mem == NULL) return 0;

  const cl_int err = (darktable.opencl->dlocl->symbols->dt_clGetImageInfo)
    (mem, CL_IMAGE_ELEMENT_SIZE, sizeof(size), &size, NULL);
  if(size > INT_MAX) size = 0;

  return (err == CL_SUCCESS) ? (int)size : 0;
}

void *dt_opencl_duplicate_image(const int devid, const cl_mem src)
{
  const int width = dt_opencl_get_image_width(src);
  const int height = dt_opencl_get_image_height(src);
  const int el = dt_opencl_get_image_element_size(src);
  if(width < 1 || height < 1 || el < sizeof(uint16_t))
    return NULL;

  cl_mem new = dt_opencl_alloc_device(devid, width, height, el);
  if(new == NULL) return NULL;

  size_t origin[]   = { 0, 0, 0 };
  size_t region[] = { width, height, 1 };
  const cl_int err = dt_opencl_enqueue_copy_image(devid, src, new, origin, origin, region);
  if(err != CL_SUCCESS)
  {
    dt_opencl_release_mem_object(new);
    new = NULL;
  }
  return new;
}

void dt_opencl_dump_pipe_pfm(const char* mod,
                             const int devid,
                             cl_mem img,
                             const gboolean input,
                             const char *pipe)
{
  if(!_cldev_running(devid))
    return;

  const int width = dt_opencl_get_image_width(img);
  const int height = dt_opencl_get_image_height(img);
  const int element_size = dt_opencl_get_image_element_size(img);
  float *data = dt_alloc_aligned((size_t)width * height * element_size);
  if(data)
  {
    const cl_int err =
      dt_opencl_read_host_from_device(devid, data, img, width, height, element_size);
    if(err == CL_SUCCESS)
      dt_dump_pfm_file(pipe, data, width, height, element_size, mod,
                       "[dt_opencl_dump_pipe_pfm]", input, !input, FALSE);

    dt_free_align(data);
  }
}

void dt_opencl_memory_statistics(int devid,
                                 const cl_mem mem,
                                 const dt_opencl_memory_t action)
{
  if(!((darktable.unmuted & DT_DEBUG_MEMORY) && (darktable.unmuted & DT_DEBUG_OPENCL)))
    return;

  if(devid < 0)
    devid = _opencl_get_mem_context_id(mem);

  if(devid < 0)
    return;

  dt_opencl_t *cl = darktable.opencl;

  if(action == OPENCL_MEMORY_ADD)
    cl->dev[devid].memory_in_use += dt_opencl_get_mem_object_size(mem);
  else
    cl->dev[devid].memory_in_use -= dt_opencl_get_mem_object_size(mem);

  cl->dev[devid].peak_memory = MAX(cl->dev[devid].peak_memory, cl->dev[devid].memory_in_use);

  if(darktable.unmuted & DT_DEBUG_MEMORY)
  {
    dt_print(DT_DEBUG_OPENCL,"[opencl memory] device '%s' id=%d: %.1fMB in use, %.1fMB available GPU mem of %.1fMB",
             cl->dev[devid].fullname, devid,
             (float)cl->dev[devid].memory_in_use/(1024*1024),
             (float)cl->dev[devid].used_available/(1024*1024),
             (float)cl->dev[devid].max_global_mem/(1024*1024));
      if(cl->dev[devid].memory_in_use > darktable.opencl->dev[devid].used_available)
      {
        dt_print(DT_DEBUG_OPENCL,
                 "[opencl memory] Warning, device '%s' id=%d used more GPU memory than available",
                 cl->dev[devid].fullname, devid);
      }
  }
}

/* amount of graphics memory declared as available depends on max_global_mem and
   "resourcelevel". We garantee
   - a headroom of DT_OPENCL_DEFAULT_HEADROOM MB in all cases not using tuned cl
   - 256MB to simulate a minimum system
   - 2GB to simulate a reference system
*/
void dt_opencl_check_tuning(const int devid)
{
  dt_sys_resources_t *res = &darktable.dtresources;
  dt_opencl_t *cl = darktable.opencl;
  if(!_cldev_running(devid)) return;

  const int level = res->level;
  const gboolean tunehead = cl->num_devs > 1
                          && level >= 0
                          && !dt_gimpmode()
                          && dt_conf_get_bool("opencl_tune_headroom");

  cl->dev[devid].tunehead = tunehead;

  if(level < 0)
  {
    cl->dev[devid].used_available = res->refresource[4*(-level-1) + 3] * 1024lu * 1024lu;
  }
  else
  {
    const size_t allmem = cl->dev[devid].max_global_mem;
    const size_t lowmem = 256ul * 1024ul * 1024ul;
    const size_t dhead = DT_OPENCL_DEFAULT_HEADROOM * 1024ul * 1024ul;
    if(cl->dev[devid].tunehead)
    {
      const size_t headroom = (cl->dev[devid].headroom ? 1024ul * 1024ul * cl->dev[devid].headroom : dhead)
                            + (cl->dev[devid].clmem_error ? dhead : 0);
      cl->dev[devid].used_available = allmem > headroom ? allmem - headroom : lowmem;
    }
    else
    {
      const size_t disposable = allmem > dhead ? allmem - dhead : 0;
      const int fraction = MIN(1024, res->fractions[4*res->level + 3]);
      cl->dev[devid].used_available = MAX(lowmem, disposable / 1024ul * fraction);
    }
  }
}

cl_ulong dt_opencl_get_device_available(const int devid)
{
  if(!darktable.opencl->inited || devid < 0) return 0;
  return darktable.opencl->dev[devid].used_available;
}

static cl_ulong _opencl_get_device_memalloc(const int devid)
{
  return darktable.opencl->dev[devid].max_mem_alloc;
}

cl_ulong dt_opencl_get_device_memalloc(const int devid)
{
  if(!darktable.opencl->inited || devid < 0) return 0;
  return _opencl_get_device_memalloc(devid);
}

gboolean dt_opencl_image_fits_device(const int devid,
                                     const size_t width,
                                     const size_t height,
                                     const uint32_t bpp,
                                     const float factor,
                                     const size_t overhead)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!_cldev_running(devid)) return FALSE;

  const size_t required  = width * height * bpp;
  const size_t total = factor * required + overhead;

  if(cl->dev[devid].max_image_width < width || cl->dev[devid].max_image_height < height)
    return FALSE;

  if(_opencl_get_device_memalloc(devid) < required)
    return FALSE;
  if(dt_opencl_get_device_available(devid) < total)
    return FALSE;
  // We know here that total memory fits and if so the buffersize will
  // also fit as there is a factor of >=2
  return TRUE;
}

/** round size to a multiple of the value given in the device specifig
 * config parameter clroundup_wd/ht */
int dt_opencl_dev_roundup_width(int size,
                                const int devid)
{
  if(_cldev_running(devid))
  {
    const int roundup = darktable.opencl->dev[devid].clroundup_wd;
    return (size % roundup == 0 ? size : (size / roundup + 1) * roundup);
  }
  else
    return 0;
}

int dt_opencl_dev_roundup_height(int size,
                                 const int devid)
{
  if(_cldev_running(devid))
  {
    const int roundup = darktable.opencl->dev[devid].clroundup_ht;
    return (size % roundup == 0 ? size : (size / roundup + 1) * roundup);
  }
  else
    return 0;
}

/** check if opencl is enabled */
gboolean dt_opencl_is_enabled(void)
{
  return darktable.opencl->inited && darktable.opencl->enabled;
}

/** runtime check for cl system running */
gboolean dt_opencl_running(void)
{
  return _cl_running();
}

/** update enabled flag and profile with value from preferences */
void dt_opencl_update_settings(void)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl) return;
  if(!cl->inited) return;

  cl->enabled = dt_conf_get_bool("opencl");
  cl->stopped = FALSE;
  cl->error_count = 0;

  dt_opencl_scheduling_profile_t profile = _opencl_get_scheduling_profile();
  _opencl_apply_scheduling_profile(profile);
  const char *pstr = dt_conf_get_string_const("opencl_scheduling_profile");
  dt_print(DT_DEBUG_OPENCL | DT_DEBUG_VERBOSE,
           "[opencl_update_settings] scheduling profile set to %s", pstr);
}

/** read scheduling profile for config variables */
static dt_opencl_scheduling_profile_t _opencl_get_scheduling_profile(void)
{
  const char *pstr = dt_conf_get_string_const("opencl_scheduling_profile");
  if(!pstr) return OPENCL_PROFILE_DEFAULT;

  dt_opencl_scheduling_profile_t profile = OPENCL_PROFILE_DEFAULT;

  if(!strcmp(pstr, "multiple GPUs"))
    profile = OPENCL_PROFILE_MULTIPLE_GPUS;
  else if(!strcmp(pstr, "very fast GPU"))
    profile = OPENCL_PROFILE_VERYFAST_GPU;

  return profile;
}

/** set opencl specific synchronization timeout */
static void _opencl_set_synchronization_timeout(const int value)
{
  darktable.opencl->opencl_synchronization_timeout = value;
  dt_print_nts(DT_DEBUG_OPENCL,
               "[opencl_synchronization_timeout] synchronization timeout set to %d\n",
               value);
}

/** adjust opencl subsystem according to scheduling profile */
static void _opencl_apply_scheduling_profile(dt_opencl_scheduling_profile_t profile)
{
  dt_pthread_mutex_lock(&darktable.opencl->lock);
  darktable.opencl->scheduling_profile = profile;

  switch(profile)
  {
    case OPENCL_PROFILE_MULTIPLE_GPUS:
      _opencl_update_priorities("*/*/*/*/*");
      _opencl_set_synchronization_timeout(20);
      break;
    case OPENCL_PROFILE_VERYFAST_GPU:
      _opencl_update_priorities("+*/+*/+*/+*/+*");
      _opencl_set_synchronization_timeout(0);
      break;
    case OPENCL_PROFILE_DEFAULT:
    default:
      _opencl_update_priorities(dt_conf_get_string_const("opencl_device_priority"));
      _opencl_set_synchronization_timeout
        (dt_conf_get_int("pixelpipe_synchronization_timeout"));
      break;
  }
  dt_pthread_mutex_unlock(&darktable.opencl->lock);
}


/** the following eventlist functions assume that affected structures
 * are locked upstream */

/** get next free slot in eventlist (and manage size of eventlist) */
static cl_event *_opencl_events_get_slot(const int devid,
                                         const char *tag)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return NULL;
  if(!cl->dev[devid].use_events) return NULL;

  static const cl_event zeroevent[1]; // implicitly initialized to zero
  cl_event **eventlist = &(cl->dev[devid].eventlist);
  dt_opencl_eventtag_t **eventtags = &(cl->dev[devid].eventtags);
  int *numevents = &(cl->dev[devid].numevents);
  int *maxevents = &(cl->dev[devid].maxevents);
  int *eventsconsolidated = &(cl->dev[devid].eventsconsolidated);
  int *lostevents = &(cl->dev[devid].lostevents);
  int *totalevents = &(cl->dev[devid].totalevents);
  int *totallost = &(cl->dev[devid].totallost);
  int *maxeventslot = &(cl->dev[devid].maxeventslot);
  // if first time called: allocate initial buffers
  if(*eventlist == NULL)
  {
    int newevents = DT_OPENCL_EVENTLISTSIZE;
    *eventlist = calloc(newevents, sizeof(cl_event));
    *eventtags = calloc(newevents, sizeof(dt_opencl_eventtag_t));
    if(!*eventlist || !*eventtags)
    {
      free(*eventlist);
      free(*eventtags);
      *eventlist = NULL;
      *eventtags = NULL;
      dt_print(DT_DEBUG_OPENCL,
               "[opencl_events_get_slot] NO eventlist for device %i", devid);
      return NULL;
    }
    *maxevents = newevents;
  }

  // check if currently highest event slot was actually consumed. If not use it again
  if(*numevents > 0 && !memcmp((*eventlist) + *numevents - 1, zeroevent, sizeof(cl_event)))
  {
    (*lostevents)++;
    (*totallost)++;
    if(tag != NULL)
    {
      g_strlcpy((*eventtags)[*numevents - 1].tag, tag, DT_OPENCL_EVENTNAMELENGTH);
    }
    else
    {
      (*eventtags)[*numevents - 1].tag[0] = '\0';
    }

    (*totalevents)++;
    return (*eventlist) + *numevents - 1;
  }

  // check if we would exceed the number of available event
  // handles. In that case first flush existing handles
  if((*numevents - *eventsconsolidated + 1 > cl->dev[devid].event_handles)
     || (*numevents == *maxevents))
    dt_opencl_events_flush(devid, FALSE);

  // if no more space left in eventlist: grow buffer
  if(*numevents == *maxevents)
  {
    int newevents = *maxevents + DT_OPENCL_EVENTLISTSIZE;
    cl_event *neweventlist = calloc(newevents, sizeof(cl_event));
    dt_opencl_eventtag_t *neweventtags = calloc(newevents, sizeof(dt_opencl_eventtag_t));
    if(!neweventlist || !neweventtags)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[opencl_events_get_slot] NO new eventlist with size %i for device %i",
               newevents, devid);
      free(neweventlist);
      free(neweventtags);
      return NULL;
    }
    memcpy(neweventlist, *eventlist, sizeof(cl_event) * *maxevents);
    memcpy(neweventtags, *eventtags, sizeof(dt_opencl_eventtag_t) * *maxevents);
    free(*eventlist);
    free(*eventtags);
    *eventlist = neweventlist;
    *eventtags = neweventtags;
    *maxevents = newevents;
  }

  // init next event slot and return it
  (*numevents)++;
  memcpy((*eventlist) + *numevents - 1, zeroevent, sizeof(cl_event));
  if(tag != NULL)
  {
    g_strlcpy((*eventtags)[*numevents - 1].tag, tag, DT_OPENCL_EVENTNAMELENGTH);
  }
  else
  {
    (*eventtags)[*numevents - 1].tag[0] = '\0';
  }

  (*totalevents)++;
  *maxeventslot = MAX(*maxeventslot, *numevents - 1);
  return (*eventlist) + *numevents - 1;
}


/** reset eventlist to empty state */
void dt_opencl_events_reset(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return;
  if(!cl->dev[devid].use_events) return;

  cl_event **eventlist = &(cl->dev[devid].eventlist);
  dt_opencl_eventtag_t **eventtags = &(cl->dev[devid].eventtags);
  int *numevents = &(cl->dev[devid].numevents);
  int *maxevents = &(cl->dev[devid].maxevents);
  int *eventsconsolidated = &(cl->dev[devid].eventsconsolidated);
  int *lostevents = &(cl->dev[devid].lostevents);
  cl_int *summary = &(cl->dev[devid].summary);

  if(*eventlist == NULL || *numevents == 0) return; // nothing to do

  // release all remaining events in eventlist, not to waste resources
  for(int k = *eventsconsolidated; k < *numevents; k++)
  {
    (cl->dlocl->symbols->dt_clReleaseEvent)((*eventlist)[k]);
  }

  memset(*eventtags, 0, sizeof(dt_opencl_eventtag_t) * *maxevents);
  *numevents = 0;
  *eventsconsolidated = 0;
  *lostevents = 0;
  *summary = CL_COMPLETE;
  return;
}


/** Wait for events in eventlist to terminate -> this is a blocking
    synchronization point!  Does not flush eventlist. Side effect:
    might adjust numevents. */
static void _opencl_events_wait_for(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return;
  if(!cl->dev[devid].use_events) return;

  static const cl_event zeroevent[1]; // implicitly initialized to zero
  cl_event **eventlist = &(cl->dev[devid].eventlist);
  int *numevents = &(cl->dev[devid].numevents);
  int *lostevents = &(cl->dev[devid].lostevents);
  int *totallost = &(cl->dev[devid].totallost);
  int *eventsconsolidated = &(cl->dev[devid].eventsconsolidated);

  if(*eventlist == NULL || *numevents == 0) return; // nothing to do

  // check if last event slot was actually used and correct numevents if needed
  if(!memcmp((*eventlist) + *numevents - 1, zeroevent, sizeof(cl_event)))
  {
    (*numevents)--;
    (*lostevents)++;
    (*totallost)++;
  }

  if(*numevents == *eventsconsolidated) return; // nothing to do

  assert(*numevents > *eventsconsolidated);

  // now wait for all remaining events to terminate
  // Risk: might never return in case of OpenCL blocks or endless loops
  // TODO: run clWaitForEvents in separate thread and implement watchdog timer
  cl_int err = (cl->dlocl->symbols->dt_clWaitForEvents)(*numevents - *eventsconsolidated,
                                           (*eventlist) + *eventsconsolidated);
  if((err != CL_SUCCESS) && (err != CL_INVALID_VALUE))
    dt_print(DT_DEBUG_OPENCL | DT_DEBUG_VERBOSE,
             "[dt_opencl_events_wait_for] reported %s for device %i",
       cl_errstr(err), devid);
}

/** display OpenCL profiling information. If "aggregated" is TRUE, try
 * to generate summarized info for each kernel */
static void _opencl_events_profiling(const int devid,
                                     const gboolean aggregated)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return;
  if(!cl->dev[devid].use_events) return;

  cl_event **eventlist = &(cl->dev[devid].eventlist);
  dt_opencl_eventtag_t **eventtags = &(cl->dev[devid].eventtags);
  int *numevents = &(cl->dev[devid].numevents);
  int *eventsconsolidated = &(cl->dev[devid].eventsconsolidated);
  int *lostevents = &(cl->dev[devid].lostevents);

  if(*eventlist == NULL
     || *numevents == 0
     || *eventtags == NULL
     || *eventsconsolidated == 0)
    return; // nothing to do

  char **tags = malloc(sizeof(char *) * (*eventsconsolidated + 1));
  float *timings = malloc(sizeof(float) * (*eventsconsolidated + 1));
  int items = 1;
  tags[0] = "";
  timings[0] = 0.0f;

  // get profiling info and arrange it
  for(int k = 0; k < *eventsconsolidated; k++)
  {
    // if aggregated is TRUE, try to sum up timings for multiple runs of each kernel
    if(aggregated)
    {
      // linear search: this is not efficient at all but acceptable given the limited
      // number of events (ca. 10 - 20)
      int tagfound = -1;
      for(int i = 0; i < items; i++)
      {
        if(!strncmp(tags[i], (*eventtags)[k].tag, DT_OPENCL_EVENTNAMELENGTH))
        {
          tagfound = i;
          break;
        }
      }

      if(tagfound >= 0) // tag was already detected before
      {
        // sum up timings
        timings[tagfound] += (*eventtags)[k].timelapsed * 1e-9;
      }
      else // tag is new
      {
        // make new entry
        items++;
        tags[items - 1] = (*eventtags)[k].tag;
        timings[items - 1] = (*eventtags)[k].timelapsed * 1e-9;
      }
    }

    else // no aggregated info wanted -> arrange event by event
    {
      items++;
      tags[items - 1] = (*eventtags)[k].tag;
      timings[items - 1] = (*eventtags)[k].timelapsed * 1e-9;
    }
  }

  // now display profiling info
  dt_print(DT_DEBUG_OPENCL,
           "[opencl_profiling] profiling device %d ('%s'):",
           devid, cl->dev[devid].fullname);

  float total = 0.0f;
  for(int i = 1; i < items; i++)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_profiling] spent %7.4f seconds in %s",
             (double)timings[i],
             tags[i][0] == '\0' ? "<?>" : tags[i]);
    total += timings[i];
  }
  // aggregated timing info for items without tag (if any)
  if(timings[0] != 0.0f)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_profiling] spent %7.4f seconds (unallocated)",
             (double)timings[0]);
    total += timings[0];
  }

  dt_print(DT_DEBUG_OPENCL,
           "[opencl_profiling] spent %7.4f seconds totally in"
           " command queue (with %d event%s missing)",
           (double)total, *lostevents, *lostevents == 1 ? "" : "s");
  free(timings);
  free(tags);
}

/** Wait for events in eventlist to terminate, check for return status
    and profiling info of events.  If "reset" is TRUE report summary
    info (would be CL_COMPLETE or last error code) and print profiling
    info if needed.  If "reset" is FALSE just store info (success
    value, profiling) from terminated events and release events for
    re-use by OpenCL driver. */
cl_int dt_opencl_events_flush(const int devid,
                              const gboolean reset)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return CL_SUCCESS;
  if(!cl->dev[devid].use_events) return CL_SUCCESS;

  cl_event **eventlist = &(cl->dev[devid].eventlist);
  dt_opencl_eventtag_t **eventtags = &(cl->dev[devid].eventtags);
  int *numevents = &(cl->dev[devid].numevents);
  int *eventsconsolidated = &(cl->dev[devid].eventsconsolidated);
  int *lostevents = &(cl->dev[devid].lostevents);
  int *totalsuccess = &(cl->dev[devid].totalsuccess);

  cl_int *summary = &(cl->dev[devid].summary);

  if(*eventlist == NULL || *numevents == 0)
    return CL_SUCCESS; // nothing to do, no news is good news

  // Wait for command queue to terminate (side effect: might adjust *numevents)
  _opencl_events_wait_for(devid);

  // now check return status and profiling data of all newly terminated events
  for(int k = *eventsconsolidated; k < *numevents; k++)
  {
    cl_int err;
    char *tag = (*eventtags)[k].tag;
    cl_int *retval = &((*eventtags)[k].retval);

    // get return value of event
    err = (cl->dlocl->symbols->dt_clGetEventInfo)((*eventlist)[k],
                                                  CL_EVENT_COMMAND_EXECUTION_STATUS,
                                                  sizeof(cl_int), retval, NULL);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[opencl_events_flush] could not get event info for '%s': %s",
               tag[0] == '\0' ? "<?>" : tag, cl_errstr(err));
    }
    else if(*retval != CL_COMPLETE)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_events_flush] execution of '%s' %s: %d",
               tag[0] == '\0' ? "<?>" : tag,
               *retval == CL_COMPLETE ? "was successful" : "failed",
               *retval);
      *summary = *retval;
    }
    else
      (*totalsuccess)++;

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      // get profiling info of event (only if darktable was called with '-d perf')
      cl_ulong start;
      cl_ulong end;
      cl_int errs = (cl->dlocl->symbols->dt_clGetEventProfilingInfo)(
          (*eventlist)[k], CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start, NULL);
      cl_int erre = (cl->dlocl->symbols->dt_clGetEventProfilingInfo)
        ((*eventlist)[k], CL_PROFILING_COMMAND_END,
         sizeof(cl_ulong), &end, NULL);
      if(errs == CL_SUCCESS && erre == CL_SUCCESS)
      {
        (*eventtags)[k].timelapsed = end - start;
      }
      else
      {
        (*eventtags)[k].timelapsed = 0;
        (*lostevents)++;
      }
    }
    else
      (*eventtags)[k].timelapsed = 0;

    // finally release event to be re-used by driver
    (cl->dlocl->symbols->dt_clReleaseEvent)((*eventlist)[k]);
    (*eventsconsolidated)++;
  }

  cl_int result = *summary;

  // do we want to get rid of all stored info?
  if(reset)
  {
    // output profiling info if wanted
    if(darktable.unmuted & DT_DEBUG_PERF) _opencl_events_profiling(devid, 1);

    // reset eventlist structures to empty state
    dt_opencl_events_reset(devid);
  }

  return (result == CL_COMPLETE) ? CL_SUCCESS : result;
}



static int _nextpow2(const int n)
{
  int k = 1;
  while(k < n)
    k <<= 1;
  return k;
}

// utility function to calculate optimal work group dimensions for a given kernel
// taking device specific restrictions and local memory limitations into account
int dt_opencl_local_buffer_opt(const int devid,
                               const int kernel,
                               dt_opencl_local_buffer_t *factors)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return FALSE;

  size_t maxsizes[3] = { 0 };     // the maximum dimensions for a work group
  size_t workgroupsize = 0;       // the maximum number of items in a work group
  unsigned long localmemsize = 0; // the maximum amount of local memory we can use
  size_t kernelworkgroupsize = 0; // the maximum amount of items in
                                  // work group for this kernel

  int *blocksizex = &factors->sizex;
  int *blocksizey = &factors->sizey;

  // initial values must be supplied in sizex and sizey.
  // we make sure that these are a power of 2 and lie within reasonable limits.
  *blocksizex = CLAMP(_nextpow2(*blocksizex), 1, 1 << 16);
  *blocksizey = CLAMP(_nextpow2(*blocksizey), 1, 1 << 16);

  if(dt_opencl_get_work_group_limits
      (devid, maxsizes, &workgroupsize, &localmemsize) == CL_SUCCESS
     && dt_opencl_get_kernel_work_group_size
          (devid, kernel, &kernelworkgroupsize) == CL_SUCCESS)
  {
    while(maxsizes[0] < *blocksizex
          || maxsizes[1] < *blocksizey
          || localmemsize < ((factors->xfactor * (*blocksizex) + factors->xoffset) *
                             (factors->yfactor * (*blocksizey) + factors->yoffset))
                            * factors->cellsize + factors->overhead
          || workgroupsize < (size_t)(*blocksizex) * (*blocksizey)
          || kernelworkgroupsize < (size_t)(*blocksizex) * (*blocksizey))
    {
      if(*blocksizex == 1 && *blocksizey == 1)
      {
        dt_print(DT_DEBUG_OPENCL,
             "[dt_opencl_local_buffer_opt] no valid resource limits for device %d",
                 devid);
        return FALSE;
      }

      if(*blocksizex > *blocksizey)
        *blocksizex >>= 1;
      else
        *blocksizey >>= 1;
    }
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL,
             "[dt_opencl_local_buffer_opt] can not identify"
             " resource limits for device %d", devid);
    return FALSE;
  }

  return TRUE;
}

#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
